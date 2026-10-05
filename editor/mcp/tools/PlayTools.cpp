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
#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "Catalog.h"
#include "Engine.h"
#include "Input.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/ScriptComponent.h"
#include "script/LuaBinding.h"
#include "util/EntityPath.h"
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
    Clock::time_point playingSince;
    bool reachedPlaying = false;
    bool stopRequested = false;     // stopped through the play tool
    uint64_t framesPlaying = 0;
    size_t errors = 0;
    size_t warnings = 0;
    std::string crash;              // "Script crash in scene ..." message
    // when ended
    std::string endReason;          // stopped | crashed | failed_to_start | cancelled | stopped_outside_mcp
    double durationSeconds = 0.0;
    uint64_t endLogId = 0;
};

// Input an agent is holding down in the game (send_input), released at stop
struct HeldInput {
    std::set<int> keys;
    std::set<int> mouseButtons;
    Vector2 mousePos = Vector2(0, 0);   // view (framebuffer) pixels
    std::map<int, Vector2> touches;     // pointer → view pixels
    std::set<int> gamepads;             // connected virtual gamepads
    std::map<int, std::set<int>> gamepadButtons;
    std::map<int, std::set<int>> gamepadAxes;
    bool empty() const {
        if (!keys.empty() || !mouseButtons.empty() || !touches.empty() || !gamepads.empty()) return false;
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

struct PlayToolsState {
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
        {"frames", record.framesPlaying}
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

// Called every frame by the monitor and at the start of every play tool call
void observeSession(McpContext& ctx) {
    PlayToolsState& s = state();
    Project* project = ctx.project();
    const uint32_t playId = project ? project->getPlaySessionSceneId() : NULL_PROJECT_SCENE;

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
        s.pendingStartLogId = 0;
        s.current = record;
    }

    if (s.current.active) {
        scanSessionLogs(s.current);
        SceneProject* sp = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
        if (sp && isRunningState(sp->playState)) {
            if (!s.current.reachedPlaying) s.current.playingSince = Clock::now();
            s.current.reachedPlaying = true;
        }
        if (sp && sp->playState == ScenePlayState::PLAYING) s.current.framesPlaying++;
        if (playId == NULL_PROJECT_SCENE) {
            SessionRecord& r = s.current;
            r.active = false;
            r.durationSeconds = secondsSince(r.startTime);
            r.endLogId = Out::lastLogId();
            if (!r.crash.empty()) r.endReason = "crashed";
            else if (!r.reachedPlaying) r.endReason = r.stopRequested ? "cancelled" : "failed_to_start";
            else if (r.stopRequested) r.endReason = "stopped";
            else r.endReason = "stopped_outside_mcp";
            s.last = r;
            s.current = SessionRecord();
            releaseHeldInput(ctx);
        }
    }

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
                c->app().enqueueMainThreadTask([c]() { run(c); });
            } else {
                state().monitorScheduled = false;
            }
        }
    };
    ctx.app().enqueueMainThreadTask([context]() { Tick::run(context); });
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
        list.push_back(item);
    }
    return list;
}

// ================================================================== input

int parseKeyName(const Json& value) {
    if (value.is_number_integer()) return value.get<int>();
    if (!value.is_string()) return D_KEY_UNKNOWN;
    std::string name = lowerCopy(value.get<std::string>());
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return D_KEY_A + (c - 'a');
        if (c >= '0' && c <= '9') return D_KEY_0 + (c - '0');
        switch (c) {
            case ' ': return D_KEY_SPACE;
            case '\'': return D_KEY_APOSTROPHE;
            case ',': return D_KEY_COMMA;
            case '-': return D_KEY_MINUS;
            case '.': return D_KEY_PERIOD;
            case '/': return D_KEY_SLASH;
            case ';': return D_KEY_SEMICOLON;
            case '=': return D_KEY_EQUAL;
            case '[': return D_KEY_LEFT_BRACKET;
            case '\\': return D_KEY_BACKSLASH;
            case ']': return D_KEY_RIGHT_BRACKET;
            case '`': return D_KEY_GRAVE_ACCENT;
            default: return D_KEY_UNKNOWN;
        }
    }
    for (char& c : name) if (c == ' ' || c == '-') c = '_';
    if (name.rfind("key_", 0) == 0) name = name.substr(4);
    if (name.rfind("d_key_", 0) == 0) name = name.substr(6);
    static const std::map<std::string, int> keys = {
        {"space", D_KEY_SPACE}, {"spacebar", D_KEY_SPACE},
        {"apostrophe", D_KEY_APOSTROPHE}, {"comma", D_KEY_COMMA}, {"minus", D_KEY_MINUS}, {"period", D_KEY_PERIOD},
        {"slash", D_KEY_SLASH}, {"semicolon", D_KEY_SEMICOLON}, {"equal", D_KEY_EQUAL}, {"equals", D_KEY_EQUAL},
        {"left_bracket", D_KEY_LEFT_BRACKET}, {"backslash", D_KEY_BACKSLASH}, {"right_bracket", D_KEY_RIGHT_BRACKET},
        {"grave_accent", D_KEY_GRAVE_ACCENT}, {"backquote", D_KEY_GRAVE_ACCENT},
        {"escape", D_KEY_ESCAPE}, {"esc", D_KEY_ESCAPE}, {"enter", D_KEY_ENTER}, {"return", D_KEY_ENTER},
        {"tab", D_KEY_TAB}, {"backspace", D_KEY_BACKSPACE}, {"insert", D_KEY_INSERT}, {"delete", D_KEY_DELETE},
        {"del", D_KEY_DELETE},
        {"right", D_KEY_RIGHT}, {"left", D_KEY_LEFT}, {"down", D_KEY_DOWN}, {"up", D_KEY_UP},
        {"right_arrow", D_KEY_RIGHT}, {"left_arrow", D_KEY_LEFT}, {"down_arrow", D_KEY_DOWN}, {"up_arrow", D_KEY_UP},
        {"arrow_right", D_KEY_RIGHT}, {"arrow_left", D_KEY_LEFT}, {"arrow_down", D_KEY_DOWN}, {"arrow_up", D_KEY_UP},
        {"page_up", D_KEY_PAGE_UP}, {"pageup", D_KEY_PAGE_UP}, {"page_down", D_KEY_PAGE_DOWN}, {"pagedown", D_KEY_PAGE_DOWN},
        {"home", D_KEY_HOME}, {"end", D_KEY_END},
        {"caps_lock", D_KEY_CAPS_LOCK}, {"scroll_lock", D_KEY_SCROLL_LOCK}, {"num_lock", D_KEY_NUM_LOCK},
        {"print_screen", D_KEY_PRINT_SCREEN}, {"pause", D_KEY_PAUSE},
        {"kp_decimal", D_KEY_KP_DECIMAL}, {"kp_divide", D_KEY_KP_DIVIDE}, {"kp_multiply", D_KEY_KP_MULTIPLY},
        {"kp_subtract", D_KEY_KP_SUBTRACT}, {"kp_add", D_KEY_KP_ADD}, {"kp_enter", D_KEY_KP_ENTER}, {"kp_equal", D_KEY_KP_EQUAL},
        {"left_shift", D_KEY_LEFT_SHIFT}, {"shift", D_KEY_LEFT_SHIFT}, {"left_control", D_KEY_LEFT_CONTROL},
        {"left_ctrl", D_KEY_LEFT_CONTROL}, {"control", D_KEY_LEFT_CONTROL}, {"ctrl", D_KEY_LEFT_CONTROL},
        {"left_alt", D_KEY_LEFT_ALT}, {"alt", D_KEY_LEFT_ALT}, {"left_super", D_KEY_LEFT_SUPER}, {"super", D_KEY_LEFT_SUPER},
        {"right_shift", D_KEY_RIGHT_SHIFT}, {"right_control", D_KEY_RIGHT_CONTROL}, {"right_ctrl", D_KEY_RIGHT_CONTROL},
        {"right_alt", D_KEY_RIGHT_ALT}, {"right_super", D_KEY_RIGHT_SUPER}, {"menu", D_KEY_MENU}
    };
    auto it = keys.find(name);
    if (it != keys.end()) return it->second;
    if (name.size() >= 2 && name[0] == 'f' && std::all_of(name.begin() + 1, name.end(), ::isdigit)) {
        const int n = std::stoi(name.substr(1));
        if (n >= 1 && n <= 25) return D_KEY_F1 + n - 1;
    }
    if (name.rfind("kp_", 0) == 0 && name.size() == 4 && std::isdigit(static_cast<unsigned char>(name[3]))) {
        return D_KEY_KP_0 + (name[3] - '0');
    }
    if (name.rfind("numpad", 0) == 0 && name.size() == 7 && std::isdigit(static_cast<unsigned char>(name[6]))) {
        return D_KEY_KP_0 + (name[6] - '0');
    }
    return D_KEY_UNKNOWN;
}

std::string keyName(int key) {
    if (key >= D_KEY_A && key <= D_KEY_Z) return std::string(1, static_cast<char>('a' + key - D_KEY_A));
    if (key >= D_KEY_0 && key <= D_KEY_9) return std::string(1, static_cast<char>('0' + key - D_KEY_0));
    if (key >= D_KEY_F1 && key <= D_KEY_F25) return "f" + std::to_string(key - D_KEY_F1 + 1);
    switch (key) {
        case D_KEY_SPACE: return "space";
        case D_KEY_ENTER: return "enter";
        case D_KEY_ESCAPE: return "escape";
        case D_KEY_TAB: return "tab";
        case D_KEY_BACKSPACE: return "backspace";
        case D_KEY_LEFT: return "left";
        case D_KEY_RIGHT: return "right";
        case D_KEY_UP: return "up";
        case D_KEY_DOWN: return "down";
        case D_KEY_LEFT_SHIFT: return "left_shift";
        case D_KEY_LEFT_CONTROL: return "left_control";
        case D_KEY_LEFT_ALT: return "left_alt";
        default: return std::to_string(key);
    }
}

int parseMouseButton(const Json& value) {
    if (value.is_null()) return D_MOUSE_BUTTON_LEFT;
    if (value.is_number_integer()) return std::clamp(value.get<int>(), 0, D_MOUSE_BUTTON_LAST);
    const std::string name = value.is_string() ? lowerCopy(value.get<std::string>()) : std::string();
    if (name == "left" || name.empty()) return D_MOUSE_BUTTON_LEFT;
    if (name == "right") return D_MOUSE_BUTTON_RIGHT;
    if (name == "middle") return D_MOUSE_BUTTON_MIDDLE;
    return -1;
}

int parseGamepadButton(const Json& value) {
    if (value.is_number_integer()) return std::clamp(value.get<int>(), 0, D_GAMEPAD_BUTTON_LAST);
    if (!value.is_string()) return -1;
    static const std::map<std::string, int> buttons = {
        {"a", D_GAMEPAD_BUTTON_A}, {"b", D_GAMEPAD_BUTTON_B}, {"x", D_GAMEPAD_BUTTON_X}, {"y", D_GAMEPAD_BUTTON_Y},
        {"cross", D_GAMEPAD_BUTTON_CROSS}, {"circle", D_GAMEPAD_BUTTON_CIRCLE}, {"square", D_GAMEPAD_BUTTON_SQUARE},
        {"triangle", D_GAMEPAD_BUTTON_TRIANGLE}, {"left_bumper", D_GAMEPAD_BUTTON_LEFT_BUMPER}, {"lb", D_GAMEPAD_BUTTON_LEFT_BUMPER},
        {"right_bumper", D_GAMEPAD_BUTTON_RIGHT_BUMPER}, {"rb", D_GAMEPAD_BUTTON_RIGHT_BUMPER},
        {"back", D_GAMEPAD_BUTTON_BACK}, {"select", D_GAMEPAD_BUTTON_BACK}, {"start", D_GAMEPAD_BUTTON_START},
        {"guide", D_GAMEPAD_BUTTON_GUIDE}, {"left_thumb", D_GAMEPAD_BUTTON_LEFT_THUMB}, {"ls", D_GAMEPAD_BUTTON_LEFT_THUMB},
        {"right_thumb", D_GAMEPAD_BUTTON_RIGHT_THUMB}, {"rs", D_GAMEPAD_BUTTON_RIGHT_THUMB},
        {"dpad_up", D_GAMEPAD_BUTTON_DPAD_UP}, {"dpad_right", D_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"dpad_down", D_GAMEPAD_BUTTON_DPAD_DOWN}, {"dpad_left", D_GAMEPAD_BUTTON_DPAD_LEFT}
    };
    auto it = buttons.find(lowerCopy(value.get<std::string>()));
    return it == buttons.end() ? -1 : it->second;
}

int parseGamepadAxis(const Json& value) {
    if (value.is_number_integer()) return std::clamp(value.get<int>(), 0, D_GAMEPAD_AXIS_LAST);
    if (!value.is_string()) return -1;
    static const std::map<std::string, int> axes = {
        {"left_x", D_GAMEPAD_AXIS_LEFT_X}, {"left_y", D_GAMEPAD_AXIS_LEFT_Y}, {"right_x", D_GAMEPAD_AXIS_RIGHT_X},
        {"right_y", D_GAMEPAD_AXIS_RIGHT_Y}, {"left_trigger", D_GAMEPAD_AXIS_LEFT_TRIGGER}, {"lt", D_GAMEPAD_AXIS_LEFT_TRIGGER},
        {"right_trigger", D_GAMEPAD_AXIS_RIGHT_TRIGGER}, {"rt", D_GAMEPAD_AXIS_RIGHT_TRIGGER}
    };
    auto it = axes.find(lowerCopy(value.get<std::string>()));
    return it == axes.end() ? -1 : it->second;
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
    held = HeldInput();
    if (PlayInput* input = ctx.playInput()) input->setPointerInjected(false);
}

Json heldInputJson() {
    const HeldInput& held = state().held;
    Json keys = Json::array();
    for (int key : held.keys) keys.push_back(keyName(key));
    Json buttons = Json::array();
    for (int button : held.mouseButtons) buttons.push_back(button == 0 ? "left" : button == 1 ? "right" : button == 2 ? "middle" : std::to_string(button));
    Json touches = Json::array();
    for (const auto& [pointer, point] : held.touches) touches.push_back(pointer);
    Json pads = Json::array();
    for (int pad : held.gamepads) {
        Json item = {{"gamepad", pad}, {"buttons", Json::array()}};
        auto it = held.gamepadButtons.find(pad);
        if (it != held.gamepadButtons.end()) for (int b : it->second) item["buttons"].push_back(b);
        pads.push_back(item);
    }
    return {{"keys", keys}, {"mouse_buttons", buttons}, {"touches", touches}, {"gamepads", pads}};
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
    } else if (SceneProject* sp = gameViewScene(project)) {
        out["play_would_start"] = {{"id", sp->id}, {"name", sp->name}};
    }
    if (s.last.serial != 0) out["last_session"] = sessionJson(s.last);
    if (!s.held.empty()) out["held_input"] = heldInputJson();
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
    if (ref.is_null()) return main;

    std::vector<SceneProject*> running = runningScenes(project);
    std::string text = ref.is_string() ? ref.get<std::string>() : ref.dump();
    // a Play copy id or a running scene's name
    for (SceneProject* sp : running) {
        if (std::to_string(sp->id) == text || sp->name == text) return sp;
    }
    std::string refError;
    SceneProject* source = McpRefs::resolveScene(project, ref, refError);
    if (source) {
        if (source == main) return main;
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
        (void)depth;
        std::string text = luaL_tolstring(L, index, nullptr);
        lua_pop(L, 1);
        return text;
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

LuaEvalContext* evalContext(lua_State* L) {
    return static_cast<LuaEvalContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Lua is built as C: lua_error longjmps past C++ destructors, so every helper below raises
// errors only once its C++ objects are gone (the message is left on the Lua stack)

int luaEvalPrint(lua_State* L) {
    LuaEvalContext* ctx = evalContext(L);
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

    // Environment: reads and global writes go to _G; the helpers live in the env itself
    lua_newtable(L);
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    if (scene && scene->scene && luabridge::push<Scene*>(L, scene->scene)) {
        lua_setfield(L, -2, "scene");
    }
    const std::pair<const char*, lua_CFunction> helpers[] = {
        {"print", luaEvalPrint}, {"entity", luaEvalEntity}, {"script", luaEvalScript},
        {"object", luaEvalObject}, {"getScene", luaEvalGetScene}
    };
    for (const auto& [name, fn] : helpers) {
        lua_pushlightuserdata(L, &context);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
    if (!lua_setupvalue(L, chunk, 1)) lua_pop(L, 1);

    lua_Hook oldHook = lua_gethook(L);
    const int oldMask = lua_gethookmask(L);
    const int oldCount = lua_gethookcount(L);
    evalTimeoutSeconds = timeoutSeconds;
    evalDeadline = Clock::now() + std::chrono::milliseconds(static_cast<long long>(timeoutSeconds * 1000.0));
    lua_sethook(L, evalTimeoutHook, LUA_MASKCOUNT, 10000);

    status = LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET);

    lua_sethook(L, oldHook, oldMask, oldCount);

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
        if (playId == wait.sceneId && sp && isRunningState(sp->playState)) {
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
    if (sp && sp->playState != ScenePlayState::CANCELLING) project->stop(playId);
    ensureMonitor(ctx);

    call->waitUntil([call, playId]() {
        McpContext& c = call->ctx();
        observeSession(c);
        Project* p = c.project();
        SceneProject* scene = p->getScene(playId);
        return p->getPlaySessionSceneId() == NULL_PROJECT_SCENE && (!scene || scene->playState == ScenePlayState::STOPPED);
    }, static_cast<int>(timeout * 1000.0), [call, after](bool timedOut) {
        McpContext& c = call->ctx();
        observeSession(c);
        if (timedOut) {
            call->fail("Play mode is still stopping (waiting for a C++ build to cancel?). Check get_play_state.",
                       w3::playStateJson(c));
            return;
        }
        if (after) {
            after();
            return;
        }
        const SessionRecord& last = state().last;
        Json data = w3::playStateJson(c);
        data["problems"] = problemsSince(last.startLogId, 30);
        data["next_cursor"] = Out::lastLogId();
        std::string text = "Stopped. " + sessionSummaryText(last) + " The scene is back to its edit-time state.";
        if (!data["problems"].empty()) text += "\nProblems during the session:\n" + w3::formatConsoleText(data["problems"], 4000);
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
    } else {
        sp = w3::gameViewScene(project);
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
        call->fail("action must be start, stop, restart, pause, resume or step.");
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
    if (sp->playState == ScenePlayState::PLAYING) project->pause(playId);
    auto remaining = std::make_shared<int>(frames);
    auto done = std::make_shared<int>(0);
    auto stepOnce = std::make_shared<std::function<void()>>();
    *stepOnce = [call, playId, remaining, done, stepOnce, cursor]() {
        McpContext& c = call->ctx();
        Project* p = c.project();
        SceneProject* scene = p->getScene(playId);
        auto finish = [call, done, cursor](const std::string& note) {
            McpContext& cx = call->ctx();
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
        if (!scene || p->getPlaySessionSceneId() != playId || !isRunningState(scene->playState) || call->cancelled()) {
            finish(", then play ended");
            return;
        }
        if (*remaining <= 0) {
            finish("; paused");
            return;
        }
        p->step(playId);
        call->waitUntil([call, playId]() {
            McpContext& cx = call->ctx();
            observeSession(cx);
            return !cx.project()->isStepPending(playId) || cx.project()->getPlaySessionSceneId() != playId;
        }, 10000, [remaining, done, stepOnce](bool timedOut) {
            (void)timedOut;
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
    } else if (s.last.serial != 0) {
        text += " Last session: " + sessionSummaryText(s.last);
    }
    call->ok(text, data);
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
    const int frames = std::clamp(args.value("frames", 0), 0, 1000000);
    const std::string until = args.value("until", "");
    if (seconds <= 0.0 && frames <= 0 && until.empty()) {
        call->fail("Pass seconds, frames or until (a Lua condition), e.g. {\"seconds\": 2} or "
                   "{\"until\": \"script('GameController').state == 'playing'\", \"timeout_seconds\": 10}.");
        return;
    }
    double timeout = args.value("timeout_seconds", until.empty() ? 0.0 : 30.0);
    if (timeout <= 0.0) timeout = std::max(seconds + 30.0, frames / 10.0 + 30.0);
    timeout = std::clamp(timeout, 0.5, 3600.0);
    const int pollEvery = std::clamp(args.value("poll_frames", 1), 1, 600);

    const uint32_t playId = project->getPlaySessionSceneId();
    SceneProject* sp = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
    const bool playing = sp && isRunningState(sp->playState);
    if (!until.empty() && !playing) {
        call->fail("until needs the game running (play start first).", w3::playStateJson(ctx));
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

    const bool pausedSteps = sp && sp->playState == ScenePlayState::PAUSED && frames > 0;
    if (playing) ensureMonitor(ctx);

    struct WaitState {
        uint64_t cursor = 0;
        uint64_t serial = 0;
        Clock::time_point start;
        int frames = 0;
        int polls = 0;
        std::string reason;
        Json conditionValue;
        std::string conditionError;
        bool stepping = false;
    };
    auto ws = std::make_shared<WaitState>();
    ws->cursor = Out::lastLogId();
    ws->serial = state().current.serial;
    ws->start = Clock::now();
    ws->stepping = pausedSteps;
    if (pausedSteps) project->step(playId);

    call->waitUntil([call, ws, seconds, frames, until, playId, pollEvery, playing]() {
        McpContext& c = call->ctx();
        observeSession(c);
        Project* p = c.project();
        ws->frames++;
        if (playing && (p->getPlaySessionSceneId() != playId || state().current.serial != ws->serial)) {
            ws->reason = "play_ended";
            return true;
        }
        if (ws->stepping) {
            // a paused game advances one frame per step
            if (p->isStepPending(playId)) {
                ws->frames--;
                return false;
            }
            if (ws->frames >= frames) {
                ws->reason = "frames";
                return true;
            }
            p->step(playId);
            return false;
        }
        if (frames > 0 && ws->frames >= frames) {
            ws->reason = "frames";
            return true;
        }
        if (seconds > 0.0 && secondsSince(ws->start) >= seconds) {
            ws->reason = "seconds";
            return true;
        }
        if (!until.empty() && (++ws->polls % pollEvery) == 0) {
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
        return false;
    }, static_cast<int>(timeout * 1000.0), [call, ws, until](bool timedOut) {
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
        data["reason"] = ws->reason;
        data["elapsed_seconds"] = round3(secondsSince(ws->start));
        data["frames"] = ws->frames;
        data["console"] = console.entries;
        data["console_more"] = console.more;
        data["next_cursor"] = console.nextCursor;
        if (!until.empty()) data["condition_value"] = ws->conditionValue;

        std::string text;
        bool isError = false;
        if (ws->reason == "play_ended") {
            const SessionRecord& last = state().last;
            text = "Play ended while waiting: " + sessionSummaryText(last);
            isError = true;
            if (!last.crash.empty()) data["crash"] = last.crash;
        } else if (ws->reason == "condition") {
            text = "Condition became true after " + std::to_string(ws->frames) + " frame(s) (" +
                   std::to_string(data["elapsed_seconds"].get<double>()).substr(0, 5) + " s): " + ws->conditionValue.dump() + ".";
        } else if (ws->reason == "condition_error") {
            text = "The until condition failed: " + ws->conditionError;
            data["condition_error"] = ws->conditionError;
            isError = true;
        } else if (ws->reason == "timeout") {
            text = until.empty() ? "Timed out." : "Timed out after " + std::to_string(data["elapsed_seconds"].get<double>()).substr(0, 5) +
                   " s; the condition is still " + ws->conditionValue.dump() + ".";
            isError = !until.empty();
        } else {
            text = "Waited " + std::to_string(data["elapsed_seconds"].get<double>()).substr(0, 5) + " s / " +
                   std::to_string(ws->frames) + " frame(s).";
        }
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

Json scriptStateJson(Project* project, SceneProject* sp, Entity entity, int maxDepth) {
    Json scripts = Json::array();
    ScriptComponent* component = sp->scene->findComponent<ScriptComponent>(entity);
    if (!component) return scripts;
    lua_State* L = LuaBinding::getLuaState();
    for (ScriptEntry& entry : component->scripts) {
        Json item = {{"class", entry.className}, {"type", entry.type == ScriptType::LUA ? "lua" : "cpp"},
                     {"enabled", entry.enabled}};
        if (entry.type == ScriptType::LUA && entry.instance && L) {
            const int top = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<lua_Integer>(reinterpret_cast<intptr_t>(entry.instance)));
            LuaSerializer serializer{L, project, maxDepth, 100};
            Json fields = serializer.toJson(-1, 0);
            if (fields.is_object()) {
                for (const char* hidden : {"__name", "__scriptRef", "scene"}) fields.erase(hidden);
            }
            item["state"] = fields;
            lua_settop(L, top);
        }
        scripts.push_back(item);
    }
    return scripts;
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
        if (type != ComponentType::ScriptComponent || !onlyProps.empty() || !only.empty()) {
            for (const auto& [name, property] : Catalog::findEntityProperties(sp->scene, entity, type)) {
                if (!onlyProps.empty() && !onlyProps.count(name)) continue;
                try {
                    props[name] = McpValues::toJson(project, name, property);
                } catch (const std::exception&) {
                    props[name] = "<unreadable>";
                }
            }
        }
        components[componentNameOf(type)] = props;
    }

    const std::vector<Entity> created = project->getPlayCreatedEntities(sp);
    Json data = McpRefs::entityJson(sp, entity);
    data["path"] = EntityPath::of(sp->scene, entity);
    data["scene"] = McpRefs::sceneJson(project, *sp);
    data["created_at_runtime"] = std::find(created.begin(), created.end(), entity) != created.end();
    data["components"] = components;
    if (args.value("include_script_state", true)) {
        data["scripts"] = scriptStateJson(project, sp, entity, std::clamp(args.value("max_depth", 2), 1, 8));
    }
    std::string text = "Live GameObject " + std::to_string(entity) + " \"" + data["path"].get<std::string>() + "\" in \"" +
                       sp->name + "\": " + data["components"].dump();
    if (data.contains("scripts") && !data["scripts"].empty()) text += "\nscripts: " + data["scripts"].dump();
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
    for (const auto& [name, property] : Catalog::findEntityProperties(sp->scene, entity, component)) {
        if (values.contains(name)) after[name] = McpValues::toJson(project, name, property);
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

struct InputRun {
    CallPtr call;
    std::vector<InputStep> steps;
    size_t next = 0;
    uint32_t playId = NULL_PROJECT_SCENE;
    uint64_t cursor = 0;
    int frames = 0;
    Clock::time_point start;
    std::string error;
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

// Turns one event into steps; false + error for a malformed event
bool buildEventSteps(McpContext& ctx, const Json& event, const std::string& defaultSpace, int gapFrames,
                     std::vector<InputStep>& steps, std::string& error) {
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
    int holdFrames = std::clamp(event.value("hold_frames", 2), 1, 100000);
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
        add([text]() {
            for (uint32_t cp : decodeUtf8(text)) Engine::systemCharInput(static_cast<wchar_t>(cp));
            return std::string();
        }, gapFrames, 0, "text");
        return true;
    }

    if (type == "mouse") {
        const std::string action = lowerCopy(event.value("action", "click"));
        const int button = parseMouseButton(event.contains("button") ? event["button"] : Json());
        if (button < 0) {
            error = "button must be left, right or middle";
            return false;
        }
        double x = 0, y = 0;
        const bool hasPoint = readPoint(event, "x", "y", "position", x, y);
        if (!hasPoint && action != "scroll" && action != "up") {
            error = "mouse " + action + " needs x and y (or position [x,y])";
            return false;
        }
        // Simulate Touch: the left button is a finger, as in the editor
        const bool asTouch = touchMode && button == D_MOUSE_BUTTON_LEFT;
        auto moveTo = [c, space, mods, asTouch](double px, double py) -> std::string {
            Vector2 p;
            std::string err;
            if (!toViewPoint(space, px, py, p, err)) return err;
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
            return std::string();
        };
        auto pressAt = [c, space, mods, button, asTouch](double px, double py) -> std::string {
            Vector2 p;
            std::string err;
            if (!toViewPoint(space, px, py, p, err)) return err;
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
            return std::string();
        };
        auto releaseAt = [c, space, mods, button, asTouch, hasPoint](double px, double py) -> std::string {
            HeldInput& held = state().held;
            Vector2 p = held.mousePos;
            if (hasPoint) {
                std::string err;
                if (!toViewPoint(space, px, py, p, err)) return err;
            }
            markPointerInjected(*c);
            if (asTouch) {
                Engine::systemTouchEnd(0, p.x, p.y);
                held.touches.erase(0);
            } else {
                Engine::systemMouseUp(button, p.x, p.y, mods);
                held.mouseButtons.erase(button);
            }
            held.mousePos = p;
            return std::string();
        };

        if (action == "move") {
            add([moveTo, x, y]() { return moveTo(x, y); }, gapFrames, 0, "mouse move");
        } else if (action == "down") {
            add([pressAt, x, y]() { return pressAt(x, y); }, gapFrames, 0, "mouse down");
        } else if (action == "up") {
            add([releaseAt, x, y]() { return releaseAt(x, y); }, gapFrames, 0, "mouse up");
        } else if (action == "click" || action == "double_click") {
            const int clicks = action == "double_click" ? 2 : 1;
            for (int i = 0; i < clicks; i++) {
                add([pressAt, x, y]() { return pressAt(x, y); }, holdFrames, holdMs, "mouse down");
                add([releaseAt, x, y]() { return releaseAt(x, y); }, gapFrames, 0, "mouse up");
            }
        } else if (action == "drag") {
            double tx = 0, ty = 0;
            if (!readPoint(event, "to_x", "to_y", "to", tx, ty)) {
                error = "mouse drag needs to: [x,y] (or to_x, to_y)";
                return false;
            }
            const int stepsCount = std::clamp(event.value("steps", 10), 1, 1000);
            add([pressAt, x, y]() { return pressAt(x, y); }, 1, 0, "drag start");
            for (int i = 1; i <= stepsCount; i++) {
                const double t = double(i) / stepsCount;
                const double px = x + (tx - x) * t;
                const double py = y + (ty - y) * t;
                add([moveTo, px, py]() { return moveTo(px, py); }, 1, 0, "drag move");
            }
            add([releaseAt, tx, ty]() { return releaseAt(tx, ty); }, gapFrames, 0, "drag end");
        } else if (action == "scroll") {
            const float dx = event.value("dx", 0.0f);
            const float dy = event.value("dy", event.value("amount", 1.0f));
            add([c, hasPoint, moveTo, x, y, dx, dy, mods]() {
                if (hasPoint) {
                    std::string err = moveTo(x, y);
                    if (!err.empty()) return err;
                }
                Engine::systemMouseScroll(dx, dy, mods);
                (void)c;
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
        double x = 0, y = 0;
        const bool hasPoint = readPoint(event, "x", "y", "position", x, y);
        if (!hasPoint && action != "end" && action != "cancel") {
            error = "touch " + action + " needs x and y";
            return false;
        }
        auto start = [c, space, pointer](double px, double py) -> std::string {
            Vector2 p;
            std::string err;
            if (!toViewPoint(space, px, py, p, err)) return err;
            markPointerInjected(*c);
            Engine::systemTouchStart(pointer, p.x, p.y);
            state().held.touches[pointer] = p;
            return std::string();
        };
        auto move = [c, space, pointer](double px, double py) -> std::string {
            Vector2 p;
            std::string err;
            if (!toViewPoint(space, px, py, p, err)) return err;
            markPointerInjected(*c);
            Engine::systemTouchMove(pointer, p.x, p.y);
            state().held.touches[pointer] = p;
            return std::string();
        };
        auto end = [c, space, pointer, hasPoint](double px, double py) -> std::string {
            HeldInput& held = state().held;
            Vector2 p = held.touches.count(pointer) ? held.touches[pointer] : held.mousePos;
            if (hasPoint) {
                std::string err;
                if (!toViewPoint(space, px, py, p, err)) return err;
            }
            markPointerInjected(*c);
            Engine::systemTouchEnd(pointer, p.x, p.y);
            held.touches.erase(pointer);
            return std::string();
        };
        if (action == "start") add([start, x, y]() { return start(x, y); }, gapFrames, 0, "touch start");
        else if (action == "move") add([move, x, y]() { return move(x, y); }, gapFrames, 0, "touch move");
        else if (action == "end") add([end, x, y]() { return end(x, y); }, gapFrames, 0, "touch end");
        else if (action == "cancel") {
            add([pointer]() {
                Engine::systemTouchCancel(pointer);
                state().held.touches.erase(pointer);
                return std::string();
            }, gapFrames, 0, "touch cancel");
        } else if (action == "tap") {
            add([start, x, y]() { return start(x, y); }, holdFrames, holdMs, "touch start");
            add([end, x, y]() { return end(x, y); }, gapFrames, 0, "touch end");
        } else if (action == "swipe") {
            double tx = 0, ty = 0;
            if (!readPoint(event, "to_x", "to_y", "to", tx, ty)) {
                error = "touch swipe needs to: [x,y]";
                return false;
            }
            const int stepsCount = std::clamp(event.value("steps", 10), 1, 1000);
            add([start, x, y]() { return start(x, y); }, 1, 0, "swipe start");
            for (int i = 1; i <= stepsCount; i++) {
                const double t = double(i) / stepsCount;
                const double px = x + (tx - x) * t;
                const double py = y + (ty - y) * t;
                add([move, px, py]() { return move(px, py); }, 1, 0, "swipe move");
            }
            add([end, tx, ty]() { return end(tx, ty); }, gapFrames, 0, "swipe end");
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

    if (type == "release_all") {
        add([c]() { releaseHeldInput(*c); return std::string(); }, gapFrames, 0, "release all");
        return true;
    }

    error = "type must be key, text, mouse, touch, gamepad, wait or release_all";
    return false;
}

void finishInputRun(const std::shared_ptr<InputRun>& run) {
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
    }
    if (!state().held.empty()) text += " Still held: " + heldInputJson().dump() + " (released at stop or with release_all).";
    if (!console.entries.empty()) text += "\nConsole:\n" + w3::formatConsoleText(console.entries, 4000);
    replyWithOptionalScreenshot(call, isError ? ToolResult::error(text, data) : ToolResult::success(text, data));
}

void runInputSteps(const std::shared_ptr<InputRun>& run) {
    const CallPtr& call = run->call;
    McpContext& ctx = call->ctx();
    while (true) {
        observeSession(ctx);
        Project* project = ctx.project();
        SceneProject* sp = project->getScene(run->playId);
        if (project->getPlaySessionSceneId() != run->playId || !sp || sp->playState != ScenePlayState::PLAYING ||
            call->cancelled()) {
            if (run->error.empty() && sp && sp->playState == ScenePlayState::PAUSED) run->error = "the game was paused";
            finishInputRun(run);
            return;
        }
        if (run->next >= run->steps.size()) {
            const int after = std::clamp(call->args().value("frames_after", 1), 0, 100000);
            if (after > 0) {
                const int start = run->frames;
                run->frames += after;
                (void)start;
                call->afterFrames(after, [run]() { finishInputRun(run); });
            } else {
                finishInputRun(run);
            }
            return;
        }
        InputStep& step = run->steps[run->next];
        const std::string error = step.run();
        run->next++;
        if (!error.empty()) {
            run->error = error;
            finishInputRun(run);
            return;
        }
        if (step.msAfter > 0) {
            const auto until = Clock::now() + std::chrono::milliseconds(step.msAfter);
            const int framesFloor = std::max(step.framesAfter, 1);
            auto count = std::make_shared<int>(0);
            call->waitUntil([until, count, framesFloor, run]() {
                run->frames++;
                return ++*count >= framesFloor && Clock::now() >= until;
            }, step.msAfter + 600000, [run](bool) { runInputSteps(run); });
            return;
        }
        if (step.framesAfter > 0) {
            run->frames += step.framesAfter;
            call->afterFrames(step.framesAfter - 1, [run]() { runInputSteps(run); });
            return;
        }
    }
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
    if (!args.contains("events") || !args["events"].is_array() || args["events"].empty()) {
        call->fail("events must be a non-empty array, e.g. [{\"type\":\"key\",\"key\":\"space\"}, "
                   "{\"type\":\"mouse\",\"action\":\"click\",\"x\":640,\"y\":360}].");
        return;
    }
    if (args["events"].size() > 500) {
        call->fail("At most 500 events per call.");
        return;
    }
    const std::string space = lowerCopy(args.value("space", "canvas"));
    const int gap = std::clamp(args.value("gap_frames", 1), 0, 1000);

    auto run = std::make_shared<InputRun>();
    run->call = call;
    run->playId = playId;
    run->cursor = Out::lastLogId();
    run->start = Clock::now();
    for (size_t i = 0; i < args["events"].size(); i++) {
        std::string error;
        if (!buildEventSteps(ctx, args["events"][i], space, gap, run->steps, error)) {
            call->fail("events[" + std::to_string(i) + "]: " + error + ".");
            return;
        }
    }
    ensureMonitor(ctx);
    call->detach();
    runInputSteps(run);
}

} // namespace

// ================================================================== registration

void registerPlayTools(McpRegistry& registry) {
    using namespace schema;

    registry.removeTool("control_play_mode");

    Json screenshotOption = Json{
        {"description", "Also return a screenshot when done: true (Game view), \"game\", \"scene\" or \"editor\"."},
        {"oneOf", Json::array({Json{{"type", "boolean"}}, Json{{"type", "string"}, {"enum", {"game", "scene", "editor"}}}})}
    };

    {
        ToolDef tool;
        tool.name = "play";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Play Mode";
        tool.description =
            "Control Play mode like the toolbar. action start: plays the scene (default: the selected one, as the Play "
            "button does; saves modified scenes, builds C++ scripts if the project has any, which can take minutes) "
            "and by default waits until the game runs: on success returns the state and the console problems logged "
            "since start; if startup fails (C++ compile error, script error) returns isError with the compiler/console "
            "errors. Past timeout_seconds (or with wait:false) it returns a job_id: poll get_job or get_play_state. "
            "stop: stops and waits until the scene is restored to its edit-time state (play-time changes are "
            "discarded, like Unity), and summarizes the session (errors, crash). restart: stop then start. pause / "
            "resume. step: advance a paused game exactly `frames` frames (pauses first). A Lua/C++ script error "
            "during play stops play automatically: wait, get_play_state and send_input report it as a crash. "
            "Input the agent holds (send_input) is released at stop. Example: {\"action\":\"start\", "
            "\"screenshot\": true}.";
        tool.inputSchema = object({
            {"action", stringEnum("What to do.", {"start", "stop", "restart", "pause", "resume", "step"})},
            {"scene", sceneRef("start: the scene to play (default: the selected scene, or the one behind Prefab Mode).")},
            {"wait", withDefault(boolean("start: wait until the game runs (or fails)."), true)},
            {"timeout_seconds", withDefault(number("start/stop: how long to wait before returning a job id (C++ builds can take minutes).", 1, 3600), 120)},
            {"frames", withDefault(integer("step: frames to advance.", 1, 600), 1)},
            {"screenshot", screenshotOption},
            {"max_width", withDefault(integer("Screenshot width.", 0, 4096), 768)}
        }, {"action"});
        tool.handler = playTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_play_state";
        tool.title = "Get Play State";
        tool.description =
            "Play mode state: stopped / loading (starting; maybe compiling C++) / playing / paused / cancelling "
            "(stopping); the playing scene and the running scenes (child scenes run as copies with their own ids); "
            "the current session (seconds, frames, errors logged, crash) and how the last session ended "
            "(stopped, crashed with the error, failed_to_start, stopped_outside_mcp = by the editor UI); input still "
            "held by send_input; console_cursor to pass to read_console.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getPlayStateTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "wait";
        tool.title = "Wait / Run Game";
        tool.description =
            "Let the game run, then report: for `seconds` (wall clock), for `frames` (editor frames = game updates; on a "
            "paused game this steps it), or `until` a Lua condition is true (evaluated every poll_frames frames in the "
            "game's Lua state with the eval_lua helpers, e.g. \"script('GameController').state == 'dead'\"; "
            "timeout_seconds default 30). Returns why it stopped (seconds / frames / condition / timeout / "
            "play_ended), elapsed time and frames, the console entries logged meanwhile and the play state; "
            "screenshot adds an image. isError when play ended during the wait (crash: the error is included), the "
            "condition errored, or an `until` timed out. The core of a test loop: send_input → wait → "
            "screenshot / eval_lua. Without play running it just waits.";
        tool.inputSchema = object({
            {"seconds", number("Run this many seconds.", 0, 600)},
            {"frames", integer("Run this many frames.", 0, 1000000)},
            {"until", string("Lua expression (or chunk returning a value); stop when truthy. Helpers: script(ref [,class]), object(ref [,type]), entity(ref), scene.")},
            {"timeout_seconds", number("Give up after this long (default 30 for until, else seconds/frames + 30).", 0.5, 3600)},
            {"poll_frames", withDefault(integer("Evaluate until every N frames.", 1, 600), 1)},
            {"console_limit", withDefault(integer("Max console entries returned.", 0, 1000), 50)},
            {"only_problems", withDefault(boolean("Only warnings/errors/build output in the returned console entries."), false)},
            {"screenshot", screenshotOption},
            {"max_width", withDefault(integer("Screenshot width.", 0, 4096), 768)}
        });
        tool.handler = waitTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "eval_lua";
        tool.title = "Evaluate Lua";
        tool.description =
            "Run Lua in the running game's Lua state (the one its scripts use) and get the returned values as JSON "
            "(tables, Vector2/3/4 as arrays, Quaternion, GameObject/Sprite/... handles as {type, entity, name, path}) "
            "plus print() output and console lines. An expression works directly (\"Engine.deltatime\"); a chunk "
            "needs return. Helpers: scene (the played Scene), script(ref [, className]) → the Lua script instance "
            "(its self table: read/write fields like .score), object(ref [, type]) → an engine handle "
            "(GameObject default; Sprite, Text, Camera, ...), entity(ref) → id, getScene(name). ref = GameObject id or "
            "path (runtime-spawned ones included). Globals assigned persist in the Lua state for later calls. Changes "
            "made while playing are discarded at stop. Errors return the Lua traceback (isError). Example: "
            "{\"code\": \"local gc = script('GameController'); return {state = gc.state, score = gc.score, y = gc.planeY}\"}. "
            "Without play it refuses unless allow_edit_mode is true (then it acts on the edit-time scene: NOT undoable).";
        tool.inputSchema = object({
            {"code", string("Lua code: an expression or a chunk ending with return.")},
            {"scene", sceneRef("Which running scene `scene` and the helpers use (default: the played scene).")},
            {"allow_edit_mode", withDefault(boolean("Run even when not playing, against the edit-time scene (changes are not undoable)."), false)},
            {"timeout_seconds", withDefault(number("Abort a runaway script after this long.", 0.1, 60), 5)},
            {"max_depth", withDefault(integer("Table nesting serialized.", 1, 12), 4)},
            {"max_items", withDefault(integer("Entries per table serialized.", 1, 5000), 200)}
        }, {"code"});
        tool.handler = evalLuaTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "list_runtime_entities";
        tool.title = "List Runtime GameObjects";
        tool.description =
            "List the GameObjects of the running game (the played scene by default, or a running child scene), "
            "including those the game spawned at runtime (created_at_runtime: true), with id, name and path. "
            "Filters: name_contains, component, created_only. Also lists the running scenes. Only while playing.";
        tool.inputSchema = object({
            {"scene", sceneRef("A running scene (default: the played scene).")},
            {"name_contains", string("Case-insensitive name filter.")},
            {"component", componentName("Only GameObjects with this component.")},
            {"created_only", withDefault(boolean("Only GameObjects created at runtime."), false)},
            {"with_components", withDefault(boolean("Include each GameObject's component names."), false)},
            {"limit", withDefault(integer("Maximum listed.", 1, 5000), 200)}
        });
        tool.annotations.readOnly = true;
        tool.handler = listRuntimeEntitiesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_runtime_entity";
        tool.title = "Get Runtime GameObject";
        tool.description =
            "Live values of a GameObject in the running game: every component's properties as they are this frame "
            "(position, velocity, text, visible, ...; same shapes as the edit-time component tools) and, for Lua "
            "scripts, the script instance's fields (state). Filter with components / properties. Only while "
            "playing; runtime-spawned GameObjects work too. Example: {\"entity\": \"Plane\", \"components\": [\"transform\"]}.";
        tool.inputSchema = object({
            {"scene", sceneRef("A running scene (default: the played scene).")},
            {"entity", entityRef("GameObject id or path.")},
            {"components", array(componentName(), "Only these components.")},
            {"properties", array(string("Property name."), "Only these property names.")},
            {"include_script_state", withDefault(boolean("Include Lua script instance fields."), true)},
            {"max_depth", withDefault(integer("Nesting of script state tables.", 1, 8), 2)}
        }, {"entity"});
        tool.annotations.readOnly = true;
        tool.handler = getRuntimeEntityTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_runtime_property";
        tool.title = "Set Runtime Property";
        tool.description =
            "Change component properties of a GameObject in the running game, live (for testing: teleport the "
            "player, change a speed, toggle visibility). Values use the same shapes as the edit-time component "
            "tools. Reverted when play stops (Unity semantics); never saved. Only while playing. Lua script fields "
            "are set with eval_lua (script('X').speed = 5). Example: {\"entity\": \"Plane\", \"component\": "
            "\"transform\", \"values\": {\"position\": [300, 400, 3]}}.";
        tool.inputSchema = object({
            {"scene", sceneRef("A running scene (default: the played scene).")},
            {"entity", entityRef("GameObject id or path.")},
            {"component", componentName()},
            {"property", string("Property name (or use values).")},
            {"value", any("Value for property.")},
            {"values", anyObject("Several properties: {name: value}.")}
        }, {"entity", "component"});
        tool.handler = setRuntimePropertyTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "send_input";
        tool.title = "Send Input";
        tool.description =
            "Inject input into the running game, executed across frames (never blocking), whether or not the Game "
            "view has focus or the mouse is over it. events run in order; each is an object with type:\n"
            "- key: {key: \"space\"|\"a\"|\"enter\"|\"left\"|\"f1\"|..., action: press (default: down, hold, up) | down | up, hold_frames (default 2), hold_ms, mods: [\"shift\",\"ctrl\",\"alt\",\"super\"]}\n"
            "- text: {text: \"hello\"} (character input for text fields)\n"
            "- mouse: {action: click (default) | double_click | down | up | move | drag | scroll, x, y, to: [x,y] (drag), steps, button: left|right|middle, dx, dy (scroll)}\n"
            "- touch: {action: tap (default) | start | move | end | cancel | swipe, pointer, x, y, to: [x,y]}\n"
            "- gamepad: {gamepad: 0, button: a|b|x|y|start|dpad_up|..., action: press|down|up} or {axis: left_x|left_y|right_x|right_y|left_trigger|right_trigger, value: -1..1} (a virtual gamepad connects on first use)\n"
            "- wait: {frames, ms}; release_all: {} releases everything held.\n"
            "Coordinates (space): canvas (default) = game canvas pixels, origin top-left, as Input.getMousePosition "
            "reports (canvas size in get_play_state / screenshot); normalized = 0..1 of the canvas; view = Game view "
            "framebuffer pixels. Points outside the canvas are rejected. gap_frames (default 1) separates events so "
            "games that poll Input see every press. Keys/buttons held with down stay held after the call (listed in "
            "held_input) until up, release_all or play stop. Returns frames used, console lines logged, play state; "
            "screenshot adds an image. Example: {\"events\": [{\"type\":\"key\",\"key\":\"space\"}, {\"type\":\"wait\",\"frames\":10}, "
            "{\"type\":\"mouse\",\"x\":640,\"y\":360}], \"screenshot\": true}.";
        tool.inputSchema = object({
            {"events", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 500},
                            {"description", "Input events, in order."},
                            {"items", anyObject("{type: key|text|mouse|touch|gamepad|wait|release_all, ...}")}}},
            {"space", withDefault(stringEnum("Coordinate space of x/y.", {"canvas", "normalized", "view"}), "canvas")},
            {"gap_frames", withDefault(integer("Frames after each event before the next.", 0, 1000), 1)},
            {"frames_after", withDefault(integer("Frames to let the game run after the last event before replying.", 0, 100000), 1)},
            {"screenshot", screenshotOption},
            {"max_width", withDefault(integer("Screenshot width.", 0, 4096), 768)}
        }, {"events"});
        tool.handler = sendInputTool;
        registry.addTool(std::move(tool));
    }
}

}
