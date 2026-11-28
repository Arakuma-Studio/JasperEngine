// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "player/PlayerTests.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "io/AtomicFile.h"
#include "io/Data.h"
#include "io/UserDataOverride.h"
#include "io/UserSettingsStore.h"
#include "manager/SceneManager.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"
#include "test/TestResults.h"
#include "test/TestRunner.h"
#include "test/TestScan.h"

#include "lua.hpp"
#include "stb_image_write.h"

#include <algorithm>
#include <filesystem>
#include <memory>

using namespace jasperengine;

namespace jasperengine::player {

namespace fs = std::filesystem;

namespace {

    bool readFile(const std::string& path, std::string& out) {
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK || !data.getMemPtr()) return false;
        out.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    // the build's tests/: under the assets (an export, a test project), else beside the content
    bool readContent(const std::string& path, std::string& out) {
        return readFile("asset://tests/" + path, out) || readFile("content://tests/" + path, out);
    }

    // R25: the launch's own data:// before the start scene runs and between cases ("" until launch)
    std::string& launchFolder() {
        static auto* folder = new std::string();
        return *folder;
    }

    // the run is over: PlayerPrefs written, the launch's sandbox deleted unless kept
    void endLaunch(bool keep) {
        UserSettingsStore::instance().save();
        UserSettingsStore::instance().unload();
        if (!keep && !launchFolder().empty()) {
            std::error_code ec;
            fs::remove_all(launchFolder(), ec);
        }
    }

    gametest::Json& snapshotIndex() {
        static auto* index = new gametest::Json(gametest::Json::object());
        return *index;
    }

    void appendPng(void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    }

    class PlayerHost : public gametest::Host {
    public:
        std::string runId;
        uint32_t startScene = 0;
        bool keepData = false;
        fs::path resultsDir;
        int caseNumber = 0;
        std::string folder;
        uint64_t framesAtLoad = 0;
        bool loaded = false;
        // a replay case (R9): no frame runs before its replay, which starts with the first update
        bool heldForReplay = false;
        // a capture: asked, read at the next offscreen frame
        bool captureWanted = false;
        bool captureDone = false;
        gametest::Json captureResult;
        std::string captureName;
        gametest::CaseSpec spec;

        bool beginSession(const gametest::CaseSpec& caseSpec, std::string& error) override {
            spec = caseSpec;
            if (!PlayerSession::begin(spec, runId, ++caseNumber, folder, error)) return false;
            heldForReplay = !spec.replay.empty();
            if (heldForReplay) Engine::setSimulationHeld(true);
            // the case's scene from its start: the build's start scene by default
            const bool started = spec.scene.empty() ? SceneManager::loadScene(startScene) : SceneManager::loadScene(spec.scene);
            if (!started) {
                if (heldForReplay) Engine::setSimulationHeld(false);
                heldForReplay = false;
                error = "no scene \"" + (spec.scene.empty() ? std::string("(the start scene)") : spec.scene) + "\" in this build";
                return false;
            }
            framesAtLoad = Engine::getGameFrames();
            loaded = true;
            return true;
        }
        SessionState sessionState(std::string&) override {
            if (SceneManager::isLoadPending() || SceneManager::isLoading()) {
                framesAtLoad = Engine::getGameFrames();
                return SessionState::Starting;
            }
            if (heldForReplay) {
                // loaded under the hold: the runner starts the replay now, before the first update
                heldForReplay = false;
                Engine::setSimulationHeld(false);
                return SessionState::Ready;
            }
            return Engine::getGameFrames() > framesAtLoad ? SessionState::Ready : SessionState::Starting;
        }
        void endSession() override {
            if (heldForReplay) Engine::setSimulationHeld(false);
            heldForReplay = false;
            PlayerSession::end(folder, keepData);
            captureWanted = false;
            loaded = false;
        }
        bool sessionEnded() override { return !loaded; }
        lua_State* luaState() override { return LuaBinding::getLuaState(); }
        bool readTestFile(const std::string& file, std::string& code, std::string& error) override {
            if (readContent(file, code)) return true;
            error = "cannot read tests/" + file + " from the content";
            return false;
        }
        uint64_t gameFrames() override { return Engine::getGameFrames(); }
        double gameTime() override { return Engine::getGameTime(); }
        bool beginCapture(const gametest::CaseSpec&, const std::string& name, const gametest::Json&, std::string&) override {
            captureName = name;
            captureWanted = true;
            captureDone = false;
            Engine::setSimulationHeld(true);
            return true;
        }
        bool pollCapture(gametest::Json& capture) override {
            if (!captureDone) return false;
            capture = captureResult;
            captureDone = false;
            return true;
        }
        std::string dataFolder() const override { return folder; }

        // R24: the composite as <results dir>/captures/<case id>/<name>.png, compared at import
        void frameRead(const FrameReader& reader) {
            if (!captureWanted || !reader) return;
            std::vector<uint8_t> rgba;
            int width = 0, height = 0;
            std::string error;
            const std::string relative = "captures/" + gametest::caseIdPath(spec.id) + "/" + captureName + ".png";
            captureResult = gametest::Json{{"name", captureName}};
            if (!reader(rgba, width, height, error)) {
                captureResult["status"] = "failed";
                captureResult["error"] = "the frame did not read back: " + error;
            } else {
                for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                std::string png;
                stbi_write_png_to_func(&appendPng, &png, width, height, 4, rgba.data(), width * 4);
                io::AtomicWriteOptions options;
                options.durability = io::Durability::Cache;
                const io::WriteResult written = io::writeFileAtomic(resultsDir / relative, png, options);
                captureResult["status"] = written.ok() ? "pending" : "failed";
                if (written.ok()) captureResult["actual"] = relative;
                else captureResult["error"] = written.message;
                captureResult["size"] = gametest::Json::array({width, height});
            }
            captureWanted = false;
            captureDone = true;
            Engine::setSimulationHeld(false);
        }
    };

    struct Run {
        std::unique_ptr<PlayerHost> host;
        std::unique_ptr<gametest::TestRunner> runner;
        fs::path resultsFile;
    };

    Run& run() {
        static auto* r = new Run();
        return *r;
    }

}

void PlayerSession::setSnapshotIndex(const gametest::Json& data) {
    snapshotIndex() = data.is_object() ? data : gametest::Json::object();
}

void PlayerSession::resetLua() {
    // released while the old state exists: a script instance's reference is of its own state
    std::vector<Scene*> scenes = ScriptRuntime::getStartedScenes();
    if (Scene* persistent = SceneManager::getPersistentScene())
        if (std::find(scenes.begin(), scenes.end(), persistent) == scenes.end()) scenes.push_back(persistent);
    for (Scene* scene : scenes) LuaBinding::cleanupLuaScripts(scene);
    LuaBinding::recreateLuaState();
}

bool PlayerSession::begin(const gametest::CaseSpec& spec, const std::string& runId, int caseNumber, std::string& folder, std::string& error) {
    // R25: a sandbox of its own for data://, PlayerPrefs included
    folder = (fs::path(System::instance().getUserDataPath()) / ".jasper-test-runs" / runId / std::to_string(caseNumber)).string();
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder, ec);
    if (ec) {
        error = "cannot make the case's data folder " + folder + ": " + ec.message();
        return false;
    }
    if (spec.data != "clean") {
        // a snapshot of Tests/Data, shipped as tests/Data/<name>/
        const gametest::Json& index = snapshotIndex();
        if (index.contains(spec.data) && index[spec.data].is_array()) {
            for (const gametest::Json& file : index[spec.data]) {
                const std::string name = file.is_string() ? file.get<std::string>() : std::string();
                std::string bytes;
                if (name.empty() || !readContent("Data/" + spec.data + "/" + name, bytes)) {
                    error = "snapshot " + spec.data + ": cannot read " + name;
                    return false;
                }
                io::AtomicWriteOptions options;
                options.durability = io::Durability::Cache;
                const io::WriteResult written = io::writeFileAtomic(fs::path(folder) / name, bytes, options);
                if (!written.ok()) {
                    error = "snapshot " + spec.data + ": " + written.message;
                    return false;
                }
            }
        } else {
            fs::path snapshot = fs::path(System::instance().getAssetPath()) / "tests" / "Data" / spec.data;
            if (!fs::is_directory(snapshot, ec)) snapshot = fs::path(System::instance().getContentPath()) / "tests" / "Data" / spec.data;
            if (!fs::is_directory(snapshot, ec)) {
                error = "no snapshot " + spec.data + " in this build's tests/Data";
                return false;
            }
            for (fs::recursive_directory_iterator it(snapshot, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::path target = fs::path(folder) / it->path().lexically_relative(snapshot);
                if (it->is_directory(ec)) fs::create_directories(target, ec);
                else fs::copy_file(it->path(), target, ec);
            }
        }
    }
    UserSettingsStore::instance().save();
    UserSettingsStore::instance().unload();
    setUserDataOverride(folder);
    // a fresh Lua state, as the editor's Play has: nothing a case left in Lua reaches this one
    resetLua();
    if (lua_State* L = LuaBinding::getLuaState()) {
        lua_getglobal(L, "math");
        lua_getfield(L, -1, "randomseed");
        lua_pushinteger(L, static_cast<lua_Integer>(spec.seed));
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
        lua_pop(L, 1);
    }
    Engine::setFixedDeltatime(1.0 / std::max(1, spec.fixedFps));
    return true;
}

void PlayerSession::end(const std::string& folder, bool keep) {
    UserSettingsStore::instance().save();
    UserSettingsStore::instance().unload();
    setUserDataOverride(launchFolder());
    if (!keep && !folder.empty()) {
        std::error_code ec;
        fs::remove_all(folder, ec);
    }
}

void PlayerTests::launch(const LaunchOptions&) {
    std::error_code ec;
    const fs::path folder = fs::path(System::instance().getUserDataPath()) / ".jasper-test-runs" / ("launch-" + gametest::newRunId());
    fs::create_directories(folder, ec);
    if (ec) {
        Log::error("[test] cannot make the launch's data folder %s: %s", folder.string().c_str(), ec.message().c_str());
        return;
    }
    launchFolder() = folder.string();
    setUserDataOverride(launchFolder());
}

bool PlayerTests::running() {
    return run().runner && run().runner->isRunning();
}

bool PlayerTests::wantsFrame() {
    return run().host && run().host->captureWanted;
}

void PlayerTests::begin(const LaunchOptions& options) {
    Run& r = run();
    gametest::Settings settings;
    std::string text, error;
    std::vector<gametest::CaseSpec> plan;
    // the build's settings and listing (tests/index.json, written with the content), else a scan of the folder
    if (readContent("index.json", text)) {
        const gametest::Json index = gametest::Json::parse(text, nullptr, false);
        if (!gametest::Settings::fromJson(index.value("settings", gametest::Json::object()), settings, error)) Log::error("tests/index.json: %s", error.c_str());
        PlayerSession::setSnapshotIndex(index.value("data", gametest::Json::object()));
        for (const gametest::Json& file : index.value("files", gametest::Json::array())) {
            std::string code;
            const std::string name = file.is_string() ? file.get<std::string>() : std::string();
            if (readContent(name, code)) {
                const gametest::ScannedFile scanned = gametest::TestScan::scanText(code, name, settings);
                plan.insert(plan.end(), scanned.cases.begin(), scanned.cases.end());
            }
        }
    } else {
        plan = gametest::TestScan::cases(gametest::TestScan::scan(fs::path(System::instance().getAssetPath()) / "tests", settings));
        if (plan.empty()) plan = gametest::TestScan::cases(gametest::TestScan::scan(fs::path(System::instance().getContentPath()) / "tests", settings));
    }
    if (plan.empty()) {
        Log::error("[test] no tests in this build: build a Development Build with Include Tests");
        endLaunch(options.keepTestData);
        RunControl::requestQuit(EXIT_NO_TESTS);
        return;
    }
    std::vector<gametest::CaseSpec> selected;
    for (const gametest::CaseSpec& spec : plan)
        if (gametest::matchesFilter(spec, options.testFilter)) selected.push_back(spec);
    if (selected.empty()) {
        Log::error("[test] no test matched \"%s\"", options.testFilter.c_str());
        endLaunch(options.keepTestData);
        RunControl::requestQuit(EXIT_NO_TESTS);
        return;
    }
    gametest::RunOptions runOptions;
    runOptions.settings = settings;
    runOptions.target.platform = gametest::hostPlatformName();
    runOptions.target.arch = gametest::hostArchName();
    runOptions.target.backend = gametest::backendName();
    runOptions.target.editor = false;
    runOptions.target.width = Engine::getCanvasWidth();
    runOptions.target.height = Engine::getCanvasHeight();
    runOptions.runId = gametest::newRunId();
    runOptions.filter = options.testFilter;
    runOptions.logProtocol = true;
    // R25: the results outside the sandbox, where adb and simctl reach a debuggable build's files
    r.resultsFile = !options.testResults.empty() ? options.testResults
                                                 : fs::path(System::instance().getUserDataPath()) / ".jasper-test-results" / (runOptions.runId + ".json");
    // the log line is read by a process with another working directory (run_tests target desktop)
    std::error_code absoluteError;
    const fs::path absoluteResults = fs::absolute(r.resultsFile, absoluteError);
    if (!absoluteError) r.resultsFile = absoluteResults;
    r.host = std::make_unique<PlayerHost>();
    r.host->runId = runOptions.runId;
    r.host->startScene = SceneManager::getCurrentSceneId();
    r.host->keepData = options.keepTestData;
    r.host->resultsDir = r.resultsFile.parent_path();
    r.runner = std::make_unique<gametest::TestRunner>();
    r.runner->start(selected, *r.host, runOptions);
}

void PlayerTests::frame(const FrameReader& reader) {
    Run& r = run();
    if (!r.runner) return;
    if (r.host) r.host->frameRead(reader);
    if (r.runner->frame()) return;
    const gametest::RunResult& result = r.runner->result();
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult written = io::writeFileAtomic(r.resultsFile, gametest::resultsJson(result).dump(1), options);
    if (written.ok()) Log::print("[test] results %s", r.resultsFile.string().c_str());
    else Log::error("[test] the results were not written: %s", written.message.c_str());
    const gametest::Counts& c = result.counts;
    const bool none = c.total == 0;
    const bool bad = c.failed + c.error + c.hung > 0;
    endLaunch(r.host && r.host->keepData);
    r.runner.reset();
    RunControl::requestQuit(none ? EXIT_NO_TESTS : bad ? EXIT_TESTS_FAILED : EXIT_OK);
}

}  // namespace jasperengine::player
