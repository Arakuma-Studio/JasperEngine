// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptScheduler.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "LuaBinding.h"
#include "manager/SceneManager.h"
#include "action/Action.h"
#include "object/AnimationPlayer.h"
#include "object/sound/AudioSource.h"
#include "component/ActionComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "animation/AnimationPlayerRuntime.h"
#include "htmlui/anim/AnimTimer.h"
#include "util/FunctionSubscribe.h"
#include "util/CrashHook.h"
#include "pixelart/HitStop.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>

using namespace jasperengine;

// Lua is built as C: lua_error and lua_yield longjmp over C++ destructors. The lua_CFunctions
// below call an *Impl function that returns the number of results, RAISE (the message pushed)
// or YIELD, once its C++ locals are gone; the lua_CFunction then raises or yields. Inside an
// Impl, Lua calls that can raise (non-raw gets, luaL_check*) come before any C++ object.

namespace {

    constexpr int RAISE = -1;
    constexpr int YIELD = -2;

    // a due time within this of the clock is due (sums of 1/60 are not exact)
    constexpr double EPSILON = 1e-9;

    int continueWait(lua_State* L, int status, lua_KContext ctx);

    int finish(lua_State* L, int results){
        if (results == RAISE) return lua_error(L);
        // the continuation checks who resumes: only the engine may (ctx: the helper's own values)
        if (results == YIELD) return lua_yieldk(L, 0, static_cast<lua_KContext>(lua_gettop(L)), continueWait);
        return results;
    }

    // ------------------------------------------------------------------ records

    enum class CoStatus : uint8_t { Running, Suspended, Finished, Stopped, Error };
    enum class Wait : uint8_t { None, Frames, Seconds, Fixed, EndOfFrame, Until, While, Target };
    enum class Target : uint8_t { None, Coroutine, Timer, Action, AudioSource, AnimationPlayer, Anime, IsDone, External };

    // the global Timer's handles (ScriptScheduler::setTimerAwaitable)
    ScriptScheduler::ExternalAwaitable externalTimer;

    struct CoText {
        std::string className;  // the owner script's class, "" when unowned
        std::string origin;     // "Spawner.lua:12"
        std::string method;     // started by name
        std::string error;      // its error (status Error)
    };

    struct CoRecord {
        TaskId id = 0;
        CoStatus status = CoStatus::Running;
        Wait wait = Wait::None;
        Target target = Target::None;
        bool unscaled = false;       // the clock of a Seconds wait, or of an Until/While timeout
        bool waitRecorded = false;   // a wait helper recorded the wait before yielding
        bool stopRequested = false;  // stopped while running: ends at its next wait
        bool hasTimeout = false;
        bool owned = false;          // a script instance started it (owner is cleared when it is released)
        bool started = false;        // its first segment ran (later resumes need a suspended thread)
        bool indexed = false;        // in State::ownedIndex under (scene, entity)
        uint64_t resumeAt = 0;       // first update frame (or fixed step, end of frame) it may resume
        double due = 0.0;            // Seconds: when; Until/While with a timeout: the deadline
        double waitStart = 0.0;
        lua_State* thread = nullptr;
        int threadRef = LUA_NOREF;
        int predicateRef = LUA_NOREF;
        int targetRef = LUA_NOREF;   // the awaited value (keeps a handle alive)
        int resultsRef = LUA_NOREF;  // what it returned (a table with n), once finished
        TaskId targetId = 0;
        const void* owner = nullptr;
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        std::weak_ptr<char> sceneLife;
        Scene* targetScene = nullptr;
        Entity targetEntity = NULL_ENTITY;
        std::weak_ptr<char> targetSceneLife;
        int handles = 0;             // live Coroutine userdata
        std::unique_ptr<CoText> text;

        bool ended() const{
            return status == CoStatus::Finished || status == CoStatus::Stopped || status == CoStatus::Error;
        }
    };

    struct TimerText {
        std::string className;  // the owner script's class, "" when unowned
        std::string origin;     // where it was made, "" for C++
    };

    struct TimerRecord {
        TaskId id = 0;
        bool unscaled = false;
        bool ended = false;          // its last call ran, or cancelled
        bool cancelled = false;
        bool everyStyle = false;     // Timer.every: the callback gets the Timer, returning false cancels
        bool missingReported = false;
        int remaining = -1;          // calls left, -1 = until cancelled
        uint64_t firstFrame = 0;     // first update frame it may fire in
        double due = 0.0;
        double interval = 0.0;       // 0 = one-shot
        const void* owner = nullptr;
        Scene* scene = nullptr;      // the owner's, for messages
        Entity entity = NULL_ENTITY;
        std::weak_ptr<char> sceneLife;
        int fnRef = LUA_NOREF;       // a Lua function
        std::string method;          // or a method of the owner instance (self:invoke("spawn", 1))
        // or C++: shared, so a repeating callable keeps its state between calls (and lives through a
        // call that cancels its timer)
        std::shared_ptr<std::function<void()>> cpp;
        int handles = 0;             // live Timer userdata
        std::unique_ptr<TimerText> text;
    };

    struct State {
        std::vector<CoRecord> coroutines;   // creation order = id order
        std::vector<TimerRecord> timers;    // creation order = id order
        std::unordered_map<lua_State*, TaskId> threads;   // engine coroutine threads alive
        std::vector<TaskId> runningStack;   // coroutines running now, innermost last
        // the live coroutines of each script GameObject: a deactivation looks up only its subtree
        std::unordered_map<const EntityRegistry*, std::unordered_map<Entity, std::vector<TaskId>>> ownedIndex;
        TaskId nextId = 1;
        uint64_t frame = 0;         // update frames begun
        uint64_t fixedStep = 0;     // fixed steps begun
        uint64_t endFrames = 0;     // end-of-frame phases begun
        double scaledClock = 0.0;   // game seconds of the frames run
        double unscaledClock = 0.0; // unscaled seconds of the frames run
        bool frameActive = false;   // beginFrame ran, the end-of-frame phase has not
        bool inPhase = false;
        int depth = 0;              // phases, resumes and stops on the C stack: records stay put
        bool erasable = false;      // a record may be erasable (something ended, a handle went)
        size_t luaRefs = 0;
    };

    State& S(){
        // never destroyed: handles' destructors run as late as lua_close
        static State* state = new State();
        return *state;
    }

    lua_State* mainState(){
        return LuaBinding::getLuaState();
    }

    template<typename Record>
    Record* findRecord(std::vector<Record>& records, TaskId id){
        auto it = std::lower_bound(records.begin(), records.end(), id,
            [](const Record& record, TaskId value){ return record.id < value; });
        return (it != records.end() && it->id == id) ? &*it : nullptr;
    }

    CoRecord* findCo(TaskId id){ return id ? findRecord(S().coroutines, id) : nullptr; }
    TimerRecord* findTimer(TaskId id){ return id ? findRecord(S().timers, id) : nullptr; }

    void indexAdd(CoRecord& rec){
        if (rec.indexed || !rec.owned || !rec.scene || rec.entity == NULL_ENTITY) return;
        S().ownedIndex[static_cast<const EntityRegistry*>(rec.scene)][rec.entity].push_back(rec.id);
        rec.indexed = true;
    }

    void indexRemove(CoRecord& rec){
        if (!rec.indexed) return;
        rec.indexed = false;
        auto& index = S().ownedIndex;
        auto registry = index.find(static_cast<const EntityRegistry*>(rec.scene));
        if (registry == index.end()) return;
        auto entity = registry->second.find(rec.entity);
        if (entity != registry->second.end()){
            std::vector<TaskId>& ids = entity->second;
            ids.erase(std::remove(ids.begin(), ids.end(), rec.id), ids.end());
            if (ids.empty()) registry->second.erase(entity);
        }
        if (registry->second.empty()) index.erase(registry);
    }

    // The thread runs Lua now (running, or "normal": it resumed another), so it cannot be closed:
    // the engine's own resume, or a coroutine.resume of user code on an engine coroutine
    bool threadIsActive(lua_State* co){
        lua_Debug ar;
        return co && lua_status(co) == LUA_OK && lua_getstack(co, 0, &ar) != 0;
    }

    // ------------------------------------------------------------------ registry refs

    // pops the value on top of L into the registry
    int takeRef(lua_State* L){
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        if (ref != LUA_NOREF && ref != LUA_REFNIL) S().luaRefs++;
        return ref;
    }

    void dropRef(int& ref){
        if (ref != LUA_NOREF && ref != LUA_REFNIL){
            if (lua_State* L = mainState()) luaL_unref(L, LUA_REGISTRYINDEX, ref);
            if (S().luaRefs > 0) S().luaRefs--;
        }
        ref = LUA_NOREF;
    }

    // ------------------------------------------------------------------ clocks and text

    double clockOf(bool unscaled){
        return unscaled ? S().unscaledClock : S().scaledClock;
    }

    Scene* liveScene(Scene* scene, const std::weak_ptr<char>& life){
        return (scene && !life.expired()) ? scene : nullptr;
    }

    // "Spawner (GameObject 'Spawner', scene 'Level1')"
    std::string ownerText(const std::string& className, Scene* scene, Entity entity){
        std::string text = className.empty() ? std::string("script") : className;
        if (scene && entity != NULL_ENTITY && scene->isEntityCreated(entity)){
            text += " (GameObject '" + scene->getEntityName(entity) + "'";
            const std::string sceneName = SceneManager::getSceneNameOf(scene);
            if (!sceneName.empty()) text += ", scene '" + sceneName + "'";
            text += ")";
        }
        return text;
    }

    std::string coroutineErrorHeader(const CoRecord& rec){
        const std::string origin = rec.text ? rec.text->origin : std::string();
        if (!rec.owned || !rec.text || rec.text->className.empty())
            return "Lua coroutine error (started at " + origin + "): ";
        return "Lua coroutine error in " + ownerText(rec.text->className, liveScene(rec.scene, rec.sceneLife), rec.entity) +
               ", started at " + origin + ": ";
    }

    // A script error: the editor's crash handler when one is set (the same as an event
    // callback's error: LuaFunctionBase), else the log
    void reportScriptError(const std::string& tag, const std::string& message){
        // oe-port's CrashHook (upstream's FunctionSubscribeGlobal crash handler)
        if (CrashHook::active() && CrashHook::report(tag, message)) return;
        Log::error("%s", message.c_str());
    }

    // a hint for Lua's own message when a wait crossed a C function
    std::string withYieldHint(const std::string& message){
        if (message.find("attempt to yield across a C-call boundary") != std::string::npos)
            return message + " (waits cannot cross a C function: a table.sort comparator, a metamethod or an engine "
                             "callback called from the coroutine; wait in the coroutine's own code)";
        return message;
    }

    std::string formatOrigin(const lua_Debug& ar, int line){
        std::string source;
        if (ar.source && (ar.source[0] == '@' || ar.source[0] == '='))
            source = ar.source + 1;
        else if (ar.source && std::strlen(ar.source) < 200 && !std::strchr(ar.source, '\n'))
            source = ar.source;   // a chunk named by its path (the engine's script files)
        else
            source = ar.short_src;
        if (line > 0) source += ":" + std::to_string(line);
        return source;
    }

    // "Spawner.lua:5": where the function at `index` of L is defined (no Lua caller: C++ started it)
    std::string functionOrigin(lua_State* L, int index){
        lua_Debug ar;
        lua_pushvalue(L, index);
        if (!lua_getinfo(L, ">S", &ar)) return "?";
        return formatOrigin(ar, ar.linedefined);
    }

    // "Spawner.lua:12" of the Lua function at `level` (1 = the caller of the running C function)
    std::string luaOrigin(lua_State* L, int level){
        lua_Debug ar;
        if (!lua_getstack(L, level, &ar) || !lua_getinfo(L, "Sl", &ar))
            return "?";
        return formatOrigin(ar, ar.currentline);
    }

    // The thread that runs Lua now: the innermost running coroutine, or the main thread
    lua_State* currentThread(){
        State& s = S();
        for (auto it = s.runningStack.rbegin(); it != s.runningStack.rend(); ++it){
            if (CoRecord* rec = findCo(*it)){
                if (rec->thread) return rec->thread;
            }
        }
        return mainState();
    }

    // A wait helper resumed: by the engine (the values it passed are the helper's results), or by
    // a coroutine.resume of user code on an engine coroutine, which is refused (the wait would end
    // early while the engine still counts it as waiting)
    int continueWait(lua_State* L, int status, lua_KContext ctx){
        (void)status;
        const State& s = S();
        auto it = s.threads.find(L);
        if (it == s.threads.end() || s.runningStack.empty() || s.runningStack.back() != it->second){
            // the engine ends it in its next phase (stopped, or failed when the error kills the thread)
            if (it != s.threads.end()){
                if (CoRecord* rec = findCo(it->second)) rec->stopRequested = true;
            }
            return luaL_error(L, "an engine coroutine waiting in a Coroutine wait was resumed by coroutine.resume; "
                                 "the engine resumes its coroutines (wait for one with Coroutine.waitFor)");
        }
        return lua_gettop(L) - static_cast<int>(ctx);
    }

    // ------------------------------------------------------------------ handles

    const char* const COROUTINE_HANDLES = "jasperengine.coroutineHandles";
    const char* const TIMER_HANDLES = "jasperengine.timerHandles";

    // a weak-valued registry table id -> handle userdata: one userdata per coroutine/timer while
    // Lua holds it, so handles compare equal
    void pushHandleCache(lua_State* L, const char* key){
        if (lua_getfield(L, LUA_REGISTRYINDEX, key) == LUA_TTABLE) return;
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushliteral(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }

    template<typename Handle>
    void pushHandle(lua_State* L, const char* key, TaskId id){
        pushHandleCache(L, key);
        lua_rawgeti(L, -1, static_cast<lua_Integer>(id));
        if (lua_isuserdata(L, -1)){
            lua_remove(L, -2);
            return;
        }
        lua_pop(L, 1);
        if (!luabridge::push(L, Handle(id))){
            lua_pop(L, 1);
            lua_pushnil(L);
            return;
        }
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, static_cast<lua_Integer>(id));
        lua_remove(L, -2);
    }

    void pushCoroutineHandle(lua_State* L, TaskId id){ pushHandle<ScriptCoroutine>(L, COROUTINE_HANDLES, id); }
    void pushTimerHandle(lua_State* L, TaskId id){ pushHandle<ScriptTimer>(L, TIMER_HANDLES, id); }

    // the coroutine / timer id of a handle at `index`, 0 when it is not one
    TaskId coroutineHandleId(lua_State* L, int index){
        if (lua_type(L, index) != LUA_TUSERDATA || !luabridge::detail::Userdata::isInstance<ScriptCoroutine>(L, index))
            return 0;
        const ScriptCoroutine* handle = luabridge::detail::Userdata::get<ScriptCoroutine>(L, index, true);
        return handle ? handle->getId() : 0;
    }

    TaskId timerHandleId(lua_State* L, int index){
        if (lua_type(L, index) != LUA_TUSERDATA || !luabridge::detail::Userdata::isInstance<ScriptTimer>(L, index))
            return 0;
        const ScriptTimer* handle = luabridge::detail::Userdata::get<ScriptTimer>(L, index, true);
        return handle ? handle->getId() : 0;
    }

    // ------------------------------------------------------------------ script instances

    struct InstanceInfo {
        int ref = 0;                 // the owner: its registry ref
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
    };

    // The live script instance at `index` (absolute): its ref, scene and entity; ref 0 when the
    // value is not one (a plain table, or an instance its GameObject's release left behind)
    InstanceInfo instanceAt(lua_State* L, int index){
        InstanceInfo info;
        if (!lua_istable(L, index)) return info;
        lua_pushliteral(L, "__scriptRef");
        lua_rawget(L, index);
        const int ref = lua_isinteger(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : 0;
        lua_pop(L, 1);
        if (ref <= 0) return info;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        const bool same = lua_rawequal(L, -1, index) != 0;
        lua_pop(L, 1);
        if (!same) return info;
        info.ref = ref;
        lua_pushliteral(L, "scene");
        lua_rawget(L, index);
        if (luabridge::isInstance<Scene*>(L, -1)){
            auto scene = luabridge::get<Scene*>(L, -1);
            if (scene) info.scene = scene.value();
        }
        lua_pop(L, 1);
        lua_pushliteral(L, "entity");
        lua_rawget(L, index);
        if (lua_isinteger(L, -1)) info.entity = static_cast<Entity>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return info;
    }

    const void* ownerOf(int ref){
        return reinterpret_cast<const void*>(static_cast<intptr_t>(ref));
    }

    int refOf(const void* owner){
        return static_cast<int>(reinterpret_cast<intptr_t>(owner));
    }

    // the instance's class name (its __name), "" when none
    std::string instanceClass(lua_State* L, int index){
        lua_pushliteral(L, "__name");
        lua_rawget(L, index);
        std::string name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return name;
    }

    // ------------------------------------------------------------------ coroutine lifetime

    void releaseWaitRefs(CoRecord& rec){
        dropRef(rec.predicateRef);
        dropRef(rec.targetRef);
        rec.target = Target::None;
        rec.targetId = 0;
        rec.targetScene = nullptr;
        rec.targetEntity = NULL_ENTITY;
        rec.targetSceneLife.reset();
    }

    // Ends the coroutine: its thread is closed (pending to-be-closed variables run, which is Lua:
    // the records may move), its references dropped. `reported`: a message already reported
    // (an error), compared with what the closing leaves.
    void endCoroutine(TaskId id, CoStatus status, const std::string* reported = nullptr){
        CoRecord* rec = findCo(id);
        if (!rec || rec->ended()) return;
        State& s = S();
        if (threadIsActive(rec->thread)){
            // running (resumed by coroutine.resume of user code): closed when control is back
            rec->stopRequested = true;
            return;
        }
        releaseWaitRefs(*rec);
        indexRemove(*rec);
        rec->status = status;
        rec->stopRequested = false;
        s.erasable = true;
        lua_State* co = rec->thread;
        int threadRef = rec->threadRef;
        rec->thread = nullptr;
        rec->threadRef = LUA_NOREF;
        if (!co){
            dropRef(threadRef);
            return;
        }
        s.threads.erase(co);
        lua_State* from = currentThread();
        s.depth++;
        const int closed = lua_closethread(co, from == co ? mainState() : from);
        s.depth--;
        if (closed != LUA_OK){
            const std::string message = LuaBinding::getLuaStackErrorString(co, -1);
            lua_settop(co, 0);
            if (!reported || message != *reported){
                rec = findCo(id);
                if (rec){
                    std::string full = coroutineErrorHeader(*rec) + message + " (closing its to-be-closed variables)";
                    if (rec->text && status != CoStatus::Error){
                        rec->status = CoStatus::Error;
                        rec->text->error = message;
                    }
                    reportScriptError("coroutine:" + (rec->text ? rec->text->origin : std::string()), full);
                }
            }
        }
        dropRef(threadRef);
    }

    // The coroutine failed with the error object on top of its thread
    void failCoroutine(TaskId id, const std::string& rawMessage, bool withTraceback){
        CoRecord* rec = findCo(id);
        if (!rec || rec->ended()) return;
        lua_State* co = rec->thread;
        lua_State* L = mainState();
        std::string message = withYieldHint(rawMessage);
        std::string full = message;
        if (withTraceback && co && L){
            luaL_traceback(L, co, message.c_str(), 0);
            full = lua_tostring(L, -1) ? lua_tostring(L, -1) : message;
            lua_pop(L, 1);
            rec = findCo(id);   // the traceback allocated: finalizers may have moved the records
            if (!rec) return;
        }
        if (rec->text) rec->text->error = full;
        const std::string header = coroutineErrorHeader(*rec);
        const std::string tag = "coroutine:" + (rec->text ? rec->text->origin : std::string());
        // closed before the report: the editor's handler stops Play
        endCoroutine(id, CoStatus::Error, &rawMessage);
        reportScriptError(tag, header + full);
    }

    // ------------------------------------------------------------------ awaitables

    enum AwaitKind : int { AWAIT_NONE = 0, AWAIT_COROUTINE, AWAIT_TIMER, AWAIT_ACTION, AWAIT_AUDIO, AWAIT_ANIMATION_PLAYER,
                           AWAIT_ANIME, AWAIT_ISDONE, AWAIT_EXTERNAL };

    // (value) -> the AwaitKind of what can be waited for (called protected: isDone is read with
    // its metamethods)
    int luaAwaitKind(lua_State* L){
        int kind = AWAIT_NONE;
        if (lua_type(L, 1) == LUA_TUSERDATA){
            using luabridge::detail::Userdata;
            if (externalTimer.is && externalTimer.is(L, 1)) kind = AWAIT_EXTERNAL;
            else if (Userdata::isInstance<ScriptCoroutine>(L, 1)) kind = AWAIT_COROUTINE;
            else if (Userdata::isInstance<ScriptTimer>(L, 1)) kind = AWAIT_TIMER;
            else if (Userdata::isInstance<Action>(L, 1)) kind = AWAIT_ACTION;
            else if (Userdata::isInstance<AudioSource>(L, 1)) kind = AWAIT_AUDIO;
            else if (Userdata::isInstance<AnimationPlayer>(L, 1)) kind = AWAIT_ANIMATION_PLAYER;
            else if (Userdata::isInstance<AnimTimer>(L, 1)) kind = AWAIT_ANIME;
        }
        if (kind == AWAIT_NONE && (lua_type(L, 1) == LUA_TTABLE || lua_type(L, 1) == LUA_TUSERDATA)){
            lua_getfield(L, 1, "isDone");
            if (lua_isfunction(L, -1)) kind = AWAIT_ISDONE;
            lua_pop(L, 1);
        }
        lua_pushinteger(L, kind);
        return 1;
    }

    // (value) -> value:isDone()
    int luaCallIsDone(lua_State* L){
        lua_getfield(L, 1, "isDone");
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        return 1;
    }

    // The kind of the value at `index` of L (protected); false with `error` on a Lua error
    bool awaitKindOf(lua_State* L, int index, int& kind, std::string& error){
        index = lua_absindex(L, index);
        lua_pushcfunction(L, luaAwaitKind);
        lua_pushvalue(L, index);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK){
            error = LuaBinding::getLuaStackErrorString(L, -1);
            lua_pop(L, 1);
            return false;
        }
        kind = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return true;
    }

    // What waitFor resumes with, pushed on `L`; -1 when the target is not done yet
    int pushCoroutineResults(lua_State* L, const CoRecord* target){
        if (!target || target->status == CoStatus::Stopped){
            lua_pushboolean(L, 0);
            lua_pushliteral(L, "stopped");
            return 2;
        }
        if (target->status == CoStatus::Error){
            lua_pushboolean(L, 0);
            const std::string& error = target->text ? target->text->error : std::string();
            lua_pushlstring(L, error.data(), error.size());
            return 2;
        }
        if (target->status != CoStatus::Finished) return -1;
        lua_pushboolean(L, 1);
        if (target->resultsRef == LUA_NOREF) return 1;
        lua_rawgeti(L, LUA_REGISTRYINDEX, target->resultsRef);
        lua_getfield(L, -1, "n");
        const int n = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        if (!lua_checkstack(L, n + 2)){
            lua_pop(L, 1);
            return 1;
        }
        for (int i = 1; i <= n; i++){
            lua_rawgeti(L, -1 - (i - 1), i);
        }
        lua_remove(L, -1 - n);
        return 1 + n;
    }

    // Records "wait for the value at `index` of L" in `rec` (Target); RAISE-style: false and
    // `error` when it cannot be waited for. `immediate` > -1: a coroutine that already ended,
    // its results pushed on `pushTo` (no wait recorded).
    bool recordTargetWait(lua_State* L, int index, TaskId selfId, std::string& error, int& immediate, lua_State* pushTo){
        immediate = -1;
        index = lua_absindex(L, index);
        int kind = AWAIT_NONE;
        if (!awaitKindOf(L, index, kind, error)) return false;
        Target target = Target::None;
        TaskId targetId = 0;
        Scene* targetScene = nullptr;
        Entity targetEntity = NULL_ENTITY;
        switch (kind){
            case AWAIT_COROUTINE: {
                targetId = coroutineHandleId(L, index);
                if (targetId == selfId){
                    error = "Coroutine.waitFor: a coroutine cannot wait for itself";
                    return false;
                }
                const CoRecord* other = findCo(targetId);
                if (!other || other->ended()){
                    immediate = pushCoroutineResults(pushTo, other);
                    return true;
                }
                // A waits for B, which waits (through others) for A: none would ever end
                int steps = 0;
                for (const CoRecord* link = other; link && !link->ended() && link->wait == Wait::Target &&
                        link->target == Target::Coroutine && steps < 100000; steps++){
                    if (link->targetId == selfId){
                        error = "Coroutine.waitFor: Coroutine " + std::to_string(targetId) + " waits" +
                                (steps > 0 ? " (through " + std::to_string(steps) + " other coroutine" + (steps > 1 ? "s" : "") + ")" : std::string()) +
                                " for this one: they would wait for each other forever";
                        return false;
                    }
                    link = findCo(link->targetId);
                }
                target = Target::Coroutine;
                break;
            }
            case AWAIT_TIMER: {
                targetId = timerHandleId(L, index);
                const TimerRecord* timer = findTimer(targetId);
                if (timer && !timer->ended && timer->interval > 0.0 && timer->remaining < 0){
                    error = "Coroutine.waitFor: a repeating timer is never done (give Timer.every a count, or wait for a one-shot)";
                    return false;
                }
                target = Target::Timer;
                break;
            }
            case AWAIT_ACTION:
            case AWAIT_AUDIO:
            case AWAIT_ANIMATION_PLAYER: {
                auto handle = luabridge::get<EntityHandle*>(L, index);
                if (!handle || !handle.value()){
                    error = "Coroutine.waitFor: an invalid handle";
                    return false;
                }
                targetScene = handle.value()->getScene();
                targetEntity = handle.value()->getEntity();
                target = kind == AWAIT_ACTION ? Target::Action : kind == AWAIT_AUDIO ? Target::AudioSource : Target::AnimationPlayer;
                break;
            }
            case AWAIT_ANIME:
                target = Target::Anime;
                break;
            case AWAIT_ISDONE:
                target = Target::IsDone;
                break;
            case AWAIT_EXTERNAL: {
                const int status = externalTimer.status(L, index);
                if (status == -2){
                    error = "Coroutine.waitFor: a repeating timer is never done (give Timer.every a count, or wait for a one-shot)";
                    return false;
                }
                if (status >= 0){
                    lua_pushboolean(pushTo, status);
                    immediate = 1;
                    return true;
                }
                target = Target::External;
                break;
            }
            default:
                error = std::string("Coroutine.waitFor: cannot wait for a ") + luaL_typename(L, index) +
                        " (a Coroutine, Timer, Action/Tween, AudioSource, AnimationPlayer, anime animation or timer, or "
                        "anything with an isDone() method)";
                return false;
        }
        // the reference first: an allocation may run finalizers (Lua), which may move the records
        lua_pushvalue(L, index);
        const int targetRef = takeRef(L);
        CoRecord* rec = findCo(selfId);
        if (!rec){
            int dropped = targetRef;
            dropRef(dropped);
            return false;
        }
        releaseWaitRefs(*rec);
        rec->wait = Wait::Target;
        rec->target = target;
        rec->targetId = targetId;
        rec->targetScene = targetScene;
        rec->targetEntity = targetEntity;
        if (targetScene) rec->targetSceneLife = targetScene->getLifetime();
        rec->targetRef = targetRef;
        rec->resumeAt = S().frame + 1;
        rec->waitRecorded = true;
        return true;
    }

    // Polls a Target wait: the results pushed on the coroutine's thread (the count), -1 not done,
    // -2 an error (message in `error`), -3 the coroutine stopped or moved on meanwhile (isDone() is
    // Lua: it may stop, start or deactivate anything)
    int pollTarget(TaskId id, std::string& error){
        CoRecord* found = findCo(id);
        if (!found || found->status != CoStatus::Suspended || !found->thread || found->wait != Wait::Target) return -3;
        CoRecord& rec = *found;
        lua_State* co = rec.thread;
        switch (rec.target){
            case Target::Coroutine:
                return pushCoroutineResults(co, findCo(rec.targetId));
            case Target::Timer: {
                const TimerRecord* timer = findTimer(rec.targetId);
                if (timer && !timer->ended) return -1;
                lua_pushboolean(co, timer && !timer->cancelled ? 1 : 0);
                return 1;
            }
            case Target::Action:
            case Target::AudioSource:
            case Target::AnimationPlayer: {
                Scene* scene = liveScene(rec.targetScene, rec.targetSceneLife);
                bool gone = !scene || !scene->isEntityCreated(rec.targetEntity);
                bool done = false;
                if (!gone && rec.target == Target::Action){
                    const ActionComponent* action = scene->findComponent<ActionComponent>(rec.targetEntity);
                    gone = !action;
                    done = action && action->state == ActionState::Stopped && !action->startTrigger;
                }else if (!gone && rec.target == Target::AudioSource){
                    const AudioSourceComponent* audio = scene->findComponent<AudioSourceComponent>(rec.targetEntity);
                    gone = !audio;
                    done = audio && audio->state == AudioSourceState::Stopped && !audio->startTrigger;
                }else if (!gone){
                    const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(rec.targetEntity);
                    gone = !player;
                    done = player && !anim::isStatePlaying(*player, nullptr);
                }
                if (!gone && !done) return -1;
                lua_pushboolean(co, gone ? 0 : 1);
                return 1;
            }
            case Target::Anime: {
                lua_State* L = mainState();
                lua_rawgeti(L, LUA_REGISTRYINDEX, rec.targetRef);
                const AnimTimer* timer = luabridge::detail::Userdata::isInstance<AnimTimer>(L, -1)
                    ? luabridge::detail::Userdata::get<AnimTimer>(L, -1, true) : nullptr;
                lua_pop(L, 1);
                if (timer && !timer->isCompleted() && !timer->isCancelled()) return -1;
                lua_pushboolean(co, timer && timer->isCompleted() ? 1 : 0);
                return 1;
            }
            case Target::External: {
                lua_State* L = mainState();
                lua_rawgeti(L, LUA_REGISTRYINDEX, rec.targetRef);
                const int status = externalTimer.status ? externalTimer.status(L, -1) : 0;
                lua_pop(L, 1);
                if (status == -1) return -1;
                lua_pushboolean(co, status > 0 ? 1 : 0);
                return 1;
            }
            case Target::IsDone: {
                // isDone() is Lua: nothing read before it is used after it (`rec`, `co`, the ref)
                lua_State* L = mainState();
                lua_pushcfunction(L, luaCallIsDone);
                lua_rawgeti(L, LUA_REGISTRYINDEX, rec.targetRef);
                if (LuaBinding::pcallWithTraceback(L, 1, 1) != LUA_OK){
                    error = "isDone() of the awaited value failed: " + LuaBinding::getLuaStackErrorString(L, -1);
                    lua_pop(L, 1);
                    const CoRecord* again = findCo(id);
                    return (again && again->status == CoStatus::Suspended && again->thread && again->wait == Wait::Target) ? -2 : -3;
                }
                const bool done = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                CoRecord* again = findCo(id);
                if (!again || again->status != CoStatus::Suspended || !again->thread || again->wait != Wait::Target ||
                        again->target != Target::IsDone)
                    return -3;
                if (!done) return -1;
                // the results go on the main state first, then over to the (checked) thread
                lua_pushboolean(L, 1);
                lua_rawgeti(L, LUA_REGISTRYINDEX, again->targetRef);
                lua_xmove(L, again->thread, 2);
                return 2;
            }
            default:
                return -1;
        }
    }

    // ------------------------------------------------------------------ resuming

    // lua_resume, under the crash guard when the editor set one; false when it caught a crash
    bool guardedResume(lua_State* co, lua_State* from, int nargs, int& status, int& nres, const std::string& origin){
        if (CrashHook::active()){
            struct Resume { lua_State* co; lua_State* from; int nargs; int* status; int* nres; } r{co, from, nargs, &status, &nres};
            std::string errorInfo;
            const bool ok = CrashHook::runGuarded([](void* data){
                Resume* c = static_cast<Resume*>(data);
                *c->status = lua_resume(c->co, c->from, c->nargs, c->nres);
            }, &r, errorInfo);
            if (!ok) CrashHook::report("coroutine:" + origin, errorInfo);
            return ok;
        }
        (void)origin;
        status = lua_resume(co, from, nargs, &nres);
        return true;
    }

    // The values of a plain coroutine.yield(...) (decision 1): nothing or nil = next frame, a
    // handle or awaitable = waitFor(it). The wait is recorded; -1 = recorded, >= 0 = results
    // to resume with at once (pushed on co), -2 = an error (message in `error`).
    int interpretYield(TaskId id, lua_State* co, int nres, std::string& error){
        const int base = lua_gettop(co) - nres;
        if (nres == 0 || lua_isnil(co, base + 1)){
            lua_settop(co, base);
            CoRecord* rec = findCo(id);
            rec->wait = Wait::Frames;
            rec->resumeAt = S().frame + 1;
            return -1;
        }
        const std::string typeName = luaL_typename(co, base + 1);
        // read on the main thread: the coroutine's own stack stays as the yield left it
        lua_State* L = mainState();
        lua_pushvalue(co, base + 1);
        lua_xmove(co, L, 1);
        int immediate = -1;
        std::string why;
        lua_settop(co, base);
        const bool ok = recordTargetWait(L, -1, id, why, immediate, co);
        lua_pop(L, 1);
        if (!ok){
            error = why.find("Coroutine.waitFor: cannot wait for") == 0
                ? "coroutine.yield(a " + typeName + ") in an engine coroutine: yield nothing (next frame) or something to "
                  "wait for (a Coroutine, Timer, Tween, AudioSource, AnimationPlayer, anime timer or anything with isDone()), "
                  "or use Coroutine.wait, waitRealtime, nextFrame, waitFrames, waitUntil, waitWhile, waitFor, "
                  "waitForFixedUpdate or waitForEndOfFrame"
                : why;
            return -2;
        }
        if (immediate >= 0) return immediate;
        CoRecord* rec = findCo(id);
        if (rec) rec->waitRecorded = false;
        return -1;
    }

    // Resumes the coroutine `id` with `nargs` values on its thread's stack, and handles what it
    // did: waits again, finished, failed, or was stopped meanwhile
    // Time.deltaTime inside a coroutine or timer callback follows its owner GameObject's Hit Stop
    // exemption (S48); an unowned one keeps the scope that called it (its first segment)
    void enterOwnerScope(std::optional<pixelart::HitStop::ScriptScope>& scope, Scene* scene, const std::weak_ptr<char>& life, Entity entity){
        if (Scene* owner = liveScene(scene, life))
            scope.emplace(owner, entity);
    }

    // A hit-stop this frame: the Game clock is everyone's frame delta (frozen for the world), but
    // an exempt owner's seconds (and an `only` hit's frozen ones) pass differently, so its due
    // times move by the difference (S48). Idle, beginFrame does not come here.
    void applyHitStopToClocks(double scaledDelta){
        State& s = S();
        for (CoRecord& rec : s.coroutines){
            if (rec.ended() || rec.unscaled) continue;
            Scene* scene = liveScene(rec.scene, rec.sceneLife);
            if (!scene) continue;
            const double difference = pixelart::HitStop::deltaFor(scene, rec.entity, scaledDelta) - scaledDelta;
            rec.due -= difference;
            rec.waitStart -= difference;
        }
        for (TimerRecord& timer : s.timers){
            if (timer.ended || timer.unscaled) continue;
            Scene* scene = liveScene(timer.scene, timer.sceneLife);
            if (!scene) continue;
            timer.due -= pixelart::HitStop::deltaFor(scene, timer.entity, scaledDelta) - scaledDelta;
        }
    }

    void resumeCoroutine(TaskId id, lua_State* from, int nargs){
        State& s = S();
        for (;;){
            CoRecord* rec = findCo(id);
            if (!rec || !rec->thread || rec->ended()) return;
            lua_State* co = rec->thread;
            if (rec->started && rec->stopRequested && lua_status(co) == LUA_YIELD){
                // stopped while user code had it running (coroutine.resume): ends instead of resuming
                lua_pop(co, nargs);
                endCoroutine(id, CoStatus::Stopped);
                return;
            }
            if (rec->started && lua_status(co) != LUA_YIELD){
                // run (or ended) by a coroutine.resume of user code meanwhile
                if (threadIsActive(co)){
                    rec->stopRequested = true;
                    return;
                }
                failCoroutine(id, "the engine coroutine was resumed with coroutine.resume outside the engine "
                                  "(engine coroutines are resumed only by the engine)", false);
                return;
            }
            rec->started = true;
            rec->status = CoStatus::Running;
            rec->wait = Wait::None;
            rec->waitRecorded = false;
            const std::string origin = rec->text ? rec->text->origin : std::string();
            int status = LUA_OK;
            int nres = 0;
            s.depth++;
            s.runningStack.push_back(id);
            bool survived;
            {
                std::optional<pixelart::HitStop::ScriptScope> ownerScope;
                enterOwnerScope(ownerScope, rec->scene, rec->sceneLife, rec->entity);
                survived = guardedResume(co, from, nargs, status, nres, origin);
            }
            s.runningStack.pop_back();
            s.depth--;
            rec = findCo(id);
            if (!rec) return;
            if (!survived){
                // the thread's state is unknown after a native crash: dropped, not closed
                s.threads.erase(co);
                rec->thread = nullptr;
                releaseWaitRefs(*rec);
                indexRemove(*rec);
                dropRef(rec->threadRef);
                rec->status = CoStatus::Error;
                if (rec->text) rec->text->error = "native crash";
                return;
            }
            if (status == LUA_YIELD){
                if (rec->stopRequested){
                    lua_pop(co, nres);
                    endCoroutine(id, CoStatus::Stopped);
                    return;
                }
                if (rec->waitRecorded){
                    lua_pop(co, nres);
                    rec->status = CoStatus::Suspended;
                    return;
                }
                std::string error;
                const int immediate = interpretYield(id, co, nres, error);
                if (immediate == -2){
                    failCoroutine(id, error, true);
                    return;
                }
                rec = findCo(id);
                if (!rec) return;
                if (rec->stopRequested){
                    // stopped by Lua run while the yielded value was read
                    if (immediate > 0) lua_pop(co, immediate);
                    endCoroutine(id, CoStatus::Stopped);
                    return;
                }
                if (immediate >= 0){
                    // waitFor a coroutine that already ended: no frame passes
                    nargs = immediate;
                    from = mainState();
                    continue;
                }
                rec->status = CoStatus::Suspended;
                return;
            }
            if (status == LUA_OK){
                // finished: what it returned stays for waitFor while a handle is alive
                if (nres > 0){
                    const int base = lua_gettop(co) - nres;
                    lua_createtable(co, nres, 1);
                    for (int i = 1; i <= nres; i++){
                        lua_pushvalue(co, base + i);
                        lua_rawseti(co, -2, i);
                    }
                    lua_pushinteger(co, nres);
                    lua_setfield(co, -2, "n");
                    const int resultsRef = takeRef(co);
                    lua_settop(co, base);
                    rec = findCo(id);
                    if (rec) rec->resultsRef = resultsRef;
                }
                endCoroutine(id, CoStatus::Finished);
                return;
            }
            const std::string message = LuaBinding::getLuaStackErrorString(co, -1);
            failCoroutine(id, message, true);
            return;
        }
    }

    // Before a resume: a coroutine of a GameObject no longer active in hierarchy stops (any
    // deactivation path; Unity). True when it was stopped.
    bool stopIfInactive(TaskId id){
        CoRecord* rec = findCo(id);
        if (!rec || !rec->owned || !rec->owner) return false;
        Scene* scene = liveScene(rec->scene, rec->sceneLife);
        if (scene && rec->entity != NULL_ENTITY && scene->isEntityCreated(rec->entity) &&
                scene->isEntityActiveInHierarchy(rec->entity))
            return false;
        if (!scene && !rec->scene) return false;   // an instance without a GameObject
        endCoroutine(id, CoStatus::Stopped);
        return true;
    }

    // stops one coroutine (deferred to its next wait while it runs)
    void stopCoroutineId(TaskId id){
        CoRecord* rec = findCo(id);
        if (!rec || rec->ended()) return;
        if (rec->status == CoStatus::Running || threadIsActive(rec->thread)){
            rec->stopRequested = true;
            return;
        }
        endCoroutine(id, CoStatus::Stopped);
    }

    // the ids of the live coroutines matching `pred` (stopping runs Lua: collect first)
    template<typename Pred>
    std::vector<TaskId> liveCoroutines(Pred pred){
        std::vector<TaskId> ids;
        for (const CoRecord& rec : S().coroutines){
            if (!rec.ended() && pred(rec)) ids.push_back(rec.id);
        }
        return ids;
    }

    // ------------------------------------------------------------------ timers

    TaskId addTimer(TimerRecord&& timer){
        State& s = S();
        timer.id = s.nextId++;
        timer.firstFrame = s.frame + 1;
        s.timers.push_back(std::move(timer));
        return s.timers.back().id;
    }

    void endTimer(TimerRecord& timer, bool cancelled){
        if (timer.ended) return;
        timer.ended = true;
        S().erasable = true;
        timer.cancelled = cancelled;
        dropRef(timer.fnRef);
        timer.cpp = nullptr;
    }

    std::string timerErrorHeader(const TimerRecord& timer){
        const std::string origin = timer.text ? timer.text->origin : std::string();
        if (!timer.owner || !timer.text || timer.text->className.empty())
            return "Lua timer error (created at " + origin + "): ";
        return "Lua timer error in " + ownerText(timer.text->className, liveScene(timer.scene, timer.sceneLife), timer.entity) +
               ", created at " + origin + ": ";
    }

    // (instance, name) -> instance:<name>(), or the MISSING_METHOD light userdata when there is no such method
    char missingMethodKey = 0;
    int luaCallMethod(lua_State* L){
        lua_getfield(L, 1, lua_tostring(L, 2));
        if (!lua_isfunction(L, -1)){
            lua_pushlightuserdata(L, &missingMethodKey);
            return 1;
        }
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        return 1;
    }

    // Calls the timer `id`'s callback once (in the update phase)
    void fireTimer(TaskId id){
        TimerRecord* timer = findTimer(id);
        if (!timer || timer->ended) return;
        std::optional<pixelart::HitStop::ScriptScope> ownerScope;
        enterOwnerScope(ownerScope, timer->scene, timer->sceneLife, timer->entity);
        const double now = clockOf(timer->unscaled);
        // the schedule moves on before the callback (it may cancel the timer or start others)
        bool last = false;
        if (timer->interval > 0.0){
            if (timer->remaining > 0) timer->remaining--;
            last = timer->remaining == 0;
            timer->due += timer->interval;
            if (timer->due <= now + EPSILON) timer->due = now + timer->interval;
        }else{
            last = true;
        }
        lua_State* L = mainState();
        if (timer->cpp){
            // the same callable every call (its state stays), alive through the call
            const std::shared_ptr<std::function<void()>> fn = timer->cpp;
            if (last) endTimer(*timer, false);
            if (CrashHook::active()){
                std::string errorInfo;
                if (!CrashHook::runGuarded([](void* data){ (*static_cast<std::function<void()>*>(data))(); }, fn.get(), errorInfo)){
                    CrashHook::report("timer:C++", errorInfo);
                    if (TimerRecord* again = findTimer(id)) endTimer(*again, true);
                }
                return;
            }
            // no hook (an exported game): a throw is logged and the timer keeps its schedule
            try {
                (*fn)();
            } catch (...) {
                CrashHook::logFailure("timer:C++");
            }
            return;
        }
        if (!L){
            endTimer(*timer, true);
            return;
        }
        const int top = lua_gettop(L);
        bool missing = false;
        int status = LUA_OK;
        if (timer->fnRef != LUA_NOREF){
            lua_rawgeti(L, LUA_REGISTRYINDEX, timer->fnRef);
            int nargs = 0;
            if (timer->everyStyle){
                pushTimerHandle(L, id);
                nargs = 1;
                timer = findTimer(id);   // the handle allocated: finalizers may have moved the records
            }
            if (last && timer) endTimer(*timer, false);
            status = LuaBinding::pcallWithTraceback(L, nargs, 1);
        }else{
            const int ownerRef = refOf(timer->owner);
            const std::string method = timer->method;
            if (last) endTimer(*timer, false);
            lua_pushcfunction(L, luaCallMethod);
            lua_rawgeti(L, LUA_REGISTRYINDEX, ownerRef);
            lua_pushlstring(L, method.data(), method.size());
            status = LuaBinding::pcallWithTraceback(L, 2, 1);
            missing = status == LUA_OK && lua_touserdata(L, -1) == &missingMethodKey;
        }
        timer = findTimer(id);
        if (status != LUA_OK){
            const std::string message = LuaBinding::getLuaStackErrorString(L, -1);
            if (timer){
                reportScriptError("timer:" + (timer->text ? timer->text->origin : std::string()), timerErrorHeader(*timer) + message);
            }
        }else if (missing){
            if (timer){
                const std::string message = "invoke: no method '" + timer->method + "' on " +
                    (timer->text ? timer->text->className : std::string("the script")) + "; the timer ends";
                if (!timer->missingReported){
                    timer->missingReported = true;
                    reportScriptError("timer:" + (timer->text ? timer->text->origin : std::string()), timerErrorHeader(*timer) + message);
                }
                if (TimerRecord* again = findTimer(id)){
                    endTimer(*again, true);
                    again->cancelled = true;   // a one-shot ended before the call: it did not fire
                }
            }
        }else if (timer && timer->everyStyle && lua_isboolean(L, -1) && !lua_toboolean(L, -1)){
            // hump rule: an every callback returning false cancels it
            endTimer(*timer, true);
        }
        lua_settop(L, top);
    }

    // ------------------------------------------------------------------ compaction

    bool coroutineErasable(const CoRecord& rec){
        return rec.ended() && rec.handles <= 0;
    }

    void compact(){
        State& s = S();
        if (s.depth > 0 || !s.erasable) return;
        s.erasable = false;
        for (CoRecord& rec : s.coroutines){
            if (coroutineErasable(rec)) dropRef(rec.resultsRef);
        }
        s.coroutines.erase(std::remove_if(s.coroutines.begin(), s.coroutines.end(), coroutineErasable), s.coroutines.end());
        s.timers.erase(std::remove_if(s.timers.begin(), s.timers.end(),
            [](const TimerRecord& timer){ return timer.ended && timer.handles <= 0; }), s.timers.end());
    }

    // Resumes a suspended coroutine whose wait is satisfied, with the values pushed on its thread
    void resumeSatisfied(TaskId id, int nargs){
        CoRecord* rec = findCo(id);
        if (!rec) return;
        releaseWaitRefs(*rec);
        resumeCoroutine(id, mainState(), nargs);
    }

    // ------------------------------------------------------------------ Lua: wait helpers

    // The engine coroutine running on L, or 0 with the message pushed
    TaskId runningCoroutine(lua_State* L, const char* fn){
        auto it = S().threads.find(L);
        if (it == S().threads.end()){
            lua_pushfstring(L, "%s: not inside an engine coroutine (start one with self:startCoroutine or Coroutine.start; "
                               "event handlers like onUpdate cannot wait)", fn);
            return 0;
        }
        const std::vector<TaskId>& running = S().runningStack;
        if (running.empty() || running.back() != it->second){
            lua_pushfstring(L, "%s: this engine coroutine was resumed by coroutine.resume; the engine resumes its "
                               "coroutines (wait for one with Coroutine.waitFor, use coroutine.wrap for generators)", fn);
            return 0;
        }
        if (!lua_isyieldable(L)){
            lua_pushfstring(L, "%s: waits cannot cross a C function (a table.sort comparator, a metamethod or an engine "
                               "callback called from the coroutine); wait in the coroutine's own code", fn);
            return 0;
        }
        return it->second;
    }

    int waitSecondsImpl(lua_State* L, bool unscaled){
        const char* fn = unscaled ? "Coroutine.waitRealtime" : "Coroutine.wait";
        if (lua_type(L, 1) != LUA_TNUMBER){
            lua_pushfstring(L, "%s(seconds): seconds must be a number, got %s", fn, luaL_typename(L, 1));
            return RAISE;
        }
        const double seconds = lua_tonumber(L, 1);
        const TaskId id = runningCoroutine(L, fn);
        if (!id) return RAISE;
        CoRecord* rec = findCo(id);
        rec->wait = Wait::Seconds;
        rec->unscaled = unscaled;
        rec->waitStart = clockOf(unscaled);
        rec->due = rec->waitStart + ((seconds > 0.0 && std::isfinite(seconds)) ? seconds : (seconds > 0.0 ? HUGE_VAL : 0.0));
        rec->resumeAt = S().frame + 1;
        rec->waitRecorded = true;
        return YIELD;
    }

    int waitFramesImpl(lua_State* L, const char* fn, lua_Integer frames){
        const TaskId id = runningCoroutine(L, fn);
        if (!id) return RAISE;
        CoRecord* rec = findCo(id);
        rec->wait = Wait::Frames;
        rec->resumeAt = S().frame + static_cast<uint64_t>(frames);
        rec->waitRecorded = true;
        return YIELD;
    }

    int waitFramesArgImpl(lua_State* L){
        if (!lua_isinteger(L, 1) || lua_tointeger(L, 1) < 1){
            lua_pushfstring(L, "Coroutine.waitFrames(n): n must be an integer >= 1, got %s",
                            lua_type(L, 1) == LUA_TNUMBER ? lua_tostring(L, 1) : luaL_typename(L, 1));
            return RAISE;
        }
        return waitFramesImpl(L, "Coroutine.waitFrames", lua_tointeger(L, 1));
    }

    int waitPhaseImpl(lua_State* L, bool fixed){
        const TaskId id = runningCoroutine(L, fixed ? "Coroutine.waitForFixedUpdate" : "Coroutine.waitForEndOfFrame");
        if (!id) return RAISE;
        CoRecord* rec = findCo(id);
        rec->wait = fixed ? Wait::Fixed : Wait::EndOfFrame;
        rec->resumeAt = fixed ? S().fixedStep + 1 : S().endFrames + 1;
        rec->waitRecorded = true;
        return YIELD;
    }

    int waitPredicateImpl(lua_State* L, bool until){
        const char* fn = until ? "Coroutine.waitUntil" : "Coroutine.waitWhile";
        if (!lua_isfunction(L, 1)){
            lua_pushfstring(L, "%s(predicate [, timeout [, unscaled]]): predicate must be a function, got %s", fn, luaL_typename(L, 1));
            return RAISE;
        }
        if (!lua_isnoneornil(L, 2) && lua_type(L, 2) != LUA_TNUMBER){
            lua_pushfstring(L, "%s: timeout must be a number of seconds, got %s", fn, luaL_typename(L, 2));
            return RAISE;
        }
        const TaskId id = runningCoroutine(L, fn);
        if (!id) return RAISE;
        const bool unscaled = lua_toboolean(L, 3) != 0;
        lua_pushvalue(L, 1);
        const int predicateRef = takeRef(L);
        CoRecord* rec = findCo(id);
        releaseWaitRefs(*rec);
        rec->predicateRef = predicateRef;
        rec->wait = until ? Wait::Until : Wait::While;
        rec->unscaled = unscaled;
        rec->hasTimeout = lua_type(L, 2) == LUA_TNUMBER;
        rec->waitStart = clockOf(unscaled);
        rec->due = rec->hasTimeout ? rec->waitStart + std::max(0.0, static_cast<double>(lua_tonumber(L, 2))) : 0.0;
        rec->resumeAt = S().frame + 1;
        rec->waitRecorded = true;
        return YIELD;
    }

    int waitForImpl(lua_State* L){
        if (lua_isnoneornil(L, 1)){
            lua_pushliteral(L, "Coroutine.waitFor(x): x is nil (a Coroutine, Timer, Tween/Action, AudioSource, AnimationPlayer, "
                               "anime animation or anything with isDone())");
            return RAISE;
        }
        const TaskId id = runningCoroutine(L, "Coroutine.waitFor");
        if (!id) return RAISE;
        std::string error;
        int immediate = -1;
        const int top = lua_gettop(L);
        const bool ok = recordTargetWait(L, 1, id, error, immediate, L);
        if (!ok){
            lua_settop(L, top);
            lua_pushlstring(L, error.data(), error.size());
            return RAISE;
        }
        CoRecord* rec = findCo(id);
        if (rec && rec->stopRequested){
            // stopped (co:stop() before, or by Lua run to read x): no result, it ends at this wait
            lua_settop(L, top);
            rec->wait = Wait::Frames;
            rec->resumeAt = S().frame + 1;
            rec->waitRecorded = true;
            return YIELD;
        }
        if (immediate >= 0) return immediate;
        return YIELD;
    }

    // ------------------------------------------------------------------ Lua: starting

    // Starts an engine coroutine running the function at `fnIndex` of L with the `nargs` values
    // after it (or, with `selfIndex`, the method `self[name]` with self first); pushes its handle
    TaskId startThread(lua_State* L, int fnIndex, int nargs, const InstanceInfo* owner, int selfIndex, int ownerIndex,
                       const char* methodName, bool fromCpp);

    int startImpl(lua_State* L, int fnIndex, int nargs, const InstanceInfo* owner, int selfIndex, const char* methodName){
        const TaskId id = startThread(L, fnIndex, nargs, owner, selfIndex, selfIndex ? selfIndex : 1, methodName, false);
        pushCoroutineHandle(L, id);
        return 1;
    }

    // the coroutine's thread and record, its first segment run; returns its id
    TaskId startThread(lua_State* L, int fnIndex, int nargs, const InstanceInfo* owner, int selfIndex, int ownerIndex,
                       const char* methodName, bool fromCpp){
        State& s = S();
        lua_State* co = lua_newthread(L);
        const int threadRef = takeRef(L);
        // the function and its arguments move to the new thread (self first for a method)
        lua_pushvalue(L, fnIndex);
        int moved = 1;
        if (selfIndex){
            lua_pushvalue(L, selfIndex);
            moved++;
        }
        for (int i = 0; i < nargs; i++){
            lua_pushvalue(L, fnIndex + 1 + i);
        }
        moved += nargs;
        lua_xmove(L, co, moved);

        CoRecord rec;
        rec.id = s.nextId++;
        rec.thread = co;
        rec.threadRef = threadRef;
        rec.text = std::make_unique<CoText>();
        rec.text->origin = fromCpp ? functionOrigin(L, fnIndex) : luaOrigin(L, 1);
        if (methodName) rec.text->method = methodName;
        if (owner && owner->ref){
            rec.owned = true;
            rec.owner = ownerOf(owner->ref);
            rec.scene = owner->scene;
            rec.entity = owner->entity;
            if (owner->scene) rec.sceneLife = owner->scene->getLifetime();
            rec.text->className = instanceClass(L, ownerIndex);
        }
        const TaskId id = rec.id;
        s.coroutines.push_back(std::move(rec));
        indexAdd(s.coroutines.back());
        s.threads[co] = id;

        // the first segment runs now, under the caller's hook (eval_lua's timeout); later
        // segments have none
        resumeCoroutine(id, L, moved - 1);
        if (CoRecord* started = findCo(id)){
            if (started->thread) lua_sethook(started->thread, nullptr, 0, 0);
        }
        return id;
    }

    // Coroutine.start(fn, ...)
    int coroutineStartImpl(lua_State* L){
        if (!lua_isfunction(L, 1)){
            lua_pushfstring(L, "Coroutine.start(fn, ...): fn must be a function, got %s", luaL_typename(L, 1));
            return RAISE;
        }
        return startImpl(L, 1, lua_gettop(L) - 1, nullptr, 0, nullptr);
    }

    // the script instance self at 1 or RAISE
    bool selfInstance(lua_State* L, const char* fn, InstanceInfo& info){
        info = instanceAt(L, 1);
        if (info.ref) return true;
        lua_pushfstring(L, "%s: self must be a live script instance (call it with a colon, self:%s(...), in a script's "
                           "method; outside scripts use Coroutine.start / Timer.after)", fn, fn);
        return false;
    }

    bool selfIsInactive(const InstanceInfo& info){
        return info.scene && info.entity != NULL_ENTITY && info.scene->isEntityCreated(info.entity) &&
               !info.scene->isEntityActiveInHierarchy(info.entity);
    }

    // self:startCoroutine(fnOrMethodName, ...): the method was looked up into index 2 already
    int startCoroutineImpl(lua_State* L, bool byName){
        InstanceInfo info;
        if (!selfInstance(L, "startCoroutine", info)) return RAISE;
        if (selfIsInactive(info)){
            lua_pushfstring(L, "startCoroutine: GameObject '%s' is inactive (coroutines run on active GameObjects)",
                            info.scene->getEntityName(info.entity).c_str());
            return RAISE;
        }
        const int top = lua_gettop(L);
        // by name: [self, name, ..., method]: the method goes first, self is passed to it
        if (byName){
            const int nargs = top - 3;
            // stack: self, name, args..., method -> move method before the args
            lua_insert(L, 3);   // self, name, method, args...
            const std::string name = lua_tostring(L, 2);
            return startImpl(L, 3, nargs, &info, 1, name.c_str());
        }
        return startImpl(L, 2, top - 2, &info, 0, nullptr);
    }

    // ------------------------------------------------------------------ Lua: timers

    struct TimerArgs {
        double delay = 0.0;
        double interval = 0.0;
        bool unscaled = false;
        int count = -1;
        bool every = false;
    };

    TaskId makeLuaTimer(lua_State* L, int callbackIndex, const char* method, const TimerArgs& args, const InstanceInfo* owner, bool everyStyle){
        TimerRecord timer;
        timer.unscaled = args.unscaled;
        timer.interval = args.every ? args.interval : 0.0;
        timer.remaining = args.every ? args.count : -1;
        timer.everyStyle = everyStyle;
        timer.due = clockOf(args.unscaled) + std::max(0.0, args.delay);
        timer.text = std::make_unique<TimerText>();
        timer.text->origin = luaOrigin(L, 1);
        if (owner && owner->ref){
            timer.owner = ownerOf(owner->ref);
            timer.scene = owner->scene;
            timer.entity = owner->entity;
            if (owner->scene) timer.sceneLife = owner->scene->getLifetime();
            lua_rawgeti(L, LUA_REGISTRYINDEX, owner->ref);
            timer.text->className = instanceClass(L, lua_gettop(L));
            lua_pop(L, 1);
        }
        if (method){
            timer.method = method;
        }else{
            lua_pushvalue(L, callbackIndex);
            timer.fnRef = takeRef(L);
        }
        return addTimer(std::move(timer));
    }

    bool numberArg(lua_State* L, int index, const char* fn, const char* name, double& out){
        if (lua_type(L, index) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, index))){
            lua_pushfstring(L, "%s: %s must be a number of seconds, got %s", fn, name, luaL_typename(L, index));
            return false;
        }
        out = lua_tonumber(L, index);
        return true;
    }

    // self:invoke(fnOrName, delay [, unscaled]) / self:invokeRepeating(fnOrName, delay, interval [, unscaled])
    int invokeImpl(lua_State* L, bool repeating){
        const char* fn = repeating ? "invokeRepeating" : "invoke";
        InstanceInfo info;
        if (!selfInstance(L, fn, info)) return RAISE;
        const int kind = lua_type(L, 2);
        if (kind != LUA_TFUNCTION && kind != LUA_TSTRING){
            lua_pushfstring(L, "%s: a function or a method name expected, got %s", fn, luaL_typename(L, 2));
            return RAISE;
        }
        TimerArgs args;
        if (!numberArg(L, 3, fn, "delay", args.delay)) return RAISE;
        int unscaledIndex = 4;
        if (repeating){
            args.every = true;
            if (!numberArg(L, 4, fn, "interval", args.interval)) return RAISE;
            if (!(args.interval > 0.0)){
                lua_pushfstring(L, "invokeRepeating: interval must be > 0, got %f", args.interval);
                return RAISE;
            }
            unscaledIndex = 5;
        }
        args.unscaled = lua_toboolean(L, unscaledIndex) != 0;
        const TaskId id = kind == LUA_TSTRING
            ? makeLuaTimer(L, 0, lua_tostring(L, 2), args, &info, false)
            : makeLuaTimer(L, 2, nullptr, args, &info, false);
        pushTimerHandle(L, id);
        return 1;
    }

    // Timer.after(delay, fn [, opts]) / Timer.every(interval, fn [, opts])
    int timerImpl(lua_State* L, bool every){
        const char* fn = every ? "Timer.every" : "Timer.after";
        TimerArgs args;
        args.every = every;
        double first = 0.0;
        if (!numberArg(L, 1, fn, every ? "interval" : "delay", first)) return RAISE;
        if (!lua_isfunction(L, 2)){
            lua_pushfstring(L, "%s: fn must be a function, got %s", fn, luaL_typename(L, 2));
            return RAISE;
        }
        if (every){
            args.interval = first;
            args.delay = first;
            if (!(args.interval > 0.0)){
                lua_pushfstring(L, "Timer.every: interval must be > 0, got %f", args.interval);
                return RAISE;
            }
        }else{
            args.delay = first;
        }
        InstanceInfo owner;
        if (!lua_isnoneornil(L, 3)){
            if (!lua_istable(L, 3)){
                lua_pushfstring(L, "%s: opts must be a table ({unscaled, owner, delay, count}), got %s", fn, luaL_typename(L, 3));
                return RAISE;
            }
            lua_getfield(L, 3, "unscaled");
            args.unscaled = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, 3, "owner");
            if (!lua_isnil(L, -1)){
                owner = instanceAt(L, lua_gettop(L));
                if (!owner.ref){
                    lua_pushfstring(L, "%s: opts.owner must be a live script instance (self)", fn);
                    return RAISE;
                }
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "delay");
            if (!lua_isnil(L, -1)){
                if (!every){
                    lua_pushliteral(L, "Timer.after: opts.delay is for Timer.every (the first argument is the delay)");
                    return RAISE;
                }
                if (lua_type(L, -1) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, -1))){
                    lua_pushliteral(L, "Timer.every: opts.delay must be a number of seconds");
                    return RAISE;
                }
                args.delay = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "count");
            if (!lua_isnil(L, -1)){
                if (!every){
                    lua_pushliteral(L, "Timer.after: opts.count is for Timer.every");
                    return RAISE;
                }
                if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1 || lua_tointeger(L, -1) > 0x7fffffff){
                    lua_pushliteral(L, "Timer.every: opts.count must be an integer >= 1");
                    return RAISE;
                }
                args.count = static_cast<int>(lua_tointeger(L, -1));
            }
            lua_pop(L, 1);
        }
        const TaskId id = makeLuaTimer(L, 2, nullptr, args, owner.ref ? &owner : nullptr, every);
        pushTimerHandle(L, id);
        return 1;
    }

    // which of self's timers `selector` (index 2: none, a Timer or a method name) means; RAISE-style
    bool timerSelector(lua_State* L, const char* fn, const void* owner, std::vector<TaskId>& out){
        const int kind = lua_type(L, 2);
        if (kind == LUA_TNONE || kind == LUA_TNIL){
            for (const TimerRecord& timer : S().timers){
                if (timer.owner == owner && !timer.ended) out.push_back(timer.id);
            }
            return true;
        }
        if (kind == LUA_TSTRING){
            const char* name = lua_tostring(L, 2);
            for (const TimerRecord& timer : S().timers){
                if (timer.owner == owner && !timer.ended && timer.method == name) out.push_back(timer.id);
            }
            return true;
        }
        const TaskId id = timerHandleId(L, 2);
        if (!id){
            lua_pushfstring(L, "%s: a Timer or a method name expected, got %s", fn, luaL_typename(L, 2));
            return false;
        }
        const TimerRecord* timer = findTimer(id);
        if (timer && timer->owner != owner){
            lua_pushfstring(L, "%s: that Timer does not belong to this script (use timer:cancel())", fn);
            return false;
        }
        if (timer && !timer->ended) out.push_back(id);
        return true;
    }

    int cancelInvokeImpl(lua_State* L){
        InstanceInfo info;
        if (!selfInstance(L, "cancelInvoke", info)) return RAISE;
        std::vector<TaskId> ids;
        if (!timerSelector(L, "cancelInvoke", ownerOf(info.ref), ids)) return RAISE;
        for (TaskId id : ids){
            if (TimerRecord* timer = findTimer(id)) endTimer(*timer, true);
        }
        return 0;
    }

    int isInvokingImpl(lua_State* L){
        InstanceInfo info;
        if (!selfInstance(L, "isInvoking", info)) return RAISE;
        std::vector<TaskId> ids;
        if (!timerSelector(L, "isInvoking", ownerOf(info.ref), ids)) return RAISE;
        lua_pushboolean(L, ids.empty() ? 0 : 1);
        return 1;
    }

    int stopCoroutineImpl(lua_State* L){
        InstanceInfo info;
        if (!selfInstance(L, "stopCoroutine", info)) return RAISE;
        const void* owner = ownerOf(info.ref);
        std::vector<TaskId> ids;
        if (lua_type(L, 2) == LUA_TSTRING){
            const std::string name = lua_tostring(L, 2);
            ids = liveCoroutines([&](const CoRecord& rec){ return rec.owner == owner && rec.text && rec.text->method == name; });
        }else{
            const TaskId id = coroutineHandleId(L, 2);
            if (!id){
                lua_pushfstring(L, "stopCoroutine: a Coroutine or a method name expected, got %s (stopAllCoroutines stops all)",
                                luaL_typename(L, 2));
                return RAISE;
            }
            const CoRecord* rec = findCo(id);
            if (rec && !rec->ended() && rec->owner != owner){
                lua_pushliteral(L, "stopCoroutine: that coroutine does not belong to this script (use co:stop())");
                return RAISE;
            }
            ids.push_back(id);
        }
        for (TaskId id : ids) stopCoroutineId(id);
        return 0;
    }

    int stopAllCoroutinesImpl(lua_State* L){
        InstanceInfo info;
        if (!selfInstance(L, "stopAllCoroutines", info)) return RAISE;
        const void* owner = ownerOf(info.ref);
        for (TaskId id : liveCoroutines([&](const CoRecord& rec){ return rec.owner == owner; })) stopCoroutineId(id);
        return 0;
    }

    std::string formatSeconds(double seconds){
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", seconds < 0.0 ? 0.0 : seconds);
        return text;
    }

    std::string waitingText(const CoRecord& rec){
        if (rec.status == CoStatus::Running) return "";
        switch (rec.wait){
            case Wait::Frames: {
                const uint64_t frame = S().frame;
                return "frames " + std::to_string(rec.resumeAt > frame ? rec.resumeAt - frame : 1);
            }
            case Wait::Seconds:
                return std::string(rec.unscaled ? "realtime " : "seconds ") + formatSeconds(rec.due - clockOf(rec.unscaled)) + " left";
            case Wait::Fixed: return "fixed update";
            case Wait::EndOfFrame: return "end of frame";
            case Wait::Until: return rec.hasTimeout ? "until (timeout " + formatSeconds(rec.due - clockOf(rec.unscaled)) + " s left)" : "until";
            case Wait::While: return rec.hasTimeout ? "while (timeout " + formatSeconds(rec.due - clockOf(rec.unscaled)) + " s left)" : "while";
            case Wait::Target:
                switch (rec.target){
                    case Target::Coroutine: return "for Coroutine " + std::to_string(rec.targetId);
                    case Target::Timer: return "for Timer " + std::to_string(rec.targetId);
                    case Target::Action: return "for Action";
                    case Target::AudioSource: return "for AudioSource";
                    case Target::AnimationPlayer: return "for AnimationPlayer";
                    case Target::Anime: return "for anime";
                    case Target::IsDone: return "for isDone()";
                    case Target::External: return "for Timer";
                    default: return "for";
                }
            default:
                return "";
        }
    }

    const char* statusName(CoStatus status){
        switch (status){
            case CoStatus::Running: return "running";
            case CoStatus::Suspended: return "suspended";
            case CoStatus::Finished: return "finished";
            case CoStatus::Stopped: return "stopped";
            default: return "error";
        }
    }
}

// ====================================================================== phases

void ScriptScheduler::beginFrame(double scaledDelta, double unscaledDelta){
    State& s = S();
    s.frame++;
    s.scaledClock += scaledDelta;
    s.unscaledClock += unscaledDelta;
    s.frameActive = true;
    if (pixelart::HitStop::isFrameActive())
        applyHitStopToClocks(scaledDelta);
}

void ScriptScheduler::beginFixedStep(){
    State& s = S();
    if (s.frameActive) s.fixedStep++;
}

void ScriptScheduler::fixedPhase(){
    State& s = S();
    if (!s.frameActive || s.inPhase || s.coroutines.empty() || !mainState()) return;
    s.inPhase = true;
    s.depth++;
    const uint64_t step = s.fixedStep;
    const size_t count = s.coroutines.size();
    for (size_t i = 0; i < count; i++){
        const CoRecord& rec = s.coroutines[i];
        if (rec.status != CoStatus::Suspended || rec.wait != Wait::Fixed || rec.resumeAt > step) continue;
        const TaskId id = rec.id;
        if (stopIfInactive(id)) continue;
        resumeSatisfied(id, 0);
    }
    s.depth--;
    s.inPhase = false;
    compact();
}

void ScriptScheduler::updatePhase(){
    State& s = S();
    if (!s.frameActive || s.inPhase || !mainState()) return;
    if (s.coroutines.empty() && s.timers.empty()) return;
    s.inPhase = true;
    s.depth++;
    const uint64_t frame = s.frame;

    // 1) timers due, in creation order, each at most once
    const size_t timerCount = s.timers.size();
    for (size_t i = 0; i < timerCount; i++){
        const TimerRecord& timer = s.timers[i];
        if (timer.ended || timer.firstFrame > frame) continue;
        if (timer.due > clockOf(timer.unscaled) + EPSILON) continue;
        fireTimer(timer.id);
    }

    // 2) coroutines whose wait is over, in creation order (those made in this phase wait)
    const size_t count = s.coroutines.size();
    for (size_t i = 0; i < count; i++){
        CoRecord& rec = s.coroutines[i];
        if (rec.status == CoStatus::Suspended && rec.stopRequested && rec.started){
            // stopped while user code ran it (coroutine.resume): it ends now, whatever it waits for
            const TaskId stopped = rec.id;
            resumeCoroutine(stopped, mainState(), 0);
            continue;
        }
        if (rec.status != CoStatus::Suspended || rec.resumeAt > frame) continue;
        const TaskId id = rec.id;
        switch (rec.wait){
            case Wait::Frames:
                if (stopIfInactive(id)) break;
                resumeSatisfied(id, 0);
                break;
            case Wait::Seconds: {
                const double now = clockOf(rec.unscaled);
                if (rec.due > now + EPSILON) break;
                if (stopIfInactive(id)) break;
                lua_State* co = s.coroutines[i].thread;
                lua_pushnumber(co, now - s.coroutines[i].waitStart);
                resumeSatisfied(id, 1);
                break;
            }
            case Wait::Until:
            case Wait::While: {
                const bool until = rec.wait == Wait::Until;
                if (stopIfInactive(id)) break;
                std::optional<pixelart::HitStop::ScriptScope> ownerScope;
                enterOwnerScope(ownerScope, s.coroutines[i].scene, s.coroutines[i].sceneLife, s.coroutines[i].entity);
                lua_State* L = mainState();
                lua_rawgeti(L, LUA_REGISTRYINDEX, s.coroutines[i].predicateRef);
                if (LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
                    const std::string message = "the " + std::string(until ? "waitUntil" : "waitWhile") +
                                                " predicate failed: " + LuaBinding::getLuaStackErrorString(L, -1);
                    lua_pop(L, 1);
                    failCoroutine(id, message, false);
                    break;
                }
                const bool value = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                CoRecord* again = findCo(id);
                // the predicate is Lua: the coroutine may have been stopped or moved on meanwhile
                if (!again || again->status != CoStatus::Suspended || !again->thread ||
                        (again->wait != Wait::Until && again->wait != Wait::While)) break;
                const bool satisfied = again->wait == Wait::Until ? value : !value;
                if (satisfied){
                    lua_pushboolean(again->thread, 1);
                    resumeSatisfied(id, 1);
                }else if (again->hasTimeout && again->due <= clockOf(again->unscaled) + EPSILON){
                    lua_pushboolean(again->thread, 0);
                    resumeSatisfied(id, 1);
                }
                break;
            }
            case Wait::Target: {
                if (stopIfInactive(id)) break;
                std::string error;
                const int results = pollTarget(id, error);
                if (results == -2){
                    failCoroutine(id, error, false);
                    break;
                }
                if (results >= 0) resumeSatisfied(id, results);
                break;
            }
            default:
                break;
        }
    }
    s.depth--;
    s.inPhase = false;
    compact();
}

void ScriptScheduler::endOfFramePhase(){
    State& s = S();
    if (!s.frameActive) return;
    s.frameActive = false;
    const uint64_t phase = ++s.endFrames;
    if (s.inPhase || s.coroutines.empty() || !mainState()) return;
    s.inPhase = true;
    s.depth++;
    const size_t count = s.coroutines.size();
    for (size_t i = 0; i < count; i++){
        const CoRecord& rec = s.coroutines[i];
        if (rec.status != CoStatus::Suspended || rec.wait != Wait::EndOfFrame || rec.resumeAt > phase) continue;
        const TaskId id = rec.id;
        if (stopIfInactive(id)) continue;
        resumeSatisfied(id, 0);
    }
    s.depth--;
    s.inPhase = false;
    compact();
}

// ====================================================================== ownership

void ScriptScheduler::stopCoroutinesOf(const void* owner){
    if (!owner) return;
    for (TaskId id : liveCoroutines([&](const CoRecord& rec){ return rec.owner == owner; })) stopCoroutineId(id);
    compact();
}

void ScriptScheduler::cancelOwner(const void* owner){
    if (!owner) return;
    State& s = S();
    for (TimerRecord& timer : s.timers){
        if (timer.owner != owner) continue;
        endTimer(timer, true);
        timer.owner = nullptr;
        timer.scene = nullptr;
        timer.sceneLife.reset();
    }
    const std::vector<TaskId> ids = liveCoroutines([&](const CoRecord& rec){ return rec.owner == owner; });
    for (TaskId id : ids) stopCoroutineId(id);
    // the owner (a registry ref) may be reused by the next instance: forgotten here
    for (CoRecord& rec : s.coroutines){
        if (rec.owner != owner) continue;
        indexRemove(rec);
        rec.owner = nullptr;
        rec.scene = nullptr;
        rec.sceneLife.reset();
    }
    compact();
}

namespace {
    // the owners' coroutines among `ids` whose GameObject is gone or no longer active in hierarchy
    void stopInactive(const std::vector<TaskId>& ids){
        std::vector<TaskId> stop;
        for (TaskId id : ids){
            const CoRecord* rec = findCo(id);
            if (!rec || rec->ended() || !rec->owner || !rec->scene) continue;
            Scene* scene = liveScene(rec->scene, rec->sceneLife);
            if (!scene || !scene->isEntityCreated(rec->entity) || !scene->isEntityActiveInHierarchy(rec->entity))
                stop.push_back(id);
        }
        for (TaskId id : stop) stopCoroutineId(id);
    }
}

bool ScriptScheduler::hasCoroutinesIn(const EntityRegistry* registry){
    return registry && S().ownedIndex.count(registry) > 0;
}

void ScriptScheduler::stopCoroutinesOfInactive(const EntityRegistry* registry){
    State& s = S();
    auto found = s.ownedIndex.find(registry);
    if (found == s.ownedIndex.end()) return;
    std::vector<TaskId> ids;
    for (const auto& [entity, list] : found->second) ids.insert(ids.end(), list.begin(), list.end());
    stopInactive(ids);
    compact();
}

void ScriptScheduler::stopCoroutinesOfInactive(const EntityRegistry* registry, const Entity* entities, size_t count){
    State& s = S();
    auto found = s.ownedIndex.find(registry);
    if (found == s.ownedIndex.end() || !entities) return;
    std::vector<TaskId> ids;
    for (size_t i = 0; i < count; i++){
        auto entity = found->second.find(entities[i]);
        if (entity != found->second.end()) ids.insert(ids.end(), entity->second.begin(), entity->second.end());
    }
    if (ids.empty()) return;
    stopInactive(ids);
    compact();
}

void ScriptScheduler::rebind(const void* owner, Scene* scene, Entity entity){
    if (!owner) return;
    State& s = S();
    for (CoRecord& rec : s.coroutines){
        if (rec.owner != owner) continue;
        const bool indexed = rec.indexed;
        indexRemove(rec);
        rec.scene = scene;
        rec.entity = entity;
        rec.sceneLife = scene ? scene->getLifetime() : std::weak_ptr<char>();
        if (indexed) indexAdd(rec);
    }
    for (TimerRecord& timer : s.timers){
        if (timer.owner != owner) continue;
        timer.scene = scene;
        timer.entity = entity;
        timer.sceneLife = scene ? scene->getLifetime() : std::weak_ptr<char>();
    }
}

void ScriptScheduler::clear(){
    State& s = S();
    for (TimerRecord& timer : s.timers) endTimer(timer, true);
    // closing runs to-be-closed variables (Lua): the ids first
    for (TaskId id : liveCoroutines([](const CoRecord&){ return true; })) stopCoroutineId(id);
    if (s.depth > 0) return;   // called from inside a phase: the rest goes with it
    // what the closing variables started meanwhile
    for (TimerRecord& timer : s.timers) endTimer(timer, true);
    for (CoRecord& rec : s.coroutines){
        // a coroutine still running (Play stopped from its own code) loses its thread here
        if (rec.thread){
            s.threads.erase(rec.thread);
            rec.thread = nullptr;
        }
        releaseWaitRefs(rec);
        dropRef(rec.threadRef);
        dropRef(rec.resultsRef);
    }
    s.coroutines.clear();
    s.timers.clear();
    s.threads.clear();
    s.runningStack.clear();
    s.ownedIndex.clear();
}

// ====================================================================== Lua coroutines from C++

TaskId ScriptScheduler::startCoroutine(lua_State* L, int instanceIndex, int fnIndex, int nargs, const char* methodName){
    if (!L || !lua_isfunction(L, fnIndex) || nargs < 0) return 0;
    instanceIndex = (instanceIndex != 0 && !lua_isnoneornil(L, instanceIndex)) ? lua_absindex(L, instanceIndex) : 0;
    fnIndex = lua_absindex(L, fnIndex);
    InstanceInfo owner;
    if (instanceIndex){
        owner = instanceAt(L, instanceIndex);
        if (!owner.ref) return 0;
    }
    const int top = lua_gettop(L);
    const TaskId id = startThread(L, fnIndex, nargs, instanceIndex ? &owner : nullptr, 0, instanceIndex, methodName, true);
    lua_settop(L, top);
    return id;
}

// ====================================================================== C++ timers

TaskId ScriptScheduler::after(double delay, std::function<void()> fn, TimeMode mode, const void* owner){
    if (!fn) return 0;
    TimerRecord timer;
    timer.unscaled = mode == TimeMode::Unscaled;
    timer.due = clockOf(timer.unscaled) + (std::isfinite(delay) ? std::max(0.0, delay) : 0.0);
    timer.owner = owner;
    timer.cpp = std::make_shared<std::function<void()>>(std::move(fn));
    return addTimer(std::move(timer));
}

TaskId ScriptScheduler::every(double delay, double interval, std::function<void()> fn, TimeMode mode, const void* owner, int count){
    if (!fn) return 0;
    if (!(interval > 0.0) || !std::isfinite(interval)){
        Log::error("ScriptScheduler::every: interval must be > 0 (got %f)", interval);
        return 0;
    }
    if (count == 0) return 0;
    TimerRecord timer;
    timer.unscaled = mode == TimeMode::Unscaled;
    timer.due = clockOf(timer.unscaled) + (std::isfinite(delay) ? std::max(0.0, delay) : 0.0);
    timer.interval = interval;
    timer.remaining = count < 0 ? -1 : count;
    timer.owner = owner;
    timer.cpp = std::make_shared<std::function<void()>>(std::move(fn));
    return addTimer(std::move(timer));
}

bool ScriptScheduler::cancel(TaskId id){
    TimerRecord* timer = findTimer(id);
    if (!timer || timer->ended) return false;
    endTimer(*timer, true);
    return true;
}

bool ScriptScheduler::isPending(TaskId id){
    const TimerRecord* timer = findTimer(id);
    return timer && !timer->ended;
}

const void* ScriptScheduler::timerOwner(TaskId id){
    const TimerRecord* timer = findTimer(id);
    return timer ? timer->owner : nullptr;
}

size_t ScriptScheduler::cancelTimersOf(const void* owner){
    size_t cancelled = 0;
    if (!owner) return 0;
    for (TimerRecord& timer : S().timers){
        if (timer.owner == owner && !timer.ended){
            endTimer(timer, true);
            cancelled++;
        }
    }
    return cancelled;
}

// ====================================================================== stats

size_t ScriptScheduler::coroutineCount(){
    size_t n = 0;
    for (const CoRecord& rec : S().coroutines) n += rec.ended() ? 0 : 1;
    return n;
}

size_t ScriptScheduler::timerCount(){
    size_t n = 0;
    for (const TimerRecord& timer : S().timers) n += timer.ended ? 0 : 1;
    return n;
}

size_t ScriptScheduler::coroutineCountOf(const void* owner){
    size_t n = 0;
    if (!owner) return 0;
    for (const CoRecord& rec : S().coroutines) n += (!rec.ended() && rec.owner == owner) ? 1 : 0;
    return n;
}

size_t ScriptScheduler::timerCountOf(const void* owner){
    size_t n = 0;
    if (!owner) return 0;
    for (const TimerRecord& timer : S().timers) n += (!timer.ended && timer.owner == owner) ? 1 : 0;
    return n;
}

std::vector<ScriptCoroutineInfo> ScriptScheduler::coroutinesOf(const void* owner){
    std::vector<ScriptCoroutineInfo> out;
    if (!owner) return out;
    for (const CoRecord& rec : S().coroutines){
        if (rec.ended() || rec.owner != owner) continue;
        ScriptCoroutineInfo info;
        info.id = rec.id;
        info.status = statusName(rec.status);
        info.waiting = waitingText(rec);
        if (rec.text){
            info.origin = rec.text->origin;
            info.method = rec.text->method;
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<ScriptTimerInfo> ScriptScheduler::timersOf(const void* owner){
    std::vector<ScriptTimerInfo> out;
    if (!owner) return out;
    for (const TimerRecord& timer : S().timers){
        if (timer.ended || timer.owner != owner) continue;
        ScriptTimerInfo info;
        info.id = timer.id;
        info.remaining = std::max(0.0, timer.due - clockOf(timer.unscaled));
        info.interval = timer.interval;
        info.unscaled = timer.unscaled;
        info.callback = !timer.method.empty() ? timer.method : timer.fnRef != LUA_NOREF ? "function" : "C++";
        out.push_back(std::move(info));
    }
    return out;
}

size_t ScriptScheduler::luaRefCount(){
    return S().luaRefs;
}

// ====================================================================== handles

ScriptCoroutine::ScriptCoroutine(TaskId id): id(id){
    if (CoRecord* rec = findCo(id)) rec->handles++;
}

ScriptCoroutine::ScriptCoroutine(const ScriptCoroutine& other): id(other.id){
    if (CoRecord* rec = findCo(id)) rec->handles++;
}

ScriptCoroutine& ScriptCoroutine::operator=(const ScriptCoroutine& other){
    if (this != &other){
        if (CoRecord* rec = findCo(id)){
            if (--rec->handles <= 0 && rec->ended()) S().erasable = true;
        }
        id = other.id;
        if (CoRecord* rec = findCo(id)) rec->handles++;
    }
    return *this;
}

ScriptCoroutine::~ScriptCoroutine(){
    if (CoRecord* rec = findCo(id)){
        if (--rec->handles <= 0 && rec->ended()) S().erasable = true;
    }
}

TaskId ScriptCoroutine::getId() const{
    return id;
}

std::string ScriptCoroutine::getStatus() const{
    const CoRecord* rec = findCo(id);
    return rec ? statusName(rec->status) : "stopped";
}

bool ScriptCoroutine::isDone() const{
    const CoRecord* rec = findCo(id);
    return !rec || rec->ended();
}

void ScriptCoroutine::stop(){
    stopCoroutineId(id);
}

std::string ScriptCoroutine::toString() const{
    const CoRecord* rec = findCo(id);
    return "Coroutine(" + std::to_string(id) + ", " + getStatus() +
           (rec && rec->text && !rec->text->origin.empty() ? ", " + rec->text->origin : std::string()) + ")";
}

ScriptTimer::ScriptTimer(TaskId id): id(id){
    if (TimerRecord* timer = findTimer(id)) timer->handles++;
}

ScriptTimer::ScriptTimer(const ScriptTimer& other): id(other.id){
    if (TimerRecord* timer = findTimer(id)) timer->handles++;
}

ScriptTimer& ScriptTimer::operator=(const ScriptTimer& other){
    if (this != &other){
        if (TimerRecord* timer = findTimer(id)){
            if (--timer->handles <= 0 && timer->ended) S().erasable = true;
        }
        id = other.id;
        if (TimerRecord* timer = findTimer(id)) timer->handles++;
    }
    return *this;
}

ScriptTimer::~ScriptTimer(){
    if (TimerRecord* timer = findTimer(id)){
        if (--timer->handles <= 0 && timer->ended) S().erasable = true;
    }
}

TaskId ScriptTimer::getId() const{
    return id;
}

void ScriptTimer::cancel(){
    if (TimerRecord* timer = findTimer(id)) endTimer(*timer, true);
}

bool ScriptTimer::isDone() const{
    const TimerRecord* timer = findTimer(id);
    return !timer || timer->ended;
}

double ScriptTimer::getRemaining() const{
    const TimerRecord* timer = findTimer(id);
    if (!timer || timer->ended) return 0.0;
    return std::max(0.0, timer->due - clockOf(timer->unscaled));
}

double ScriptTimer::getInterval() const{
    const TimerRecord* timer = findTimer(id);
    return timer ? timer->interval : 0.0;
}

bool ScriptTimer::isUnscaled() const{
    const TimerRecord* timer = findTimer(id);
    return timer && timer->unscaled;
}

std::string ScriptTimer::toString() const{
    const TimerRecord* timer = findTimer(id);
    std::string text = "Timer(" + std::to_string(id) + ", ";
    if (!timer || timer->ended) text += (timer && timer->cancelled) ? "cancelled" : "done";
    else text += formatSeconds(getRemaining()) + " s left";
    if (timer && timer->interval > 0.0) text += ", every " + formatSeconds(timer->interval) + " s";
    return text + ")";
}

// ====================================================================== Lua functions

void ScriptScheduler::setTimerAwaitable(ExternalAwaitable awaitable){ externalTimer = awaitable; }

int ScriptSchedulerLua::start(lua_State* L){ return finish(L, coroutineStartImpl(L)); }

int ScriptSchedulerLua::running(lua_State* L){
    auto it = S().threads.find(L);
    if (it == S().threads.end()){
        lua_pushnil(L);
        return 1;
    }
    pushCoroutineHandle(L, it->second);
    return 1;
}

int ScriptSchedulerLua::isInside(lua_State* L){
    lua_pushboolean(L, S().threads.count(L) ? 1 : 0);
    return 1;
}

int ScriptSchedulerLua::wait(lua_State* L){ return finish(L, waitSecondsImpl(L, false)); }
int ScriptSchedulerLua::waitRealtime(lua_State* L){ return finish(L, waitSecondsImpl(L, true)); }
int ScriptSchedulerLua::nextFrame(lua_State* L){ return finish(L, waitFramesImpl(L, "Coroutine.nextFrame", 1)); }
int ScriptSchedulerLua::waitFrames(lua_State* L){ return finish(L, waitFramesArgImpl(L)); }
int ScriptSchedulerLua::waitForFixedUpdate(lua_State* L){ return finish(L, waitPhaseImpl(L, true)); }
int ScriptSchedulerLua::waitForEndOfFrame(lua_State* L){ return finish(L, waitPhaseImpl(L, false)); }
int ScriptSchedulerLua::waitUntil(lua_State* L){ return finish(L, waitPredicateImpl(L, true)); }
int ScriptSchedulerLua::waitWhile(lua_State* L){ return finish(L, waitPredicateImpl(L, false)); }
int ScriptSchedulerLua::waitFor(lua_State* L){ return finish(L, waitForImpl(L)); }

int ScriptSchedulerLua::timerAfter(lua_State* L){ return finish(L, timerImpl(L, false)); }
int ScriptSchedulerLua::timerEvery(lua_State* L){ return finish(L, timerImpl(L, true)); }

int ScriptSchedulerLua::startCoroutine(lua_State* L){
    if (!lua_istable(L, 1))
        return luaL_error(L, "startCoroutine: call it with a colon on a script instance: self:startCoroutine(fn, ...)");
    // the method of a name is looked up here, with its metamethods, before any C++ object
    const bool byName = lua_type(L, 2) == LUA_TSTRING;
    if (byName){
        lua_pushvalue(L, 2);
        lua_gettable(L, 1);
        if (!lua_isfunction(L, -1)){
            lua_pushfstring(L, "startCoroutine: no method '%s' on this script", lua_tostring(L, 2));
            return lua_error(L);
        }
    }else if (!lua_isfunction(L, 2)){
        return luaL_error(L, "startCoroutine: a function or a method name expected, got %s", luaL_typename(L, 2));
    }
    return finish(L, startCoroutineImpl(L, byName));
}

int ScriptSchedulerLua::stopCoroutine(lua_State* L){ return finish(L, stopCoroutineImpl(L)); }
int ScriptSchedulerLua::stopAllCoroutines(lua_State* L){ return finish(L, stopAllCoroutinesImpl(L)); }
int ScriptSchedulerLua::invoke(lua_State* L){ return finish(L, invokeImpl(L, false)); }
int ScriptSchedulerLua::invokeRepeating(lua_State* L){ return finish(L, invokeImpl(L, true)); }
int ScriptSchedulerLua::cancelInvoke(lua_State* L){ return finish(L, cancelInvokeImpl(L)); }
int ScriptSchedulerLua::isInvoking(lua_State* L){ return finish(L, isInvokingImpl(L)); }

int ScriptSchedulerLua::coroutineError(lua_State* L, TaskId id){
    const CoRecord* rec = findCo(id);
    if (rec && rec->status == CoStatus::Error && rec->text){
        lua_pushlstring(L, rec->text->error.data(), rec->text->error.size());
    }else{
        lua_pushnil(L);
    }
    return 1;
}

int ScriptSchedulerLua::coroutineOwner(lua_State* L, TaskId id){
    const CoRecord* rec = findCo(id);
    if (rec && rec->owner){
        lua_rawgeti(L, LUA_REGISTRYINDEX, refOf(rec->owner));
    }else{
        lua_pushnil(L);
    }
    return 1;
}

void ScriptSchedulerLua::addInstanceMethods(lua_State* L, int index){
    index = lua_absindex(L, index);
    const std::pair<const char*, lua_CFunction> methods[] = {
        {"startCoroutine", &ScriptSchedulerLua::startCoroutine},
        {"stopCoroutine", &ScriptSchedulerLua::stopCoroutine},
        {"stopAllCoroutines", &ScriptSchedulerLua::stopAllCoroutines},
        {"invoke", &ScriptSchedulerLua::invoke},
        {"invokeRepeating", &ScriptSchedulerLua::invokeRepeating},
        {"cancelInvoke", &ScriptSchedulerLua::cancelInvoke},
        {"isInvoking", &ScriptSchedulerLua::isInvoking},
    };
    for (const auto& [name, fn] : methods){
        lua_pushcfunction(L, fn);
        lua_setfield(L, index, name);
    }
}
