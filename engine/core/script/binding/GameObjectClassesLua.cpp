// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "EntityHandle.h"
#include "Fog.h"
#include "ReflectionProbe.h"
#include "Skybox.h"
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
#include "Tilemap.h"
#include "Bone.h"
#include "Model.h"
#include "MeshPolygon.h"
#include "Points.h"
#include "LineRenderer.h"
#include "Sprite.h"
#include "Text.h"
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
#ifdef JASPERENGINE_PHYSICS_2D
#include "Rigidbody2D.h"
#include "Joint2D.h"
#include "Contact2D.h"
#include "Manifold2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "Rigidbody.h"
#include "Joint3D.h"
#include "Contact3D.h"
#include "CollideShapeResult3D.h"
#endif

using namespace jasperengine;

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

void LuaBinding::registerGameObjectClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

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
#endif

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
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<GameObject, EntityHandle>("GameObject")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addStaticFunction("instantiate", &luaInstantiate)
        .addStaticFunction("destroy", &luaDestroy)
        .addStaticFunction("destroyImmediate", &luaDestroyImmediate)
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
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        .addFunction("getRigidbody", &GameObject::getRigidbody)
        .addFunction("removeRigidbody", &GameObject::removeRigidbody)
#endif
        .addFunction("getRay", &GameObject::getRay)
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
        .deriveClass<Mesh, GameObject>("Mesh")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Mesh::isEnabled, &Mesh::setEnabled)
        .addFunction("setTexture", 
            luabridge::overload<const std::string&>(&Mesh::setTexture),
            luabridge::overload<const std::string&,TextureData>(&Mesh::setTexture),
            luabridge::overload<Framebuffer*>(&Mesh::setTexture))
        .addProperty("color", &Mesh::getColor, (void(Mesh::*)(Vector4))&Mesh::setColor)
        .addFunction("setColor", 
            luabridge::overload<const float, const float, const float>(&Mesh::setColor),
            luabridge::overload<const float, const float, const float, const float>(&Mesh::setColor),
            luabridge::overload<Vector4>(&Mesh::setColor))
        .addProperty("alpha", &Mesh::getAlpha, &Mesh::setAlpha)
        .addProperty("material", (Material(Mesh::*)()const)&Mesh::getMaterial, (void(Mesh::*)(const Material&))&Mesh::setMaterial)
        .addFunction("setMaterial", 
            luabridge::overload<const Material&>(&Mesh::setMaterial),
            luabridge::overload<unsigned int, const Material&>(&Mesh::setMaterial))
        .addFunction("getMaterial", 
            luabridge::overload<>(&Mesh::getMaterial),
            luabridge::overload<unsigned int>(&Mesh::getMaterial))
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
        .addFunction("applyForce", &Rigidbody2D::applyForce)
        .addFunction("applyForceToCenter", &Rigidbody2D::applyForceToCenter)
        .addFunction("applyTorque", &Rigidbody2D::applyTorque)
        .addFunction("applyLinearImpulse", &Rigidbody2D::applyLinearImpulse)
        .addFunction("applyLinearImpulseToCenter", &Rigidbody2D::applyLinearImpulseToCenter)
        .addFunction("applyAngularImpulse", &Rigidbody2D::applyAngularImpulse)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Joint2D>("Joint2D")
        .addConstructor <void (Scene*), void (Scene*, Entity)> ()
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
