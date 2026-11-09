// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <cmath>
#include "object/spline/Spline.h"
#include "object/spline/SplineAnimate.h"
#include "object/spline/SplineExtrude.h"
#include "object/spline/SplineInstantiate.h"
#include <unordered_map>
#include "LuaBinding.h"
#include "log/Logger.h"
#include "diag/HangWatchdog.h"
#include "LuaWatchdog.h"
#include "util/SubscriptionOwnerIndex.h"
#include "script/AsyncOwner.h"
#include "shader/ShaderManifest.h"
#include "asset/AssetMap.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include "Log.h"
#include "memory/MemoryStats.h"
#include "debug/DebugOverlay.h"
#include "System.h"
#include "Engine.h"
#include "Scene.h"
#include "SceneManager.h"
#include "manager/ObjectManager.h"
#include "component/ScriptComponent.h"
#include "ScriptProperty.h"
#include "LuaReload.h"
#include "ScriptScheduler.h"
#include "ScriptEvent.h"
#include "ScriptMessages.h"
#include "util/FunctionSubscribe.h"
#include "ScriptRuntime.h"
#include "object/EntityHandle.h"
#include "object/GameObject.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "object/Lattice.h"
#include "object/Mirror.h"
#include "object/Model.h"
#include "object/MeshPolygon.h"
#include "object/Points.h"
#include "object/LineRenderer.h"
#include "object/TrailRenderer.h"
#include "object/ui/CanvasGroup.h"
#include "object/Sprite.h"
#include "object/Terrain.h"
#include "object/Tilemap.h"
#include "object/Light.h"
#include "object/Light2D.h"
#include "object/ShadowCaster2D.h"
#include "object/CapsuleShadowCaster.h"
#include "object/Camera.h"
#include "object/Bone.h"
#include "object/environment/Fog.h"
#include "object/FluidSimulation.h"
#include "object/environment/ReflectionProbe.h"
#include "object/environment/Skybox.h"
#include "object/sound/AudioSource.h"
#include "object/ui/RectTransform.h"
#include "object/ui/LayoutGroup.h"
#include "object/ui/Polygon.h"
#include "object/ui/Text.h"
#include "object/ui/Image.h"
#include "object/ui/Button.h"
#include "object/ui/Panel.h"
#include "object/ui/Scrollbar.h"
#include "object/ui/Progressbar.h"
#include "object/ui/InputField.h"
#include "object/ui/OnScreenControls.h"
#if JASPERENGINE_HTMLUI
#include "object/ui/UIDocument.h"
#endif
#include "subsystem/UIDocumentSystem.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#include "object/physics/Joint2D.h"
#include "object/physics/CharacterController2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "object/physics/Joint3D.h"
#include "object/physics/ControllerColliderHit.h"
#include "object/physics/CharacterController.h"
#endif
#include "action/Action.h"
#include "action/Tween.h"
#include "action/AlphaTween.h"
#include "action/ColorTween.h"
#include "action/PositionTween.h"
#include "action/RotationTween.h"
#include "action/ScaleTween.h"
#include "action/Animation.h"
#include "object/ParticleSystem.h"
#include "action/SpriteAnimation.h"
#include "action/keyframe/KeyframeTracks.h"
#include "action/keyframe/MorphTracks.h"
#include "action/keyframe/LatticeTracks.h"
#include "action/keyframe/RotateTracks.h"
#include "action/keyframe/ScaleTracks.h"
#include "action/keyframe/TranslateTracks.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <memory>

#include "profiler/Profile.h"
#include "pixelart/HitStop.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <execinfo.h>
#include <unistd.h>
#endif

using namespace jasperengine;



lua_State *LuaBinding::luastate = NULL;
uint64_t LuaBinding::luastateGeneration = 0;

// modules the engine itself loads, anything after this is project code
static std::set<std::string> baselineLuaModules;

// require() reads this registry table, package.loaded is only an alias to it
static void collectLoadedModuleNames(lua_State* L, std::set<std::string>& out){
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);

    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        // lua_tostring on a number key would break the traversal
        if (lua_type(L, -2) == LUA_TSTRING) {
            out.insert(lua_tostring(L, -2));
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
}


// ---- the owner thread of the state (see LuaBinding::getLuaState)

namespace {
    std::atomic<std::thread::id> luaThread{};
    std::atomic<int> luaThreadCheck{static_cast<int>(LuaBinding::ThreadCheck::Report)};
    std::atomic<size_t> offThreadLuaUses{0};
    // luaL_newstate's allocator, which the checking one forwards to
    lua_Alloc baseLuaAlloc = nullptr;

    // the engine's global names, taken when the state is made
    std::mutex engineGlobalNamesMutex;
    std::vector<std::string> engineGlobalNames;

    void reportOffThreadLuaUse(){
        const size_t uses = offThreadLuaUses.fetch_add(1, std::memory_order_relaxed);
        const bool abortHere = luaThreadCheck.load(std::memory_order_relaxed) == static_cast<int>(LuaBinding::ThreadCheck::Abort);
        if (uses > 0 && !abortHere)
            return;
        std::fprintf(stderr, "[JasperEngine] the engine's Lua state was used from a thread other than its own "
                             "(Lua states are not thread-safe: run that work on the main thread)%s\n",
                             abortHere ? "; aborting (JASPER_LUA_THREAD_CHECK=abort)" : "");
#if defined(__linux__) && !defined(__ANDROID__)
        void* frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif
        std::fflush(stderr);
        if (abortHere){
            // not a SIGABRT a crash guard (callWithCrashGuard) or crash reporter would catch
            std::signal(SIGABRT, SIG_DFL);
            std::abort();
        }
    }

    // for uses that allocate nothing (a field read, a float's write): the allocator's check below
    // cannot see them
    void checkLuaThread(){
#ifndef NO_THREAD_SUPPORT
        if (std::this_thread::get_id() != luaThread.load(std::memory_order_relaxed))
            reportOffThreadLuaUse();
#endif
    }

    // the Lua heap in the memory stats (get_memory_stats, Jobs & Memory)
    memory::TrackedBytes& luaHeapBytes(){
        static auto* tracked = new memory::TrackedBytes("Lua.Heap", memory::Residency::Cpu);
        return *tracked;
    }

    void* threadCheckedLuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize){
#ifndef NO_THREAD_SUPPORT
        if (std::this_thread::get_id() != luaThread.load(std::memory_order_relaxed))
            reportOffThreadLuaUse();
#endif
        void* result = baseLuaAlloc(ud, ptr, osize, nsize);
        // a new block's osize is its type tag, not a size
        const size_t oldSize = ptr ? osize : 0;
        if (nsize == 0 || result){
            if (nsize > oldSize)
                luaHeapBytes().add(nsize - oldSize, ptr ? 0 : 1);
            else if (oldSize > nsize)
                luaHeapBytes().subtract(oldSize - nsize, nsize == 0 ? 1 : 0);
        }
        return result;
    }

    std::vector<std::string> collectGlobalNames(lua_State* L){
        std::vector<std::string> names;
        lua_pushglobaltable(L);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) names.push_back(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        std::sort(names.begin(), names.end());
        return names;
    }
}

void LuaBinding::setThreadCheck(ThreadCheck mode){
    luaThreadCheck.store(static_cast<int>(mode), std::memory_order_relaxed);
}

LuaBinding::ThreadCheck LuaBinding::getThreadCheck(){
    return static_cast<ThreadCheck>(luaThreadCheck.load(std::memory_order_relaxed));
}

bool LuaBinding::isLuaThread(){
    return std::this_thread::get_id() == luaThread.load(std::memory_order_relaxed);
}

size_t LuaBinding::getOffThreadUseCount(){
    return offThreadLuaUses.load(std::memory_order_relaxed);
}

std::vector<std::string> LuaBinding::getEngineGlobalNames(){
    std::scoped_lock lock(engineGlobalNamesMutex);
    return engineGlobalNames;
}

LuaBinding::LuaBinding() {

}

LuaBinding::~LuaBinding() {

}


void LuaBinding::createLuaState(){
    LuaBinding::luastate = luaL_newstate();
    LuaBinding::luastateGeneration++;

    // the thread making the state owns it; from here on its allocations check the thread
    luaThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    // and is the main thread of the log (its listeners, Log.onMessageReceived)
    Log::setMainThread();
    const char* check = std::getenv("JASPER_LUA_THREAD_CHECK");
    if (check && std::strcmp(check, "abort") == 0)
        setThreadCheck(ThreadCheck::Abort);
    void* allocData = nullptr;
    lua_Alloc alloc = lua_getallocf(luastate, &allocData);
    if (alloc != threadCheckedLuaAlloc){
        baseLuaAlloc = alloc;
        lua_setallocf(luastate, threadCheckedLuaAlloc, allocData);
    }

    registerClasses(luastate);
    configureProjectLuaLoader();
    registerHelpersFunctions(luastate);

    collectLoadedModuleNames(luastate, baselineLuaModules);

    std::vector<std::string> names = collectGlobalNames(luastate);
    std::scoped_lock lock(engineGlobalNamesMutex);
    engineGlobalNames = std::move(names);
}

namespace {
    // registry key of the weak table Material userdata -> drawn shader
    const char* const MATERIAL_SHADER_TABLE = "jasperengine.materialDrawnShader";

    void pushMaterialShaderTable(lua_State* L){
        if (lua_getfield(L, LUA_REGISTRYINDEX, MATERIAL_SHADER_TABLE) == LUA_TTABLE)
            return;
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, MATERIAL_SHADER_TABLE);
    }
}

void LuaBinding::setMaterialDrawnShader(lua_State* L, int index, const std::string& shader){
    index = lua_absindex(L, index);
    if (!lua_isuserdata(L, index))
        return;
    pushMaterialShaderTable(L);
    lua_pushvalue(L, index);
    if (shader.empty())
        lua_pushnil(L);
    else
        lua_pushstring(L, shader.c_str());
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

std::string LuaBinding::getMaterialDrawnShader(lua_State* L, int index){
    index = lua_absindex(L, index);
    if (!lua_isuserdata(L, index))
        return std::string();
    pushMaterialShaderTable(L);
    lua_pushvalue(L, index);
    lua_rawget(L, -2);
    std::string shader = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);
    return shader;
}

Vector4 LuaBinding::getShaderParamValue(const std::string& shader, const std::string& name, const Vector3& value){
    float w = 0.0f;
    if (!shader.empty()){
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
        const ShaderManifestProperty* property = manifest ? manifest->findProperty(name) : nullptr;
        if (property && (property->kind == ShaderPropertyKind::COLOR || property->kind == ShaderPropertyKind::HDRCOLOR))
            w = 1.0f;
    }
    return Vector4(value.x, value.y, value.z, w);
}

lua_State* LuaBinding::getLuaState(){
    return luastate;
}

uint64_t LuaBinding::getLuaStateGeneration(){
    return luastateGeneration;
}

lua_State* LuaBinding::mainThread(lua_State* L){
    if (!L) return nullptr;
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State* main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main ? main : L;
}


std::string LuaBinding::getLuaStackErrorString(lua_State* L, int index) {
    if (lua_isstring(L, index)) {
        size_t len = 0;
        const char* str = lua_tolstring(L, index, &len);
        return str ? std::string(str, len) : std::string();
    }

    if (luaL_callmeta(L, index, "__tostring")) {
        if (lua_isstring(L, -1)) {
            std::string result = lua_tostring(L, -1);
            lua_pop(L, 1);
            return result;
        }
        lua_pop(L, 1);
    }

    return std::string("(") + luaL_typename(L, index) + " error value)";
}

#if JASPERENGINE_PROFILER
namespace {
    // A Lua call in the profiler: the outermost one in a "Lua" scope (the CPU chart's Scripts),
    // and with Deep Profile Lua every callback in a scope named by its chunk and line
    class LuaProfileScope {
    public:
        LuaProfileScope(lua_State* L, int function){
            if (!profiler::isEnabled())
                return;
            static profiler::ScopeSite luaSite{"Lua", __FILE__, (uint32_t)__LINE__};
            if (depth()++ == 0){
                const uint32_t id = luaSite.id.load(std::memory_order_relaxed);
                outer = profiler::beginScope(id ? id : profiler::registerSite(luaSite));
            }
            counted = true;
            if (profiler::isDeepLua() && lua_isfunction(L, function)){
                lua_Debug info;
                lua_pushvalue(L, function);
                if (lua_getinfo(L, ">S", &info)){
                    char name[160];
                    std::snprintf(name, sizeof(name), "Lua %s:%d", info.short_src, info.linedefined);
                    deep = profiler::beginScope(profiler::internName(name));
                }
            }
        }
        ~LuaProfileScope(){
            if (deep)
                profiler::endScope();
            if (counted && --depth() == 0 && outer)
                profiler::endScope();
        }
        LuaProfileScope(const LuaProfileScope&) = delete;
        LuaProfileScope& operator=(const LuaProfileScope&) = delete;
    private:
        bool counted = false;
        bool outer = false;
        bool deep = false;
        static int& depth(){
            thread_local int value = 0;
            return value;
        }
    };
}
#endif

int LuaBinding::pcallWithTraceback(lua_State* L, int nargs, int nresults){
    // a stall in here is a Lua phase: the hang watchdog names the function, and in editor Play
    // may interrupt it (script/LuaWatchdog.h)
    diag::PhaseScope phase("lua");
#if JASPERENGINE_PROFILER
    LuaProfileScope profile(L, lua_gettop(L) - nargs);
#endif
    //message handler goes below the function and its arguments
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, handleLuaError);
    lua_insert(L, base);

    int status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);

    return status;
}

void LuaBinding::luaCallback(int nargs, int nresults, int msgh){
    diag::PhaseScope phase("lua");
#if JASPERENGINE_PROFILER
    LuaProfileScope profile(luastate, lua_gettop(luastate) - nargs);
#endif
    int status = lua_pcall(luastate, nargs, nresults, msgh);
    if (status != 0){
        // an interrupted runaway function is logged like any error here
        LuaWatchdog::takeInterrupt();
        logLuaError("Lua Error: %s", getLuaStackErrorString(luastate, -1).c_str());
        lua_pop(luastate, 1);
    }
}

void LuaBinding::stackDump (lua_State *L) {
    int i = lua_gettop(L);
    Log::debug(" ----------------  Stack Dump ----------------" );
    while(  i   ) {
        int t = lua_type(L, i);
        switch (t) {
            case LUA_TSTRING:
                Log::debug("%d:`%s'", i, lua_tostring(L, i));
                break;
            case LUA_TBOOLEAN:
                Log::debug("%d: %s",i,lua_toboolean(L, i) ? "true" : "false");
                break;
            case LUA_TNUMBER:
                Log::debug("%d: %g",  i, lua_tonumber(L, i));
                break;
            default: Log::debug("%d: %s", i, lua_typename(L, t)); break;
        }
        i--;
    }
    Log::debug("--------------- Stack Dump Finished ---------------" );
}

void LuaBinding::configureProjectLuaLoader() {

    lua_State *L = luastate;

    // Replace the default searchers with the project asset loader.
    lua_getglobal(L, "package");
    if(lua_isnil(L, -1))
        luaL_error(L, "package table does not exist.");

    lua_getfield(L, -1, "searchers");
    if(lua_isnil(L, -1))
        luaL_error(L, "package.loaders table does not exist.");

    size_t numloaders = lua_rawlen(L, -1);

    for (int i = 0; i < numloaders; i++) {
        lua_pushnil(L);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushcfunction(L, moduleLoader);
    lua_rawseti(L, -2, 1);

    lua_pop(L, 1);
}

// ui-multitouch D4: a method named like an EventSystem callback (onPointerDown, onDrag...)
// subscribed to a UIComponent pointer event (x, y) is the old pattern: the engine does not
// also call it with a PointerEventData while that subscription lives
static void noteUIPointerSubscription(lua_State* L, int eventIndex, int selfIndex, const char* methodName, const char* tag) {
    ScriptMessage message;
    if (!methodName || !tag || !ScriptMessages::fromMethodName(methodName, message) || message < ScriptMessage::PointerDown) {
        return;
    }
    if (luabridge::isInstance<FunctionSubscribe<void(float,float)>>(L, eventIndex)) {
        auto event = luabridge::get<FunctionSubscribe<void(float,float)>*>(L, eventIndex);
        if (event && event.value()) {
            ScriptMessages::noteLegacySubscription(lua_topointer(L, selfIndex), methodName, event.value()->subscriptionProbe(tag));
        }
        return;
    }
    // ui-components: the selection callbacks share names with argument-less component events
    // (an InputField's onSubmit, a Panel's onMove): such a subscription is the old pattern too
    if (message >= ScriptMessage::Select && luabridge::isInstance<FunctionSubscribe<void()>>(L, eventIndex)) {
        auto event = luabridge::get<FunctionSubscribe<void()>*>(L, eventIndex);
        if (event && event.value()) {
            ScriptMessages::noteLegacySubscription(lua_topointer(L, selfIndex), methodName, event.value()->subscriptionProbe(tag));
        }
    }
}

// Common implementation for event registration
int LuaBinding::luaRegisterEventImpl(lua_State* L, int eventIndex, int selfIndex, const char* methodName, const char* tag, bool guardActive) {
    // Look up self[methodName] using __index
    lua_pushvalue(L, selfIndex);
    lua_pushstring(L, methodName);
    lua_gettable(L, -2);
    lua_remove(L, -2);

    int methodFuncIndex = lua_gettop(L);
    if (!lua_isfunction(L, methodFuncIndex)) {
        return luaL_error(L, "Event registration: method '%s' not found on script", methodName);
    }

    // Tag stays on the Lua stack, error raises below longjmp past destructors
    pushSubscriptionTag(L, selfIndex, methodName, tag);
    int tagIndex = lua_gettop(L);

    // A scene script's engine events (onUpdate, input...) are skipped while its
    // GameObject is inactive or its entry disabled; other tables are never guarded. Where the
    // script is (self.scene, self.entity) is read at each call: a persistent object's
    // GameObject moves to another scene, and a deleted scene is never reached through it.
    // Build closure: function(...) return method(self, ...) end
    lua_pushvalue(L, methodFuncIndex); // method
    lua_pushvalue(L, selfIndex);       // self
    lua_pushboolean(L, guardActive ? 1 : 0);
    lua_pushcclosure(L,
        [](lua_State* Linner) -> int {
            int nargs = lua_gettop(Linner);
            // the GameObject of a scene script: Time.deltaTime inside it follows its Hit Stop
            // exemption (S48)
            Scene* ownerScene = nullptr;
            Entity ownerEntity = NULL_ENTITY;

            if (lua_toboolean(Linner, lua_upvalueindex(3))) {
                const int self = lua_upvalueindex(2);
                lua_pushstring(Linner, "__released");
                lua_rawget(Linner, self);
                const bool released = lua_toboolean(Linner, -1) != 0;
                lua_pop(Linner, 1);
                if (released) {
                    return 0;
                }
                Scene* scene = nullptr;
                lua_pushstring(Linner, "scene");
                lua_rawget(Linner, self);
                if (lua_isuserdata(Linner, -1)) {
                    auto sceneResult = luabridge::get<Scene*>(Linner, -1);
                    if (sceneResult) {
                        scene = sceneResult.value();
                    }
                }
                lua_pop(Linner, 1);
                lua_pushstring(Linner, "entity");
                lua_rawget(Linner, self);
                const lua_Integer entity = lua_isinteger(Linner, -1) ? lua_tointeger(Linner, -1) : 0;
                lua_pop(Linner, 1);
                lua_pushstring(Linner, "__scriptRef");
                lua_rawget(Linner, self);
                const lua_Integer ref = lua_isinteger(Linner, -1) ? lua_tointeger(Linner, -1) : 0;
                lua_pop(Linner, 1);
                void* instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
                if (scene && entity > 0 && !isScriptActive(scene, static_cast<Entity>(entity), instance)) {
                    return 0;
                }
                if (scene && entity > 0) {
                    ownerScene = scene;
                    ownerEntity = static_cast<Entity>(entity);
                }
            }

            lua_pushvalue(Linner, lua_upvalueindex(1)); // method
            lua_pushvalue(Linner, lua_upvalueindex(2)); // self

            for (int i = 1; i <= nargs; ++i) {
                lua_pushvalue(Linner, i);
            }

            // lua_error below unwinds the script frames, get the traceback here (the scope
            // ends before it: lua_error does not run destructors)
            int status;
            {
                pixelart::HitStop::ScriptScope owner(ownerScene, ownerEntity);
                status = pcallWithTraceback(Linner, 1 + nargs, LUA_MULTRET);
            }
            if (status != LUA_OK) {
                return lua_error(Linner);
            }
            // Filters like preSolve2D read what the method returns
            return lua_gettop(Linner) - nargs;
        },
        3);
    int closureIndex = lua_gettop(L);

    // a Lua Event: the listener is the script's own, dropped once it is released
    if (luabridge::isInstance<ScriptEvent>(L, eventIndex)) {
        auto event = luabridge::get<ScriptEvent*>(L, eventIndex);
        if (event && event.value()) {
            event.value()->addTagged(L, eventIndex, lua_tostring(L, tagIndex), closureIndex, guardActive ? 0 : selfIndex);
            return 0;
        }
    }

    // event:add(tag, closure)
    lua_getfield(L, eventIndex, "add");
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "Event registration: event object has no 'add' method");
    }

    lua_pushvalue(L, eventIndex);
    lua_pushvalue(L, tagIndex);
    lua_pushvalue(L, closureIndex);

    // the subscription belongs to the script (its table), explicit tag or not: releasing the
    // script removes it (SubscriptionOwnerIndex::releaseOwner)
    const uint64_t previousOwner = SubscriptionOwnerIndex::setRegistrationOwner(ownerKeyOf(lua_topointer(L, selfIndex)));
    const int status = pcallWithTraceback(L, 3, 0);
    SubscriptionOwnerIndex::setRegistrationOwner(previousOwner);
    if (status != LUA_OK) {
        return lua_error(L);
    }

    // a hot reload during Play replays it against the new method (LuaReload)
    lua_reload::recordRegistration(L, selfIndex, eventIndex, methodName, lua_tostring(L, tagIndex), guardActive);

    noteUIPointerSubscription(L, eventIndex, selfIndex, methodName, lua_tostring(L, tagIndex));
    return 0;
}

bool LuaBinding::replayScriptEvent(lua_State* L, int eventIndex, int selfIndex, const std::string& method,
                                   const std::string& tag, bool guarded, std::string& error) {
    eventIndex = lua_absindex(L, eventIndex);
    selfIndex = lua_absindex(L, selfIndex);
    lua_pushcfunction(L, [](lua_State* Linner) -> int {
        // (event, self, method, tag, guarded)
        return luaRegisterEventImpl(Linner, 1, 2, lua_tostring(Linner, 3), lua_tostring(Linner, 4), lua_toboolean(Linner, 5) != 0);
    });
    lua_pushvalue(L, eventIndex);
    lua_pushvalue(L, selfIndex);
    lua_pushstring(L, method.c_str());
    lua_pushstring(L, tag.c_str());
    lua_pushboolean(L, guarded);
    if (lua_pcall(L, 5, 0, 0) != LUA_OK) {
        error = getLuaStackErrorString(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void LuaBinding::pushSubscriptionTag(lua_State* L, int selfIndex, const char* methodName, const char* tag) {
    selfIndex = lua_absindex(L, selfIndex);
    if (tag) {
        lua_pushstring(L, tag);
        return;
    }
    lua_getfield(L, selfIndex, "__name");
    const char* typeName = lua_tostring(L, -1);
    if (!typeName) {
        lua_pop(L, 1);
        lua_getfield(L, selfIndex, "name");
        typeName = lua_tostring(L, -1);
    }
    if (!typeName)
        typeName = "Script";

    // Get pointer address of the table
    const void* ptr = lua_topointer(L, selfIndex);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);

    // Format: ClassName_Address_MethodName
    std::string tagStr = std::string(typeName) + "_" + std::to_string(address) + "_" + methodName;
    lua_pushstring(L, tagStr.c_str());

    lua_remove(L, -2); // pop typeName
}

int LuaBinding::pushEngineEvent(lua_State* L, const char* caller, const char* methodName) {
    lua_getglobal(L, "Engine");
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "%s: Engine table not found", caller);
    }
    lua_getfield(L, -1, methodName);
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "%s: Engine.%s not found", caller, methodName);
    }
    lua_remove(L, -2);
    return 0;
}

// The registration's tag, then event:remove(tag): true when a subscription went
static int removeSubscription(lua_State* L, int eventIndex, int tagIndex, const char* caller) {
    eventIndex = lua_absindex(L, eventIndex);
    tagIndex = lua_absindex(L, tagIndex);
    lua_getfield(L, eventIndex, "remove");
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "%s: the event object has no 'remove' method", caller);
    }
    lua_pushvalue(L, eventIndex);
    lua_pushvalue(L, tagIndex);
    if (LuaBinding::pcallWithTraceback(L, 2, 1) != LUA_OK) {
        return lua_error(L);
    }
    lua_pushboolean(L, lua_toboolean(L, -1));
    return 1;
}

// UnregisterEvent(self, event, methodName [, tag])
int LuaBinding::luaUnregisterEvent(lua_State* L) {
    const int top = lua_gettop(L);
    if (top < 3)
        return luaL_error(L, "UnregisterEvent(self, event, methodName [, tag]) requires at least 3 arguments");
    if (!lua_istable(L, 1))
        return luaL_error(L, "UnregisterEvent: 'self' must be a table");
    if (!lua_isstring(L, 3))
        return luaL_error(L, "UnregisterEvent: methodName must be a string");
    if (lua_isnil(L, 2))
        return luaL_error(L, "UnregisterEvent: event is nil");
    const char* tag = (top >= 4 && !lua_isnil(L, 4)) ? lua_tostring(L, 4) : nullptr;
    pushSubscriptionTag(L, 1, lua_tostring(L, 3), tag);
    return removeSubscription(L, 2, -1, "UnregisterEvent");
}

// UnregisterEngineEvent(self, methodName [, tag])
int LuaBinding::luaUnregisterEngineEvent(lua_State* L) {
    const int top = lua_gettop(L);
    if (top < 2)
        return luaL_error(L, "UnregisterEngineEvent(self, methodName [, tag]) requires at least 2 arguments");
    if (!lua_istable(L, 1))
        return luaL_error(L, "UnregisterEngineEvent: 'self' must be a table");
    if (!lua_isstring(L, 2))
        return luaL_error(L, "UnregisterEngineEvent: methodName must be a string");
    const char* methodName = lua_tostring(L, 2);
    const char* tag = (top >= 3 && !lua_isnil(L, 3)) ? lua_tostring(L, 3) : nullptr;
    pushEngineEvent(L, "UnregisterEngineEvent", methodName);
    const int eventIndex = lua_gettop(L);
    pushSubscriptionTag(L, 1, methodName, tag);
    return removeSubscription(L, eventIndex, -1, "UnregisterEngineEvent");
}

// Generic: RegisterEvent(self, event, methodName [, tag])
int LuaBinding::luaRegisterEvent(lua_State* L) {
    int top = lua_gettop(L);
    if (top < 3)
        return luaL_error(L, "RegisterEvent(self, event, methodName [, tag]) requires at least 3 arguments");

    if (!lua_istable(L, 1))
        return luaL_error(L, "RegisterEvent: 'self' must be a table");

    if (!lua_isstring(L, 3))
        return luaL_error(L, "RegisterEvent: methodName must be a string");

    const char* methodName = lua_tostring(L, 3);
    const char* tag = (top >= 4 && !lua_isnil(L, 4)) ? lua_tostring(L, 4) : nullptr;

    return luaRegisterEventImpl(L, 2, 1, methodName, tag, false);
}

// Engine-specific: RegisterEngineEvent(self, methodName [, tag])
int LuaBinding::luaRegisterEngineEvent(lua_State* L) {
    int top = lua_gettop(L);
    if (top < 2)
        return luaL_error(L, "RegisterEngineEvent(self, methodName [, tag]) requires at least 2 arguments");

    if (!lua_istable(L, 1))
        return luaL_error(L, "RegisterEngineEvent: 'self' must be a table");

    if (!lua_isstring(L, 2))
        return luaL_error(L, "RegisterEngineEvent: methodName must be a string");

    const char* methodName = lua_tostring(L, 2);
    const char* tag = (top >= 3 && !lua_isnil(L, 3)) ? lua_tostring(L, 3) : nullptr;

    // Map methodName to Engine event
    pushEngineEvent(L, "RegisterEngineEvent", methodName);
    int eventIndex = lua_gettop(L);

    return luaRegisterEventImpl(L, eventIndex, 1, methodName, tag, true);
}

namespace {
    // An editor script (S23: a file under a folder named Editor with ---@menu, ---@toolbar or
    // ---@validate annotations) belongs to the editor: a game never require()s it
    bool isEditorScriptModule(const std::string& chunkPath, Data& data){
        bool editorFolder = false;
        size_t start = 0;
        while (start < chunkPath.size()) {
            const size_t slash = chunkPath.find('/', start);
            if (slash == std::string::npos) break;
            std::string part = chunkPath.substr(start, slash - start);
            std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c){ return std::tolower(c); });
            if (part == "editor") editorFolder = true;
            start = slash + 1;
        }
        if (!editorFolder) return false;
        const std::string_view text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        for (const char* annotation : {"---@menu ", "---@toolbar ", "---@validate "}) {
            if (text.find(annotation) != std::string_view::npos) return true;
        }
        return false;
    }
}

int LuaBinding::moduleLoader(lua_State *L) {
    
    const char *filename = lua_tostring(L, 1);
    std::string separator(1, System::instance().getDirSeparator());
    filename = luaL_gsub(L, filename, ".", separator.c_str());
    
    std::string filepath;
    Data filedata;
    // chunks are named "@<path>" (standard Lua message form), with forward slashes
    std::string chunkPath(lua_tostring(L, 1));
    std::replace(chunkPath.begin(), chunkPath.end(), '.', '/');
    chunkPath += ".lua";

    filepath = std::string("lua://") + filename + ".lua";
    filedata.open(filepath.c_str());
    if (filedata.getMemPtr() != NULL && isEditorScriptModule(chunkPath, filedata)) {
        lua_pushfstring(L, "\n\t'%s' is an editor script (an Editor folder with ---@menu annotations): only the editor runs it", chunkPath.c_str());
        return 1;
    }
    if (filedata.getMemPtr() != NULL) {
        
        luaL_loadbuffer(L, (const char *) filedata.getMemPtr(), filedata.length(),
                        lua_reload::chunkName(chunkPath).c_str());
        
        return 1;
    }

    filepath = "lua://" + std::string("lua") + System::instance().getDirSeparator() + filename + ".lua";
    filedata.open(filepath.c_str());
    if (filedata.getMemPtr() != NULL && isEditorScriptModule("lua/" + chunkPath, filedata)) {
        lua_pushfstring(L, "\n\t'lua/%s' is an editor script (an Editor folder with ---@menu annotations): only the editor runs it", chunkPath.c_str());
        return 1;
    }
    if (filedata.getMemPtr() != NULL) {
        
        luaL_loadbuffer(L, (const char *) filedata.getMemPtr(), filedata.length(),
                        lua_reload::chunkName("lua/" + chunkPath).c_str());
        
        return 1;
    }
    
    lua_pushstring(L, "\n\tno file in assets directory");
    
    return 1;
}

//The same msghandler of lua.c
int LuaBinding::handleLuaError(lua_State* L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) {  /* is error object not a string? */
    if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
        return 1;  /* that is the message */
    else
        msg = lua_pushfstring(L, "(error object is a %s value)",
                                luaL_typename(L, 1));
    }
    if (strstr(msg, "stack traceback:") != NULL)
        return 1;  /* a nested handler already added one */

    luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
    return 1;  /* return the traceback */
}

void LuaBinding::init(){

    lua_State *L = luastate;

    std::string luadir = std::string("lua") + System::instance().getDirSeparator();

    std::string luafile = std::string("lua://") + "main.lua";
    std::string luafile_subdir = std::string("lua://") + luadir + "main.lua";

    Data filedata;
    std::string chunk = lua_reload::chunkName("main.lua");

    //First try open on root assets dir
    if (filedata.open(luafile.c_str()) != FileErrors::FILEDATA_OK){
        //Second try to open on lua dir
        filedata.open(luafile_subdir.c_str());
        chunk = lua_reload::chunkName("lua/main.lua");
    }

    if (luaL_loadbuffer(L,(const char*)filedata.getMemPtr(),filedata.length(), chunk.c_str()) == 0){
        if(pcallWithTraceback(L, 0, LUA_MULTRET) != 0){
            logLuaError("Lua Error: %s", getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
            lua_close(L);
        }
    }else{
        logLuaError("Lua Error: %s", getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        lua_close(L);
    }

}

void LuaBinding::registerClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS
    // luaL_openlibs() open all libraries: base, string, io, os, package, table, debug
    luaL_openlibs(L);

    registerCoreClasses(L);
    registerGameObjectClasses(L);
    registerActionClasses(L);
    registerECSClasses(L);
    registerAudioClasses(L);
    registerIOClasses(L);
    registerMathClasses(L);
    // after the GameObject and math classes: the module proxies on ParticleSystem (MinMaxCurve values)
    registerParticleClasses(L);
    registerNetClasses(L);
    // the coroutines, invoke and Time (ScriptScheduler); the one global Timer, on the C++
    // Scheduler, is the util classes' (its handles also work with Coroutine.waitFor)
    registerCoroutineClasses(L);
    registerNativeClasses(L);
    registerUtilClasses(L);
    registerThreadClasses(L);
    registerProfilerClasses(L);
    registerGameplayClasses(L);
    registerNavigationClasses(L);
    registerSplineClasses(L);
    // after the GameObject classes: Palette Swap's members on Sprite, Tilemap and MeshPolygon
    registerPixelArtClasses(L);
    registerHitStopClasses(L);
    registerTransitionClasses(L);
    registerCrashClasses(L);
#if JASPERENGINE_HTMLUI
    registerHtmlUIClasses(L);
    registerHtmlUIAnimeClasses(L);
    registerHtmlUIVectorClasses(L);
#if JASPERENGINE_VIDEO
    registerHtmlUIMediaClasses(L);
#endif
    registerHtmlUIWidgetClasses(L);
#endif
    registerScriptClasses(L);

#endif //DISABLE_LUA_BINDINGS
}

// Lua's print, through the logger (category lua, with the chunk and line it was called from)
// instead of stdout: the editor's Console shows it (Play and edit time alike), and a game's
// platform log (logcat, the terminal) gets it. Rate limited like every record.
static int luaPrint(lua_State* L){
    LogCategory& category = logCategoryLua();
    if (!Logger::shouldLog(LogLevel::Info, category)) return 0;
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    LuaBinding::logFromLua(L, LogLevel::Info, category, line);
    return 0;
}

void LuaBinding::logLuaError(const char* format, ...){
    LogCategory& category = logCategoryLua();
    if (!Logger::shouldLog(LogLevel::Error, category)) return;
    char buffer[2048];
    va_list args;
    va_start(args, format);
    va_list sizing;
    va_copy(sizing, args);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, sizing);
    va_end(sizing);
    std::string text;
    if (length >= static_cast<int>(sizeof(buffer))) {
        text.resize(static_cast<size_t>(length) + 1);
        std::vsnprintf(&text[0], text.size(), format, args);
        text.resize(static_cast<size_t>(length));
    } else if (length > 0) {
        text.assign(buffer, static_cast<size_t>(length));
    }
    va_end(args);
    Logger::write(LogLevel::Error, category, text, nullptr, LogFlagLua | LogFlagNoBacktrace, format);
}

void LuaBinding::logFromLua(lua_State* L, LogLevel level, const LogCategory& category, std::string_view text,
                            const std::string& stackTrace){
    // the calling Lua function's chunk and line (level 1: the caller of this C function)
    lua_Debug ar{};
    LogSource source{nullptr, nullptr, 0};
    if (L && lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar) && ar.currentline > 0) {
        source.file = ar.short_src;
        source.function = ar.name ? ar.name : "";
        source.line = static_cast<uint32_t>(ar.currentline);
    }
    if (stackTrace.empty()) {
        Logger::write(level, category, text, source.file ? &source : nullptr, LogFlagLua | LogFlagNoBacktrace);
        return;
    }
    Log::emitEntry(level, std::string(text), stackTrace, true, &category, source.file ? &source : nullptr);
}

void LuaBinding::registerHelpersFunctions(lua_State *L){
    lua_pushcfunction(L, luaPrint);
    lua_setglobal(L, "print");

    // Event helper table
    lua_newtable(L);

    lua_pushcfunction(L, luaRegisterEvent);
    lua_setglobal(L, "RegisterEvent");

    lua_pushcfunction(L, luaRegisterEngineEvent);
    lua_setglobal(L, "RegisterEngineEvent");

    lua_pushcfunction(L, luaUnregisterEvent);
    lua_setglobal(L, "UnregisterEvent");

    lua_pushcfunction(L, luaUnregisterEngineEvent);
    lua_setglobal(L, "UnregisterEngineEvent");

    // Unity's global DontDestroyOnLoad(obj) (also GameObject.dontDestroyOnLoad)
    lua_pushcfunction(L, luaDontDestroyOnLoad);
    lua_setglobal(L, "DontDestroyOnLoad");
}


namespace {
    // Compiled script files by path (registry refs): a script instantiated many times is
    // read and compiled once. The chunk still runs for every instance, which gets a module
    // table of its own as before.
    std::unordered_map<std::string, int> scriptChunks;

    void clearScriptChunks(lua_State* L) {
        if (L) {
            for (const auto& [path, ref] : scriptChunks) {
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
            }
        }
        scriptChunks.clear();
    }
}

void LuaBinding::cleanup(){
    // nothing may call into the state once it is closed: a line logged later would reach the
    // scripts' log listeners, a console command its Lua function
    Log::onMessageReceived.clear();
    DebugOverlay::removeOwnedCommands();
    // no coroutine thread or timer reference may outlive the state
    ScriptScheduler::clear();
    clearScriptChunks(luastate);
    lua_reload::clearGenerations();
    if (luastate) {
        lua_close(luastate);
        luastate = NULL;
    }
}

void LuaBinding::recreateLuaState(){
    cleanup();
    createLuaState();
    LuaWatchdog::attach();
}

void LuaBinding::removeScriptSubscriptions(int luaRef){
    if (!luastate) return;
    lua_rawgeti(luastate, LUA_REGISTRYINDEX, luaRef);
    const void* table = lua_topointer(luastate, -1);
    lua_pop(luastate, 1);
    // every event subscription (engine, components, physics) and timer of the script
    if (table) SubscriptionOwnerIndex::releaseOwner(ownerKeyOf(table));
}

// "_<address>_": the tag part every subscription of the script's table carries
std::string LuaBinding::getScriptSubscriptionTag(int luaRef){
    if (!luastate) return "";

    lua_rawgeti(luastate, LUA_REGISTRYINDEX, luaRef);
    const void* ptr = lua_topointer(luastate, -1);
    std::string addr;
    if (ptr) {
        addr = "_" + std::to_string(reinterpret_cast<std::uintptr_t>(ptr)) + "_";
    }
    lua_pop(luastate, 1);

    return addr;
}

void LuaBinding::releaseLuaRef(int luaRef){
    if (!luastate) return;
    luaL_unref(luastate, LUA_REGISTRYINDEX, luaRef);
}

std::string LuaBinding::normalizePtrTypeName(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.back())) || value.back() == '*' || value.back() == '&'))
        value.pop_back();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return std::tolower(c); });
    return value;
}

template <typename T>
bool LuaBinding::pushEntityHandleTyped(lua_State* L, jasperengine::Scene* scene, jasperengine::Entity entity) {
    T handle(scene, entity);
    if (!luabridge::push<T>(L, handle)) {
        lua_pushnil(L);
        return false;
    }
    return true;
}

bool LuaBinding::pushEntityHandleByType(lua_State* L, jasperengine::Scene* scene, jasperengine::Entity entity, const std::string& ptrTypeName) {
    const std::string key = normalizePtrTypeName(ptrTypeName);

    #define DISPATCH_TYPE(Type, KeyName) \
        if (key == KeyName) return pushEntityHandleTyped<Type>(L, scene, entity)

    DISPATCH_TYPE(Fog, "fog");
    DISPATCH_TYPE(Skybox, "skybox");
    DISPATCH_TYPE(ReflectionProbe, "reflectionprobe");
    DISPATCH_TYPE(FluidSimulation, "fluidsimulation");
    DISPATCH_TYPE(GameObject, "gameobject");
    DISPATCH_TYPE(Camera, "camera");
    DISPATCH_TYPE(Light, "light");
    DISPATCH_TYPE(Light2D, "light2d");
    DISPATCH_TYPE(ShadowCaster2D, "shadowcaster2d");
    DISPATCH_TYPE(CapsuleShadowCaster, "capsuleshadowcaster");
    DISPATCH_TYPE(Mesh, "mesh");
    DISPATCH_TYPE(Primitive, "primitive");
    DISPATCH_TYPE(Lattice, "lattice");
    DISPATCH_TYPE(Mirror, "mirror");
    DISPATCH_TYPE(Terrain, "terrain");
    DISPATCH_TYPE(Tilemap, "tilemap");
    DISPATCH_TYPE(Model, "model");
    DISPATCH_TYPE(Spline, "spline");
    DISPATCH_TYPE(SplineExtrude, "splineextrude");
    DISPATCH_TYPE(SplineInstantiate, "splineinstantiate");
    DISPATCH_TYPE(SplineAnimate, "splineanimate");
    DISPATCH_TYPE(MeshPolygon, "meshpolygon");
    DISPATCH_TYPE(Points, "points");
    DISPATCH_TYPE(LineRenderer, "linerenderer");
    DISPATCH_TYPE(TrailRenderer, "trailrenderer");
    DISPATCH_TYPE(Sprite, "sprite");
    DISPATCH_TYPE(AudioSource, "audiosource");
    DISPATCH_TYPE(RectTransform, "recttransform");
    DISPATCH_TYPE(LayoutGroup, "layoutgroup");
    DISPATCH_TYPE(CanvasGroup, "canvasgroup");
    DISPATCH_TYPE(Polygon, "polygon");
    DISPATCH_TYPE(Text, "text");
    DISPATCH_TYPE(Image, "image");
    DISPATCH_TYPE(Button, "button");
    DISPATCH_TYPE(Panel, "panel");
    DISPATCH_TYPE(Scrollbar, "scrollbar");
    DISPATCH_TYPE(Progressbar, "progressbar");
    DISPATCH_TYPE(InputField, "inputfield");
    DISPATCH_TYPE(OnScreenStick, "onscreenstick");
    DISPATCH_TYPE(OnScreenButton, "onscreenbutton");
    DISPATCH_TYPE(TouchArea, "toucharea");
#if JASPERENGINE_HTMLUI
    DISPATCH_TYPE(UIDocument, "uidocument");
#endif
    DISPATCH_TYPE(Bone, "bone");
#ifdef JASPERENGINE_PHYSICS_2D
    DISPATCH_TYPE(Rigidbody2D, "rigidbody2d");
    DISPATCH_TYPE(Joint2D, "joint2d");
    DISPATCH_TYPE(CharacterController2D, "charactercontroller2d");
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    DISPATCH_TYPE(Rigidbody, "rigidbody");
    DISPATCH_TYPE(Joint3D, "joint3d");
    DISPATCH_TYPE(CharacterController, "charactercontroller");
#endif
    DISPATCH_TYPE(Action, "action");
    DISPATCH_TYPE(Tween, "tween");
    DISPATCH_TYPE(AlphaTween, "alphatween");
    DISPATCH_TYPE(ColorTween, "colortween");
    DISPATCH_TYPE(PositionTween, "positiontween");
    DISPATCH_TYPE(RotationTween, "rotationtween");
    DISPATCH_TYPE(ScaleTween, "scaletween");
    DISPATCH_TYPE(Animation, "animation");
    DISPATCH_TYPE(ParticleSystem, "particlesystem");
    DISPATCH_TYPE(SpriteAnimation, "spriteanimation");
    DISPATCH_TYPE(KeyframeTracks, "keyframetracks");
    DISPATCH_TYPE(MorphTracks, "morphtracks");
    DISPATCH_TYPE(LatticeTracks, "latticetracks");
    DISPATCH_TYPE(RotateTracks, "rotatetracks");
    DISPATCH_TYPE(ScaleTracks, "scaletracks");
    DISPATCH_TYPE(TranslateTracks, "translatetracks");

    #undef DISPATCH_TYPE

    if (!key.empty() && key != "entityhandle") {
        Log::warn("pushEntityHandleByType: unknown type '%s', falling back to EntityHandle", ptrTypeName.c_str());
    }
    return pushEntityHandleTyped<EntityHandle>(L, scene, entity);
}

void LuaBinding::clearLoadedProjectModules() {
    lua_State* L = luastate;
    // scripts may have changed since: compiled again
    clearScriptChunks(L);
    lua_reload::clearGenerations();
    if (!L) return;

    std::set<std::string> loadedModules;
    collectLoadedModuleNames(L, loadedModules);

    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);

    for (const std::string& moduleName : loadedModules) {
        if (baselineLuaModules.count(moduleName) > 0) continue;

        lua_pushnil(L);
        lua_setfield(L, -2, moduleName.c_str());
        Log::debug("Cleared Lua project module cache: %s", moduleName.c_str());
    }

    lua_pop(L, 1);
}

namespace {
    // registry key: the methods every script instance has
    const char* const SCRIPT_INSTANCE_BASE = "jasperengine.ScriptInstanceBase";

    bool isReservedMember(const char* key) {
        return std::strcmp(key, "enabled") == 0 || std::strcmp(key, "isActiveAndEnabled") == 0 ||
               std::strcmp(key, "gameObject") == 0;
    }

    // The scene, GameObject and entry of the script instance at `index` (a table made by
    // createLuaScriptInstance); the entry is null once the instance is released
    ScriptEntry* instanceEntry(lua_State* L, int index, Scene*& scene, Entity& entity, size_t* scriptIndex = nullptr) {
        index = lua_absindex(L, index);
        scene = nullptr;
        entity = NULL_ENTITY;
        lua_pushstring(L, "scene");
        lua_rawget(L, index);
        if (lua_isuserdata(L, -1)) {
            auto result = luabridge::get<Scene*>(L, -1);
            if (result) scene = result.value();
        }
        lua_pop(L, 1);
        lua_pushstring(L, "entity");
        lua_rawget(L, index);
        entity = lua_isinteger(L, -1) ? static_cast<Entity>(lua_tointeger(L, -1)) : NULL_ENTITY;
        lua_pop(L, 1);
        lua_pushstring(L, "__released");
        lua_rawget(L, index);
        const bool released = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_pushstring(L, "__scriptRef");
        lua_rawget(L, index);
        const lua_Integer ref = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
        if (released || !scene || ref == 0 || entity == NULL_ENTITY) {
            return nullptr;
        }
        return ScriptRuntime::findEntry(scene, entity, reinterpret_cast<void*>(static_cast<intptr_t>(ref)), scriptIndex);
    }
}

void LuaBinding::pushScriptInstanceBase(lua_State* L) {
    if (lua_getfield(L, LUA_REGISTRYINDEX, SCRIPT_INSTANCE_BASE) == LUA_TTABLE) {
        return;
    }
    lua_pop(L, 1);
    lua_newtable(L);
    const luaL_Reg methods[] = {
        {"getScript", luaGetScript},
        {"getScripts", luaGetScripts},
        {"getScriptInChildren", luaGetScriptInChildren},
        {"getScriptsInChildren", luaGetScriptsInChildren},
        {"getScriptInParent", luaGetScriptInParent},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, methods, 0);
    // self:startCoroutine, stopCoroutine, stopAllCoroutines, invoke, invokeRepeating, cancelInvoke, isInvoking
    ScriptSchedulerLua::addInstanceMethods(L, -1);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, SCRIPT_INSTANCE_BASE);
}

bool LuaBinding::pushScriptInstance(lua_State* L, const ScriptEntry& entry) {
    if (entry.type != ScriptType::LUA || !entry.instance) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<int>(reinterpret_cast<intptr_t>(entry.instance)));
    return true;
}

namespace {
    int getFieldUnprotected(lua_State* L) {
        lua_gettable(L, 1);
        return 1;
    }

    // the GameObject wrapper of the members table at `members`, made again when the
    // instance's GameObject is another one (a move to another scene)
    void syncGameObject(lua_State* L, int members, Scene* scene, Entity entity) {
        lua_pushstring(L, "gameObject");
        lua_rawget(L, members);
        if (luabridge::isInstance<GameObject>(L, -1)) {
            auto cached = luabridge::get<GameObject*>(L, -1);
            if (cached && cached.value() && cached.value()->getScene() == scene && cached.value()->getEntity() == entity) {
                lua_pop(L, 1);
                return;
            }
        }
        lua_pop(L, 1);
        lua_pushstring(L, "gameObject");
        if (!scene || entity == NULL_ENTITY || !luabridge::push<GameObject>(L, GameObject(scene, entity))) {
            lua_pushnil(L);
        }
        lua_rawset(L, members);
    }

    // pushes the members table of the instance at `index` (nil when it has none)
    void pushMembers(lua_State* L, int index) {
        if (!lua_getmetatable(L, index)) {
            lua_pushnil(L);
            return;
        }
        lua_pushstring(L, "__members");
        lua_rawget(L, -2);
        lua_remove(L, -2);
    }
}

bool LuaBinding::protectedGetField(lua_State* L, int index, const char* key) {
    index = lua_absindex(L, index);
    lua_pushcfunction(L, getFieldUnprotected);
    lua_pushvalue(L, index);
    lua_pushstring(L, key);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return false;
    }
    return true;
}

void LuaBinding::syncScriptMembers(Scene* scene, Entity entity, const ScriptEntry& entry, bool activeAndEnabled) {
    lua_State* L = luastate;
    if (!L || entry.type != ScriptType::LUA || !entry.instance) {
        return;
    }
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<int>(reinterpret_cast<intptr_t>(entry.instance)));
    if (lua_istable(L, -1)) {
        pushMembers(L, -1);
        if (lua_istable(L, -1)) {
            const int members = lua_gettop(L);
            lua_pushstring(L, "enabled");
            lua_pushboolean(L, entry.enabled);
            lua_rawset(L, members);
            lua_pushstring(L, "isActiveAndEnabled");
            lua_pushboolean(L, activeAndEnabled);
            lua_rawset(L, members);
            syncGameObject(L, members, scene, entity);
        }
    }
    lua_settop(L, top);
}

// __index of an instance whose class (the module, upvalue 1) has a metatable of its own (it
// inherits, or is strict) or is shared with other instances: the engine members (upvalue 2),
// the class's own fields, ScriptInstanceBase, then what the class inherits. A plain class needs
// none of this: it holds the members and methods itself, the instance's __index is the class.
int LuaBinding::luaInstanceIndex(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(2));
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);
    pushScriptInstanceBase(L);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 2);
    lua_pushvalue(L, 2);
    lua_gettable(L, lua_upvalueindex(1));
    return 1;
}

// __newindex of a script instance: `self.enabled = x` enables the script (onEnable /
// onDisable), the read-only members refuse, anything else is a field of the instance
int LuaBinding::luaInstanceNewIndex(lua_State* L) {
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 2);
        if (std::strcmp(key, "enabled") == 0) {
            if (!lua_isboolean(L, 3)) {
                return luaL_error(L, "enabled must be a boolean");
            }
            Scene* scene = nullptr;
            Entity entity = NULL_ENTITY;
            size_t index = 0;
            if (instanceEntry(L, 1, scene, entity, &index)) {
                ScriptRuntime::setScriptEnabled(scene, entity, index, lua_toboolean(L, 3) != 0);
            } else {
                lua_pushstring(L, "__name");
                lua_rawget(L, 1);
                Log::warn("enabled set on a script instance (%s) that was released or is not attached: ignored",
                          lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
                lua_pop(L, 1);
            }
            return 0;
        }
        if (isReservedMember(key)) {
            return luaL_error(L, "%s is read-only", key);
        }
    }
    lua_settop(L, 3);
    lua_rawset(L, 1);
    return 0;
}

bool LuaBinding::createLuaScriptInstance(lua_State* L, Scene* scene, Entity entity, ScriptEntry& scriptEntry) {
    for (const auto& prop : scriptEntry.properties) {
        if (isReservedMember(prop.name.c_str())) {
            Log::error("Lua script '%s': property '%s' uses a reserved name (every script has enabled, isActiveAndEnabled "
                       "and gameObject); rename it. The script does not run.", scriptEntry.className.c_str(), prop.name.c_str());
            return false;
        }
    }

    auto chunk = scriptChunks.find(scriptEntry.path);
    if (chunk == scriptChunks.end()) {
        std::string luaFile = std::string("lua://") + scriptEntry.path;
        Data filedata;
        if (filedata.open(luaFile.c_str()) != FileErrors::FILEDATA_OK) {
            Log::error("Lua script file not found: %s", scriptEntry.path.c_str());
            return false;
        }

        const std::string chunkName = lua_reload::chunkName(scriptEntry.path.str());
        int loadStatus = luaL_loadbuffer(L, (const char*)filedata.getMemPtr(), filedata.length(), chunkName.c_str());
        if (loadStatus != LUA_OK) {
            logLuaError("Failed to load Lua file '%s': %s", scriptEntry.path.c_str(), getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
            return false;
        }
        chunk = scriptChunks.emplace(scriptEntry.path, luaL_ref(L, LUA_REGISTRYINDEX)).first;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, chunk->second);

    int status = pcallWithTraceback(L, 0, 1);
    if (status != LUA_OK) {
        logLuaError("Failed to execute Lua module '%s': %s", scriptEntry.className.c_str(), getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        return false;
    }

    if (!lua_istable(L, -1)) {
        Log::error("Lua module '%s' did not return a table", scriptEntry.className.c_str());
        lua_pop(L, 1);
        return false;
    }
    const int moduleIndex = lua_gettop(L);

    // The instance: its fields first (raw), then its metatable
    lua_newtable(L);

    lua_pushstring(L, scriptEntry.className.c_str());
    lua_setfield(L, -2, "__name");

    if (!luabridge::push<Scene*>(L, scene)) {
        Log::error("Failed to push scene to Lua");
        lua_pop(L, 2);
        return false;
    }
    lua_setfield(L, -2, "scene");

    lua_pushinteger(L, static_cast<lua_Integer>(entity));
    lua_setfield(L, -2, "entity");

    // Set script properties (EntityReference ones are resolved in a second pass)
    for (auto& prop : scriptEntry.properties) {
        if (prop.type == ScriptPropertyType::EntityReference) {
            lua_pushnil(L);
        } else {
            pushScriptPropertyValue(L, scene, prop);
        }
        lua_setfield(L, -2, prop.name.c_str());
    }

    // the class may not define the engine's members either (a class another instance already
    // holds them in is shared: below)
    lua_pushstring(L, "__scriptInstanceClass");
    lua_rawget(L, moduleIndex);
    const bool sharedClass = !lua_isnil(L, -1);
    lua_pop(L, 1);
    for (const char* reserved : {"enabled", "isActiveAndEnabled", "gameObject"}) {
        if (sharedClass) break;
        lua_pushstring(L, reserved);
        lua_rawget(L, moduleIndex);
        const bool defined = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (defined) {
            Log::error("Lua script '%s': its table defines '%s', a member every script has (enabled, isActiveAndEnabled, "
                       "gameObject); rename it. The script does not run.", scriptEntry.className.c_str(), reserved);
            lua_pop(L, 2);
            return false;
        }
    }

    // The engine's members of this instance (enabled, isActiveAndEnabled, gameObject: kept
    // current by ScriptRuntime, syncScriptMembers) and ScriptInstanceBase's methods. The class
    // (the module) is one per instance (its chunk runs per instance): a plain one holds them
    // itself, so every lookup on self stays one table hop (instance -> class), as before. A
    // class with a metatable of its own (inheritance, a strict __index), or one shared by
    // several instances, keeps its chain: the members go to a table of their own, asked by a
    // closure.
    lua_pushstring(L, "__scriptInstanceClass");
    lua_rawget(L, moduleIndex);
    const bool shared = !lua_isnil(L, -1);
    lua_pop(L, 1);
    const bool plainModule = !shared && !lua_getmetatable(L, moduleIndex);
    if (!plainModule && !shared) {
        lua_pop(L, 1);
    }
    if (plainModule) {
        lua_pushvalue(L, moduleIndex);
        lua_pushstring(L, "__scriptInstanceClass");
        lua_pushboolean(L, 1);
        lua_rawset(L, moduleIndex);
        // ScriptInstanceBase's methods the class does not define itself
        pushScriptInstanceBase(L);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pushvalue(L, -2);
            lua_rawget(L, moduleIndex);
            const bool own = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (!own) {
                lua_pushvalue(L, -2);
                lua_insert(L, -2);
                lua_rawset(L, moduleIndex);
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    } else {
        lua_newtable(L);
    }
    const int membersIndex = lua_gettop(L);
    lua_pushboolean(L, scriptEntry.enabled);
    lua_setfield(L, membersIndex, "enabled");
    lua_pushboolean(L, scriptEntry.enabled && scene->isEntityCreated(entity) && scene->isEntityActiveInHierarchy(entity));
    lua_setfield(L, membersIndex, "isActiveAndEnabled");
    syncGameObject(L, membersIndex, scene, entity);

    lua_newtable(L);
    if (plainModule) {
        lua_pushvalue(L, moduleIndex);
    } else {
        lua_pushvalue(L, moduleIndex);
        lua_pushvalue(L, membersIndex);
        lua_pushcclosure(L, luaInstanceIndex, 2);
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, luaInstanceNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, membersIndex);
    lua_setfield(L, -2, "__members");
    lua_setmetatable(L, -3);
    lua_pop(L, 1); // members

    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    scriptEntry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    // the refs are reused: the generation tells this instance from an earlier one (main thread)
    static uint64_t instanceGenerations = 0;
    scriptEntry.instanceGeneration = ++instanceGenerations;

    // lets RegisterEngineEvent and the instance's members find this entry
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushstring(L, "__scriptRef");
    lua_pushinteger(L, ref);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    for (auto& prop : scriptEntry.properties) {
        prop.luaRef = ref;
    }

    lua_pop(L, 1); // pop module
    ScriptMessages::notifyReceiversChanged(scene, entity);
    return true;
}

void LuaBinding::registerMathClassesIn(lua_State* L) {
#ifndef DISABLE_LUA_BINDINGS
    registerMathClasses(L);
#endif
}

void LuaBinding::setScriptChunk(lua_State* L, const std::string& path, int index) {
    lua_pushvalue(L, index);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    auto chunk = scriptChunks.find(path);
    if (chunk != scriptChunks.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, chunk->second);
        chunk->second = ref;
    } else {
        scriptChunks.emplace(path, ref);
    }
}

void LuaBinding::pushScriptPropertyValue(lua_State* L, Scene* scene, const ScriptProperty& prop) {
    const ScriptPropertyValue& value = prop.value;
    if (prop.type == ScriptPropertyType::EntityReference) {
        bool found = false;
        if (std::holds_alternative<EntityReference>(value)) {
            const EntityReference& entRef = std::get<EntityReference>(value);
            if (entRef.entity != NULL_ENTITY) {
                Scene* targetScene = entRef.sceneId != 0 ? SceneManager::getScenePtr(entRef.sceneId) : scene;
                found = pushScriptObject(L, targetScene, entRef.entity, prop.ptrTypeName);
            }
        }
        if (!found) lua_pushnil(L);
    } else if (std::holds_alternative<bool>(value)) {
        lua_pushboolean(L, std::get<bool>(value));
    } else if (std::holds_alternative<int>(value)) {
        lua_pushinteger(L, std::get<int>(value));
    } else if (std::holds_alternative<float>(value)) {
        lua_pushnumber(L, std::get<float>(value));
    } else if (std::holds_alternative<std::string>(value)) {
        lua_pushstring(L, std::get<std::string>(value).c_str());
    } else if (std::holds_alternative<Vector2>(value)) {
        if (!luabridge::push<Vector2>(L, std::get<Vector2>(value))) lua_pushnil(L);
    } else if (std::holds_alternative<Vector3>(value)) {
        if (!luabridge::push<Vector3>(L, std::get<Vector3>(value))) lua_pushnil(L);
    } else if (std::holds_alternative<Vector4>(value)) {
        if (!luabridge::push<Vector4>(L, std::get<Vector4>(value))) lua_pushnil(L);
    } else if (std::holds_alternative<PrefabAsset>(value) && !std::get<PrefabAsset>(value).isEmpty()) {
        // a prefab property left at None stays nil
        if (!luabridge::push<PrefabAsset>(L, std::get<PrefabAsset>(value))) lua_pushnil(L);
    } else if (std::holds_alternative<AssetRef>(value) && std::get<AssetRef>(value).isSet()) {
        // an asset property left at None stays nil
        if (!luabridge::push<ScriptAssetRef>(L, ScriptAssetRef(std::get<AssetRef>(value)))) lua_pushnil(L);
    } else {
        lua_pushnil(L);
    }
}

bool LuaBinding::pushScriptObject(lua_State* L, Scene* scene, Entity entity, const std::string& className) {
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
        return false;
    }
    // enabled or not (Unity: a reference to a disabled component is still that component)
    if (ScriptComponent* scriptComp = scene->findComponent<ScriptComponent>(entity)) {
        for (auto& script : scriptComp->scripts) {
            // until it is released (onDestroy still finds it, as Unity's components)
            if (script.type == ScriptType::LUA && script.instance &&
                    (className.empty() || script.className == className)) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<int>(reinterpret_cast<intptr_t>(script.instance)));
                return true;
            }
        }
    }
    return pushEntityHandleByType(L, scene, entity, className);
}

void LuaBinding::resolveLuaScriptReferences(lua_State* L, Scene* scene, ScriptEntry& scriptEntry) {
    int ref = static_cast<int>(reinterpret_cast<intptr_t>(scriptEntry.instance));
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);

    for (auto& prop : scriptEntry.properties) {
        if (prop.type != ScriptPropertyType::EntityReference) continue;

        if (!std::holds_alternative<EntityReference>(prop.value)) {
            lua_pushnil(L);
            lua_setfield(L, -2, prop.name.c_str());
            continue;
        }

        const auto& entRef = std::get<EntityReference>(prop.value);
        bool found = false;
        if (entRef.entity != NULL_ENTITY) {
            Scene* targetScene = scene;
            if (entRef.sceneId != 0) {
                targetScene = SceneManager::getScenePtr(entRef.sceneId);
            }
            found = pushScriptObject(L, targetScene, entRef.entity, prop.ptrTypeName);
        }

        if (!found) lua_pushnil(L);
        lua_setfield(L, -2, prop.name.c_str());
    }

    lua_pop(L, 1);
}

bool LuaBinding::callLuaScriptMethod(Scene* scene, Entity entity, const std::string& method, const std::vector<AnimationEventArg>& args,
                                     bool integers) {
    return ScriptMessages::sendNamed(scene, entity, method, args, integers);
}

#ifdef JASPERENGINE_PHYSICS_3D
void LuaBinding::callScriptHook(Scene* scene, Entity entity, const char* function, const ControllerColliderHit& hit) {
    lua_State* L = luastate;
    if (!L || !scene) return;
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    if (!scripts) return;
    // a copy of the instance refs: the hook may add or remove scripts
    std::vector<std::pair<int, std::string>> instances;
    for (const ScriptEntry& entry : scripts->scripts) {
        if (entry.type == ScriptType::LUA && entry.enabled && entry.instance)
            instances.emplace_back(static_cast<int>(reinterpret_cast<intptr_t>(entry.instance)), entry.className);
    }
    for (const auto& [ref, className] : instances) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_getfield(L, -1, function);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2);
            if (!luabridge::push<ControllerColliderHit>(L, hit)) {
                lua_pop(L, 3);
                continue;
            }
            if (pcallWithTraceback(L, 2, 0) != LUA_OK) {
                logLuaError("Lua %s() failed for '%s': %s", function, className.c_str(), getLuaStackErrorString(L, -1).c_str());
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}
#endif
namespace {
    // the first enabled (else first) Lua script `className` of the GameObject and its declared
    // property `name`; false when either is missing
    bool findAnimatedScriptField(Scene* scene, Entity entity, const std::string& className, const std::string& name,
                                 ScriptEntry*& entryOut, ScriptProperty*& propertyOut, size_t index) {
        ScriptComponent* scripts = (scene && scene->isEntityCreated(entity)) ? scene->findComponent<ScriptComponent>(entity) : nullptr;
        if (!scripts) {
            return false;
        }
        ScriptEntry* chosen = nullptr;
        if (index != SIZE_MAX) {
            if (index >= scripts->scripts.size() || scripts->scripts[index].type != ScriptType::LUA ||
                scripts->scripts[index].className != className) {
                return false;
            }
            chosen = &scripts->scripts[index];
        }
        for (ScriptEntry& entry : scripts->scripts) {
            if (index != SIZE_MAX) {
                break;
            }
            if (entry.type != ScriptType::LUA || entry.className != className) {
                continue;
            }
            if (!chosen || (!chosen->enabled && entry.enabled)) {
                chosen = &entry;
            }
            if (chosen->enabled) {
                break;
            }
        }
        if (!chosen) {
            return false;
        }
        for (ScriptProperty& property : chosen->properties) {
            if (property.name == name) {
                entryOut = chosen;
                propertyOut = &property;
                return true;
            }
        }
        return false;
    }

    bool scriptValueToFloats(const ScriptPropertyValue& value, ScriptPropertyType type, float* out) {
        switch (type) {
            case ScriptPropertyType::Bool:
                if (!std::holds_alternative<bool>(value)) return false;
                out[0] = std::get<bool>(value) ? 1.0f : 0.0f;
                return true;
            case ScriptPropertyType::Int:
                if (std::holds_alternative<int>(value)) { out[0] = (float)std::get<int>(value); return true; }
                if (std::holds_alternative<float>(value)) { out[0] = std::round(std::get<float>(value)); return true; }
                return false;
            case ScriptPropertyType::Float:
                if (std::holds_alternative<float>(value)) { out[0] = std::get<float>(value); return true; }
                if (std::holds_alternative<int>(value)) { out[0] = (float)std::get<int>(value); return true; }
                return false;
            case ScriptPropertyType::Vector2:
                if (!std::holds_alternative<Vector2>(value)) return false;
                out[0] = std::get<Vector2>(value).x; out[1] = std::get<Vector2>(value).y;
                return true;
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3: {
                if (!std::holds_alternative<Vector3>(value)) return false;
                const Vector3& v = std::get<Vector3>(value);
                out[0] = v.x; out[1] = v.y; out[2] = v.z;
                return true;
            }
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4: {
                if (!std::holds_alternative<Vector4>(value)) return false;
                const Vector4& v = std::get<Vector4>(value);
                out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.w;
                return true;
            }
            default:
                return false;
        }
    }

    // the Lua value at `index` as floats of `type` (false when it is another type)
    bool luaValueToFloats(lua_State* L, int index, ScriptPropertyType type, float* out) {
        switch (type) {
            case ScriptPropertyType::Bool:
                if (!lua_isboolean(L, index)) return false;
                out[0] = lua_toboolean(L, index) ? 1.0f : 0.0f;
                return true;
            case ScriptPropertyType::Int:
            case ScriptPropertyType::Float:
                if (lua_type(L, index) != LUA_TNUMBER) return false;
                out[0] = (float)lua_tonumber(L, index);
                return true;
            case ScriptPropertyType::Vector2: {
                if (!luabridge::isInstance<Vector2>(L, index)) return false;
                auto v = luabridge::get<Vector2>(L, index);
                if (!v) return false;
                out[0] = v.value().x; out[1] = v.value().y;
                return true;
            }
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3: {
                if (!luabridge::isInstance<Vector3>(L, index)) return false;
                auto v = luabridge::get<Vector3>(L, index);
                if (!v) return false;
                out[0] = v.value().x; out[1] = v.value().y; out[2] = v.value().z;
                return true;
            }
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4: {
                if (!luabridge::isInstance<Vector4>(L, index)) return false;
                auto v = luabridge::get<Vector4>(L, index);
                if (!v) return false;
                out[0] = v.value().x; out[1] = v.value().y; out[2] = v.value().z; out[3] = v.value().w;
                return true;
            }
            default:
                return false;
        }
    }
}

bool LuaBinding::getLuaScriptProperty(Scene* scene, Entity entity, const std::string& className, const std::string& name,
                                      ScriptPropertyType type, float* out, size_t index) {
    ScriptEntry* entry = nullptr;
    ScriptProperty* property = nullptr;
    if (!findAnimatedScriptField(scene, entity, className, name, entry, property, index)) {
        return false;
    }
    lua_State* L = luastate;
    if (L && entry->instance) {
        checkLuaThread();
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<int>(reinterpret_cast<intptr_t>(entry->instance)));
        if (lua_istable(L, -1)) {
            lua_pushstring(L, name.c_str());
            lua_rawget(L, -2);
            const bool ok = luaValueToFloats(L, -1, type, out);
            lua_settop(L, top);
            if (ok) {
                return true;
            }
        }
        lua_settop(L, top);
    }
    return scriptValueToFloats(property->value, type, out);
}

bool LuaBinding::setLuaScriptProperty(Scene* scene, Entity entity, const std::string& className, const std::string& name,
                                      ScriptPropertyType type, const float* in, size_t index) {
    ScriptEntry* entry = nullptr;
    ScriptProperty* property = nullptr;
    if (!findAnimatedScriptField(scene, entity, className, name, entry, property, index)) {
        return false;
    }
    // the stored value first (the Inspector shows it in play mode)
    switch (type) {
        case ScriptPropertyType::Bool: property->value = in[0] > 0.5f; break;
        case ScriptPropertyType::Int: property->value = (int)std::lround(in[0]); break;
        case ScriptPropertyType::Float: property->value = in[0]; break;
        case ScriptPropertyType::Vector2: property->value = Vector2(in[0], in[1]); break;
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3: property->value = Vector3(in[0], in[1], in[2]); break;
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4: property->value = Vector4(in[0], in[1], in[2], in[3]); break;
        default: return false;
    }
    lua_State* L = luastate;
    if (!L || !entry->instance) {
        return true;
    }
    checkLuaThread();
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<int>(reinterpret_cast<intptr_t>(entry->instance)));
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return true;
    }
    lua_pushstring(L, name.c_str());
    bool pushed = true;
    switch (type) {
        case ScriptPropertyType::Bool: lua_pushboolean(L, in[0] > 0.5f); break;
        case ScriptPropertyType::Int: lua_pushinteger(L, (lua_Integer)std::llround(in[0])); break;
        case ScriptPropertyType::Float: lua_pushnumber(L, in[0]); break;
        // a fresh userdata per change: a table holding the old value never sees it mutate
        case ScriptPropertyType::Vector2: pushed = static_cast<bool>(luabridge::push<Vector2>(L, Vector2(in[0], in[1]))); break;
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3: pushed = static_cast<bool>(luabridge::push<Vector3>(L, Vector3(in[0], in[1], in[2]))); break;
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4: pushed = static_cast<bool>(luabridge::push<Vector4>(L, Vector4(in[0], in[1], in[2], in[3]))); break;
        default: pushed = false; break;
    }
    if (pushed) {
        // raw: a declared property is a plain field of the instance (never an engine member)
        lua_rawset(L, -3);
    }
    lua_settop(L, top);
    return pushed;
}

void LuaBinding::releaseLuaScriptInstance(Scene* scene, ScriptEntry& scriptEntry) {
    // async work owned by the instance (HTTP requests with owner = self) ends without its
    // callbacks
    AsyncOwners::scriptInstanceReleased(scriptEntry.instance);
    int ref = static_cast<int>(reinterpret_cast<intptr_t>(scriptEntry.instance));
    // its coroutines and timers end with it (destroy, scene unload, Play stop), while the
    // instance still exists for their to-be-closed variables
    ScriptScheduler::cancelOwner(scriptEntry.instance);
    Entity entity = NULL_ENTITY;
    if (luastate) {
        // Event listeners it owns go, `enabled` assignments to it are ignored
        lua_rawgeti(luastate, LUA_REGISTRYINDEX, ref);
        if (lua_istable(luastate, -1)) {
            lua_pushstring(luastate, "entity");
            lua_rawget(luastate, -2);
            entity = lua_isinteger(luastate, -1) ? static_cast<Entity>(lua_tointeger(luastate, -1)) : NULL_ENTITY;
            lua_pop(luastate, 1);
            lua_pushstring(luastate, "__released");
            lua_pushboolean(luastate, 1);
            lua_rawset(luastate, -3);
            pushMembers(luastate, -1);
            if (lua_istable(luastate, -1)) {
                lua_pushboolean(luastate, 0);
                lua_setfield(luastate, -2, "enabled");
                lua_pushboolean(luastate, 0);
                lua_setfield(luastate, -2, "isActiveAndEnabled");
            }
            lua_pop(luastate, 1);
            // D4's notes of the instance (ui-multitouch): its table address can be reused
            ScriptMessages::forgetLegacySubscriptions(lua_topointer(luastate, -1));
            // the Event listeners it owns go now, so nothing keeps it alive
            ScriptEvent::releaseOwner(luastate, -1);
        }
        lua_pop(luastate, 1);
    }
    removeScriptSubscriptions(ref);
    // the ref is free for another instance: its lifecycle state goes first
    ScriptRuntime::forgetInstance(scriptEntry.instance);
    releaseLuaRef(ref);
    scriptEntry.instance = nullptr;
    for (auto& prop : scriptEntry.properties) {
        prop.luaRef = 0;
    }
    if (scene && entity != NULL_ENTITY) {
        ScriptMessages::notifyReceiversChanged(scene, entity);
    }
}

namespace {
    // A GameObject wrapper in a field of a moved script that points at a moved GameObject
    // follows it (the wrapper object itself, so whoever else holds it sees it too)
    void rebindHandle(lua_State* L, int index, Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved) {
        if (lua_type(L, index) != LUA_TUSERDATA || !luabridge::isInstance<EntityHandle>(L, index)) {
            return;
        }
        auto handle = luabridge::get<EntityHandle*>(L, index);
        if (!handle || !handle.value() || handle.value()->getScene() != from) {
            return;
        }
        auto it = moved.find(handle.value()->getEntity());
        if (it != moved.end()) {
            handle.value()->rebind(to, it->second);
        }
    }
}

void LuaBinding::rebindLuaScripts(Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved) {
    lua_State* L = luastate;
    if (!L || !from || !to) return;
    const int top = lua_gettop(L);
    for (const auto& [oldEntity, newEntity] : moved) {
        ScriptComponent* scripts = to->isEntityCreated(newEntity) ? to->findComponent<ScriptComponent>(newEntity) : nullptr;
        if (!scripts) continue;
        for (const ScriptEntry& entry : scripts->scripts) {
            if (!pushScriptInstance(L, entry)) continue;
            const int instance = lua_gettop(L);
            if (lua_istable(L, instance)) {
                lua_pushstring(L, "scene");
                if (!luabridge::push<Scene*>(L, to)) {
                    lua_pushnil(L);
                }
                lua_rawset(L, instance);
                lua_pushstring(L, "entity");
                lua_pushinteger(L, static_cast<lua_Integer>(newEntity));
                lua_rawset(L, instance);
                // its own fields (entity reference properties, wrappers it made): one level
                lua_pushnil(L);
                while (lua_next(L, instance) != 0) {
                    rebindHandle(L, -1, from, to, moved);
                    lua_pop(L, 1);
                }
            }
            lua_settop(L, top);
        }
    }
    lua_settop(L, top);
}

void LuaBinding::createLuaScripts(Scene* scene) {
    if (!scene) return;
    auto scriptsArray = scene->getComponentArray<ScriptComponent>();
    std::vector<Entity> entities;
    entities.reserve(scriptsArray->size());
    for (size_t i = 0; i < scriptsArray->size(); i++) {
        entities.push_back(scriptsArray->getEntity(i));
    }
    createLuaScripts(scene, entities);
}

void LuaBinding::createLuaScripts(Scene* scene, const std::vector<Entity>& entities) {
    if (!scene) return;

    lua_State* L = luastate;
    if (!L) {
        Log::error("Lua state not initialized");
        return;
    }

    // PASS 1: an instance for every Lua entry, enabled or not (Unity: a disabled component
    // exists); EntityReference properties wait for pass 2. The component is looked up again
    // for every script: a module's chunk could make GameObjects, which moves the components.
    std::vector<std::pair<Entity, const void*>> created;
    for (Entity entity : entities) {
        for (size_t s = 0; scene->isEntityCreated(entity); s++) {
            ScriptComponent* scriptComp = scene->findComponent<ScriptComponent>(entity);
            if (!scriptComp || s >= scriptComp->scripts.size()) break;
            ScriptEntry& scriptEntry = scriptComp->scripts[s];
            if (scriptEntry.type != ScriptType::LUA || scriptEntry.instance) continue;
            if (createLuaScriptInstance(L, scene, entity, scriptEntry)) {
                created.push_back({entity, scriptEntry.instance});
            }
        }
    }

    // PASS 2: their entity references, now that every instance exists (missing or broken
    // files have none)
    for (const auto& [entity, instance] : created) {
        if (ScriptEntry* scriptEntry = ScriptRuntime::findEntry(scene, entity, instance)) {
            resolveLuaScriptReferences(L, scene, *scriptEntry);
        }
    }
}

void LuaBinding::initializeLuaScripts(Scene* scene) {
    if (!scene) return;
    if (!luastate) {
        Log::error("Lua state not initialized");
        return;
    }

    createLuaScripts(scene);
    // all init, then all onEnable; start waits for the first frame
    ScriptRuntime::awakeScene(scene);

    // the scene's HTML documents run their <script type="text/lua"> from now on (phase 2)
    scene->getSystem<UIDocumentSystem>()->startDocumentScripts();
}

void LuaBinding::initializeLuaScripts(Scene* scene, const std::vector<Entity>& entities) {
    if (!scene) return;
    createLuaScripts(scene, entities);
    ScriptRuntime::awakeEntities(scene, entities);
}

void LuaBinding::cleanupLuaScripts(Scene* scene) {
    if (!scene) return;

    // onDisable, then onDestroy of every script still alive (a no-op when the scene was
    // unloaded before: a scene load, Play stop)
    ScriptRuntime::unloadScenes({scene});

    auto scriptsArray = scene->getComponentArray<ScriptComponent>();

    for (size_t i = 0; i < scriptsArray->size(); i++) {
        ScriptComponent& scriptComp = scriptsArray->getComponentFromIndex(i);
        for (auto& scriptEntry : scriptComp.scripts) {
            if (scriptEntry.type != ScriptType::LUA) continue;
            if (scriptEntry.instance) {
                releaseLuaScriptInstance(scene, scriptEntry);
            }
        }
    }

    // what the scripts made in the scene's HTML documents (listeners, data models) goes too
    scene->getSystem<UIDocumentSystem>()->releaseScriptState();
}

void LuaBinding::cleanupLuaScripts(Scene* scene, const std::vector<Entity>& entities) {
    if (!scene) return;

    // onDisable, then onDestroy of every script first (Unity), while the others still run
    ScriptRuntime::destroyEntities(scene, entities);

    for (Entity entity : entities) {
        if (!scene->isEntityCreated(entity)) continue;
        ScriptComponent* scriptComp = scene->findComponent<ScriptComponent>(entity);
        if (!scriptComp) continue;
        for (auto& scriptEntry : scriptComp->scripts) {
            if (scriptEntry.type == ScriptType::LUA && scriptEntry.instance) {
                releaseLuaScriptInstance(scene, scriptEntry);
            }
        }
    }
}
