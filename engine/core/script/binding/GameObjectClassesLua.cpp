// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"
#include "Scene.h"
#include "subsystem/RenderSystem.h"

#include "EntityHandle.h"
#include "Fog.h"
#include "ReflectionProbe.h"
#include "object/FluidSimulation.h"
#include "Skybox.h"
#include "Water.h"
#include "GameObject.h"
#include "Camera.h"
#include "Polygon.h"
#include "Terrain.h"
#include "Light.h"
#include "Light2D.h"
#include "ShadowCaster2D.h"
#include "Mesh.h"
#include "Primitive.h"
#include "Mirror.h"
#include "Lattice.h"
#include "Tilemap.h"
#include "Bone.h"
#include "Model.h"
#include "PoseAnimator.h"
#include "AnimationPlayer.h"
#include "VertexAnimator.h"
#include "animation/AnimationEvent.h"
#include "MeshPolygon.h"
#include "Points.h"
#include "LineRenderer.h"
#include "Sprite.h"
#include "object/ui/Text.h"   // by path: lexbor has a dom/interfaces/text.h (Xcode header maps ignore case)
#include "Image.h"
#include "Button.h"
#include "Panel.h"
#include "Scrollbar.h"
#include "Progressbar.h"
#include "InputField.h"
#include "LayoutGroup.h"
#include "AudioSource.h"
#include "Engine.h"
#include "ObjectManager.h"
#include "PrefabAsset.h"
#include "physics/PhysicsMaterial.h"
#include "component/PhysicsCommon.h"
#include <optional>
#ifdef JASPERENGINE_PHYSICS_2D
#include "Rigidbody2D.h"
#include "Joint2D.h"
#include "Contact2D.h"
#include "Manifold2D.h"
#include "Collision2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "Rigidbody.h"
#include "Joint3D.h"
#include "Contact3D.h"
#include "CollideShapeResult3D.h"
#include "Collision.h"
#include <stdexcept>
#endif

using namespace jasperengine;

// The contact callbacks' values (physics-contacts): what they name may be gone since (nil then)
namespace {

    template<typename Ref>
    std::optional<GameObject> luaColliderGameObject(const Ref& ref){
        return ref.hasGameObject() ? std::optional<GameObject>(ref.getGameObject()) : std::nullopt;
    }

    template<typename Ref>
    std::optional<GameObject> luaColliderReceiver(const Ref& ref){
        const Entity receiver = ref.getReceiver();
        return receiver != NULL_ENTITY ? std::optional<GameObject>(GameObject(ref.scene, receiver)) : std::nullopt;
    }

    // a new 1-based table of the contact points
    template<typename CollisionT>
    luabridge::LuaRef luaContactsTable(const CollisionT* self, lua_State* L){
        luabridge::LuaRef table = luabridge::newTable(L);
        for (size_t i = 0; i < self->contacts.size(); i++){
            table[(int)i + 1] = self->contacts[i];
        }
        return table;
    }

    // getContact(i): 0-based, an error out of range (Unity throws)
    template<typename CollisionT>
    int luaGetContact(lua_State* L){
        auto self = luabridge::get<const CollisionT*>(L, 1);
        if (!self || !self.value()){
            return luaL_error(L, "getContact: call it on a collision (collision:getContact(i))");
        }
        const lua_Integer index = luaL_checkinteger(L, 2);
        const CollisionT* collision = self.value();
        if (index < 0 || (size_t)index >= collision->contacts.size()){
            return luaL_error(L, "getContact(%d): index out of range, the collision has %d contacts (0-based)", (int)index, (int)collision->contacts.size());
        }
        if (!luabridge::push(L, collision->contacts[(size_t)index])){
            lua_pushnil(L);
        }
        return 1;
    }

}

// Layer masks read as Unity's int (Everything is -1) and take any integer, keeping its
// low 32 bits, so both -1 and ~LayerMask.getMask("UI") work
template<typename T>
static lua_Integer getLuaCullingMask(const T* self){
    return (lua_Integer)(int32_t)self->getCullingMask();
}

template<typename T>
static void setLuaCullingMask(T* self, lua_Integer mask){
    self->setCullingMask((uint32_t)mask);
}


// ---- GameObject.instantiate / destroy / destroyImmediate (Unity's Object.Instantiate...) ----

namespace {

// A GameObject argument: any GameObject handle (EntityHandle and what derives from it), or
// a Lua script instance (its self: a table with scene and entity), whose class is returned
bool luaObjectArg(lua_State* L, int index, Scene*& scene, Entity& entity, std::string* scriptClass = nullptr){
    if (luabridge::isInstance<EntityHandle>(L, index)){
        auto handle = luabridge::get<EntityHandle*>(L, index);
        if (!handle || !handle.value()){
            return false;
        }
        scene = handle.value()->getScene();
        entity = handle.value()->getEntity();
        return true;
    }
    if (lua_istable(L, index)){
        const int top = lua_gettop(L);
        lua_getfield(L, index, "scene");
        lua_getfield(L, index, "entity");
        bool ok = false;
        if (luabridge::isInstance<Scene*>(L, -2) && lua_isinteger(L, -1)){
            auto sceneResult = luabridge::get<Scene*>(L, -2);
            if (sceneResult && sceneResult.value()){
                scene = sceneResult.value();
                entity = (Entity)lua_tointeger(L, -1);
                ok = true;
            }
        }
        if (ok && scriptClass){
            lua_getfield(L, index, "__name");
            if (lua_isstring(L, -1)){
                *scriptClass = lua_tostring(L, -1);
            }
        }
        lua_settop(L, top);
        return ok;
    }
    return false;
}

template<typename T>
bool luaValueArg(lua_State* L, int index, T& value){
    if (!luabridge::isInstance<T>(L, index)){
        return false;
    }
    auto result = luabridge::get<T>(L, index);
    if (!result){
        return false;
    }
    value = result.value();
    return true;
}

// GameObject.instantiate(original [, ...]): original is a PrefabAsset, a prefab path or
// name, a prefab id, a GameObject or a script (self). Then:
//   (parent [, instantiateInWorldSpace])     parent: a GameObject or a script
//   (position, rotation [, parent])          world values
//   (scene [, position, rotation])           a prefab in that scene
// Returns the new GameObject (for a script, the copy's script of the same class), or nil.
int luaInstantiate(lua_State* L){
    const int argc = lua_gettop(L);

    PrefabAsset prefab;
    bool fromPrefab = false;
    Scene* originalScene = nullptr;
    Entity original = NULL_ENTITY;
    std::string scriptClass;

    if (luaValueArg<PrefabAsset>(L, 1, prefab)){
        fromPrefab = true;
    }else if (lua_type(L, 1) == LUA_TSTRING){
        prefab = PrefabAsset::load(std::string(lua_tostring(L, 1)));
        fromPrefab = true;
    }else if (lua_type(L, 1) == LUA_TNUMBER){
        prefab = PrefabAsset::load((uint32_t)lua_tointeger(L, 1));
        fromPrefab = true;
    }else if (!luaObjectArg(L, 1, originalScene, original, &scriptClass)){
        return luaL_error(L, "GameObject.instantiate: argument 1 must be a PrefabAsset, a prefab path, a GameObject or a script");
    }
    if (fromPrefab && prefab.getId() == 0){
        lua_pushnil(L);
        return 1;
    }

    Scene* scene = nullptr;
    Scene* parentScene = nullptr;
    Entity parent = NULL_ENTITY;
    bool worldSpace = false;
    bool placed = false;
    Vector3 position;
    Quaternion rotation;

    int next = 2;
    if (argc >= next && luaValueArg<Scene*>(L, next, scene)){
        if (!fromPrefab){
            return luaL_error(L, "GameObject.instantiate: a scene is given with a prefab only; a copy goes to its parent's scene");
        }
        next++;
    }
    if (argc >= next && luabridge::isInstance<Vector3>(L, next)){
        if (!luaValueArg<Vector3>(L, next, position) || !luaValueArg<Quaternion>(L, next + 1, rotation)){
            return luaL_error(L, "GameObject.instantiate: a position goes with a rotation (Quaternion)");
        }
        placed = true;
        next += 2;
    }
    if (argc >= next && !lua_isnil(L, next)){
        if (!luaObjectArg(L, next, parentScene, parent)){
            return luaL_error(L, "GameObject.instantiate: argument %d must be the parent GameObject", next);
        }
        if (!placed && argc >= next + 1){
            worldSpace = lua_toboolean(L, next + 1) != 0;
        }
    }

    Entity root = NULL_ENTITY;
    Scene* resultScene = nullptr;
    if (fromPrefab){
        resultScene = parent != NULL_ENTITY ? parentScene : (scene ? scene : ObjectManager::getActiveScene());
        root = placed ? ObjectManager::instantiate(prefab, resultScene, position, rotation, parent)
                      : ObjectManager::instantiate(prefab, resultScene, parent, worldSpace);
    }else{
        resultScene = parent != NULL_ENTITY ? parentScene : originalScene;
        root = placed ? ObjectManager::instantiate(originalScene, original, resultScene, position, rotation, parent)
                      : ObjectManager::instantiate(originalScene, original, resultScene, parent, worldSpace);
    }

    if (root == NULL_ENTITY){
        lua_pushnil(L);
        return 1;
    }
    if (!scriptClass.empty() && LuaBinding::pushScriptObject(L, resultScene, root, scriptClass)){
        return 1;
    }
    if (!luabridge::push<GameObject>(L, GameObject(resultScene, root))){
        lua_pushnil(L);
    }
    return 1;
}

// GameObject.destroy(object [, delay]) and GameObject.destroyImmediate(object): a
// GameObject or a script (its GameObject goes)
int luaDestroy(lua_State* L){
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    if (lua_isnil(L, 1)){
        return 0;
    }
    if (!luaObjectArg(L, 1, scene, entity)){
        return luaL_error(L, "GameObject.destroy: argument 1 must be a GameObject or a script");
    }
    const float delay = (float)luaL_optnumber(L, 2, 0.0);
    ObjectManager::destroy(scene, entity, delay);
    return 0;
}

int luaDestroyImmediate(lua_State* L){
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    if (lua_isnil(L, 1)){
        return 0;
    }
    if (!luaObjectArg(L, 1, scene, entity)){
        return luaL_error(L, "GameObject.destroyImmediate: argument 1 must be a GameObject or a script");
    }
    ObjectManager::destroyImmediate(scene, entity);
    return 0;
}

}

// ---- 2D physics: bodies given to joints, materials given to bodies and colliders ----

namespace {

    // a material argument: a .physicsmaterial path, a PhysicsMaterial or nil (none)
    bool luaMaterialArg(const luabridge::LuaRef& value, PhysicsMaterial& material, std::string& path){
        material = PhysicsMaterial();
        path.clear();
        if (value.isNil()){
            return true;
        }
        if (value.isString()){
            path = value.tostring();
            return true;
        }
        if (value.isInstance<PhysicsMaterial>()){
            material = value.cast<PhysicsMaterial>().value();
            return true;
        }
        return false;
    }

    std::optional<PhysicsMaterial> luaMaterialResult(const PhysicsMaterial& material){
        if (!material.valid()){
            return std::nullopt;
        }
        return material;
    }

    PhysicsMaterialCombine luaCombineArg(const luabridge::LuaRef& value, PhysicsMaterialCombine fallback){
        if (value.isNumber()){
            const int mode = value.unsafe_cast<int>();
            if (mode >= (int)PhysicsMaterialCombine::AVERAGE && mode <= (int)PhysicsMaterialCombine::MAXIMUM){
                return (PhysicsMaterialCombine)mode;
            }
        }
        if (value.isString()){
            PhysicsMaterialCombine mode;
            if (physicsmaterial::combineFromName(value.tostring(), mode)){
                return mode;
            }
        }
        return fallback;
    }

#ifdef JASPERENGINE_PHYSICS_2D
    // a joint body argument: a GameObject (or any handle, a script) with a Rigidbody 2D, or nil
    Entity luaJointBodyArg(Joint2D* self, const luabridge::LuaRef& value, lua_State* L, const char* property){
        if (value.isNil()){
            return NULL_ENTITY;
        }
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        const int top = lua_gettop(L);
        value.push(L);
        const bool isObject = luaObjectArg(L, lua_gettop(L), scene, entity);
        lua_settop(L, top);
        if (!isObject){
            // an exception (LuaBridge turns it into a Lua error): luaL_error would longjmp past
            // the C++ objects alive here
            throw std::runtime_error(std::string("Joint2D.") + property + ": a GameObject or Rigidbody2D with a Rigidbody 2D, or nil");
        }
        if (scene != self->getScene() || !scene->isEntityCreated(entity) || !scene->findComponent<Rigidbody2DComponent>(entity)){
            throw std::runtime_error(std::string("Joint2D.") + property + ": \"" + (scene ? scene->getEntityName(entity) : std::string("?")) +
                                     "\" has no Rigidbody 2D (in the joint's scene)");
        }
        return entity;
    }

    std::optional<GameObject> luaJointBody(const Joint2D* self, Entity body){
        Scene* scene = self->getScene();
        if (body == NULL_ENTITY || !scene || !scene->isEntityCreated(body)){
            return std::nullopt;
        }
        return GameObject(scene, body);
    }
#endif

}

namespace {
    // the shader a component draws with: its customShader, else the scene's default
    template <class T>
    std::string componentShader(const T* self, ShaderType type){
        const std::string shader = self->getCustomShader();
        Scene* scene = self->getScene();
        return (shader.empty() && scene) ? scene->getDefaultCustomShader(type) : shader;
    }
}

void LuaBinding::registerGameObjectClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    // Mesh:getMaterial: the submesh's material, a copy that knows the shader the submesh draws
    // with when the material names none (the mesh's customShader, else the scene default)
    static luabridge::LuaRef (*meshMaterial)(const Mesh*, unsigned int, lua_State*) =
        +[](const Mesh* self, unsigned int submesh, lua_State* L) -> luabridge::LuaRef {
            const MeshComponent& mesh = self->getComponent<MeshComponent>();
            luabridge::LuaRef ref(L, self->getMaterial(submesh));
            if (submesh < mesh.numSubmeshes && mesh.submeshes[submesh].material.shader.empty() && self->getScene()) {
                const std::string drawn = RenderSystem::getSubmeshShader(mesh, mesh.submeshes[submesh],
                    self->getScene()->getDefaultCustomShader(ShaderType::MESH));
                ref.push(L);
                LuaBinding::setMaterialDrawnShader(L, -1, drawn);
                lua_pop(L, 1);
            }
            return ref;
        };

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CameraType")
        .addVariable("CAMERA_UI", CameraType::CAMERA_UI)
        .addVariable("CAMERA_ORTHO", CameraType::CAMERA_ORTHO)
        .addVariable("CAMERA_PERSPECTIVE", CameraType::CAMERA_PERSPECTIVE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FrustumPlane")
        .addVariable("FRUSTUM_PLANE_NEAR", FrustumPlane::FRUSTUM_PLANE_NEAR)
        .addVariable("FRUSTUM_PLANE_FAR", FrustumPlane::FRUSTUM_PLANE_FAR)
        .addVariable("FRUSTUM_PLANE_LEFT", FrustumPlane::FRUSTUM_PLANE_LEFT)
        .addVariable("FRUSTUM_PLANE_RIGHT", FrustumPlane::FRUSTUM_PLANE_RIGHT)
        .addVariable("FRUSTUM_PLANE_TOP", FrustumPlane::FRUSTUM_PLANE_TOP)
        .addVariable("FRUSTUM_PLANE_BOTTOM", FrustumPlane::FRUSTUM_PLANE_BOTTOM)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LightType")
        .addVariable("DIRECTIONAL", LightType::DIRECTIONAL)
        .addVariable("POINT", LightType::POINT)
        .addVariable("SPOT", LightType::SPOT)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LightShading")
        .addVariable("BOTH", LightShading::BOTH)
        .addVariable("PIXEL", LightShading::PIXEL)
        .addVariable("VERTEX", LightShading::VERTEX)
        .endNamespace();

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .beginNamespace("Collider2DType")
        .addVariable("POLYGON", Collider2DType::POLYGON)
        .addVariable("CIRCLE", Collider2DType::CIRCLE)
        .addVariable("CAPSULE", Collider2DType::CAPSULE)
        .addVariable("SEGMENT", Collider2DType::SEGMENT)
        .addVariable("CHAIN", Collider2DType::CHAIN)
        .endNamespace();
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginNamespace("Collider3DType")
        .addVariable("SPHERE", Collider3DType::SPHERE)
        .addVariable("BOX", Collider3DType::BOX)
        .addVariable("CAPSULE", Collider3DType::CAPSULE)
        .addVariable("TAPERED_CAPSULE", Collider3DType::TAPERED_CAPSULE)
        .addVariable("CYLINDER", Collider3DType::CYLINDER)
        .addVariable("CONVEX_HULL", Collider3DType::CONVEX_HULL)
        .addVariable("MESH", Collider3DType::MESH)
        .addVariable("HEIGHTFIELD", Collider3DType::HEIGHTFIELD)
        .endNamespace();
#endif

    luabridge::getGlobalNamespace(L)
        .beginNamespace("BodyType")
        .addVariable("STATIC", BodyType::STATIC)
        .addVariable("KINEMATIC", BodyType::KINEMATIC)
        .addVariable("DYNAMIC", BodyType::DYNAMIC)
        .endNamespace();

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginNamespace("RigidbodyMotionQuality")
        .addVariable("DISCRETE", RigidbodyMotionQuality::DISCRETE)
        .addVariable("LINEAR_CAST", RigidbodyMotionQuality::LINEAR_CAST)
        .endNamespace();
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .beginNamespace("Joint2DType")
        .addVariable("DISTANCE", Joint2DType::DISTANCE)
        .addVariable("REVOLUTE", Joint2DType::REVOLUTE)
        .addVariable("PRISMATIC", Joint2DType::PRISMATIC)
        .addVariable("MOUSE", Joint2DType::MOUSE)
        .addVariable("WHEEL", Joint2DType::WHEEL)
        .addVariable("WELD", Joint2DType::WELD)
        .addVariable("MOTOR", Joint2DType::MOTOR)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("JointLimitState2D")
        .addVariable("INACTIVE", JointLimitState2D::INACTIVE)
        .addVariable("LOWER", JointLimitState2D::LOWER)
        .addVariable("UPPER", JointLimitState2D::UPPER)
        .addVariable("EQUAL", JointLimitState2D::EQUAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CollisionDetectionMode2D")
        .addVariable("DISCRETE", CollisionDetectionMode2D::DISCRETE)
        .addVariable("CONTINUOUS", CollisionDetectionMode2D::CONTINUOUS)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("RigidbodySleepMode2D")
        .addVariable("NEVER_SLEEP", RigidbodySleepMode2D::NEVER_SLEEP)
        .addVariable("START_AWAKE", RigidbodySleepMode2D::START_AWAKE)
        .addVariable("START_ASLEEP", RigidbodySleepMode2D::START_ASLEEP)
        .endNamespace();
#endif

    // shared by 2D and 3D joints and materials (component/PhysicsCommon.h)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("JointAnchorSpace")
        .addVariable("WORLD", JointAnchorSpace::WORLD)
        .addVariable("LOCAL", JointAnchorSpace::LOCAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("JointBreakAction")
        .addVariable("IGNORE", JointBreakAction::IGNORE)
        .addVariable("CALLBACK_ONLY", JointBreakAction::CALLBACK_ONLY)
        .addVariable("DISABLE", JointBreakAction::DISABLE)
        .addVariable("DESTROY", JointBreakAction::DESTROY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("PhysicsMaterialCombine")
        .addVariable("AVERAGE", PhysicsMaterialCombine::AVERAGE)
        .addVariable("GEOMETRIC_MEAN", PhysicsMaterialCombine::GEOMETRIC_MEAN)
        .addVariable("MINIMUM", PhysicsMaterialCombine::MINIMUM)
        .addVariable("MULTIPLY", PhysicsMaterialCombine::MULTIPLY)
        .addVariable("MAXIMUM", PhysicsMaterialCombine::MAXIMUM)
        .endNamespace();

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginNamespace("Joint3DType")
        .addVariable("FIXED", Joint3DType::FIXED)
        .addVariable("DISTANCE", Joint3DType::DISTANCE)
        .addVariable("POINT", Joint3DType::POINT)
        .addVariable("HINGE", Joint3DType::HINGE)
        .addVariable("CONE", Joint3DType::CONE)
        .addVariable("PRISMATIC", Joint3DType::PRISMATIC)
        .addVariable("SWINGTWIST", Joint3DType::SWINGTWIST)
        .addVariable("SIXDOF", Joint3DType::SIXDOF)
        .addVariable("PATH", Joint3DType::PATH)
        .addVariable("GEAR", Joint3DType::GEAR)
        .addVariable("RACKANDPINON", Joint3DType::RACKANDPINON)
        .addVariable("PULLEY", Joint3DType::PULLEY)
        .endNamespace();
#endif

    luabridge::getGlobalNamespace(L)
        .beginClass<EntityHandle>("EntityHandle")
        .addConstructor <void (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("scene", &EntityHandle::getScene)
        .addProperty("entity", &EntityHandle::getEntity)
        .addProperty("name", &EntityHandle::getName, &EntityHandle::setName)
        .addProperty("entityOwned", &EntityHandle::isEntityOwned, &EntityHandle::setEntityOwned)
        .addFunction("setActive", &EntityHandle::setActive)
        .addFunction("isActive", &EntityHandle::isActive)
        .addFunction("isActiveInHierarchy", &EntityHandle::isActiveInHierarchy)
        .addProperty("activeSelf", &EntityHandle::isActive)
        .addProperty("activeInHierarchy", &EntityHandle::isActiveInHierarchy)
        .addProperty("tag", &EntityHandle::getTag, &EntityHandle::setTag)
        .addFunction("compareTag", &EntityHandle::compareTag)
        .addProperty("layer", &EntityHandle::getLayer, &EntityHandle::setLayer)
        // Unity's GetComponent<Script>() family: the Lua script instances of this GameObject
        .addFunction("getScript", &LuaBinding::luaGetScript)
        .addFunction("getScripts", &LuaBinding::luaGetScripts)
        .addFunction("getScriptInChildren", &LuaBinding::luaGetScriptInChildren)
        .addFunction("getScriptsInChildren", &LuaBinding::luaGetScriptsInChildren)
        .addFunction("getScriptInParent", &LuaBinding::luaGetScriptInParent)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Fog, EntityHandle>("Fog")
        .addConstructor <void (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("type", &Fog::getType, &Fog::setType)
        .addProperty("color", &Fog::getColor, (void(Fog::*)(Vector3))&Fog::setColor)
        .addFunction("setColor", (void(Fog::*)(const float, const float, const float))&Fog::setColor)
        .addProperty("density", &Fog::getDensity, &Fog::setDensity)
        .addProperty("linearStart", &Fog::getLinearStart, &Fog::setLinearStart)
        .addProperty("linearEnd", &Fog::getLinearEnd, &Fog::setLinearEnd)
        .addFunction("setLinearStartEnd", &Fog::setLinearStartEnd)
        .endClass();

    // the water surfaces (meshes drawn with library:water) at a point, with their waves
    luabridge::getGlobalNamespace(L)
        .beginClass<Water>("Water")
        .addStaticFunction("getHeight", &Water::getHeight)
        .addStaticFunction("getNormal", &Water::getNormal)
        .addStaticFunction("isUnderwater", &Water::isUnderwater)
        .addStaticFunction("getTime", &Water::getTime)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Skybox, EntityHandle>("Skybox")
        .addConstructor <void (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setTextures", 
            luabridge::overload<const std::string&,const std::string&,const std::string&,const std::string&,const std::string&,const std::string&>(&Skybox::setTextures),
            luabridge::overload<const std::string&,TextureData,TextureData,TextureData,TextureData,TextureData,TextureData>(&Skybox::setTextures))
        .addFunction("setTexture", &Skybox::setTexture)
        .addFunction("setTexturePositiveX", &Skybox::setTexturePositiveX)
        .addFunction("setTextureNegativeX", &Skybox::setTextureNegativeX)
        .addFunction("setTexturePositiveY", &Skybox::setTexturePositiveY)
        .addFunction("setTextureNegativeY", &Skybox::setTextureNegativeY)
        .addFunction("setTexturePositiveZ", &Skybox::setTexturePositiveZ)
        .addFunction("setTextureNegativeZ", &Skybox::setTextureNegativeZ)
        .addProperty("color", &Skybox::getColor, (void(Skybox::*)(Vector4))&Skybox::setColor)
        .addFunction("setColor", 
            luabridge::overload<const float, const float, const float>(&Skybox::setColor),
            luabridge::overload<const float, const float, const float, const float>(&Skybox::setColor))
        .addProperty("alpha", &Skybox::getAlpha, &Skybox::setAlpha)
        .addProperty("rotation", &Skybox::getRotation, &Skybox::setRotation)
        .addProperty("customShader", &Skybox::getCustomShader, &Skybox::setCustomShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&Skybox::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&Skybox::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&Skybox::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Skybox::setShaderUniform))
        .addFunction("getShaderUniform", &Skybox::getShaderUniform)
        .addFunction("removeShaderUniform", &Skybox::removeShaderUniform)
        // shader library: setShaderParam sets a @property (as setShaderUniform), getShaderParam
        // falls back to the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<const std::string&, const Vector4&>(&Skybox::setShaderUniform),
            luabridge::overload<Skybox*, const std::string&, const Vector3&>(+[](Skybox* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                self->setShaderUniform(name, LuaBinding::getShaderParamValue(componentShader(self, ShaderType::SKYBOX), name, value));
            }),
            luabridge::overload<const std::string&, const Vector2&>(&Skybox::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Skybox::setShaderUniform))
        .addFunction("getShaderParam", &Skybox::getShaderParam)
        .addFunction("removeShaderParam", &Skybox::removeShaderUniform)
        .addFunction("setShaderTexture", &Skybox::setShaderTexture)
        .addFunction("getShaderTexture", &Skybox::getShaderTexture)
        .addFunction("setShaderFeature", &Skybox::setShaderFeature)
        .addFunction("isShaderFeatureEnabled", &Skybox::isShaderFeatureEnabled)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<GameObject, EntityHandle>("GameObject")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addStaticFunction("instantiate", &luaInstantiate)
        .addStaticFunction("destroy", &luaDestroy)
        .addStaticFunction("destroyImmediate", &luaDestroyImmediate)
        // scene-wide lookups (Unity's GameObject.Find, FindWithTag, FindObjectOfType...)
        .addStaticFunction("find", &LuaBinding::luaFind)
        .addStaticFunction("findWithTag", &LuaBinding::luaFindWithTag)
        .addStaticFunction("findGameObjectsWithTag", &LuaBinding::luaFindGameObjectsWithTag)
        .addStaticFunction("findObjectOfType", &LuaBinding::luaFindObjectOfType)
        .addStaticFunction("findObjectsOfType", &LuaBinding::luaFindObjectsOfType)
        .addFunction("addChild", 
            luabridge::overload<GameObject*>(&GameObject::addChild),
            luabridge::overload<Entity>(&GameObject::addChild))
        .addFunction("removeChild", 
            luabridge::overload<GameObject*>(&GameObject::removeChild),
            luabridge::overload<Entity>(&GameObject::removeChild))
        .addFunction("removeParent", &GameObject::removeParent)
        .addFunction("moveToTop", &GameObject::moveToTop)
        .addFunction("moveUp", &GameObject::moveUp)
        .addFunction("moveDown", &GameObject::moveDown)
        .addFunction("moveToBottom", &GameObject::moveToBottom)
        .addProperty("position", &GameObject::getPosition, (void(GameObject::*)(Vector3))&GameObject::setPosition)
        .addFunction("setPosition", 
            luabridge::overload<const float, const float, const float>(&GameObject::setPosition),
            luabridge::overload<const float, const float>(&GameObject::setPosition))
        .addFunction("getWorldPosition", &GameObject::getWorldPosition)
        .addProperty("rotation", &GameObject::getRotation, (void(GameObject::*)(Quaternion))&GameObject::setRotation)
        .addFunction("setRotation", (void(GameObject::*)(const float, const float, const float))&GameObject::setRotation)
        .addFunction("getWorldRotation", &GameObject::getWorldRotation)
        .addProperty("scale", &GameObject::getScale, (void(GameObject::*)(Vector3))&GameObject::setScale)
        .addFunction("setScale", (void(GameObject::*)(const float))&GameObject::setScale)
        .addFunction("getWorldScale", &GameObject::getWorldScale)
        .addProperty("visible", &GameObject::isVisible, &GameObject::setVisible)
        .addFunction("setVisibleOnly", &GameObject::setVisibleOnly)
        .addFunction("setBillboard", (void(GameObject::*)(bool, bool, bool))&GameObject::setBillboard)
        .addProperty("billboard", &GameObject::isBillboard, (void(GameObject::*)(bool))&GameObject::setBillboard)
        .addProperty("billboardRotation", &GameObject::getBillboardRotation, (void(GameObject::*)(Quaternion))&GameObject::setBillboardRotation)
        .addFunction("setBillboardRotation", (void(GameObject::*)(const float, const float, const float))&GameObject::setBillboardRotation)
        .addProperty("fakeBillboard", &GameObject::isFakeBillboard, &GameObject::setFakeBillboard)
        .addProperty("cylindricalBillboard", &GameObject::isCylindricalBillboard, &GameObject::setCylindricalBillboard)
        .addProperty("localMatrix", &GameObject::getLocalMatrix, &GameObject::setLocalMatrix)
        .addProperty("modelMatrix", &GameObject::getModelMatrix)
        .addProperty("normalMatrix", &GameObject::getNormalMatrix)
        .addFunction("updateTransform", &GameObject::updateTransform)
#ifdef JASPERENGINE_PHYSICS_2D
        .addFunction("getRigidbody2D", &GameObject::getRigidbody2D)
        .addFunction("removeRigidbody2D", &GameObject::removeRigidbody2D)
        // the Joint 2D of the GameObject, nil when it has none
        .addFunction("getJoint2D", +[](GameObject* self) -> std::optional<Joint2D> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<Joint2DComponent>(self->getEntity())){
                return std::nullopt;
            }
            return Joint2D(scene, self->getEntity());
        })
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        .addFunction("getRigidbody", &GameObject::getRigidbody)
        .addFunction("removeRigidbody", &GameObject::removeRigidbody)
#endif
        .addFunction("getRay", &GameObject::getRay)
        // the Animation Player of the GameObject, nil when it has none (Unity's GetComponent)
        .addFunction("getAnimationPlayer", +[](GameObject* self) -> std::optional<AnimationPlayer> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<AnimationPlayerComponent>(self->getEntity())){
                return std::nullopt;
            }
            return AnimationPlayer(scene, self->getEntity());
        })
        .addFunction("hasAnimationPlayer", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<AnimationPlayerComponent>(self->getEntity()) != nullptr;
        })
        // the Vertex Animator of the GameObject (baked .vat clips on the GPU), nil when it has none
        .addFunction("getVertexAnimator", +[](GameObject* self) -> std::optional<VertexAnimator> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<VertexAnimatorComponent>(self->getEntity())){
                return std::nullopt;
            }
            return VertexAnimator(scene, self->getEntity());
        })
        .addFunction("hasVertexAnimator", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<VertexAnimatorComponent>(self->getEntity()) != nullptr;
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Camera, GameObject>("Camera")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Camera::isEnabled, &Camera::setEnabled)
        .addFunction("activate", &Camera::activate)
        .addFunction("setOrtho", &Camera::setOrtho)
        .addFunction("setPerspective", &Camera::setPerspective)
        .addProperty("autoResize",  &Camera::isAutoResize, &Camera::setAutoResize)
        .addProperty("nearClip",  &Camera::getNearClip, &Camera::setNearClip)
        .addProperty("farClip",  &Camera::getFarClip, &Camera::setFarClip)
        .addProperty("leftClip",  &Camera::getLeftClip, &Camera::setLeftClip)
        .addProperty("rightClip",  &Camera::getRightClip, &Camera::setRightClip)
        .addProperty("bottomClip",  &Camera::getBottomClip, &Camera::setBottomClip)
        .addProperty("topClip",  &Camera::getTopClip, &Camera::setTopClip)
        .addProperty("aspect",  &Camera::getAspect, &Camera::setAspect)
        .addProperty("yfov",  &Camera::getYFov, &Camera::setYFov)
        .addProperty("type",  &Camera::getType, &Camera::setType)
        .addFunction("setType", &Camera::setType)
        .addProperty("target",  &Camera::getTarget, (void(Camera::*)(Vector3))&Camera::setTarget)
        .addFunction("setTarget", (void(Camera::*)(const float, const float, const float))&Camera::setTarget)
        .addFunction("disableTarget", &Camera::disableTarget)
        .addFunction("isUsingTarget", &Camera::isUsingTarget)
        .addProperty("up",  &Camera::getUp, (void(Camera::*)(Vector3))&Camera::setUp)
        .addFunction("setUp", (void(Camera::*)(const float, const float, const float))&Camera::setUp)
        .addFunction("getUp", &Camera::getUp)
        .addFunction("getDirection", &Camera::getDirection)
        .addFunction("getRight", &Camera::getRight)
        .addFunction("getWorldTarget", &Camera::getWorldTarget)
        .addFunction("getWorldDirection", &Camera::getWorldDirection)
        .addFunction("getWorldUp", &Camera::getWorldUp)
        .addFunction("getWorldRight", &Camera::getWorldRight)
        .addFunction("getViewMatrix", &Camera::getViewMatrix)
        .addFunction("getProjectionMatrix", &Camera::getProjectionMatrix)
        .addFunction("getViewProjectionMatrix", &Camera::getViewProjectionMatrix)
        .addFunction("rotateView", &Camera::rotateView)
        .addFunction("rotatePosition", &Camera::rotatePosition)
        .addFunction("elevateView", &Camera::elevateView)
        .addFunction("elevatePosition", &Camera::elevatePosition)
        .addFunction("walkForward", &Camera::walkForward)
        .addFunction("zoom", &Camera::zoom)
        .addFunction("slide", &Camera::slide)
        .addFunction("slideForward", &Camera::slideForward)
        .addFunction("slideUp", &Camera::slideUp)
        .addProperty("renderToTexture", &Camera::isRenderToTexture, &Camera::setRenderToTexture)
        .addFunction("getFramebuffer", &Camera::getFramebuffer)
        .addFunction("setFramebufferSize", &Camera::setFramebufferSize)
        .addFunction("setFramebufferFilter", &Camera::setFramebufferFilter)
        .addProperty("transparentSort", &Camera::isTransparentSort, &Camera::setTransparentSort)
        .addProperty("depthTest", &Camera::isDepthTest, &Camera::setDepthTest)
        .addProperty("cullingMask", &getLuaCullingMask<Camera>, &setLuaCullingMask<Camera>)
        .addFunction("screenToRay", &Camera::screenToRay)
        .addFunction("getDistanceFromTarget", &Camera::getDistanceFromTarget)
        .addFunction("updateCamera", &Camera::updateCamera)
        .endClass();


    luabridge::getGlobalNamespace(L)
        .deriveClass<Light, GameObject>("Light")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Light::isEnabled, &Light::setEnabled)
        .addProperty("cullingMask", &getLuaCullingMask<Light>, &setLuaCullingMask<Light>)
        .addProperty("shading", &Light::getShading, &Light::setShading)
        .addProperty("type", &Light::getType, &Light::setType)
        .addFunction("setType", &Light::setType)
        .addProperty("direction", &Light::getDirection, (void(Light::*)(Vector3))&Light::setDirection)
        .addFunction("setDirection", (void(Light::*)(const float, const float, const float))&Light::setDirection)
        .addProperty("color", &Light::getColor, (void(Light::*)(Vector3))&Light::setColor)
        .addFunction("setColor", (void(Light::*)(const float, const float, const float))&Light::setColor)
        .addProperty("range", &Light::getRange, &Light::setRange)
        .addFunction("setRange", &Light::setRange)
        .addProperty("intensity", &Light::getIntensity, &Light::setIntensity)
        .addFunction("setIntensity", &Light::setIntensity)
        .addFunction("setConeAngle", &Light::setConeAngle)
        .addProperty("innerConeAngle", &Light::getInnerConeAngle, &Light::setInnerConeAngle)
        .addFunction("setInnerConeAngle", &Light::setInnerConeAngle)
        .addProperty("outerConeAngle", &Light::getOuterConeAngle, &Light::setOuterConeAngle)
        .addFunction("setOuterConeAngle", &Light::setOuterConeAngle)
        .addProperty("spotMask", &Light::getSpotMask)
        .addFunction("setSpotMask",
            luabridge::overload<const std::string&>(&Light::setSpotMask),
            luabridge::overload<const std::string&, TextureData>(&Light::setSpotMask))
        .addFunction("clearSpotMask", &Light::clearSpotMask)
        .addProperty("shadows", &Light::isShadows, &Light::setShadows)
        .addFunction("setShadows", &Light::setShadows)
        .addFunction("setShadowCameraNearFar", &Light::setShadowCameraNearFar)
        .addProperty("cameraNear", &Light::getCameraNear, &Light::setCameraNear)
        .addFunction("setCameraNear", &Light::setCameraNear)
        .addProperty("cameraFar", &Light::getCameraFar, &Light::setCameraFar)
        .addFunction("setCameraFar", &Light::setCameraFar)
        .addProperty("automaticShadowCamera", &Light::isAutomaticShadowCamera, &Light::setAutomaticShadowCamera)
        .addProperty("numCascades", &Light::getNumCascades, &Light::setNumCascades)
        .addFunction("setNumCascades", &Light::setNumCascades)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Light2D, GameObject>("Light2D")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Light2D::isEnabled, &Light2D::setEnabled)
        .addProperty("color", &Light2D::getColor, (void(Light2D::*)(Vector3))&Light2D::setColor)
        .addFunction("setColor", (void(Light2D::*)(const float, const float, const float))&Light2D::setColor)
        .addProperty("intensity", &Light2D::getIntensity, &Light2D::setIntensity)
        .addFunction("setIntensity", &Light2D::setIntensity)
        .addProperty("range", &Light2D::getRange, &Light2D::setRange)
        .addFunction("setRange", &Light2D::setRange)
        .addProperty("falloff", &Light2D::getFalloff, &Light2D::setFalloff)
        .addFunction("setFalloff", &Light2D::setFalloff)
        .addProperty("height", &Light2D::getHeight, &Light2D::setHeight)
        .addFunction("setHeight", &Light2D::setHeight)
        .addProperty("shadows", &Light2D::isShadows, &Light2D::setShadows)
        .addFunction("setShadows", &Light2D::setShadows)
        .addProperty("shadowBias", &Light2D::getShadowBias, &Light2D::setShadowBias)
        .addProperty("shadowSoftness", &Light2D::getShadowSoftness, &Light2D::setShadowSoftness)
        .addProperty("mapResolution", &Light2D::getMapResolution, &Light2D::setMapResolution)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowCaster2DShape")
        .addVariable("AUTO_QUAD", ShadowCaster2DShape::AUTO_QUAD)
        .addVariable("POLYGON", ShadowCaster2DShape::POLYGON)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<ShadowCaster2D, GameObject>("ShadowCaster2D")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("shape", &ShadowCaster2D::getShape, &ShadowCaster2D::setShape)
        .addFunction("addVertex", (void(ShadowCaster2D::*)(float, float))&ShadowCaster2D::addVertex)
        .addFunction("clearVertices", &ShadowCaster2D::clearVertices)
        .addFunction("getVertexCount", &ShadowCaster2D::getVertexCount)
        .addProperty("closed", &ShadowCaster2D::isClosed, &ShadowCaster2D::setClosed)
        .addProperty("enabled", &ShadowCaster2D::isEnabled, &ShadowCaster2D::setEnabled)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ReflectionProbeMode")
        .addVariable("STATIC", ReflectionProbeMode::STATIC)
        .addVariable("DYNAMIC", ReflectionProbeMode::DYNAMIC)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ReflectionProbeUpdateMode")
        .addVariable("ON_LOAD", ReflectionProbeUpdateMode::ON_LOAD)
        .addVariable("ON_MOVE", ReflectionProbeUpdateMode::ON_MOVE)
        .addVariable("INTERVAL", ReflectionProbeUpdateMode::INTERVAL)
        .addVariable("MANUAL", ReflectionProbeUpdateMode::MANUAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<ReflectionProbe, GameObject>("ReflectionProbe")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("mode", &ReflectionProbe::getMode, &ReflectionProbe::setMode)
        .addProperty("updateMode", &ReflectionProbe::getUpdateMode, &ReflectionProbe::setUpdateMode)
        .addProperty("boxOffset", &ReflectionProbe::getBoxOffset, (void(ReflectionProbe::*)(Vector3))&ReflectionProbe::setBoxOffset)
        .addFunction("setBoxOffset", (void(ReflectionProbe::*)(const float, const float, const float))&ReflectionProbe::setBoxOffset)
        .addProperty("boxSize", &ReflectionProbe::getBoxSize, (void(ReflectionProbe::*)(Vector3))&ReflectionProbe::setBoxSize)
        .addFunction("setBoxSize", (void(ReflectionProbe::*)(const float, const float, const float))&ReflectionProbe::setBoxSize)
        .addProperty("blendDistance", &ReflectionProbe::getBlendDistance, &ReflectionProbe::setBlendDistance)
        .addProperty("intensity", &ReflectionProbe::getIntensity, &ReflectionProbe::setIntensity)
        .addProperty("priority", &ReflectionProbe::getPriority, &ReflectionProbe::setPriority)
        .addProperty("resolution", &ReflectionProbe::getResolution, &ReflectionProbe::setResolution)
        .addFunction("setClipPlanes", &ReflectionProbe::setClipPlanes)
        .addProperty("nearClip", &ReflectionProbe::getNearClip, &ReflectionProbe::setNearClip)
        .addProperty("farClip", &ReflectionProbe::getFarClip, &ReflectionProbe::setFarClip)
        .addProperty("updateInterval", &ReflectionProbe::getUpdateInterval, &ReflectionProbe::setUpdateInterval)
        .addProperty("includeSky", &ReflectionProbe::isIncludeSky, &ReflectionProbe::setIncludeSky)
        .addProperty("cullingMask", &getLuaCullingMask<ReflectionProbe>, &setLuaCullingMask<ReflectionProbe>)
        .addFunction("setTextures", &ReflectionProbe::setTextures)
        .addFunction("setTexture", &ReflectionProbe::setTexture)
        .addFunction("refresh", &ReflectionProbe::refresh)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FluidOutput")
        .addVariable("SCREEN", FluidOutput::SCREEN)
        .addVariable("MATERIAL", FluidOutput::MATERIAL)
        .addVariable("TEXTURE", FluidOutput::TEXTURE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<FluidStats>("FluidStats")
        .addProperty("steps", &FluidStats::steps)
        .addProperty("time", &FluidStats::time)
        .addProperty("splats", &FluidStats::splats)
        .addProperty("simWidth", &FluidStats::simWidth)
        .addProperty("simHeight", &FluidStats::simHeight)
        .addProperty("dyeWidth", &FluidStats::dyeWidth)
        .addProperty("dyeHeight", &FluidStats::dyeHeight)
        .addProperty("gpuBytes", &FluidStats::gpuBytes)
        .addProperty("passesLastFrame", &FluidStats::passesLastFrame)
        .addProperty("seed", &FluidStats::seed)
        .addProperty("shadingActive", &FluidStats::shadingActive)
        .addProperty("bloomActive", &FluidStats::bloomActive)
        .addProperty("sunraysActive", &FluidStats::sunraysActive)
        .addProperty("manualFiltering", &FluidStats::manualFiltering)
        .addProperty("compactFormats", &FluidStats::compactFormats)
        .addProperty("outputWidth", &FluidStats::outputWidth)
        .addProperty("outputHeight", &FluidStats::outputHeight)
        .addProperty("targetFailures", &FluidStats::targetFailures)
        .addProperty("drawn", &FluidStats::drawn)
        .endClass();

    // Pavel Dobryakov's WebGL fluid on a GameObject (GameObject > Effects > Fluid Simulation)
    luabridge::getGlobalNamespace(L)
        .deriveClass<FluidSimulation, GameObject>("FluidSimulation")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &FluidSimulation::isEnabled, &FluidSimulation::setEnabled)
        .addProperty("output", &FluidSimulation::getOutput, &FluidSimulation::setOutput)
        .addProperty("interactive", &FluidSimulation::isInteractive, &FluidSimulation::setInteractive)
        .addProperty("aspectRatio", &FluidSimulation::getAspectRatio, &FluidSimulation::setAspectRatio)
        .addProperty("simResolution", &FluidSimulation::getSimResolution, &FluidSimulation::setSimResolution)
        .addProperty("dyeResolution", &FluidSimulation::getDyeResolution, &FluidSimulation::setDyeResolution)
        .addProperty("densityDissipation", &FluidSimulation::getDensityDissipation, &FluidSimulation::setDensityDissipation)
        .addProperty("velocityDissipation", &FluidSimulation::getVelocityDissipation, &FluidSimulation::setVelocityDissipation)
        .addProperty("pressure", &FluidSimulation::getPressure, &FluidSimulation::setPressure)
        .addProperty("pressureIterations", &FluidSimulation::getPressureIterations, &FluidSimulation::setPressureIterations)
        .addProperty("curl", &FluidSimulation::getCurl, &FluidSimulation::setCurl)
        .addProperty("gradientScale", &FluidSimulation::getGradientScale, &FluidSimulation::setGradientScale)
        .addProperty("simulationRate", &FluidSimulation::getSimulationRate, &FluidSimulation::setSimulationRate)
        .addProperty("maxSubsteps", &FluidSimulation::getMaxSubsteps, &FluidSimulation::setMaxSubsteps)
        .addProperty("timeScale", &FluidSimulation::getTimeScale, &FluidSimulation::setTimeScale)
        .addProperty("paused", &FluidSimulation::isPaused, &FluidSimulation::setPaused)
        .addProperty("splatRadius", &FluidSimulation::getSplatRadius, &FluidSimulation::setSplatRadius)
        .addProperty("splatForce", &FluidSimulation::getSplatForce, &FluidSimulation::setSplatForce)
        .addProperty("colorful", &FluidSimulation::isColorful, &FluidSimulation::setColorful)
        .addProperty("colorUpdateSpeed", &FluidSimulation::getColorUpdateSpeed, &FluidSimulation::setColorUpdateSpeed)
        .addProperty("splatsOnStart", &FluidSimulation::isSplatsOnStart, &FluidSimulation::setSplatsOnStart)
        .addProperty("randomSplatCount", &FluidSimulation::getRandomSplatCount, &FluidSimulation::setRandomSplatCount)
        .addProperty("seed", &FluidSimulation::getSeed, &FluidSimulation::setSeed)
        .addProperty("shading", &FluidSimulation::isShading, &FluidSimulation::setShading)
        .addProperty("backColor", &FluidSimulation::getBackColor, (void(FluidSimulation::*)(Vector3))&FluidSimulation::setBackColor)
        .addFunction("setBackColor", (void(FluidSimulation::*)(const float, const float, const float))&FluidSimulation::setBackColor)
        .addProperty("transparent", &FluidSimulation::isTransparent, &FluidSimulation::setTransparent)
        .addProperty("bloom", &FluidSimulation::isBloom, &FluidSimulation::setBloom)
        .addProperty("bloomIterations", &FluidSimulation::getBloomIterations, &FluidSimulation::setBloomIterations)
        .addProperty("bloomResolution", &FluidSimulation::getBloomResolution, &FluidSimulation::setBloomResolution)
        .addProperty("bloomIntensity", &FluidSimulation::getBloomIntensity, &FluidSimulation::setBloomIntensity)
        .addProperty("bloomThreshold", &FluidSimulation::getBloomThreshold, &FluidSimulation::setBloomThreshold)
        .addProperty("bloomSoftKnee", &FluidSimulation::getBloomSoftKnee, &FluidSimulation::setBloomSoftKnee)
        .addProperty("sunrays", &FluidSimulation::isSunrays, &FluidSimulation::setSunrays)
        .addProperty("sunraysResolution", &FluidSimulation::getSunraysResolution, &FluidSimulation::setSunraysResolution)
        .addProperty("sunraysWeight", &FluidSimulation::getSunraysWeight, &FluidSimulation::setSunraysWeight)
        // splat(x, y, dx, dy, r, g, b) or splat(position, velocity, color[, radius])
        .addFunction("splat",
            luabridge::overload<const float, const float, const float, const float, const float, const float, const float>(&FluidSimulation::splat),
            luabridge::overload<Vector2, Vector2, Vector3, float>(&FluidSimulation::splat),
            luabridge::overload<FluidSimulation*, Vector2, Vector2, Vector3>(+[](FluidSimulation* self, Vector2 position, Vector2 velocity, Vector3 color){
                self->splat(position, velocity, color);
            }))
        .addFunction("randomSplats",
            luabridge::overload<unsigned int>(&FluidSimulation::randomSplats),
            luabridge::overload<FluidSimulation*>(+[](FluidSimulation* self){ self->randomSplats(); }))
        .addFunction("reset", &FluidSimulation::reset)
        .addFunction("step",
            luabridge::overload<int>(&FluidSimulation::step),
            luabridge::overload<FluidSimulation*>(+[](FluidSimulation* self){ self->step(); }))
        .addFunction("applyPreset", &FluidSimulation::applyPreset)
        .addFunction("getPreset", &FluidSimulation::getPreset)
        .addProperty("preset", &FluidSimulation::getPreset)
        .addStaticFunction("getPresetIds", &FluidSimulation::getPresetIds)
        // the output texture (a copy of the handle: it shows the fluid while the entity lives)
        .addFunction("getTexture", +[](const FluidSimulation* self) -> Texture { return self->getTexture(); })
        .addFunction("getStats", &FluidSimulation::getStats)
        .addFunction("getFluidSimulationComponent", (FluidSimulationComponent&(FluidSimulation::*)() const)&FluidSimulation::getComponent)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Mesh, GameObject>("Mesh")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Mesh::isEnabled, &Mesh::setEnabled)
        .addFunction("setTexture", 
            luabridge::overload<const std::string&>(&Mesh::setTexture),
            luabridge::overload<const std::string&,TextureData>(&Mesh::setTexture),
            luabridge::overload<Framebuffer*>(&Mesh::setTexture))
        // the normal map (tangent-space, OpenGL green up; answers Light 2Ds with height > 0) of
        // submesh 0 or of a submesh; "" removes it
        .addFunction("setNormalTexture",
            luabridge::overload<const std::string&>(&Mesh::setNormalTexture),
            luabridge::overload<unsigned int, const std::string&>(&Mesh::setNormalTexture),
            luabridge::overload<const std::string&, TextureData>(&Mesh::setNormalTexture))
        .addFunction("getNormalTexture",
            luabridge::overload<>(&Mesh::getNormalTexture),
            luabridge::overload<unsigned int>(&Mesh::getNormalTexture))
        .addProperty("normalTexture", (std::string(Mesh::*)()const)&Mesh::getNormalTexture,
                     (void(Mesh::*)(const std::string&))&Mesh::setNormalTexture)
        .addProperty("color", &Mesh::getColor, (void(Mesh::*)(Vector4))&Mesh::setColor)
        .addFunction("setColor", 
            luabridge::overload<const float, const float, const float>(&Mesh::setColor),
            luabridge::overload<const float, const float, const float, const float>(&Mesh::setColor),
            luabridge::overload<Vector4>(&Mesh::setColor))
        .addProperty("alpha", &Mesh::getAlpha, &Mesh::setAlpha)
        // submesh 0's material, a copy whose shader values follow the shader the submesh draws with
        .addProperty("material", +[](const Mesh* self, lua_State* L) -> luabridge::LuaRef { return meshMaterial(self, 0, L); },
                     (void(Mesh::*)(const Material&))&Mesh::setMaterial)
        .addFunction("setMaterial", 
            luabridge::overload<const Material&>(&Mesh::setMaterial),
            luabridge::overload<unsigned int, const Material&>(&Mesh::setMaterial))
        .addFunction("getMaterial", 
            luabridge::overload<const Mesh*, lua_State*>(+[](const Mesh* self, lua_State* L) -> luabridge::LuaRef {
                return meshMaterial(self, 0, L);
            }),
            luabridge::overload<const Mesh*, unsigned int, lua_State*>(+[](const Mesh* self, unsigned int submesh, lua_State* L) -> luabridge::LuaRef {
                return meshMaterial(self, submesh, L);
            }))
        .addProperty("primitiveType", (PrimitiveType(Mesh::*)()const)&Mesh::getPrimitiveType, (void(Mesh::*)(PrimitiveType))&Mesh::setPrimitiveType)
        .addFunction("setPrimitiveType", 
            luabridge::overload<PrimitiveType>(&Mesh::setPrimitiveType),
            luabridge::overload<unsigned int, PrimitiveType>(&Mesh::setPrimitiveType))
        .addFunction("getPrimitiveType", 
            luabridge::overload<>(&Mesh::getPrimitiveType),
            luabridge::overload<unsigned int>(&Mesh::getPrimitiveType))
        .addProperty("faceCulling", (bool(Mesh::*)()const)&Mesh::isFaceCulling, (void(Mesh::*)(bool))&Mesh::setFaceCulling)
        .addFunction("setFaceCulling", 
            luabridge::overload<bool>(&Mesh::setFaceCulling),
            luabridge::overload<unsigned int, bool>(&Mesh::setFaceCulling))
        .addFunction("isFaceCulling", 
            luabridge::overload<>(&Mesh::isFaceCulling),
            luabridge::overload<unsigned int>(&Mesh::isFaceCulling))
        .addProperty("castShadowsWithTexture", (bool(Mesh::*)()const)&Mesh::isCastShadowsWithTexture, (void(Mesh::*)(bool))&Mesh::setCastShadowsWithTexture)
        .addFunction("setCastShadowsWithTexture", 
            luabridge::overload<bool>(&Mesh::setCastShadowsWithTexture),
            luabridge::overload<unsigned int, bool>(&Mesh::setCastShadowsWithTexture))
        .addFunction("isCastShadowsWithTexture", 
            luabridge::overload<>(&Mesh::isCastShadowsWithTexture),
            luabridge::overload<unsigned int>(&Mesh::isCastShadowsWithTexture))
        .addProperty("cullingMode", &Mesh::getCullingMode, &Mesh::setCullingMode)
        .addProperty("windingOrder", &Mesh::getWindingOrder, &Mesh::setWindingOrder)
        .addFunction("getAABB", &Mesh::getAABB)
        .addFunction("getVerticesAABB", &Mesh::getVerticesAABB)
        .addFunction("getWorldAABB", &Mesh::getWorldAABB)
        .addFunction("getNumSubmeshes", &Mesh::getNumSubmeshes)
        .addProperty("receiveLights", &Mesh::isReceiveLights, &Mesh::setReceiveLights)
        .addProperty("receiveIBL", &Mesh::isReceiveIBL, &Mesh::setReceiveIBL)
        .addProperty("castShadows", &Mesh::isCastShadows, &Mesh::setCastShadows)
        .addProperty("receiveShadows", &Mesh::isReceiveShadows, &Mesh::setReceiveShadows)
        .addProperty("lodEnabled", &Mesh::isLodEnabled, &Mesh::setLodEnabled)
        .addProperty("lodBias", &Mesh::getLodBias, &Mesh::setLodBias)
        .addProperty("renderInReflectionProbes", &Mesh::isRenderInReflectionProbes, &Mesh::setRenderInReflectionProbes)
        .addProperty("shadowsBillboard", &Mesh::isShadowsBillboard, &Mesh::setShadowsBillboard)
        .addProperty("transparent", &Mesh::isTransparent, &Mesh::setTransparent)
        .addProperty("autoTransparency", &Mesh::isAutoTransparency, &Mesh::setAutoTransparency)
        .addProperty("customShader", &Mesh::getCustomShader, &Mesh::setCustomShader)
        .addProperty("customDepthShader", &Mesh::getCustomDepthShader, &Mesh::setCustomDepthShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&Mesh::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&Mesh::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&Mesh::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Mesh::setShaderUniform))
        .addFunction("getShaderUniform", &Mesh::getShaderUniform)
        .addFunction("removeShaderUniform", &Mesh::removeShaderUniform)
        .addFunction("setAsMirror",
            luabridge::overload<>(&Mesh::setAsMirror),
            luabridge::overload<Vector3>(&Mesh::setAsMirror))
        .addFunction("removeMirror", &Mesh::removeMirror)
        .addFunction("isMirror", &Mesh::isMirror)
        .addFunction("createInstancedMesh", &Mesh::createInstancedMesh)
        .addFunction("removeInstancedMesh", &Mesh::removeInstancedMesh)
        .addFunction("hasInstancedMesh", &Mesh::hasInstancedMesh)
        .addFunction("setInstancedBillboard", (void(Mesh::*)(bool, bool))&Mesh::setInstancedBillboard)
        .addProperty("instancedBillboard", &Mesh::isInstancedBillboard, (void(Mesh::*)(bool))&Mesh::setInstancedBillboard)
        .addProperty("instancedCylindricalBillboard", &Mesh::isInstancedCylindricalBillboard, &Mesh::setInstancedCylindricalBillboard)
        .addProperty("maxInstances", &Mesh::getMaxInstances, &Mesh::setMaxInstances)
        .addProperty("distanceFade", &Mesh::isDistanceFade, &Mesh::setDistanceFade)
        .addFunction("setFadeRange", &Mesh::setFadeRange)
        .addProperty("fadeStart", &Mesh::getFadeStart, &Mesh::setFadeStart)
        .addProperty("fadeEnd", &Mesh::getFadeEnd, &Mesh::setFadeEnd)
        .addProperty("cullDistance", &Mesh::getCullDistance, &Mesh::setCullDistance)
        .addProperty("cullInstances", &Mesh::isCullInstances, &Mesh::setCullInstances)
        .addFunction("addInstance",
            luabridge::overload<InstanceData>(&Mesh::addInstance),
            luabridge::overload<Vector3>(&Mesh::addInstance),
            luabridge::overload<float, float, float>(&Mesh::addInstance),
            luabridge::overload<Vector3, Quaternion, Vector3>(&Mesh::addInstance),
            luabridge::overload<Vector3, Quaternion, Vector3, Vector4>(&Mesh::addInstance),
            luabridge::overload<Vector3, Quaternion, Vector3, Vector4, Rect>(&Mesh::addInstance))
        .addFunction("getInstance", &Mesh::getInstance)
        .addFunction("updateInstance",
            luabridge::overload<size_t, InstanceData>(&Mesh::updateInstance),
            luabridge::overload<size_t, Vector3>(&Mesh::updateInstance),
            luabridge::overload<size_t, float, float, float>(&Mesh::updateInstance),
            luabridge::overload<size_t, Vector3, Quaternion, Vector3>(&Mesh::updateInstance),
            luabridge::overload<size_t, Vector3, Quaternion, Vector3, Vector4>(&Mesh::updateInstance),
            luabridge::overload<size_t, Vector3, Quaternion, Vector3, Vector4, Rect>(&Mesh::updateInstance))
        .addFunction("removeInstance", &Mesh::removeInstance)
        .addFunction("isInstanceVisible", &Mesh::isInstanceVisible)
        .addFunction("setInstanceVisible", &Mesh::setInstanceVisible)
        .addFunction("updateInstances", &Mesh::updateInstances)
        .addFunction("getNumInstances", &Mesh::getNumInstances)
        .addFunction("clearInstances", &Mesh::clearInstances)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Primitive, Mesh>("Primitive")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("createPlane", 
            luabridge::overload<float, float>(&Primitive::createPlane),
            luabridge::overload<float, float, unsigned int>(&Primitive::createPlane))
        .addFunction("createGrid",
            luabridge::overload<float, float, unsigned int>(&Primitive::createGrid),
            luabridge::overload<float, float, unsigned int, unsigned int>(&Primitive::createGrid))
        .addFunction("createWall", 
            luabridge::overload<float, float>(&Primitive::createWall),
            luabridge::overload<float, float, unsigned int>(&Primitive::createWall))
        .addFunction("createBox", 
            luabridge::overload<float, float, float>(&Primitive::createBox),
            luabridge::overload<float, float, float, unsigned int>(&Primitive::createBox))
        .addFunction("createSphere", 
            luabridge::overload<float>(&Primitive::createSphere),
            luabridge::overload<float, unsigned int, unsigned int>(&Primitive::createSphere))
        .addFunction("createCylinder", 
            luabridge::overload<float, float>(&Primitive::createCylinder),
            luabridge::overload<float, float, float>(&Primitive::createCylinder),
            luabridge::overload<float, float, unsigned int, unsigned int>(&Primitive::createCylinder),
            luabridge::overload<float, float, float, unsigned int, unsigned int>(&Primitive::createCylinder))
        .addFunction("createCapsule", 
            luabridge::overload<float, float>(&Primitive::createCapsule),
            luabridge::overload<float, float, float>(&Primitive::createCapsule),
            luabridge::overload<float, float, unsigned int, unsigned int>(&Primitive::createCapsule),
            luabridge::overload<float, float, float, unsigned int, unsigned int>(&Primitive::createCapsule))
        .addFunction("createTorus", 
            luabridge::overload<float, float>(&Primitive::createTorus),
            luabridge::overload<float, float, unsigned int, unsigned int>(&Primitive::createTorus))
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Mirror, Primitive>("Mirror")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("normal", &Mirror::getNormal, (void(Mirror::*)(Vector3))&Mirror::setNormal)
        .addFunction("setNormal", (void(Mirror::*)(const float, const float, const float))&Mirror::setNormal)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Lattice, GameObject>("Lattice")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Lattice::isEnabled, &Lattice::setEnabled)
        .addFunction("getLatticeComponent", &Lattice::getLatticeComponent)
        .addFunction("setResolution", &Lattice::setResolution)
        .addFunction("getResolution", &Lattice::getResolution)
        .addFunction("getResolutionX", &Lattice::getResolutionX)
        .addFunction("getResolutionY", &Lattice::getResolutionY)
        .addFunction("getResolutionZ", &Lattice::getResolutionZ)
        .addFunction("getPointCount", &Lattice::getPointCount)
        .addFunction("getPointIndex", &Lattice::getPointIndex)
        .addFunction("getOffset", &Lattice::getOffset)
        .addFunction("setOffset", &Lattice::setOffset)
        .addFunction("getOffsetAt", &Lattice::getOffsetAt)
        .addFunction("setOffsetAt", &Lattice::setOffsetAt)
        .addFunction("getRestPosition", &Lattice::getRestPosition)
        .addFunction("getPointPosition", &Lattice::getPointPosition)
        .addFunction("setPointPosition", &Lattice::setPointPosition)
        .addFunction("getPointWorldPosition", &Lattice::getPointWorldPosition)
        .addFunction("setPointWorldPosition", &Lattice::setPointWorldPosition)
        .addFunction("resetOffsets", &Lattice::resetOffsets)
        .addProperty("interpolation", &Lattice::getInterpolation, &Lattice::setInterpolation)
        .addFunction("setInterpolation", &Lattice::setInterpolation)
        .addProperty("outside", &Lattice::getOutside, &Lattice::setOutside)
        .addFunction("setOutside", &Lattice::setOutside)
        .addProperty("fadeDistance", &Lattice::getFadeDistance, &Lattice::setFadeDistance)
        .addFunction("setFadeDistance", &Lattice::setFadeDistance)
        .addFunction("applyPreset",
            luabridge::overload<Lattice*, const std::string&, const std::string&, float>(+[](Lattice* self, const std::string& name, const std::string& axis, float amount) -> bool {
                return self->applyPreset(name, axis, amount);
            }),
            luabridge::overload<const std::string&, const std::string&, float, const std::string&>(&Lattice::applyPreset))
        .addFunction("savePose", &Lattice::savePose)
        .addFunction("hasPose", &Lattice::hasPose)
        .addFunction("removePose", &Lattice::removePose)
        .addFunction("applyPose",
            luabridge::overload<Lattice*, const std::string&>(+[](Lattice* self, const std::string& name) -> bool {
                return self->applyPose(name);
            }),
            luabridge::overload<const std::string&, float>(&Lattice::applyPose))
        .addFunction("tweenToPose",
            luabridge::overload<Lattice*, const std::string&, float>(+[](Lattice* self, const std::string& name, float duration) -> bool {
                return self->tweenToPose(name, duration);
            }),
            luabridge::overload<const std::string&, float, EaseType>(&Lattice::tweenToPose))
        .addFunction("tweenToRest",
            luabridge::overload<Lattice*, float>(+[](Lattice* self, float duration) {
                self->tweenToRest(duration);
            }),
            luabridge::overload<float, EaseType>(&Lattice::tweenToRest))
        .addFunction("isTweening", &Lattice::isTweening)
        .addFunction("stopTween", &Lattice::stopTween)
        .addProperty("jiggle", &Lattice::isJiggle, &Lattice::setJiggle)
        .addFunction("setJiggle", &Lattice::setJiggle)
        .addProperty("jiggleStiffness", &Lattice::getJiggleStiffness, &Lattice::setJiggleStiffness)
        .addFunction("setJiggleStiffness", &Lattice::setJiggleStiffness)
        .addProperty("jiggleDamping", &Lattice::getJiggleDamping, &Lattice::setJiggleDamping)
        .addFunction("setJiggleDamping", &Lattice::setJiggleDamping)
        .addProperty("jiggleInertia", &Lattice::getJiggleInertia, &Lattice::setJiggleInertia)
        .addFunction("setJiggleInertia", &Lattice::setJiggleInertia)
        .addProperty("jiggleAnchor", &Lattice::getJiggleAnchor, &Lattice::setJiggleAnchor)
        .addFunction("setJiggleAnchor", &Lattice::setJiggleAnchor)
        .addProperty("jiggleMaxOffset", &Lattice::getJiggleMaxOffset, &Lattice::setJiggleMaxOffset)
        .addFunction("setJiggleMaxOffset", &Lattice::setJiggleMaxOffset)
        .addFunction("isJiggleSleeping", &Lattice::isJiggleSleeping)
        .addFunction("getJiggleOffset", &Lattice::getJiggleOffset)
        .addFunction("resetJiggle", &Lattice::resetJiggle)
        .addFunction("poke", &Lattice::poke)
        .addFunction("pokeAt", &Lattice::pokeAt)
        .addFunction("deformPoint", &Lattice::deformPoint)
        // a GameObject (any wrapper) or an entity
        .addFunction("bind",
            luabridge::overload<Lattice*, Entity>(+[](Lattice* self, Entity target) -> bool {
                return self->bind(target);
            }),
            luabridge::overload<Entity, float>(&Lattice::bind),
            luabridge::overload<Lattice*, EntityHandle*>(+[](Lattice* self, EntityHandle* target) -> bool {
                return self->bind(target);
            }),
            luabridge::overload<EntityHandle*, float>(&Lattice::bind))
        .addFunction("unbind",
            luabridge::overload<Entity>(&Lattice::unbind),
            luabridge::overload<EntityHandle*>(&Lattice::unbind))
        // every arity of addFollower(follower, rest [, applyPosition, applyRotation, applyScale,
        // weight]): LuaBridge picks an overload by its exact argument count
        .addFunction("addFollower",
            luabridge::overload<Lattice*, Entity, Entity>(+[](Lattice* self, Entity follower, Entity rest) -> bool {
                return self->addFollower(follower, rest);
            }),
            luabridge::overload<Lattice*, Entity, Entity, bool>(+[](Lattice* self, Entity follower, Entity rest, bool position) -> bool {
                return self->addFollower(follower, rest, position);
            }),
            luabridge::overload<Lattice*, Entity, Entity, bool, bool>(+[](Lattice* self, Entity follower, Entity rest, bool position, bool rotation) -> bool {
                return self->addFollower(follower, rest, position, rotation);
            }),
            luabridge::overload<Lattice*, Entity, Entity, bool, bool, bool>(+[](Lattice* self, Entity follower, Entity rest, bool position, bool rotation, bool scale) -> bool {
                return self->addFollower(follower, rest, position, rotation, scale);
            }),
            luabridge::overload<Entity, Entity, bool, bool, bool, float>(&Lattice::addFollower),
            luabridge::overload<Lattice*, EntityHandle*, EntityHandle*>(+[](Lattice* self, EntityHandle* follower, EntityHandle* rest) -> bool {
                return self->addFollower(follower, rest);
            }),
            luabridge::overload<Lattice*, EntityHandle*, EntityHandle*, bool>(+[](Lattice* self, EntityHandle* follower, EntityHandle* rest, bool position) -> bool {
                return self->addFollower(follower, rest, position);
            }),
            luabridge::overload<Lattice*, EntityHandle*, EntityHandle*, bool, bool>(+[](Lattice* self, EntityHandle* follower, EntityHandle* rest, bool position, bool rotation) -> bool {
                return self->addFollower(follower, rest, position, rotation);
            }),
            luabridge::overload<Lattice*, EntityHandle*, EntityHandle*, bool, bool, bool>(+[](Lattice* self, EntityHandle* follower, EntityHandle* rest, bool position, bool rotation, bool scale) -> bool {
                return self->addFollower(follower, rest, position, rotation, scale);
            }),
            luabridge::overload<EntityHandle*, EntityHandle*, bool, bool, bool, float>(&Lattice::addFollower))
        .addFunction("removeFollower",
            luabridge::overload<Entity>(&Lattice::removeFollower),
            luabridge::overload<EntityHandle*>(&Lattice::removeFollower))
        .addFunction("getModifierComponent", &Lattice::getModifierComponent)
        .addFunction("getFollowerComponent", &Lattice::getFollowerComponent)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TerrainSurfaceLayer>("TerrainSurfaceLayer")
        .addConstructor <void (*) (void)> ()
        .addProperty("pbr", &TerrainSurfaceLayer::pbr, &TerrainSurfaceLayer::pbr)
        .addProperty("colorTexture", &TerrainSurfaceLayer::colorTexture, &TerrainSurfaceLayer::colorTexture)
        .addProperty("normalTexture", &TerrainSurfaceLayer::normalTexture, &TerrainSurfaceLayer::normalTexture)
        .addProperty("roughnessTexture", &TerrainSurfaceLayer::roughnessTexture, &TerrainSurfaceLayer::roughnessTexture)
        .addProperty("metallicTexture", &TerrainSurfaceLayer::metallicTexture, &TerrainSurfaceLayer::metallicTexture)
        .addProperty("occlusionTexture", &TerrainSurfaceLayer::occlusionTexture, &TerrainSurfaceLayer::occlusionTexture)
        .addProperty("heightTexture", &TerrainSurfaceLayer::heightTexture, &TerrainSurfaceLayer::heightTexture)
        .addProperty("colorFactor", &TerrainSurfaceLayer::colorFactor, &TerrainSurfaceLayer::colorFactor)
        .addProperty("normalStrength", &TerrainSurfaceLayer::normalStrength, &TerrainSurfaceLayer::normalStrength)
        .addProperty("roughnessFactor", &TerrainSurfaceLayer::roughnessFactor, &TerrainSurfaceLayer::roughnessFactor)
        .addProperty("metallicFactor", &TerrainSurfaceLayer::metallicFactor, &TerrainSurfaceLayer::metallicFactor)
        .addProperty("occlusionStrength", &TerrainSurfaceLayer::occlusionStrength, &TerrainSurfaceLayer::occlusionStrength)
        .addProperty("uvScale", &TerrainSurfaceLayer::uvScale, &TerrainSurfaceLayer::uvScale)
        .addProperty("uvOffset", &TerrainSurfaceLayer::uvOffset, &TerrainSurfaceLayer::uvOffset)
        .addProperty("stochasticTiling", &TerrainSurfaceLayer::stochasticTiling, &TerrainSurfaceLayer::stochasticTiling)
        .addProperty("stochasticCellSize", &TerrainSurfaceLayer::stochasticCellSize, &TerrainSurfaceLayer::stochasticCellSize)
        .addProperty("stochasticBlendSharpness", &TerrainSurfaceLayer::stochasticBlendSharpness, &TerrainSurfaceLayer::stochasticBlendSharpness)
        .addProperty("stochasticContrast", &TerrainSurfaceLayer::stochasticContrast, &TerrainSurfaceLayer::stochasticContrast)
        .addProperty("stochasticRotation", &TerrainSurfaceLayer::stochasticRotation, &TerrainSurfaceLayer::stochasticRotation)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Terrain, Mesh>("Terrain")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Terrain::isEnabled, &Terrain::setEnabled)
        .addFunction("setHeightMap", (void(Terrain::*)(const std::string&))&Terrain::setHeightMap)
        .addFunction("setBlendMap", (void(Terrain::*)(const std::string&))&Terrain::setBlendMap)
        .addFunction("setBlendMapIndex", (void(Terrain::*)(unsigned int, const std::string&))&Terrain::setBlendMap)
        .addFunction("setTextureLayer", &Terrain::setTextureLayer)
        .addFunction("setTextureDetailRed", (void(Terrain::*)(const std::string&))&Terrain::setTextureDetailRed)
        .addFunction("setTextureDetailGreen", (void(Terrain::*)(const std::string&))&Terrain::setTextureDetailGreen)
        .addFunction("setTextureDetailBlue", (void(Terrain::*)(const std::string&))&Terrain::setTextureDetailBlue)
        .addFunction("setSurfaceLayer", &Terrain::setSurfaceLayer)
        .addFunction("getSurfaceLayer", &Terrain::getSurfaceLayer)
        .addFunction("setLayerFromMaterial", &Terrain::setLayerFromMaterial)
        .addFunction("removeSurfaceLayer", &Terrain::removeSurfaceLayer)
        .addFunction("getNumLayers", &Terrain::getNumLayers)
        .addProperty("size", &Terrain::getSize, &Terrain::setSize)
        .addProperty("maxHeight", &Terrain::getMaxHeight, &Terrain::setMaxHeight)
        .addProperty("resolution", &Terrain::getResolution, &Terrain::setResolution)
        .addProperty("textureBaseTiles", &Terrain::getTextureBaseTiles, &Terrain::setTextureBaseTiles)
        .addProperty("textureDetailTiles", &Terrain::getTextureDetailTiles, &Terrain::setTextureDetailTiles)
        .addProperty("rootGridSize", &Terrain::getRootGridSize, &Terrain::setRootGridSize)
        .addProperty("levels", &Terrain::getLevels, &Terrain::setLevels)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Bone, GameObject>("Bone")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Tilemap, Mesh>("Tilemap")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Tilemap::isEnabled, &Tilemap::setEnabled)
        .addFunction("findRectByString", &Tilemap::findRectByString)
        .addFunction("findTileByString", &Tilemap::findTileByString)
        .addProperty("textureScaleFactor", &Tilemap::getTextureScaleFactor, &Tilemap::setTextureScaleFactor)
        .addFunction("addRect", 
            luabridge::overload<int, const std::string&, const std::string&, TextureFilter, Rect>(&Tilemap::addRect),
            luabridge::overload<int, const std::string&, const std::string&, Rect>(&Tilemap::addRect),
            luabridge::overload<int, const std::string&, Rect>(&Tilemap::addRect),
            luabridge::overload<const std::string&, float, float, float, float>(&Tilemap::addRect),
            luabridge::overload<float, float, float, float>(&Tilemap::addRect),
            luabridge::overload<Rect>(&Tilemap::addRect))
        .addFunction("removeRect", 
            luabridge::overload<int>(&Tilemap::removeRect),
            luabridge::overload<const std::string&>(&Tilemap::removeRect))
        .addFunction("clearRects", &Tilemap::clearRects)
        .addFunction("getRect", 
            luabridge::overload<int>(&Tilemap::getRect),
            luabridge::overload<const std::string&>(&Tilemap::getRect))
        .addFunction("addTile", 
            luabridge::overload<int, const std::string&, int, Vector2, float, float>(&Tilemap::addTile),
            luabridge::overload<const std::string&, int, Vector2, float, float>(&Tilemap::addTile),
            luabridge::overload<int, Vector2, float, float>(&Tilemap::addTile),
            luabridge::overload<const std::string&, const std::string&, Vector2, float, float>(&Tilemap::addTile),
            luabridge::overload<const std::string&, Vector2, float, float>(&Tilemap::addTile))
        .addFunction("removeTile", 
            luabridge::overload<int>(&Tilemap::removeTile),
            luabridge::overload<const std::string&>(&Tilemap::removeTile))
        .addFunction("clearTiles", &Tilemap::clearTiles)
        .addFunction("getTile", 
            luabridge::overload<int>(&Tilemap::getTile),
            luabridge::overload<const std::string&>(&Tilemap::getTile))
        .addProperty("reserveTiles", &Tilemap::getReserveTiles, &Tilemap::setReserveTiles)
        .addFunction("getWidth", &Tilemap::getWidth)
        .addFunction("getHeight", &Tilemap::getHeight)
        .addFunction("clearAll", &Tilemap::clearAll)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Model, Mesh>("Model")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Model::isEnabled, &Model::setEnabled)
        .addFunction("loadOBJ", &Model::loadOBJ)
        .addFunction("loadGLTF", &Model::loadGLTF)
        .addFunction("loadModel", &Model::loadModel)
        .addFunction("getAnimation", &Model::getAnimation)
        .addFunction("findAnimation", &Model::findAnimation)
        .addFunction("playAnimation",
            luabridge::overload<int>(&Model::playAnimation),
            luabridge::overload<int, float>(&Model::playAnimation),
            luabridge::overload<const std::string&>(&Model::playAnimation),
            luabridge::overload<const std::string&, float>(&Model::playAnimation))
        .addFunction("stopAnimations", &Model::stopAnimations)
        .addFunction("getBone",
            luabridge::overload<int>(&Model::getBone),
            luabridge::overload<const std::string&>(&Model::getBone))
        .addFunction("getMorphWeight", 
            luabridge::overload<int>(&Model::getMorphWeight),
            luabridge::overload<const std::string&>(&Model::getMorphWeight))
        .addFunction("setMorphWeight", 
            luabridge::overload<int, float>(&Model::setMorphWeight),
            luabridge::overload<const std::string&, float>(&Model::setMorphWeight))
        .addFunction("resetToBindPose", &Model::resetToBindPose)
        .addFunction("getPoseAnimator", &Model::getPoseAnimator)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<MeshPolygon, Mesh>("MeshPolygon")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("addVertex", 
            luabridge::overload<Vector3>(&MeshPolygon::addVertex),
            luabridge::overload<float, float>(&MeshPolygon::addVertex))
        .addProperty("width", [] (MeshPolygon* self) -> int { return self->getWidth(); })
        .addProperty("height", [] (MeshPolygon* self) -> int { return self->getHeight(); })
        .addProperty("flipY", &MeshPolygon::isFlipY, &MeshPolygon::setFlipY)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Points, GameObject>("Points")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Points::isEnabled, &Points::setEnabled)
        .addProperty("maxPoints", &Points::getMaxPoints, &Points::setMaxPoints)
        .addFunction("addPoint", 
            luabridge::overload<PointData>(&Points::addPoint),
            luabridge::overload<Vector3>(&Points::addPoint),
            luabridge::overload<float, float, float>(&Points::addPoint),
            luabridge::overload<Vector3, Vector4>(&Points::addPoint),
            luabridge::overload<Vector3, Vector4, float>(&Points::addPoint),
            luabridge::overload<Vector3, Vector4, float, float>(&Points::addPoint),
            luabridge::overload<Vector3, Vector4, float, float, Rect>(&Points::addPoint))
        .addFunction("getPoint", &Points::getPoint)
        .addFunction("updatePoint", 
            luabridge::overload<size_t, PointData>(&Points::updatePoint),
            luabridge::overload<size_t, Vector3>(&Points::updatePoint),
            luabridge::overload<size_t, float, float, float>(&Points::updatePoint),
            luabridge::overload<size_t, Vector3, Vector4>(&Points::updatePoint),
            luabridge::overload<size_t, Vector3, Vector4, float>(&Points::updatePoint),
            luabridge::overload<size_t, Vector3, Vector4, float, float>(&Points::updatePoint),
            luabridge::overload<size_t, Vector3, Vector4, float, float, Rect>(&Points::updatePoint))
        .addFunction("removePoint", &Points::removePoint)
        .addFunction("isPointVisible", &Points::isPointVisible)
        .addFunction("setPointVisible", &Points::setPointVisible)
        .addFunction("updatePoints", &Points::updatePoints)
        .addFunction("getNumPoints", &Points::getNumPoints)
        .addFunction("clearPoints", &Points::clearPoints)
        .addFunction("addSpriteFrame", 
            luabridge::overload<int, const std::string&, Rect>(&Points::addSpriteFrame),
            luabridge::overload<const std::string&, float, float, float, float>(&Points::addSpriteFrame),
            luabridge::overload<float, float, float, float>(&Points::addSpriteFrame),
            luabridge::overload<Rect>(&Points::addSpriteFrame))
        .addFunction("removeSpriteFrame", 
            luabridge::overload<int>(&Points::removeSpriteFrame),
            luabridge::overload<const std::string&>(&Points::removeSpriteFrame))
        .addFunction("setTexture", 
            luabridge::overload<const std::string&>(&Points::setTexture),
            luabridge::overload<const std::string&,TextureData>(&Points::setTexture),
            luabridge::overload<Framebuffer*>(&Points::setTexture))
        .addProperty("transparent", &Points::isTransparent, &Points::setTransparent)
        .addProperty("autoTransparency", &Points::isAutoTransparency, &Points::setAutoTransparency)
        .addProperty("customShader", &Points::getCustomShader, &Points::setCustomShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&Points::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&Points::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&Points::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Points::setShaderUniform))
        .addFunction("getShaderUniform", &Points::getShaderUniform)
        .addFunction("removeShaderUniform", &Points::removeShaderUniform)
        // shader library: setShaderParam sets a @property (as setShaderUniform), getShaderParam
        // falls back to the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<const std::string&, const Vector4&>(&Points::setShaderUniform),
            luabridge::overload<Points*, const std::string&, const Vector3&>(+[](Points* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                self->setShaderUniform(name, LuaBinding::getShaderParamValue(componentShader(self, ShaderType::POINTS), name, value));
            }),
            luabridge::overload<const std::string&, const Vector2&>(&Points::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Points::setShaderUniform))
        .addFunction("getShaderParam", &Points::getShaderParam)
        .addFunction("removeShaderParam", &Points::removeShaderUniform)
        .addFunction("setShaderTexture", &Points::setShaderTexture)
        .addFunction("getShaderTexture", &Points::getShaderTexture)
        .addFunction("setShaderFeature", &Points::setShaderFeature)
        .addFunction("isShaderFeatureEnabled", &Points::isShaderFeatureEnabled)
        .addProperty("blendMode", &Points::getBlendMode, &Points::setBlendMode)
        .addProperty("depthWrite", &Points::getDepthWrite, &Points::setDepthWrite)
        .addProperty("stencil", &Points::getStencil, &Points::setStencil)
        .addProperty("colorWrite", &Points::getColorWrite, &Points::setColorWrite)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<LineRenderer, GameObject>("LineRenderer")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &LineRenderer::isEnabled, &LineRenderer::setEnabled)
        .addProperty("maxLines", &LineRenderer::getMaxLines, &LineRenderer::setMaxLines)
        .addFunction("addLine", 
            luabridge::overload<Vector3, Vector3>(&LineRenderer::addLine),
            luabridge::overload<Vector3, Vector3, Vector3>(&LineRenderer::addLine),
            luabridge::overload<Vector3, Vector3, Vector4>(&LineRenderer::addLine),
            luabridge::overload<Vector3, Vector3, Vector4, Vector4>(&LineRenderer::addLine))
        .addFunction("getLine", &LineRenderer::getLine)
        .addFunction("updateLine", 
            luabridge::overload<size_t, Vector3, Vector3>(&LineRenderer::updateLine),
            luabridge::overload<size_t, Vector3, Vector3, Vector3>(&LineRenderer::updateLine),
            luabridge::overload<size_t, Vector3, Vector3, Vector4>(&LineRenderer::updateLine),
            luabridge::overload<size_t, Vector3, Vector3, Vector4, Vector4>(&LineRenderer::updateLine),
            luabridge::overload<size_t, Vector3>(&LineRenderer::updateLine),
            luabridge::overload<size_t, Vector4>(&LineRenderer::updateLine),
            luabridge::overload<size_t, Vector4, Vector4>(&LineRenderer::updateLine))
        .addFunction("removeLine", &LineRenderer::removeLine)
        .addFunction("updateLines", &LineRenderer::updateLines)
        .addFunction("getNumLines", &LineRenderer::getNumLines)
        .addFunction("clearLines", &LineRenderer::clearLines)
        .addProperty("customShader", &LineRenderer::getCustomShader, &LineRenderer::setCustomShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&LineRenderer::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&LineRenderer::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&LineRenderer::setShaderUniform),
            luabridge::overload<const std::string&, float>(&LineRenderer::setShaderUniform))
        .addFunction("getShaderUniform", &LineRenderer::getShaderUniform)
        .addFunction("removeShaderUniform", &LineRenderer::removeShaderUniform)
        // shader library: setShaderParam sets a @property (as setShaderUniform), getShaderParam
        // falls back to the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<const std::string&, const Vector4&>(&LineRenderer::setShaderUniform),
            luabridge::overload<LineRenderer*, const std::string&, const Vector3&>(+[](LineRenderer* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                self->setShaderUniform(name, LuaBinding::getShaderParamValue(componentShader(self, ShaderType::LINES), name, value));
            }),
            luabridge::overload<const std::string&, const Vector2&>(&LineRenderer::setShaderUniform),
            luabridge::overload<const std::string&, float>(&LineRenderer::setShaderUniform))
        .addFunction("getShaderParam", &LineRenderer::getShaderParam)
        .addFunction("removeShaderParam", &LineRenderer::removeShaderUniform)
        .addFunction("setShaderTexture", &LineRenderer::setShaderTexture)
        .addFunction("getShaderTexture", &LineRenderer::getShaderTexture)
        .addFunction("setShaderFeature", &LineRenderer::setShaderFeature)
        .addFunction("isShaderFeatureEnabled", &LineRenderer::isShaderFeatureEnabled)
        .addProperty("blendMode", &LineRenderer::getBlendMode, &LineRenderer::setBlendMode)
        .addProperty("depthWrite", &LineRenderer::getDepthWrite, &LineRenderer::setDepthWrite)
        .addProperty("stencil", &LineRenderer::getStencil, &LineRenderer::setStencil)
        .addProperty("colorWrite", &LineRenderer::getColorWrite, &LineRenderer::setColorWrite)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Sprite, Mesh>("Sprite")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Sprite::isEnabled, &Sprite::setEnabled)
        .addFunction("getShadowCaster2D", &Sprite::getShadowCaster2D)
        .addFunction("removeShadowCaster2D", &Sprite::removeShadowCaster2D)
        .addFunction("setSize", &Sprite::setSize)
        .addProperty("width", &Sprite::getWidth, &Sprite::setWidth)
        .addProperty("height", &Sprite::getHeight, &Sprite::setHeight)
        .addProperty("flipY", &Sprite::isFlipY, &Sprite::setFlipY)
        .addProperty("textureScaleFactor", &Sprite::getTextureScaleFactor, &Sprite::setTextureScaleFactor)
        .addProperty("textureRect", &Sprite::getTextureRect, (void(Sprite::*)(Rect))&Sprite::setTextureRect)
        .addFunction("setTextureRect",(void(Sprite::*)(float, float, float, float)) &Sprite::setTextureRect)
        .addProperty("pivotPreset", &Sprite::getPivotPreset, &Sprite::setPivotPreset)
        .addFunction("addFrame", 
            luabridge::overload<int, const std::string&, Rect>(&Sprite::addFrame),
            luabridge::overload<const std::string&, float, float, float, float>(&Sprite::addFrame),
            luabridge::overload<float, float, float, float>(&Sprite::addFrame),
            luabridge::overload<Rect>(&Sprite::addFrame))
        .addFunction("removeFrame", 
            luabridge::overload<int>(&Sprite::removeFrame),
            luabridge::overload<const std::string&>(&Sprite::removeFrame))
        .addFunction("setFrame", 
            luabridge::overload<int>(&Sprite::setFrame),
            luabridge::overload<const std::string&>(&Sprite::setFrame))
        .addFunction("startAnimation", 
            luabridge::overload<std::vector<int>, std::vector<int>, bool>(&Sprite::startAnimation),
            luabridge::overload<int, int, int, bool>(&Sprite::startAnimation),
            luabridge::overload<const std::string&, int, bool>(&Sprite::startAnimation))
        .addFunction("pauseAnimation", &Sprite::pauseAnimation)
        .addFunction("stopAnimation", &Sprite::stopAnimation)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<RectTransform, GameObject>("RectTransform")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &RectTransform::isEnabled, &RectTransform::setEnabled)
        .addFunction("setSize", &RectTransform::setSize)
        .addProperty("width", &RectTransform::getWidth, &RectTransform::setWidth)
        .addProperty("height", &RectTransform::getHeight, &RectTransform::setHeight)
        .addFunction("setAnchorPoints", &RectTransform::setAnchorPoints)
        .addProperty("anchorPointLeft", &RectTransform::getAnchorPointLeft, &RectTransform::setAnchorPointLeft)
        .addProperty("anchorPointTop", &RectTransform::getAnchorPointTop, &RectTransform::setAnchorPointTop)
        .addProperty("anchorPointRight", &RectTransform::getAnchorPointRight, &RectTransform::setAnchorPointRight)
        .addProperty("anchorPointBottom", &RectTransform::getAnchorPointBottom, &RectTransform::setAnchorPointBottom)
        .addFunction("setAnchorOffsets", &RectTransform::setAnchorOffsets)
        .addProperty("anchorOffsetLeft", &RectTransform::getAnchorOffsetLeft, &RectTransform::setAnchorOffsetLeft)
        .addProperty("anchorOffsetTop", &RectTransform::getAnchorOffsetTop, &RectTransform::setAnchorOffsetTop)
        .addProperty("anchorOffsetRight", &RectTransform::getAnchorOffsetRight, &RectTransform::setAnchorOffsetRight)
        .addProperty("anchorOffsetBottom", &RectTransform::getAnchorOffsetBottom, &RectTransform::setAnchorOffsetBottom)
        .addProperty("positionXOffset", &RectTransform::getPositionXOffset, &RectTransform::setPositionXOffset)
        .addProperty("positionYOffset", &RectTransform::getPositionYOffset, &RectTransform::setPositionYOffset)
        .addProperty("positionOffset", &RectTransform::getPositionOffset, &RectTransform::setPositionOffset)
        .addProperty("pivot", &RectTransform::getPivot, (void(RectTransform::*)(Vector2))&RectTransform::setPivot)
        .addFunction("setPivot", (void(RectTransform::*)(float, float))&RectTransform::setPivot)
        .addProperty("anchorPreset", &RectTransform::getAnchorPreset, &RectTransform::setAnchorPreset)
        .addProperty("usingAnchors", &RectTransform::isUsingAnchors, &RectTransform::setUsingAnchors)
        .addProperty("ignoreScissor", &RectTransform::isIgnoreScissor, &RectTransform::setIgnoreScissor)
        .addFunction("getRectTransformComponent", &RectTransform::getComponent<RectTransformComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<LayoutGroup, RectTransform>("LayoutGroup")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("type", &LayoutGroup::getType, &LayoutGroup::setType)
        .addProperty("useAllWrapSpace", &LayoutGroup::isUseAllWrapSpace, &LayoutGroup::setUseAllWrapSpace)
        .addProperty("wrapCellWidth", &LayoutGroup::getWrapCellWidth, &LayoutGroup::setWrapCellWidth)
        .addProperty("wrapCellHeight", &LayoutGroup::getWrapCellHeight, &LayoutGroup::setWrapCellHeight)
        .addProperty("contentWidth", &LayoutGroup::getContentWidth)
        .addProperty("contentHeight", &LayoutGroup::getContentHeight)
        .addFunction("resize", &LayoutGroup::resize)
        .addFunction("setBoxExpand", 
            luabridge::overload<size_t, bool>(&LayoutGroup::setBoxExpand),
            luabridge::overload<bool>(&LayoutGroup::setBoxExpand))
        .addFunction("isBoxExpand", &LayoutGroup::isBoxExpand)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Polygon, RectTransform>("Polygon")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("addVertex", 
            luabridge::overload<Vector3>(&Polygon::addVertex),
            luabridge::overload<float, float>(&Polygon::addVertex))
        .addProperty("color", &Polygon::getColor, (void(Polygon::*)(Vector4))&Polygon::setColor)
        .addFunction("setColor", 
            luabridge::overload<const float, const float, const float>(&Polygon::setColor),
            luabridge::overload<const float, const float, const float, const float>(&Polygon::setColor))
        .addProperty("alpha", &Polygon::getAlpha, &Polygon::setAlpha)
        .addFunction("setTexture", 
            luabridge::overload<const std::string&>(&Polygon::setTexture),
            luabridge::overload<const std::string&,TextureData>(&Polygon::setTexture),
            luabridge::overload<Framebuffer*>(&Polygon::setTexture))
        .addProperty("flipY", &Polygon::isFlipY, &Polygon::setFlipY)
        .addFunction("getAABB", &Polygon::getAABB)
        .addFunction("getWorldAABB", &Polygon::getWorldAABB)
        .addProperty("customShader", &Polygon::getCustomShader, &Polygon::setCustomShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&Polygon::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&Polygon::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&Polygon::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Polygon::setShaderUniform))
        .addFunction("getShaderUniform", &Polygon::getShaderUniform)
        .addFunction("removeShaderUniform", &Polygon::removeShaderUniform)
        // shader library: setShaderParam sets a @property (as setShaderUniform), getShaderParam
        // falls back to the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<const std::string&, const Vector4&>(&Polygon::setShaderUniform),
            luabridge::overload<Polygon*, const std::string&, const Vector3&>(+[](Polygon* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                self->setShaderUniform(name, LuaBinding::getShaderParamValue(componentShader(self, ShaderType::UI), name, value));
            }),
            luabridge::overload<const std::string&, const Vector2&>(&Polygon::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Polygon::setShaderUniform))
        .addFunction("getShaderParam", &Polygon::getShaderParam)
        .addFunction("removeShaderParam", &Polygon::removeShaderUniform)
        .addFunction("setShaderTexture", &Polygon::setShaderTexture)
        .addFunction("getShaderTexture", &Polygon::getShaderTexture)
        .addFunction("setShaderFeature", &Polygon::setShaderFeature)
        .addFunction("isShaderFeatureEnabled", &Polygon::isShaderFeatureEnabled)
        .addProperty("blendMode", &Polygon::getBlendMode, &Polygon::setBlendMode)
        .addProperty("depthWrite", &Polygon::getDepthWrite, &Polygon::setDepthWrite)
        .addProperty("stencil", &Polygon::getStencil, &Polygon::setStencil)
        .addProperty("colorWrite", &Polygon::getColorWrite, &Polygon::setColorWrite)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Text, RectTransform>("Text")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setFixedSize", &Text::setFixedSize)
        .addProperty("fixedWidth", &Text::isFixedWidth, &Text::setFixedWidth)
        .addProperty("fixedHeight", &Text::isFixedHeight, &Text::setFixedHeight)
        .addProperty("maxTextSize", &Text::getMaxTextSize, &Text::setMaxTextSize)
        .addProperty("text", &Text::getText, &Text::setText)
        .addProperty("font", luabridge::constOverload<>(&Text::getFont), luabridge::overload<const std::string&>(&Text::setFont))
        .addFunction("setFont",
            luabridge::overload<const std::string&>(&Text::setFont),
            luabridge::overload<unsigned int, const std::string&>(&Text::setFont))
        .addFunction("getFont",
            luabridge::constOverload<>(&Text::getFont),
            luabridge::constOverload<unsigned int>(&Text::getFont))
        .addProperty("fontSize", &Text::getFontSize, &Text::setFontSize)
        .addProperty("multiline", &Text::getMultiline, &Text::setMultiline)
        .addProperty("color", &Text::getColor, (void(Text::*)(Vector4))&Text::setColor)
        .addFunction("setColor", 
            luabridge::overload<const float, const float, const float>(&Text::setColor),
            luabridge::overload<const float, const float, const float, const float>(&Text::setColor))
        .addProperty("alpha", &Text::getAlpha, &Text::setAlpha)
        .addFunction("getAscent", &Text::getAscent)
        .addFunction("getDescent", &Text::getDescent)
        .addFunction("getLineGap", &Text::getLineGap)
        .addFunction("getLineHeight", &Text::getLineHeight)
        .addFunction("getNumChars", &Text::getNumChars)
        .addFunction("getCharPosition", &Text::getCharPosition)
        .addFunction("getCharWidth", &Text::getCharWidth)
        .addProperty("flipY", &Text::isFlipY, &Text::setFlipY)
        .addProperty("customShader", &Text::getCustomShader, &Text::setCustomShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&Text::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&Text::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&Text::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Text::setShaderUniform))
        .addFunction("getShaderUniform", &Text::getShaderUniform)
        .addFunction("removeShaderUniform", &Text::removeShaderUniform)
        // shader library: setShaderParam sets a @property (as setShaderUniform), getShaderParam
        // falls back to the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<const std::string&, const Vector4&>(&Text::setShaderUniform),
            luabridge::overload<Text*, const std::string&, const Vector3&>(+[](Text* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                self->setShaderUniform(name, LuaBinding::getShaderParamValue(componentShader(self, ShaderType::UI), name, value));
            }),
            luabridge::overload<const std::string&, const Vector2&>(&Text::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Text::setShaderUniform))
        .addFunction("getShaderParam", &Text::getShaderParam)
        .addFunction("removeShaderParam", &Text::removeShaderUniform)
        .addFunction("setShaderTexture", &Text::setShaderTexture)
        .addFunction("getShaderTexture", &Text::getShaderTexture)
        .addFunction("setShaderFeature", &Text::setShaderFeature)
        .addFunction("isShaderFeatureEnabled", &Text::isShaderFeatureEnabled)
        .addProperty("blendMode", &Text::getBlendMode, &Text::setBlendMode)
        .addProperty("depthWrite", &Text::getDepthWrite, &Text::setDepthWrite)
        .addProperty("stencil", &Text::getStencil, &Text::setStencil)
        .addProperty("colorWrite", &Text::getColorWrite, &Text::setColorWrite)
        .addProperty("pivotBaseline", &Text::isPivotBaseline, &Text::setPivotBaseline)
        .addProperty("pivotCentered", &Text::isPivotCentered, &Text::setPivotCentered)
        .addFunction("getAABB", &Text::getAABB)
        .addFunction("getWorldAABB", &Text::getWorldAABB)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Image, RectTransform>("Image")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setPatchMargin", 
            luabridge::overload<unsigned int>(&Image::setPatchMargin),
            luabridge::overload<unsigned int, unsigned int, unsigned int, unsigned int>(&Image::setPatchMargin))
        .addProperty("patchMarginBottom", &Image::getPatchMarginBottom, &Image::setPatchMarginBottom)
        .addProperty("patchMarginLeft", &Image::getPatchMarginLeft, &Image::setPatchMarginLeft)
        .addProperty("patchMarginRight", &Image::getPatchMarginRight, &Image::setPatchMarginRight)
        .addProperty("patchMarginTop", &Image::getPatchMarginTop, &Image::setPatchMarginTop)
        .addFunction("setTexture", 
            luabridge::overload<const std::string&>(&Image::setTexture),
            luabridge::overload<const std::string&,TextureData>(&Image::setTexture),
            luabridge::overload<Framebuffer*>(&Image::setTexture))
        .addProperty("textureScaleFactor", &Image::getTextureScaleFactor, &Image::setTextureScaleFactor)
        .addProperty("color", &Image::getColor, (void(Image::*)(Vector4))&Image::setColor)
        .addFunction("setColor", 
            luabridge::overload<const float, const float, const float>(&Image::setColor),
            luabridge::overload<const float, const float, const float, const float>(&Image::setColor))
        .addProperty("alpha", &Image::getAlpha, &Image::setAlpha)
        .addProperty("flipY", &Image::isFlipY, &Image::setFlipY)
        .addFunction("getAABB", &Image::getAABB)
        .addFunction("getWorldAABB", &Image::getWorldAABB)
        .addFunction("getUIComponent", &Image::getComponent<UIComponent>)
        .addProperty("customShader", &Image::getCustomShader, &Image::setCustomShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&Image::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&Image::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&Image::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Image::setShaderUniform))
        .addFunction("getShaderUniform", &Image::getShaderUniform)
        .addFunction("removeShaderUniform", &Image::removeShaderUniform)
        // shader library: setShaderParam sets a @property (as setShaderUniform), getShaderParam
        // falls back to the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<const std::string&, const Vector4&>(&Image::setShaderUniform),
            luabridge::overload<Image*, const std::string&, const Vector3&>(+[](Image* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                self->setShaderUniform(name, LuaBinding::getShaderParamValue(componentShader(self, ShaderType::UI), name, value));
            }),
            luabridge::overload<const std::string&, const Vector2&>(&Image::setShaderUniform),
            luabridge::overload<const std::string&, float>(&Image::setShaderUniform))
        .addFunction("getShaderParam", &Image::getShaderParam)
        .addFunction("removeShaderParam", &Image::removeShaderUniform)
        .addFunction("setShaderTexture", &Image::setShaderTexture)
        .addFunction("getShaderTexture", &Image::getShaderTexture)
        .addFunction("setShaderFeature", &Image::setShaderFeature)
        .addFunction("isShaderFeatureEnabled", &Image::isShaderFeatureEnabled)
        .addProperty("blendMode", &Image::getBlendMode, &Image::setBlendMode)
        .addProperty("depthWrite", &Image::getDepthWrite, &Image::setDepthWrite)
        .addProperty("stencil", &Image::getStencil, &Image::setStencil)
        .addProperty("colorWrite", &Image::getColorWrite, &Image::setColorWrite)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Button, Image>("Button")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("getLabelObject", &Button::getLabelObject)
        .addProperty("label", &Button::getLabel, &Button::setLabel)
        .addProperty("labelColor", &Button::getLabelColor, (void(Button::*)(Vector4))&Button::setLabelColor)
        .addFunction("setLabelColor", (void(Button::*)(const float, const float, const float, const float))&Button::setLabelColor)
        .addProperty("labelFont", &Button::getLabelFont, &Button::setLabelFont)
        .addProperty("labelFontSize", &Button::getLabelFontSize, &Button::setLabelFontSize)
        .addFunction("setTextureNormal", (void(Button::*)(const std::string&))&Button::setTextureNormal)
        .addFunction("setTextureHovered", (void(Button::*)(const std::string&))&Button::setTextureHovered)
        .addFunction("setTexturePressed", (void(Button::*)(const std::string&))&Button::setTexturePressed)
        .addFunction("setTextureDisabled", (void(Button::*)(const std::string&))&Button::setTextureDisabled)
        .addProperty("colorNormal", &Button::getColorNormal, (void(Button::*)(Vector4))&Button::setColorNormal)
        .addFunction("setColorNormal", 
            luabridge::overload<const float, const float, const float>(&Button::setColorNormal),
            luabridge::overload<const float, const float, const float, const float>(&Button::setColorNormal))
        .addProperty("colorHovered", &Button::getColorHovered, (void(Button::*)(Vector4))&Button::setColorHovered)
        .addFunction("setColorHovered", luabridge::overload<const float, const float, const float, const float>(&Button::setColorHovered),
            luabridge::overload<const float, const float, const float>(&Button::setColorHovered))
        .addProperty("colorPressed", &Button::getColorPressed, (void(Button::*)(Vector4))&Button::setColorPressed)
        .addFunction("setColorPressed", 
            luabridge::overload<const float, const float, const float>(&Button::setColorPressed),
            luabridge::overload<const float, const float, const float, const float>(&Button::setColorPressed))
        .addProperty("colorDisabled", &Button::getColorDisabled, (void(Button::*)(Vector4))&Button::setColorDisabled)
        .addFunction("setColorDisabled", 
            luabridge::overload<const float, const float, const float>(&Button::setColorDisabled),
            luabridge::overload<const float, const float, const float, const float>(&Button::setColorDisabled))
        .addProperty("disabled", &Button::getDisabled, &Button::setDisabled)
        .addFunction("getButtonComponent", &Button::getComponent<ButtonComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Panel, Image>("Panel")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("getHeaderImageObject", &Panel::getHeaderImageObject)
        .addFunction("getHeaderContainerObject", &Panel::getHeaderContainerObject)
        .addFunction("getHeaderTextObject", &Panel::getHeaderTextObject)
        .addProperty("title", &Panel::getTitle, &Panel::setTitle)
        .addProperty("titleAnchorPreset", &Panel::getTitleAnchorPreset, &Panel::setTitleAnchorPreset)
        .addProperty("titleColor", &Panel::getTitleColor, (void(Panel::*)(Vector4))&Panel::setTitleColor)
        .addFunction("setTitleColor", (void(Panel::*)(const float, const float, const float, const float))&Panel::setTitleColor)
        .addProperty("titleFont", &Panel::getTitleFont, &Panel::setTitleFont)
        .addProperty("titleFontSize", &Panel::getTitleFontSize, &Panel::setTitleFontSize)
        .addProperty("headerColor", &Panel::getHeaderColor, (void(Panel::*)(Vector4))&Panel::setHeaderColor)
        .addFunction("setHeaderColor", (void(Panel::*)(const float, const float, const float, const float))&Panel::setHeaderColor)
        .addFunction("setHeaderPatchMargin", &Panel::setHeaderPatchMargin)
        .addFunction("setHeaderTexture", &Panel::setHeaderTexture)
        .addFunction("setHeaderMargin", &Panel::setHeaderMargin)
        .addProperty("headerMarginLeft", &Panel::getHeaderMarginLeft, &Panel::setHeaderMarginLeft)
        .addProperty("headerMarginTop", &Panel::getHeaderMarginTop, &Panel::setHeaderMarginTop)
        .addProperty("headerMarginRight", &Panel::getHeaderMarginRight, &Panel::setHeaderMarginRight)
        .addProperty("headerMarginBottom", &Panel::getHeaderMarginBottom, &Panel::setHeaderMarginBottom)
        .addFunction("setMinSize", &Panel::setMinSize)
        .addProperty("minWidth", &Panel::getMinWidth, &Panel::setMinWidth)
        .addProperty("minHeight", &Panel::getMinHeight, &Panel::setMinHeight)
        .addProperty("resizeMargin", &Panel::getResizeMargin, &Panel::setResizeMargin)
        .addFunction("setWindowProperties", &Panel::setWindowProperties)
        .addProperty("canMove", &Panel::isCanMove, &Panel::setCanMove)
        .addProperty("canResize", &Panel::isCanResize, &Panel::setCanResize)
        .addProperty("canBringToFront", &Panel::isCanBringToFront, &Panel::setCanBringToFront)
        .addFunction("getPanelComponent", &Panel::getComponent<PanelComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Scrollbar, Image>("Scrollbar")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("getBarObject", &Scrollbar::getBarObject)
        .addProperty("type", &Scrollbar::getType, &Scrollbar::setType)
        .addProperty("barSize", &Scrollbar::getBarSize, &Scrollbar::setBarSize)
        .addProperty("step", &Scrollbar::getStep, &Scrollbar::setStep)
        .addFunction("setBarTexture", 
            luabridge::overload<const std::string&>(&Scrollbar::setBarTexture),
            luabridge::overload<Framebuffer*>(&Scrollbar::setBarTexture))
        .addProperty("barColor", &Scrollbar::getBarColor, (void(Scrollbar::*)(Vector4))&Scrollbar::setBarColor)
        .addFunction("setBarColor", 
            luabridge::overload<const float, const float, const float>(&Scrollbar::setBarColor),
            luabridge::overload<const float, const float, const float, const float>(&Scrollbar::setBarColor))
        .addProperty("barAlpha", &Scrollbar::getBarAlpha, &Scrollbar::setBarAlpha)
        .addFunction("setBarPatchMargin", 
            luabridge::overload<int>(&Scrollbar::setBarPatchMargin),
            luabridge::overload<int, int, int, int>(&Scrollbar::setBarPatchMargin))
        .addFunction("setBarMargin",
            luabridge::overload<int>(&Scrollbar::setBarMargin),
            luabridge::overload<int, int, int, int>(&Scrollbar::setBarMargin))
        .addFunction("getScrollbarComponent", &Scrollbar::getComponent<ScrollbarComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Progressbar, Image>("Progressbar")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("getFillObject", &Progressbar::getFillObject)
        .addProperty("type", &Progressbar::getType, &Progressbar::setType)
        .addProperty("value", &Progressbar::getValue, &Progressbar::setValue)
        .addFunction("setFillTexture", 
            luabridge::overload<const std::string&>(&Progressbar::setFillTexture),
            luabridge::overload<Framebuffer*>(&Progressbar::setFillTexture))
        .addProperty("fillColor", &Progressbar::getFillColor, (void(Progressbar::*)(Vector4))&Progressbar::setFillColor)
        .addFunction("setFillColor", 
            luabridge::overload<const float, const float, const float>(&Progressbar::setFillColor),
            luabridge::overload<const float, const float, const float, const float>(&Progressbar::setFillColor))
        .addProperty("fillAlpha", &Progressbar::getFillAlpha, &Progressbar::setFillAlpha)
        .addFunction("setFillPatchMargin", 
            luabridge::overload<int>(&Progressbar::setFillPatchMargin),
            luabridge::overload<int, int, int, int>(&Progressbar::setFillPatchMargin))
        .addFunction("setFillMargin",
            luabridge::overload<int>(&Progressbar::setFillMargin),
            luabridge::overload<int, int, int, int>(&Progressbar::setFillMargin))
        .addFunction("getProgressbarComponent", &Progressbar::getComponent<ProgressbarComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<InputField, Image>("InputField")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("getTextObject", &InputField::getTextObject)
        .addFunction("getSelectionObject", &InputField::getSelectionObject)
        .addProperty("disabled", &InputField::getDisabled, &InputField::setDisabled)
        .addProperty("text", &InputField::getText, &InputField::setText)
        .addProperty("textColor", &InputField::getTextColor, (void(InputField::*)(Vector4))&InputField::setTextColor)
        .addFunction("setTextColor", 
            luabridge::overload<const float, const float, const float>(&InputField::setTextColor),
            luabridge::overload<const float, const float, const float, const float>(&InputField::setTextColor))
        .addProperty("textFont", &InputField::getTextFont, &InputField::setTextFont)
        .addProperty("fontSize", &InputField::getFontSize, &InputField::setFontSize)
        .addProperty("maxTextSize", &InputField::getMaxTextSize, &InputField::setMaxTextSize)
        .addProperty("placeholder", &InputField::getPlaceholder, &InputField::setPlaceholder)
        .addProperty("placeholderColor", &InputField::getPlaceholderColor, (void(InputField::*)(Vector4))&InputField::setPlaceholderColor)
        .addFunction("setPlaceholderColor",
            luabridge::overload<const float, const float, const float>(&InputField::setPlaceholderColor),
            luabridge::overload<const float, const float, const float, const float>(&InputField::setPlaceholderColor))
        .addProperty("password", &InputField::getPassword, &InputField::setPassword)
        .addProperty("passwordChar", &InputField::getPasswordChar, &InputField::setPasswordChar)
        .addProperty("cursorIndex", &InputField::getCursorIndex, &InputField::setCursorIndex)
        .addFunction("getSelectionAnchor", &InputField::getSelectionAnchor)
        .addFunction("getSelectionFocus", &InputField::getSelectionFocus)
        .addFunction("setSelection", &InputField::setSelection)
        .addProperty("cursorColor", &InputField::getCursorColor, (void(InputField::*)(Vector4))&InputField::setCursorColor)
        .addFunction("setCursorColor", 
            luabridge::overload<const float, const float, const float>(&InputField::setCursorColor),
            luabridge::overload<const float, const float, const float, const float>(&InputField::setCursorColor))
        .addProperty("selectionColor", &InputField::getSelectionColor, (void(InputField::*)(Vector4))&InputField::setSelectionColor)
        .addFunction("setSelectionColor",
            luabridge::overload<const float, const float, const float>(&InputField::setSelectionColor),
            luabridge::overload<const float, const float, const float, const float>(&InputField::setSelectionColor))
        .addProperty("cursorBlink", &InputField::getCursorBlink, &InputField::setCursorBlink)
        .addProperty("cursorWidth", &InputField::getCursorWidth, &InputField::setCursorWidth)
        .addFunction("getInputFieldComponent", &InputField::getComponent<InputFieldComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<PoseAnimator, EntityHandle>("PoseAnimator")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addFunction("setAnimation", &PoseAnimator::setAnimation)
        .addFunction("getAnimation", &PoseAnimator::getAnimation)
        .addFunction("play", &PoseAnimator::play)
        .addFunction("pause", &PoseAnimator::pause)
        .addFunction("stop", &PoseAnimator::stop)
        .addFunction("isPlaying", &PoseAnimator::isPlaying)
        .addFunction("setTime", &PoseAnimator::setTime)
        .addFunction("getTime", &PoseAnimator::getTime)
        .addFunction("getLength", &PoseAnimator::getLength)
        .addFunction("setSpeed", &PoseAnimator::setSpeed)
        .addFunction("getSpeed", &PoseAnimator::getSpeed)
        .addFunction("setWeight", &PoseAnimator::setWeight)
        .addFunction("getWeight", &PoseAnimator::getWeight)
        .addProperty("autoplay", &PoseAnimator::isAutoplay, &PoseAnimator::setAutoplay)
        .addFunction("getPoseCount", &PoseAnimator::getPoseCount)
        .addFunction("getPoseName", &PoseAnimator::getPoseName)
        .addFunction("findPose", &PoseAnimator::findPose)
        .addFunction("goToPose",
            luabridge::overload<int>(&PoseAnimator::goToPose),
            luabridge::overload<int, float>(&PoseAnimator::goToPose),
            luabridge::overload<int, float, float>(&PoseAnimator::goToPose),
            luabridge::overload<int, float, float, float>(&PoseAnimator::goToPose),
            luabridge::overload<const std::string&>(&PoseAnimator::goToPose),
            luabridge::overload<const std::string&, float>(&PoseAnimator::goToPose),
            luabridge::overload<const std::string&, float, float>(&PoseAnimator::goToPose),
            luabridge::overload<const std::string&, float, float, float>(&PoseAnimator::goToPose))
        .addFunction("isLoaded", &PoseAnimator::isLoaded)
        // a function called when a one-shot animation ends
        .addProperty("onFinish", [] (PoseAnimator* self, lua_State* L) { return &self->getOnFinish(); }, [] (PoseAnimator* self, lua_State* L) { self->getOnFinish() = L; })
        .addFunction("getPoseAnimatorComponent", &PoseAnimator::getComponent<PoseAnimatorComponent>)
        .endClass();

    // a clip event as onEvent subscribers receive it (a copy)
    luabridge::getGlobalNamespace(L)
        .beginClass<AnimationEvent>("AnimationEvent")
        .addProperty("functionName", &AnimationEvent::functionName)
        .addProperty("state", &AnimationEvent::state)
        .addProperty("time", &AnimationEvent::time)
        // the event's args: a 1-based table of numbers, strings and booleans
        .addProperty("args", +[](const AnimationEvent* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef table = luabridge::newTable(L);
            for (size_t i = 0; i < self->args.size(); i++){
                const AnimationEventArg& arg = self->args[i];
                if (const double* number = std::get_if<double>(&arg)){
                    table[(int)i + 1] = *number;
                }else if (const std::string* text = std::get_if<std::string>(&arg)){
                    table[(int)i + 1] = *text;
                }else{
                    table[(int)i + 1] = std::get<bool>(arg);
                }
            }
            return table;
        })
        .addFunction("getArgCount", &AnimationEvent::getArgCount)
        .addFunction("getArgType", &AnimationEvent::getArgType)
        .addFunction("getNumberArg", &AnimationEvent::getNumberArg)
        .addFunction("getStringArg", &AnimationEvent::getStringArg)
        .addFunction("getBoolArg", &AnimationEvent::getBoolArg)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimationState>("AnimationState")
        .addFunction("isValid", &AnimationState::isValid)
        .addProperty("name", &AnimationState::getName)
        .addProperty("clip", &AnimationState::getClip)
        .addProperty("time", &AnimationState::getTime, &AnimationState::setTime)
        .addProperty("normalizedTime", &AnimationState::getNormalizedTime, &AnimationState::setNormalizedTime)
        .addProperty("speed", &AnimationState::getSpeed, &AnimationState::setSpeed)
        .addProperty("weight", &AnimationState::getWeight, &AnimationState::setWeight)
        .addProperty("length", &AnimationState::getLength)
        .addProperty("wrap", &AnimationState::getWrap, &AnimationState::setWrap)
        .addProperty("enabled", &AnimationState::isEnabled, &AnimationState::setEnabled)
        .endClass();

    // Unity's legacy Animation component: clips played with fades, blends and queues
    luabridge::getGlobalNamespace(L)
        .deriveClass<AnimationPlayer, EntityHandle>("AnimationPlayer")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addFunction("play",
            luabridge::overload<const std::string&>(&AnimationPlayer::play),
            luabridge::overload<const std::string&, float>(&AnimationPlayer::play))
        .addFunction("crossFade",
            luabridge::overload<const std::string&>(&AnimationPlayer::crossFade),
            luabridge::overload<const std::string&, float>(&AnimationPlayer::crossFade))
        .addFunction("blend",
            luabridge::overload<const std::string&, float>(&AnimationPlayer::blend),
            luabridge::overload<const std::string&, float, float>(&AnimationPlayer::blend))
        .addFunction("queue",
            luabridge::overload<const std::string&>(&AnimationPlayer::queue),
            luabridge::overload<const std::string&, float>(&AnimationPlayer::queue))
        .addFunction("stop",
            luabridge::overload<>(&AnimationPlayer::stop),
            luabridge::overload<const std::string&>(&AnimationPlayer::stop))
        .addFunction("rewind",
            luabridge::overload<>(&AnimationPlayer::rewind),
            luabridge::overload<const std::string&>(&AnimationPlayer::rewind))
        .addFunction("sample", &AnimationPlayer::sample)
        .addFunction("isPlaying",
            luabridge::constOverload<>(&AnimationPlayer::isPlaying),
            luabridge::constOverload<const std::string&>(&AnimationPlayer::isPlaying))
        .addFunction("getState", &AnimationPlayer::getState)
        .addFunction("addClip",
            luabridge::overload<const std::string&>(&AnimationPlayer::addClip),
            luabridge::overload<const std::string&, const std::string&>(&AnimationPlayer::addClip))
        .addFunction("removeClip", &AnimationPlayer::removeClip)
        .addFunction("getClipCount", &AnimationPlayer::getClipCount)
        .addFunction("getClipName", &AnimationPlayer::getClipName)
        .addProperty("speed", &AnimationPlayer::getSpeed, &AnimationPlayer::setSpeed)
        .addProperty("defaultClip", &AnimationPlayer::getDefaultClip, &AnimationPlayer::setDefaultClip)
        .addProperty("defaultFadeTime", &AnimationPlayer::getDefaultFadeTime, &AnimationPlayer::setDefaultFadeTime)
        .addProperty("playAutomatically", &AnimationPlayer::isPlayAutomatically, &AnimationPlayer::setPlayAutomatically)
        .addProperty("enabled", &AnimationPlayer::isEnabled, &AnimationPlayer::setEnabled)
        .addFunction("isLoaded", &AnimationPlayer::isLoaded)
        // a function called with an AnimationEvent when a clip event fires
        .addProperty("onEvent", [] (AnimationPlayer* self, lua_State*) { return &self->getOnEvent(); }, [] (AnimationPlayer* self, lua_State* L) { self->getOnEvent() = L; })
        // a function called with the state name when a once state ends (or a clamp_forever one first reaches its end)
        .addProperty("onFinish", [] (AnimationPlayer* self, lua_State*) { return &self->getOnFinish(); }, [] (AnimationPlayer* self, lua_State* L) { self->getOnFinish() = L; })
        .addFunction("getAnimationPlayerComponent", &AnimationPlayer::getComponent<AnimationPlayerComponent>)
        .endClass();

    // A baked socket's pose in the GameObject's space (VertexAnimator:getSocketPose)
    luabridge::getGlobalNamespace(L)
        .beginClass<VertexAnimatorSocketPose>("VertexAnimatorSocketPose")
        .addProperty("position", &VertexAnimatorSocketPose::position)
        .addProperty("rotation", &VertexAnimatorSocketPose::rotation)
        // false when the socket or the asset is not there
        .addProperty("valid", &VertexAnimatorSocketPose::valid)
        .endClass();

    // Plays a baked Vertex Animation (.vat) on the GPU: the GameObject and every instance of its
    // Instanced Mesh, each with its own clip, time, speed and crossfade. Instances count from 0
    luabridge::getGlobalNamespace(L)
        .deriveClass<VertexAnimator, EntityHandle>("VertexAnimator")
        .addConstructor <void (*) (Scene*, Entity)> ()
        // plays a clip now; with a fade > 0, crossFade
        .addFunction("play",
            luabridge::overload<const std::string&>(&VertexAnimator::play),
            luabridge::overload<const std::string&, float>(&VertexAnimator::play))
        // fades to a clip over `fade` seconds (defaultFadeTime without it)
        .addFunction("crossFade",
            luabridge::overload<const std::string&>(&VertexAnimator::crossFade),
            luabridge::overload<const std::string&, float>(&VertexAnimator::crossFade))
        // holds clips a and b at a fixed weight of b (0..1), b's phase following a's (walk/run by speed)
        .addFunction("blend", &VertexAnimator::blend)
        // plays a clip when the current one ends (or ends its cycle)
        .addFunction("queue", &VertexAnimator::queue)
        // holds the current pose
        .addFunction("stop", &VertexAnimator::stop)
        .addFunction("isPlaying",
            luabridge::constOverload<>(&VertexAnimator::isPlaying),
            luabridge::constOverload<const std::string&>(&VertexAnimator::isPlaying))
        // seconds in the current clip
        .addProperty("time", &VertexAnimator::getTime, &VertexAnimator::setTime)
        // 0..1 in the current clip
        .addProperty("normalizedTime", &VertexAnimator::getNormalizedTime)
        // multiplies every clip's speed (phases are kept)
        .addProperty("speed", &VertexAnimator::getSpeed, &VertexAnimator::setSpeed)
        // the clip playing (the one faded to during a crossfade)
        .addProperty("currentClip", &VertexAnimator::getCurrentClip)
        .addFunction("getClipNames", &VertexAnimator::getClipNames)
        // seconds, 0 for an unknown clip
        .addFunction("getClipLength", &VertexAnimator::getClipLength)
        // a baked socket now: position and rotation in the GameObject's space (of an instance with its index)
        .addFunction("getSocketPose",
            luabridge::constOverload<const std::string&>(&VertexAnimator::getSocketPose),
            luabridge::constOverload<const std::string&, int>(&VertexAnimator::getSocketPose))
        .addFunction("playInstance",
            luabridge::overload<int, const std::string&>(&VertexAnimator::playInstance),
            luabridge::overload<int, const std::string&, float>(&VertexAnimator::playInstance))
        .addFunction("crossFadeInstance", &VertexAnimator::crossFadeInstance)
        .addFunction("setInstanceSpeed", &VertexAnimator::setInstanceSpeed)
        .addFunction("setInstanceTime", &VertexAnimator::setInstanceTime)
        .addFunction("getInstanceClip", &VertexAnimator::getInstanceClip)
        .addFunction("getInstanceNormalizedTime", &VertexAnimator::getInstanceNormalizedTime)
        .addFunction("getInstanceCount", &VertexAnimator::getInstanceCount)
        // the GameObject and every instance
        .addFunction("playAll",
            luabridge::overload<const std::string&>(&VertexAnimator::playAll),
            luabridge::overload<const std::string&, float>(&VertexAnimator::playAll))
        // every instance at a seeded time of its clip (the same seed, the same times)
        .addFunction("randomizeTimes", &VertexAnimator::randomizeTimes)
        // the .vat, relative to the assets folder
        .addProperty("animation", &VertexAnimator::getAnimation, &VertexAnimator::setAnimation)
        .addProperty("defaultClip", &VertexAnimator::getDefaultClip, &VertexAnimator::setDefaultClip)
        .addProperty("playAutomatically", &VertexAnimator::isPlayAutomatically, &VertexAnimator::setPlayAutomatically)
        .addProperty("randomStart", &VertexAnimator::isRandomStart, &VertexAnimator::setRandomStart)
        .addProperty("defaultFadeTime", &VertexAnimator::getDefaultFadeTime, &VertexAnimator::setDefaultFadeTime)
        .addProperty("instanceEvents", &VertexAnimator::isInstanceEvents, &VertexAnimator::setInstanceEvents)
        // editor: animate in edit mode
        .addProperty("preview", &VertexAnimator::isPreview, &VertexAnimator::setPreview)
        .addProperty("enabled", &VertexAnimator::isEnabled, &VertexAnimator::setEnabled)
        .addFunction("isLoaded", &VertexAnimator::isLoaded)
        // why it does not play ("" when it does)
        .addFunction("getLoadError", &VertexAnimator::getLoadError)
        // a function called with an AnimationEvent when a clip event fires (GameObjects without instances)
        .addProperty("onEvent", [] (VertexAnimator* self, lua_State*) { return &self->getOnEvent(); }, [] (VertexAnimator* self, lua_State* L) { self->getOnEvent() = L; })
        // a function called with the clip name when a once clip ends
        .addProperty("onFinish", [] (VertexAnimator* self, lua_State*) { return &self->getOnFinish(); }, [] (VertexAnimator* self, lua_State* L) { self->getOnFinish() = L; })
        // a function called with (instance, AnimationEvent) when instanceEvents is on
        .addProperty("onInstanceEvent", [] (VertexAnimator* self, lua_State*) { return &self->getOnInstanceEvent(); }, [] (VertexAnimator* self, lua_State* L) { self->getOnInstanceEvent() = L; })
        // a function called with (instance, clip name) when instanceEvents is on
        .addProperty("onInstanceFinish", [] (VertexAnimator* self, lua_State*) { return &self->getOnInstanceFinish(); }, [] (VertexAnimator* self, lua_State* L) { self->getOnInstanceFinish() = L; })
        .addFunction("getVertexAnimatorComponent", &VertexAnimator::getComponent<VertexAnimatorComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<AudioSource, EntityHandle>("AudioSource")
        .addProperty("enabled", &AudioSource::isEnabled, &AudioSource::setEnabled)
        // Entity before bool: a number must bind the entity overload, not is3D
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity), void (*) (Scene*, bool)> ()
        .addFunction("loadSound", &AudioSource::loadSound)
        .addFunction("destroySound", &AudioSource::destroySound)
        .addFunction("getGameObject", &AudioSource::getGameObject)
        .addFunction("play", &AudioSource::play)
        .addFunction("pause", &AudioSource::pause)
        .addFunction("stop", &AudioSource::stop)
        .addFunction("seek", &AudioSource::seek)
        .addFunction("getLength", &AudioSource::getLength)
        .addFunction("getPlayingTime", &AudioSource::getPlayingTime)
        .addFunction("isPlaying", &AudioSource::isPlaying)
        .addFunction("isPaused", &AudioSource::isPaused)
        .addFunction("isStopped", &AudioSource::isStopped)
        .addProperty("sound3D", &AudioSource::isSound3D, &AudioSource::setSound3D)
        .addProperty("clockedSound", &AudioSource::isClockedSound, &AudioSource::setClockedSound)
        .addProperty("volume", &AudioSource::getVolume, &AudioSource::setVolume)
        .addProperty("speed", &AudioSource::getSpeed, &AudioSource::setSpeed)
        .addProperty("pan", &AudioSource::getPan, &AudioSource::setPan)
        .addProperty("looping", &AudioSource::isLooping, &AudioSource::setLooping)
        .addProperty("loopingPoint", &AudioSource::getLoopingPoint, &AudioSource::setLoopingPoint)
        .addProperty("protectVoice", &AudioSource::isProtectVoice, &AudioSource::setProtectVoice)
        .addFunction("setInaudibleBehavior", &AudioSource::setInaudibleBehavior)
        .addFunction("setMinMaxDistance", &AudioSource::setMinMaxDistance)
        .addProperty("minDistance", &AudioSource::getMinDistance, &AudioSource::setMinDistance)
        .addProperty("maxDistance", &AudioSource::getMaxDistance, &AudioSource::setMaxDistance)
        .addProperty("attenuationModel", &AudioSource::getAttenuationModel, &AudioSource::setAttenuationModel)
        .addProperty("attenuationRolloffFactor", &AudioSource::getAttenuationRolloffFactor, &AudioSource::setAttenuationRolloffFactor)
        .addProperty("dopplerFactor", &AudioSource::getDopplerFactor, &AudioSource::setDopplerFactor)
        .addFunction("getAudioSourceComponent", &AudioSource::getComponent<AudioSourceComponent>)
        .endClass();

    // a runtime physics material: PhysicsMaterial.load(path) is the asset's copy shared by every
    // collider naming it, PhysicsMaterial.new{...} a material of the script's own
    luabridge::getGlobalNamespace(L)
        .beginClass<PhysicsMaterial>("PhysicsMaterial")
        .addStaticFunction("load", +[](const std::string& path) -> std::optional<PhysicsMaterial> {
            return luaMaterialResult(PhysicsMaterial::load(path));
        })
        .addStaticFunction("new", +[](luabridge::LuaRef params) -> PhysicsMaterial {
            PhysicsMaterialData defaults;
            float friction = defaults.friction;
            float bounciness = defaults.bounciness;
            PhysicsMaterialCombine frictionCombine = PhysicsMaterialCombine::AVERAGE;
            PhysicsMaterialCombine bounceCombine = PhysicsMaterialCombine::AVERAGE;
            if (params.isTable()){
                if (params["friction"].isNumber()) friction = params["friction"].unsafe_cast<float>();
                if (params["bounciness"].isNumber()) bounciness = params["bounciness"].unsafe_cast<float>();
                frictionCombine = luaCombineArg(params["frictionCombine"], frictionCombine);
                bounceCombine = luaCombineArg(params["bounceCombine"], bounceCombine);
            }
            return PhysicsMaterial::create(friction, bounciness, frictionCombine, bounceCombine);
        })
        .addProperty("path", &PhysicsMaterial::getPath)
        .addProperty("friction", &PhysicsMaterial::getFriction, &PhysicsMaterial::setFriction)
        .addProperty("bounciness", &PhysicsMaterial::getBounciness, &PhysicsMaterial::setBounciness)
        .addProperty("frictionCombine", &PhysicsMaterial::getFrictionCombine, &PhysicsMaterial::setFrictionCombine)
        .addProperty("bounceCombine", &PhysicsMaterial::getBounceCombine, &PhysicsMaterial::setBounceCombine)
        .addFunction("__eq", +[](const PhysicsMaterial* self, const PhysicsMaterial* other) -> bool { return other && *self == *other; })
        .endClass();

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .deriveClass<Rigidbody2D, EntityHandle>("Rigidbody2D")
        .addConstructor <void (Scene*, Entity)> ()
        .addFunction("getAttachedObject", &Rigidbody2D::getAttachedObject)
        .addProperty("pointsToMeterScale", &Rigidbody2D::getPointsToMeterScale)
        .addFunction("load", &Rigidbody2D::load)
        .addFunction("createBoxCollider", &Rigidbody2D::createBoxCollider)
        .addFunction("createCenteredBoxCollider", 
            luabridge::overload<float, float>(&Rigidbody2D::createCenteredBoxCollider),
            luabridge::overload<float, float, Vector2, float>(&Rigidbody2D::createCenteredBoxCollider))
        .addFunction("createRoundedBoxCollider", &Rigidbody2D::createRoundedBoxCollider)
        .addFunction("createPolygonCollider", &Rigidbody2D::createPolygonCollider)
        .addFunction("createCircleCollider", &Rigidbody2D::createCircleCollider)
        .addFunction("createCapsuleCollider", &Rigidbody2D::createCapsuleCollider)
        .addFunction("createSegmentCollider", &Rigidbody2D::createSegmentCollider)
        .addFunction("createChainCollider", &Rigidbody2D::createChainCollider)
        .addFunction("removeAllColliders", &Rigidbody2D::removeAllColliders)
        .addFunction("getBodyContacts", &Rigidbody2D::getBodyContacts)
        .addFunction("getColliderContacts", &Rigidbody2D::getColliderContacts)
        .addFunction("getNumColliders", &Rigidbody2D::getNumColliders)
        .addFunction("getColliderType", &Rigidbody2D::getColliderType)
        .addProperty("shapeDensity", (float(Rigidbody2D::*)()const)&Rigidbody2D::getColliderDensity, (void(Rigidbody2D::*)(float))&Rigidbody2D::setColliderDensity)
        .addFunction("setColliderDensity", 
            luabridge::overload<float>(&Rigidbody2D::setColliderDensity),
            luabridge::overload<size_t, float>(&Rigidbody2D::setColliderDensity))
        .addFunction("getColliderDensity", 
            luabridge::overload<>(&Rigidbody2D::getColliderDensity),
            luabridge::overload<size_t>(&Rigidbody2D::getColliderDensity))
        .addProperty("shapeFriction", (float(Rigidbody2D::*)()const)&Rigidbody2D::getColliderFriction, (void(Rigidbody2D::*)(float))&Rigidbody2D::setColliderFriction)
        .addFunction("setColliderFriction", 
            luabridge::overload<float>(&Rigidbody2D::setColliderFriction),
            luabridge::overload<size_t, float>(&Rigidbody2D::setColliderFriction))
        .addFunction("getColliderFriction", 
            luabridge::overload<>(&Rigidbody2D::getColliderFriction),
            luabridge::overload<size_t>(&Rigidbody2D::getColliderFriction))
        .addProperty("shapeRestitution", (float(Rigidbody2D::*)()const)&Rigidbody2D::getColliderRestitution, (void(Rigidbody2D::*)(float))&Rigidbody2D::setColliderRestitution)
        .addFunction("setColliderRestitution", 
            luabridge::overload<float>(&Rigidbody2D::setColliderRestitution),
            luabridge::overload<size_t, float>(&Rigidbody2D::setColliderRestitution))
        .addFunction("getColliderRestitution", 
            luabridge::overload<>(&Rigidbody2D::getColliderRestitution),
            luabridge::overload<size_t>(&Rigidbody2D::getColliderRestitution))
        .addProperty("shapeSensor", (bool(Rigidbody2D::*)()const)&Rigidbody2D::isColliderSensor, (void(Rigidbody2D::*)(bool))&Rigidbody2D::setColliderSensor)
        .addFunction("setColliderSensor", 
            luabridge::overload<bool>(&Rigidbody2D::setColliderSensor),
            luabridge::overload<size_t, bool>(&Rigidbody2D::setColliderSensor))
        .addFunction("isColliderSensor", 
            luabridge::overload<>(&Rigidbody2D::isColliderSensor),
            luabridge::overload<size_t>(&Rigidbody2D::isColliderSensor))
        .addProperty("shapeEnableHitEvents", (bool(Rigidbody2D::*)()const)&Rigidbody2D::isColliderEnableHitEvents, (void(Rigidbody2D::*)(bool))&Rigidbody2D::setColliderEnableHitEvents)
        .addFunction("setColliderEnableHitEvents", 
            luabridge::overload<bool>(&Rigidbody2D::setColliderEnableHitEvents),
            luabridge::overload<size_t, bool>(&Rigidbody2D::setColliderEnableHitEvents))
        .addFunction("isColliderEnableHitEvents", 
            luabridge::overload<>(&Rigidbody2D::isColliderEnableHitEvents),
            luabridge::overload<size_t>(&Rigidbody2D::isColliderEnableHitEvents))
        .addProperty("shapeContactEvents", (bool(Rigidbody2D::*)()const)&Rigidbody2D::isColliderContactEvents, (void(Rigidbody2D::*)(bool))&Rigidbody2D::setColliderContactEvents)
        .addFunction("setColliderContactEvents", 
            luabridge::overload<bool>(&Rigidbody2D::setColliderContactEvents),
            luabridge::overload<size_t, bool>(&Rigidbody2D::setColliderContactEvents))
        .addFunction("isColliderContactEvents", 
            luabridge::overload<>(&Rigidbody2D::isColliderContactEvents),
            luabridge::overload<size_t>(&Rigidbody2D::isColliderContactEvents))
        .addProperty("shapePreSolveEvents", (bool(Rigidbody2D::*)()const)&Rigidbody2D::isColliderPreSolveEvents, (void(Rigidbody2D::*)(bool))&Rigidbody2D::setColliderPreSolveEvents)
        .addFunction("setColliderPreSolveEvents", 
            luabridge::overload<bool>(&Rigidbody2D::setColliderPreSolveEvents),
            luabridge::overload<size_t, bool>(&Rigidbody2D::setColliderPreSolveEvents))
        .addFunction("isColliderPreSolveEvents", 
            luabridge::overload<>(&Rigidbody2D::isColliderPreSolveEvents),
            luabridge::overload<size_t>(&Rigidbody2D::isColliderPreSolveEvents))
        .addProperty("shapeSensorEvents", (bool(Rigidbody2D::*)() const)&Rigidbody2D::isColliderSensorEvents, (void(Rigidbody2D::*)(bool))&Rigidbody2D::setColliderSensorEvents)
        .addFunction("setColliderSensorEvents", 
            luabridge::overload<bool>(&Rigidbody2D::setColliderSensorEvents),
            luabridge::overload<size_t, bool>(&Rigidbody2D::setColliderSensorEvents))
        .addFunction("isColliderSensorEvents", 
            luabridge::overload<>(&Rigidbody2D::isColliderSensorEvents),
            luabridge::overload<size_t>(&Rigidbody2D::isColliderSensorEvents))
        .addProperty("position", &Rigidbody2D::getPosition, &Rigidbody2D::setPosition)
        .addProperty("angle", &Rigidbody2D::getAngle, &Rigidbody2D::setAngle)
        .addProperty("linearVelocity", &Rigidbody2D::getLinearVelocity, &Rigidbody2D::setLinearVelocity)
        .addProperty("angularVelocity", &Rigidbody2D::getAngularVelocity, &Rigidbody2D::setAngularVelocity)
        .addProperty("linearDamping", &Rigidbody2D::getLinearDamping, &Rigidbody2D::setLinearDamping)
        .addProperty("angularDamping", &Rigidbody2D::getAngularDamping, &Rigidbody2D::setAngularDamping)
        .addProperty("enableSleep", &Rigidbody2D::isEnableSleep, &Rigidbody2D::setEnableSleep)
        .addProperty("awake", &Rigidbody2D::isAwake, &Rigidbody2D::setAwake)
        .addProperty("fixedRotation", &Rigidbody2D::isFixedRotation, &Rigidbody2D::setFixedRotation)
        .addProperty("bullet", &Rigidbody2D::isBullet, &Rigidbody2D::setBullet)
        .addProperty("type", &Rigidbody2D::getType, &Rigidbody2D::setType)
        .addProperty("enabled", &Rigidbody2D::isEnabled, &Rigidbody2D::setEnabled)
        .addProperty("gravityScale", &Rigidbody2D::getGravityScale, &Rigidbody2D::setGravityScale)
        .addFunction("setBitsFilter", 
            luabridge::overload<uint16_t, uint16_t>(&Rigidbody2D::setBitsFilter),
            luabridge::overload<size_t, uint16_t, uint16_t>(&Rigidbody2D::setBitsFilter))
        .addProperty("categoryBitsFilter", (uint16_t(Rigidbody2D::*)()const)&Rigidbody2D::getCategoryBitsFilter, (void(Rigidbody2D::*)(uint16_t))&Rigidbody2D::setCategoryBitsFilter)
        .addFunction("setCategoryBitsFilter", 
            luabridge::overload<uint16_t>(&Rigidbody2D::setCategoryBitsFilter),
            luabridge::overload<size_t, uint16_t>(&Rigidbody2D::setCategoryBitsFilter))
        .addFunction("getCategoryBitsFilter", 
            luabridge::overload<>(&Rigidbody2D::getCategoryBitsFilter),
            luabridge::overload<size_t>(&Rigidbody2D::getCategoryBitsFilter))
        .addProperty("maskBitsFilter", (uint16_t(Rigidbody2D::*)()const)&Rigidbody2D::getMaskBitsFilter, (void(Rigidbody2D::*)(uint16_t))&Rigidbody2D::setMaskBitsFilter)
        .addFunction("setMaskBitsFilter", 
            luabridge::overload<uint16_t>(&Rigidbody2D::setMaskBitsFilter),
            luabridge::overload<size_t, uint16_t>(&Rigidbody2D::setMaskBitsFilter))
        .addFunction("getMaskBitsFilter", 
            luabridge::overload<>(&Rigidbody2D::getMaskBitsFilter),
            luabridge::overload<size_t>(&Rigidbody2D::getMaskBitsFilter))
        .addProperty("groupIndexFilter", (int16_t(Rigidbody2D::*)()const)&Rigidbody2D::getGroupIndexFilter, (void(Rigidbody2D::*)(int16_t))&Rigidbody2D::setGroupIndexFilter)
        .addFunction("setGroupIndexFilter", 
            luabridge::overload<int16_t>(&Rigidbody2D::setGroupIndexFilter),
            luabridge::overload<size_t, int16_t>(&Rigidbody2D::setGroupIndexFilter))
        .addFunction("getGroupIndexFilter", 
            luabridge::overload<>(&Rigidbody2D::getGroupIndexFilter),
            luabridge::overload<size_t>(&Rigidbody2D::getGroupIndexFilter))
        .addFunction("getMass", &Rigidbody2D::getMass)
        .addFunction("getRotationalInertia", &Rigidbody2D::getRotationalInertia)
        .addFunction("applyMassFromColliders", &Rigidbody2D::applyMassFromColliders)
        // Unity's mass fields: setting mass, centerOfMass or inertia turns its Auto off
        .addProperty("mass", &Rigidbody2D::getMass, &Rigidbody2D::setMass)
        .addProperty("useAutoMass", &Rigidbody2D::isUseAutoMass, &Rigidbody2D::setUseAutoMass)
        .addProperty("centerOfMass", &Rigidbody2D::getCenterOfMass, &Rigidbody2D::setCenterOfMass)
        .addProperty("autoCenterOfMass", &Rigidbody2D::isAutoCenterOfMass, &Rigidbody2D::setAutoCenterOfMass)
        .addProperty("inertia", &Rigidbody2D::getInertia, &Rigidbody2D::setInertia)
        .addProperty("autoInertia", &Rigidbody2D::isAutoInertia, &Rigidbody2D::setAutoInertia)
        .addProperty("collisionDetection", &Rigidbody2D::getCollisionDetection, &Rigidbody2D::setCollisionDetection)
        .addProperty("sleepingMode", &Rigidbody2D::getSleepingMode, &Rigidbody2D::setSleepingMode)
        // the default material of the colliders without one: a path, a PhysicsMaterial or nil
        .addProperty("material",
            +[](const Rigidbody2D* self) -> std::optional<PhysicsMaterial> { return luaMaterialResult(self->getMaterial()); },
            +[](Rigidbody2D* self, luabridge::LuaRef value) {
                PhysicsMaterial material;
                std::string path;
                if (!luaMaterialArg(value, material, path)){
                    throw std::runtime_error("Rigidbody2D.material: a .physicsmaterial path, a PhysicsMaterial or nil");
                }
                if (material.valid()) self->setMaterial(material);
                else self->setMaterial(path);
            })
        // collider edits: that collider only is made again (index from 0; out of range: an error)
        .addFunction("removeCollider", &Rigidbody2D::removeCollider)
        .addFunction("setColliderPoints", &Rigidbody2D::setColliderPoints)
        .addFunction("getColliderPoints", &Rigidbody2D::getColliderPoints)
        .addFunction("setColliderLoop", &Rigidbody2D::setColliderLoop)
        .addFunction("isColliderLoop", &Rigidbody2D::isColliderLoop)
        .addFunction("setColliderRadius", &Rigidbody2D::setColliderRadius)
        .addFunction("getColliderRadius", &Rigidbody2D::getColliderRadius)
        .addFunction("setColliderCenter", &Rigidbody2D::setColliderCenter)
        .addFunction("getColliderCenter", &Rigidbody2D::getColliderCenter)
        .addFunction("setColliderSegment", &Rigidbody2D::setColliderSegment)
        .addFunction("setColliderMaterial", +[](Rigidbody2D* self, size_t index, luabridge::LuaRef value) {
            PhysicsMaterial material;
            std::string path;
            if (!luaMaterialArg(value, material, path)){
                throw std::runtime_error("Rigidbody2D:setColliderMaterial: a .physicsmaterial path, a PhysicsMaterial or nil");
            }
            if (material.valid()) self->setColliderMaterial(index, material);
            else self->setColliderMaterial(index, path);
        })
        .addFunction("getColliderMaterial", +[](const Rigidbody2D* self, size_t index) -> std::optional<PhysicsMaterial> {
            return luaMaterialResult(self->getColliderMaterial(index));
        })
        .addFunction("setColliderTrigger", &Rigidbody2D::setColliderTrigger)
        .addFunction("isColliderTrigger", &Rigidbody2D::isColliderTrigger)
        .addFunction("getColliderPieceCount", &Rigidbody2D::getColliderPieceCount)
        .addFunction("applyForce", &Rigidbody2D::applyForce)
        .addFunction("applyForceToCenter", &Rigidbody2D::applyForceToCenter)
        .addFunction("applyTorque", &Rigidbody2D::applyTorque)
        .addFunction("applyLinearImpulse", &Rigidbody2D::applyLinearImpulse)
        .addFunction("applyLinearImpulseToCenter", &Rigidbody2D::applyLinearImpulseToCenter)
        .addFunction("applyAngularImpulse", &Rigidbody2D::applyAngularImpulse)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Joint2D, EntityHandle>("Joint2D")
        .addConstructor <void (Scene*), void (Scene*, Entity)> ()
        // as before local anchors: world points, the other fields as Box2D had them
        .addFunction("setDistanceJoint",
            luabridge::overload<Entity, Entity>(&Joint2D::setDistanceJoint),
            luabridge::overload<Entity, Entity, Vector2, Vector2, bool>(&Joint2D::setDistanceJoint))
        .addFunction("setRevoluteJoint", &Joint2D::setRevoluteJoint)
        .addFunction("setPrismaticJoint", &Joint2D::setPrismaticJoint)
        .addFunction("setMouseJoint", &Joint2D::setMouseJoint)
        .addFunction("setWheelJoint", &Joint2D::setWheelJoint)
        .addFunction("setWeldJoint", &Joint2D::setWeldJoint)
        .addFunction("setMotorJoint", &Joint2D::setMotorJoint)
        .addFunction("getType", &Joint2D::getType)
        .addProperty("type", &Joint2D::getType, &Joint2D::setType)
        .addProperty("enabled", &Joint2D::isEnabled, &Joint2D::setEnabled)
        // the body the joint is on (Unity: the joint's Rigidbody 2D) and the connected one;
        // set with a GameObject, a Rigidbody2D or nil (the world)
        .addProperty("bodyA",
            +[](const Joint2D* self) { return luaJointBody(self, self->getBodyA()); },
            +[](Joint2D* self, luabridge::LuaRef value) { self->setBodyA(luaJointBodyArg(self, value, value.state(), "bodyA")); })
        .addProperty("bodyB",
            +[](const Joint2D* self) { return luaJointBody(self, self->getBodyB()); },
            +[](Joint2D* self, luabridge::LuaRef value) { self->setBodyB(luaJointBodyArg(self, value, value.state(), "bodyB")); })
        .addProperty("connectedBody",
            +[](const Joint2D* self) { return luaJointBody(self, self->getBodyB()); },
            +[](Joint2D* self, luabridge::LuaRef value) { self->setBodyB(luaJointBodyArg(self, value, value.state(), "connectedBody")); })
        .addProperty("anchorSpace", &Joint2D::getAnchorSpace, &Joint2D::setAnchorSpace)
        .addProperty("anchorA", &Joint2D::getAnchorA, &Joint2D::setAnchorA)
        .addProperty("anchorB", &Joint2D::getAnchorB, &Joint2D::setAnchorB)
        .addProperty("anchor", &Joint2D::getAnchorA, &Joint2D::setAnchorA)
        .addProperty("connectedAnchor", &Joint2D::getAnchorB, &Joint2D::setAnchorB)
        .addProperty("autoAnchors", &Joint2D::isAutoAnchors, &Joint2D::setAutoAnchors)
        .addProperty("axis", &Joint2D::getAxis, &Joint2D::setAxis)
        .addProperty("enableCollision", &Joint2D::isEnableCollision, &Joint2D::setEnableCollision)
        .addProperty("breakForce", &Joint2D::getBreakForce, &Joint2D::setBreakForce)
        .addProperty("breakTorque", &Joint2D::getBreakTorque, &Joint2D::setBreakTorque)
        .addProperty("breakAction", &Joint2D::getBreakAction, &Joint2D::setBreakAction)
        .addProperty("useLimits", &Joint2D::isUseLimits, &Joint2D::setUseLimits)
        .addProperty("lowerAngle", &Joint2D::getLowerAngle, &Joint2D::setLowerAngle)
        .addProperty("upperAngle", &Joint2D::getUpperAngle, &Joint2D::setUpperAngle)
        .addProperty("lowerTranslation", &Joint2D::getLowerTranslation, &Joint2D::setLowerTranslation)
        .addProperty("upperTranslation", &Joint2D::getUpperTranslation, &Joint2D::setUpperTranslation)
        .addProperty("useMotor", &Joint2D::isUseMotor, &Joint2D::setUseMotor)
        .addProperty("motorSpeed", &Joint2D::getMotorSpeed, &Joint2D::setMotorSpeed)
        .addProperty("maxMotorTorque", &Joint2D::getMaxMotorTorque, &Joint2D::setMaxMotorTorque)
        .addProperty("maxMotorForce", &Joint2D::getMaxMotorForce, &Joint2D::setMaxMotorForce)
        .addProperty("enableSpring", &Joint2D::isEnableSpring, &Joint2D::setEnableSpring)
        .addProperty("frequency", &Joint2D::getFrequency, &Joint2D::setFrequency)
        .addProperty("dampingRatio", &Joint2D::getDampingRatio, &Joint2D::setDampingRatio)
        .addProperty("length", &Joint2D::getLength, &Joint2D::setLength)
        .addProperty("autoLength", &Joint2D::isAutoLength, &Joint2D::setAutoLength)
        .addProperty("minLength", &Joint2D::getMinLength, &Joint2D::setMinLength)
        .addProperty("maxLength", &Joint2D::getMaxLength, &Joint2D::setMaxLength)
        .addProperty("rope", &Joint2D::isRope, &Joint2D::setRope)
        .addProperty("linearFrequency", &Joint2D::getLinearFrequency, &Joint2D::setLinearFrequency)
        .addProperty("angularFrequency", &Joint2D::getAngularFrequency, &Joint2D::setAngularFrequency)
        .addProperty("linearDampingRatio", &Joint2D::getLinearDampingRatio, &Joint2D::setLinearDampingRatio)
        .addProperty("angularDampingRatio", &Joint2D::getAngularDampingRatio, &Joint2D::setAngularDampingRatio)
        .addProperty("linearOffset", &Joint2D::getLinearOffset, &Joint2D::setLinearOffset)
        .addProperty("angularOffset", &Joint2D::getAngularOffset, &Joint2D::setAngularOffset)
        .addProperty("autoOffset", &Joint2D::isAutoOffset, &Joint2D::setAutoOffset)
        .addProperty("maxForce", &Joint2D::getMaxForce, &Joint2D::setMaxForce)
        .addProperty("maxTorque", &Joint2D::getMaxTorque, &Joint2D::setMaxTorque)
        .addProperty("correctionFactor", &Joint2D::getCorrectionFactor, &Joint2D::setCorrectionFactor)
        .addProperty("target", &Joint2D::getTarget, &Joint2D::setTarget)
        // read-outs of the live joint (0 without one)
        .addProperty("reactionForce", &Joint2D::getReactionForce)
        .addProperty("reactionTorque", &Joint2D::getReactionTorque)
        .addProperty("jointAngle", &Joint2D::getJointAngle)
        .addProperty("jointSpeed", &Joint2D::getJointSpeed)
        .addProperty("jointTranslation", &Joint2D::getJointTranslation)
        .addProperty("jointLinearSpeed", &Joint2D::getJointLinearSpeed)
        .addProperty("motorTorque", &Joint2D::getMotorTorque)
        .addProperty("motorForce", &Joint2D::getMotorForce)
        .addProperty("currentLength", &Joint2D::getCurrentLength)
        .addProperty("limitState", &Joint2D::getLimitState)
        .addProperty("referenceAngle", &Joint2D::getReferenceAngle)
        .addProperty("broken", &Joint2D::isBroken)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Manifold2D>("Manifold2D")
        .addFunction("getManifoldPointAnchorA", &Manifold2D::getManifoldPointAnchorA)
        .addFunction("getManifoldPointAnchorB", &Manifold2D::getManifoldPointAnchorB)
        .addFunction("getManifoldPointPosition", &Manifold2D::getManifoldPointPosition)
        .addFunction("getManifoldPointNormalImpulse", &Manifold2D::getManifoldPointNormalImpulse)
        .addFunction("getManifoldPointNormalVelocity", &Manifold2D::getManifoldPointNormalVelocity)
        .addFunction("getManifoldPointTangentImpulse", &Manifold2D::getManifoldPointTangentImpulse)
        .addFunction("getManifoldPointSeparation", &Manifold2D::getManifoldPointSeparation)
        .addFunction("isManifoldPointPersisted", &Manifold2D::isManifoldPointPersisted)
        .addFunction("getNormal", &Manifold2D::getNormal)
        .addFunction("getPointCount", &Manifold2D::getPointCount)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Contact2D>("Contact2D")
        .addFunction("getManifold", &Contact2D::getManifold)
        .addFunction("getBodyEntityA", &Contact2D::getBodyEntityA)
        .addFunction("getBodyA", &Contact2D::getBodyA)
        .addFunction("getColliderIndexA", &Contact2D::getColliderIndexA)
        .addFunction("getBodyEntityB", &Contact2D::getBodyEntityB)
        .addFunction("getBodyB", &Contact2D::getBodyB)
        .addFunction("getColliderIndexB", &Contact2D::getColliderIndexB)
        .endClass();
    // physics-contacts: what onCollisionEnter2D / onTriggerEnter2D... get. Unity's Collision2D
    // names the other side collider / rigidbody and this side otherCollider / otherRigidbody;
    // thisCollider / thisRigidbody say it plainly.
    luabridge::getGlobalNamespace(L)
        .beginClass<ColliderRef2D>("Collider2D")
        .addProperty("gameObject", [] (const ColliderRef2D* self) -> std::optional<GameObject> { return luaColliderGameObject(*self); })
        .addProperty("rigidbody", [] (const ColliderRef2D* self) -> std::optional<Rigidbody2D> { return self->hasRigidbody() ? std::optional<Rigidbody2D>(self->getRigidbody()) : std::nullopt; })
        .addProperty("attachedRigidbody", [] (const ColliderRef2D* self) -> std::optional<Rigidbody2D> { return self->hasRigidbody() ? std::optional<Rigidbody2D>(self->getRigidbody()) : std::nullopt; })
        .addProperty("index", &ColliderRef2D::getIndex)
        .addProperty("type", &ColliderRef2D::getType)
        .addProperty("isTrigger", &ColliderRef2D::isTrigger)
        .addProperty("tag", &ColliderRef2D::getTag)
        .addFunction("compareTag", &ColliderRef2D::compareTag)
        .addProperty("receiver", [] (const ColliderRef2D* self) -> std::optional<GameObject> { return luaColliderReceiver(*self); })
        .addFunction("isValid", &ColliderRef2D::isValid)
        .addFunction("__eq", [] (const ColliderRef2D* self, const ColliderRef2D* other) { return other && *self == *other; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ContactPoint2D>("ContactPoint2D")
        .addProperty("point", &ContactPoint2D::getPoint)
        .addProperty("normal", &ContactPoint2D::getNormal)
        .addProperty("separation", &ContactPoint2D::getSeparation)
        .addProperty("normalImpulse", &ContactPoint2D::getNormalImpulse)
        .addProperty("tangentImpulse", &ContactPoint2D::getTangentImpulse)
        .addProperty("relativeVelocity", &ContactPoint2D::getRelativeVelocity)
        .addProperty("thisCollider", &ContactPoint2D::getThisCollider)
        .addProperty("otherCollider", &ContactPoint2D::getOtherCollider)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Collision2D>("Collision2D")
        .addProperty("gameObject", [] (const Collision2D* self) -> std::optional<GameObject> { return luaColliderGameObject(self->otherCollider); })
        .addProperty("collider", &Collision2D::getCollider)
        .addProperty("rigidbody", [] (const Collision2D* self) -> std::optional<Rigidbody2D> { return self->otherCollider.hasRigidbody() ? std::optional<Rigidbody2D>(self->getRigidbody()) : std::nullopt; })
        .addProperty("otherCollider", &Collision2D::getThisCollider)
        .addProperty("otherRigidbody", [] (const Collision2D* self) -> std::optional<Rigidbody2D> { return self->thisCollider.hasRigidbody() ? std::optional<Rigidbody2D>(self->getThisRigidbody()) : std::nullopt; })
        .addProperty("thisCollider", &Collision2D::getThisCollider)
        .addProperty("thisRigidbody", [] (const Collision2D* self) -> std::optional<Rigidbody2D> { return self->thisCollider.hasRigidbody() ? std::optional<Rigidbody2D>(self->getThisRigidbody()) : std::nullopt; })
        .addProperty("relativeVelocity", &Collision2D::getRelativeVelocity)
        .addProperty("enabled", &Collision2D::isEnabled)
        .addProperty("contactCount", &Collision2D::getContactCount)
        .addFunction("getContact", &luaGetContact<Collision2D>)
        .addProperty("contacts", [] (const Collision2D* self, lua_State* L) -> luabridge::LuaRef { return luaContactsTable(self, L); })
        .endClass();
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .deriveClass<Rigidbody, EntityHandle>("Rigidbody")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("enabled", &Rigidbody::isEnabled, &Rigidbody::setEnabled)
        .addFunction("getAttachedObject", &Rigidbody::getAttachedObject)
        .addFunction("load", &Rigidbody::load)
        .addFunction("createBoxCollider", 
            luabridge::overload<float, float, float>(&Rigidbody::createBoxCollider),
            luabridge::overload<Vector3, Quaternion, float, float, float>(&Rigidbody::createBoxCollider))
        .addFunction("createSphereCollider", 
            luabridge::overload<float>(&Rigidbody::createSphereCollider),
            luabridge::overload<Vector3, Quaternion, float>(&Rigidbody::createSphereCollider))
        .addFunction("createCapsuleCollider", 
            luabridge::overload<float, float>(&Rigidbody::createCapsuleCollider),
            luabridge::overload<Vector3, Quaternion, float, float>(&Rigidbody::createCapsuleCollider))
        .addFunction("createTaperedCapsuleCollider", 
            luabridge::overload<float, float, float>(&Rigidbody::createTaperedCapsuleCollider),
            luabridge::overload<Vector3, Quaternion, float, float, float>(&Rigidbody::createTaperedCapsuleCollider))
        .addFunction("createCylinderCollider", 
            luabridge::overload<float, float>(&Rigidbody::createCylinderCollider),
            luabridge::overload<Vector3, Quaternion, float, float>(&Rigidbody::createCylinderCollider))
        .addFunction("createConvexHullCollider",
            luabridge::overload<>(&Rigidbody::createConvexHullCollider),
            luabridge::overload<std::vector<Vector3>>(&Rigidbody::createConvexHullCollider),
            luabridge::overload<Vector3, Quaternion, std::vector<Vector3>>(&Rigidbody::createConvexHullCollider))
        .addFunction("createMeshCollider", 
            luabridge::overload<>(&Rigidbody::createMeshCollider),
            luabridge::overload<std::vector<Vector3>, std::vector<uint16_t>>(&Rigidbody::createMeshCollider),
            luabridge::overload<Vector3, Quaternion, std::vector<Vector3>, std::vector<uint16_t>>(&Rigidbody::createMeshCollider))
        .addFunction("createHeightFieldCollider", 
            luabridge::overload<>(&Rigidbody::createHeightFieldCollider),
            luabridge::overload<unsigned int>(&Rigidbody::createHeightFieldCollider))
        .addFunction("getNumColliders", &Rigidbody::getNumColliders)
        .addFunction("getColliderType", &Rigidbody::getColliderType)
        .addProperty("shapeDensity", (float(Rigidbody::*)()const)&Rigidbody::getColliderDensity, (void(Rigidbody::*)(float))&Rigidbody::setColliderDensity)
        .addFunction("setColliderDensity", 
            luabridge::overload<float>(&Rigidbody::setColliderDensity),
            luabridge::overload<size_t, float>(&Rigidbody::setColliderDensity))
        .addFunction("getColliderDensity", 
            luabridge::overload<>(&Rigidbody::getColliderDensity),
            luabridge::overload<size_t>(&Rigidbody::getColliderDensity))
        .addProperty("type", &Rigidbody::getType, &Rigidbody::setType)
        .addProperty("motionQuality", &Rigidbody::getMotionQuality, &Rigidbody::setMotionQuality)
        .addFunction("canBeKinematicOrDynamic", &Rigidbody::canBeKinematicOrDynamic)
        .addFunction("activate", &Rigidbody::activate)
        .addFunction("deactivate", &Rigidbody::deactivate)
        .addProperty("sensor", &Rigidbody::isSensor, &Rigidbody::setIsSensor)
        .addProperty("collideKinematicVsNonDynamic", &Rigidbody::isCollideKinematicVsNonDynamic, &Rigidbody::setCollideKinematicVsNonDynamic)
        .addFunction("setAllowedDOFsAll", &Rigidbody::setAllowedDOFsAll)
        .addFunction("setAllowedDOFs2DPlane", &Rigidbody::setAllowedDOFs2DPlane)
        .addFunction("setAllowedDOFs", &Rigidbody::setAllowedDOFs)
        .addProperty("mass", &Rigidbody::getMass, &Rigidbody::setMass)
        .addFunction("setOverrideMassAndInertia", &Rigidbody::setOverrideMassAndInertia)
        .addProperty("gravityFactor", &Rigidbody::getGravityFactor, &Rigidbody::setGravityFactor)
        .addProperty("collisionGroupID", &Rigidbody::getCollisionGroupID, &Rigidbody::setCollisionGroupID)
        .addFunction("setBitsFilter", &Rigidbody::setBitsFilter)
        .addProperty("categoryBitsFilter", &Rigidbody::getCategoryBitsFilter, &Rigidbody::setCategoryBitsFilter)
        .addProperty("maskBitsFilter", &Rigidbody::getMaskBitsFilter, &Rigidbody::setMaskBitsFilter)
        .addProperty("allowSleeping", &Rigidbody::isAllowSleeping, &Rigidbody::setAllowSleeping)
        .addProperty("friction", &Rigidbody::getFriction, &Rigidbody::setFriction)
        .addProperty("restitution", &Rigidbody::getRestitution, &Rigidbody::setRestitution)
        .addProperty("buoyancy", &Rigidbody::getBuoyancy, &Rigidbody::setBuoyancy)
        .addProperty("waterDrag", &Rigidbody::getWaterDrag, &Rigidbody::setWaterDrag)
        .addProperty("waterAngularDrag", &Rigidbody::getWaterAngularDrag, &Rigidbody::setWaterAngularDrag)
        .addProperty("position", &Rigidbody::getPosition, &Rigidbody::setPosition)
        .addProperty("rotation", &Rigidbody::getRotation, &Rigidbody::setRotation)
        .addProperty("linearVelocity", &Rigidbody::getLinearVelocity, &Rigidbody::setLinearVelocity)
        .addFunction("setLinearVelocityClamped", &Rigidbody::setLinearVelocityClamped)
        .addProperty("angularVelocity", &Rigidbody::getAngularVelocity, &Rigidbody::setAngularVelocity)
        .addFunction("setAngularVelocityClamped", &Rigidbody::setAngularVelocityClamped)
        .addFunction("getPointVelocityCOM", &Rigidbody::getPointVelocityCOM)
        .addFunction("getPointVelocity", &Rigidbody::getPointVelocity)
        .addFunction("getAccumulatedForce", &Rigidbody::getAccumulatedForce)
        .addFunction("getAccumulatedTorque", &Rigidbody::getAccumulatedTorque)
        .addFunction("getInverseInertia", &Rigidbody::getInverseInertia)
        .addFunction("applyForce", 
            luabridge::overload<const Vector3&>(&Rigidbody::applyForce),
            luabridge::overload<const Vector3&, const Vector3&>(&Rigidbody::applyForce))
        .addFunction("applyTorque", &Rigidbody::applyTorque)
        .addFunction("applyImpulse", 
            luabridge::overload<const Vector3&>(&Rigidbody::applyImpulse),
            luabridge::overload<const Vector3&, const Vector3&>(&Rigidbody::applyImpulse))
        .addFunction("applyAngularImpulse", &Rigidbody::applyAngularImpulse)
        .addFunction("applyBuoyancyImpulse", &Rigidbody::applyBuoyancyImpulse)
        .addFunction("getCenterOfMassPosition", &Rigidbody::getCenterOfMassPosition)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Joint3D>("Joint3D")
        .addConstructor <void (Scene*), void (Scene*, Entity)> ()
        .addFunction("setFixedJoint", &Joint3D::setFixedJoint)
        .addFunction("setDistanceJoint",
            luabridge::overload<Entity, Entity>(&Joint3D::setDistanceJoint),
            luabridge::overload<Entity, Entity, Vector3, Vector3>(&Joint3D::setDistanceJoint))
        .addFunction("setPointJoint", &Joint3D::setPointJoint)
        .addFunction("setHingeJoint", &Joint3D::setHingeJoint)
        .addFunction("setConeJoint", &Joint3D::setConeJoint)
        .addFunction("setPrismaticJoint", &Joint3D::setPrismaticJoint)
        .addFunction("setSwingTwistJoint", &Joint3D::setSwingTwistJoint)
        .addFunction("setSixDOFJoint", &Joint3D::setSixDOFJoint)
        .addFunction("setPathJoint", &Joint3D::setPathJoint)
        .addFunction("setGearJoint", &Joint3D::setGearJoint)
        .addFunction("setRackAndPinionJoint", &Joint3D::setRackAndPinionJoint)
        .addFunction("setPulleyJoint", &Joint3D::setPulleyJoint)
        .addFunction("getType", &Joint3D::getType)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Contact3D>("Contact3D")
        .addFunction("getBaseOffset", &Contact3D::getBaseOffset)
        .addFunction("getWorldSpaceNormal", &Contact3D::getWorldSpaceNormal)
        .addFunction("getPenetrationDepth", &Contact3D::getPenetrationDepth)
        .addFunction("getColliderIndex1", &Contact3D::getColliderIndex1)
        .addFunction("getColliderIndex2", &Contact3D::getColliderIndex2)
        .addFunction("getRelativeContactPointsOnA", &Contact3D::getRelativeContactPointsOnA)
        .addFunction("getRelativeContactPointsOnB", &Contact3D::getRelativeContactPointsOnB)
        .addProperty("combinedFriction", &Contact3D::getCombinedFriction, &Contact3D::setCombinedFriction)
        .addProperty("combinedRestitution", &Contact3D::getCombinedRestitution, &Contact3D::setCombinedRestitution)
        .addProperty("sensor", &Contact3D::isSensor, &Contact3D::setIsSensor)
        // the points of getRelativeContactPointsOnA / OnB
        .addProperty("numContactPoints", &Contact3D::getNumContactPoints)
        // delivered after the step (setters do nothing then)
        .addProperty("snapshot", &Contact3D::isSnapshot)
        .endClass();

    // physics-contacts: what onCollisionEnter / onTriggerEnter... get
    luabridge::getGlobalNamespace(L)
        .beginClass<ColliderRef>("Collider")
        .addProperty("gameObject", [] (const ColliderRef* self) -> std::optional<GameObject> { return luaColliderGameObject(*self); })
        .addProperty("rigidbody", [] (const ColliderRef* self) -> std::optional<Rigidbody> { return self->hasRigidbody() ? std::optional<Rigidbody>(self->getRigidbody()) : std::nullopt; })
        .addProperty("attachedRigidbody", [] (const ColliderRef* self) -> std::optional<Rigidbody> { return self->hasRigidbody() ? std::optional<Rigidbody>(self->getRigidbody()) : std::nullopt; })
        .addProperty("index", &ColliderRef::getIndex)
        .addProperty("type", &ColliderRef::getType)
        .addProperty("isTrigger", &ColliderRef::isTrigger)
        .addProperty("tag", &ColliderRef::getTag)
        .addFunction("compareTag", &ColliderRef::compareTag)
        .addProperty("receiver", [] (const ColliderRef* self) -> std::optional<GameObject> { return luaColliderReceiver(*self); })
        .addFunction("isValid", &ColliderRef::isValid)
        .addFunction("__eq", [] (const ColliderRef* self, const ColliderRef* other) { return other && *self == *other; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ContactPoint>("ContactPoint")
        .addProperty("point", &ContactPoint::getPoint)
        .addProperty("normal", &ContactPoint::getNormal)
        .addProperty("separation", &ContactPoint::getSeparation)
        .addProperty("impulse", &ContactPoint::getImpulse)
        .addProperty("thisCollider", &ContactPoint::getThisCollider)
        .addProperty("otherCollider", &ContactPoint::getOtherCollider)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Collision>("Collision")
        .addProperty("gameObject", [] (const Collision* self) -> std::optional<GameObject> { return luaColliderGameObject(self->otherCollider); })
        .addProperty("collider", &Collision::getCollider)
        .addProperty("rigidbody", [] (const Collision* self) -> std::optional<Rigidbody> { return self->otherCollider.hasRigidbody() ? std::optional<Rigidbody>(self->getRigidbody()) : std::nullopt; })
        .addProperty("thisCollider", &Collision::getThisCollider)
        .addProperty("thisRigidbody", [] (const Collision* self) -> std::optional<Rigidbody> { return self->thisCollider.hasRigidbody() ? std::optional<Rigidbody>(self->getThisRigidbody()) : std::nullopt; })
        .addProperty("impulse", &Collision::getImpulse)
        .addProperty("relativeVelocity", &Collision::getRelativeVelocity)
        .addProperty("contactCount", &Collision::getContactCount)
        .addFunction("getContact", &luaGetContact<Collision>)
        .addProperty("contacts", [] (const Collision* self, lua_State* L) -> luabridge::LuaRef { return luaContactsTable(self, L); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<CollideShapeResult3D>("CollideShapeResult3D")
        .addFunction("getContactPointOnA", &CollideShapeResult3D::getContactPointOnA)
        .addFunction("getContactPointOnB", &CollideShapeResult3D::getContactPointOnB)
        .addFunction("getPenetrationAxis", &CollideShapeResult3D::getPenetrationAxis)
        .addFunction("getColliderIndex1", &CollideShapeResult3D::getColliderIndex1)
        .addFunction("getColliderIndex2", &CollideShapeResult3D::getColliderIndex2)
        .endClass();
#endif

#endif //DISABLE_LUA_BINDINGS
}
