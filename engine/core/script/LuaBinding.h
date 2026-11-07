// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef luabinding_h
#define luabinding_h

#include "log/LogCategory.h"
#include "log/LogFormatCheck.h"
#include <cstdint>
#include <string_view>
#include "Export.h"
#include "ecs/Entity.h"
#include "animation/AnimationEvent.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct lua_State lua_State;
typedef int (*lua_CFunction) (lua_State *L);

namespace jasperengine {

    class Scene;
    class Vector3;
    class Vector4;
    struct ScriptEntry;
    struct ControllerColliderHit;
    struct ScriptProperty;
    enum class ScriptPropertyType;

    class JASPERENGINE_API LuaBinding {
        
        friend class Engine;
        
    private:
        static lua_State *luastate;
        static uint64_t luastateGeneration;
        
        static void createLuaState();
        static void configureProjectLuaLoader();

        static int luaRegisterEvent(lua_State* L);
        static int luaRegisterEngineEvent(lua_State* L);
        static int luaRegisterEventImpl(lua_State* L, int eventIndex, int selfIndex, const char* methodName, const char* tag, bool guardActive);
        static int luaUnregisterEvent(lua_State* L);
        static int luaUnregisterEngineEvent(lua_State* L);
        // Pushes the tag RegisterEvent / RegisterEngineEvent give a subscription
        static void pushSubscriptionTag(lua_State* L, int selfIndex, const char* methodName, const char* tag);
        // Pushes Engine.<methodName> (an Engine event object), or raises the registration's error
        static int pushEngineEvent(lua_State* L, const char* caller, const char* methodName);
        static std::string getScriptSubscriptionTag(int luaRef);

        // script instances (script-lifecycle 6.7, 7.1): the metatable of every instance, the
        // members every instance has (enabled, isActiveAndEnabled, gameObject, getScript...)
        static int luaInstanceIndex(lua_State* L);
        static int luaInstanceNewIndex(lua_State* L);

        // GameObject / EntityHandle script lookups (binding/ScriptClassesLua.cpp)
        static int luaGetScript(lua_State* L);
        static int luaGetScripts(lua_State* L);
        static int luaGetScriptInChildren(lua_State* L);
        static int luaGetScriptsInChildren(lua_State* L);
        static int luaGetScriptInParent(lua_State* L);
        static int luaFind(lua_State* L);
        static int luaFindWithTag(lua_State* L);
        static int luaFindGameObjectsWithTag(lua_State* L);
        static int luaFindObjectOfType(lua_State* L);
        static int luaFindObjectsOfType(lua_State* L);
        // persistent objects (DontDestroyOnLoad, binding/ScriptClassesLua.cpp)
        static int luaDontDestroyOnLoad(lua_State* L);
        static int luaIsPersistent(lua_State* L);
        static int luaPersistSingleton(lua_State* L);
        static int luaMoveGameObjectToScene(lua_State* L);

        static int moduleLoader(lua_State *L);
        static int handleLuaError(lua_State* L);
        static void registerClasses(lua_State *L);
        static void registerHelpersFunctions(lua_State *L);

        // in binding directory
        static void registerActionClasses(lua_State *L);
        static void registerParticleClasses(lua_State *L);
        static void registerFoliageClasses(lua_State *L);
        static void registerCoreClasses(lua_State *L);
        static void registerECSClasses(lua_State *L);
        static void registerAudioClasses(lua_State *L);
        static void registerIOClasses(lua_State *L);
        static void registerMathClasses(lua_State *L);
        static void registerNetClasses(lua_State *L);
        static void registerNativeClasses(lua_State *L);
        static void registerGameObjectClasses(lua_State *L);
        static void registerUtilClasses(lua_State *L);
        static void registerThreadClasses(lua_State *L);
        static void registerProfilerClasses(lua_State *L);
        static void registerGameplayClasses(lua_State *L);
        static void registerNavigationClasses(lua_State *L);
        static void registerSplineClasses(lua_State *L);
        static void registerCrashClasses(lua_State *L);
#if JASPERENGINE_HTMLUI
        static void registerHtmlUIClasses(lua_State *L);
        static void registerHtmlUIAnimeClasses(lua_State *L);
        static void registerHtmlUIVectorClasses(lua_State *L);
#if JASPERENGINE_VIDEO
        static void registerHtmlUIMediaClasses(lua_State *L);
#endif
        static void registerHtmlUIWidgetClasses(lua_State *L);
#endif
        static void registerCoroutineClasses(lua_State *L);
        static void registerScriptClasses(lua_State *L);

        static void init();

        // Shader library values from Lua (binding directory). A Material is a copy
        // (Mesh:getMaterial): the shader its submesh draws with when the material names none
        // (the mesh's customShader, else the scene default) is kept beside the copy's userdata
        // at `index`, in a weak-keyed registry table, so its manifest defaults, default-on
        // features and keyword groups resolve as the owner's.
        static void setMaterialDrawnShader(lua_State* L, int index, const std::string& shader);
        static std::string getMaterialDrawnShader(lua_State* L, int index);
        // a Vector3 given for a @property of `shader`: a colour kind keeps an opaque alpha
        static Vector4 getShaderParamValue(const std::string& shader, const std::string& name, const Vector3& value);

        // For editor scripts use
        static std::string normalizePtrTypeName(std::string value);
        template <typename T>
        static bool pushEntityHandleTyped(lua_State* L, Scene* scene, Entity entity);

        static bool createLuaScriptInstance(lua_State* L, Scene* scene, Entity entity, ScriptEntry& scriptEntry);
        static void resolveLuaScriptReferences(lua_State* L, Scene* scene, ScriptEntry& scriptEntry);
        static void releaseLuaScriptInstance(Scene* scene, ScriptEntry& scriptEntry);

    public:
        LuaBinding();
        virtual ~LuaBinding();
        
        // The engine's one Lua state. It belongs to the thread that made it (Engine::systemInit,
        // the main thread): Lua states are not thread-safe, so only that thread may use it.
        static lua_State* getLuaState();
        // Which Lua state this is: a new state (a restarted engine, an Android activity relaunched
        // in the same process) has a new generation, even at the address of the one it replaces
        static uint64_t getLuaStateGeneration();

#ifdef JASPERENGINE_PHYSICS_3D
        // Calls `function(self, hit)` on each enabled Lua script of the entity that defines it
        // (onControllerColliderHit after a CharacterController's move)
        static void callScriptHook(Scene* scene, Entity entity, const char* function, const ControllerColliderHit& hit);
#endif

        // The main thread of L's state (LUA_RIDX_MAINTHREAD). Audit rule for bindings: never
        // store a lua_State* received from a binding, it may be a coroutine's thread, which ends
        // (and is collected) long before a stored callback runs; store LuaBinding::mainThread(L).
        static lua_State* mainThread(lua_State* L);

        // The check of that rule: every allocation of the state (any Lua work that allocates,
        // and every collection step) compares the thread with the owner's. A use from another
        // thread is counted and reported on stderr (the first one); with Abort, or
        // JASPER_LUA_THREAD_CHECK=abort in the environment when the state is made (the tests
        // set it), the process aborts there, with a backtrace where available.
        enum class ThreadCheck { Report, Abort };
        static void setThreadCheck(ThreadCheck mode);
        static ThreadCheck getThreadCheck();
        static bool isLuaThread();
        static size_t getOffThreadUseCount();

        // The global names of the state as the engine made it (classes, helpers, standard
        // libraries), not what scripts added since. Readable from any thread.
        static std::vector<std::string> getEngineGlobalNames();
        
        // old lua function call
        static void luaCallback(int nargs, int nresults, int msgh);

        // lua_pcall with stack traceback on error
        static int pcallWithTraceback(lua_State* L, int nargs, int nresults);

        static void stackDump (lua_State *L);

        static void cleanup();
        // A new state in place of the current one (closed first), as an engine restarted in the
        // same process gets: what systemShutdown and systemInit do to Lua, without the rest
        static void recreateLuaState();

        // Removes the event subscriptions and timers of a script instance (its table)
        static void removeScriptSubscriptions(int luaRef);
        static void releaseLuaRef(int luaRef);

        // A scene's Lua scripts without C++ ones (the editor without a script library):
        // createLuaScripts, ScriptRuntime::awakeScene (init, onEnable), then the scene's HTML
        // documents' scripts. The generated initScripts does the same with the C++ scripts
        // made and bound between the first two.
        static void initializeLuaScripts(Scene* scene);
        // ScriptRuntime::unloadScenes (onDisable, onDestroy of every script still alive, Lua and
        // C++), then the Lua instances are released
        static void cleanupLuaScripts(Scene* scene);

        // The Lua instances of every script entry (enabled or not) of the scene, and their
        // entity references; nothing is called yet
        static void createLuaScripts(Scene* scene);
        // ...of some GameObjects only (instantiated ones)
        static void createLuaScripts(Scene* scene, const std::vector<Entity>& entities);

        // The scripts of some GameObjects only: createLuaScripts then ScriptRuntime::awakeEntities;
        // ScriptRuntime::destroyEntities (onDisable, onDestroy) then the instances are released
        static void initializeLuaScripts(Scene* scene, const std::vector<Entity>& entities);
        static void cleanupLuaScripts(Scene* scene, const std::vector<Entity>& entities);

        // GameObjects moved to another scene (persistence; old -> new entity, their Script
        // components in `to` now): their Lua instances get `self.scene` and `self.entity` of the
        // new place, and the GameObject wrappers held in their fields that point at a moved
        // GameObject follow it
        static void rebindLuaScripts(Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved);

        // The table of the methods every Lua script instance has after its own class's
        // (getScript...; ScriptInstanceBase): features add theirs to it
        static void pushScriptInstanceBase(lua_State* L);
        // t[key] of the value at `index` in a protected call (a class's __index may raise):
        // pushes the value, or nil and false on an error
        static bool protectedGetField(lua_State* L, int index, const char* key);
        // The engine members of a Lua script instance (enabled, isActiveAndEnabled, gameObject),
        // plain fields of a table the instance reads through: ScriptRuntime keeps them current
        static void syncScriptMembers(Scene* scene, Entity entity, const ScriptEntry& entry, bool activeAndEnabled);
        // Pushes the Lua instance of a script entry; false (nothing pushed) without one
        static bool pushScriptInstance(lua_State* L, const ScriptEntry& entry);

        // Pushes the wrapper of that type name (case-insensitive, "Sprite", "camera"...) for the
        // GameObject; an unknown name gives an EntityHandle (with a warning)
        static bool pushEntityHandleByType(lua_State* L, Scene* scene, Entity entity, const std::string& ptrTypeName);
        // Pushes what a script sees for a GameObject: its Lua script instance of class
        // `className` (its first one when empty), else a handle typed after className
        static bool pushScriptObject(lua_State* L, Scene* scene, Entity entity, const std::string& className);
        // Calls `method` on every enabled Lua script instance of the GameObject that defines it,
        // with `args` spread as its parameters (an Animation Player's clip event, animation clips
        // spec 3.5). True when at least one script defines it. ScriptMessages::sendNamed.
        // `integers`: whole numbers are passed as Lua integers (an Animator Controller's
        // onStateEnter layer index), else as floats
        static bool callLuaScriptMethod(Scene* scene, Entity entity, const std::string& method, const std::vector<AnimationEventArg>& args,
                                        bool integers = false);
        // A clip's property track on a Lua script field (anim-properties spec 5.7): the declared
        // `properties` field `name` of the GameObject's first enabled (else first) Lua script
        // `className`, as floats of its type (Bool 0/1, Int, Float, Vector2-4, Color3/4 as stored:
        // linear). The read takes the instance field (self.<name>) when the instance exists, else
        // ScriptProperty::value; the write sets both (the instance field raw, a fresh Vector
        // userdata for vectors: a script holding the old one never sees it change). False when
        // the script or the declared field is missing. `index`: the script at that index of the
        // GameObject's scripts (a binding picks it once; false when it is not a `className`
        // Lua script any more). Main thread.
        static bool getLuaScriptProperty(Scene* scene, Entity entity, const std::string& className, const std::string& name,
                                         ScriptPropertyType type, float* out, size_t index = SIZE_MAX);
        static bool setLuaScriptProperty(Scene* scene, Entity entity, const std::string& className, const std::string& name,
                                         ScriptPropertyType type, const float* in, size_t index = SIZE_MAX);
        static void clearLoadedProjectModules();

        static std::string getLuaStackErrorString(lua_State* L, int index = -1);
        // A Lua error (its traceback is in the text): category lua, no C++ backtrace
        static void logLuaError(const char* format, ...) JE_PRINTF_FORMAT(1, 2);
        // Logs text with the calling Lua function's chunk and line as its source; a stackTrace
        // (a Lua traceback) goes under it, and to the Log buffer as the line's own (Log::emitEntry)
        static void logFromLua(lua_State* L, LogLevel level, const LogCategory& category, std::string_view text,
                               const std::string& stackTrace = std::string());

        // The math classes (Vector2/3/4, Quaternion, Matrix..., Color) in another state: the
        // editor's sandboxed editor-script state
        static void registerMathClassesIn(lua_State* L);

        // ---- for the hot reload during Play (LuaReload.cpp)

        // The compiled chunk of a script path becomes the function at `index`: instances made
        // from now on run it
        static void setScriptChunk(lua_State* L, const std::string& path, int index);
        // Pushes a script property's value as an instance field gets it at creation (an unset
        // prefab or asset is nil; an EntityReference is resolved in `scene` like init's pass 2)
        static void pushScriptPropertyValue(lua_State* L, Scene* scene, const ScriptProperty& property);
        // RegisterEvent's work for `self` (index) on the event at eventIndex, in a protected
        // call: false and the error when it failed
        static bool replayScriptEvent(lua_State* L, int eventIndex, int selfIndex, const std::string& method,
                                      const std::string& tag, bool guarded, std::string& error);
        
    };
    
}

#endif /* luabinding_h */
