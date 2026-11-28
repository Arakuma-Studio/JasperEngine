// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// InputRecording and InputReplay (docs/specs/input-recorder-and-input-scripts.md R27, R28):
// record the input from now into a data:// file, replay a recording or a script (attract modes,
// game tools), give the checkpoints game values.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so the functions do their work in a
// lambda and raise only from a frame holding nothing with a destructor.

#include "LuaBinding.h"
#include "LuaJson.h"
#include "script/binding/NetClassesLua.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "Engine.h"
#include "Log.h"
#include "input/InputScriptSwitch.h"
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED
#include "input/InputRecorder.h"
#include "input/InputReplay.h"
#endif
#include "io/AtomicFile.h"
#include "io/FileData.h"
#include "io/watch/FileWatcher.h"
#include "jobs/JobSystem.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cmath>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

#if JASPERENGINE_INPUT_SCRIPTS_ENABLED
namespace jasperengine {

    // a replay started by InputRecording.play (Coroutine.waitFor(replay) waits for it); its replay
    // exists once the file is parsed, on a job for a large one
    class InputReplayLua : public std::enable_shared_from_this<InputReplayLua> {
    public:
        std::shared_ptr<input::Replay> replay;
        input::ReplayOptions options;
        LuaRegistrySlot onFinished;
        std::string error;
        bool cancelled = false;

        bool isDone() const { return cancelled || !error.empty() || (replay && replay->isDone()); }
        void stop(){
            cancelled = true;
            if (replay) replay->stop();
        }
        int64_t getFrame() const { return replay ? replay->frame() : -1; }
    };

}
#endif

#if !defined(DISABLE_LUA_BINDINGS) && JASPERENGINE_INPUT_SCRIPTS_ENABLED
namespace {

    constexpr size_t LUA_DEFAULT_MAX_BYTES = 1024 * 1024;
    constexpr size_t LUA_MAX_BYTES = 16 * 1024 * 1024;
    // a file larger than this parses on a job, never on the main thread (R35)
    constexpr size_t PARSE_ON_MAIN_MAX_BYTES = 1024 * 1024;

    std::string timestamp(){
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
        return text;
    }

    bool startsWith(const std::string& text, const char* prefix){
        return text.rfind(prefix, 0) == 0;
    }

    int pushNilError(lua_State* L, const std::string& error){
        lua_pushnil(L);
        lua_pushlstring(L, error.data(), error.size());
        return 2;
    }

    // InputRecording.start([{checkpointFrames, probes, maxBytes}]) -> true | nil, err
    int luaStart(lua_State* L){
        std::string error;
        const bool ok = [&]() -> bool {
            input::RecordOptions options;
            options.maxBytes = LUA_DEFAULT_MAX_BYTES;
            if (!lua_isnoneornil(L, 1)) {
                if (!lua_istable(L, 1)) {
                    error = "InputRecording.start: opts must be a table {checkpointFrames, probes, maxBytes}";
                    return false;
                }
                lua_getfield(L, 1, "checkpointFrames");
                if (lua_isnumber(L, -1)) options.checkpointFrames = std::max(0, (int)lua_tointeger(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, 1, "maxBytes");
                if (lua_isnumber(L, -1)) {
                    const lua_Number bytes = lua_tonumber(L, -1);
                    if (bytes < 1024 || bytes > (lua_Number)LUA_MAX_BYTES) {
                        lua_pop(L, 1);
                        error = "InputRecording.start: maxBytes is 1024 to 16777216 (16 MB)";
                        return false;
                    }
                    options.maxBytes = (size_t)bytes;
                }
                lua_pop(L, 1);
                lua_getfield(L, 1, "probes");
                if (lua_istable(L, -1)) {
                    const lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
                    for (lua_Integer i = 1; i <= count && i <= 16; i++) {
                        lua_rawgeti(L, -1, i);
                        if (lua_type(L, -1) == LUA_TSTRING) options.probes.push_back(lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            input::ScriptHeader& h = options.header;
            h.kind = input::ScriptKind::Recording;
            h.fromPlayStart = false;
            const double fixed = Engine::getFixedDeltatime();
            h.fixedFps = fixed > 0.0 ? (int)std::lround(1.0 / fixed) : 60;
            h.space = input::Space::Normalized;
            h.platform = input::platformTag();
            h.canvas = Vector2((float)Engine::getCanvasWidth(), (float)Engine::getCanvasHeight());
            return input::InputRecorder::begin(options, error);
        }();
        if (!ok) return pushNilError(L, "InputRecording.start: " + error);
        lua_pushboolean(L, 1);
        return 1;
    }

    // InputRecording.stop([path]) -> path | nil, err; the file is written on a job
    int luaStop(lua_State* L){
        std::string error;
        std::string path;
        const bool ok = [&]() -> bool {
            path = lua_type(L, 1) == LUA_TSTRING ? std::string(lua_tostring(L, 1)) : "data://recordings/" + timestamp() + ".input.json";
            if (startsWith(path, "lua://") || startsWith(path, "asset://")) {
                error = "lua:// and asset:// are read-only: stop(\"data://recordings/name.input.json\")";
                return false;
            }
            if (path.find("://") == std::string::npos) path = "data://" + path;
            if (!startsWith(path, "data://")) {
                error = "the path must be data:// (the game's user data folder)";
                return false;
            }
            if (!input::InputRecorder::isRecording() && !input::InputRecorder::events()) {
                error = "no recording runs (InputRecording.start first)";
                return false;
            }
            std::shared_ptr<input::RecordedInput> recorded = input::InputRecorder::end();
            if (!recorded) {
                error = "no recording runs (InputRecording.start first)";
                return false;
            }
            const std::string systemPath = FileData::getSystemPath(path);
            jobs::submit([recorded, systemPath, path]() {
                std::string text, why;
                if (!input::writeScript(*recorded->toScript(), text, why)) {
                    Log::error("InputRecording.stop: %s not written: %s", path.c_str(), why.c_str());
                    return;
                }
                const io::WriteResult result = io::writeFileAtomic(watch::pathFromUtf8(systemPath), text);
                if (!result.written())
                    Log::error("InputRecording.stop: %s not written: %s", path.c_str(), result.message.c_str());
            }, {jobs::Lane::Background, jobs::GateClass::Default, "input-recording-write"});
            return true;
        }();
        if (!ok) return pushNilError(L, "InputRecording.stop: " + error);
        lua_pushlstring(L, path.data(), path.size());
        return 1;
    }

    void startReplay(const std::shared_ptr<InputReplayLua>& handle, std::shared_ptr<input::ParsedScript> script){
        if (handle->cancelled) return;
        if (script->header.kind == input::ScriptKind::Recording && script->header.fromPlayStart)
            Log::warn("InputRecording.play: a recording from Play's start plays from now, without its seed");
        std::weak_ptr<InputReplayLua> weak = handle;
        input::ReplayOptions options = handle->options;
        options.onFinished = [weak](input::Replay&) {
            std::shared_ptr<InputReplayLua> self = weak.lock();
            if (!self || !self->onFinished.isSet()) return;
            lua_State* L = LuaBinding::getLuaState();
            if (!L) return;
            const int top = lua_gettop(L);
            self->onFinished.push(L);
            if (!luabridge::push(L, self)) lua_pushnil(L);
            if (LuaBinding::pcallWithTraceback(L, 1, 0) != LUA_OK)
                Log::error("Lua error in InputRecording.play's onFinished: %s", LuaBinding::getLuaStackErrorString(L, -1).c_str());
            lua_settop(L, top);
            self->onFinished.reset();
        };
        handle->replay = input::Replay::start(std::move(script), std::move(options));
    }

    bool readBool(lua_State* L, int table, const char* key, bool fallback){
        lua_getfield(L, table, key);
        const bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return value;
    }

    // InputRecording.play(source [, {loop, blockRealInput, stopOnRealInput, onFinished}]) -> InputReplay | nil, err
    int luaPlay(lua_State* L){
        std::string error;
        auto handle = std::make_shared<InputReplayLua>();
        const bool ok = [&]() -> bool {
            if (!lua_isnoneornil(L, 2)) {
                if (!lua_istable(L, 2)) {
                    error = "opts must be a table {loop, blockRealInput, stopOnRealInput, onFinished}";
                    return false;
                }
                handle->options.loop = readBool(L, 2, "loop", false);
                handle->options.blockRealInput = readBool(L, 2, "blockRealInput", true);
                handle->options.stopOnRealInput = readBool(L, 2, "stopOnRealInput", false);
                lua_getfield(L, 2, "onFinished");
                if (lua_isfunction(L, -1)) handle->onFinished.set(L, -1);
                lua_pop(L, 1);
            }
            auto script = std::make_shared<input::ParsedScript>();
            if (lua_istable(L, 1)) {
                nlohmann::json root;
                if (!LuaJson::toJson(L, 1, root, LuaJson::EncodeOptions(), error) || !input::parseScriptJson(root, *script, error))
                    return false;
                startReplay(handle, script);
                return true;
            }
            if (lua_type(L, 1) != LUA_TSTRING) {
                error = "source must be a path (lua://, asset://, data://) or a decoded input file table";
                return false;
            }
            const std::string path = lua_tostring(L, 1);
            std::unique_ptr<FileData> file(FileData::newFile(path.c_str()));
            if (!file || file->length() == 0) {
                error = "no input file at " + path;
                return false;
            }
            std::string text = file->readString();
            if (text.size() <= PARSE_ON_MAIN_MAX_BYTES) {
                if (!input::parseScript(text, *script, error)) {
                    error = path + ": " + error;
                    return false;
                }
                startReplay(handle, script);
                return true;
            }
            std::weak_ptr<InputReplayLua> weak = handle;
            jobs::submit([weak, path, text = std::move(text)]() {
                auto parsed = std::make_shared<input::ParsedScript>();
                std::string why;
                const bool good = input::parseScript(text, *parsed, why);
                jobs::postToMain([weak, parsed, why, good, path]() {
                    std::shared_ptr<InputReplayLua> self = weak.lock();
                    if (!self) return;
                    if (!good) {
                        self->error = path + ": " + why;
                        Log::error("InputRecording.play: %s", self->error.c_str());
                        return;
                    }
                    startReplay(self, parsed);
                });
            }, {jobs::Lane::Normal, jobs::GateClass::Default, "input-script-parse"});
            return true;
        }();
        if (!ok) return pushNilError(L, "InputRecording.play: " + error);
        if (!luabridge::push(L, handle)) return pushNilError(L, "InputRecording.play: could not create the handle");
        return 1;
    }

    bool luaIsRecording(){
        return input::InputRecorder::isRecording();
    }

    bool luaIsReplaying(){
        return !input::Replay::running().empty();
    }

    void luaStopReplay(){
        for (const std::shared_ptr<input::Replay>& replay : input::Replay::running())
            replay->stop();
    }

    // the recording's frame, else the first replay's; -1 when idle
    int64_t luaFrame(){
        if (input::InputRecorder::isRecording()) return input::InputRecorder::frame();
        const std::vector<std::shared_ptr<input::Replay>> replays = input::Replay::running();
        return replays.empty() ? -1 : replays.front()->frame();
    }

    // InputRecording.checkpoint(name, value) -> true | nil, err: numbers, booleans, strings, vectors
    int luaCheckpoint(lua_State* L){
        std::string error;
        const bool ok = [&]() -> bool {
            if (lua_type(L, 1) != LUA_TSTRING) {
                error = "name must be a string";
                return false;
            }
            const std::string name = lua_tostring(L, 1);
            nlohmann::json value;
            switch (lua_type(L, 2)) {
                case LUA_TNUMBER: value = lua_tonumber(L, 2); break;
                case LUA_TBOOLEAN: value = lua_toboolean(L, 2) != 0; break;
                case LUA_TSTRING: value = std::string(lua_tostring(L, 2)); break;
                default:
                    if (luabridge::isInstance<Vector3>(L, 2)) {
                        const Vector3 v = *luabridge::get<Vector3*>(L, 2).value();
                        value = nlohmann::json::array({v.x, v.y, v.z});
                    } else if (luabridge::isInstance<Vector2>(L, 2)) {
                        const Vector2 v = *luabridge::get<Vector2*>(L, 2).value();
                        value = nlohmann::json::array({v.x, v.y});
                    } else {
                        error = "value must be a number, boolean, string or vector";
                        return false;
                    }
            }
            return input::InputRecorder::checkpointValue(name, value, error);
        }();
        if (!ok) return pushNilError(L, "InputRecording.checkpoint: " + error);
        lua_pushboolean(L, 1);
        return 1;
    }

    // InputReplay.diverged: nil, or {frame, differences, comparable}
    luabridge::LuaRef replayDiverged(const InputReplayLua* self, lua_State* L){
        if (!self->replay || self->replay->report().divergedAtFrame < 0) {
            lua_pushnil(L);
            return luabridge::LuaRef::fromStack(L);
        }
        const input::ReplayReport& report = self->replay->report();
        LuaJson::push(L, nlohmann::json{{"frame", report.divergedAtFrame}, {"differences", report.differences}});
        return luabridge::LuaRef::fromStack(L);
    }

}
#endif

#if !defined(DISABLE_LUA_BINDINGS) && !JASPERENGINE_INPUT_SCRIPTS_ENABLED
namespace {

    // Without the input scripts module the names exist: any use logs once and does nothing
    int inputScriptsNotCompiled(lua_State* L) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log::warn("InputRecording: the input scripts module is not compiled into this build (JASPERENGINE_INPUT_SCRIPTS is off): "
                      "InputRecording and InputReplay do nothing");
        }
        lua_settop(L, 0);
        lua_pushnil(L);
        return 1;
    }

    // any field is the stub again, a call logs and returns nil
    void pushInputScriptsStub(lua_State* L) {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, inputScriptsNotCompiled);
        lua_setfield(L, -2, "__call");
        lua_pushcfunction(L, [](lua_State* state) -> int {
            lua_pushvalue(state, 1);
            return 1;
        });
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
        lua_setfield(L, -2, "__newindex");
        lua_setmetatable(L, -2);
    }

}
#endif

void LuaBinding::registerInputRecordingClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED
    // Records the input that reaches the game from now, and replays recordings and scripts
    // (.input.json) through the engine's input entry points (docs/input-recording.md)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("InputRecording")
        // start([opts]) -> true | nil, err: opts.checkpointFrames (60; 0 none), opts.probes (GameObject
        // paths, up to 16), opts.maxBytes (1 MB, at most 16 MB). Refused while one runs
        .addFunction("start", &luaStart)
        // stop([path]) -> path | nil, err: writes data:// (default data://recordings/<time>.input.json) on a job
        .addFunction("stop", &luaStop)
        // play(source [, opts]) -> InputReplay | nil, err: source a lua://, asset:// or data:// path, or a
        // decoded table; opts.loop (false), blockRealInput (true), stopOnRealInput (false), onFinished(replay)
        .addFunction("play", &luaPlay)
        .addFunction("isRecording", &luaIsRecording)
        .addFunction("isReplaying", &luaIsReplaying)
        // stops every replay
        .addFunction("stopReplay", &luaStopReplay)
        // the recording's frame (else the replay's); -1 when idle
        .addProperty("frame", &luaFrame)
        // checkpoint(name, value) -> true | nil, err: a game value the checkpoints sample and compare
        .addFunction("checkpoint", &luaCheckpoint)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<InputReplayLua>("InputReplay")
        // it ended (its events ran, it failed or was stopped)
        .addFunction("isDone", &InputReplayLua::isDone)
        // stops it; what it held is released
        .addFunction("stop", &InputReplayLua::stop)
        // the frame it is on (-1 once done)
        .addProperty("frame", &InputReplayLua::getFrame)
        // nil, or {frame, differences} at the first checkpoint that differed
        .addProperty("diverged", &replayDiverged)
        .endClass();
#else
    pushInputScriptsStub(L);
    lua_setglobal(L, "InputRecording");
    pushInputScriptsStub(L);
    lua_setglobal(L, "InputReplay");
#endif
#endif
}
