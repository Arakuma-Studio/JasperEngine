// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/TestRunHost.h"

#include "App.h"
#include "Out.h"
#include "Project.h"
#include "command/PlayChangeReview.h"
#include "mcp/McpRefs.h"
#include "mcp/tools/PlayViewCommon.h"
#include "pool/ShaderPool.h"
#include "pool/TextureDataPool.h"
#include "texture/Framebuffer.h"
#include "script/LuaBinding.h"
#include "test/GameTests.h"
#include "test/PlayData.h"
#include "texsynth/SynthImage.h"
#include "util/FileUtils.h"
#include "util/GraphicUtils.h"
#include "window/ConsoleWindow.h"
#include "window/GameWindow.h"

#include "Engine.h"
#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::gametests {

namespace {

    constexpr const char* INPUT_BLOCKED = "Tests are running: input to the game is ignored until the run ends (Test Runner > Cancel)";
    constexpr double CAPTURE_WAIT_SECONDS = 120.0;

    bool isRunning(ScenePlayState state) {
        return state == ScenePlayState::PLAYING || state == ScenePlayState::PAUSED;
    }

    std::chrono::steady_clock::time_point captureStarted;
    uint64_t sessionLogCursor = 0;

}

TestRunHost::TestRunHost(App& a, Project* p, fs::path r, gametest::Settings s, gametest::Target t, bool keep)
    : app(a), project(p), root(std::move(r)), settings(std::move(s)), target(std::move(t)), keepData(keep) {
    mcp::w3::setPlayClockOwnedByTests(true);
    if (ConsoleWindow* console = app.getConsoleWindow()) {
        savedErrorPause = console->isErrorPause();
        errorPauseSaved = true;
        console->setErrorPause(false);
    }
    if (GameWindow* game = app.getGameWindow()) game->setInputBlockedReason(INPUT_BLOCKED);
}

TestRunHost::~TestRunHost() {
    restore();
}

void TestRunHost::restore() {
    if (heldForCapture || heldForReplay) {
        Engine::setSimulationHeld(false);
        heldForCapture = false;
        heldForReplay = false;
    }
    PlayData::get().beforeScripts = nullptr;
    if (GameWindow* game = app.getGameWindow()) {
        game->setRenderSizeOverride(0, 0);
        game->setInputBlockedReason("");
    }
    if (errorPauseSaved) {
        if (ConsoleWindow* console = app.getConsoleWindow()) console->setErrorPause(savedErrorPause);
        errorPauseSaved = false;
    }
    mcp::w3::setPlayClockOwnedByTests(false);
}

bool TestRunHost::beginSession(const gametest::CaseSpec& caseSpec, std::string& error) {
    spec = caseSpec;
    started = false;
    stopping = false;
    capturing = false;
    if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        error = "Play is running: tests start their own sessions";
        return false;
    }
    SceneProject* sp = nullptr;
    if (spec.scene.empty()) {
        sp = project->getScene(project->getStartSceneId());
        if (!sp) {
            error = "the project has no start scene: set one (Build Settings) or give the case a scene";
            return false;
        }
    } else {
        sp = mcp::McpRefs::resolveScene(project, spec.scene, error);
        if (!sp) {
            std::error_code ec;
            const fs::path file = project->getProjectPath() / FileUtils::pathFromUtf8(spec.scene);
            if (!fs::is_regular_file(file, ec)) {
                error = "no scene \"" + spec.scene + "\": " + error;
                return false;
            }
            error.clear();
            const uint32_t opened = project->openSceneNow(file, false, UnsavedChoice::Cancel, &error);
            sp = project->getScene(opened);
            if (!sp) return false;
        }
    }
    if (!sp->opened || !sp->scene) {
        // Play runs an open scene
        const uint32_t opened = project->openSceneNow(sp->filepath, false, UnsavedChoice::Cancel, &error);
        sp = project->getScene(opened);
        if (!sp) {
            error = "cannot open the case's scene: " + error;
            return false;
        }
    }
    playSceneId = sp->id;
    // a fresh Lua state (R14): nothing an earlier case or Play session left in Lua reaches this one
    LuaBinding::recreateLuaState();
    PlayData::get().setNextSession(spec.data, keepData);
    const uint64_t seed = spec.seed;
    PlayData::get().beforeScripts = [seed]() {
        lua_State* L = LuaBinding::getLuaState();
        if (!L) return;
        lua_getglobal(L, "math");
        lua_getfield(L, -1, "randomseed");
        lua_pushinteger(L, static_cast<lua_Integer>(seed));
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
        lua_pop(L, 1);
    };
    if (GameWindow* game = app.getGameWindow()) game->setRenderSizeOverride(spec.width, spec.height);
    Engine::setFixedDeltatime(1.0 / std::max(1, spec.fixedFps));
    PlayChangeReview::get().setQuietStop(true);
    sessionLogCursor = Out::lastLogId();
    heldForReplay = !spec.replay.empty();
    if (heldForReplay) Engine::setSimulationHeld(true);
    project->start(playSceneId);
    if (project->getPlaySessionSceneId() != playSceneId) {
        if (heldForReplay) Engine::setSimulationHeld(false);
        heldForReplay = false;
        error = "Play did not start";
        for (const LogEntry& entry : Out::getLogsSince(sessionLogCursor, 3, true)) error += ": " + entry.message;
        return false;
    }
    framesAtStart = Engine::getGameFrames();
    started = true;
    return true;
}

gametest::Host::SessionState TestRunHost::sessionState(std::string& error) {
    if (!started) return SessionState::Failed;
    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId == NULL_PROJECT_SCENE) {
        error = "Play stopped";
        for (const LogEntry& entry : Out::getLogsSince(sessionLogCursor, 0, true)) {
            if (entry.type != LogType::Error) continue;
            error += ": " + entry.message.substr(0, 2000);
            break;
        }
        return SessionState::Failed;
    }
    SceneProject* sp = project->getScene(playId);
    if (sp && isRunning(sp->playState) && heldForReplay) {
        // the runner starts the replay now: its frame 0 is the session's first update
        heldForReplay = false;
        Engine::setSimulationHeld(false);
        return SessionState::Ready;
    }
    if (sp && isRunning(sp->playState) && Engine::getGameFrames() > framesAtStart) return SessionState::Ready;
    return SessionState::Starting;
}

void TestRunHost::enforce() {
    if (!started || stopping) return;
    // the case's clock, whatever Play's tools set since
    Engine::setFixedDeltatime(1.0 / std::max(1, spec.fixedFps));
    if (!capturing && !heldForReplay && Engine::isSimulationHeld()) Engine::setSimulationHeld(false);
    if (GameWindow* game = app.getGameWindow()) game->setRenderSizeOverride(spec.width, spec.height);
}

void TestRunHost::endSession() {
    if (heldForCapture || heldForReplay) {
        Engine::setSimulationHeld(false);
        heldForCapture = false;
        heldForReplay = false;
    }
    capturing = false;
    stopping = true;
    PlayData::get().beforeScripts = nullptr;
    const uint32_t playId = project->getPlaySessionSceneId();
    PlayChangeReview::get().setQuietStop(true);
    if (playId != NULL_PROJECT_SCENE) {
        SceneProject* sp = project->getScene(playId);
        if (sp && sp->playState != ScenePlayState::CANCELLING) project->stop(playId);
    }
}

bool TestRunHost::sessionEnded() {
    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId != NULL_PROJECT_SCENE) return false;
    SceneProject* sp = project->getScene(playSceneId);
    if (sp && sp->playState != ScenePlayState::STOPPED) return false;
    // every play-mode change is discarded (keep_changes: none)
    PlayChangeReview::get().discard(playSceneId);
    PlayChangeReview::get().setQuietStop(false);
    PlayData::get().update(project);
    dataFolderPath = FileUtils::pathToUtf8(PlayData::get().lastFolder());
    started = false;
    return true;
}

lua_State* TestRunHost::luaState() {
    return LuaBinding::getLuaState();
}

bool TestRunHost::readTestFile(const std::string& file, std::string& code, std::string& error) {
    const fs::path path = root / FileUtils::pathFromUtf8(file);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + FileUtils::pathToGenericUtf8(path);
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    code = text.str();
    return true;
}

uint64_t TestRunHost::gameFrames() {
    return Engine::getGameFrames();
}

double TestRunHost::gameTime() {
    return Engine::getGameTime();
}

std::string TestRunHost::dataFolder() const {
    return PlayData::get().active() ? FileUtils::pathToUtf8(PlayData::get().folder()) : dataFolderPath;
}

bool TestRunHost::beginCapture(const gametest::CaseSpec&, const std::string& name, const gametest::Json& options, std::string& error) {
    GameWindow* game = app.getGameWindow();
    if (!game || !game->getFramebuffer()) {
        error = "no Game view to capture";
        return false;
    }
    capturing = true;
    captureName = name;
    captureOptions = options;
    settledFrames = 0;
    captureFrames = 0;
    captureStarted = std::chrono::steady_clock::now();
    // the game holds while the frame waits for its shaders and textures (never a stand-in image)
    if (!Engine::isSimulationHeld()) {
        Engine::setSimulationHeld(true);
        heldForCapture = true;
    }
    return true;
}

bool TestRunHost::pollCapture(gametest::Json& capture) {
    GameWindow* game = app.getGameWindow();
    Framebuffer* fb = game ? game->getFramebuffer() : nullptr;
    const bool idle = ShaderPool::isCreationIdle() && TextureDataPool::getLoadingCount() == 0;
    settledFrames = idle ? settledFrames + 1 : 0;
    captureFrames++;
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - captureStarted).count();
    auto finish = [&]() {
        capturing = false;
        if (heldForCapture) {
            Engine::setSimulationHeld(false);
            heldForCapture = false;
        }
    };
    capture = gametest::Json{{"name", captureName}};
    if (!idle && waited > CAPTURE_WAIT_SECONDS) {
        capture["status"] = "failed";
        capture["error"] = "still loading after " + std::to_string(static_cast<int>(CAPTURE_WAIT_SECONDS)) + " s (shaders or textures): no capture";
        finish();
        return true;
    }
    // two drawn frames with everything loaded: the frame shows the loaded content
    if (settledFrames < 2 || !fb || !fb->isCreated()) return false;

    std::vector<uint8_t> rgba;
    if (!GraphicUtils::readFramebufferRGBA(fb, rgba)) {
        capture["status"] = "failed";
        capture["error"] = "reading the Game view back failed";
        finish();
        return true;
    }
    const int width = static_cast<int>(fb->getWidth());
    const int height = static_cast<int>(fb->getHeight());
    texsynth::Image actual = texsynth::Image::make(width, height, 4, 8);
    actual.data = std::move(rgba);
    // opaque, as a player's --screenshot writes it: the swap chain shows no alpha
    for (size_t i = 3; i < actual.data.size(); i += 4) actual.data[i] = 255;

    capture = compareCapture(project->getProjectPath(), root, spec.id, captureName, actual, target.platform, target.backend, settings.image,
                             captureOptions);
    finish();
    return true;
}

}  // namespace jasperengine::editor::gametests
