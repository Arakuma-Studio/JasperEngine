// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "recorder/RecordInput.h"

#include "EditorVersion.h"
#include "Out.h"
#include "Project.h"
#include "script/LuaBinding.h"
#include "test/GameTests.h"
#include "test/PlayData.h"
#include "test/TestRun.h"
#include "util/FileUtils.h"
#include "window/GameResolutions.h"

#include "Engine.h"
#include "input/InputRecorder.h"
#include "input/InputReplay.h"
#include "io/AtomicFile.h"
#include "lua.hpp"

#include "stb_image_write.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::recordinput {

namespace {

    // a shot waits this long for what loads before it fails (a stand-in is never saved)
    constexpr double SHOT_WAIT_SECONDS = 120.0;

    bool recordSessionRunning(ScenePlayState state) {
        return state == ScenePlayState::PLAYING || state == ScenePlayState::PAUSED;
    }

    std::tm localNow() {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        return local;
    }

    std::string readInputFile(const fs::path& path, bool& ok) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        if (in) text << in.rdbuf();
        ok = static_cast<bool>(in) || !text.str().empty();
        return text.str();
    }

    // math.randomseed(seed) in the game's Lua state before any script's init (R16): the editor keeps
    // one Lua state across sessions, so the seed goes in at the session's start, not when a state opens
    void seedBeforeScripts(uint32_t seed) {
        gametests::PlayData::get().beforeScripts = [seed]() {
            lua_State* L = LuaBinding::getLuaState();
            if (!L) return;
            lua_getglobal(L, "math");
            lua_getfield(L, -1, "randomseed");
            lua_pushinteger(L, static_cast<lua_Integer>(seed));
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
            lua_pop(L, 1);
        };
    }

    void appendShotPng(void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    }

    // the shot's PNG, written the atomic way (off the main thread)
    Shot writeShot(Shot shot, std::vector<uint8_t> rgba) {
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        std::string png;
        if (!stbi_write_png_to_func(&appendShotPng, &png, shot.width, shot.height, 4, rgba.data(), shot.width * 4)) {
            shot.error = "the PNG could not be encoded";
            return shot;
        }
        const io::WriteResult written = io::writeFileAtomic(shot.path, png);
        if (!written.ok()) shot.error = "cannot write " + FileUtils::pathToGenericUtf8(shot.path) + ": " + written.message;
        return shot;
    }

}

// ================================================================== settings and files

fs::path settingsFile(const fs::path& projectPath) {
    return projectPath / ".jasperengine" / "user" / "record-input.yaml";
}

bool loadSettings(const fs::path& file, Settings& settings, std::string& error) {
    bool ok = false;
    const std::string text = readInputFile(file, ok);
    if (!ok) return true;   // the defaults
    try {
        const YAML::Node root = YAML::Load(text);
        if (!root.IsMap()) return true;
        if (root["frame_rate"]) settings.frameRate = root["frame_rate"].as<int>();
        if (root["checkpoint_frames"]) settings.checkpointFrames = root["checkpoint_frames"].as<int>();
        if (root["probes"] && root["probes"].IsSequence()) {
            settings.probes.clear();
            for (const YAML::Node& probe : root["probes"]) {
                if (probe.IsScalar() && settings.probes.size() < static_cast<size_t>(MAX_PROBES)) settings.probes.push_back(probe.as<std::string>());
            }
        }
    } catch (const YAML::Exception& e) {
        error = FileUtils::pathToGenericUtf8(file) + ": " + e.what();
        return false;
    }
    settings.frameRate = std::clamp(settings.frameRate, 1, 240);
    settings.checkpointFrames = std::clamp(settings.checkpointFrames, 0, 100000);
    return true;
}

bool saveSettings(const fs::path& file, const Settings& settings, std::string& error) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "frame_rate" << YAML::Value << settings.frameRate;
    out << YAML::Key << "checkpoint_frames" << YAML::Value << settings.checkpointFrames;
    out << YAML::Key << "probes" << YAML::Value << YAML::BeginSeq;
    for (const std::string& probe : settings.probes) out << probe;
    out << YAML::EndSeq;
    out << YAML::EndMap;
    const io::WriteResult written = io::writeFileAtomic(file, std::string(out.c_str()) + "\n");
    if (!written.ok()) {
        error = "cannot write " + FileUtils::pathToGenericUtf8(file) + ": " + written.message;
        return false;
    }
    return true;
}

fs::path recordingsFolder(const fs::path& projectPath) {
    const fs::path root = gametests::findRoot(projectPath);
    return root.empty() ? projectPath / "Recordings" / "Input" : root / "Input";
}

std::string recordingFileName(const std::string& scene, const std::tm& when) {
    std::string name = scene.empty() ? std::string("Scene") : scene;
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &when);
    return name + "-" + stamp + ".input.json";
}

std::string fileStem(const fs::path& file) {
    std::string stem = FileUtils::pathToUtf8(file.filename());
    for (const char* suffix : {".json", ".input"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) stem.resize(stem.size() - n);
    }
    return stem;
}

FileSummary summarize(std::string_view text) {
    FileSummary out;
    input::ParsedScript script;
    if (!input::parseScript(text, script, out.error)) return out;
    const input::ScriptHeader& h = script.header;
    out.ok = true;
    out.kind = h.kind == input::ScriptKind::Recording ? "recording" : "script";
    if (h.kind == input::ScriptKind::Recording) out.start = h.fromPlayStart ? "play" : "now";
    out.scene = h.scene;
    out.platform = h.platform;
    out.space = input::spaceName(h.space);
    out.fixedFps = h.fixedFps;
    out.frames = script.lastFrame();
    out.events = script.events;
    out.checkpoints = h.checkpoints.size();
    out.probes = h.probes.size();
    out.truncated = h.truncated;
    out.hasSeed = h.hasSeed;
    out.seed = h.seed;
    // by the type each event was written with ("touch tap" -> touch)
    for (const std::string& label : script.eventLabels) out.eventsByType[label.substr(0, label.find(' '))]++;
    return out;
}

bool parseResolution(const std::string& text, int& width, int& height, std::string& error) {
    GameResolution resolution;
    if (!findGameResolution(text, {}, resolution, error)) {
        error = "\"" + text + "\" is not a size: give \"WxH\" (\"1290x2796\") or a Game view resolution's name";
        return false;
    }
    width = resolution.width;
    height = resolution.height;
    return true;
}

bool writePending(PendingWrite& pending, const Writer& writer) {
    std::string why;
    bool ok = false;
    if (writer) {
        ok = writer(pending.path, pending.text, why);
    } else {
        const io::WriteResult written = io::writeFileAtomic(pending.path, pending.text);
        ok = written.ok();
        why = written.message;
    }
    if (ok) {
        pending.error.clear();
        return true;
    }
    pending.error = "The input recording was not saved to " + FileUtils::pathToGenericUtf8(pending.path) + ": " +
                    (why.empty() ? std::string("the write failed") : why);
    return false;
}

// ================================================================== RecordInput

RecordInput& RecordInput::get() {
    static RecordInput instance;
    return instance;
}

RecordInput::~RecordInput() {
    // the jobs end before what they write into goes
    if (encoding.valid()) encoding.wait();
    for (std::future<Shot>& write : shotWrites) write.wait();
}

void RecordInput::setHost(Host newHost) {
    host = std::move(newHost);
}

void RecordInput::loadSettings(const fs::path& projectPath) {
    if (projectPath.empty() || settingsFile(projectPath) == settingsPath) return;
    settingsPath = settingsFile(projectPath);
    armed = false;
    currentSettings = Settings();
    std::string error;
    if (!recordinput::loadSettings(settingsPath, currentSettings, error)) Out::warning("Record Input settings: %s (the defaults are used)", error.c_str());
}

bool RecordInput::setSettings(const Settings& settings, std::string& error) {
    Settings clamped = settings;
    clamped.frameRate = std::clamp(clamped.frameRate, 1, 240);
    clamped.checkpointFrames = std::clamp(clamped.checkpointFrames, 0, 100000);
    clamped.probes.erase(std::remove_if(clamped.probes.begin(), clamped.probes.end(), [](const std::string& p) { return p.empty(); }),
                         clamped.probes.end());
    if (clamped.probes.size() > static_cast<size_t>(MAX_PROBES)) clamped.probes.resize(MAX_PROBES);
    if (clamped == currentSettings) return true;
    currentSettings = clamped;
    if (settingsPath.empty()) return true;
    return saveSettings(settingsPath, currentSettings, error);
}

Project* RecordInput::project() const {
    return host.project ? host.project() : nullptr;
}

bool RecordInput::playing() const {
    const Project* p = project();
    return p && p->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
}

bool RecordInput::sceneRunning() const {
    const Project* p = project();
    if (!p) return false;
    const SceneProject* sp = p->getScene(p->getPlaySessionSceneId());
    return sp && recordSessionRunning(sp->playState);
}

void RecordInput::applyHold(bool hold) {
    holding = hold;
    if (hold && !Engine::isSimulationHeld()) {
        Engine::setSimulationHeld(true);
        heldByUs = true;
    } else if (!hold && heldByUs) {
        heldByUs = false;
        Engine::setSimulationHeld(false);
    }
}

// ---------------------------------------------------------------- Record Input

bool RecordInput::isRecording() const {
    return recordThisSession && input::InputRecorder::isRecording();
}

RecordInput::Button RecordInput::buttonState() const {
    if (isRecording() || (recordThisSession && phase == Phase::Starting)) return Button::Recording;
    if (playing()) return Button::Disabled;
    return armed ? Button::Armed : Button::Off;
}

void RecordInput::setArmed(bool on) {
    armed = on;
}

bool RecordInput::canToggle() const {
    const Button state = buttonState();
    return state == Button::Off || state == Button::Armed || state == Button::Recording;
}

void RecordInput::toggle() {
    switch (buttonState()) {
        case Button::Off: armed = true; break;
        case Button::Armed: armed = false; break;
        case Button::Recording: stopRecording(); break;
        case Button::Disabled: break;
    }
}

std::string RecordInput::recordingLabel() const {
    if (!recordThisSession) return std::string();
    if (phase == Phase::Starting) return "REC (waiting for Play)";
    const uint32_t frame = input::InputRecorder::frame();
    const double seconds = sessionFps > 0 ? static_cast<double>(frame) / sessionFps : 0.0;
    char text[64];
    std::snprintf(text, sizeof(text), "REC %02d:%02d \xc2\xb7 %zu events", static_cast<int>(seconds / 60.0),
                  static_cast<int>(std::fmod(seconds, 60.0)), input::InputRecorder::events());
    return text;
}

void RecordInput::beginRecording() {
    Project* p = project();
    const SceneProject* sp = p ? p->getScene(p->getPlaySessionSceneId()) : nullptr;
    input::RecordOptions options;
    input::ScriptHeader& h = options.header;
    h.kind = input::ScriptKind::Recording;
    h.fromPlayStart = true;
    h.fixedFps = sessionFps;
    h.space = input::Space::Normalized;
    h.hasSeed = true;
    h.seed = recordSeed;
    h.scene = sp && !sp->filepath.empty() ? FileUtils::pathToGenericUtf8(p->normalizeToProjectRelative(sp->filepath)) : std::string();
    h.platform = input::platformTag();
    h.engineVersion = JASPERENGINE_EDITOR_VERSION;
    h.canvas = Vector2(static_cast<float>(Engine::getCanvasWidth()), static_cast<float>(Engine::getCanvasHeight()));
    h.simulateTouch = p && p->isSimulateTouch();
    options.checkpointFrames = currentSettings.checkpointFrames;
    options.probes = currentSettings.probes;
    std::string error;
    if (!input::InputRecorder::begin(options, error)) {
        Out::error("Record Input: %s", error.c_str());
        recordThisSession = false;
        return;
    }
    recordFile = recordingsFolder(p ? p->getProjectPath() : fs::path()) / recordingFileName(sp ? sp->name : std::string(), localNow());
    Out::info("Record Input: recording from frame 0 (seed %u, %d fps) into %s", recordSeed, sessionFps,
              FileUtils::pathToGenericUtf8(p ? p->normalizeToProjectRelative(recordFile) : recordFile).c_str());
}

void RecordInput::stopRecording() {
    if (!recordThisSession) return;
    recordThisSession = false;
    std::shared_ptr<input::RecordedInput> recorded = input::InputRecorder::end();
    if (!recorded) return;
    finishRecording(std::move(recorded), recordFile);
}

void RecordInput::finishRecording(std::shared_ptr<input::RecordedInput> recorded, const fs::path& file) {
    if (pending && !pending->error.empty()) {
        Out::warning("Record Input: the unsaved recording %s is dropped for the new one", FileUtils::pathToGenericUtf8(pending->path).c_str());
    }
    pending.reset();
    if (encoding.valid()) encoding.wait();
    writing = true;
    // the swap was O(1); the JSON is made off the main thread (R15)
    encoding = std::async(std::launch::async, [recorded, file]() -> std::optional<PendingWrite> {
        PendingWrite out;
        out.path = file;
        std::string error;
        if (!input::writeScript(*recorded->toScript(), out.text, error)) {
            out.error = "The input recording could not be encoded: " + error;
            out.text.clear();
        }
        return out;
    });
}

bool RecordInput::pollWrite() {
    if (!encoding.valid()) return !writing;
    if (encoding.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    std::optional<PendingWrite> encoded = encoding.get();
    writing = false;
    if (!encoded || encoded->text.empty()) {
        Out::error("Record Input: %s", encoded ? encoded->error.c_str() : "the recording was not encoded");
        return true;
    }
    pending = std::move(*encoded);
    startWrite();
    return true;
}

void RecordInput::startWrite() {
    if (!pending) return;
    if (writePending(*pending, host.writer)) {
        lastWrittenFile = pending->path;
        const Project* p = project();
        Out::success("Record Input: saved %s",
                     FileUtils::pathToGenericUtf8(p ? p->normalizeToProjectRelative(pending->path) : pending->path).c_str());
        pending.reset();
        return;
    }
    Out::error("%s (Record Input menu > Retry Saving Recording)", pending->error.c_str());
}

void RecordInput::retryWrite() {
    if (!pending || writing) return;
    startWrite();
}

// ---------------------------------------------------------------- Replay Input

bool RecordInput::loadReplay(const std::string& file, std::shared_ptr<input::ParsedScript>& script, std::string& error) const {
    const Project* p = project();
    fs::path path = FileUtils::pathFromUtf8(file);
    if (path.is_relative() && p) path = p->getProjectPath() / path;
    bool ok = false;
    const std::string text = readInputFile(path, ok);
    if (!ok) {
        error = "No input file at " + FileUtils::pathToGenericUtf8(path) + " (input is project-relative or absolute).";
        return false;
    }
    script = std::make_shared<input::ParsedScript>();
    if (!input::parseScript(text, *script, error)) {
        error = file + ": " + error + ".";
        script.reset();
        return false;
    }
    return true;
}

void RecordInput::prepareReplay(std::shared_ptr<input::ParsedScript> script, const std::string& file, const fs::path& shotFolder, bool shots) {
    pendingScript = std::move(script);
    replay.reset();
    replayPath = file;
    takeShots = shots;
    takenShots.clear();
    shotDir = shotFolder;
    if (shotDir.empty()) {
        const Project* p = project();
        shotDir = (p ? p->getProjectPath() : fs::path()) / "Recordings" / "Input" / fileStem(FileUtils::pathFromUtf8(file));
    }
}

void RecordInput::cancelPrepared() {
    pendingScript.reset();
    if (phase == Phase::Starting) endSession();
}

bool RecordInput::replayFile(const std::string& file, std::string& error) {
    Project* p = project();
    if (!p) {
        error = "No project is open.";
        return false;
    }
    if (playing()) {
        error = "Play is running: a replay starts its own session (stop Play first).";
        return false;
    }
    std::shared_ptr<input::ParsedScript> script;
    if (!loadReplay(file, script, error)) return false;
    return startSession(std::move(script), file, {}, true, error);
}

bool RecordInput::startSession(std::shared_ptr<input::ParsedScript> script, const std::string& file, const fs::path& shotFolder, bool shots,
                               std::string& error) {
    Project* p = project();
    if (!p || playing()) {
        error = p ? "Play is running: a replay starts its own session (stop Play first)." : "No project is open.";
        return false;
    }
    // a recording's scene, else the one Play would run
    uint32_t sceneId = p->getSceneBehindPrefabStage(p->getSelectedSceneId());
    if (script->header.kind == input::ScriptKind::Recording && !script->header.scene.empty()) {
        const fs::path wanted = (p->getProjectPath() / FileUtils::pathFromUtf8(script->header.scene)).lexically_normal();
        uint32_t found = NULL_PROJECT_SCENE;
        for (const SceneProject& sp : p->getScenes()) {
            const fs::path at = (sp.filepath.is_absolute() ? sp.filepath : p->getProjectPath() / sp.filepath).lexically_normal();
            if (at == wanted && sp.opened) found = sp.id;
        }
        if (found == NULL_PROJECT_SCENE) found = p->openSceneNow(wanted, false, UnsavedChoice::Cancel, &error);
        if (found == NULL_PROJECT_SCENE) {
            error = "The recording's scene " + script->header.scene + " did not open: " + error;
            return false;
        }
        sceneId = found;
    }
    if (!p->getScene(sceneId)) {
        error = "No scene to play: open one.";
        return false;
    }
    prepareReplay(script, file, shotFolder, shots);
    p->start(sceneId);
    if (p->getPlaySessionSceneId() != sceneId) {
        cancelPrepared();
        error = "Play did not start (see the Console).";
        return false;
    }
    return true;
}

bool RecordInput::replayRunning() const {
    return replay && !replay->isDone();
}

const input::ParsedScript* RecordInput::replayScript() const {
    if (pendingScript) return pendingScript.get();
    return replay ? &replay->script() : nullptr;
}

void RecordInput::startReplay() {
    std::shared_ptr<input::ParsedScript> script = std::move(pendingScript);
    pendingScript.reset();
    const Project* p = project();
    input::ReplayOptions options;
    options.play.simulateTouch = script->header.kind == input::ScriptKind::Recording ? script->header.simulateTouch : (p && p->isSimulateTouch());
    if (takeShots) {
        options.host.shot = [this](const std::string& name, uint64_t, std::string&) {
            ShotRequest request;
            request.name = name;
            request.started = std::chrono::steady_clock::now();
            shotRequest = request;
            applyHold(true);
            return true;
        };
    }
    // the game waits for a shot until its PNG is written: the frames after it are the recorded ones
    options.host.busy = [this]() { return shotPending(); };
    replay = input::Replay::start(script, std::move(options));
}

void RecordInput::pollShot() {
    for (auto it = shotWrites.begin(); it != shotWrites.end();) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        Shot shot = it->get();
        if (shot.error.empty()) Out::info("Record Input: shot \"%s\" saved to %s", shot.name.c_str(), FileUtils::pathToGenericUtf8(shot.path).c_str());
        else Out::error("Record Input: shot \"%s\" failed: %s", shot.name.c_str(), shot.error.c_str());
        takenShots.push_back(std::move(shot));
        it = shotWrites.erase(it);
    }
    if (shotRequest) {
        ShotRequest& request = *shotRequest;
        const bool idle = !host.assetsIdle || host.assetsIdle();
        request.settledFrames = idle ? request.settledFrames + 1 : 0;
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - request.started).count();
        Shot shot;
        shot.name = request.name;
        shot.path = shotDir / (request.name + ".png");
        if (!idle && waited > SHOT_WAIT_SECONDS) {
            shot.error = "still loading after " + std::to_string(static_cast<int>(SHOT_WAIT_SECONDS)) + " s: a stand-in is never saved";
            takenShots.push_back(shot);
            shotRequest.reset();
        } else if (request.settledFrames >= 2) {
            // two frames drawn with nothing loading: the image shows the loaded content
            std::vector<uint8_t> rgba;
            if (!host.readGameView || !host.readGameView(rgba, shot.width, shot.height) || shot.width <= 0 || shot.height <= 0) {
                shot.error = "the Game view could not be read back";
                takenShots.push_back(shot);
            } else {
                std::error_code ec;
                fs::create_directories(shotDir, ec);
                shotWrites.push_back(std::async(std::launch::async, &writeShot, shot, std::move(rgba)));
            }
            shotRequest.reset();
        }
    }
    if (!shotPending() && holding && phase == Phase::Running) applyHold(false);
}

Json RecordInput::shotsJson() const {
    Json out = Json::array();
    const Project* p = project();
    for (const Shot& shot : takenShots) {
        Json item = {{"name", shot.name}, {"path", FileUtils::pathToGenericUtf8(p ? p->normalizeToProjectRelative(shot.path) : shot.path)}};
        if (!shot.error.empty()) item["error"] = shot.error;
        else item["size"] = Json::array({shot.width, shot.height});
        out.push_back(item);
    }
    return out;
}

Json RecordInput::replayingJson() const {
    const input::ParsedScript* script = replayScript();
    if (!script) return nullptr;
    Json out = {{"file", replayPath}, {"kind", script->header.kind == input::ScriptKind::Recording ? "recording" : "script"},
                {"last_frame", script->lastFrame()}, {"frame", -1}, {"events_left", script->actions.size()}};
    if (replay) {
        const input::ReplayReport& report = replay->report();
        out["frame"] = replay->frame();
        out["done"] = replay->isDone();
        out["events_left"] = script->actions.size() - std::min(report.actionsDone, script->actions.size());
        out["comparable"] = report.comparable;
        out["compared"] = report.compared;
        out["diverged_at_frame"] = report.divergedAtFrame >= 0 ? Json(report.divergedAtFrame) : Json(nullptr);
        out["differences"] = report.differences;
        if (report.failed) out["error"] = replay->errorText();
    }
    return out;
}

std::string RecordInput::bannerText() const {
    const input::ParsedScript* script = replayScript();
    if (!script || (replay && replay->isDone() && !shotPending())) return std::string();
    const std::string name = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(replayPath).filename());
    if (!replay) return "Replaying " + name + " \xc2\xb7 waiting for the scene";
    return "Replaying " + name + " \xc2\xb7 frame " + std::to_string(std::max<int64_t>(replay->frame(), 0)) + "/" + std::to_string(script->lastFrame());
}

std::string RecordInput::divergenceText() const {
    if (!replay) return std::string();
    const input::ReplayReport& report = replay->report();
    if (report.divergedAtFrame < 0) return std::string();
    std::string text = "Diverged at frame " + std::to_string(report.divergedAtFrame);
    if (report.differences.is_array() && !report.differences.empty()) {
        const Json& first = report.differences[0];
        if (first.contains("probe")) text += ": " + first["probe"].get<std::string>() + " moved " + first.value("distance", Json(0.0)).dump();
        else if (first.contains("value")) text += ": value " + first["value"].get<std::string>() + " differs";
    }
    return text;
}

// ---------------------------------------------------------------- the session

void RecordInput::playStarting() {
    if (gametests::TestRun::get().running()) {
        // a test run owns its sessions' clock and input
        pendingScript.reset();
        return;
    }
    if (phase != Phase::Idle) endSession();
    if (pendingScript) {
        const input::ScriptHeader& h = pendingScript->header;
        sessionFps = std::max(1, h.fixedFps);
        if (h.kind == input::ScriptKind::Recording && h.hasSeed) {
            seedBeforeScripts(h.seed);
            seeded = true;
        }
    } else if (armed) {
        if (input::InputRecorder::isRecording()) {
            Out::warning("Record Input: an input recording already runs (InputRecording.start): this Play is not recorded");
            return;
        }
        recordThisSession = true;
        std::random_device device;
        recordSeed = static_cast<uint32_t>(device());
        seedBeforeScripts(recordSeed);
        seeded = true;
        sessionFps = currentSettings.frameRate;
    } else {
        return;
    }
    Engine::setFixedDeltatime(1.0 / sessionFps);
    applyHold(true);
    phase = Phase::Starting;
}

void RecordInput::endSession() {
    if (recordThisSession) stopRecording();
    recordThisSession = false;
    if (seeded) {
        gametests::PlayData::get().beforeScripts = nullptr;
        seeded = false;
    }
    pendingScript.reset();
    if (replay && !replay->isDone()) replay->stop();
    shotRequest.reset();
    applyHold(false);
    phase = Phase::Idle;
    sessionFps = 0;
}

void RecordInput::tick() {
    pollWrite();
    if (phase == Phase::Idle) {
        pollShot();
        return;
    }
    if (!playing()) {
        // the shots in flight are still written; a running recording is saved
        pollShot();
        endSession();
        return;
    }
    if (phase == Phase::Starting) {
        if (!sceneRunning()) return;
        // the scene runs: the recording and the replay start with the next frame, the game let go
        if (recordThisSession) beginRecording();
        if (pendingScript) startReplay();
        // the scripts' init ran with the seed
        if (seeded) {
            gametests::PlayData::get().beforeScripts = nullptr;
            seeded = false;
        }
        phase = Phase::Running;
        applyHold(false);
        return;
    }
    // a recording the cap stopped is saved now (truncated)
    if (recordThisSession && !input::InputRecorder::isRecording()) stopRecording();
    pollShot();
}

}  // namespace jasperengine::editor::recordinput
