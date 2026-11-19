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
#include "test/TestResults.h"
#include "test/TestRunner.h"
#include "test/TestScan.h"

#include "lua.hpp"
#include "stb_image_write.h"

#include <filesystem>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::player;

namespace fs = std::filesystem;

namespace {

    bool readContent(const std::string& path, std::string& out) {
        Data data;
        if (data.open(("content://tests/" + path).c_str()) == 0 || !data.getMemPtr()) return false;
        out.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    void appendPng(void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    }

    class PlayerHost : public gametest::Host {
    public:
        std::string runId;
        bool keepData = false;
        fs::path resultsDir;
        int caseNumber = 0;
        std::string folder;
        uint64_t framesAtLoad = 0;
        bool loaded = false;
        // a capture: asked, read at the next offscreen frame
        bool captureWanted = false;
        bool captureDone = false;
        gametest::Json captureResult;
        std::string captureName;
        gametest::CaseSpec spec;

        bool beginSession(const gametest::CaseSpec& caseSpec, std::string& error) override {
            spec = caseSpec;
            // R25: a sandbox of its own for data://, PlayerPrefs included
            folder = (fs::path(System::instance().getUserDataPath()) / ".jasper-test-runs" / runId / std::to_string(++caseNumber)).string();
            std::error_code ec;
            fs::remove_all(folder, ec);
            fs::create_directories(folder, ec);
            if (ec) {
                error = "cannot make the case's data folder " + folder + ": " + ec.message();
                return false;
            }
            if (spec.data != "clean") {
                // a snapshot of Tests/Data: shipped under tests/Data/<name>/ (desktop content folders)
                const fs::path snapshot = fs::path(System::instance().getContentPath()) / "tests" / "Data" / spec.data;
                for (fs::recursive_directory_iterator it(snapshot, ec), end; !ec && it != end; it.increment(ec)) {
                    const fs::path target = fs::path(folder) / it->path().lexically_relative(snapshot);
                    if (it->is_directory(ec)) fs::create_directories(target, ec);
                    else fs::copy_file(it->path(), target, ec);
                }
            }
            UserSettingsStore::instance().save();
            UserSettingsStore::instance().unload();
            setUserDataOverride(folder);
            // the session as Play starts it: the project's modules again, the seed, the fixed delta
            LuaBinding::clearLoadedProjectModules();
            if (lua_State* L = LuaBinding::getLuaState()) {
                lua_getglobal(L, "math");
                lua_getfield(L, -1, "randomseed");
                lua_pushinteger(L, static_cast<lua_Integer>(spec.seed));
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
                lua_pop(L, 1);
            }
            Engine::setFixedDeltatime(1.0 / std::max(1, spec.fixedFps));
            const bool started = spec.scene.empty() ? SceneManager::loadScene(SceneManager::getCurrentSceneId()) : SceneManager::loadScene(spec.scene);
            if (!started) {
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
            return Engine::getGameFrames() > framesAtLoad ? SessionState::Ready : SessionState::Starting;
        }
        void endSession() override {
            UserSettingsStore::instance().save();
            UserSettingsStore::instance().unload();
            setUserDataOverride("");
            if (!keepData) {
                std::error_code ec;
                fs::remove_all(folder, ec);
            }
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
        for (const gametest::Json& file : index.value("files", gametest::Json::array())) {
            std::string code;
            const std::string name = file.is_string() ? file.get<std::string>() : std::string();
            if (readContent(name, code)) {
                const gametest::ScannedFile scanned = gametest::TestScan::scanText(code, name, settings);
                plan.insert(plan.end(), scanned.cases.begin(), scanned.cases.end());
            }
        }
    } else {
        plan = gametest::TestScan::cases(gametest::TestScan::scan(fs::path(System::instance().getContentPath()) / "tests", settings));
    }
    if (plan.empty()) {
        Log::error("[test] no tests in this build: build a Development Build with Include Tests");
        RunControl::requestQuit(EXIT_NO_TESTS);
        return;
    }
    std::vector<gametest::CaseSpec> selected;
    for (const gametest::CaseSpec& spec : plan)
        if (gametest::matchesFilter(spec, options.testFilter)) selected.push_back(spec);
    if (selected.empty()) {
        Log::error("[test] no test matched \"%s\"", options.testFilter.c_str());
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
    r.host = std::make_unique<PlayerHost>();
    r.host->runId = runOptions.runId;
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
    r.runner.reset();
    RunControl::requestQuit(none ? EXIT_NO_TESTS : bad ? EXIT_TESTS_FAILED : EXIT_OK);
}
