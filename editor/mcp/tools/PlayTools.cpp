// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W3: play control, runtime inspection, Lua eval, input injection
//
//   play                  start / stop / pause / resume / step / restart, waits (job) for startup
//   get_play_state        state of the play session, crash report of the last one
//   wait                  let the game run N seconds / frames / until a Lua condition, then report
//   eval_lua              run Lua in the game's Lua state, results as JSON
//   list_runtime_entities / get_runtime_entity / set_runtime_property   live GameObjects
//   send_input            keyboard / text / mouse / touch / gamepad sequences across frames
#include "camera/ShakerRuntime.h"
#include "component/CameraFollowComponent.h"
#include "component/CameraShakerComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "object/physics/Joint3D.h"
#include "object/physics/Rigidbody.h"
#include "McpTools.h"

#include "debug/DebugOverlay.h"
#include "HtmlUiShared.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpValues.h"
#include "subsystem/VideoPlayerSystem.h"

#include "App.h"
#include "Catalog.h"
#include "Engine.h"
#include "net/Http.h"
#include "Input.h"
#include "input/InputActions.h"
#include "input/InputControlPath.h"
#include "Platform.h"
#include <filesystem>
#include <memory>
#include "audio/AudioAnalysis.h"
#include "audio/AudioCapture.h"
#include "audio/AudioMixer.h"
#include "audio/MusicPlayer.h"
#include "audio/AudioSessionReader.h"
#include "jobs/JobSystem.h"
#include "audio/LoudnessMonitor.h"
#include "subsystem/AudioSystem.h"
#include "Project.h"
#include "Screen.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
#include "command/PlayChangeReview.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/ScriptComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "object/physics/Joint2D.h"
#include "object/physics/Rigidbody2D.h"
#include "subsystem/PhysicsSystem.h"
#include "script/LuaBinding.h"
#include "script/ScriptScheduler.h"
#include "script/ScriptRuntime.h"
#include "manager/SceneManager.h"
#include "util/EntityPath.h"
#include "util/PlatformRules.h"
#include "window/DeviceProfiles.h"
#include "window/GameWindow.h"
#include "window/PlayInput.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include "object/EntityHandle.h"
#include "math/Quaternion.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>

namespace jasperengine::editor::mcp {

namespace {

using Clock = std::chrono::steady_clock;

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isRunningState(ScenePlayState state) {
    return state == ScenePlayState::PLAYING || state == ScenePlayState::PAUSED;
}

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// A call that waits for game frames is guarded against a game that stopped advancing, not by a
// wall-clock budget: under load (software rendering, a busy host) a frame takes any time, and a
// wait for N frames must still run N frames, so that a fixed_fps game reaches the same state
// however fast the machine is. Stalled: no game frame for kStallSeconds.
constexpr double kStallSeconds = 60.0;

struct FrameProgress {
    uint64_t frames = Engine::getGameFrames();
    Clock::time_point at = Clock::now();

    bool stalled() {
        const uint64_t now = Engine::getGameFrames();
        if (now != frames) {
            frames = now;
            at = Clock::now();
            return false;
        }
        return secondsSince(at) >= kStallSeconds;
    }
};

double round3(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

// ================================================================== session tracking

// What the server saw of a play session (whoever started it)
struct SessionRecord {
    uint64_t serial = 0;
    bool active = false;
    uint32_t sceneId = NULL_PROJECT_SCENE;
    std::string sceneName;
    uint64_t startLogId = 0;
    uint64_t scanCursor = 0;
    Clock::time_point startTime;
    bool reachedPlaying = false;
    bool stopRequested = false;     // stopped through the play tool
    uint64_t framesPlaying = 0;     // game frames (Engine::getGameFrames since the session began)
    bool ranFirstFrame = false;     // a game frame updated the running scene (afterGameFrame)
    uint64_t engineFramesBase = 0;
    double engineTimeBase = 0.0;
    double gameTime = 0.0;          // scaled seconds the game advanced
    size_t errors = 0;
    size_t warnings = 0;
    std::string crash;              // "Script crash in scene ..." message
    // when ended
    std::string endReason;          // stopped | crashed | failed_to_start | cancelled | stopped_outside_mcp
    double durationSeconds = 0.0;
};

// Input an agent is holding down in the game (send_input), released at stop
struct HeldInput {
    std::set<int> keys;
    std::set<int> mouseButtons;
    Vector2 mousePos = Vector2(0, 0);   // view (framebuffer) pixels
    bool mousePosKnown = false;         // mousePos was set by an injected event this session
    std::map<int, Vector2> touches;     // pointer → view pixels
    std::set<int> gamepads;             // connected virtual gamepads
    std::map<int, std::set<int>> gamepadButtons;
    std::map<int, std::set<int>> gamepadAxes;
    std::set<std::string> actions;      // Input Actions forced down (input_action down)
    // the Input Manager's actions held and axes set through the "mcp" source (action / axis events)
    std::set<std::string> managerActions;
    std::map<std::string, float> axes;
    bool empty() const {
        if (!keys.empty() || !mouseButtons.empty() || !touches.empty() || !gamepads.empty() || !actions.empty() ||
            !managerActions.empty() || !axes.empty()) return false;
        for (const auto& [id, buttons] : gamepadButtons) if (!buttons.empty()) return false;
        return true;
    }
};

struct PlayStartWait {
    JobPtr job;
    uint32_t sceneId = NULL_PROJECT_SCENE;
    uint64_t startLogId = 0;
    Clock::time_point started;
    bool cancelling = false;
};

// Game clock settings of the MCP (play time_scale / fixed_fps / auto_pause); they stay for
// later sessions until changed
struct TimeControl {
    double timeScale = 1.0;
    int fixedFps = 0;               // 0: real time
    bool autoPause = false;
    int advancers = 0;              // calls letting an auto-paused game run (wait, send_input, step)
};

struct PlayToolsState {
    McpContext* frameHookCtx = nullptr;  // afterGameFrame's context
    TimeControl time;
    SessionRecord current;
    SessionRecord last;
    uint64_t serialCounter = 0;
    uint64_t pendingStartLogId = 0;     // set by play start just before Project::start
    HeldInput held;
    std::vector<PlayStartWait> startWaits;
    bool monitorScheduled = false;
};

PlayToolsState& state() {
    static PlayToolsState s;
    return s;
}

void releaseHeldInput(McpContext& ctx);

void scanSessionLogs(SessionRecord& record) {
    const std::vector<LogEntry> logs = Out::getLogsSince(record.scanCursor, 0, true);
    for (const LogEntry& entry : logs) {
        record.scanCursor = entry.id;
        if (entry.type == LogType::Error) {
            record.errors++;
            if (record.crash.empty() && entry.message.rfind("Script crash in scene", 0) == 0) record.crash = entry.message;
        } else if (entry.type == LogType::Warning) {
            record.warnings++;
        }
    }
    record.scanCursor = std::max(record.scanCursor, Out::lastLogId());
}

Json problemsSince(uint64_t since, size_t limit) {
    w3::ConsoleQuery query;
    query.since = since;
    query.onlyProblems = true;
    query.limit = 0;
    query.maxMessageChars = 3000;
    w3::ConsoleResult all = w3::queryConsole(query);
    // Build output is mostly progress lines: keep errors, warnings and build lines that
    // look like diagnostics, the newest when there are too many
    Json picked = Json::array();
    int context = 0;   // source lines a compiler prints under a diagnostic
    for (const Json& entry : all.entries) {
        const std::string type = entry.value("type", "");
        const std::string text = lowerCopy(entry.value("message", ""));
        const bool diagnostic = text.find("error") != std::string::npos || text.find("fatal") != std::string::npos ||
                                text.find("undefined reference") != std::string::npos || text.find("failed") != std::string::npos;
        if (type == "error" || type == "warning" || diagnostic) {
            picked.push_back(entry);
            context = (type == "build" && (text.find("error:") != std::string::npos || text.find("warning:") != std::string::npos)) ? 2 : 0;
        } else if (context > 0 && type == "build") {
            picked.push_back(entry);
            context--;
        }
    }
    if (picked.size() > limit) {
        Json tail = Json::array();
        for (size_t i = picked.size() - limit; i < picked.size(); i++) tail.push_back(picked[i]);
        return tail;
    }
    return picked;
}

Json sessionJson(const SessionRecord& record) {
    if (record.serial == 0) return nullptr;
    Json out = {
        {"scene_id", record.sceneId},
        {"scene", record.sceneName},
        {"console_cursor_at_start", record.startLogId},
        {"errors", record.errors},
        {"warnings", record.warnings},
        {"crashed", !record.crash.empty()},
        {"frames", record.framesPlaying},
        {"game_time", round3(record.gameTime)}
    };
    if (!record.crash.empty()) out["crash"] = record.crash;
    if (record.active) {
        out["seconds"] = round3(secondsSince(record.startTime));
    } else {
        out["ended"] = record.endReason;
        out["seconds"] = round3(record.durationSeconds);
    }
    return out;
}

void finishStartWaits(McpContext& ctx);

// ================================================================== game clock

// The MCP's time_scale / fixed_fps on the Engine (Project resets both when play stops)
void applyTimeSettings() {
    const TimeControl& t = state().time;
    Engine::setTimeScale(static_cast<float>(t.timeScale));
    Engine::setFixedDeltatime(t.fixedFps > 0 ? 1.0 / t.fixedFps : 0.0);
}

// auto_pause: the simulation is held unless a call is advancing it; the game always gets its
// first frame (so it has drawn once) before the first hold
void applyHold(McpContext& ctx) {
    PlayToolsState& s = state();
    Project* project = ctx.project();
    const uint32_t playId = project ? project->getPlaySessionSceneId() : NULL_PROJECT_SCENE;
    SceneProject* sp = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
    const bool running = sp && isRunningState(sp->playState) && s.current.active;
    const bool hold = running && s.time.autoPause && s.time.advancers == 0 && s.current.ranFirstFrame;
    if (Engine::isSimulationHeld() != hold) Engine::setSimulationHeld(hold);
}

// Engine::onPostUpdate, after every game frame's update: the first frame that updates the running
// scene is the last before an auto_pause hold. Holding here, not when a tool next looks (one or
// more editor frames later), makes the frames a game runs before its first hold the same on every
// machine: one.
void afterGameFrame() {
    PlayToolsState& s = state();
    McpContext* ctx = s.frameHookCtx;
    if (!ctx || !s.current.active) return;
    Project* project = ctx->project();
    if (!project || project->getPlaySessionSceneId() != s.current.sceneId) return;
    SceneProject* sp = project->getScene(s.current.sceneId);
    if (!sp || !isRunningState(sp->playState)) return;
    s.current.ranFirstFrame = true;
    applyHold(*ctx);
}

// (again on every look: Engine::clearAllSubscriptions at play stop drops it; add replaces)
void subscribeFrameHook(McpContext& ctx) {
    state().frameHookCtx = &ctx;
    Engine::onPostUpdate.add("jasperengine::mcp::PlayTools_afterGameFrame", std::function<void()>(afterGameFrame));
}

// A call lets the game run (auto_pause) from now until its Advance is released
struct Advance {
    McpContext* ctx = nullptr;
    bool active = false;
    void acquire(McpContext& c) {
        if (active) return;
        ctx = &c;
        active = true;
        state().time.advancers++;
        applyHold(c);
    }
    void release() {
        if (!active) return;
        active = false;
        state().time.advancers = std::max(0, state().time.advancers - 1);
        applyHold(*ctx);
    }
    // a dropped call (cancelled): the monitor re-applies the hold on its next frame
    ~Advance() {
        if (active) state().time.advancers = std::max(0, state().time.advancers - 1);
    }
};

Json timeJson(bool withSession) {
    const PlayToolsState& s = state();
    // while it plays, the Engine's (a script may change Engine.timeScale)
    Json out = {
        {"time_scale", withSession ? round3(Engine::getTimeScale()) : s.time.timeScale},
        {"fixed_fps", s.time.fixedFps > 0 ? Json(s.time.fixedFps) : Json(nullptr)},
        {"auto_pause", s.time.autoPause}
    };
    if (withSession) {
        out["held"] = Engine::isSimulationHeld();
        out["game_time"] = round3(s.current.gameTime);
        out["frames"] = s.current.framesPlaying;
    }
    if (withSession && std::abs(Engine::getTimeScale() - s.time.timeScale) > 1e-3) out["time_scale_set_by_mcp"] = s.time.timeScale;
    return out;
}

// Called every frame by the monitor and at the start of every play tool call
void observeSession(McpContext& ctx) {
    PlayToolsState& s = state();
    Project* project = ctx.project();
    const uint32_t playId = project ? project->getPlaySessionSceneId() : NULL_PROJECT_SCENE;
    if (playId != NULL_PROJECT_SCENE) subscribeFrameHook(ctx);

    if (playId != NULL_PROJECT_SCENE && (!s.current.active || s.current.sceneId != playId)) {
        if (s.current.active) {
            s.current.active = false;
            s.current.endReason = "stopped_outside_mcp";
            s.last = s.current;
        }
        SessionRecord record;
        record.serial = ++s.serialCounter;
        record.active = true;
        record.sceneId = playId;
        SceneProject* sp = project->getScene(playId);
        record.sceneName = sp ? sp->name : std::string();
        record.startLogId = s.pendingStartLogId ? s.pendingStartLogId : Out::lastLogId();
        record.scanCursor = record.startLogId;
        record.startTime = Clock::now();
        record.engineFramesBase = Engine::getGameFrames();
        record.engineTimeBase = Engine::getGameTime();
        s.pendingStartLogId = 0;
        s.current = record;
        applyTimeSettings();
    }

    if (s.current.active) {
        scanSessionLogs(s.current);
        SceneProject* sp = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
        if (sp && isRunningState(sp->playState)) {
            s.current.reachedPlaying = true;
        }
        if (playId != NULL_PROJECT_SCENE) {
            s.current.framesPlaying = Engine::getGameFrames() - std::min(Engine::getGameFrames(), s.current.engineFramesBase);
            s.current.gameTime = std::max(0.0, Engine::getGameTime() - s.current.engineTimeBase);
        }
        if (playId == NULL_PROJECT_SCENE) {
            SessionRecord& r = s.current;
            r.active = false;
            r.durationSeconds = secondsSince(r.startTime);
            if (!r.crash.empty()) r.endReason = "crashed";
            else if (!r.reachedPlaying) r.endReason = r.stopRequested ? "cancelled" : "failed_to_start";
            else if (r.stopRequested) r.endReason = "stopped";
            else r.endReason = "stopped_outside_mcp";
            s.last = r;
            s.current = SessionRecord();
            releaseHeldInput(ctx);
        }
    }

    applyHold(ctx);
    finishStartWaits(ctx);
}

bool monitorNeeded(McpContext& ctx) {
    PlayToolsState& s = state();
    Project* project = ctx.project();
    return (project && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) || s.current.active ||
           !s.startWaits.empty() || !s.held.empty();
}

// A main-thread task re-posting itself every frame while there is something to watch
void ensureMonitor(McpContext& ctx) {
    PlayToolsState& s = state();
    if (s.monitorScheduled) return;
    s.monitorScheduled = true;
    McpContext* context = &ctx;
    struct Tick {
        static void run(McpContext* c) {
            observeSession(*c);
            if (monitorNeeded(*c)) {
                c->app().enqueueMainThreadTask([c]() { McpWriteScope writeScope; run(c); });
            } else {
                state().monitorScheduled = false;
            }
        }
    };
    ctx.app().enqueueMainThreadTask([context]() { McpWriteScope writeScope; Tick::run(context); });
}

std::string playStateName(Project* project) {
    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId == NULL_PROJECT_SCENE) return "stopped";
    SceneProject* sp = project->getScene(playId);
    return sp ? McpRefs::playStateName(sp->playState) : "stopped";
}

std::string sessionSummaryText(const SessionRecord& r) {
    std::string text = "\"" + r.sceneName + "\" ";
    if (r.endReason == "crashed") text += "crashed after " + std::to_string((int)std::round(r.durationSeconds)) + " s: " + r.crash;
    else if (r.endReason == "failed_to_start") text += "failed to start";
    else if (r.endReason == "cancelled") text += "was stopped while starting";
    else if (r.endReason == "stopped") text += "played " + std::to_string((int)std::round(r.durationSeconds)) + " s and was stopped";
    else text += "stopped (by the editor UI or the game) after " + std::to_string((int)std::round(r.durationSeconds)) + " s";
    text += " (" + std::to_string(r.errors) + " error(s), " + std::to_string(r.warnings) + " warning(s) logged).";
    return text;
}

// ================================================================== runtime scenes / entities

std::vector<SceneProject*> runningScenes(Project* project) {
    std::vector<SceneProject*> out;
    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId == NULL_PROJECT_SCENE) return out;
    SceneProject* main = project->getScene(playId);
    if (!main || !isRunningState(main->playState)) return out;
    out.push_back(main);
    for (SceneProject* layer : project->getRunningRuntimeLayerScenes(playId)) {
        if (layer && layer != main && std::find(out.begin(), out.end(), layer) == out.end()) out.push_back(layer);
    }
    return out;
}

Json runningScenesJson(Project* project) {
    Json list = Json::array();
    for (SceneProject* sp : runningScenes(project)) {
        Json item = {{"id", sp->id}, {"name", sp->name}};
        const uint32_t source = project->getPlaySourceSceneId(sp->id);
        if (source != sp->id) item["source_scene_id"] = source;
        item["main"] = sp->id == project->getPlaySessionSceneId();
        // DontDestroyOnLoad's scene: survives scene loads
        if (project->isPersistentPlayScene(sp->id)) item["persistent"] = true;
        list.push_back(item);
    }
    return list;
}

// ================================================================== input

// The control names are InputControlNames' (engine/core/input/InputControlPath.cpp), shared with
// InputActions' control paths and get_input_state
int parseKeyName(const Json& value) {
    if (value.is_number_integer()) return value.get<int>();
    if (!value.is_string()) return D_KEY_UNKNOWN;
    return InputControlNames::parseKey(value.get<std::string>());
}

std::string keyName(int key) {
    return InputControlNames::keyName(key);
}

int parseMouseButton(const Json& value) {
    if (value.is_null()) return D_MOUSE_BUTTON_LEFT;
    if (value.is_number_integer()) return std::clamp(value.get<int>(), 0, D_MOUSE_BUTTON_LAST);
    const std::string name = value.is_string() ? value.get<std::string>() : std::string();
    if (name.empty()) return D_MOUSE_BUTTON_LEFT;
    return InputControlNames::parseMouseButton(name);
}

int parseGamepadButton(const Json& value) {
    if (value.is_number_integer()) return std::clamp(value.get<int>(), 0, D_GAMEPAD_BUTTON_LAST);
    if (!value.is_string()) return -1;
    return InputControlNames::parseGamepadButton(value.get<std::string>());
}

int parseGamepadAxis(const Json& value) {
    if (value.is_number_integer()) return std::clamp(value.get<int>(), 0, D_GAMEPAD_AXIS_LAST);
    if (!value.is_string()) return -1;
    return InputControlNames::parseGamepadAxis(value.get<std::string>());
}

int parseMods(const Json& value) {
    int mods = 0;
    if (!value.is_array()) return 0;
    for (const Json& item : value) {
        const std::string name = item.is_string() ? lowerCopy(item.get<std::string>()) : std::string();
        if (name == "shift") mods |= D_MODIFIER_SHIFT;
        else if (name == "ctrl" || name == "control") mods |= D_MODIFIER_CONTROL;
        else if (name == "alt") mods |= D_MODIFIER_ALT;
        else if (name == "super" || name == "cmd" || name == "meta") mods |= D_MODIFIER_SUPER;
    }
    return mods;
}

// Engine view (framebuffer) pixels of the Game view → the system* entry points
Vector2 clampToView(Vector2 point) {
    const Rect view = Engine::getViewRect();
    return Vector2(std::clamp(point.x, view.getX(), view.getX() + std::max(0.0f, view.getWidth())),
                   std::clamp(point.y, view.getY(), view.getY() + std::max(0.0f, view.getHeight())));
}

// the owner of what send_input's action / axis / axis_delta events give the input actions (a
// virtual source, as an on-screen control; input_action forces an action with InputActions::inject)
uintptr_t mcpInputSource() {
    return InputActions::namedSource("mcp");
}

void releaseHeldInput(McpContext& ctx) {
    HeldInput& held = state().held;
    for (int key : held.keys) Engine::systemKeyUp(key, false, 0);
    const Vector2 pos = clampToView(held.mousePos);
    for (int button : held.mouseButtons) Engine::systemMouseUp(button, pos.x, pos.y, 0);
    for (const auto& [pointer, point] : held.touches) {
        const Vector2 p = clampToView(point);
        Engine::systemTouchEnd(pointer, p.x, p.y);
    }
    for (const auto& [pad, buttons] : held.gamepadButtons) {
        for (int button : buttons) Engine::systemGamepadButtonUp(pad, button);
    }
    for (const auto& [pad, axes] : held.gamepadAxes) {
        for (int axis : axes) Engine::systemGamepadAxisMove(pad, axis, 0.0f);
    }
    for (int pad : held.gamepads) Engine::systemGamepadDisconnect(pad);
    for (const std::string& action : held.actions) InputActions::releaseInjected(action);
    // the Input Manager's actions and axes it held (send_input action / axis / axis_delta)
    InputActions::clearSource(mcpInputSource());
    held = HeldInput();
    if (PlayInput* input = ctx.playInput()) input->setPointerInjected(false);
}

Json heldInputJson() {
    const HeldInput& held = state().held;
    Json keys = Json::array();
    for (int key : held.keys) keys.push_back(keyName(key));
    Json buttons = Json::array();
    for (int button : held.mouseButtons) buttons.push_back(InputControlNames::mouseButtonName(button));
    Json touches = Json::array();
    for (const auto& [pointer, point] : held.touches) touches.push_back(pointer);
    Json pads = Json::array();
    for (int pad : held.gamepads) {
        Json item = {{"gamepad", pad}, {"buttons", Json::array()}};
        auto it = held.gamepadButtons.find(pad);
        if (it != held.gamepadButtons.end()) for (int b : it->second) item["buttons"].push_back(b);
        pads.push_back(item);
    }
    Json actions = Json::array();
    for (const std::string& action : held.actions) actions.push_back(action);
    Json out = {{"keys", keys}, {"mouse_buttons", buttons}, {"touches", touches}, {"gamepads", pads}, {"input_actions", actions}};
    if (!held.managerActions.empty()) out["actions"] = held.managerActions;
    if (!held.axes.empty()) out["axes"] = held.axes;
    return out;
}

// What the game's input reads now (get_play_state while playing): the Input Manager's view of
// the actions that are not at rest (a value, or "pressed" for a button action), the touch count,
// the keys held
Json playInputJson() {
    Json actions = Json::object();
    for (const std::string& name : InputActions::getActionNames()) {
        InputActions::State st;
        const InputAction* action = InputActions::find(name);
        if (!action || !InputActions::getState(name, st) || !st.enabled) continue;
        if (action->getType() == InputActionType::BUTTON) {
            if (st.pressed) actions[name] = "pressed";
        } else if (std::fabs(st.value) >= 0.0005f) {
            actions[name] = std::round(st.value * 1000.0f) / 1000.0f;
        }
    }
    Json keys = Json::array();
    for (int key = 0; key <= D_KEY_LAST && keys.size() < 16; key++) {
        if (Input::isKeyPressed(key)) keys.push_back(keyName(key));
    }
    return {{"actions", actions}, {"touches", Input::getTouchCount()}, {"keys_held", keys}};
}

// What the game's audio does now (get_play_state while playing): the voices, and the mixer
// groups that are audible (above -60 dB)
Json playAudioJson() {
    Json meters = Json::object();
    for (const std::string& group : AudioMixer::getGroupNames()) {
        const float db = AudioMixer::getMeterDb(group);
        if (db > -60.0f) meters[group] = std::round(db * 10.0f) / 10.0f;
    }
    Json out = {{"voices", AudioSystem::getVoiceCount()}, {"mixer_meters", meters}};
    if (!AudioSystem::isOutputAvailable()) out["output"] = "none (mixing without a device)";
    if (AudioSystem::isListenerPaused()) out["listener_paused"] = true;
    // the music system (S39 R20)
    const MusicPlayer::Status music = MusicPlayer::status();
    if (music.playing) {
        out["music"] = {{"track", music.track}, {"section", music.section}, {"time_s", std::round(music.timeSeconds * 1000.0) / 1000.0},
                        {"bar", music.bar}, {"beat", music.beat}, {"bpm", music.bpm}};
    }
    // from the first read until Stop: a first read's value is null, the next ones measure
    LoudnessMonitor::get().requestByAgent();
    const LoudnessMonitor::Reading reading = LoudnessMonitor::get().reading();
    out["loudness"] = {{"momentary_lufs", reading.momentary ? Json(std::round(*reading.momentary * 100.0) / 100.0) : Json(nullptr)},
                       {"true_peak_dbtp", std::round(reading.truePeakDb * 100.0) / 100.0}};
    return out;
}

} // namespace

// ================================================================== shared (w3)

namespace w3 {

SceneProject* gameViewScene(Project* project) {
    if (!project) return nullptr;
    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId != NULL_PROJECT_SCENE) return project->getScene(playId);
    return project->getScene(project->getSceneBehindPrefabStage(project->getSelectedSceneId()));
}

// What play start runs without a scene: the project's start scene when it is open (not a
// selected child/HUD scene), else the selected scene
SceneProject* defaultPlayScene(Project* project) {
    SceneProject* start = project ? project->getScene(project->getStartSceneId()) : nullptr;
    return start && start->scene && start->opened ? start : gameViewScene(project);
}

// The game's HTTP requests (Http): counts of this Play, the simulated network, the last failures
static Json httpJson() {
    const Http::Stats stats = Http::getStats();
    Json failures = Json::array();
    for (const HttpResponse& failure : stats.lastFailures) {
        failures.push_back({{"method", failure.method}, {"url", failure.url}, {"status", failure.status},
                            {"error_code", failure.errorCode}, {"error", failure.error}});
    }
    return {{"active", stats.active}, {"queued", stats.queued}, {"finished", stats.finished}, {"failed", stats.failed},
            {"cancelled", stats.cancelled}, {"offline", Http::isOfflineSimulated()}, {"last_failures", failures}};
}

// The platform side of the running game: lifecycle, frame pacing, orientation, the simulated
// device and language, and what the game asked the system for (URLs, vibrations)
Json platformJson() {
    System& system = System::instance();
    Json urls = Json::array();
    for (const OpenedUrl& url : Platform::getOpenedUrls()) {
        urls.push_back({{"url", url.url}, {"launched", url.launched},
                        {"seconds_ago", std::round((Engine::getSystemTime() - url.time) * 10.0) / 10.0}});
    }
    const PlatformSimulation& simulation = Platform::getSimulation();
    Json out = {
        {"focused", Engine::isFocused()},
        {"paused", Engine::isPaused()},
        {"orientation", PlatformRules::orientationName(Screen::getOrientation())},
        {"requested_orientation", PlatformRules::orientationName(Screen::getRequestedOrientation())},
        {"target_frame_rate", Engine::getTargetFrameRate()},
        {"vsync_count", Engine::getVSyncCount()},
        {"language", system.getLocale()},
        {"system_language", LocaleRules::systemLanguageName(system.getSystemLanguage())},
        {"device", simulation.device ? Json(simulation.device->id) : Json()},
        {"opened_urls", urls},
        {"vibrations", Platform::getVibrationCount()},
        {"haptics", Platform::getHapticCount()}
    };
    if (Platform::getOpenedUrlCount() > (int)urls.size()) out["opened_url_count"] = Platform::getOpenedUrlCount();
    return out;
}

// Screen.safeArea as the game sees it (canvas units; stopped: for the project canvas, as the
// Scene view lays UI out), the insets with extras, the extras, and the simulated device
Json safeAreaJson() {
    auto r = [](float v) { return std::round(v * 100.0f) / 100.0f; };
    const Rect rect = Screen::getSafeArea();
    const EdgeInsets insets = Screen::getSafeAreaInsets();
    const EdgeInsets extra = Screen::getExtraInsets();
    const PlatformSimulation& simulation = Platform::getSimulation();
    return {{"rect", {r(rect.getX()), r(rect.getY()), r(rect.getWidth()), r(rect.getHeight())}},
            {"insets", {r(insets.left), r(insets.top), r(insets.right), r(insets.bottom)}},
            {"extra", {r(extra.left), r(extra.top), r(extra.right), r(extra.bottom)}},
            {"device", simulation.device ? Json(simulation.device->id) : Json()}};
}

Json playStateJson(McpContext& ctx) {
    Project* project = ctx.project();
    PlayToolsState& s = state();
    Json out = Json::object();
    if (!project) {
        out["state"] = "no_project";
        return out;
    }
    const uint32_t playId = project->getPlaySessionSceneId();
    out["state"] = playStateName(project);
    if (playId != NULL_PROJECT_SCENE) {
        if (SceneProject* sp = project->getScene(playId)) out["scene"] = McpRefs::sceneJson(project, *sp);
        out["running_scenes"] = runningScenesJson(project);
        if (s.current.active) out["session"] = sessionJson(s.current);
    } else if (SceneProject* sp = defaultPlayScene(project)) {
        out["play_would_start"] = {{"id", sp->id}, {"name", sp->name}};
    }
    out["time"] = timeJson(playId != NULL_PROJECT_SCENE && s.current.active);
    if (playId != NULL_PROJECT_SCENE) {
        out["platform"] = platformJson();
        out["http"] = httpJson();
    }
    out["safe_area"] = safeAreaJson();
    if (s.last.serial != 0) out["last_session"] = sessionJson(s.last);
    // Play Mode Changes waiting after a Stop (get_play_changes)
    size_t pending = 0;
    for (uint32_t sceneId : PlayChangeReview::get().pendingScenes()) pending += PlayChangeReview::get().pendingCount(sceneId);
    out["pending_play_changes"] = pending;
    if (!s.held.empty()) out["held_input"] = heldInputJson();
    // what the game's input reads (the Input Manager's actions, touches, keys)
    if (playId != NULL_PROJECT_SCENE && s.current.active) {
        out["input"] = playInputJson();
        // the on-device console and stats overlay (Project Settings > Debug, DebugOverlay)
        static const char* const statsModes[] = {"off", "fps", "full"};
        const int statsMode = static_cast<int>(DebugOverlay::getStatsMode());
        out["debug_overlay"] = {{"enabled", DebugOverlay::isEnabled()}, {"console_visible", DebugOverlay::isConsoleVisible()},
                                {"stats", statsMode >= 0 && statsMode < 3 ? statsModes[statsMode] : "off"}};
    }
    if (playId != NULL_PROJECT_SCENE && s.current.active) out["audio"] = playAudioJson();
    // script coroutines and pending timers of the game (get_runtime_entity lists them per script)
    out["coroutines"] = ScriptScheduler::coroutineCount();
    out["timers"] = ScriptScheduler::timerCount();
    return out;
}

SceneProject* resolveRuntimeScene(Project* project, const Json& ref, std::string& error) {
    if (!project) {
        error = "No project is open.";
        return nullptr;
    }
    const uint32_t playId = project->getPlaySessionSceneId();
    SceneProject* main = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
    if (!main) {
        error = "Nothing is playing. Start the game with play {action: \"start\"} first.";
        return nullptr;
    }
    if (!isRunningState(main->playState)) {
        error = "Play mode is " + McpRefs::playStateName(main->playState) + "; wait until it is playing (play start waits).";
        return nullptr;
    }
    std::vector<SceneProject*> running = runningScenes(project);
    // after a scene load replaced the played scene (another scene, or the same one loaded
    // again: a fresh copy), the default is the main scene the game shows now
    SceneProject* shown = main;
    if (project->isPlayedSceneReplaced(main->id)) {
        Scene* loadedMain = SceneManager::getScenePtr(SceneManager::getCurrentSceneId());
        for (SceneProject* sp : running) {
            if (sp != main && sp->scene == loadedMain) shown = sp;
        }
    }
    if (ref.is_null()) return shown;

    std::string text = ref.is_string() ? ref.get<std::string>() : ref.dump();
    // a Play copy id or a running scene's name
    for (SceneProject* sp : running) {
        if (std::to_string(sp->id) == text || sp->name == text) return sp;
    }
    std::string refError;
    SceneProject* source = McpRefs::resolveScene(project, ref, refError);
    if (source) {
        if (source == main) return shown;
        const uint32_t runtimeId = project->getPlayRuntimeSceneId(source->id);
        if (runtimeId != NULL_PROJECT_SCENE) {
            if (SceneProject* runtime = project->getScene(runtimeId)) return runtime;
        }
    }
    error = "Scene " + text + " is not running. Running scenes:";
    for (SceneProject* sp : running) error += " " + std::to_string(sp->id) + " \"" + sp->name + "\"";
    error += ".";
    return nullptr;
}

std::vector<Entity> liveEntities(Project* project, SceneProject* scene) {
    std::vector<Entity> entities = scene->entities;
    if (project && scene->scene && isRunningState(scene->playState)) {
        for (Entity entity : project->getPlayCreatedEntities(scene)) {
            if (std::find(entities.begin(), entities.end(), entity) == entities.end()) entities.push_back(entity);
        }
    }
    return entities;
}

Entity resolveLiveEntity(Project* project, SceneProject* scene, const Json& ref, std::string& error) {
    if (!scene || !scene->scene) {
        error = "The scene is not loaded.";
        return NULL_ENTITY;
    }
    if (!isRunningState(scene->playState)) return McpRefs::resolveEntity(scene, ref, error);

    const std::vector<Entity> entities = liveEntities(project, scene);
    const std::string label = "scene " + std::to_string(scene->id) + " \"" + scene->name + "\"";
    std::string text;
    if (ref.is_number_integer()) text = std::to_string(ref.get<long long>());
    else if (ref.is_string()) text = ref.get<std::string>();
    else {
        error = "entity must be a GameObject id (integer) or a name path like \"Player/Gun\".";
        return NULL_ENTITY;
    }
    if (!text.empty() && text.size() < 11 && std::all_of(text.begin(), text.end(), ::isdigit)) {
        const Entity entity = static_cast<Entity>(std::stoul(text));
        if (std::find(entities.begin(), entities.end(), entity) != entities.end() && scene->scene->isEntityCreated(entity)) {
            return entity;
        }
        if (!ref.is_string()) {
            error = "No GameObject with id " + text + " in the running " + label + ". List them with list_runtime_entities.";
            return NULL_ENTITY;
        }
    }
    while (!text.empty() && text.front() == '/') text.erase(0, 1);
    const std::vector<Entity> matches = EntityPath::resolve(scene->scene, entities, text);
    if (matches.size() == 1) return matches[0];
    if (matches.size() > 1) {
        EntityPath::Cache cache;
        error = "\"" + text + "\" matches " + std::to_string(matches.size()) + " GameObjects in the running " + label + ":";
        for (size_t i = 0; i < matches.size() && i < 20; i++) {
            error += " " + std::to_string(matches[i]) + " (" + EntityPath::of(scene->scene, matches[i], &cache) + ")";
        }
        error += ". Pass an id or a full path.";
        return NULL_ENTITY;
    }
    error = "No GameObject \"" + text + "\" in the running " + label + ". List them with list_runtime_entities.";
    return NULL_ENTITY;
}

} // namespace w3

namespace {

// ================================================================== Lua evaluation

struct LuaEvalContext {
    Project* project = nullptr;
    SceneProject* scene = nullptr;
    std::vector<std::string>* printed = nullptr;
};

struct LuaSerializer {
    lua_State* L;
    Project* project;
    int maxDepth = 4;
    size_t maxItems = 200;
    size_t maxString = 4000;
    bool iteratePairs = false;   // userdata with __pairs (data-model proxies) as tables
    std::unordered_set<const void*> visiting;

    std::string sceneNameOf(Scene* scene) const {
        if (!project || !scene) return {};
        for (SceneProject& sp : project->getScenes()) if (sp.scene == scene) return sp.name;
        for (SceneProject* sp : runningScenes(project)) if (sp->scene == scene) return sp->name;
        return {};
    }

    SceneProject* sceneProjectOf(Scene* scene) const {
        if (!project || !scene) return nullptr;
        for (SceneProject* sp : runningScenes(project)) if (sp->scene == scene) return sp;
        for (SceneProject* sp : project->getScenesAndPrefabStages()) if (sp->scene == scene) return sp;
        return nullptr;
    }

    std::string luabridgeTypeName(int index) {
        std::string name;
        if (lua_getmetatable(L, index)) {
            lua_rawgetp(L, -1, luabridge::detail::getTypeKey());
            if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
            lua_pop(L, 2);
        }
        return name;
    }

    Json toJson(int index, int depth) {
        index = lua_absindex(L, index);
        switch (lua_type(L, index)) {
            case LUA_TNIL:
            case LUA_TNONE:
                return nullptr;
            case LUA_TBOOLEAN:
                return lua_toboolean(L, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index)) return static_cast<long long>(lua_tointeger(L, index));
                {
                    const double value = lua_tonumber(L, index);
                    if (!std::isfinite(value)) return std::isnan(value) ? "nan" : (value > 0 ? "inf" : "-inf");
                    return value;
                }
            case LUA_TSTRING: {
                size_t len = 0;
                const char* text = lua_tolstring(L, index, &len);
                std::string value(text, len);
                if (value.size() > maxString) value = value.substr(0, maxString) + "...";
                return value;
            }
            case LUA_TTABLE:
                return tableToJson(index, depth);
            case LUA_TFUNCTION:
                return "<function>";
            case LUA_TUSERDATA:
                return userdataToJson(index, depth);
            default: {
                std::string text = luaL_tolstring(L, index, nullptr);
                lua_pop(L, 1);
                return text;
            }
        }
    }

    Json userdataToJson(int index, int depth) {
        if (luabridge::Stack<Vector3>::isInstance(L, index)) {
            if (auto v = luabridge::Stack<Vector3>::get(L, index)) return McpValues::toJson(*v);
        }
        if (luabridge::Stack<Vector2>::isInstance(L, index)) {
            if (auto v = luabridge::Stack<Vector2>::get(L, index)) return McpValues::toJson(*v);
        }
        if (luabridge::Stack<Vector4>::isInstance(L, index)) {
            if (auto v = luabridge::Stack<Vector4>::get(L, index)) return McpValues::toJson(*v);
        }
        if (luabridge::Stack<Quaternion>::isInstance(L, index)) {
            if (auto v = luabridge::Stack<Quaternion>::get(L, index)) return McpValues::toJson(*v);
        }
        if (luabridge::Stack<Rect>::isInstance(L, index)) {
            if (auto v = luabridge::Stack<Rect>::get(L, index)) {
                const Rect r = *v;
                return Json{{"x", r.getX()}, {"y", r.getY()}, {"width", r.getWidth()}, {"height", r.getHeight()}};
            }
        }
        const std::string typeName = luabridgeTypeName(index);
        if (luabridge::Stack<EntityHandle*>::isInstance(L, index)) {
            auto handle = luabridge::Stack<EntityHandle*>::get(L, index);
            if (handle && *handle) {
                EntityHandle* h = *handle;
                Json out = {{"type", typeName.empty() ? "EntityHandle" : typeName}, {"entity", h->getEntity()}};
                // Only scenes the editor still has are dereferenced: a handle kept in a Lua
                // global from an earlier Play can point at a Play copy deleted since
                SceneProject* sp = sceneProjectOf(h->getScene());
                if (!sp) {
                    out["stale"] = true;
                } else if (sp->scene->isEntityCreated(h->getEntity())) {
                    out["name"] = sp->scene->getEntityName(h->getEntity());
                    out["path"] = EntityPath::of(sp->scene, h->getEntity());
                    out["scene"] = sp->name;
                } else {
                    out["destroyed"] = true;
                }
                return out;
            }
        }
        if (luabridge::Stack<Scene*>::isInstance(L, index)) {
            auto scene = luabridge::Stack<Scene*>::get(L, index);
            if (scene && *scene) return Json{{"type", "Scene"}, {"name", sceneNameOf(*scene)}};
        }
        if (iteratePairs && typeName.empty() && luaL_getmetafield(L, index, "__pairs") != LUA_TNIL) {
            lua_pop(L, 1);
            return pairsToJson(index, depth);
        }
        std::string text = luaL_tolstring(L, index, nullptr);
        lua_pop(L, 1);
        return text;
    }

    // A userdata walked through its __pairs (next, state, control), as `pairs` does: an array when
    // its keys are 1..n in order, else an object
    Json pairsToJson(int index, int depth) {
        const void* ptr = lua_topointer(L, index);
        if (visiting.count(ptr)) return "<cycle>";
        const int top = lua_gettop(L);
        luaL_getmetafield(L, index, "__pairs");
        lua_pushvalue(L, index);
        if (lua_pcall(L, 1, 3, 0) != LUA_OK) {
            lua_settop(L, top);
            std::string text = luaL_tolstring(L, index, nullptr);
            lua_pop(L, 1);
            return text;
        }
        visiting.insert(ptr);
        Json array = Json::array();
        Json object = Json::object();
        bool sequence = true;
        size_t count = 0;
        bool more = false;
        while (true) {
            lua_pushvalue(L, top + 1);
            lua_pushvalue(L, top + 2);
            lua_pushvalue(L, top + 3);
            if (lua_pcall(L, 2, 2, 0) != LUA_OK || lua_isnil(L, -2)) {
                lua_settop(L, top + 3);
                break;
            }
            if (count >= maxItems) {
                more = true;
                lua_settop(L, top + 3);
                break;
            }
            std::string key;
            if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
                const lua_Integer k = lua_tointeger(L, -2);
                if (k != static_cast<lua_Integer>(count) + 1) sequence = false;
                key = std::to_string(k);
            } else {
                sequence = false;
                lua_pushvalue(L, -2);
                key = luaL_tolstring(L, -1, nullptr);
                lua_pop(L, 2);
            }
            Json value = depth + 1 >= maxDepth && (lua_type(L, -1) == LUA_TTABLE || lua_type(L, -1) == LUA_TUSERDATA)
                             ? Json("<nested>") : toJson(-1, depth + 1);
            array.push_back(value);
            object[key] = std::move(value);
            count++;
            lua_pop(L, 1);                 // the value: the key becomes the next control value
            lua_replace(L, top + 3);
        }
        lua_settop(L, top);
        visiting.erase(ptr);
        Json out = sequence && count > 0 ? array : object;
        if (more) {
            if (out.is_array()) out.push_back("... more");
            else out["..."] = "more";
        }
        return out;
    }

    Json tableToJson(int index, int depth) {
        const void* ptr = lua_topointer(L, index);
        if (visiting.count(ptr)) return "<cycle>";
        if (depth >= maxDepth) {
            size_t count = 0;
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                count++;
                lua_pop(L, 1);
            }
            return "<table with " + std::to_string(count) + " entries>";
        }
        visiting.insert(ptr);

        // A sequence 1..n is an array
        const lua_Unsigned length = lua_rawlen(L, index);
        size_t count = 0;
        bool sequence = length > 0;
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            count++;
            if (sequence) {
                if (!lua_isinteger(L, -2)) sequence = false;
                else {
                    const lua_Integer key = lua_tointeger(L, -2);
                    if (key < 1 || static_cast<lua_Unsigned>(key) > length) sequence = false;
                }
            }
            lua_pop(L, 1);
        }
        if (sequence && count != length) sequence = false;

        Json out;
        if (sequence) {
            out = Json::array();
            for (lua_Unsigned i = 1; i <= length; i++) {
                if (out.size() >= maxItems) {
                    out.push_back("... " + std::to_string(length - out.size()) + " more");
                    break;
                }
                lua_rawgeti(L, index, static_cast<lua_Integer>(i));
                out.push_back(toJson(-1, depth + 1));
                lua_pop(L, 1);
            }
        } else {
            out = Json::object();
            size_t added = 0;
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                if (added >= maxItems) {
                    lua_pop(L, 2);
                    out["..."] = std::to_string(count - added) + " more";
                    break;
                }
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    key = lua_tostring(L, -2);
                } else {
                    lua_pushvalue(L, -2);
                    key = luaL_tolstring(L, -1, nullptr);
                    lua_pop(L, 2);
                }
                // the scene handle every script instance carries says nothing useful
                if (key == "__scriptRef" || (key == "scene" && lua_type(L, -1) == LUA_TUSERDATA && depth > 0)) {
                    lua_pop(L, 1);
                    continue;
                }
                out[key] = toJson(-1, depth + 1);
                added++;
                lua_pop(L, 1);
            }
        }
        visiting.erase(ptr);
        return out;
    }
};

} // namespace

namespace w3 {

Json luaToJson(Project* project, lua_State* L, int index, int maxDepth, size_t maxItems) {
    LuaSerializer serializer{L, project, maxDepth, maxItems};
    serializer.iteratePairs = true;
    return serializer.toJson(index, 0);
}

} // namespace w3

namespace {

// The call's context, through a box (a full userdata) the call empties when it returns: a
// function eval_lua defined can outlive it (a coroutine it started, a callback), and then has none
LuaEvalContext* evalContext(lua_State* L) {
    LuaEvalContext** box = static_cast<LuaEvalContext**>(lua_touserdata(L, lua_upvalueindex(1)));
    return box ? *box : nullptr;
}

// Lua is built as C: lua_error longjmps past C++ destructors, so every helper below raises
// errors only once its C++ objects are gone (the message is left on the Lua stack)

int luaEvalPrint(lua_State* L) {
    LuaEvalContext* ctx = evalContext(L);
    if (!ctx) {
        // the eval_lua call ended (a coroutine or callback it started prints): the game's print
        lua_getglobal(L, "print");
        lua_insert(L, 1);
        lua_call(L, lua_gettop(L) - 1, 0);
        return 0;
    }
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) lua_pushliteral(L, "\t");
        luaL_tolstring(L, i, nullptr);
    }
    if (n == 0) lua_pushliteral(L, "");
    else lua_concat(L, 2 * n - 1);
    size_t len = 0;
    const char* text = lua_tolstring(L, -1, &len);
    if (ctx->printed && ctx->printed->size() < 500) ctx->printed->push_back(std::string(text, len));
    return 0;
}

// Reads a ref argument (integer or string); false with an error message pushed
bool helperRef(lua_State* L, int argIndex, Json& out) {
    if (lua_isinteger(L, argIndex)) {
        out = static_cast<long long>(lua_tointeger(L, argIndex));
        return true;
    }
    if (lua_type(L, argIndex) == LUA_TSTRING) {
        out = std::string(lua_tostring(L, argIndex));
        return true;
    }
    lua_pushfstring(L, "argument %d must be a GameObject id or path (or scene name)", argIndex);
    return false;
}

// The scene a helper works on: its optional scene argument, else the eval's scene
bool helperScene(lua_State* L, int argIndex, SceneProject*& out) {
    LuaEvalContext* ctx = evalContext(L);
    if (!ctx) {
        lua_pushliteral(L, "eval_lua's entity/script/object/getScene helpers work only during the eval_lua call "
                           "(this runs later, in a coroutine or callback it started): keep the values in locals");
        return false;
    }
    out = ctx->scene;
    if (lua_gettop(L) < argIndex || lua_isnil(L, argIndex)) return true;
    Json ref;
    if (!helperRef(L, argIndex, ref)) return false;
    std::string error;
    if (ctx->project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) out = w3::resolveRuntimeScene(ctx->project, ref, error);
    else out = McpRefs::resolveScene(ctx->project, ref, error);
    if (!out) {
        lua_pushstring(L, error.c_str());
        return false;
    }
    return true;
}

bool helperEntity(lua_State* L, SceneProject* sp, int argIndex, Entity& out) {
    LuaEvalContext* ctx = evalContext(L);
    Json ref;
    if (!helperRef(L, argIndex, ref)) return false;
    std::string error;
    out = w3::resolveLiveEntity(ctx->project, sp, ref, error);
    if (out == NULL_ENTITY) {
        lua_pushstring(L, error.c_str());
        return false;
    }
    return true;
}

// entity(ref [, scene]) → GameObject id
int luaEvalEntity(lua_State* L) {
    SceneProject* sp = nullptr;
    Entity entity = NULL_ENTITY;
    if (!helperScene(L, 2, sp) || !helperEntity(L, sp, 1, entity)) return lua_error(L);
    lua_pushinteger(L, static_cast<lua_Integer>(entity));
    return 1;
}

// Pushes the instance, or an error message; true when found
bool pushScriptInstance(lua_State* L, SceneProject* sp, Entity entity, const char* className) {
    const std::string wanted = className ? className : "";
    std::string available;
    if (ScriptComponent* scripts = sp->scene->findComponent<ScriptComponent>(entity)) {
        for (ScriptEntry& entry : scripts->scripts) {
            if (entry.type != ScriptType::LUA) continue;
            available += " " + entry.className;
            if (!entry.instance || (!wanted.empty() && entry.className != wanted)) continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<lua_Integer>(reinterpret_cast<intptr_t>(entry.instance)));
            return true;
        }
    }
    const std::string message = available.empty()
        ? std::string("That GameObject has no Lua script.")
        : "No running Lua script" + (wanted.empty() ? std::string() : " " + wanted) + " on that GameObject (it has:" +
          available + "; script instances exist only while playing).";
    lua_pushstring(L, message.c_str());
    return false;
}

// script(ref [, className [, scene]]) → the Lua script instance (self) of that GameObject
int luaEvalScript(lua_State* L) {
    SceneProject* sp = nullptr;
    Entity entity = NULL_ENTITY;
    if (!helperScene(L, 3, sp) || !helperEntity(L, sp, 1, entity)) return lua_error(L);
    const char* className = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (!pushScriptInstance(L, sp, entity, className)) return lua_error(L);
    return 1;
}

bool pushObjectHandle(lua_State* L, SceneProject* sp, Entity entity, const char* typeName) {
    const std::string type = typeName ? typeName : "GameObject";
    const int top = lua_gettop(L);
    if (LuaBinding::pushScriptObject(L, sp->scene, entity, type)) return true;
    lua_settop(L, top);
    const std::string message = "Can't wrap that GameObject as " + type +
                                " (types: GameObject, Sprite, Text, Image, Camera, Mesh, Model, AudioSource, ...).";
    lua_pushstring(L, message.c_str());
    return false;
}

// object(ref [, type [, scene]]) → an engine handle (GameObject, Sprite, Text, ...)
int luaEvalObject(lua_State* L) {
    SceneProject* sp = nullptr;
    Entity entity = NULL_ENTITY;
    if (!helperScene(L, 3, sp) || !helperEntity(L, sp, 1, entity)) return lua_error(L);
    const char* typeName = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (!pushObjectHandle(L, sp, entity, typeName)) return lua_error(L);
    return 1;
}

// getScene(name) → a Scene of the running game
int luaEvalGetScene(lua_State* L) {
    SceneProject* sp = nullptr;
    if (!helperScene(L, 1, sp)) return lua_error(L);
    if (!sp || !sp->scene || !luabridge::push<Scene*>(L, sp->scene)) lua_pushnil(L);
    return 1;
}

thread_local Clock::time_point evalDeadline;
thread_local double evalTimeoutSeconds = 5.0;

void evalTimeoutHook(lua_State* L, lua_Debug*) {
    if (Clock::now() > evalDeadline) {
        char message[96];
        snprintf(message, sizeof(message), "eval_lua: timed out after %.1f s (infinite loop?)", evalTimeoutSeconds);
        luaL_error(L, "%s", message);
    }
}

struct LuaEvalResult {
    bool ok = false;
    std::string error;
    Json values = Json::array();
    std::vector<std::string> printed;
};

LuaEvalResult runLua(Project* project, SceneProject* scene, const std::string& code, double timeoutSeconds,
                     int maxDepth, size_t maxItems) {
    LuaEvalResult result;
    lua_State* L = LuaBinding::getLuaState();
    if (!L) {
        result.error = "The Lua state does not exist.";
        return result;
    }
    const int top = lua_gettop(L);

    // An expression first (like a REPL), then a chunk
    const std::string expression = "return " + code;
    int status = luaL_loadbufferx(L, expression.c_str(), expression.size(), "=eval_lua", "t");
    if (status != LUA_OK) {
        lua_pop(L, 1);
        status = luaL_loadbufferx(L, code.c_str(), code.size(), "=eval_lua", "t");
        if (status != LUA_OK) {
            result.error = "Lua syntax error: " + LuaBinding::getLuaStackErrorString(L, -1);
            lua_settop(L, top);
            return result;
        }
    }
    const int chunk = lua_gettop(L);

    LuaEvalContext context;
    context.project = project;
    context.scene = scene;
    context.printed = &result.printed;

    // Environment: reads and global writes go to _G; the helpers live in the env itself. They
    // are set before the metatable: with __newindex in place they would land in _G, and the
    // game's own print would then write through this call's dead context.
    lua_newtable(L);
    if (scene && scene->scene && luabridge::push<Scene*>(L, scene->scene)) {
        lua_setfield(L, -2, "scene");
    }
    const std::pair<const char*, lua_CFunction> helpers[] = {
        {"print", luaEvalPrint}, {"entity", luaEvalEntity}, {"script", luaEvalScript},
        {"object", luaEvalObject}, {"getScene", luaEvalGetScene}
    };
    LuaEvalContext** contextBox = static_cast<LuaEvalContext**>(lua_newuserdatauv(L, sizeof(LuaEvalContext*), 0));
    *contextBox = &context;
    // anchored while the call runs: the box is emptied after it, whatever the collector did
    lua_pushvalue(L, -1);
    const int contextBoxRef = luaL_ref(L, LUA_REGISTRYINDEX);
    for (const auto& [name, fn] : helpers) {
        lua_pushvalue(L, -1);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -3, name);
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    if (!lua_setupvalue(L, chunk, 1)) lua_pop(L, 1);

    lua_Hook oldHook = lua_gethook(L);
    const int oldMask = lua_gethookmask(L);
    const int oldCount = lua_gethookcount(L);
    evalTimeoutSeconds = timeoutSeconds;
    evalDeadline = Clock::now() + std::chrono::milliseconds(static_cast<long long>(timeoutSeconds * 1000.0));
    lua_sethook(L, evalTimeoutHook, LUA_MASKCOUNT, 10000);

    status = LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET);

    lua_sethook(L, oldHook, oldMask, oldCount);
    *contextBox = nullptr;
    luaL_unref(L, LUA_REGISTRYINDEX, contextBoxRef);

    if (status != LUA_OK) {
        result.error = LuaBinding::getLuaStackErrorString(L, -1);
        lua_settop(L, top);
        return result;
    }
    LuaSerializer serializer{L, project, maxDepth, maxItems};
    const int results = lua_gettop(L) - (chunk - 1);
    for (int i = 0; i < results; i++) {
        result.values.push_back(serializer.toJson(chunk + i, 0));
    }
    lua_settop(L, top);
    result.ok = true;
    return result;
}

bool luaTruthy(const Json& value) {
    return !(value.is_null() || (value.is_boolean() && !value.get<bool>()));
}

// ================================================================== play start jobs

Json startResultJson(McpContext& ctx, uint64_t startLogId, double seconds) {
    Json data = w3::playStateJson(ctx);
    data["startup_seconds"] = round3(seconds);
    data["console_cursor_at_start"] = startLogId;
    data["problems"] = problemsSince(startLogId, 30);
    data["next_cursor"] = Out::lastLogId();
    return data;
}

void finishStartWaits(McpContext& ctx) {
    PlayToolsState& s = state();
    if (s.startWaits.empty()) return;
    Project* project = ctx.project();
    std::vector<PlayStartWait> keep;
    for (PlayStartWait& wait : s.startWaits) {
        if (wait.job->finished()) continue;
        const uint32_t playId = project->getPlaySessionSceneId();
        SceneProject* sp = project->getScene(wait.sceneId);
        const double seconds = secondsSince(wait.started);
        if (wait.job->cancelRequested() && !wait.cancelling) {
            wait.cancelling = true;
            s.current.stopRequested = true;
            if (playId == wait.sceneId && sp && sp->playState != ScenePlayState::CANCELLING) project->stop(wait.sceneId);
        }
        // auto_pause: once its first frame ran and it is held
        const bool settled = !s.time.autoPause || Engine::isSimulationHeld() || seconds > 30.0;
        if (playId == wait.sceneId && sp && isRunningState(sp->playState) && settled) {
            Json data = startResultJson(ctx, wait.startLogId, seconds);
            wait.job->succeed("Playing \"" + sp->name + "\".", data);
            continue;
        }
        if (playId != wait.sceneId) {
            Json data = startResultJson(ctx, wait.startLogId, seconds);
            if (wait.cancelling || (!s.current.active && s.last.endReason == "cancelled")) {
                wait.job->markCancelled("Play start cancelled: play was stopped while starting.");
                continue;
            }
            std::string message = "Play failed to start";
            const Json& problems = data["problems"];
            // the first compiler diagnostic says more than "Build failed"
            std::string firstError;
            for (const Json& entry : problems) {
                if (entry.value("type", "") == "build" && entry.value("message", "").find("error") != std::string::npos &&
                    entry.value("message", "").find(": ") != std::string::npos) {
                    firstError = entry.value("message", "");
                    break;
                }
            }
            for (const Json& entry : problems) {
                if (!firstError.empty()) break;
                if (entry.value("type", "") == "error") firstError = entry.value("message", "");
            }
            if (!firstError.empty()) message += ": " + firstError.substr(0, 300);
            wait.job->fail(message, data);
            continue;
        }
        if (sp) {
            wait.job->setProgress(-1.0, sp->playState == ScenePlayState::LOADING ? "Loading (building scripts if needed)..." :
                                         McpRefs::playStateName(sp->playState));
        }
        keep.push_back(wait);
    }
    s.startWaits.swap(keep);
}

// ================================================================== tool: play

// A console error from a game script that failed while Play started (a Lua init() that
// threw, a script that did not load, a C++ script crash). Other errors (no audio device...)
// don't make the start a failure.
bool isScriptStartupError(const Json& entry) {
    if (entry.value("type", "") != "error") return false;
    const std::string message = entry.value("message", "");
    static const char* const markers[] = {
        "Lua Error:", "Lua script file not found", "Failed to load Lua file", "Failed to execute Lua module",
        "did not return a table", "Script crash in scene"
    };
    for (const char* marker : markers) {
        if (message.find(marker) != std::string::npos) return true;
    }
    // LuaBinding: "Lua init() failed for 'Player': ..."
    return message.rfind("Lua ", 0) == 0 && message.find("() failed for") != std::string::npos;
}

ToolResult playStartResult(McpContext& ctx, const McpJob& job, bool screenshotLater) {
    (void)screenshotLater;
    Json data = job.result();
    if (!data.is_object()) data = Json::object();
    data["job_id"] = job.id();
    std::string text;
    if (const Project* project = ctx.project(); project && !project->getAudioCaptureError().empty()) {
        data["audio_capture_error"] = project->getAudioCaptureError();
    }
    const Json problems = data.value("problems", Json::array());
    Json scriptErrors = Json::array();
    for (const Json& entry : problems) {
        if (entry.is_object() && isScriptStartupError(entry)) scriptErrors.push_back(entry);
    }
    if (job.status() == JobStatus::Succeeded && !scriptErrors.empty()) {
        // The game runs, but a script failed while it started: report it as the error it is
        data["script_errors"] = scriptErrors;
        text = "Play started but " + std::to_string(scriptErrors.size()) +
               " script error(s) happened at startup (the game is still running; fix the script, then play "
               "{action: \"restart\"}, or stop it):\n" + w3::formatConsoleText(scriptErrors, 6000);
        Json others = Json::array();
        for (const Json& entry : problems) {
            if (!(entry.is_object() && isScriptStartupError(entry))) others.push_back(entry);
        }
        if (!others.empty()) text += "\nOther console problems since start:\n" + w3::formatConsoleText(others, 2000);
        return ToolResult::error(text, data);
    }
    if (job.status() == JobStatus::Succeeded) {
        text = job.message() + " Started in " + std::to_string(data.value("startup_seconds", 0.0)).substr(0, 5) + " s.";
        if (data.contains("audio_capture_error")) text += " Audio capture failed: " + data["audio_capture_error"].get<std::string>() + ".";
        if (AudioCapture::isActive()) {
            data["audio_capture"] = {{"session", AudioCapture::sessionId()}, {"next", "play stop gives the numbers; audio_session report (group audio) reads them"}};
            text += " Capturing audio: play stop gives its numbers, audio_session report (group audio) reads them.";
        }
        if (!problems.empty()) {
            text += " Console problems since start:\n" + w3::formatConsoleText(problems, 4000);
        } else {
            text += " No errors in the console.";
        }
        text += "\nNext: wait / send_input / screenshot / eval_lua; read_console since " +
                std::to_string(data.value("next_cursor", 0ull)) + ".";
        return ToolResult::success(text, data);
    }
    text = job.message() + ".";
    if (!problems.empty()) text += "\n" + w3::formatConsoleText(problems, 8000);
    else text += " Nothing was logged; check read_console.";
    (void)ctx;
    return ToolResult::error(text, data);
}

void replyWithOptionalScreenshot(const CallPtr& call, ToolResult result) {
    w3::CaptureRequest request;
    bool wanted = false;
    std::string error;
    if (!w3::parseScreenshotOption(call->args(), request, wanted, error) || !wanted) {
        if (!error.empty()) result.text += "\n(" + error + ")";
        call->reply(std::move(result));
        return;
    }
    auto shared = std::make_shared<ToolResult>(std::move(result));
    w3::captureView(call, request, [call, shared](w3::CaptureResult capture) {
        if (capture.ok) {
            shared->images.push_back(std::move(capture.image));
            if (!capture.note.empty()) shared->text += "\nScreenshot note: " + capture.note;
        } else {
            shared->text += "\n(Screenshot failed: " + capture.error + ")";
        }
        call->reply(std::move(*shared));
    });
}

void playStart(const CallPtr& call, const Json& sceneRef);

void playStop(const CallPtr& call, std::function<void()> after) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const uint32_t playId = project->getPlaySessionSceneId();
    PlayToolsState& s = state();
    const Json& args = call->args();
    const double timeout = std::clamp(args.value("timeout_seconds", 120.0), 1.0, 3600.0);

    if (playId == NULL_PROJECT_SCENE) {
        if (after) {
            after();
            return;
        }
        Json data = w3::playStateJson(ctx);
        call->ok("Not playing." + (s.last.serial ? " Last session: " + sessionSummaryText(s.last) : std::string()), data);
        return;
    }
    SceneProject* sp = project->getScene(playId);
    s.current.stopRequested = true;
    releaseHeldInput(ctx);
    // an agent's Stop: the Play Mode Changes window stays shut (keep_changes, get_play_changes)
    PlayChangeReview::get().setQuietStop(true);
    if (sp && sp->playState != ScenePlayState::CANCELLING) project->stop(playId);
    ensureMonitor(ctx);

    call->waitUntil([call, playId]() {
        McpContext& c = call->ctx();
        observeSession(c);
        Project* p = c.project();
        SceneProject* scene = p->getScene(playId);
        return p->getPlaySessionSceneId() == NULL_PROJECT_SCENE && (!scene || scene->playState == ScenePlayState::STOPPED);
    }, static_cast<int>(timeout * 1000.0), [call, after, playId](bool timedOut) {
        McpContext& c = call->ctx();
        observeSession(c);
        PlayChangeReview::get().setQuietStop(false);
        if (timedOut) {
            call->fail("Play mode is still stopping (waiting for a C++ build to cancel?). Check get_play_state.",
                       w3::playStateJson(c));
            return;
        }
        if (after) {
            after();
            return;
        }
        // keep_changes: what to do with the edits made while playing (absent: they wait, get_play_changes)
        std::string keepNote;
        const std::string keep = call->args().value("keep_changes", std::string());
        Json kept = nullptr;
        if (PlayChangeReview::get().pending(playId)) {
            if (keep == "all") {
                PlayChangeReview::ApplyResult applied = PlayChangeReview::get().apply(c.project(), playId, nullptr);
                kept = {{"kept", applied.kept.size()}, {"skipped", applied.skipped.size()}};
                if (applied.entryId != 0) kept["undo"] = {{"id", applied.entryId}, {"name", applied.entryName}};
                keepNote = " Kept " + std::to_string(applied.kept.size()) + " play-mode change(s)" +
                           (applied.skipped.empty() ? std::string(".") : ", " + std::to_string(applied.skipped.size()) + " could not be kept.");
            } else if (keep == "none") {
                PlayChangeReview::get().discard(playId);
                keepNote = " Play-mode changes discarded.";
            } else {
                keepNote = " " + std::to_string(PlayChangeReview::get().pendingCount(playId)) +
                           " play-mode change(s) can be kept: get_play_changes, then apply_play_changes.";
            }
        }
        const SessionRecord& last = state().last;
        Json data = w3::playStateJson(c);
        data["problems"] = problemsSince(last.startLogId, 30);
        data["next_cursor"] = Out::lastLogId();
        if (!kept.is_null()) data["kept_changes"] = kept;
        std::string text = "Stopped. " + sessionSummaryText(last) + " The scene is back to its edit-time state." + keepNote;
        if (!data["problems"].empty()) text += "\nProblems during the session:\n" + w3::formatConsoleText(data["problems"], 4000);
        // the capture's numbers, read off the main thread (R20)
        const std::string session = c.project()->takeAudioCaptureSession();
        if (!session.empty()) {
            const std::filesystem::path root = AudioAnalysis::captureRoot(c.project()->getProjectPath());
            auto summary = std::make_shared<Json>();
            JobPtr job = c.jobs().create("play_audio_summary", "Measure the audio capture");
            jobs::submit([root, session, summary, job]() {
                AudioAnalysis::Capture capture;
                AudioSessionData audio;
                std::string error;
                if (AudioAnalysis::findCapture(root, session, std::string(), capture, error) && AudioSessionReader::load(capture, 0.0, -1.0, audio, error)) {
                    *summary = AudioSessionReader::stopSummary(audio);
                    job->succeed("Measured.", *summary);
                } else {
                    job->fail(error);
                }
            }, {jobs::Lane::Background, jobs::GateClass::Default, "play-audio-summary"});
            replyWhenJobDone(call, job, true, 120.0, [data, text, summary](const McpJob&) mutable {
                if (summary->is_object()) {
                    const Json& audio = *summary;
                    data["audio"] = audio;
                    const auto number = [&audio](const char* key) { return audio[key].is_number() ? std::to_string(audio[key].get<double>()).substr(0, 6) : std::string("none"); };
                    text += "\nAudio capture " + audio.value("session", "") + ": " + number("duration_s") + " s, " + number("integrated_lufs") + " LUFS, momentary max " +
                            number("momentary_max") + ", true peak " + number("true_peak_dbtp") + " dBTP, " + std::to_string(audio.value("voices_started", 0)) + " voices. Next: audio_session report.";
                }
                return ToolResult::success(text, data);
            });
            return;
        }
        call->ok(text, data);
    });
}

void playStart(const CallPtr& call, const Json& sceneRef) {
    // Play builds the C++ scripts: not while an export or compile_scripts job holds the project
    if (call->server().refuseIfBusy(call, ToolActivity::PlayStart)) return;
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    const bool wait = args.value("wait", true);
    const double timeout = std::clamp(args.value("timeout_seconds", 120.0), 1.0, 3600.0);

    SceneProject* sp = nullptr;
    if (!sceneRef.is_null()) {
        std::string error;
        sp = McpRefs::resolveScene(project, sceneRef, error);
        if (!sp) {
            call->fail(error);
            return;
        }
        if (project->isPrefabStage(sp->id)) sp = project->getScene(project->getSceneBehindPrefabStage(sp->id));
    } else if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        sp = w3::gameViewScene(project);
    } else {
        sp = w3::defaultPlayScene(project);
    }
    if (!sp) {
        call->fail("No scene to play: open or create a scene first.");
        return;
    }
    if (!sp->scene || !sp->opened) {
        call->fail("Scene \"" + sp->name + "\" is not open in the editor; open it first (Play runs an open scene, "
                   "like the toolbar), or play the scene that loads it.");
        return;
    }

    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId != NULL_PROJECT_SCENE) {
        SceneProject* playing = project->getScene(playId);
        if (playId == sp->id && playing && isRunningState(playing->playState)) {
            Json data = w3::playStateJson(ctx);
            replyWithOptionalScreenshot(call, ToolResult::success(
                "Already playing \"" + sp->name + "\" (" + McpRefs::playStateName(playing->playState) +
                "). Use action restart to start over.", data));
            return;
        }
        if (playId == sp->id && playing && playing->playState == ScenePlayState::LOADING) {
            // join the startup already under way
            for (PlayStartWait& w : state().startWaits) {
                if (w.sceneId == playId && !w.job->finished()) {
                    replyWhenJobDone(call, w.job, wait, timeout, [call](const McpJob& job) {
                        return playStartResult(call->ctx(), job, false);
                    });
                    return;
                }
            }
        }
        if (playing && playing->playState == ScenePlayState::CANCELLING) {
            call->fail("Play mode is still stopping; call get_play_state until it is stopped, then start again.",
                       w3::playStateJson(ctx));
            return;
        }
        call->fail("\"" + (playing ? playing->name : std::string("?")) + "\" is " +
                   (playing ? McpRefs::playStateName(playing->playState) : std::string("playing")) +
                   "; stop it first (play {action: \"stop\"}) or use action restart.", w3::playStateJson(ctx));
        return;
    }
    if (sp->playState != ScenePlayState::STOPPED) {
        call->fail("Scene \"" + sp->name + "\" is " + McpRefs::playStateName(sp->playState) + "; try again in a moment.");
        return;
    }

    observeSession(ctx);
    releaseHeldInput(ctx);
    const uint64_t startLogId = Out::lastLogId();
    state().pendingStartLogId = startLogId;
    project->start(sp->id);
    if (project->getPlaySessionSceneId() != sp->id) {
        state().pendingStartLogId = 0;
        Json problems = problemsSince(startLogId, 20);
        call->fail("Play did not start." + (problems.empty() ? std::string() : "\n" + w3::formatConsoleText(problems)),
                   Json{{"problems", problems}});
        return;
    }
    observeSession(ctx);

    JobPtr job = ctx.jobs().create("play", "Play \"" + sp->name + "\"");
    job->setProgress(-1.0, "Starting");
    const uint32_t sceneId = sp->id;
    job->setCancelHandler([&ctx, sceneId]() {
        // finishStartWaits stops play on the next frame and marks the job cancelled
        (void)sceneId;
        ensureMonitor(ctx);
    });
    PlayStartWait startWait;
    startWait.job = job;
    startWait.sceneId = sceneId;
    startWait.startLogId = startLogId;
    startWait.started = Clock::now();
    state().startWaits.push_back(startWait);
    ensureMonitor(ctx);

    if (!wait) {
        call->ok("Play is starting (job " + job->id() + "). Poll get_job or get_play_state; play {action:\"start\"} "
                 "again joins it.", Json{{"job_id", job->id()}, {"state", "loading"}});
        return;
    }
    // Screenshots only make sense once it plays: reply, then shoot
    const bool wantsShot = args.contains("screenshot") && !args["screenshot"].is_null() && args["screenshot"] != false;
    if (!wantsShot) {
        replyWhenJobDone(call, job, true, timeout, [call](const McpJob& done) {
            return playStartResult(call->ctx(), done, false);
        });
        return;
    }
    call->waitUntil([job]() { return job->finished(); }, static_cast<int>(timeout * 1000.0), [call, job](bool timedOut) {
        if (timedOut) {
            call->ok("Play is still starting (job " + job->id() + "): poll get_job.", job->toJson(10));
            return;
        }
        ToolResult result = playStartResult(call->ctx(), *job, true);
        if (result.isError) {
            call->reply(std::move(result));
            return;
        }
        // let the first frames draw
        call->afterFrames(3, [call, result]() mutable { replyWithOptionalScreenshot(call, std::move(result)); });
    });
}

// play's time_scale / fixed_fps / auto_pause (any action); false + error for bad values
bool applyTimeArgs(McpContext& ctx, const Json& args, std::string& error) {
    TimeControl next = state().time;
    if (args.contains("time_scale") && !args["time_scale"].is_null()) {
        if (!args["time_scale"].is_number() || args["time_scale"].get<double>() < 0.0 || args["time_scale"].get<double>() > 100.0) {
            error = "time_scale must be a number from 0 to 100 (1 = real time, 0.5 = half speed, 0 = frozen).";
            return false;
        }
        next.timeScale = args["time_scale"].get<double>();
    }
    if (args.contains("fixed_fps") && !args["fixed_fps"].is_null()) {
        const Json& v = args["fixed_fps"];
        if (v.is_boolean()) next.fixedFps = v.get<bool>() ? 60 : 0;
        else if (v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() <= 1000) next.fixedFps = v.get<int>();
        else {
            error = "fixed_fps must be an integer from 0 (real time) to 1000; 60 = every frame advances exactly 1/60 s.";
            return false;
        }
    }
    if (args.contains("auto_pause") && !args["auto_pause"].is_null()) {
        if (!args["auto_pause"].is_boolean()) {
            error = "auto_pause must be true or false.";
            return false;
        }
        next.autoPause = args["auto_pause"].get<bool>();
    }
    const bool clockChanged = next.timeScale != state().time.timeScale || next.fixedFps != state().time.fixedFps ||
                              args.contains("time_scale") || args.contains("fixed_fps");
    next.advancers = state().time.advancers;
    state().time = next;
    if (clockChanged && ctx.project() && ctx.project()->getPlaySessionSceneId() != NULL_PROJECT_SCENE) applyTimeSettings();
    applyHold(ctx);
    return true;
}

std::string timeSummary() {
    const TimeControl& t = state().time;
    std::string text = "Clock: ";
    text += t.fixedFps > 0 ? "fixed 1/" + std::to_string(t.fixedFps) + " s per frame" : std::string("real time");
    if (t.timeScale != 1.0) text += ", time_scale " + std::to_string(t.timeScale).substr(0, 5);
    if (t.autoPause) text += ", auto_pause (the game only runs inside wait / send_input / step)";
    return text + ".";
}

void playTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    observeSession(ctx);
    const std::string action = lowerCopy(args.value("action", ""));
    const Json sceneRef = args.contains("scene") ? args["scene"] : Json();
    {
        std::string error;
        if (!applyTimeArgs(ctx, args, error)) {
            call->fail(error);
            return;
        }
    }
    if (action == "set") {
        Json data = w3::playStateJson(ctx);
        call->ok(timeSummary() + (project->getPlaySessionSceneId() == NULL_PROJECT_SCENE
                                      ? " Applies when play starts (and to later sessions until changed)."
                                      : " Applied to the running game."), data);
        return;
    }

    if (action == "start" || action == "restart") {
        // Play writes generated sources and build trees into the project's internal folder (in
        // later frames too, outside this call's write scope): it must stay inside the project
        if (const std::string escape = McpRefs::internalFoldersEscape(project); !escape.empty()) {
            call->fail(escape);
            return;
        }
    }
    if (action == "start") {
        playStart(call, sceneRef);
        return;
    }
    if (action == "stop") {
        playStop(call, nullptr);
        return;
    }
    if (action == "restart") {
        Json scene = sceneRef;
        if (scene.is_null() && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
            scene = project->getPlaySessionSceneId();
        }
        playStop(call, [call, scene]() { playStart(call, scene); });
        return;
    }

    const uint32_t playId = project->getPlaySessionSceneId();
    SceneProject* sp = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
    if (action != "pause" && action != "resume" && action != "step") {
        call->fail("action must be start, stop, restart, pause, resume, step or set.");
        return;
    }
    if (!sp || !isRunningState(sp->playState)) {
        call->fail(std::string("Can't ") + action + ": " +
                   (sp ? "play mode is " + McpRefs::playStateName(sp->playState) + "." : "nothing is playing; use action start."),
                   w3::playStateJson(ctx));
        return;
    }
    const uint64_t cursor = Out::lastLogId();

    if (action == "pause") {
        project->pause(playId);
        replyWithOptionalScreenshot(call, ToolResult::success("Paused \"" + sp->name + "\". Use step to advance frame by frame, resume to continue.",
                                                             w3::playStateJson(ctx)));
        return;
    }
    if (action == "resume") {
        project->resume(playId);
        replyWithOptionalScreenshot(call, ToolResult::success("Resumed \"" + sp->name + "\".", w3::playStateJson(ctx)));
        return;
    }

    // step: n frames, each a single update of the paused game
    const int frames = std::clamp(args.value("frames", 1), 1, 600);
    auto done = std::make_shared<int>(0);
    auto finish = [call, done, cursor](const std::string& note) {
        McpContext& cx = call->ctx();
        observeSession(cx);
        Json data = w3::playStateJson(cx);
        w3::ConsoleQuery query;
        query.since = cursor;
        query.limit = 50;
        w3::ConsoleResult console = w3::queryConsole(query);
        data["stepped_frames"] = *done;
        data["console"] = console.entries;
        data["next_cursor"] = console.nextCursor;
        std::string text = "Stepped " + std::to_string(*done) + " frame(s)" + note + ".";
        if (!console.entries.empty()) text += " Console:\n" + w3::formatConsoleText(console.entries, 3000);
        // stepping that ends play (crash) is an error
        replyWithOptionalScreenshot(call, cx.project()->getPlaySessionSceneId() == NULL_PROJECT_SCENE
            ? ToolResult::error(text + "\nPlay ended: " + sessionSummaryText(state().last), data)
            : ToolResult::success(text, data));
    };

    // auto_pause: the game stays "playing" and runs exactly `frames` frames
    if (state().time.autoPause && sp->playState == ScenePlayState::PLAYING) {
        auto advance = std::make_shared<Advance>();
        const uint64_t base = Engine::getGameFrames();
        advance->acquire(ctx);
        auto progress = std::make_shared<FrameProgress>();
        auto stalled = std::make_shared<bool>(false);
        call->waitUntil([call, playId, base, frames, done, progress, stalled]() {
            McpContext& cx = call->ctx();
            observeSession(cx);
            *done = static_cast<int>(Engine::getGameFrames() - base);
            if (cx.project()->getPlaySessionSceneId() != playId || *done >= frames) return true;
            *stalled = progress->stalled();
            return *stalled;
        }, 0, [advance, finish, playId, call, stalled](bool timedOut) {
            advance->release();
            if (call->ctx().project()->getPlaySessionSceneId() != playId) finish(", then play ended");
            else finish(*stalled || timedOut ? " (stopped: the game did not advance for " + std::to_string((int)kStallSeconds) +
                                               " s; is it rendering?); held" : "; held (auto_pause)");
        });
        return;
    }

    if (sp->playState == ScenePlayState::PLAYING) project->pause(playId);
    auto remaining = std::make_shared<int>(frames);
    auto advance = std::make_shared<Advance>();
    advance->acquire(ctx);   // a held simulation would not step
    auto stepOnce = std::make_shared<std::function<void()>>();
    *stepOnce = [call, playId, remaining, done, stepOnce, finish, advance]() {
        McpContext& c = call->ctx();
        Project* p = c.project();
        SceneProject* scene = p->getScene(playId);
        if (!scene || p->getPlaySessionSceneId() != playId || !isRunningState(scene->playState) || call->cancelled()) {
            advance->release();
            finish(", then play ended");
            return;
        }
        if (*remaining <= 0) {
            advance->release();
            finish("; paused");
            return;
        }
        p->step(playId);
        call->waitUntil([call, playId]() {
            McpContext& cx = call->ctx();
            observeSession(cx);
            return !cx.project()->isStepPending(playId) || cx.project()->getPlaySessionSceneId() != playId;
        }, static_cast<int>(kStallSeconds * 1000.0), [remaining, done, stepOnce, advance, finish](bool timedOut) {
            if (timedOut) {
                // the step did not happen: not counted, and no more steps
                advance->release();
                finish(" (stopped: a step did not run in " + std::to_string((int)kStallSeconds) + " s; is the game rendering?)");
                return;
            }
            (*remaining)--;
            (*done)++;
            (*stepOnce)();
        });
    };
    (*stepOnce)();
}

// ================================================================== tool: get_play_state

void getPlayStateTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    observeSession(ctx);
    if (ctx.project() && ctx.project()->getPlaySessionSceneId() != NULL_PROJECT_SCENE) ensureMonitor(ctx);
    Json data = w3::playStateJson(ctx);
    data["console_cursor"] = Out::lastLogId();
    PlayToolsState& s = state();
    std::string text = "Play state: " + data.value("state", std::string("?"));
    if (data.contains("scene")) text += " (\"" + data["scene"].value("name", std::string()) + "\")";
    text += ".";
    if (s.current.active) {
        text += " Session: " + std::to_string((int)secondsSince(s.current.startTime)) + " s, " +
                std::to_string(s.current.errors) + " error(s) logged.";
        if (!s.current.crash.empty()) text += " CRASHED: " + s.current.crash;
        text += " Script coroutines: " + std::to_string(data.value("coroutines", 0)) + ", timers: " +
                std::to_string(data.value("timers", 0)) + ".";
    } else if (s.last.serial != 0) {
        text += " Last session: " + sessionSummaryText(s.last);
    }
    call->ok(text, data);
}

// ================================================================== tool: eval_lua

void evalLuaTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    observeSession(ctx);
    const std::string code = args.value("code", "");
    if (code.empty()) {
        call->fail("code is empty. Example: {\"code\": \"local gc = script('GameController'); return gc.state, gc.score\"}");
        return;
    }
    const bool allowEdit = args.value("allow_edit_mode", false);
    const double timeout = std::clamp(args.value("timeout_seconds", 5.0), 0.1, 60.0);
    const int maxDepth = std::clamp(args.value("max_depth", 4), 1, 12);
    const size_t maxItems = static_cast<size_t>(std::clamp(args.value("max_items", 200), 1, 5000));

    const uint32_t playId = project->getPlaySessionSceneId();
    SceneProject* scene = nullptr;
    bool editMode = false;
    std::string error;
    if (playId != NULL_PROJECT_SCENE) {
        scene = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!scene) {
            call->fail(error, w3::playStateJson(ctx));
            return;
        }
    } else {
        if (!allowEdit) {
            call->fail("eval_lua runs in the playing game: start it with play {action: \"start\"} first. To run Lua "
                       "against the edit-time scene pass allow_edit_mode: true (changes made that way are NOT undoable "
                       "and mark nothing modified).");
            return;
        }
        scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!scene) {
            call->fail(error);
            return;
        }
        editMode = true;
    }

    const uint64_t cursor = Out::lastLogId();
    LuaEvalResult result = runLua(project, scene, code, timeout, maxDepth, maxItems);
    if (editMode && scene) scene->needUpdateRender = true;

    w3::ConsoleQuery query;
    query.since = cursor;
    query.limit = 50;
    w3::ConsoleResult console = w3::queryConsole(query);

    Json data = {
        {"mode", editMode ? "edit" : "play"},
        {"scene", McpRefs::sceneJson(project, *scene)},
        {"values", result.values},
        {"printed", result.printed},
        {"console", console.entries},
        {"next_cursor", console.nextCursor}
    };
    std::string text;
    if (!result.ok) {
        data["error"] = result.error;
        text = "Lua error: " + result.error;
    } else if (result.values.empty()) {
        text = "OK (no return value).";
    } else if (result.values.size() == 1) {
        text = "=> " + result.values[0].dump();
    } else {
        text = "=> " + result.values.dump();
    }
    if (text.size() > 20000) text = text.substr(0, 20000) + " ... (see structuredContent)";
    if (!result.printed.empty()) {
        text += "\nprinted:";
        for (const std::string& line : result.printed) text += "\n" + line;
    }
    if (!console.entries.empty()) text += "\nconsole:\n" + w3::formatConsoleText(console.entries, 3000);
    if (editMode) text += "\n(edit mode: changes to the scene are not undoable)";
    observeSession(ctx);
    if (playId != NULL_PROJECT_SCENE && project->getPlaySessionSceneId() == NULL_PROJECT_SCENE) {
        text += "\nPlay ended: " + sessionSummaryText(state().last);
    }
    if (result.ok) call->ok(text, data);
    else call->fail(text, data);
}

// ================================================================== runtime entities

std::string componentNameOf(ComponentType type) {
    return lowerCopy(Catalog::getComponentName(type, true));
}

void listRuntimeEntitiesTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    std::string error;
    SceneProject* sp = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    const std::string nameFilter = lowerCopy(args.value("name_contains", ""));
    const bool createdOnly = args.value("created_only", false);
    const bool withComponents = args.value("with_components", false);
    const size_t limit = static_cast<size_t>(std::clamp(args.value("limit", 200), 1, 5000));
    bool hasComponentFilter = false;
    ComponentType componentFilter = ComponentType::Transform;
    if (args.contains("component")) {
        if (!McpRefs::resolveComponent(args["component"], componentFilter, error)) {
            call->fail(error);
            return;
        }
        hasComponentFilter = true;
    }

    const std::vector<Entity> created = project->getPlayCreatedEntities(sp);
    const std::set<Entity> createdSet(created.begin(), created.end());
    EntityPath::Cache cache;
    Json list = Json::array();
    size_t matched = 0;
    for (Entity entity : w3::liveEntities(project, sp)) {
        if (!sp->scene->isEntityCreated(entity)) continue;
        const bool isCreated = createdSet.count(entity) > 0;
        if (createdOnly && !isCreated) continue;
        const std::string name = sp->scene->getEntityName(entity);
        if (!nameFilter.empty() && lowerCopy(name).find(nameFilter) == std::string::npos) continue;
        std::vector<ComponentType> components;
        if (hasComponentFilter || withComponents) components = Catalog::findComponents(sp->scene, entity);
        if (hasComponentFilter && std::find(components.begin(), components.end(), componentFilter) == components.end()) continue;
        matched++;
        if (list.size() >= limit) continue;
        Json item = {{"id", entity}, {"name", name}, {"path", EntityPath::of(sp->scene, entity, &cache)}};
        if (isCreated) item["created_at_runtime"] = true;
        if (withComponents) {
            Json names = Json::array();
            for (ComponentType type : components) names.push_back(componentNameOf(type));
            item["components"] = names;
        }
        list.push_back(item);
    }
    Json data = {{"scene", McpRefs::sceneJson(project, *sp)}, {"entities", list}, {"total", matched},
                 {"created_at_runtime", created.size()}, {"running_scenes", runningScenesJson(project)}};
    std::string text = std::to_string(matched) + " GameObject(s) in the running scene \"" + sp->name + "\" (" +
                       std::to_string(created.size()) + " created at runtime)";
    if (matched > list.size()) text += ", first " + std::to_string(list.size()) + " listed";
    text += ":";
    for (const Json& item : list) {
        text += "\n" + std::to_string(item["id"].get<Entity>()) + " " + item["path"].get<std::string>() +
                (item.contains("created_at_runtime") ? " (runtime)" : "");
        if (text.size() > 8000) {
            text += "\n... (see structured entities)";
            break;
        }
    }
    call->ok(text, data);
}

// A C++ script's DPROPERTY member, read live through the pointer the generated bindings set
Json cppMemberJson(Project* project, SceneProject* sp, const ScriptProperty& prop) {
    void* member = prop.memberPtr;
    switch (prop.type) {
        case ScriptPropertyType::Bool: return *static_cast<bool*>(member);
        case ScriptPropertyType::Int: return *static_cast<int*>(member);
        case ScriptPropertyType::Float: return McpValues::number(*static_cast<float*>(member));
        case ScriptPropertyType::String: return *static_cast<std::string*>(member);
        case ScriptPropertyType::Vector2: return McpValues::toJson(*static_cast<Vector2*>(member));
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3: return McpValues::toJson(*static_cast<Vector3*>(member));
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4: return McpValues::toJson(*static_cast<Vector4*>(member));
        case ScriptPropertyType::EntityReference: {
            // a pointer member (Sprite*, a script class...): null, or the GameObject it was bound to
            if (*static_cast<void**>(member) == nullptr) return nullptr;
            if (!std::holds_alternative<EntityReference>(prop.value)) return Json{{"bound", true}};
            return McpValues::entityReferenceToJson(project, sp, std::get<EntityReference>(prop.value));
        }
        case ScriptPropertyType::Prefab: {
            const std::string& path = static_cast<PrefabAsset*>(member)->getPath();
            return path.empty() ? Json(nullptr) : Json(path);
        }
        case ScriptPropertyType::Asset: {
            const AssetRef& ref = *static_cast<AssetRef*>(member);
            if (!ref.isSet()) return nullptr;
            return Json{{"path", ref.path}, {"guid", ref.guid.str()}};
        }
    }
    return nullptr;
}

// what a script's coroutines wait for and its pending timers (ScriptScheduler)
Json coroutinesJson(const void* owner) {
    Json list = Json::array();
    for (const ScriptCoroutineInfo& co : ScriptScheduler::coroutinesOf(owner)) {
        Json item = {{"id", co.id}, {"status", co.status}, {"origin", co.origin}};
        if (!co.waiting.empty()) item["waiting"] = co.waiting;
        if (!co.method.empty()) item["method"] = co.method;
        list.push_back(item);
    }
    return list;
}

Json timersJson(const void* owner) {
    Json list = Json::array();
    for (const ScriptTimerInfo& timer : ScriptScheduler::timersOf(owner)) {
        list.push_back({{"id", timer.id}, {"remaining", round3(timer.remaining)}, {"interval", round3(timer.interval)},
                        {"unscaled", timer.unscaled}, {"callback", timer.callback}});
    }
    return list;
}

Json scriptStateJson(Project* project, SceneProject* sp, Entity entity, int maxDepth) {
    Json scripts = Json::array();
    ScriptComponent* component = sp->scene->findComponent<ScriptComponent>(entity);
    if (!component) return scripts;
    lua_State* L = LuaBinding::getLuaState();
    for (ScriptEntry& entry : component->scripts) {
        // the lifecycle (script-lifecycle 6.1): init ran, start ran, onEnable ran and no onDisable since
        const ScriptState* state = ScriptRuntime::getState(entry.instance);
        Json item = {{"class", entry.className}, {"type", entry.type == ScriptType::LUA ? "lua" : "cpp"},
                     {"enabled", entry.enabled}, {"initialized", state && state->initialized}, {"started", state && state->started},
                     {"active_and_enabled", entry.instance && entry.enabled && !(state && state->destroyed) &&
                                            sp->scene->isEntityActiveInHierarchy(entity)}};
        if (!entry.instance) item["running"] = false;
        if (entry.type == ScriptType::LUA && entry.instance && L) {
            const int top = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<lua_Integer>(reinterpret_cast<intptr_t>(entry.instance)));
            LuaSerializer serializer{L, project, maxDepth, 100};
            Json fields = serializer.toJson(-1, 0);
            if (fields.is_object()) {
                for (const char* hidden : {"__name", "__scriptRef", "__released", "scene"}) fields.erase(hidden);
            }
            item["state"] = fields;
            lua_settop(L, top);
            item["coroutines"] = coroutinesJson(entry.instance);
            item["timers"] = timersJson(entry.cppScript ? static_cast<const void*>(entry.cppScript) : entry.instance);
        } else if (entry.type == ScriptType::CPP && entry.instance) {
            // C++ has no reflection: the DPROPERTY members (what the Inspector shows)
            Json fields = Json::object();
            for (const ScriptProperty& prop : entry.properties) {
                if (prop.memberPtr) fields[prop.name] = cppMemberJson(project, sp, prop);
            }
            item["state"] = fields;
            // ScriptBase::invoke timers (owner: the script object)
            item["timers"] = timersJson(entry.instance);
        }
        scripts.push_back(item);
    }
    return scripts;
}

// what a component holds at runtime besides its properties (the last move and step of a
// character controller), read only
void addRuntimeState(Scene* scene, Entity entity, ComponentType type, const std::set<std::string>& onlyProps, Json& props) {
    auto put = [&](const char* name, Json value) {
        if (onlyProps.empty() || onlyProps.count(name)) props[name] = std::move(value);
    };
    auto vec2 = [](const Vector2& v) { return Json::array({v.x, v.y}); };
    if (type == ComponentType::CharacterController2DComponent) {
        const CharacterController2DComponent* c = scene->findComponent<CharacterController2DComponent>(entity);
        if (!c) return;
        put("isGrounded", c->isGrounded);
        put("isOnSteepSlope", c->isOnSteepSlope);
        put("collisionFlags", c->collisionFlags);
        put("velocity", vec2(c->velocity));
        put("groundNormal", vec2(c->groundNormal));
        put("groundEntity", c->groundEntity == NULL_ENTITY ? Json() : Json(c->groundEntity));
        put("groundVelocity", vec2(c->groundVelocity));
        put("refusedWithRigidbody", c->refusedWithRigidbody);
    }
    if (type == ComponentType::CharacterControllerComponent) {
        const CharacterControllerComponent* c = scene->findComponent<CharacterControllerComponent>(entity);
        if (!c) return;
        auto vec3 = [](const Vector3& v) { return Json::array({v.x, v.y, v.z}); };
        put("isGrounded", c->isGrounded);
        put("isOnSteepSlope", c->isOnSteepSlope);
        put("collisionFlags", c->collisionFlags);
        put("velocity", vec3(c->velocity));
        put("groundNormal", vec3(c->groundNormal));
        put("groundEntity", c->groundEntity == NULL_ENTITY ? Json() : Json(c->groundEntity));
        put("groundVelocity", vec3(c->groundVelocity));
        put("refusedWithRigidbody", c->refusedWithRigidbody);
    }
    if (type == ComponentType::NavMeshAgentComponent) {
        // what the crowd says (S19 R37)
        const NavMeshAgentComponent* c = scene->findComponent<NavMeshAgentComponent>(entity);
        if (!c) return;
        auto vec3 = [](const Vector3& v) { return Json::array({v.x, v.y, v.z}); };
        put("destination", vec3(c->destination));
        put("pathStatus", nav::pathStatusName(c->pathStatus));
        put("pathPending", c->pathPending);
        put("remainingDistance", c->remainingDistance);
        put("velocity", vec3(c->velocity));
        put("desiredVelocity", vec3(c->desiredVelocity));
        put("isOnNavMesh", c->isOnNavMesh);
        put("isOnOffMeshLink", c->isOnOffMeshLink);
    }
}

// camera shake spec 10: a Camera Shaker's play state and a Camera Follow's motion (read only)
Json shakerRuntimeJson(const CameraShakerComponent& shaker) {
    Json instances = Json::array();
    float trauma = 0.0f;
    Vector3 position(0, 0, 0), rotation(0, 0, 0);
    if (const camera::ShakerRuntime* runtime = shaker.runtime.get()) {
        for (const camera::ShakerInstanceEntry& entry : runtime->instances) {
            const char* state = "stopped";
            switch (entry.instance.getState()) {
                case camera::ShakeState::FadingIn: state = "fading_in"; break;
                case camera::ShakeState::Sustained: state = "sustained"; break;
                case camera::ShakeState::FadingOut: state = "fading_out"; break;
                default: break;
            }
            if (entry.instance.isPaused() && !entry.instance.isFinished()) state = "paused";
            instances.push_back({{"preset", entry.presetName}, {"state", state},
                                 {"strength", McpValues::number(entry.instance.getCurrentStrength())}});
        }
        trauma = runtime->trauma.trauma;
        position = runtime->offsetPosition;
        rotation = runtime->offsetRotation;
    }
    return {{"instances", instances}, {"trauma", McpValues::number(trauma)},
            {"offset_position", McpValues::toJson(position)}, {"offset_rotation", McpValues::toJson(rotation)}};
}

Json followRuntimeJson(const CameraFollowComponent& follow) {
    Json runtime = {{"velocity", McpValues::toJson(follow.velocity)}, {"desired", McpValues::toJson(follow.desired)},
                    {"placed", follow.placed}};
    if (follow.foreignWriteDetected) runtime["foreign_write"] = true;
    if (follow.targetPersistent) runtime["target_persistent"] = true;
    return runtime;
}

// the live state of a 2D joint (Unity's read-outs): forces N, torques N m, points, engine angle units
Json joint2DReadoutsJson(Scene* scene, Entity entity) {
    Joint2D joint(scene, entity);
    static const char* limitStates[] = {"inactive", "lower", "upper", "equal"};
    Json out = {{"live", joint.isLive()}, {"broken", joint.isBroken()}};
    if (!joint.isLive()) return out;
    const Vector2 force = joint.getReactionForce();
    out["reaction_force"] = {force.x, force.y};
    out["reaction_torque"] = joint.getReactionTorque();
    out["joint_angle"] = joint.getJointAngle();
    out["joint_speed"] = joint.getJointSpeed();
    out["joint_translation"] = joint.getJointTranslation();
    out["joint_linear_speed"] = joint.getJointLinearSpeed();
    out["motor_torque"] = joint.getMotorTorque();
    out["motor_force"] = joint.getMotorForce();
    out["current_length"] = joint.getCurrentLength();
    out["limit_state"] = limitStates[(int)joint.getLimitState()];
    out["reference_angle"] = joint.getReferenceAngle();
    return out;
}

// the live state of a Rigidbody 2D: mass, velocities, the Box2D shapes of each collider
Json rigidbody2DReadoutsJson(Scene* scene, Entity entity) {
    Rigidbody2DComponent& component = scene->getComponent<Rigidbody2DComponent>(entity);
    if (!b2Body_IsValid(component.body)) return Json{{"live", false}};
    Rigidbody2D body(scene, entity);
    const Vector2 velocity = body.getLinearVelocity();
    const Vector2 center = body.getCenterOfMass();
    Json pieces = Json::array();
    for (size_t i = 0; i < component.colliders.size(); i++) pieces.push_back(body.getBox2DShapes(i).size());
    return Json{{"live", true}, {"mass", body.getMass()}, {"inertia", body.getRotationalInertia()},
                {"center_of_mass", {center.x, center.y}}, {"linear_velocity", {velocity.x, velocity.y}},
                {"angular_velocity", body.getAngularVelocity()}, {"awake", body.isAwake()}, {"collider_shapes", pieces}};
}

// physics.contacts [{other, other_path, collider, other_collider, trigger, since_step, asleep}]
// and physics.callbacks (the contact methods its scripts define), for bodies and listeners
Json physicsContactsJson(SceneProject* sp, Entity entity) {
    Json out = Json::object();
    Scene* scene = sp->scene;
    std::vector<ContactSummary> contacts;
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    bool hasBody = false;
#ifdef JASPERENGINE_PHYSICS_2D
    if (scene->findComponent<Rigidbody2DComponent>(entity)) {
        hasBody = true;
        contacts = physics->getContactSummary2D(entity);
    }
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    if (scene->findComponent<RigidbodyComponent>(entity)) {
        hasBody = true;
        const std::vector<ContactSummary> contacts3D = physics->getContactSummary3D(entity);
        contacts.insert(contacts.end(), contacts3D.begin(), contacts3D.end());
    }
#endif
    const std::vector<std::string> callbacks = ScriptMessages::describeReceivers(scene, entity, PhysicsCallbacks::ALL_3D | PhysicsCallbacks::ALL_2D);
    if (!hasBody && callbacks.empty()) return out;
    if (hasBody) {
        Json list = Json::array();
        for (const ContactSummary& contact : contacts) {
            Json item = {{"other", contact.other}, {"collider", contact.collider}, {"other_collider", contact.otherCollider},
                         {"trigger", contact.trigger}, {"asleep", contact.asleep}};
            if (scene->isEntityCreated(contact.other)) item["other_path"] = EntityPath::of(scene, contact.other);
            item["since_step"] = contact.sinceStep > 0 ? Json(contact.sinceStep) : Json(nullptr);
            list.push_back(item);
        }
        out["contacts"] = list;
    }
    out["callbacks"] = callbacks;
    return out;
}

Json vec3Json(const Vector3& v) {
    return Json::array({v.x, v.y, v.z});
}

Json rigidbody3DReadoutsJson(Scene* scene, Entity entity) {
    RigidbodyComponent& component = scene->getComponent<RigidbodyComponent>(entity);
    if (component.body.IsInvalid()) return Json{{"live", false}};
    Rigidbody body(scene, entity);
    return Json{{"live", true}, {"type", component.type == BodyType::DYNAMIC ? "dynamic" : (component.type == BodyType::KINEMATIC ? "kinematic" : "static")},
                {"mass", body.getMass()}, {"center_of_mass_world", vec3Json(body.getCenterOfMassPosition())},
                {"linear_velocity", vec3Json(body.getLinearVelocity())}, {"angular_velocity", vec3Json(body.getAngularVelocity())},
                {"sleeping", body.isSleeping()}, {"colliders", component.colliders.size()}};
}

Json joint3DReadoutsJson(Scene* scene, Entity entity) {
    Joint3D joint(scene, entity);
    const Joint3DComponent& component = scene->getComponent<Joint3DComponent>(entity);
    Json out = {{"live", joint.isLive()}, {"broken", joint.isBroken()}};
    if (!joint.isLive()) return out;
    out["current_force"] = vec3Json(joint.getCurrentForce());
    out["current_torque"] = vec3Json(joint.getCurrentTorque());
    if (component.type == Joint3DType::HINGE) {
        out["angle"] = joint.getAngle();
        out["velocity"] = joint.getVelocity();
    } else if (component.type == Joint3DType::PRISMATIC) {
        out["position"] = joint.getPosition();
        out["speed"] = joint.getSpeed();
    } else if (component.type == Joint3DType::DISTANCE) {
        out["current_length"] = joint.getCurrentLength();
    }
    return out;
}

void getRuntimeEntityTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    std::string error;
    SceneProject* sp = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    if (!args.contains("entity")) {
        call->fail("entity is required (id or path); list_runtime_entities lists them.");
        return;
    }
    const Entity entity = w3::resolveLiveEntity(project, sp, args["entity"], error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    std::set<ComponentType> only;
    if (args.contains("components") && args["components"].is_array()) {
        for (const Json& name : args["components"]) {
            ComponentType type;
            if (!McpRefs::resolveComponent(name, type, error)) {
                call->fail(error);
                return;
            }
            only.insert(type);
        }
    }
    std::set<std::string> onlyProps;
    if (args.contains("properties") && args["properties"].is_array()) {
        for (const Json& name : args["properties"]) if (name.is_string()) onlyProps.insert(name.get<std::string>());
    }

    Json components = Json::object();
    for (ComponentType type : Catalog::findComponents(sp->scene, entity)) {
        if (!only.empty() && !only.count(type)) continue;
        Json props = Json::object();
        McpValues::ReadContext ctx;
        ctx.project = project;
        ctx.scene = sp;
        ctx.entity = entity;
        ctx.component = type;
        if (type != ComponentType::ScriptComponent || !onlyProps.empty() || !only.empty()) {
            for (const auto& [name, property] : Catalog::findEntityProperties(sp->scene, entity, type)) {
                if (!onlyProps.empty() && !onlyProps.count(name)) continue;
                try {
                    // context reads give enum names and resolvable entity refs; custom values stay
                    // out of this listing (they can be large)
                    props[name] = property.type == PropertyType::Custom ? McpValues::toJson(project, type, name, property)
                                                                         : McpValues::readProperty(ctx, name, property);
                } catch (const std::exception&) {
                    props[name] = "<unreadable>";
                }
            }
            addRuntimeState(sp->scene, entity, type, onlyProps, props);
        }
        if (type == ComponentType::CameraShakerComponent) {
            props["runtime"] = shakerRuntimeJson(sp->scene->getComponent<CameraShakerComponent>(entity));
        } else if (type == ComponentType::CameraFollowComponent) {
            props["runtime"] = followRuntimeJson(sp->scene->getComponent<CameraFollowComponent>(entity));
        }
        components[componentNameOf(type)] = props;
    }

    const std::vector<Entity> created = project->getPlayCreatedEntities(sp);
    Json data = McpRefs::entityJson(sp, entity);
    data["path"] = EntityPath::of(sp->scene, entity);
    data["scene"] = McpRefs::sceneJson(project, *sp);
    data["created_at_runtime"] = std::find(created.begin(), created.end(), entity) != created.end();
    // activeSelf / activeInHierarchy (what a clip's gameobject.active keys or a script set)
    data["active"] = sp->scene->isEntityActive(entity);
    data["active_in_hierarchy"] = sp->scene->isEntityActiveInHierarchy(entity);
    // in the DontDestroyOnLoad scene, or marked to move there
    if (project->isPersistentPlayScene(sp->id) || SceneManager::isPersistent(sp->scene, entity)) data["persistent"] = true;
    data["components"] = components;
    // a fluid's runtime state (read only; FluidStats, the matching preset)
    if (only.empty() || only.count(ComponentType::FluidSimulationComponent)) {
        const Json fluid = McpValues::fluidStatsToJson(sp->scene, entity);
        if (!fluid.is_null()) data["fluid_stats"] = fluid;
    }
    // the Canvas Groups over a UI element (inherited alpha, interactable, click-through)
    const Json groupState = McpValues::canvasGroupStateToJson(sp->scene, entity);
    if (!groupState.is_null()) data["canvas_group_state"] = groupState;
    // a Video Player's playback
    if (only.empty() || only.count(ComponentType::VideoPlayerComponent)) {
        const Json video = McpValues::videoStatusToJson(sp->scene, entity);
        if (!video.is_null()) data["video"] = video;
    }
    // physics read-outs (joint forces and angles, body mass and velocities)
    if ((only.empty() || only.count(ComponentType::Joint2DComponent)) && sp->scene->findComponent<Joint2DComponent>(entity)) {
        data["joint2d"] = joint2DReadoutsJson(sp->scene, entity);
    }
    if ((only.empty() || only.count(ComponentType::Rigidbody2DComponent)) && sp->scene->findComponent<Rigidbody2DComponent>(entity)) {
        data["rigidbody2d"] = rigidbody2DReadoutsJson(sp->scene, entity);
    }
    if ((only.empty() || only.count(ComponentType::Joint3DComponent)) && sp->scene->findComponent<Joint3DComponent>(entity)) {
        data["joint3d"] = joint3DReadoutsJson(sp->scene, entity);
    }
    if ((only.empty() || only.count(ComponentType::RigidbodyComponent)) && sp->scene->findComponent<RigidbodyComponent>(entity)) {
        data["rigidbody"] = rigidbody3DReadoutsJson(sp->scene, entity);
    }
    // physics-contacts: what the body touches now and the contact callbacks its scripts define
    {
        const Json physics = physicsContactsJson(sp, entity);
        if (!physics.empty()) data["physics"] = physics;
    }
    if (args.value("include_script_state", true)) {
        data["scripts"] = scriptStateJson(project, sp, entity, std::clamp(args.value("max_depth", 2), 1, 8));
    }
    std::string text = "Live GameObject " + std::to_string(entity) + " \"" + data["path"].get<std::string>() + "\" in \"" +
                       sp->name + "\": " + data["components"].dump();
    if (data.contains("scripts") && !data["scripts"].empty()) text += "\nscripts: " + data["scripts"].dump();
    if (data.contains("fluid_stats")) text += "\nfluid_stats: " + data["fluid_stats"].dump();
    if (data.contains("canvas_group_state")) text += "\ncanvas_group_state: " + data["canvas_group_state"].dump();
    if (data.contains("video")) text += "\nvideo: " + data["video"].dump();
    if (data.contains("joint2d")) text += "\njoint2d: " + data["joint2d"].dump();
    if (data.contains("rigidbody2d")) text += "\nrigidbody2d: " + data["rigidbody2d"].dump();
    if (data.contains("physics")) text += "\nphysics: " + data["physics"].dump();
    if (data.contains("joint3d")) text += "\njoint3d: " + data["joint3d"].dump();
    if (data.contains("rigidbody")) text += "\nrigidbody: " + data["rigidbody"].dump();
    if (text.size() > 12000) text = text.substr(0, 12000) + " ... (see structuredContent)";
    call->ok(text, data);
}

void setRuntimePropertyTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    std::string error;
    SceneProject* sp = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error + " (set_runtime_property changes the running game; for edit-time values use the component tools.)");
        return;
    }
    if (!args.contains("entity") || !args.contains("component")) {
        call->fail("entity and component are required, plus property + value or values {name: value}.");
        return;
    }
    const Entity entity = w3::resolveLiveEntity(project, sp, args["entity"], error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    ComponentType component;
    if (!McpRefs::resolveComponent(args["component"], component, error)) {
        call->fail(error);
        return;
    }
    const std::vector<ComponentType> has = Catalog::findComponents(sp->scene, entity);
    if (std::find(has.begin(), has.end(), component) == has.end()) {
        std::string names;
        for (ComponentType type : has) names += " " + componentNameOf(type);
        call->fail("The GameObject has no " + componentNameOf(component) + " component. It has:" + names + ".");
        return;
    }
    Json values = Json::object();
    if (args.contains("values") && args["values"].is_object()) values = args["values"];
    if (args.contains("property")) {
        if (!args.contains("value")) {
            call->fail("property needs a value.");
            return;
        }
        values[args.value("property", "")] = args["value"];
    }
    if (values.empty()) {
        call->fail("Pass property + value, or values {name: value}.");
        return;
    }

    // a Video Player's playback (spec 6.10): state play | pause | stop | prepare, time (seconds,
    // seeks), frame (seeks to it); its fields go on below as any component's
    if (component == ComponentType::VideoPlayerComponent) {
        std::shared_ptr<VideoPlayerSystem> videos = sp->scene->findSystem<VideoPlayerSystem>();
        if (!videos) {
            call->fail("The scene has no Video Player system.");
            return;
        }
        const bool seeks = values.contains("time") || values.contains("frame");
        if (values.contains("state")) {
            const Json& value = values["state"];
            const std::string state = value.is_string() ? value.get<std::string>() : std::string();
            if (state == "play") videos->play(entity);
            else if (state == "pause") videos->pause(entity);
            else if (state == "stop") videos->stop(entity);
            else if (state == "prepare") videos->prepare(entity);
            else {
                call->fail("state must be play, pause, stop or prepare.");
                return;
            }
            values.erase("state");
        }
        if (seeks && !videos->getStatus(entity).prepared) {
            call->fail("time and frame seek a prepared player: state prepare (or play), then wait until video.prepared.");
            return;
        }
        if (values.contains("time")) {
            if (!values["time"].is_number() || values["time"].get<double>() < 0.0) {
                call->fail("time must be a number of seconds >= 0.");
                return;
            }
            videos->setTime(entity, values["time"].get<double>());
            values.erase("time");
        }
        if (values.contains("frame")) {
            if (!values["frame"].is_number_integer() || values["frame"].get<int64_t>() < 0) {
                call->fail("frame must be an integer >= 0.");
                return;
            }
            videos->setFrame(entity, values["frame"].get<int64_t>());
            values.erase("frame");
        }
        if (values.empty()) {
            Json data = McpRefs::entityJson(sp, entity);
            data["component"] = componentNameOf(component);
            const Json video = McpValues::videoStatusToJson(sp->scene, entity);
            data["video"] = video;
            call->ok("Video Player of \"" + data.value("path", std::string()) + "\": " + video.dump() +
                     " (the next frames show it; get_runtime_entity reads it again).", data);
            return;
        }
    }

    const std::map<std::string, PropertyData> properties = Catalog::findEntityProperties(sp->scene, entity, component);
    auto multi = new MultiPropertyCmd();
    std::vector<std::string> names;
    for (auto it = values.begin(); it != values.end(); ++it) {
        auto prop = properties.find(it.key());
        if (prop == properties.end()) {
            delete multi;
            std::string valid;
            for (const auto& [name, data] : properties) valid += " " + name;
            call->fail("No property \"" + it.key() + "\" on " + componentNameOf(component) + ". Properties:" + valid + ".");
            return;
        }
        Command* cmd = McpValues::buildPropertyCommand(project, sp->id, entity, component, it.key(), prop->second,
                                                       it.value(), error);
        if (!cmd) {
            delete multi;
            call->fail(error);
            return;
        }
        multi->addCommand(std::unique_ptr<Command>(cmd));
        names.push_back(it.key());
    }
    multi->setNoMerge();
    if (!CommandHandle::get(sp->id)->addCommand(multi)) {
        call->fail("The change was rejected by the editor.");
        return;
    }
    sp->needUpdateRender = true;

    Json after = Json::object();
    McpValues::ReadContext readCtx;
    readCtx.project = project;
    readCtx.scene = sp;
    readCtx.entity = entity;
    readCtx.component = component;
    for (const auto& [name, property] : Catalog::findEntityProperties(sp->scene, entity, component)) {
        if (values.contains(name)) after[name] = McpValues::readProperty(readCtx, name, property);
    }
    Json data = McpRefs::entityJson(sp, entity);
    data["component"] = componentNameOf(component);
    data["values"] = after;
    call->ok("Set " + componentNameOf(component) + " " + after.dump() + " on the live GameObject \"" +
             data.value("path", std::string()) + "\". Play-time change: reverted when play stops.", data);
}

// ================================================================== tool: send_input

struct InputStep {
    std::function<std::string()> run;   // returns an error ("" = ok)
    int framesAfter = 0;
    int msAfter = 0;
    std::string label;
};

// A sequence of input steps, advanced one frame at a time by the call running it
// (send_input, or wait with input)
struct InputRun {
    CallPtr call;
    std::vector<InputStep> steps;
    size_t next = 0;
    uint32_t playId = NULL_PROJECT_SCENE;
    uint64_t cursor = 0;
    int frames = 0;
    Clock::time_point start;
    std::string error;
    int pendingFrames = 0;          // game frames to let pass before the next step
    uint64_t gameFrame = 0;         // Engine::getGameFrames() when the run last advanced
    bool msPending = false;
    Clock::time_point msDeadline;
    bool finished = false;
    HeldInput heldBefore;           // what was held before the run (cutInputShort)
    std::shared_ptr<Advance> advance = std::make_shared<Advance>();
    std::shared_ptr<Json> targets = std::make_shared<Json>(Json::array());   // resolved target / to_target points
};

// Coordinates (canvas / normalized / view) → view pixels; error when outside the game canvas
bool toViewPoint(const std::string& space, double x, double y, Vector2& out, std::string& error) {
    const Rect view = Engine::getViewRect();
    const double canvasW = Engine::getCanvasWidth();
    const double canvasH = Engine::getCanvasHeight();
    if (view.getWidth() <= 0 || view.getHeight() <= 0 || canvasW <= 0 || canvasH <= 0) {
        error = "The game view has no size yet.";
        return false;
    }
    double cx = x, cy = y;
    if (space == "normalized") {
        cx = x * canvasW;
        cy = y * canvasH;
    } else if (space == "view") {
        cx = (x - view.getX()) / view.getWidth() * canvasW;
        cy = (y - view.getY()) / view.getHeight() * canvasH;
    }
    if (!Engine::isAllowEventsOutCanvas() && (cx < 0 || cy < 0 || cx > canvasW || cy > canvasH)) {
        error = "Point (" + std::to_string(x) + ", " + std::to_string(y) + ") in " + space + " space is outside the game canvas (" +
                std::to_string((int)canvasW) + "x" + std::to_string((int)canvasH) + "); the game would ignore it.";
        return false;
    }
    out = Vector2(static_cast<float>(view.getX() + cx / canvasW * view.getWidth()),
                  static_cast<float>(view.getY() + cy / canvasH * view.getHeight()));
    return true;
}

// Where a mouse event without x,y happens: the last injected pointer position, else the game's
// current mouse position (view pixels)
Vector2 currentPointerView() {
    const HeldInput& held = state().held;
    if (held.mousePosKnown) return held.mousePos;
    const Vector2 canvas = Input::getMousePosition();
    Vector2 p;
    std::string error;
    if (toViewPoint("canvas", canvas.x, canvas.y, p, error)) return p;
    return held.mousePos;
}

void markPointerInjected(McpContext& ctx) {
    if (PlayInput* input = ctx.playInput()) input->setPointerInjected(true);
}

// UTF-8 → code points
std::vector<uint32_t> decodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (size_t k = 1; k < len && i + k < text.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

bool readPoint(const Json& event, const char* xKey, const char* yKey, const char* arrayKey, double& x, double& y) {
    if (event.contains(arrayKey) && event[arrayKey].is_array() && event[arrayKey].size() >= 2 &&
        event[arrayKey][0].is_number() && event[arrayKey][1].is_number()) {
        x = event[arrayKey][0].get<double>();
        y = event[arrayKey][1].get<double>();
        return true;
    }
    if (event.contains(xKey) && event.contains(yKey) && event[xKey].is_number() && event[yKey].is_number()) {
        x = event[xKey].get<double>();
        y = event[yKey].get<double>();
        return true;
    }
    return false;
}

// A point of a mouse / touch event: x, y in the event's space, or `target`, the center of a UI
// Document element (htmlui_mcp::elementCenter, canvas px) found when the step runs, so an element
// an earlier event showed can be targeted
struct EventPoint {
    bool given = false;
    double x = 0, y = 0;
    std::string target;
    Json document;   // the UI Document to search (null: every one, topmost first)
};

// {xKey, yKey} / arrayKey [x,y] / targetKey; false + error when the target is not a selector
bool readEventPoint(const Json& event, const char* xKey, const char* yKey, const char* arrayKey, const char* targetKey,
                    EventPoint& out, std::string& error) {
    if (event.contains(targetKey) && !event[targetKey].is_null()) {
        if (!event[targetKey].is_string() || event[targetKey].get<std::string>().empty()) {
            error = std::string(targetKey) + " must be a CSS selector of a UI Document element";
            return false;
        }
        out.target = event[targetKey].get<std::string>();
        out.document = event.contains("document") ? event["document"] : Json();
        out.given = true;
        return true;
    }
    out.given = readPoint(event, xKey, yKey, arrayKey, out.x, out.y);
    return true;
}

// The point in view pixels ("" or the error); a resolved target is reported in `targets`
std::string resolveEventPoint(McpContext& ctx, const EventPoint& point, const std::string& space, size_t eventIndex,
                              const std::shared_ptr<Json>& targets, Vector2& out) {
    std::string error;
    if (point.target.empty()) {
        return toViewPoint(space, point.x, point.y, out, error) ? std::string() : error;
    }
    double x = 0, y = 0;
    Json document;
    if (!htmlui_mcp::elementCenter(ctx, point.document, point.target, x, y, error, &document) ||
        !toViewPoint("canvas", x, y, out, error)) {
        return "target \"" + point.target + "\": " + error;
    }
    if (targets) {
        targets->push_back({{"event", eventIndex}, {"target", point.target},
                            {"point", Json::array({std::round(x * 10.0) / 10.0, std::round(y * 10.0) / 10.0})},
                            {"document", document}});
    }
    return std::string();
}

// Turns one event into steps; false + error for a malformed event. Resolved targets go to `targets`.
bool buildEventSteps(McpContext& ctx, const Json& event, const std::string& defaultSpace, int gapFrames,
                     std::vector<InputStep>& steps, std::string& error, size_t eventIndex = 0,
                     const std::shared_ptr<Json>& targets = nullptr) {
    if (!event.is_object()) {
        error = "each event must be an object with a type";
        return false;
    }
    const std::string type = lowerCopy(event.value("type", ""));
    const std::string space = lowerCopy(event.value("space", defaultSpace));
    if (space != "canvas" && space != "normalized" && space != "view") {
        error = "space must be canvas, normalized or view";
        return false;
    }
    // 0: the press and its release in the same frame (a tap Input.getKeyDown and getKeyUp both see)
    int holdFrames = std::clamp(event.value("hold_frames", 2), 0, 100000);
    const int holdMs = std::clamp(event.value("hold_ms", 0), 0, 600000);
    const int mods = parseMods(event.value("mods", Json::array()));
    McpContext* c = &ctx;
    const bool touchMode = ctx.project()->isSimulateTouch();

    auto add = [&](std::function<std::string()> fn, int framesAfter, int msAfter, std::string label) {
        steps.push_back({std::move(fn), framesAfter, msAfter, std::move(label)});
    };

    if (type == "wait") {
        const int frames = std::clamp(event.value("frames", 0), 0, 1000000);
        const int ms = std::clamp(event.value("ms", 0), 0, 600000);
        if (frames == 0 && ms == 0) {
            error = "wait needs frames or ms";
            return false;
        }
        add([]() { return std::string(); }, frames, ms, "wait");
        return true;
    }

    if (type == "key") {
        const Json keyValue = event.contains("key") ? event["key"] : Json();
        const int key = parseKeyName(keyValue);
        if (key == D_KEY_UNKNOWN) {
            error = "unknown key " + keyValue.dump() + " (names like space, enter, escape, a-z, 0-9, left, right, up, down, "
                    "shift, ctrl, alt, tab, backspace, f1-f12, kp_0-kp_9)";
            return false;
        }
        const std::string action = lowerCopy(event.value("action", "press"));
        auto down = [key, mods]() {
            Engine::systemKeyDown(key, false, mods);
            state().held.keys.insert(key);
            return std::string();
        };
        auto up = [key, mods]() {
            Engine::systemKeyUp(key, false, mods);
            state().held.keys.erase(key);
            return std::string();
        };
        if (action == "down") add(down, gapFrames, 0, "key down " + keyName(key));
        else if (action == "up") add(up, gapFrames, 0, "key up " + keyName(key));
        else if (action == "press" || action == "tap") {
            add(down, holdFrames, holdMs, "key down " + keyName(key));
            add(up, gapFrames, 0, "key up " + keyName(key));
        } else {
            error = "key action must be press, down or up";
            return false;
        }
        return true;
    }

    if (type == "text") {
        const std::string text = event.value("text", "");
        if (text.empty()) {
            error = "text event needs text";
            return false;
        }
        // the whole string at once: documents get it as one text input, the game and the
        // component UI each code point (surrogate pairs where wchar_t is 16 bits)
        add([text]() {
            Engine::systemTextInput(text);
            return std::string();
        }, gapFrames, 0, "text");
        return true;
    }

    // IME composition of the focused HTML UI text field (cursor in code points, -1 selects it)
    if (type == "ime") {
        const std::string text = event.value("text", "");
        const int cursor = event.contains("cursor") && event["cursor"].is_number_integer()
            ? event["cursor"].get<int>() : static_cast<int>(decodeUtf8(text).size());
        add([text, cursor]() {
            Engine::systemImeComposition(text, cursor);
            return std::string();
        }, gapFrames, 0, "ime");
        return true;
    }

    if (type == "ime_commit") {
        const std::string text = event.value("text", "");
        add([text]() {
            if (text.empty()) Engine::systemImeCancel();
            else Engine::systemTextInput(text);
            return std::string();
        }, gapFrames, 0, "ime commit");
        return true;
    }

    if (type == "ime_cancel") {
        add([]() {
            Engine::systemImeCancel();
            return std::string();
        }, gapFrames, 0, "ime cancel");
        return true;
    }

    if (type == "mouse_leave" || type == "mouse_enter") {
        const bool leave = type == "mouse_leave";
        add([c, leave]() {
            markPointerInjected(*c);
            if (leave) Engine::systemMouseLeave();
            else Engine::systemMouseEnter();
            return std::string();
        }, gapFrames, 0, type);
        return true;
    }

    if (type == "mouse") {
        const std::string action = lowerCopy(event.value("action", "click"));
        const int button = parseMouseButton(event.contains("button") ? event["button"] : Json());
        if (button < 0) {
            error = "button must be left, right, middle or button4..button8";
            return false;
        }
        EventPoint from, to;
        if (!readEventPoint(event, "x", "y", "position", "target", from, error) ||
            !readEventPoint(event, "to_x", "to_y", "to", "to_target", to, error)) {
            return false;
        }
        const bool hasPoint = from.given;
        // a move without a point and with dx, dy goes that far from the pointer (mouse-look)
        const bool relativeMove = action == "move" && !hasPoint && (event.contains("dx") || event.contains("dy"));
        // down / up / click without a point: where the pointer is
        if (!hasPoint && !relativeMove && (action == "move" || action == "drag")) {
            error = "mouse " + action + " needs x and y (or position [x,y], or target), or dx/dy for a move";
            return false;
        }
        if (!to.target.empty() && action != "drag") {
            error = "to_target is for mouse drag and touch swipe";
            return false;
        }
        // Simulate Touch: the left button is a finger, as in the editor
        const bool asTouch = touchMode && button == D_MOUSE_BUTTON_LEFT;
        auto resolve = [c, space, eventIndex, targets](const EventPoint& point, Vector2& out) {
            return resolveEventPoint(*c, point, space, eventIndex, targets, out);
        };
        auto moveTo = [c, mods, asTouch](Vector2 p) -> std::string {
            markPointerInjected(*c);
            HeldInput& held = state().held;
            if (asTouch) {
                if (held.touches.count(0)) {
                    Engine::systemTouchMove(0, p.x, p.y);
                    held.touches[0] = p;
                }
            } else {
                Engine::systemMouseMove(p.x, p.y, mods);
            }
            held.mousePos = p;
            held.mousePosKnown = true;
            return std::string();
        };
        auto pressAt = [c, mods, button, asTouch](bool known, Vector2 p) -> std::string {
            if (!known) p = currentPointerView();
            markPointerInjected(*c);
            HeldInput& held = state().held;
            if (asTouch) {
                Engine::systemTouchStart(0, p.x, p.y);
                held.touches[0] = p;
            } else {
                Engine::systemMouseMove(p.x, p.y, mods);
                Engine::systemMouseDown(button, p.x, p.y, mods);
                held.mouseButtons.insert(button);
            }
            held.mousePos = p;
            held.mousePosKnown = true;
            return std::string();
        };
        auto releaseAt = [c, mods, button, asTouch](bool known, Vector2 p) -> std::string {
            HeldInput& held = state().held;
            if (!known) p = currentPointerView();
            markPointerInjected(*c);
            if (asTouch) {
                Engine::systemTouchEnd(0, p.x, p.y);
                held.touches.erase(0);
            } else {
                Engine::systemMouseUp(button, p.x, p.y, mods);
                held.mouseButtons.erase(button);
            }
            held.mousePos = p;
            held.mousePosKnown = true;
            return std::string();
        };
        // a point resolved once per event: a click's down and up happen at the same place
        auto at = std::make_shared<Vector2>();

        if (action == "move" && relativeMove) {
            const double dx = event.value("dx", 0.0);
            const double dy = event.value("dy", 0.0);
            add([moveTo, dx, dy, space]() {
                const Rect view = Engine::getViewRect();
                const double canvasW = std::max(1, Engine::getCanvasWidth());
                const double canvasH = std::max(1, Engine::getCanvasHeight());
                // the event's space to view pixels
                double sx = 1.0, sy = 1.0;
                if (space == "canvas") {
                    sx = view.getWidth() / canvasW;
                    sy = view.getHeight() / canvasH;
                } else if (space == "normalized") {
                    sx = view.getWidth();
                    sy = view.getHeight();
                }
                const Vector2 from = currentPointerView();
                return moveTo(Vector2(static_cast<float>(from.x + dx * sx), static_cast<float>(from.y + dy * sy)));
            }, gapFrames, 0, "mouse move");
        } else if (action == "move") {
            add([resolve, moveTo, from]() {
                Vector2 p;
                const std::string err = resolve(from, p);
                return err.empty() ? moveTo(p) : err;
            }, gapFrames, 0, "mouse move");
        } else if (action == "down" || action == "up") {
            const bool down = action == "down";
            add([resolve, pressAt, releaseAt, from, hasPoint, down]() {
                Vector2 p;
                if (hasPoint) {
                    const std::string err = resolve(from, p);
                    if (!err.empty()) return err;
                }
                return down ? pressAt(hasPoint, p) : releaseAt(hasPoint, p);
            }, gapFrames, 0, down ? "mouse down" : "mouse up");
        } else if (action == "click" || action == "double_click") {
            const int clicks = action == "double_click" ? 2 : 1;
            for (int i = 0; i < clicks; i++) {
                add([resolve, pressAt, from, hasPoint, at, i]() {
                    if (hasPoint && i == 0) {
                        const std::string err = resolve(from, *at);
                        if (!err.empty()) return err;
                    }
                    return pressAt(hasPoint, *at);
                }, holdFrames, holdMs, "mouse down");
                add([releaseAt, hasPoint, at]() { return releaseAt(hasPoint, *at); }, gapFrames, 0, "mouse up");
            }
        } else if (action == "drag") {
            if (!to.given) {
                error = "mouse drag needs to: [x,y] (or to_x, to_y, or to_target)";
                return false;
            }
            const int stepsCount = std::clamp(event.value("steps", 10), 1, 1000);
            // both ends are resolved when the drag starts (before it moves anything)
            auto path = std::make_shared<std::pair<Vector2, Vector2>>();
            add([resolve, pressAt, from, to, path]() {
                std::string err = resolve(from, path->first);
                if (err.empty()) err = resolve(to, path->second);
                return err.empty() ? pressAt(true, path->first) : err;
            }, 1, 0, "drag start");
            for (int i = 1; i <= stepsCount; i++) {
                const float t = float(i) / stepsCount;
                add([moveTo, path, t]() {
                    const Vector2 a = path->first, b = path->second;
                    return moveTo(Vector2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
                }, 1, 0, "drag move");
            }
            add([releaseAt, path]() { return releaseAt(true, path->second); }, gapFrames, 0, "drag end");
        } else if (action == "scroll") {
            const float dx = event.value("dx", 0.0f);
            const float dy = event.value("dy", event.value("amount", 1.0f));
            add([resolve, hasPoint, moveTo, from, dx, dy, mods]() {
                if (hasPoint) {
                    Vector2 p;
                    std::string err = resolve(from, p);
                    if (err.empty()) err = moveTo(p);
                    if (!err.empty()) return err;
                }
                Engine::systemMouseScroll(dx, dy, mods);
                return std::string();
            }, gapFrames, 0, "scroll");
        } else {
            error = "mouse action must be click, double_click, down, up, move, drag or scroll";
            return false;
        }
        return true;
    }

    if (type == "touch") {
        const std::string action = lowerCopy(event.value("action", "tap"));
        const int pointer = std::clamp(event.value("pointer", 0), 0, 9);
        EventPoint from, to;
        if (!readEventPoint(event, "x", "y", "position", "target", from, error) ||
            !readEventPoint(event, "to_x", "to_y", "to", "to_target", to, error)) {
            return false;
        }
        const bool hasPoint = from.given;
        if (!hasPoint && action != "end" && action != "cancel") {
            error = "touch " + action + " needs x and y (or target)";
            return false;
        }
        if (!to.target.empty() && action != "swipe") {
            error = "to_target is for mouse drag and touch swipe";
            return false;
        }
        auto resolve = [c, space, eventIndex, targets](const EventPoint& point, Vector2& out) {
            return resolveEventPoint(*c, point, space, eventIndex, targets, out);
        };
        auto start = [c, pointer](Vector2 p) -> std::string {
            markPointerInjected(*c);
            Engine::systemTouchStart(pointer, p.x, p.y);
            state().held.touches[pointer] = p;
            return std::string();
        };
        auto move = [c, pointer](Vector2 p) -> std::string {
            markPointerInjected(*c);
            Engine::systemTouchMove(pointer, p.x, p.y);
            state().held.touches[pointer] = p;
            return std::string();
        };
        auto end = [c, pointer](bool known, Vector2 p) -> std::string {
            HeldInput& held = state().held;
            if (!known) p = held.touches.count(pointer) ? held.touches[pointer] : held.mousePos;
            markPointerInjected(*c);
            Engine::systemTouchEnd(pointer, p.x, p.y);
            held.touches.erase(pointer);
            return std::string();
        };
        auto at = std::make_shared<Vector2>();
        if (action == "start" || action == "move") {
            const bool begin = action == "start";
            add([resolve, start, move, from, begin]() {
                Vector2 p;
                const std::string err = resolve(from, p);
                if (!err.empty()) return err;
                return begin ? start(p) : move(p);
            }, gapFrames, 0, begin ? "touch start" : "touch move");
        } else if (action == "end") {
            add([resolve, end, from, hasPoint]() {
                Vector2 p;
                if (hasPoint) {
                    const std::string err = resolve(from, p);
                    if (!err.empty()) return err;
                }
                return end(hasPoint, p);
            }, gapFrames, 0, "touch end");
        } else if (action == "cancel") {
            add([pointer]() {
                Engine::systemTouchCancel(pointer);
                state().held.touches.erase(pointer);
                return std::string();
            }, gapFrames, 0, "touch cancel");
        } else if (action == "tap") {
            add([resolve, start, from, at]() {
                const std::string err = resolve(from, *at);
                return err.empty() ? start(*at) : err;
            }, holdFrames, holdMs, "touch start");
            add([end, at]() { return end(true, *at); }, gapFrames, 0, "touch end");
        } else if (action == "swipe") {
            if (!to.given) {
                error = "touch swipe needs to: [x,y] (or to_target)";
                return false;
            }
            const int stepsCount = std::clamp(event.value("steps", 10), 1, 1000);
            auto path = std::make_shared<std::pair<Vector2, Vector2>>();
            add([resolve, start, from, to, path]() {
                std::string err = resolve(from, path->first);
                if (err.empty()) err = resolve(to, path->second);
                return err.empty() ? start(path->first) : err;
            }, 1, 0, "swipe start");
            for (int i = 1; i <= stepsCount; i++) {
                const float t = float(i) / stepsCount;
                add([move, path, t]() {
                    const Vector2 a = path->first, b = path->second;
                    return move(Vector2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
                }, 1, 0, "swipe move");
            }
            add([end, path]() { return end(true, path->second); }, gapFrames, 0, "swipe end");
        } else {
            error = "touch action must be tap, start, move, end, cancel or swipe";
            return false;
        }
        return true;
    }

    if (type == "gamepad") {
        const int pad = std::clamp(event.value("gamepad", 0), 0, 15);
        const std::string action = lowerCopy(event.value("action", event.contains("axis") ? "axis" : "press"));
        auto connect = [pad]() {
            HeldInput& held = state().held;
            if (!held.gamepads.count(pad)) {
                Engine::systemGamepadConnect(pad, "MCP Virtual Gamepad");
                held.gamepads.insert(pad);
            }
        };
        if (action == "connect") {
            add([connect]() { connect(); return std::string(); }, gapFrames, 0, "gamepad connect");
            return true;
        }
        if (action == "disconnect") {
            add([pad]() {
                HeldInput& held = state().held;
                Engine::systemGamepadDisconnect(pad);
                held.gamepads.erase(pad);
                held.gamepadButtons.erase(pad);
                held.gamepadAxes.erase(pad);
                return std::string();
            }, gapFrames, 0, "gamepad disconnect");
            return true;
        }
        if (action == "axis") {
            const int axis = parseGamepadAxis(event.contains("axis") ? event["axis"] : Json());
            if (axis < 0) {
                error = "axis must be left_x, left_y, right_x, right_y, left_trigger or right_trigger";
                return false;
            }
            const float value = std::clamp(event.value("value", 0.0f), -1.0f, 1.0f);
            add([connect, pad, axis, value]() {
                connect();
                Engine::systemGamepadAxisMove(pad, axis, value);
                if (value != 0.0f) state().held.gamepadAxes[pad].insert(axis);
                else state().held.gamepadAxes[pad].erase(axis);
                return std::string();
            }, gapFrames, 0, "gamepad axis");
            return true;
        }
        const int button = parseGamepadButton(event.contains("button") ? event["button"] : Json());
        if (button < 0) {
            error = "gamepad button must be a, b, x, y, left_bumper, right_bumper, back, start, guide, left_thumb, "
                    "right_thumb, dpad_up, dpad_right, dpad_down, dpad_left";
            return false;
        }
        auto down = [connect, pad, button]() {
            connect();
            Engine::systemGamepadButtonDown(pad, button);
            state().held.gamepadButtons[pad].insert(button);
            return std::string();
        };
        auto up = [pad, button]() {
            Engine::systemGamepadButtonUp(pad, button);
            state().held.gamepadButtons[pad].erase(button);
            return std::string();
        };
        if (action == "down") add(down, gapFrames, 0, "gamepad down");
        else if (action == "up") add(up, gapFrames, 0, "gamepad up");
        else if (action == "press") {
            add(down, holdFrames, holdMs, "gamepad down");
            add(up, gapFrames, 0, "gamepad up");
        } else {
            error = "gamepad action must be press, down, up, axis, connect or disconnect";
            return false;
        }
        return true;
    }

    if (type == "action" || type == "axis" || type == "axis_delta") {
        // the Input Manager's actions (a flat name or Map/Action), through the virtual source
        // "mcp" (as an on-screen control)
        const Json nameValue = event.contains("name") ? event["name"] : (event.contains("action") ? event["action"] : Json());
        const std::string name = nameValue.is_string() ? nameValue.get<std::string>() : std::string();
        if (!InputActions::hasAction(name)) {
            std::string list;
            for (const std::string& known : InputActions::getActionNames()) list += (list.empty() ? "" : ", ") + known;
            error = (name.empty() ? std::string("name must be an input action") : "no input action \"" + name + "\"") +
                    " (Project Settings > Input: " + list + ")";
            return false;
        }
        const uintptr_t source = mcpInputSource();
        if (type == "action") {
            const std::string phase = lowerCopy(event.value("phase", "press"));
            auto down = [name, source]() {
                InputActions::setSourceValue(source, name, 1.0f, InputSourceKind::Agent);
                state().held.managerActions.insert(name);
                return std::string();
            };
            auto up = [name, source]() {
                InputActions::setSourceValue(source, name, 0.0f, InputSourceKind::Agent);
                state().held.managerActions.erase(name);
                return std::string();
            };
            if (phase == "down") add(down, gapFrames, 0, "action down " + name);
            else if (phase == "up") add(up, gapFrames, 0, "action up " + name);
            else if (phase == "press" || phase == "tap") {
                add(down, holdFrames, holdMs, "action down " + name);
                add(up, gapFrames, 0, "action up " + name);
            } else {
                error = "action phase must be press, down or up";
                return false;
            }
            return true;
        }
        const char* key = type == "axis" ? "value" : "delta";
        if (!event.contains(key) || !event[key].is_number()) {
            error = std::string(type) + " needs a number " + key;
            return false;
        }
        if (type == "axis_delta") {
            const float delta = event[key].get<float>();
            add([name, source, delta]() {
                InputActions::addSourceDelta(source, name, delta, InputSourceKind::Agent);
                return std::string();
            }, gapFrames, 0, "axis delta " + name);
            return true;
        }
        const float value = std::clamp(event[key].get<float>(), -1.0f, 1.0f);
        auto set = [name, source](float v) {
            InputActions::setSourceValue(source, name, v, InputSourceKind::Agent);
            if (v != 0.0f) state().held.axes[name] = v;
            else state().held.axes.erase(name);
            return std::string();
        };
        // with hold_frames / hold_ms it goes back to 0 after them, else it stays (0 releases)
        if (value != 0.0f && (event.contains("hold_frames") || event.contains("hold_ms"))) {
            add([set, value]() { return set(value); }, holdFrames, holdMs, "axis " + name);
            add([set]() { return set(0.0f); }, gapFrames, 0, "axis release " + name);
        } else {
            add([set, value]() { return set(value); }, gapFrames, 0, "axis " + name);
        }
        return true;
    }

    // an Input Action forced at top priority (InputActions::inject): press for hold_frames
    // windows (0: pressed and released in one), down until up, up, or a value for hold_frames
    if (type == "input_action") {
        const std::string name = event.value("name", "");
        if (name.find('/') == std::string::npos) {
            error = "input_action needs name \"Map/Action\"";
            return false;
        }
        const std::string action = lowerCopy(event.value("action", "press"));
        Vector2 value(1.0f, 0.0f);
        if (event.contains("value")) {
            const Json& v = event["value"];
            if (v.is_number()) value = Vector2(v.get<float>(), 0.0f);
            else if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) value = Vector2(v[0].get<float>(), v[1].get<float>());
            else {
                error = "input_action value is a number or [x, y]";
                return false;
            }
        }
        auto exists = [name]() {
            return InputActions::get(name) ? std::string() : "no Input Action " + name + " is loaded (get_input_state fields actions lists them)";
        };
        if (action == "press" || action == "value") {
            const int windows = holdFrames;
            add([exists, name, value, windows]() {
                const std::string err = exists();
                if (err.empty()) InputActions::inject(name, value, windows);
                return err;
            }, std::max(holdFrames, 0), holdMs, "input action " + action + " " + name);
            add([]() { return std::string(); }, gapFrames, 0, "input action gap");
        } else if (action == "down") {
            add([exists, name, value]() {
                const std::string err = exists();
                if (err.empty()) {
                    InputActions::inject(name, value, -1);
                    state().held.actions.insert(name);
                }
                return err;
            }, gapFrames, 0, "input action down " + name);
        } else if (action == "up") {
            add([exists, name]() {
                const std::string err = exists();
                if (err.empty()) {
                    InputActions::releaseInjected(name);
                    state().held.actions.erase(name);
                }
                return err;
            }, gapFrames, 0, "input action up " + name);
        } else {
            error = "input_action action must be press, down, up or value";
            return false;
        }
        return true;
    }

    if (type == "release_all") {
        add([c]() { releaseHeldInput(*c); return std::string(); }, gapFrames, 0, "release all");
        return true;
    }

    // The app's lifecycle as a device reports it (Engine::systemPause / systemResume /
    // systemFocus / systemLowMemory), or the simulated device turned (configure_game_view device)
    if (type == "app") {
        const std::string action = lowerCopy(event.value("action", ""));
        if (action == "pause" || action == "resume" || action == "focus_lost" || action == "focus_gained" ||
            action == "low_memory") {
            add([action]() {
                if (action == "pause") Engine::systemPause();
                else if (action == "resume") Engine::systemResume();
                else if (action == "focus_lost") Engine::systemFocus(false);
                else if (action == "focus_gained") Engine::systemFocus(true);
                else Engine::systemLowMemory();
                return std::string();
            }, gapFrames, 0, "app " + action);
            return true;
        }
        if (action == "orientation") {
            ScreenOrientation orientation;
            if (!PlatformRules::parseOrientationName(event.value("orientation", ""), orientation) ||
                !PlatformRules::isFixedOrientation(orientation)) {
                error = "app orientation needs orientation: portrait, portrait_upside_down, landscape_left or landscape_right";
                return false;
            }
            add([c, orientation]() {
                const DeviceProfile* device = Platform::getSimulation().device;
                if (!device) return std::string("app orientation needs a simulated device: configure_game_view {device: \"iphone_15_pro\"}");
                GameWindow* gw = c->gameWindow();
                const uint8_t allowed = gw ? gw->getRotatableOrientations() : profileOrientationMask(*device);
                if (!(allowed & PlatformRules::orientationBit(orientation))) {
                    std::string text = std::string(device->name) + " cannot show " + PlatformRules::orientationName(orientation) + " now (allowed:";
                    for (ScreenOrientation o : {ScreenOrientation::Portrait, ScreenOrientation::PortraitUpsideDown,
                                                ScreenOrientation::LandscapeLeft, ScreenOrientation::LandscapeRight}) {
                        if (allowed & PlatformRules::orientationBit(o)) text += std::string(" ") + PlatformRules::orientationName(o);
                    }
                    return text + "; the game sets them with Screen.orientation / Screen.autorotateTo*)";
                }
                Platform::setSimulatedOrientation(orientation);
                return std::string();
            }, gapFrames, 0, "app orientation");
            return true;
        }
        error = "app action must be pause, resume, focus_lost, focus_gained, low_memory or orientation";
        return false;
    }

    error = "type must be key, text, ime, ime_commit, ime_cancel, mouse, mouse_leave, mouse_enter, touch, gamepad, input_action, action, axis, axis_delta, app, wait or release_all";
    return false;
}

// Runs the steps due now: once when the run starts (newFrame false), then once per frame.
// True when every step ran or one failed (error).
bool advanceInput(InputRun& run, bool newFrame) {
    if (run.finished) return true;
    // the game frames that ran since the run last advanced (a call is polled once per editor
    // frame, which need not run exactly one game frame)
    const uint64_t gameFrame = Engine::getGameFrames();
    int elapsed = newFrame ? static_cast<int>(gameFrame - run.gameFrame) : 0;
    run.gameFrame = gameFrame;
    // an app paused by an `app` event does not update (as on a device): the editor's frames
    // drive the steps meanwhile, so a later resume step still runs
    if (newFrame && elapsed == 0 && Engine::isPaused()) elapsed = 1;
    if (run.pendingFrames > 0 || run.msPending) {
        if (elapsed <= 0) return false;
        run.frames += elapsed;
        run.pendingFrames = std::max(run.pendingFrames - elapsed, 0);
        if (run.pendingFrames > 0 || (run.msPending && Clock::now() < run.msDeadline)) return false;
        run.msPending = false;
    }
    while (run.next < run.steps.size()) {
        InputStep& step = run.steps[run.next];
        const std::string error = step.run();
        run.next++;
        if (!error.empty()) {
            run.error = error;
            run.finished = true;
            return true;
        }
        if (step.msAfter > 0 && state().time.fixedFps > 0) {
            // fixed_fps: milliseconds of game frames (each exactly 1/fixed_fps s), not of the wall
            // clock, so the input lasts the same frames however fast the machine runs
            const int msFrames = static_cast<int>(std::ceil(step.msAfter * state().time.fixedFps / 1000.0 - 1e-9));
            run.pendingFrames = std::max({step.framesAfter, msFrames, 1});
            return false;
        }
        if (step.msAfter > 0) {
            run.pendingFrames = std::max(step.framesAfter, 1);
            run.msPending = true;
            run.msDeadline = Clock::now() + std::chrono::milliseconds(step.msAfter);
            return false;
        }
        if (step.framesAfter > 0) {
            run.pendingFrames = step.framesAfter;
            return false;
        }
    }
    run.finished = true;
    return true;
}

// The input can keep running this frame; otherwise sets run.error when the game paused
bool inputPlayable(McpContext& ctx, InputRun& run) {
    Project* project = ctx.project();
    SceneProject* sp = project->getScene(run.playId);
    if (project->getPlaySessionSceneId() != run.playId || !sp || run.call->cancelled()) return false;
    if (sp->playState != ScenePlayState::PLAYING) {
        if (run.error.empty() && sp->playState == ScenePlayState::PAUSED) run.error = "the game was paused";
        return false;
    }
    return true;
}

// A wait ended before its input: drop the remaining steps and release what they pressed
// (what was held before the run stays held)
bool cutInputShort(InputRun& run) {
    if (run.finished) return false;
    run.finished = true;
    if (run.next >= run.steps.size()) return false;   // only the trailing gap was left
    HeldInput& held = state().held;
    const HeldInput& before = run.heldBefore;
    std::vector<int> keys, buttons, touches;
    for (int key : held.keys) if (!before.keys.count(key)) keys.push_back(key);
    for (int key : keys) {
        Engine::systemKeyUp(key, false, 0);
        held.keys.erase(key);
    }
    const Vector2 pos = clampToView(held.mousePos);
    for (int button : held.mouseButtons) if (!before.mouseButtons.count(button)) buttons.push_back(button);
    for (int button : buttons) {
        Engine::systemMouseUp(button, pos.x, pos.y, 0);
        held.mouseButtons.erase(button);
    }
    for (const auto& [pointer, point] : held.touches) if (!before.touches.count(pointer)) touches.push_back(pointer);
    for (int pointer : touches) {
        const Vector2 p = clampToView(held.touches[pointer]);
        Engine::systemTouchEnd(pointer, p.x, p.y);
        held.touches.erase(pointer);
    }
    for (auto& [pad, padButtons] : held.gamepadButtons) {
        std::vector<int> up;
        auto it = before.gamepadButtons.find(pad);
        for (int b : padButtons) if (it == before.gamepadButtons.end() || !it->second.count(b)) up.push_back(b);
        for (int b : up) {
            Engine::systemGamepadButtonUp(pad, b);
            padButtons.erase(b);
        }
    }
    // the input actions it forced down are released
    std::vector<std::string> actions;
    for (const std::string& name : held.actions) if (!before.actions.count(name)) actions.push_back(name);
    for (const std::string& name : actions) {
        InputActions::releaseInjected(name);
        held.actions.erase(name);
    }
    // the Input Manager's actions it pressed are released, the axes it moved go back to where they were
    std::vector<std::string> managerActions;
    for (const std::string& name : held.managerActions) if (!before.managerActions.count(name)) managerActions.push_back(name);
    for (const std::string& name : managerActions) {
        InputActions::setSourceValue(mcpInputSource(), name, 0.0f, InputSourceKind::Agent);
        held.managerActions.erase(name);
    }
    std::vector<std::string> axes;
    for (const auto& [name, value] : held.axes) {
        auto it = before.axes.find(name);
        if (it == before.axes.end() || it->second != value) axes.push_back(name);
    }
    for (const std::string& name : axes) {
        auto it = before.axes.find(name);
        const float value = it == before.axes.end() ? 0.0f : it->second;
        InputActions::setSourceValue(mcpInputSource(), name, value, InputSourceKind::Agent);
        if (value != 0.0f) held.axes[name] = value;
        else held.axes.erase(name);
    }
    return true;
}

// events (+ space, gap_frames) of a call → a run; false after failing the call
std::shared_ptr<InputRun> buildInputRun(const CallPtr& call, const Json& events, const std::string& argName) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    if (!events.is_array() || events.empty()) {
        call->fail(argName + " must be a non-empty array, e.g. [{\"type\":\"key\",\"key\":\"space\"}, "
                   "{\"type\":\"mouse\",\"action\":\"click\",\"x\":640,\"y\":360}].");
        return nullptr;
    }
    if (events.size() > 500) {
        call->fail("At most 500 events per call.");
        return nullptr;
    }
    const std::string space = lowerCopy(args.value("space", "canvas"));
    const int gap = std::clamp(args.value("gap_frames", 1), 0, 1000);
    auto run = std::make_shared<InputRun>();
    run->call = call;
    run->playId = ctx.project()->getPlaySessionSceneId();
    run->cursor = Out::lastLogId();
    run->start = Clock::now();
    run->heldBefore = state().held;
    for (size_t i = 0; i < events.size(); i++) {
        std::string error;
        if (!buildEventSteps(ctx, events[i], space, gap, run->steps, error, i, run->targets)) {
            call->fail(argName + "[" + std::to_string(i) + "]: " + error + ".");
            return nullptr;
        }
    }
    return run;
}

void finishInputRun(const std::shared_ptr<InputRun>& run) {
    run->advance->release();
    const CallPtr& call = run->call;
    McpContext& ctx = call->ctx();
    observeSession(ctx);
    w3::ConsoleQuery query;
    query.since = run->cursor;
    query.limit = 50;
    w3::ConsoleResult console = w3::queryConsole(query);
    Json data = w3::playStateJson(ctx);
    data["events_done"] = run->next;
    data["steps_total"] = run->steps.size();
    data["frames"] = run->frames;
    data["held_input"] = heldInputJson();
    data["console"] = console.entries;
    data["next_cursor"] = console.nextCursor;
    if (!run->targets->empty()) data["targets"] = *run->targets;

    const bool ended = ctx.project()->getPlaySessionSceneId() != run->playId;
    std::string text;
    bool isError = false;
    if (!run->error.empty()) {
        text = "Input stopped at step " + std::to_string(run->next) + ": " + run->error;
        isError = true;
    } else if (ended) {
        text = "Play ended during the input: " + sessionSummaryText(state().last);
        isError = true;
    } else {
        text = "Sent " + std::to_string(run->steps.size()) + " input step(s) over " + std::to_string(run->frames) + " frame(s).";
        if (Engine::isSimulationHeld()) text += " Held again (auto_pause).";
    }
    if (!state().held.empty()) text += " Still held: " + heldInputJson().dump() + " (released at stop or with release_all).";
    for (const Json& target : *run->targets) {
        text += "\nTarget \"" + target["target"].get<std::string>() + "\" (event " + target["event"].dump() + ") at canvas " +
                target["point"].dump() + " in " + target["document"].value("html", std::string("?")) + ".";
    }
    if (!console.entries.empty()) text += "\nConsole:\n" + w3::formatConsoleText(console.entries, 4000);
    replyWithOptionalScreenshot(call, isError ? ToolResult::error(text, data) : ToolResult::success(text, data));
}

void sendInputTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    observeSession(ctx);
    const uint32_t playId = project->getPlaySessionSceneId();
    SceneProject* sp = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
    if (!sp || sp->playState != ScenePlayState::PLAYING) {
        call->fail(sp ? "The game is " + McpRefs::playStateName(sp->playState) + "; input only reaches a playing game (resume it)."
                      : "Nothing is playing: start the game with play {action: \"start\"} first.",
                   w3::playStateJson(ctx));
        return;
    }
    std::shared_ptr<InputRun> run = buildInputRun(call, args.contains("events") ? args["events"] : Json(), "events");
    if (!run) return;
    ensureMonitor(ctx);
    run->advance->acquire(ctx);

    const int after = std::clamp(args.value("frames_after", 1), 0, 100000);
    auto afterLeft = std::make_shared<int>(after);
    advanceInput(*run, false);
    if (run->finished && (!run->error.empty() || after == 0)) {
        finishInputRun(run);
        return;
    }
    // no wall-clock deadline: the steps take the frames (and real-time ms) they ask for, however
    // slowly the game runs; a game that stops advancing ends the run (FrameProgress)
    auto progress = std::make_shared<FrameProgress>();
    call->waitUntil([call, run, afterLeft, progress]() {
        McpContext& c = call->ctx();
        observeSession(c);
        if (!inputPlayable(c, *run)) return true;
        if (!Engine::isPaused() && progress->stalled()) {
            run->error = "the game did not advance for " + std::to_string((int)kStallSeconds) + " s (is it rendering?)";
            return true;
        }
        if (!run->finished) {
            if (!advanceInput(*run, true) || !run->error.empty()) return !run->error.empty();
            return *afterLeft == 0;
        }
        // frames_after: game frames after the last step
        const uint64_t gameFrame = Engine::getGameFrames();
        int elapsed = static_cast<int>(gameFrame - run->gameFrame);
        run->gameFrame = gameFrame;
        if (elapsed == 0 && Engine::isPaused()) elapsed = 1;
        if (*afterLeft > 0 && elapsed > 0) {
            const int counted = std::min(*afterLeft, elapsed);
            *afterLeft -= counted;
            run->frames += counted;
        }
        return *afterLeft == 0;
    }, 0, [run](bool timedOut) {
        if (timedOut && run->error.empty()) run->error = "timed out (is the game rendering?)";
        finishInputRun(run);
    });
}

// ================================================================== tool: wait

void waitTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    observeSession(ctx);
    const double seconds = std::clamp(args.value("seconds", 0.0), 0.0, 600.0);
    const double gameSeconds = std::clamp(args.value("game_seconds", 0.0), 0.0, 3600.0);
    const int frames = std::clamp(args.value("frames", 0), 0, 1000000);
    const std::string until = args.value("until", "");
    const bool hasInput = args.contains("input") && !args["input"].is_null();
    if (seconds <= 0.0 && gameSeconds <= 0.0 && frames <= 0 && until.empty() && !hasInput) {
        call->fail("Pass seconds, game_seconds, frames or until (a Lua condition), e.g. {\"frames\": 60} or "
                   "{\"until\": \"script('GameController').state == 'playing'\", \"timeout_seconds\": 10}; input "
                   "events run during the wait.");
        return;
    }
    // until alone: 30 s of wall clock. A frames / game_seconds cap of a playing game has no
    // wall-clock deadline: the wait runs them however slowly the game runs (under load a fixed_fps
    // game must still reach the same frame), and ends early only when the game stops advancing
    // (FrameProgress). An explicit timeout_seconds is a wall-clock cap.
    const bool frameCapped = gameSeconds > 0.0 || frames > 0;
    const bool capped = frameCapped || seconds > 0.0;
    const bool explicitTimeout = args.contains("timeout_seconds") && args["timeout_seconds"].is_number();
    const int pollEvery = std::clamp(args.value("poll_frames", 1), 1, 600);

    const uint32_t playId = project->getPlaySessionSceneId();
    SceneProject* sp = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
    const bool playing = sp && isRunningState(sp->playState);
    if ((!until.empty() || gameSeconds > 0.0 || hasInput) && !playing) {
        call->fail(std::string(!until.empty() ? "until" : gameSeconds > 0.0 ? "game_seconds" : "input") +
                   " needs the game running (play start first).", w3::playStateJson(ctx));
        return;
    }
    if (hasInput && sp->playState != ScenePlayState::PLAYING) {
        call->fail("The game is paused; input only reaches a playing game (play resume).", w3::playStateJson(ctx));
        return;
    }
    if (!until.empty()) {
        // syntax check up front
        lua_State* L = LuaBinding::getLuaState();
        const std::string expr = "return " + until;
        if (L) {
            const int top = lua_gettop(L);
            if (luaL_loadbufferx(L, expr.c_str(), expr.size(), "=until", "t") != LUA_OK) {
                lua_settop(L, top);
                if (luaL_loadbufferx(L, until.c_str(), until.size(), "=until", "t") != LUA_OK) {
                    const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
                    lua_settop(L, top);
                    call->fail("until is not valid Lua: " + error);
                    return;
                }
            }
            lua_settop(L, top);
        }
    }
    std::shared_ptr<InputRun> input;
    if (hasInput) {
        input = buildInputRun(call, args["input"], "input");
        if (!input) return;
    }

    const bool pausedSteps = sp && sp->playState == ScenePlayState::PAUSED && frames > 0;
    const bool stallGuard = playing && frameCapped && !explicitTimeout;
    double timeout = args.value("timeout_seconds", until.empty() || capped ? 0.0 : 30.0);
    if (timeout <= 0.0) timeout = std::max({seconds + 30.0, frames / 10.0 + 30.0, gameSeconds * 10.0 + 30.0});
    timeout = std::clamp(timeout, 0.5, 3600.0);
    if (playing) ensureMonitor(ctx);

    struct WaitState {
        uint64_t cursor = 0;
        uint64_t serial = 0;
        Clock::time_point start;
        uint64_t engineFrames = 0;
        double engineTime = 0.0;
        int ticks = 0;
        int frames = 0;
        int polls = 0;
        std::string reason;
        Json conditionValue;
        std::string conditionError;
        bool stepping = false;
        bool inputCut = false;
        bool stallGuard = false;
        FrameProgress progress;
        double progressGameTime = 0.0;      // a game_seconds cap: the game time must advance too
        Clock::time_point progressGameAt;
        Advance advance;
    };
    auto ws = std::make_shared<WaitState>();
    ws->cursor = Out::lastLogId();
    ws->serial = state().current.serial;
    ws->start = Clock::now();
    ws->engineFrames = Engine::getGameFrames();
    ws->engineTime = Engine::getGameTime();
    ws->stepping = pausedSteps;
    ws->stallGuard = stallGuard;
    ws->progressGameTime = Engine::getGameTime();
    ws->progressGameAt = ws->start;
    if (playing) ws->advance.acquire(ctx);
    if (pausedSteps) project->step(playId);
    if (input) advanceInput(*input, false);

    call->waitUntil([call, ws, seconds, gameSeconds, frames, until, playId, pollEvery, playing, input]() {
        McpContext& c = call->ctx();
        observeSession(c);
        Project* p = c.project();
        ws->ticks++;
        if (playing && (p->getPlaySessionSceneId() != playId || state().current.serial != ws->serial)) {
            ws->reason = "play_ended";
            return true;
        }
        if (ws->stallGuard) {
            bool stalled = ws->progress.stalled();
            // a game_seconds cap alone also needs the game time to run (time scale 0 stops it)
            if (gameSeconds > 0.0 && frames <= 0) {
                if (Engine::getGameTime() != ws->progressGameTime) {
                    ws->progressGameTime = Engine::getGameTime();
                    ws->progressGameAt = Clock::now();
                } else if (secondsSince(ws->progressGameAt) >= kStallSeconds) {
                    stalled = true;
                }
            }
            if (stalled) {
                ws->reason = "stalled";
                return true;
            }
        }
        if (ws->stepping) {
            // a paused game advances one frame per step
            if (p->isStepPending(playId)) return false;
            ws->frames++;
            if (ws->frames >= frames) {
                ws->reason = "frames";
                return true;
            }
            p->step(playId);
            return false;
        }
        // game frames while it plays; editor frames otherwise
        ws->frames = playing ? static_cast<int>(Engine::getGameFrames() - ws->engineFrames) : ws->ticks;
        // the condition first: with it, seconds / game_seconds / frames are caps
        const bool capReached = (frames > 0 && ws->frames >= frames) || (seconds > 0.0 && secondsSince(ws->start) >= seconds) ||
                                (gameSeconds > 0.0 && Engine::getGameTime() - ws->engineTime >= gameSeconds - 1e-9);
        if (!until.empty() && ((++ws->polls % pollEvery) == 0 || capReached)) {
            SceneProject* scene = p->getScene(playId);
            if (scene && scene->playState == ScenePlayState::PLAYING) {
                LuaEvalResult r = runLua(p, scene, until, 2.0, 3, 50);
                if (!r.ok) {
                    ws->reason = "condition_error";
                    ws->conditionError = r.error;
                    return true;
                }
                ws->conditionValue = r.values.empty() ? Json(nullptr) : r.values[0];
                if (luaTruthy(ws->conditionValue)) {
                    ws->reason = "condition";
                    return true;
                }
            }
        }
        if (frames > 0 && ws->frames >= frames) {
            ws->reason = "frames";
            return true;
        }
        if (seconds > 0.0 && secondsSince(ws->start) >= seconds) {
            ws->reason = "seconds";
            return true;
        }
        if (gameSeconds > 0.0 && Engine::getGameTime() - ws->engineTime >= gameSeconds - 1e-9) {
            ws->reason = "game_seconds";
            return true;
        }
        // the input's events for the next frame, once this one ran its checks
        if (input && !input->finished) {
            if (!inputPlayable(c, *input)) {
                ws->reason = "input_error";
                if (input->error.empty()) input->error = "the game stopped playing";
                return true;
            }
            advanceInput(*input, true);
            if (!input->error.empty()) {
                ws->reason = "input_error";
                return true;
            }
        }
        if (input && input->finished && frames <= 0 && seconds <= 0.0 && gameSeconds <= 0.0 && until.empty()) {
            ws->reason = "input_done";
            return true;
        }
        return false;
    }, stallGuard ? 0 : static_cast<int>(timeout * 1000.0), [call, ws, until, input, frames, gameSeconds](bool timedOut) {
        // hold again first: nothing may run between the condition and the reply
        ws->advance.release();
        if (input) ws->inputCut = cutInputShort(*input);
        McpContext& c = call->ctx();
        observeSession(c);
        if (timedOut && ws->reason.empty()) ws->reason = "timeout";
        const Json& a = call->args();
        w3::ConsoleQuery query;
        query.since = ws->cursor;
        query.limit = static_cast<size_t>(std::clamp(a.value("console_limit", 50), 0, 1000));
        query.onlyProblems = a.value("only_problems", false);
        w3::ConsoleResult console = query.limit > 0 ? w3::queryConsole(query) : w3::ConsoleResult();
        if (query.limit == 0) console.nextCursor = Out::lastLogId();

        Json data = w3::playStateJson(c);
        const double gameAdvanced = Engine::getGameTime() - ws->engineTime;
        data["reason"] = ws->reason;
        data["elapsed_seconds"] = round3(secondsSince(ws->start));
        data["frames"] = ws->frames;
        data["game_seconds"] = round3(std::max(0.0, gameAdvanced));
        data["console"] = console.entries;
        data["console_more"] = console.more;
        data["next_cursor"] = console.nextCursor;
        if (!until.empty()) data["condition_value"] = ws->conditionValue;
        if (input) {
            data["input"] = {{"steps_done", input->next}, {"steps_total", input->steps.size()}, {"cut_short", ws->inputCut}};
            if (!input->error.empty()) data["input"]["error"] = input->error;
            data["held_input"] = heldInputJson();
        }

        const std::string elapsed = std::to_string(data["elapsed_seconds"].get<double>()).substr(0, 5) + " s, " +
                                    std::to_string(ws->frames) + " frame(s), game time +" +
                                    std::to_string(data["game_seconds"].get<double>()).substr(0, 5) + " s";
        std::string text;
        bool isError = false;
        if (ws->reason == "play_ended") {
            const SessionRecord& last = state().last;
            text = "Play ended while waiting: " + sessionSummaryText(last);
            isError = true;
            if (!last.crash.empty()) data["crash"] = last.crash;
        } else if (ws->reason == "condition") {
            text = "Condition became true (" + elapsed + "): " + ws->conditionValue.dump() + ".";
        } else if (ws->reason == "condition_error") {
            text = "The until condition failed: " + ws->conditionError;
            data["condition_error"] = ws->conditionError;
            isError = true;
        } else if (ws->reason == "input_error") {
            text = "Input stopped at step " + std::to_string(input ? input->next : 0) + ": " + (input ? input->error : std::string());
            isError = true;
        } else if (ws->reason == "stalled") {
            text = "The game stopped advancing: no frame" + std::string(gameSeconds > 0.0 && frames <= 0 ? " or game time" : "") +
                   " for " + std::to_string((int)kStallSeconds) + " s (" + elapsed + "; " +
                   (Engine::isPaused() ? "the app is paused by an app event: send_input {type: \"app\", action: \"resume\"}"
                                       : "is it rendering, or the time scale 0?") + ").";
            isError = true;
        } else if (ws->reason == "timeout") {
            text = until.empty() ? "Timed out (" + elapsed + ")." : "Timed out (" + elapsed + "); the condition is still " +
                   ws->conditionValue.dump() + ".";
            // a frames / game_seconds wait cut short by the wall clock did not do what it was asked
            if (until.empty() && (frames > 0 || gameSeconds > 0.0))
                text += " The frames / game_seconds asked for did not run.";
            isError = !until.empty() || frames > 0 || gameSeconds > 0.0;
        } else if (!until.empty() && (ws->reason == "frames" || ws->reason == "seconds" || ws->reason == "game_seconds")) {
            text = "Reached the " + ws->reason + " cap (" + elapsed + "); the condition is still " + ws->conditionValue.dump() + ".";
            isError = true;
        } else {
            text = "Waited " + elapsed + ".";
        }
        if (ws->inputCut) text += " The wait ended before its input: the rest was skipped and its presses released.";
        if (Engine::isSimulationHeld() && !isError) text += " Held (auto_pause).";
        if (data.value("state", "") == "stopped" && ws->reason != "play_ended") text += " (Not playing: the editor just waited.)";
        if (data.value("state", "") == "paused" && ws->reason == "seconds") text += " (Paused: the game did not advance.)";
        if (!console.entries.empty()) {
            text += "\nConsole (" + std::to_string(console.errors) + " error(s)):\n" + w3::formatConsoleText(console.entries, 5000);
        } else {
            text += " No new console output.";
        }
        ToolResult result = isError ? ToolResult::error(text, data) : ToolResult::success(text, data);
        replyWithOptionalScreenshot(call, std::move(result));
    });
}

} // namespace

// ================================================================== registration

// ================================================================== Play Mode Changes

// The scene a review is pending for: the one asked, else the only one, else the selected
const PlayChangeReview::Pending* pendingReview(const CallPtr& call, std::string& error) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    PlayChangeReview& review = PlayChangeReview::get();
    if (args.contains("scene") && !args["scene"].is_null()) {
        SceneProject* scene = McpRefs::resolveScene(project, args["scene"], error);
        if (!scene) return nullptr;
        if (const PlayChangeReview::Pending* pending = review.pending(scene->id)) return pending;
        error = "No play-mode changes are waiting for scene \"" + scene->name + "\".";
        return nullptr;
    }
    const std::vector<uint32_t> scenes = review.pendingScenes();
    if (scenes.size() == 1) return review.pending(scenes.front());
    if (const PlayChangeReview::Pending* pending = review.pending(project->getSelectedSceneId())) return pending;
    error = scenes.empty() ? std::string("No play-mode changes are waiting: they are listed after a Stop (play stop).")
                           : std::string("Several scenes have play-mode changes waiting: pass scene.");
    return nullptr;
}

Json playChangeJson(const KeepRow& row) {
    Json item = {
        {"id", row.id},
        {"kind", [&]() -> std::string {
            switch (row.kind) {
                case KeepRow::Kind::Property: return "property";
                case KeepRow::Kind::Transform: return "transform";
                case KeepRow::Kind::Component: return "component";
                case KeepRow::Kind::AddComponent: return "add_component";
                case KeepRow::Kind::RemoveComponent: return "remove_component";
                case KeepRow::Kind::Name: return "name";
                case KeepRow::Kind::State: return "state";
                case KeepRow::Kind::Created: return "created";
                case KeepRow::Kind::Deleted: return "deleted";
                case KeepRow::Kind::Unsupported: return "unsupported";
            }
            return "property";
        }()},
        {"game_object", row.entity == NULL_ENTITY ? Json(nullptr) : Json{{"id", row.entity}, {"name", row.gameObject}}},
        {"component", row.hasComponent ? Json(McpRefs::componentName(row.component)) : Json(nullptr)},
        {"property", row.property},
        {"edit_value", row.editValue},
        {"play_value", row.playValue},
        {"source", row.source},
        {"keepable", row.keepable}
    };
    if (!row.reason.empty()) item["reason"] = row.reason;
    return item;
}

void getPlayChangesTool(const CallPtr& call) {
    std::string error;
    const PlayChangeReview::Pending* pending = pendingReview(call, error);
    if (!pending) {
        call->fail(error, Json{{"rows", Json::array()}});
        return;
    }
    Json rows = Json::array();
    std::string text;
    for (const KeepRow& row : pending->rows) {
        rows.push_back(playChangeJson(row));
        text += "\n" + std::to_string(row.id) + ". " + (row.entity == NULL_ENTITY ? std::string() : row.gameObject + " ") +
                (row.hasComponent ? CommandNames::component(row.component) + " " : std::string()) + row.property +
                ": " + row.editValue + " -> " + row.playValue + (row.keepable ? "" : " (can't keep: " + row.reason + ")");
    }
    call->ok(std::to_string(rows.size()) + " play-mode change(s) for scene \"" + pending->sceneName + "\":" + text,
             Json{{"scene_id", pending->sceneId}, {"rows", rows}});
}

void applyPlayChangesTool(const CallPtr& call) {
    std::string error;
    const PlayChangeReview::Pending* pending = pendingReview(call, error);
    if (!pending) {
        call->fail(error);
        return;
    }
    const Json& args = call->args();
    const bool all = args.value("all", false);
    std::vector<uint64_t> ids;
    if (args.contains("ids") && args["ids"].is_array()) {
        for (const Json& id : args["ids"]) ids.push_back(id.get<uint64_t>());
    }
    if (!all && ids.empty()) {
        call->fail("Pass ids (row ids from get_play_changes) or all: true. Rows not listed are discarded.");
        return;
    }
    const uint32_t sceneId = pending->sceneId;
    PlayChangeReview::ApplyResult result = PlayChangeReview::get().apply(call->ctx().project(), sceneId, all ? nullptr : &ids);
    Json skipped = Json::array();
    for (const auto& [id, why] : result.skipped) skipped.push_back({{"id", id}, {"reason", why}});
    Json data = {{"scene_id", sceneId}, {"kept", result.kept}, {"skipped", skipped}};
    if (result.entryId != 0) data["undo"] = {{"id", result.entryId}, {"name", result.entryName}};
    if (result.kept.empty()) {
        call->fail("No play-mode change was kept." + std::string(skipped.empty() ? "" : " Skipped: " + skipped.dump()), data);
        return;
    }
    call->ok("Kept " + std::to_string(result.kept.size()) + " play-mode change(s) as one undo step" +
             (result.entryName.empty() ? std::string(".") : " \"" + result.entryName + "\".") +
             (skipped.empty() ? std::string() : " " + std::to_string(skipped.size()) + " skipped."), data);
}

void discardPlayChangesTool(const CallPtr& call) {
    std::string error;
    const PlayChangeReview::Pending* pending = pendingReview(call, error);
    if (!pending) {
        call->fail(error);
        return;
    }
    const size_t count = pending->rows.size();
    PlayChangeReview::get().discard(pending->sceneId);
    call->ok("Discarded " + std::to_string(count) + " play-mode change(s).");
}

void registerPlayTools(McpRegistry& registry) {
    using namespace schema;

    const Json screenshotOption = w3::screenshotArgSchema();
    const Json maxWidth = w3::maxWidthArgSchema(4096, 768);
    // the played scene by default: the instructions' Addressing says so once for every tool
    const Json runtimeScene = sceneRef();

    {
        ToolDef tool;
        tool.name = "play";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Play Mode";
        tool.description =
            "Control Play mode. start: saves modified scenes, builds C++ scripts if any (minutes), waits until "
            "running; returns the state + console problems, or isError with compile/script errors; past "
            "timeout_seconds (or wait:false) a job_id (get_job). stop: restores the edit-time state, summarizes "
            "the session. step: advance a paused game `frames` frames. Script errors stop play.\nClock (any "
            "action; set = only it; kept until changed): time_scale, fixed_fps, auto_pause (frozen between "
            "calls; runs only in wait / send_input / step).";
        tool.inputSchema = object({
            {"action", stringEnum("", {"start", "stop", "restart", "pause", "resume", "step", "set"})},
            {"scene", sceneRef("start: default the start scene (else selected).")},
            {"wait", withDefault(boolean("start only."), true)},
            {"timeout_seconds", withDefault(number("start/stop: then return a job.", 1, 3600), 120)},
            {"frames", withDefault(integer("", 1, 600), 1)},
            {"time_scale", withDefault(number("", 0, 100), 1)},
            {"fixed_fps", withDefault(integer("0 = real time.", 0, 1000), 0)},
            {"auto_pause", withDefault(boolean(""), false)},
            {"screenshot", screenshotOption},
            {"max_width", maxWidth},
            {"keep_changes", stringEnum("stop: keep all or none of the edits made while playing (absent: get_play_changes).",
                                        {"none", "all"})}
        }, {"action"});
        tool.handler = playTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_play_state";
        tool.title = "Get Play State";
        tool.description =
            "Play state: played and running scenes (child scenes run as copies with own ids), the session (time, "
            "frames, errors, crash), how the last one ended, held input, input (actions, touches, keys), debug_overlay, audio "
            "(voices, mixer_meters dB), "
            "console_cursor, script coroutines and timers (counts), the clock (time_scale, fixed_fps, auto_pause, held), "
            "platform (lifecycle, orientation, opened_urls...), http (request counts, last failures), safe_area; stopped: play_would_start.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getPlayStateTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_play_changes";
        tool.group = "editor_ux";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Get Play Mode Changes";
        tool.description =
            "After play stop: the edits made while playing that can be kept, rows {id, kind, game_object, component, "
            "property, edit_value, play_value, source step, keepable, reason}. Script-made GameObjects can't be kept.";
        tool.inputSchema = object({{"scene", sceneRef()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getPlayChangesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "apply_play_changes";
        tool.group = "editor_ux";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Apply Play Mode Changes";
        tool.description =
            "Keep chosen play-mode changes (ids from get_play_changes, or all) as one undo step on the restored scene; "
            "the other rows are discarded. Returns undo {id, name} and the skipped rows.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"ids", Json{{"type", "array"}, {"items", Json{{"type", "integer"}}}}},
            {"all", withDefault(boolean(""), false)}
        });
        tool.handler = applyPlayChangesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "discard_play_changes";
        tool.group = "editor_ux";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Discard Play Mode Changes";
        tool.description = "Drop the play-mode changes waiting after a Stop.";
        tool.inputSchema = object({{"scene", sceneRef()}});
        tool.annotations.idempotent = true;
        tool.handler = discardPlayChangesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "wait";
        tool.title = "Wait / Run Game";
        tool.description =
            "Let the game run `seconds` (wall clock), `game_seconds`, `frames` (steps a paused game) or until "
            "the Lua `until` is truthy (checked between frames; the others cap it); returns reason, elapsed "
            "time/frames/game_seconds, console lines, play state. input: send_input events from the first frame; "
            "ends at its condition (leftover input skipped, presses released) or when the input is done. isError "
            "if play ended, input failed or `until` errored/timed out/hit its cap.";
        tool.inputSchema = object({
            {"seconds", number("", 0, 600)},
            {"game_seconds", number("", 0, 3600)},
            {"frames", integer("", 0, 1000000)},
            {"until", string("Lua condition (eval_lua helpers).")},
            {"input", Json{{"type", "array"}, {"maxItems", 500}, {"items", anyObject("")}}},
            {"space", withDefault(stringEnum("", {"canvas", "normalized", "view"}), "canvas")},
            {"timeout_seconds", number("Wall clock. Default: 30 (until alone); frames/game_seconds run in full "
                                       "(error if no frame for 60 s).", 0.5, 3600)},
            {"poll_frames", withDefault(integer("until: check every N frames.", 1, 600), 1)},
            {"console_limit", withDefault(integer("", 0, 1000), 50)},
            {"only_problems", withDefault(boolean(""), false)},
            {"screenshot", screenshotOption},
            {"max_width", maxWidth}
        });
        tool.handler = waitTool;
        // runs the game, changes nothing (busy policy: McpServer::busyRefusal)
        tool.activity = ToolActivity::Control;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "eval_lua";
        tool.title = "Evaluate Lua";
        tool.description =
            "Run Lua in the running game: the values as JSON (handles as {type, entity, name, path}), print() "
            "output, console lines. Helpers: scene, script(ref [,class]) -> its self, object(ref [,type]) -> "
            "handle (GameObject, Sprite...), entity(ref) -> id, getScene(name). Globals persist; changes revert "
            "at stop. Stopped: refused unless allow_edit_mode (NOT undoable).";
        tool.inputSchema = object({
            {"code", string("Expression, or chunk with return.")},
            {"scene", runtimeScene},
            {"allow_edit_mode", withDefault(boolean(""), false)},
            {"timeout_seconds", withDefault(number("", 0.1, 60), 5)},
            {"max_depth", withDefault(integer("Table nesting.", 1, 12), 4)},
            {"max_items", withDefault(integer("Entries per table.", 1, 5000), 200)}
        }, {"code"});
        tool.annotations.destructive = true;
        tool.handler = evalLuaTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "list_runtime_entities";
        tool.title = "List Runtime GameObjects";
        tool.description =
            "GameObjects of the running game (id, name, path), runtime-spawned ones included (created_at_runtime), "
            "and the running scenes. Only while playing.";
        tool.inputSchema = object({
            {"scene", runtimeScene},
            {"name_contains", string("Case-insensitive.")},
            {"component", componentName("")},
            {"created_only", withDefault(boolean("Runtime-spawned only."), false)},
            {"with_components", withDefault(boolean(""), false)},
            {"limit", withDefault(integer("", 1, 5000), 200)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listRuntimeEntitiesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_runtime_entity";
        tool.title = "Get Runtime GameObject";
        tool.description =
            "Live values of a running GameObject (runtime-spawned too): components this frame, script state (Lua "
            "fields, C++ DPROPERTY members; coroutines with their waits, timers), physics contacts and callbacks. "
            "Only while playing.";
        tool.inputSchema = object({
            {"scene", runtimeScene},
            {"entity", entityRef()},
            {"components", array(string(""), "")},
            {"properties", array(string(""), "")},
            {"include_script_state", withDefault(boolean(""), true)},
            {"max_depth", withDefault(integer("Script table nesting.", 1, 8), 2)}
        }, {"entity"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getRuntimeEntityTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_runtime_property";
        tool.title = "Set Runtime Property";
        tool.description =
            "Set component properties of a GameObject in the running game, live: property + value, or values {name: "
            "value}. Reverted at stop, never saved; only while playing. Lua script fields: eval_lua. videoplayer also: "
            "state play|pause|stop|prepare, time, frame.";
        tool.inputSchema = object({
            {"scene", runtimeScene},
            {"entity", entityRef()},
            {"component", componentName()},
            {"property", string("")},
            {"value", any("")},
            {"values", anyObject("")}
        }, {"entity", "component"});
        tool.annotations.idempotent = true;
        tool.handler = setRuntimePropertyTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        // free-form events; the UI Document keys are declared so clients see their names
        Json eventItem = anyObject("");
        eventItem["properties"] = {{"target", string("")}, {"to_target", string("")}, {"document", entityRef()}};
        tool.name = "send_input";
        tool.title = "Send Input";
        tool.description =
            "Input into the running game, over frames. events in order, {type, ...} (first action: default):\nkey: key "
            "(space, a, f1...), action press|down|up, hold_frames (2; 0: same frame), hold_ms, mods "
            "[shift,ctrl,alt,super]\ntext: text; ime: text, cursor; ime_commit: text; ime_cancel\n"
            "mouse_leave, mouse_enter\nmouse: action click|double_click|down|up|move|drag|scroll, x, y (default: "
            "pointer), button left|right|middle, to [x,y] + steps (drag), dx, dy (scroll; move w/o x, y)\ntouch: action "
            "tap|start|move|end|cancel|swipe, pointer, x, y, to\ntarget / to_target: UI Document element CSS selector "
            "(document: which)\ngamepad: gamepad (0), button (a, dpad_up...) + action, or axis (left_x..right_trigger) + "
            "value\ninput_action: name Map/Action, action press|down|up|value, value\naction|axis|axis_delta: name + "
            "phase (press|down|up)|value|delta\napp: action pause|resume|focus_lost|focus_gained|low_memory|orientation "
            "(+orientation)\nwait: frames, ms. release_all.\nspace: canvas px, normalized 0..1, view = Game view px. "
            "down holds until up/release_all/stop. Returns frames, console, play state (auto_pause: events + "
            "frames_after only).";
        tool.inputSchema = object({
            {"events", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 500}, {"items", eventItem}}},
            {"space", withDefault(stringEnum("", {"canvas", "normalized", "view"}), "canvas")},
            {"gap_frames", withDefault(integer("Between events.", 0, 1000), 1)},
            {"frames_after", withDefault(integer("Run before replying.", 0, 100000), 1)},
            {"screenshot", screenshotOption},
            {"max_width", maxWidth}
        }, {"events"});
        tool.handler = sendInputTool;
        // input into the running game, changes nothing (busy policy: McpServer::busyRefusal)
        tool.activity = ToolActivity::Control;
        registry.addTool(std::move(tool));
    }
}

}
