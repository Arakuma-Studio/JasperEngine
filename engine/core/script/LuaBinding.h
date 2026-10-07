// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef luabinding_h
#define luabinding_h

#include "Export.h"
#include "ecs/Entity.h"
#include <string>
#include <vector>

typedef struct lua_State lua_State;
typedef int (*lua_CFunction) (lua_State *L);

namespace jasperengine {

    class Scene;
    struct ScriptEntry;

    class JASPERENGINE_API LuaBinding {
        
        friend class Engine;
        
    private:
        static lua_State *luastate;
        
        static void createLuaState();
        static void configureProjectLuaLoader();

        static int luaRegisterEvent(lua_State* L);
        static int luaRegisterEngineEvent(lua_State* L);
        static int luaRegisterEventImpl(lua_State* L, int eventIndex, int selfIndex, const char* methodName, const char* tag, bool guardActive);
        static std::string getScriptSubscriptionTag(int luaRef);

        static int moduleLoader(lua_State *L);
        static int handleLuaError(lua_State* L);
        static void registerClasses(lua_State *L);
        static void registerHelpersFunctions(lua_State *L);

        // in binding directory
        static void registerActionClasses(lua_State *L);
        static void registerCoreClasses(lua_State *L);
        static void registerECSClasses(lua_State *L);
        static void registerIOClasses(lua_State *L);
        static void registerMathClasses(lua_State *L);
        static void registerGameObjectClasses(lua_State *L);
        static void registerUtilClasses(lua_State *L);
        static void registerThreadClasses(lua_State *L);
        static void registerHtmlUIClasses(lua_State *L);
        static void registerHtmlUIAnimeClasses(lua_State *L);

        static void init();

        // For editor scripts use
        static std::string normalizePtrTypeName(std::string value);
        template <typename T>
        static bool pushEntityHandleTyped(lua_State* L, Scene* scene, Entity entity);
        static bool pushEntityHandleByType(lua_State* L, Scene* scene, Entity entity, const std::string& ptrTypeName);

        static bool createLuaScriptInstance(lua_State* L, Scene* scene, Entity entity, ScriptEntry& scriptEntry);
        static void resolveLuaScriptReferences(lua_State* L, Scene* scene, ScriptEntry& scriptEntry);
        static void callLuaScriptFunction(lua_State* L, ScriptEntry& scriptEntry, const char* function);
        static void releaseLuaScriptInstance(Scene* scene, ScriptEntry& scriptEntry);

    public:
        LuaBinding();
        virtual ~LuaBinding();
        
        static lua_State* getLuaState();
        
        // old lua function call
        static void luaCallback(int nargs, int nresults, int msgh);

        // lua_pcall with stack traceback on error
        static int pcallWithTraceback(lua_State* L, int nargs, int nresults);

        static void stackDump (lua_State *L);

        static void cleanup();

        static void removeScriptSubscriptions(Scene* scene, int luaRef);
        static void releaseLuaRef(int luaRef);

        // For editor scripts use
        static void initializeLuaScripts(Scene* scene);
        static void cleanupLuaScripts(Scene* scene);

        // The scripts of some GameObjects only: ones a script instantiated, ones destroyed
        // (their onDestroy() is called first)
        static void initializeLuaScripts(Scene* scene, const std::vector<Entity>& entities);
        static void cleanupLuaScripts(Scene* scene, const std::vector<Entity>& entities);

        // Pushes what a script sees for a GameObject: its Lua script instance of class
        // `className` (its first one when empty), else a handle typed after className
        static bool pushScriptObject(lua_State* L, Scene* scene, Entity entity, const std::string& className);
        static void clearLoadedProjectModules();

        static std::string getLuaStackErrorString(lua_State* L, int index = -1);
        
    };
    
}

#endif /* luabinding_h */
