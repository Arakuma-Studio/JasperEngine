// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <unordered_map>
#include "LuaBinding.h"

#include "Log.h"
#include "System.h"
#include "Engine.h"
#include "Scene.h"
#include "SceneManager.h"
#include "manager/ObjectManager.h"
#include "component/ScriptComponent.h"
#include "ScriptProperty.h"
#include "object/EntityHandle.h"
#include "object/GameObject.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "object/Mirror.h"
#include "object/Model.h"
#include "object/MeshPolygon.h"
#include "object/Points.h"
#include "object/LineRenderer.h"
#include "object/Sprite.h"
#include "object/Terrain.h"
#include "object/Tilemap.h"
#include "object/Light.h"
#include "object/Light2D.h"
#include "object/ShadowCaster2D.h"
#include "object/Camera.h"
#include "object/Bone.h"
#include "object/environment/Fog.h"
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
#include "object/ui/UIDocument.h"
#include "subsystem/UIDocumentSystem.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#include "object/physics/Joint2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "object/physics/Joint3D.h"
#endif
#include "action/Action.h"
#include "action/Tween.h"
#include "action/AlphaTween.h"
#include "action/ColorTween.h"
#include "action/PositionTween.h"
#include "action/RotationTween.h"
#include "action/ScaleTween.h"
#include "action/Animation.h"
#include "action/ParticleSystem.h"
#include "action/SpriteAnimation.h"
#include "action/keyframe/MorphTracks.h"
#include "action/keyframe/RotateTracks.h"
#include "action/keyframe/ScaleTracks.h"
#include "action/keyframe/TranslateTracks.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <locale>
#include <set>
#include <vector>
#include <memory>

using namespace jasperengine;



lua_State *LuaBinding::luastate = NULL;

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


LuaBinding::LuaBinding() {

}

LuaBinding::~LuaBinding() {

}


void LuaBinding::createLuaState(){
    LuaBinding::luastate = luaL_newstate();

    registerClasses(luastate);
    configureProjectLuaLoader();
    registerHelpersFunctions(luastate);

    collectLoadedModuleNames(luastate, baselineLuaModules);
}

lua_State* LuaBinding::getLuaState(){
    return luastate;
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

int LuaBinding::pcallWithTraceback(lua_State* L, int nargs, int nresults){
    //message handler goes below the function and its arguments
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, handleLuaError);
    lua_insert(L, base);

    int status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);

    return status;
}

void LuaBinding::luaCallback(int nargs, int nresults, int msgh){
    int status = lua_pcall(luastate, nargs, nresults, msgh);
    if (status != 0){
        Log::error("Lua Error: %s", getLuaStackErrorString(luastate, -1).c_str());
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
    if (tag) {
        lua_pushstring(L, tag);
    } else {
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
    int tagIndex = lua_gettop(L);

    // A scene script's engine events (onUpdate, input...) are skipped while its
    // GameObject is inactive or its entry disabled; other tables are never guarded
    Scene* guardScene = nullptr;
    lua_Integer guardEntity = 0;
    lua_Integer guardRef = 0;
    if (guardActive) {
        lua_getfield(L, selfIndex, "scene");
        if (lua_isuserdata(L, -1)) {
            auto sceneResult = luabridge::get<Scene*>(L, -1);
            if (sceneResult) {
                guardScene = sceneResult.value();
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, selfIndex, "entity");
        guardEntity = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
        lua_getfield(L, selfIndex, "__scriptRef");
        guardRef = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
    }

    // Build closure: function(...) return method(self, ...) end
    lua_pushvalue(L, methodFuncIndex); // method
    lua_pushvalue(L, selfIndex);       // self
    if (guardScene && guardEntity > 0) {
        lua_pushlightuserdata(L, guardScene);
    } else {
        lua_pushnil(L);
    }
    lua_pushinteger(L, guardEntity);
    lua_pushinteger(L, guardRef);
    lua_pushcclosure(L,
        [](lua_State* Linner) -> int {
            int nargs = lua_gettop(Linner);

            if (lua_islightuserdata(Linner, lua_upvalueindex(3))) {
                Scene* scene = static_cast<Scene*>(lua_touserdata(Linner, lua_upvalueindex(3)));
                Entity entity = static_cast<Entity>(lua_tointeger(Linner, lua_upvalueindex(4)));
                void* instance = reinterpret_cast<void*>(static_cast<intptr_t>(lua_tointeger(Linner, lua_upvalueindex(5))));
                if (!isScriptActive(scene, entity, instance)) {
                    return 0;
                }
            }

            lua_pushvalue(Linner, lua_upvalueindex(1)); // method
            lua_pushvalue(Linner, lua_upvalueindex(2)); // self

            for (int i = 1; i <= nargs; ++i) {
                lua_pushvalue(Linner, i);
            }

            // lua_error below unwinds the script frames, get the traceback here
            if (pcallWithTraceback(Linner, 1 + nargs, LUA_MULTRET) != LUA_OK) {
                return lua_error(Linner);
            }
            // Filters like preSolve2D read what the method returns
            return lua_gettop(Linner) - nargs;
        },
        5);
    int closureIndex = lua_gettop(L);

    // event:add(tag, closure)
    lua_getfield(L, eventIndex, "add");
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "Event registration: event object has no 'add' method");
    }

    lua_pushvalue(L, eventIndex);
    lua_pushvalue(L, tagIndex);
    lua_pushvalue(L, closureIndex);

    if (pcallWithTraceback(L, 3, 0) != LUA_OK) {
        return lua_error(L);
    }

    return 0;
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
    lua_getglobal(L, "Engine");
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "RegisterEngineEvent: Engine table not found");
    }

    lua_getfield(L, -1, methodName);
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "RegisterEngineEvent: Engine.%s not found", methodName);
    }

    int eventIndex = lua_gettop(L);

    return luaRegisterEventImpl(L, eventIndex, 1, methodName, tag, true);
}

int LuaBinding::moduleLoader(lua_State *L) {
    
    const char *filename = lua_tostring(L, 1);
    std::string separator(1, System::instance().getDirSeparator());
    filename = luaL_gsub(L, filename, ".", separator.c_str());
    
    std::string filepath;
    Data filedata;
    
    filepath = std::string("lua://") + filename + ".lua";
    filedata.open(filepath.c_str());
    if (filedata.getMemPtr() != NULL) {
        
        luaL_loadbuffer(L, (const char *) filedata.getMemPtr(), filedata.length(),
                        filepath.c_str());
        
        return 1;
    }

    filepath = "lua://" + std::string("lua") + System::instance().getDirSeparator() + filename + ".lua";
    filedata.open(filepath.c_str());
    if (filedata.getMemPtr() != NULL) {
        
        luaL_loadbuffer(L, (const char *) filedata.getMemPtr(), filedata.length(),
                        filepath.c_str());
        
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

    //First try open on root assets dir
    if (filedata.open(luafile.c_str()) != FileErrors::FILEDATA_OK){
        //Second try to open on lua dir
        filedata.open(luafile_subdir.c_str());
    }

    if (luaL_loadbuffer(L,(const char*)filedata.getMemPtr(),filedata.length(), luafile.c_str()) == 0){
        if(pcallWithTraceback(L, 0, LUA_MULTRET) != 0){
            Log::error("Lua Error: %s", getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
            lua_close(L);
        }
    }else{
        Log::error("Lua Error: %s", getLuaStackErrorString(L, -1).c_str());
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
    registerIOClasses(L);
    registerMathClasses(L); 
    registerUtilClasses(L);
    registerThreadClasses(L);
    registerHtmlUIClasses(L);
    registerHtmlUIAnimeClasses(L);
    registerHtmlUIVectorClasses(L);

#endif //DISABLE_LUA_BINDINGS
}

// Lua's print, through Log::print instead of stdout: the editor's Console shows it (Play
// and edit time alike), and a game's platform log (logcat, the terminal) gets it
static int luaPrint(lua_State* L){
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    Log::print("%s", line.c_str());
    return 0;
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
    clearScriptChunks(luastate);
    if (luastate) {
        lua_close(luastate);
        luastate = NULL;
    }
}

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

void LuaBinding::removeScriptSubscriptions(Scene* scene, int luaRef){
    std::string addr = getScriptSubscriptionTag(luaRef);
    if (addr.empty()) return;

    Engine::removeSubscriptionsByTag(addr);
    if (scene) {
        scene->removeSubscriptionsByTag(addr);
    }
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
    DISPATCH_TYPE(GameObject, "gameobject");
    DISPATCH_TYPE(Camera, "camera");
    DISPATCH_TYPE(Light, "light");
    DISPATCH_TYPE(Light2D, "light2d");
    DISPATCH_TYPE(ShadowCaster2D, "shadowcaster2d");
    DISPATCH_TYPE(Mesh, "mesh");
    DISPATCH_TYPE(Primitive, "primitive");
    DISPATCH_TYPE(Mirror, "mirror");
    DISPATCH_TYPE(Terrain, "terrain");
    DISPATCH_TYPE(Tilemap, "tilemap");
    DISPATCH_TYPE(Model, "model");
    DISPATCH_TYPE(MeshPolygon, "meshpolygon");
    DISPATCH_TYPE(Points, "points");
    DISPATCH_TYPE(LineRenderer, "linerenderer");
    DISPATCH_TYPE(Sprite, "sprite");
    DISPATCH_TYPE(AudioSource, "audiosource");
    DISPATCH_TYPE(RectTransform, "recttransform");
    DISPATCH_TYPE(LayoutGroup, "layoutgroup");
    DISPATCH_TYPE(Polygon, "polygon");
    DISPATCH_TYPE(Text, "text");
    DISPATCH_TYPE(Image, "image");
    DISPATCH_TYPE(Button, "button");
    DISPATCH_TYPE(Panel, "panel");
    DISPATCH_TYPE(Scrollbar, "scrollbar");
    DISPATCH_TYPE(Progressbar, "progressbar");
    DISPATCH_TYPE(InputField, "inputfield");
    DISPATCH_TYPE(UIDocument, "uidocument");
    DISPATCH_TYPE(Bone, "bone");
#ifdef JASPERENGINE_PHYSICS_2D
    DISPATCH_TYPE(Rigidbody2D, "rigidbody2d");
    DISPATCH_TYPE(Joint2D, "joint2d");
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    DISPATCH_TYPE(Rigidbody, "rigidbody");
    DISPATCH_TYPE(Joint3D, "joint3d");
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
    DISPATCH_TYPE(MorphTracks, "morphtracks");
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

bool LuaBinding::createLuaScriptInstance(lua_State* L, Scene* scene, Entity entity, ScriptEntry& scriptEntry) {
    auto chunk = scriptChunks.find(scriptEntry.path);
    if (chunk == scriptChunks.end()) {
        std::string luaFile = std::string("lua://") + scriptEntry.path;
        Data filedata;
        if (filedata.open(luaFile.c_str()) != FileErrors::FILEDATA_OK) {
            Log::error("Lua script file not found: %s", scriptEntry.path.c_str());
            return false;
        }

        int loadStatus = luaL_loadbuffer(L, (const char*)filedata.getMemPtr(), filedata.length(), scriptEntry.path.c_str());
        if (loadStatus != LUA_OK) {
            Log::error("Failed to load Lua file '%s': %s", scriptEntry.path.c_str(), getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
            return false;
        }
        chunk = scriptChunks.emplace(scriptEntry.path, luaL_ref(L, LUA_REGISTRYINDEX)).first;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, chunk->second);

    int status = pcallWithTraceback(L, 0, 1);
    if (status != LUA_OK) {
        Log::error("Failed to execute Lua module '%s': %s", scriptEntry.className.c_str(), getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        return false;
    }

    if (!lua_istable(L, -1)) {
        Log::error("Lua module '%s' did not return a table", scriptEntry.className.c_str());
        lua_pop(L, 1);
        return false;
    }

    // Create instance table with module as prototype
    lua_newtable(L);
    lua_newtable(L);
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);

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
            lua_setfield(L, -2, prop.name.c_str());
            continue;
        }
        if (std::holds_alternative<bool>(prop.value)) {
            lua_pushboolean(L, std::get<bool>(prop.value));
        } else if (std::holds_alternative<int>(prop.value)) {
            lua_pushinteger(L, std::get<int>(prop.value));
        } else if (std::holds_alternative<float>(prop.value)) {
            lua_pushnumber(L, std::get<float>(prop.value));
        } else if (std::holds_alternative<std::string>(prop.value)) {
            lua_pushstring(L, std::get<std::string>(prop.value).c_str());
        } else if (std::holds_alternative<Vector2>(prop.value)) {
            if (!luabridge::push<Vector2>(L, std::get<Vector2>(prop.value))) lua_pushnil(L);
        } else if (std::holds_alternative<Vector3>(prop.value)) {
            if (!luabridge::push<Vector3>(L, std::get<Vector3>(prop.value))) lua_pushnil(L);
        } else if (std::holds_alternative<Vector4>(prop.value)) {
            if (!luabridge::push<Vector4>(L, std::get<Vector4>(prop.value))) lua_pushnil(L);
        } else if (std::holds_alternative<PrefabAsset>(prop.value) && !std::get<PrefabAsset>(prop.value).isEmpty()) {
            // a prefab property left at None stays nil
            if (!luabridge::push<PrefabAsset>(L, std::get<PrefabAsset>(prop.value))) lua_pushnil(L);
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, prop.name.c_str());
    }

    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    scriptEntry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));

    // lets RegisterEngineEvent find this entry to honour its enabled flag
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, ref);
    lua_setfield(L, -2, "__scriptRef");
    lua_pop(L, 1);
    for (auto& prop : scriptEntry.properties) {
        prop.luaRef = ref;
    }

    lua_pop(L, 1); // pop module
    return true;
}

bool LuaBinding::pushScriptObject(lua_State* L, Scene* scene, Entity entity, const std::string& className) {
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
        return false;
    }
    if (ScriptComponent* scriptComp = scene->findComponent<ScriptComponent>(entity)) {
        for (auto& script : scriptComp->scripts) {
            if (script.type == ScriptType::LUA && script.enabled && script.instance &&
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

void LuaBinding::callLuaScriptFunction(lua_State* L, ScriptEntry& scriptEntry, const char* function) {
    int ref = static_cast<int>(reinterpret_cast<intptr_t>(scriptEntry.instance));
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_getfield(L, -1, function);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        if (pcallWithTraceback(L, 1, 0) != LUA_OK) {
            Log::error("Lua %s() failed for '%s': %s", function, scriptEntry.className.c_str(), getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void LuaBinding::releaseLuaScriptInstance(Scene* scene, ScriptEntry& scriptEntry) {
    int ref = static_cast<int>(reinterpret_cast<intptr_t>(scriptEntry.instance));
    removeScriptSubscriptions(scene, ref);
    releaseLuaRef(ref);
    scriptEntry.instance = nullptr;
    for (auto& prop : scriptEntry.properties) {
        prop.luaRef = 0;
    }
}

void LuaBinding::initializeLuaScripts(Scene* scene) {
    if (!scene) return;

    lua_State* L = luastate;
    if (!L) {
        Log::error("Lua state not initialized");
        return;
    }

    auto scriptsArray = scene->getComponentArray<ScriptComponent>();

    // PASS 1: Create all Lua script instances (without resolving EntityRef properties)
    for (size_t i = 0; i < scriptsArray->size(); i++) {
        ScriptComponent& scriptComp = scriptsArray->getComponentFromIndex(i);
        Entity entity = scriptsArray->getEntity(i);

        for (auto& scriptEntry : scriptComp.scripts) {
            if (!scriptEntry.enabled) continue;
            if (scriptEntry.type != ScriptType::LUA) continue;
            createLuaScriptInstance(L, scene, entity, scriptEntry);
        }
    }

    // PASS 2: Resolve Entity pointer properties
    for (size_t i = 0; i < scriptsArray->size(); i++) {
        ScriptComponent& scriptComp = scriptsArray->getComponentFromIndex(i);

        for (auto& scriptEntry : scriptComp.scripts) {
            if (scriptEntry.type != ScriptType::LUA || !scriptEntry.enabled) continue;
            // PASS 1 skips missing or broken files: indexing a nil instance panics Lua
            if (!scriptEntry.instance) continue;
            resolveLuaScriptReferences(L, scene, scriptEntry);
        }
    }

    // PASS 3: Call init() methods. A script instantiating in init() appends to the array
    // (its new scripts start there), so the component is read again by index each time.
    // What they instantiate goes to their scene, not yet the Engine's.
    ObjectManager::pushActiveScene(scene);
    const size_t count = scriptsArray->size();
    for (size_t i = 0; i < count && i < scriptsArray->size(); i++) {
        for (size_t s = 0; s < scriptsArray->getComponentFromIndex(i).scripts.size(); s++) {
            ScriptEntry& scriptEntry = scriptsArray->getComponentFromIndex(i).scripts[s];
            if (scriptEntry.type != ScriptType::LUA || !scriptEntry.enabled) continue;
            if (!scriptEntry.instance) continue;
            callLuaScriptFunction(L, scriptEntry, "init");
        }
    }
    ObjectManager::popActiveScene();
}

void LuaBinding::initializeLuaScripts(Scene* scene, const std::vector<Entity>& entities) {
    if (!scene) return;

    lua_State* L = luastate;
    if (!L) {
        Log::error("Lua state not initialized");
        return;
    }

    // The component is looked up again for every script: a script can make GameObjects
    // (instantiate in init()), which moves the components of the array
    auto forEachEntry = [&](auto&& callback) {
        for (Entity entity : entities) {
            if (!scene->isEntityCreated(entity)) continue;
            ScriptComponent* scriptComp = scene->findComponent<ScriptComponent>(entity);
            for (size_t s = 0; scriptComp && s < scriptComp->scripts.size(); s++) {
                ScriptEntry& scriptEntry = scriptComp->scripts[s];
                if (scriptEntry.type == ScriptType::LUA && scriptEntry.enabled) {
                    callback(entity, scriptEntry);
                }
                scriptComp = scene->isEntityCreated(entity) ? scene->findComponent<ScriptComponent>(entity) : nullptr;
            }
        }
    };

    forEachEntry([&](Entity entity, ScriptEntry& scriptEntry) {
        if (!scriptEntry.instance) {
            createLuaScriptInstance(L, scene, entity, scriptEntry);
        }
    });
    forEachEntry([&](Entity, ScriptEntry& scriptEntry) {
        if (scriptEntry.instance) {
            resolveLuaScriptReferences(L, scene, scriptEntry);
        }
    });
    forEachEntry([&](Entity, ScriptEntry& scriptEntry) {
        if (scriptEntry.instance) {
            callLuaScriptFunction(L, scriptEntry, "init");
        }
    });
}

void LuaBinding::cleanupLuaScripts(Scene* scene) {
    if (!scene) return;

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

    lua_State* L = luastate;

    // onDestroy() of every script first (Unity's OnDestroy), while the others still run
    for (Entity entity : entities) {
        if (!L || !scene->isEntityCreated(entity)) continue;
        ScriptComponent* scriptComp = scene->findComponent<ScriptComponent>(entity);
        for (size_t s = 0; scriptComp && s < scriptComp->scripts.size(); s++) {
            if (scriptComp->scripts[s].type == ScriptType::LUA && scriptComp->scripts[s].instance) {
                callLuaScriptFunction(L, scriptComp->scripts[s], "onDestroy");
            }
            scriptComp = scene->isEntityCreated(entity) ? scene->findComponent<ScriptComponent>(entity) : nullptr;
        }
    }

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
