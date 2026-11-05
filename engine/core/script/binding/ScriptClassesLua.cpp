// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Script lifecycle bindings (script-lifecycle spec 7): the Event class, the getScript family
// (on every GameObject handle and script instance) and GameObject's scene-wide finds.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "EntityHandle.h"
#include "GameObject.h"
#include "ScriptEvent.h"
#include "ScriptRuntime.h"
#include "registry/TagManager.h"
#include "manager/ObjectManager.h"
#include "manager/SceneManager.h"
#include "registry/EntityCloner.h"
#include "component/Transform.h"
#include "component/ScriptComponent.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/BoneComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/FogComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/LatticeComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/MirrorComponent.h"
#include "component/ModelComponent.h"
#include "component/PanelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/TilemapComponent.h"
#include "component/TweenComponent.h"
#include "component/UIDocumentComponent.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "component/Rigidbody2DComponent.h"
#include "component/Joint2DComponent.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#include "component/Joint3DComponent.h"
#endif

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace jasperengine;

namespace {

    // The GameObject of argument `index`: a GameObject handle (EntityHandle or a wrapper), or a
    // Lua script instance (self)
    bool objectArg(lua_State* L, int index, Scene*& scene, Entity& entity) {
        if (luabridge::isInstance<EntityHandle>(L, index)) {
            auto handle = luabridge::get<EntityHandle*>(L, index);
            if (!handle || !handle.value()) {
                return false;
            }
            scene = handle.value()->getScene();
            entity = handle.value()->getEntity();
            return scene != nullptr;
        }
        if (lua_istable(L, index)) {
            const int top = lua_gettop(L);
            lua_getfield(L, index, "scene");
            lua_getfield(L, index, "entity");
            bool ok = false;
            if (luabridge::isInstance<Scene*>(L, -2) && lua_isinteger(L, -1)) {
                auto sceneResult = luabridge::get<Scene*>(L, -2);
                if (sceneResult && sceneResult.value()) {
                    scene = sceneResult.value();
                    entity = static_cast<Entity>(lua_tointeger(L, -1));
                    ok = true;
                }
            }
            lua_settop(L, top);
            return ok;
        }
        return false;
    }

    void checkObject(lua_State* L, const char* caller, Scene*& scene, Entity& entity) {
        if (!objectArg(L, 1, scene, entity)) {
            luaL_error(L, "%s: call it with a colon on a GameObject or a script (obj:%s(\"Enemy\"))", caller, caller);
        }
    }

    // className at `index`: required (a string) or optional (nil / none: every class)
    std::string classArg(lua_State* L, int index, const char* caller, bool optional) {
        if (optional && lua_isnoneornil(L, index)) {
            return std::string();
        }
        if (lua_type(L, index) != LUA_TSTRING) {
            luaL_error(L, "%s: className must be a string", caller);
        }
        return lua_tostring(L, index);
    }

    // until it is released: an onDestroy still finds its siblings (Unity's components)
    bool liveLuaEntry(const ScriptEntry& entry, const std::string& className) {
        return entry.type == ScriptType::LUA && entry.instance && (className.empty() || entry.className == className);
    }

    // Pushes the matching Lua script instances of the GameObject, enabled or not; `limit` 1: the
    // first. Their count.
    int pushScriptsOf(lua_State* L, Scene* scene, Entity entity, const std::string& className, bool requireEnabled,
                      int tableIndex, int count, int limit) {
        if (!scene->isEntityCreated(entity)) {
            return count;
        }
        ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
        if (!scripts) {
            return count;
        }
        for (const ScriptEntry& entry : scripts->scripts) {
            if (!liveLuaEntry(entry, className) || (requireEnabled && !entry.enabled)) {
                continue;
            }
            LuaBinding::pushScriptInstance(L, entry);
            if (tableIndex != 0) {
                lua_rawseti(L, tableIndex, ++count);
            } else {
                return count + 1;
            }
            if (limit > 0 && count >= limit) {
                return count;
            }
        }
        return count;
    }

    // `root` and its Transform descendants in pre-order
    std::vector<Entity> branchOf(Scene* scene, Entity root) {
        std::vector<Entity> branch{root};
        if (!scene->findComponent<Transform>(root)) {
            return branch;
        }
        std::unordered_set<Entity> inBranch{root};
        scene->getComponentArray<Transform>()->forEachAfter(root, [&](Entity entity, const Transform& transform) {
            if (!inBranch.count(transform.parent)) {
                return false;
            }
            inBranch.insert(entity);
            branch.push_back(entity);
            return true;
        });
        return branch;
    }

    int getScriptInChildren(lua_State* L, bool all) {
        const char* caller = all ? "getScriptsInChildren" : "getScriptInChildren";
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        checkObject(L, caller, scene, entity);
        const std::string className = classArg(L, 2, caller, false);
        const bool includeInactive = lua_toboolean(L, 3) != 0;
        if (all) {
            lua_newtable(L);
        }
        const int table = all ? lua_gettop(L) : 0;
        int count = 0;
        if (scene->isEntityCreated(entity)) {
            for (Entity e : branchOf(scene, entity)) {
                if (!includeInactive && !scene->isEntityActiveInHierarchy(e)) {
                    continue;
                }
                count = pushScriptsOf(L, scene, e, className, false, table, count, 0);
                if (!all && count > 0) {
                    return 1;
                }
            }
        }
        if (!all) {
            lua_pushnil(L);
        }
        return 1;
    }

    // ---- GameObject.find* ----

    // Where the finds look (spec decision 9): the running scenes in Engine order; while a scene
    // starts (a load factory or a scene's init / onEnable), also the scenes started meanwhile
    // and the one being built, so a script's init finds the objects of its own scene
    std::vector<Scene*> findScenes() {
        std::vector<Scene*> scenes = Engine::getSceneLayers();
        const std::vector<Scene*> building = ObjectManager::getActiveSceneStack();
        if (SceneManager::isFactoryRunning() || !building.empty()) {
            for (Scene* scene : ScriptRuntime::getStartedScenes()) {
                if (std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) scenes.push_back(scene);
            }
            for (Scene* scene : building) {
                if (scene && std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) scenes.push_back(scene);
            }
        }
        return scenes;
    }

    Entity parentOf(Scene* scene, Entity entity) {
        Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    // "Name", "Parent/Name" (Name below a Parent anywhere) or "/Root/Name" (from a root)
    bool matchesPath(Scene* scene, Entity entity, const std::vector<std::string>& path, bool fromRoot) {
        Entity current = entity;
        for (size_t i = path.size(); i-- > 0;) {
            if (current == NULL_ENTITY || !scene->isEntityCreated(current) || scene->getEntityNameRef(current) != path[i]) {
                return false;
            }
            current = parentOf(scene, current);
        }
        return !fromRoot || current == NULL_ENTITY;
    }

    void pushGameObject(lua_State* L, Scene* scene, Entity entity) {
        if (!luabridge::push<GameObject>(L, GameObject(scene, entity))) {
            lua_pushnil(L);
        }
    }

    // A wrapper class name (lua-api.md 1.2) and whether a GameObject has its component
    struct WrapperType {
        const char* name;
        std::function<bool(Scene*, Entity)> has;
    };

    template<typename T>
    std::function<bool(Scene*, Entity)> hasComponent() {
        return [](Scene* scene, Entity entity) { return scene->findComponent<T>(entity) != nullptr; };
    }

    const std::vector<WrapperType>& wrapperTypes() {
        static const std::vector<WrapperType> types = {
            {"Camera", hasComponent<CameraComponent>()},
            {"Light", hasComponent<LightComponent>()},
            {"Light2D", hasComponent<Light2DComponent>()},
            {"ShadowCaster2D", hasComponent<ShadowCaster2DComponent>()},
            {"CapsuleShadowCaster", hasComponent<CapsuleShadowCasterComponent>()},
            {"Mesh", hasComponent<MeshComponent>()},
            {"Model", hasComponent<ModelComponent>()},
            {"Sprite", hasComponent<SpriteComponent>()},
            {"Terrain", hasComponent<TerrainComponent>()},
            {"Tilemap", hasComponent<TilemapComponent>()},
            {"Lattice", hasComponent<LatticeComponent>()},
            {"Mirror", hasComponent<MirrorComponent>()},
            {"MeshPolygon", hasComponent<MeshPolygonComponent>()},
            {"Points", hasComponent<PointsComponent>()},
            {"LineRenderer", hasComponent<LineRendererComponent>()},
            {"AudioSource", hasComponent<AudioSourceComponent>()},
            {"RectTransform", hasComponent<RectTransformComponent>()},
            {"LayoutGroup", hasComponent<LayoutGroupComponent>()},
            {"Polygon", hasComponent<PolygonComponent>()},
            {"Text", hasComponent<TextComponent>()},
            {"Image", hasComponent<ImageComponent>()},
            {"Button", hasComponent<ButtonComponent>()},
            {"Panel", hasComponent<PanelComponent>()},
            {"Scrollbar", hasComponent<ScrollbarComponent>()},
            {"Progressbar", hasComponent<ProgressbarComponent>()},
            {"InputField", hasComponent<InputFieldComponent>()},
            {"OnScreenStick", hasComponent<OnScreenStickComponent>()},
            {"OnScreenButton", hasComponent<OnScreenButtonComponent>()},
            {"TouchArea", hasComponent<TouchAreaComponent>()},
            {"UIDocument", hasComponent<UIDocumentComponent>()},
            {"Bone", hasComponent<BoneComponent>()},
            {"Fog", hasComponent<FogComponent>()},
            {"Skybox", hasComponent<SkyboxComponent>()},
            {"ReflectionProbe", hasComponent<ReflectionProbeComponent>()},
            {"FluidSimulation", hasComponent<FluidSimulationComponent>()},
#ifdef JASPERENGINE_PHYSICS_2D
            {"Rigidbody2D", hasComponent<Rigidbody2DComponent>()},
            {"Joint2D", hasComponent<Joint2DComponent>()},
#endif
#ifdef JASPERENGINE_PHYSICS_3D
            {"Rigidbody", hasComponent<RigidbodyComponent>()},
            {"Joint3D", hasComponent<Joint3DComponent>()},
#endif
            {"Action", hasComponent<ActionComponent>()},
            {"Tween", hasComponent<TweenComponent>()},
            {"Animation", hasComponent<AnimationComponent>()},
            {"ParticleSystem", hasComponent<ParticleSystemComponent>()},
            {"SpriteAnimation", hasComponent<SpriteAnimationComponent>()},
        };
        return types;
    }

    const WrapperType* findWrapperType(const std::string& name) {
        for (const WrapperType& type : wrapperTypes()) {
            if (name == type.name) return &type;
        }
        return nullptr;
    }

    int findObjectsOfType(lua_State* L, bool all) {
        const char* caller = all ? "GameObject.findObjectsOfType" : "GameObject.findObjectOfType";
        const std::string className = classArg(L, 1, caller, false);
        const bool includeInactive = lua_toboolean(L, 2) != 0;
        if (className == "GameObject" || className == "EntityHandle" || className == "Primitive") {
            return luaL_error(L, "%s: '%s' is not a component type (a Lua script class or a component wrapper such as \"Camera\")",
                              caller, className.c_str());
        }
        const WrapperType* wrapper = findWrapperType(className);
        if (all) {
            lua_newtable(L);
        }
        const int table = all ? lua_gettop(L) : 0;
        int count = 0;
        for (Scene* scene : findScenes()) {
            for (Entity entity : scene->getEntityList()) {
                if (!scene->isEntityCreated(entity)) {
                    continue;
                }
                const bool active = scene->isEntityActiveInHierarchy(entity);
                if (!includeInactive && !active) {
                    continue;
                }
                if (wrapper) {
                    if (!wrapper->has(scene, entity)) {
                        continue;
                    }
                    LuaBinding::pushEntityHandleByType(L, scene, entity, className);
                    if (!all) {
                        return 1;
                    }
                    lua_rawseti(L, table, ++count);
                    continue;
                }
                // Unity: without includeInactive, only scripts that are active and enabled
                const int before = count;
                count = pushScriptsOf(L, scene, entity, className, !includeInactive, table, count, all ? 0 : 1);
                if (!all && count > before) {
                    return 1;
                }
            }
        }
        if (!all) {
            lua_pushnil(L);
        }
        return 1;
    }

    int findWithTag(lua_State* L, bool all) {
        const char* caller = all ? "GameObject.findGameObjectsWithTag" : "GameObject.findWithTag";
        if (lua_type(L, 1) != LUA_TSTRING) {
            return luaL_error(L, "%s: tag must be a string", caller);
        }
        const std::string tag = lua_tostring(L, 1);
        if (all) {
            lua_newtable(L);
        }
        if (!TagManager::isTagDefined(tag)) {
            Log::error("Tag: %s is not defined.", tag.c_str());
            if (!all) {
                lua_pushnil(L);
            }
            return 1;
        }
        const int table = all ? lua_gettop(L) : 0;
        int count = 0;
        for (Scene* scene : findScenes()) {
            for (Entity entity : scene->findEntitiesByTag(tag)) {
                if (!scene->isEntityCreated(entity) || !scene->isEntityActiveInHierarchy(entity)) {
                    continue;
                }
                pushGameObject(L, scene, entity);
                if (!all) {
                    return 1;
                }
                lua_rawseti(L, table, ++count);
            }
        }
        if (!all) {
            lua_pushnil(L);
        }
        return 1;
    }

}

int LuaBinding::luaGetScript(lua_State* L) {
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    checkObject(L, "getScript", scene, entity);
    const std::string className = classArg(L, 2, "getScript", false);
    if (pushScriptsOf(L, scene, entity, className, false, 0, 0, 1) == 0) {
        lua_pushnil(L);
    }
    return 1;
}

int LuaBinding::luaGetScripts(lua_State* L) {
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    checkObject(L, "getScripts", scene, entity);
    const std::string className = classArg(L, 2, "getScripts", true);
    lua_newtable(L);
    pushScriptsOf(L, scene, entity, className, false, lua_gettop(L), 0, 0);
    return 1;
}

int LuaBinding::luaGetScriptInChildren(lua_State* L) {
    return getScriptInChildren(L, false);
}

int LuaBinding::luaGetScriptsInChildren(lua_State* L) {
    return getScriptInChildren(L, true);
}

int LuaBinding::luaGetScriptInParent(lua_State* L) {
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    checkObject(L, "getScriptInParent", scene, entity);
    const std::string className = classArg(L, 2, "getScriptInParent", false);
    const bool includeInactive = lua_toboolean(L, 3) != 0;
    for (int depth = 0; entity != NULL_ENTITY && scene->isEntityCreated(entity) && depth < 1024; depth++) {
        if ((includeInactive || scene->isEntityActiveInHierarchy(entity)) &&
                pushScriptsOf(L, scene, entity, className, false, 0, 0, 1) > 0) {
            return 1;
        }
        entity = parentOf(scene, entity);
    }
    lua_pushnil(L);
    return 1;
}

int LuaBinding::luaFind(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "GameObject.find: name must be a string");
    }
    std::string text = lua_tostring(L, 1);
    const bool fromRoot = !text.empty() && text[0] == '/';
    std::vector<std::string> path;
    size_t start = fromRoot ? 1 : 0;
    while (start <= text.size()) {
        const size_t slash = text.find('/', start);
        path.push_back(text.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (path.empty() || path.back().empty()) {
        lua_pushnil(L);
        return 1;
    }
    for (Scene* scene : findScenes()) {
        for (Entity entity : scene->getEntityList()) {
            if (scene->isEntityCreated(entity) && scene->getEntityNameRef(entity) == path.back() &&
                    scene->isEntityActiveInHierarchy(entity) && matchesPath(scene, entity, path, fromRoot)) {
                pushGameObject(L, scene, entity);
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

int LuaBinding::luaFindWithTag(lua_State* L) {
    return findWithTag(L, false);
}

int LuaBinding::luaFindGameObjectsWithTag(lua_State* L) {
    return findWithTag(L, true);
}

int LuaBinding::luaFindObjectOfType(lua_State* L) {
    return findObjectsOfType(L, false);
}

int LuaBinding::luaFindObjectsOfType(lua_State* L) {
    return findObjectsOfType(L, true);
}

// ---- persistent objects (Unity's DontDestroyOnLoad, SceneManager.MoveGameObjectToScene)

namespace {

    // The live Lua instance of `className` on a GameObject of `scene`'s `entities` (ascending
    // ids), other than `self`, pushed; false (nothing pushed) without one
    bool pushOtherInstance(lua_State* L, Scene* scene, const std::vector<Entity>& entities, const std::string& className, const void* self) {
        for (Entity entity : entities) {
            ScriptComponent* scripts = scene->isEntityCreated(entity) ? scene->findComponent<ScriptComponent>(entity) : nullptr;
            if (!scripts) {
                continue;
            }
            for (const ScriptEntry& entry : scripts->scripts) {
                if (entry.type != ScriptType::LUA || entry.className != className || !entry.instance) {
                    continue;
                }
                const ScriptState* state = ScriptRuntime::getState(entry.instance);
                if (state && (state->destroyed || state->destroying)) {
                    continue;
                }
                LuaBinding::pushScriptInstance(L, entry);
                if (lua_topointer(L, -1) != self) {
                    return true;
                }
                lua_pop(L, 1);
            }
        }
        return false;
    }

    void persistentObjectArg(lua_State* L, const char* caller, Scene*& scene, Entity& entity) {
        if (!objectArg(L, 1, scene, entity)) {
            luaL_error(L, "%s: argument 1 must be a GameObject or a script (self)", caller);
        }
    }

}

// DontDestroyOnLoad(obj) and GameObject.dontDestroyOnLoad(obj): nil is ignored
int LuaBinding::luaDontDestroyOnLoad(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    persistentObjectArg(L, "DontDestroyOnLoad", scene, entity);
    lua_pushboolean(L, SceneManager::dontDestroyOnLoad(scene, entity) ? 1 : 0);
    return 1;
}

// GameObject.isPersistent(obj)
int LuaBinding::luaIsPersistent(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    persistentObjectArg(L, "GameObject.isPersistent", scene, entity);
    lua_pushboolean(L, SceneManager::isPersistent(scene, entity) ? 1 : 0);
    return 1;
}

// GameObject.persistSingleton(self): the surviving instance of self's class among the
// persistent objects (self made persistent when there is none, else self's GameObject goes)
int LuaBinding::luaPersistSingleton(lua_State* L) {
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    if (!lua_istable(L, 1) || !objectArg(L, 1, scene, entity)) {
        return luaL_error(L, "GameObject.persistSingleton: argument 1 must be a script instance (self)");
    }
    lua_pushstring(L, "__name");
    lua_rawget(L, 1);
    const std::string className = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    if (className.empty()) {
        return luaL_error(L, "GameObject.persistSingleton: argument 1 must be a script instance (self)");
    }
    const void* self = lua_topointer(L, 1);

    // one already persistent, or marked earlier (two duplicates starting in one pass: the
    // first wins)
    bool found = false;
    if (Scene* persistent = SceneManager::getPersistentScene()) {
        std::vector<Entity> entities;
        auto scripts = persistent->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scripts->size(); i++) {
            entities.push_back(scripts->getEntity(i));
        }
        std::sort(entities.begin(), entities.end());
        found = pushOtherInstance(L, persistent, entities, className, self);
    }
    if (!found) {
        for (const auto& [from, root] : SceneManager::getPendingPersistentRoots()) {
            if (pushOtherInstance(L, from, EntityCloner::collectHierarchy(from, root), className, self)) {
                found = true;
                break;
            }
        }
    }
    if (found) {
        // Unity's `else if (instance != this) Destroy(gameObject)`
        ObjectManager::destroy(scene, entity);
        return 1;
    }

    SceneManager::dontDestroyOnLoad(scene, entity);
    lua_pushvalue(L, 1);
    return 1;
}

// SceneManager.moveGameObjectToScene(obj, scene)
int LuaBinding::luaMoveGameObjectToScene(lua_State* L) {
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    persistentObjectArg(L, "SceneManager.moveGameObjectToScene", scene, entity);
    Scene* target = nullptr;
    if (luabridge::isInstance<Scene*>(L, 2)) {
        auto result = luabridge::get<Scene*>(L, 2);
        if (result) {
            target = result.value();
        }
    } else if (!lua_isnoneornil(L, 2)) {
        return luaL_error(L, "SceneManager.moveGameObjectToScene: argument 2 must be a Scene");
    }
    lua_pushboolean(L, SceneManager::moveGameObjectToScene(scene, entity, target) ? 1 : 0);
    return 1;
}

void LuaBinding::registerScriptClasses(lua_State* L) {
    luabridge::getGlobalNamespace(L)
        .beginClass<ScriptEvent>("Event")
        .addConstructor<void (*) ()>()
        .addFunction("addListener", &ScriptEvent::luaAddListener)
        .addFunction("removeListener", &ScriptEvent::luaRemoveListener)
        .addFunction("removeAllListeners", &ScriptEvent::luaRemoveAllListeners)
        .addFunction("invoke", &ScriptEvent::luaInvoke)
        .addFunction("add", &ScriptEvent::luaAdd)
        .addFunction("remove", &ScriptEvent::luaRemove)
        .addFunction("has", &ScriptEvent::luaHas)
        .addProperty("listenerCount", &ScriptEvent::listenerCount)
        .addFunction("__tostring", &ScriptEvent::luaToString)
        .endClass();
}
