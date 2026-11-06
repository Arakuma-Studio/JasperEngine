// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"
#include "object/spline/Spline.h"
#include "object/spline/SplineAnimate.h"
#include "object/spline/SplineExtrude.h"
#include "object/spline/SplineInstantiate.h"
#include "component/AudioSnapshotComponent.h"
#include "component/ButtonComponent.h"
#include "component/InputFieldComponent.h"
#include "component/PanelComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/ActionComponent.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"
#include "LuaFunction.h"
#include "Scene.h"
#include "subsystem/RenderSystem.h"

#include "EntityHandle.h"
#include "Fog.h"
#include "LocalVolumetricFog.h"
#include "VolumetricClouds.h"
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
#include "CapsuleShadowCaster.h"
#include "Mesh.h"
#include "Primitive.h"
#include "Mirror.h"
#include "Lattice.h"
#include "Tilemap.h"
#include "Bone.h"
#include "Model.h"
#include "PoseAnimator.h"
#include "AnimationPlayer.h"
#include <cstdio>

#include "Log.h"

#include "VertexAnimator.h"
#include "Animator.h"
#include "CameraShaker.h"
#include "CameraFollow.h"
#include "camera/CameraShake.h"
#include "camera/ShakePreset.h"
#include "VideoPlayer.h"
#include "animation/AnimationEvent.h"
#include "MeshPolygon.h"
#include "Points.h"
#include "ParticleSystem.h"
#include "LineRenderer.h"
#include "TrailRenderer.h"
#include "Sprite.h"
#include "script/LuaJson.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "object/ui/Text.h"   // by path: lexbor has a dom/interfaces/text.h (Xcode header maps ignore case)
#include "object/ui/TextEffect.h"
#include "Image.h"
#include "Button.h"
#include "Panel.h"
#include "Scrollbar.h"
#include "Progressbar.h"
#include "InputField.h"
#include "object/ui/OnScreenControls.h"

#include <algorithm>
#include <cmath>
#include "LayoutGroup.h"
#include "object/ui/CanvasGroup.h"
#include "AudioSource.h"
#include "Engine.h"
#include "ObjectManager.h"
#include "PrefabAsset.h"
#include "physics/PhysicsMaterial.h"
#include "component/PhysicsCommon.h"
#include <optional>
#include "WaterVolume.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "Rigidbody2D.h"
#include "CharacterController2D.h"
#include "Joint2D.h"
#include "Contact2D.h"
#include "Manifold2D.h"
#include "Collision2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "Rigidbody.h"
#include "Buoyancy.h"
#include "CharacterController.h"
#include "ControllerColliderHit.h"
#include "Joint3D.h"
#include "Ragdoll.h"
#include "Contact3D.h"
#include "CollideShapeResult3D.h"
#include "Collision.h"
#include <stdexcept>
#endif

using namespace jasperengine;

namespace{

    // upstream's state info (Animator:getCurrentStateInfo, upstream's name) from S21's
    jasperengine::ControllerStateInfo toControllerStateInfo(const jasperengine::AnimatorStateInfo& info){
        jasperengine::ControllerStateInfo out;
        out.isValid = info.valid;
        out.name = info.name;
        out.fullName = info.fullPath;
        out.normalizedTime = info.normalizedTime;
        out.length = info.length;
        out.speed = info.speed;
        out.loop = info.loop;
        return out;
    }

}

// The Lua SpriteSheet (docs/specs/S44-aseprite-import.md R38, R51): a shared immutable sheet and
// the path it was loaded from; frames, tags and slices as plain tables (0-based frame indices)
namespace {

    struct LuaSpriteSheet {
        std::string path;
        std::shared_ptr<const SpriteSheetData> data;
    };

    luabridge::LuaRef luaSpriteSheetJson(lua_State* L, const nlohmann::json& value){
        if (value.is_null()){
            return luabridge::LuaRef(L);
        }
        LuaJson::push(L, value);
        return luabridge::LuaRef::fromStack(L);
    }

    luabridge::LuaRef luaSpriteSheetFrame(lua_State* L, const SpriteSheetData& sheet, int index){
        if (index < 0 || index >= sheet.frameCount()){
            return luabridge::LuaRef(L);
        }
        const SpriteFrame& f = sheet.frames[(size_t)index];
        luabridge::LuaRef t = luabridge::newTable(L);
        t["name"] = f.name;
        t["index"] = index;
        t["page"] = (int)f.page;
        t["rect"] = f.rect;
        t["sourceSize"] = f.sourceSize;
        t["offset"] = f.offset;
        t["pivot"] = f.pivot;
        t["border"] = f.border;
        t["durationMs"] = (int)f.durationMs;
        return t;
    }

    luabridge::LuaRef luaSpriteSheetTag(lua_State* L, const SpriteSheetData& sheet, const SpriteTag& tag){
        luabridge::LuaRef t = luabridge::newTable(L);
        t["name"] = tag.name;
        t["from"] = tag.from;
        t["to"] = tag.to;
        t["direction"] = std::string(spriteTagDirectionName(tag.direction));
        t["repeat"] = tag.repeat;
        // one pass (repeat 0) or the whole playback
        int ms = 0;
        for (int f : tag.playback()){
            if (f >= 0 && f < sheet.frameCount()) ms += (int)sheet.frames[(size_t)f].durationMs;
        }
        t["durationMs"] = ms;
        t["color"] = tag.color;
        t["userData"] = luaSpriteSheetJson(L, tag.userData);
        return t;
    }

    // a slice on a frame in the sheet's pixels (y down)
    luabridge::LuaRef luaSpriteSheetSlicePixels(lua_State* L, const SpriteSlice& slice, int frame){
        luabridge::LuaRef t = luabridge::newTable(L);
        t["name"] = slice.name;
        const SpriteSliceKey* key = slice.keyAt(frame);
        t["visible"] = key != nullptr;
        if (key){
            t["rect"] = key->bounds;
            if (key->hasCenter) t["center"] = key->center;
            if (key->hasPivot) t["pivot"] = key->pivot;
        }
        t["userData"] = luaSpriteSheetJson(L, slice.userData);
        return t;
    }

    // a slice on the sprite's shown frame in its local units
    luabridge::LuaRef luaSpriteSlice(lua_State* L, const Sprite& sprite, const SpriteSheetData& sheet, const SpriteSlice& slice){
        luabridge::LuaRef t = luabridge::newTable(L);
        t["name"] = slice.name;
        Rect bounds, center;
        Vector2 pivot;
        const bool visible = sprite.getSliceRect(slice.name, bounds, &center, &pivot);
        t["visible"] = visible;
        if (visible){
            t["rect"] = bounds;
            const int frame = std::clamp(sprite.getFrameIndex(), 0, std::max(0, sheet.frameCount() - 1));
            const SpriteSliceKey* key = slice.keyAt(frame);
            if (key && key->hasCenter) t["center"] = center;
            t["pivot"] = pivot;
        }
        t["userData"] = luaSpriteSheetJson(L, slice.userData);
        return t;
    }

    luabridge::LuaRef luaSpriteSheetList(lua_State* L, const std::vector<std::string>& names){
        luabridge::LuaRef t = luabridge::newTable(L);
        for (size_t i = 0; i < names.size(); i++) t[(int)i + 1] = names[i];
        return t;
    }

}

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

// AudioSource (audio-mixer spec 8): the calls with option tables read the Lua stack themselves
namespace {

    AudioSource* luaAudioSelf(lua_State* L){
        AudioSource* self = luabridge::detail::Userdata::get<AudioSource>(L, 1, false);
        if (!self) luaL_error(L, "AudioSource: call with ':' on an AudioSource");
        return self;
    }

    bool luaAudioOpt(lua_State* L, int index, const char* field){
        if (!lua_istable(L, index)) return false;
        lua_getfield(L, index, field);
        const bool value = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return value;
    }

    // sound:playOneShot(path [, volumeScale])
    int luaAudioPlayOneShot(lua_State* L){
        AudioSource* self = luaAudioSelf(L);
        self->playOneShot(luaL_checkstring(L, 2), (float)luaL_optnumber(L, 3, 1.0));
        return 0;
    }

    // sound:fadeIn(seconds [, opts])
    int luaAudioFadeIn(lua_State* L){
        AudioSource* self = luaAudioSelf(L);
        self->fadeIn(luaL_checknumber(L, 2), luaAudioOpt(L, 3, "unscaled"));
        return 0;
    }

    // sound:fadeOut(seconds [, opts]): opts.pause pauses instead of stopping
    int luaAudioFadeOut(lua_State* L){
        AudioSource* self = luaAudioSelf(L);
        self->fadeOut(luaL_checknumber(L, 2), luaAudioOpt(L, 3, "pause"), luaAudioOpt(L, 3, "unscaled"));
        return 0;
    }

    // sound:fadeTo(volume, seconds [, opts])
    int luaAudioFadeTo(lua_State* L){
        AudioSource* self = luaAudioSelf(L);
        self->fadeTo(luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaAudioOpt(L, 4, "unscaled"));
        return 0;
    }

    // AudioSource.playClipAtPoint(scene, path, position [, volume [, group]])
    int luaAudioPlayClipAtPoint(lua_State* L){
        Scene* scene = nullptr;
        if (luabridge::isInstance<Scene*>(L, 1)){
            auto result = luabridge::get<Scene*>(L, 1);
            if (result) scene = result.value();
        }
        if (!scene) return luaL_error(L, "AudioSource.playClipAtPoint: argument 1 must be a Scene");
        const std::string path = luaL_checkstring(L, 2);
        auto position = luabridge::get<Vector3>(L, 3);
        if (!position) return luaL_error(L, "AudioSource.playClipAtPoint: argument 3 must be a Vector3");
        const float volume = (float)luaL_optnumber(L, 4, 1.0);
        const std::string group = lua_isstring(L, 5) ? lua_tostring(L, 5) : "";
        AudioSource::playClipAtPoint(scene, path, position.value(), volume, group);
        return 0;
    }

}

namespace {
    // a binding's global argument: a name, or nil to unbind (anything else is an error)
    std::string luaBindingGlobal(const luabridge::LuaRef& global, const char* function){
        if (global.isNil())
            return std::string();
        if (global.isString())
            return global.unsafe_cast<std::string>();
        throw std::runtime_error(std::string(function) + ": the global is a name (a string), or nil to unbind");
    }


bool luaObjectArg(lua_State* L, int index, Scene*& scene, Entity& entity, std::string* scriptClass);

// An entity field of a component shown to Lua (the Video Player's targets): a GameObject, nil
// for none
std::optional<GameObject> gameObjectOrNil(Scene* scene, Entity entity){
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity))
        return std::nullopt;
    return GameObject(scene, entity);
}

// ... set from a GameObject (any handle, a script instance), an entity number or nil
Entity entityOf(const luabridge::LuaRef& value, const char* field){
    if (value.isNil())
        return NULL_ENTITY;
    if (value.isNumber())
        return (Entity)value.unsafe_cast<lua_Integer>();
    lua_State* L = value.state();
    value.push(L);
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    const bool ok = luaObjectArg(L, -1, scene, entity, nullptr);
    lua_pop(L, 1);
    if (!ok){
        Log::error("VideoPlayer.%s takes a GameObject or nil", field);
        return NULL_ENTITY;
    }
    return entity;
}

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
// LineRenderer / TrailRenderer :setPosition(index, position) and :getPosition(index): 0-based
// like Unity; an index out of range raises an error naming the count
template<typename T>
int luaRendererSetPosition(lua_State* L, const char* className){
    auto self = luabridge::get<T*>(L, 1);
    if (!self || !*self){
        return luaL_error(L, "%s.setPosition: call it on a %s (renderer:setPosition(index, position))", className, className);
    }
    if (!luabridge::isInstance<Vector3>(L, 3)){
        // GameObject's setPosition(x, y[, z]) (the object's own position) keeps working
        if (lua_isnumber(L, 2) && lua_isnumber(L, 3)){
            if (lua_isnumber(L, 4)){
                (*self)->GameObject::setPosition((float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4));
            }else{
                (*self)->GameObject::setPosition((float)lua_tonumber(L, 2), (float)lua_tonumber(L, 3));
            }
            return 0;
        }
        return luaL_error(L, "%s.setPosition: expected (index, Vector3), or the GameObject's (x, y[, z])", className);
    }
    if (!lua_isinteger(L, 2)){
        return luaL_error(L, "%s.setPosition: the index must be an integer (0-based)", className);
    }
    const lua_Integer index = lua_tointeger(L, 2);
    auto position = luabridge::get<Vector3>(L, 3);
    const int count = (*self)->getPositionCount();
    if (index < 0 || index >= count || !position){
        return luaL_error(L, "%s.setPosition: index %d is out of range: positionCount is %d (indices 0..%d)",
                          className, (int)index, count, count - 1);
    }
    (*self)->setPosition((int)index, *position);
    return 0;
}

template<typename T>
int luaRendererGetPosition(lua_State* L, const char* className){
    auto self = luabridge::get<T*>(L, 1);
    if (!self || !*self){
        return luaL_error(L, "%s.getPosition: call it on a %s (renderer:getPosition(index))", className, className);
    }
    if (!lua_isinteger(L, 2)){
        return luaL_error(L, "%s.getPosition: expected (index)", className);
    }
    const lua_Integer index = lua_tointeger(L, 2);
    const int count = (*self)->getPositionCount();
    if (index < 0 || index >= count){
        return luaL_error(L, "%s.getPosition: index %d is out of range: positionCount is %d (indices 0..%d)",
                          className, (int)index, count, count - 1);
    }
    if (!luabridge::push(L, (*self)->getPosition((int)index))){
        return luaL_error(L, "%s.getPosition: out of Lua stack", className);
    }
    return 1;
}

int luaLineSetPosition(lua_State* L){ return luaRendererSetPosition<LineRenderer>(L, "LineRenderer"); }
int luaLineGetPosition(lua_State* L){ return luaRendererGetPosition<LineRenderer>(L, "LineRenderer"); }
int luaTrailSetPosition(lua_State* L){ return luaRendererSetPosition<TrailRenderer>(L, "TrailRenderer"); }
int luaTrailGetPosition(lua_State* L){ return luaRendererGetPosition<TrailRenderer>(L, "TrailRenderer"); }

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

#if defined(JASPERENGINE_PHYSICS_2D) || defined(JASPERENGINE_PHYSICS_3D)
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
#endif

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

#ifdef JASPERENGINE_PHYSICS_3D
namespace {
    // a 3D joint body argument: a GameObject (or any handle, a script) with a Rigidbody, or nil
    Entity luaJoint3DBodyArg(Joint3D* self, const luabridge::LuaRef& value, lua_State* L, const char* property){
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
            throw std::runtime_error(std::string("Joint3D.") + property + ": a GameObject or Rigidbody with a Rigidbody, or nil");
        }
        if (scene != self->getScene() || !scene->isEntityCreated(entity) || !scene->findComponent<RigidbodyComponent>(entity)){
            throw std::runtime_error(std::string("Joint3D.") + property + ": \"" + (scene ? scene->getEntityName(entity) : std::string("?")) +
                                     "\" has no Rigidbody (in the joint's scene)");
        }
        return entity;
    }

    std::optional<GameObject> luaJoint3DBody(const Joint3D* self, Entity body){
        Scene* scene = self->getScene();
        if (body == NULL_ENTITY || !scene || !scene->isEntityCreated(body)){
            return std::nullopt;
        }
        return GameObject(scene, body);
    }

    // a Ragdoll entity argument: a GameObject (or handle) or nil
    Entity luaEntityArg(Scene* expected, const luabridge::LuaRef& value, lua_State* L, const char* what){
        if (value.isNil()){
            return NULL_ENTITY;
        }
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        const int top = lua_gettop(L);
        value.push(L);
        const bool isObject = luaObjectArg(L, lua_gettop(L), scene, entity);
        lua_settop(L, top);
        if (!isObject || scene != expected){
            throw std::runtime_error(std::string(what) + ": a GameObject of the same scene, or nil");
        }
        return entity;
    }
}
#endif

namespace {
    // the shader a component draws with: its customShader, else the scene's default
    template <class T>
    std::string componentShader(const T* self, ShaderType type){
        const std::string shader = self->getCustomShader();
        Scene* scene = self->getScene();
        return (shader.empty() && scene) ? scene->getDefaultCustomShader(type) : shader;
    }
}

// ---- Camera Shake (camera shake spec 8): preset and options arguments ----

namespace {

// The error of a camera shake argument, formatted for luaL_error once the C++ objects are gone
struct ShakeArgError{
    char text[320] = {0};

    bool set(const char* function, const char* format, const char* detail = ""){
        char what[256];
        std::snprintf(what, sizeof(what), format, detail);
        std::snprintf(text, sizeof(text), "%s: %s", function, what);
        return false;
    }
};

// a number field of the table at `index`: absent (nil) leaves `out` and returns true; another
// type is an error
bool luaShakeNumberField(lua_State* L, int index, const char* key, float& out, const char* function, ShakeArgError& error){
    lua_getfield(L, index, key);
    const int type = lua_type(L, -1);
    bool ok = true;
    if (type == LUA_TNUMBER){
        out = (float)lua_tonumber(L, -1);
    }else if (type != LUA_TNIL){
        ok = error.set(function, "%s must be a number", key);
    }
    lua_pop(L, 1);
    return ok;
}

bool luaShakeVectorField(lua_State* L, int index, const char* key, Vector3& out, const char* function, ShakeArgError& error){
    lua_getfield(L, index, key);
    bool ok = true;
    if (!lua_isnil(L, -1) && !luaValueArg<Vector3>(L, -1, out)){
        ok = error.set(function, "%s must be a Vector3", key);
    }
    lua_pop(L, 1);
    return ok;
}

// preset: a built-in name or a .shake path, or a table {type = "one_shot"|"sustained", strength,
// roughness, fadeIn, fadeOut, positionInfluence = Vector3, rotationInfluence = Vector3} whose
// missing keys take the .shake defaults
bool luaShakeSourceArg(lua_State* L, int index, ShakeSource& out, const char* function, ShakeArgError& error){
    index = lua_absindex(L, index);
    if (lua_type(L, index) == LUA_TSTRING){
        out = ShakeSource(std::string(lua_tostring(L, index)));
        return true;
    }
    if (!lua_istable(L, index)){
        return error.set(function, "the preset must be a built-in name, a .shake path or a table, got %s", luaL_typename(L, index));
    }
    camera::ShakePreset preset;
    lua_getfield(L, index, "type");
    if (lua_type(L, -1) == LUA_TSTRING){
        if (!camera::shakeTypeFromName(lua_tostring(L, -1), preset.type)){
            lua_pop(L, 1);
            return error.set(function, "preset type must be \"one_shot\" or \"sustained\"");
        }
    }else if (!lua_isnil(L, -1)){
        lua_pop(L, 1);
        return error.set(function, "preset type must be \"one_shot\" or \"sustained\"");
    }
    lua_pop(L, 1);
    if (!luaShakeNumberField(L, index, "strength", preset.strength, function, error) ||
        !luaShakeNumberField(L, index, "roughness", preset.roughness, function, error) ||
        !luaShakeNumberField(L, index, "fadeIn", preset.fadeIn, function, error) ||
        !luaShakeNumberField(L, index, "fadeOut", preset.fadeOut, function, error) ||
        !luaShakeVectorField(L, index, "positionInfluence", preset.positionInfluence, function, error) ||
        !luaShakeVectorField(L, index, "rotationInfluence", preset.rotationInfluence, function, error)){
        return false;
    }
    out = ShakeSource(preset);
    return true;
}

// options: nil or {strength, roughness, seed, unscaled, fadeIn, fadeOut}
bool luaShakeOptionsArg(lua_State* L, int index, ShakeOptions& out, const char* function, ShakeArgError& error){
    index = lua_absindex(L, index);
    if (lua_isnoneornil(L, index)) return true;
    if (!lua_istable(L, index)){
        return error.set(function, "options must be a table, got %s", luaL_typename(L, index));
    }
    if (!luaShakeNumberField(L, index, "strength", out.strength, function, error) ||
        !luaShakeNumberField(L, index, "roughness", out.roughness, function, error) ||
        !luaShakeNumberField(L, index, "fadeIn", out.fadeIn, function, error) ||
        !luaShakeNumberField(L, index, "fadeOut", out.fadeOut, function, error)){
        return false;
    }
    lua_getfield(L, index, "seed");
    if (lua_isinteger(L, -1)){
        out.hasSeed = true;
        out.seed = (int)lua_tointeger(L, -1);
    }else if (!lua_isnil(L, -1)){
        lua_pop(L, 1);
        return error.set(function, "seed must be an integer");
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "unscaled");
    if (lua_isboolean(L, -1)){
        out.unscaled = lua_toboolean(L, -1) ? 1 : 0;
    }else if (!lua_isnil(L, -1)){
        lua_pop(L, 1);
        return error.set(function, "unscaled must be a boolean");
    }
    lua_pop(L, 1);
    return true;
}

void luaPushShakeInstance(lua_State* L, const ShakeInstanceHandle& handle){
    if (!luabridge::push(L, handle)){
        lua_pushnil(L);
    }
}

// shake(preset [, options]) / shakeAt(position, radius, preset [, options]), on every global
// shaker (`shaker` null) or on one; `first` is the stack index of the first argument
int luaShakeCall(lua_State* L, CameraShaker* shaker, int first, bool fromPoint, const char* function){
    ShakeArgError error;
    const bool ok = [&]() -> bool {
        Vector3 position;
        float radius = 0.0f;
        int next = first;
        if (fromPoint){
            if (!luaValueArg<Vector3>(L, next, position)){
                return error.set(function, "position must be a Vector3");
            }
            if (lua_type(L, next + 1) != LUA_TNUMBER){
                return error.set(function, "radius must be a number");
            }
            radius = (float)lua_tonumber(L, next + 1);
            next += 2;
        }
        ShakeSource source;
        ShakeOptions options;
        if (!luaShakeSourceArg(L, next, source, function, error) || !luaShakeOptionsArg(L, next + 1, options, function, error)){
            return false;
        }
        ShakeInstanceHandle handle;
        if (shaker){
            handle = fromPoint ? shaker->shakeAt(position, radius, source, options) : shaker->shake(source, options);
        }else{
            handle = fromPoint ? CameraShake::shakeAt(position, radius, source, options) : CameraShake::shake(source, options);
        }
        luaPushShakeInstance(L, handle);
        return true;
    }();
    if (!ok){
        return luaL_error(L, "%s", error.text);
    }
    return 1;
}

// the CameraShaker of a method call (argument 1), null when it is not one
CameraShaker* luaShakerSelf(lua_State* L){
    if (!luabridge::isInstance<CameraShaker>(L, 1)) return nullptr;
    auto self = luabridge::get<CameraShaker*>(L, 1);
    return self ? self.value() : nullptr;
}

int luaCameraShakeShake(lua_State* L){
    return luaShakeCall(L, nullptr, 1, false, "CameraShake.shake");
}

int luaCameraShakeShakeAt(lua_State* L){
    return luaShakeCall(L, nullptr, 1, true, "CameraShake.shakeAt");
}

int luaCameraShakerShake(lua_State* L){
    CameraShaker* self = luaShakerSelf(L);
    if (!self) return luaL_error(L, "CameraShaker.shake: call it with a colon on a CameraShaker (shaker:shake(preset))");
    return luaShakeCall(L, self, 2, false, "CameraShaker.shake");
}

int luaCameraShakerShakeAt(lua_State* L){
    CameraShaker* self = luaShakerSelf(L);
    if (!self) return luaL_error(L, "CameraShaker.shakeAt: call it with a colon on a CameraShaker (shaker:shakeAt(position, radius, preset))");
    return luaShakeCall(L, self, 2, true, "CameraShaker.shakeAt");
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

    // what a Light's intensity is in (util/LightPhotometry.h)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("LightUnit")
        .addVariable("ENGINE", LightUnit::ENGINE)
        .addVariable("LUX", LightUnit::LUX)
        .addVariable("CANDELA", LightUnit::CANDELA)
        .addVariable("LUMEN", LightUnit::LUMEN)
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
        .beginNamespace("RigidbodyInterpolation")
        .addVariable("SCENE", RigidbodyInterpolation::SCENE)
        .addVariable("NONE", RigidbodyInterpolation::NONE)
        .addVariable("INTERPOLATE", RigidbodyInterpolation::INTERPOLATE)
        .addVariable("EXTRAPOLATE", RigidbodyInterpolation::EXTRAPOLATE)
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

    // a SIXDOF axis (Unity ConfigurableJointMotion)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("JointMotion")
        .addVariable("LOCKED", JointMotion::LOCKED)
        .addVariable("LIMITED", JointMotion::LIMITED)
        .addVariable("FREE", JointMotion::FREE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SwingType")
        .addVariable("CONE", JointSwingType::CONE)
        .addVariable("PYRAMID", JointSwingType::PYRAMID)
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
        .addProperty("colorMode", &Fog::getColorMode, &Fog::setColorMode)
        .addProperty("attenuationDistance", &Fog::getAttenuationDistance, &Fog::setAttenuationDistance)
        .addProperty("baseHeight", &Fog::getBaseHeight, &Fog::setBaseHeight)
        .addProperty("maximumHeight", &Fog::getMaximumHeight, &Fog::setMaximumHeight)
        .addProperty("startDistance", &Fog::getStartDistance, &Fog::setStartDistance)
        .addProperty("maxFogDistance", &Fog::getMaxFogDistance, &Fog::setMaxFogDistance)
        .addProperty("maxOpacity", &Fog::getMaxOpacity, &Fog::setMaxOpacity)
        .addProperty("directionalInscattering", &Fog::getDirectionalInscattering, &Fog::setDirectionalInscattering)
        .addProperty("anisotropy", &Fog::getAnisotropy, &Fog::setAnisotropy)
        .addProperty("skyFill", &Fog::isSkyFill, &Fog::setSkyFill)
        .addProperty("skyFillAngle", &Fog::getSkyFillAngle, &Fog::setSkyFillAngle)
        .addProperty("volumetric", &Fog::isVolumetric, &Fog::setVolumetric)
        .addProperty("volumetricDistance", &Fog::getVolumetricDistance, &Fog::setVolumetricDistance)
        .addProperty("lightShafts", &Fog::isLightShafts, &Fog::setLightShafts)
        .addProperty("noiseIntensity", &Fog::getNoiseIntensity, &Fog::setNoiseIntensity)
        .addProperty("noiseScale", &Fog::getNoiseScale, &Fog::setNoiseScale)
        .addProperty("wind", &Fog::getWind, (void(Fog::*)(Vector3))&Fog::setWind)
        .addFunction("setWind", (void(Fog::*)(float, float, float))&Fog::setWind)
        .addProperty("qualityOverride", &Fog::getQualityOverride, &Fog::setQualityOverride)
        .addFunction("applyPreset", &Fog::applyPreset)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<VolumetricClouds, EntityHandle>("VolumetricClouds")
        .addConstructor <void (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("preset", &VolumetricClouds::getPreset, &VolumetricClouds::setPreset)
        .addProperty("rendering", &VolumetricClouds::getRendering, &VolumetricClouds::setRendering)
        .addProperty("coverage", &VolumetricClouds::getCoverage, &VolumetricClouds::setCoverage)
        .addProperty("density", &VolumetricClouds::getDensity, &VolumetricClouds::setDensity)
        .addProperty("shapeScale", &VolumetricClouds::getShapeScale, &VolumetricClouds::setShapeScale)
        .addProperty("shapeFactor", &VolumetricClouds::getShapeFactor, &VolumetricClouds::setShapeFactor)
        .addProperty("erosionFactor", &VolumetricClouds::getErosionFactor, &VolumetricClouds::setErosionFactor)
        .addProperty("erosionScale", &VolumetricClouds::getErosionScale, &VolumetricClouds::setErosionScale)
        .addProperty("bottomAltitude", &VolumetricClouds::getBottomAltitude, &VolumetricClouds::setBottomAltitude)
        .addProperty("altitudeRange", &VolumetricClouds::getAltitudeRange, &VolumetricClouds::setAltitudeRange)
        .addProperty("earthCurvature", &VolumetricClouds::getEarthCurvature, &VolumetricClouds::setEarthCurvature)
        .addProperty("windDirection", &VolumetricClouds::getWindDirection, &VolumetricClouds::setWindDirection)
        .addProperty("windSpeed", &VolumetricClouds::getWindSpeed, &VolumetricClouds::setWindSpeed)
        .addProperty("weatherMap", &VolumetricClouds::getWeatherMap, &VolumetricClouds::setWeatherMap)
        .addProperty("weatherMapScale", &VolumetricClouds::getWeatherMapScale, &VolumetricClouds::setWeatherMapScale)
        .addProperty("sunLightDimmer", &VolumetricClouds::getSunLightDimmer, &VolumetricClouds::setSunLightDimmer)
        .addProperty("ambientLightDimmer", &VolumetricClouds::getAmbientLightDimmer, &VolumetricClouds::setAmbientLightDimmer)
        .addProperty("maxDistance", &VolumetricClouds::getMaxDistance, &VolumetricClouds::setMaxDistance)
        .addProperty("qualityOverride", &VolumetricClouds::getQualityOverride, &VolumetricClouds::setQualityOverride)
        .endClass();

    // the water surfaces (meshes drawn with library:water) at a point, with their waves
    luabridge::getGlobalNamespace(L)
        .beginClass<Water>("Water")
        .addStaticFunction("getHeight", &Water::getHeight)
        .addStaticFunction("getNormal", &Water::getNormal)
        .addStaticFunction("isUnderwater", &Water::isUnderwater)
        .addStaticFunction("getTime", &Water::getTime)
        .addStaticFunction("sample", &Water::sample)
        .addStaticFunction("sampleHeight", &Water::sampleHeight)
        .endClass();

    // what the water is at a point (Water.sample)
    luabridge::getGlobalNamespace(L)
        .beginClass<WaterSample>("WaterSample")
        .addProperty("inWater", &WaterSample::isInWater)
        .addProperty("surfaceHeight", &WaterSample::getSurfaceHeight)
        .addProperty("normal", &WaterSample::getNormal)
        .addProperty("flowVelocity", &WaterSample::getFlowVelocity)
        .addProperty("volume", &WaterSample::getVolume)
        .addProperty("hasWater", &WaterSample::hasWater)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("WaterVolumeWaves")
        .addVariable("FROM_MATERIAL", WaterVolumeWaves::FROM_MATERIAL)
        .addVariable("NONE", WaterVolumeWaves::NONE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<WaterVolume, EntityHandle>("WaterVolume")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &WaterVolume::isValid)
        .addProperty("enabled", &WaterVolume::isEnabled, &WaterVolume::setEnabled)
        .addProperty("size", &WaterVolume::getSize, &WaterVolume::setSize)
        .addProperty("surfaceOffset", &WaterVolume::getSurfaceOffset, &WaterVolume::setSurfaceOffset)
        .addProperty("infinite", &WaterVolume::isInfinite, &WaterVolume::setInfinite)
        .addProperty("density", &WaterVolume::getDensity, &WaterVolume::setDensity)
        .addProperty("flow", &WaterVolume::getFlow, &WaterVolume::setFlow)
        .addProperty("linearDrag", &WaterVolume::getLinearDrag, &WaterVolume::setLinearDrag)
        .addProperty("angularDrag", &WaterVolume::getAngularDrag, &WaterVolume::setAngularDrag)
        .addProperty("waves", &WaterVolume::getWaves, &WaterVolume::setWaves)
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
        // a custom-block member that follows a global shader value (Shader.setGlobal*); nil unbinds
        .addFunction("setShaderGlobalBinding", +[](Skybox* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setShaderGlobalBinding(member, luaBindingGlobal(global, "Skybox.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const Skybox* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getShaderGlobalBinding(member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
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
        // persistent objects (Unity's DontDestroyOnLoad, also a global function)
        .addStaticFunction("dontDestroyOnLoad", &LuaBinding::luaDontDestroyOnLoad)
        .addStaticFunction("isPersistent", &LuaBinding::luaIsPersistent)
        .addStaticFunction("persistSingleton", &LuaBinding::luaPersistSingleton)
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
        // the Mixer Snapshot action's component (S40 R24), nil without one
        .addFunction("getAudioSnapshotComponent", [] (GameObject* self) -> AudioSnapshotComponent* {
            return self->getScene()->findComponent<AudioSnapshotComponent>(self->getEntity());
        })
        .addFunction("getRigidbody2D", &GameObject::getRigidbody2D)
        .addFunction("removeRigidbody2D", &GameObject::removeRigidbody2D)
        .addFunction("getCharacterController2D", &GameObject::getCharacterController2D)
        .addFunction("removeCharacterController2D", &GameObject::removeCharacterController2D)
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
        .addFunction("getCharacterController", &GameObject::getCharacterController)
        .addFunction("removeCharacterController", &GameObject::removeCharacterController)
        .addFunction("getBuoyancy", &GameObject::getBuoyancy)
        // the Joint 3D of the GameObject, nil when it has none
        .addFunction("getJoint3D", +[](GameObject* self) -> std::optional<Joint3D> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<Joint3DComponent>(self->getEntity())){
                return std::nullopt;
            }
            return Joint3D(scene, self->getEntity());
        })
        // the Ragdoll of the GameObject, nil when it has none
        .addFunction("getRagdoll", +[](GameObject* self) -> std::optional<Ragdoll> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<RagdollComponent>(self->getEntity())){
                return std::nullopt;
            }
            return Ragdoll(scene, self->getEntity());
        })
#endif
        .addFunction("getWaterVolume", &GameObject::getWaterVolume)
        .addFunction("getSpline", &GameObject::getSpline)
        .addFunction("getSplineExtrude", &GameObject::getSplineExtrude)
        .addFunction("getSplineInstantiate", &GameObject::getSplineInstantiate)
        .addFunction("getSplineAnimate", &GameObject::getSplineAnimate)
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
        // the Animator of the GameObject; without one its Animation Player (upstream's
        // getAnimator, an alias of getAnimationPlayer: players with layers keep working); nil when
        // it has neither (Animator(scene, entity) adds one)
        .addFunction("getAnimator", +[](GameObject* self, lua_State* L) -> luabridge::LuaRef {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity())) return luabridge::LuaRef(L);
            if (scene->findComponent<AnimatorComponent>(self->getEntity())) return luabridge::LuaRef(L, Animator(scene, self->getEntity()));
            if (scene->findComponent<AnimationPlayerComponent>(self->getEntity())) return luabridge::LuaRef(L, AnimationPlayer(scene, self->getEntity()));
            return luabridge::LuaRef(L);
        })
        .addFunction("hasAnimator", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<AnimatorComponent>(self->getEntity()) != nullptr;
        })
        // the Trail Renderer of the GameObject, nil when it has none
        .addFunction("getTrailRenderer", +[](GameObject* self) -> std::optional<TrailRenderer> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<TrailRendererComponent>(self->getEntity())){
                return std::nullopt;
            }
            return TrailRenderer(scene, self->getEntity());
        })
        .addFunction("hasTrailRenderer", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<TrailRendererComponent>(self->getEntity()) != nullptr;
        })
        // Unity's GetComponent<ParticleSystem>: the GameObject's Particle System, nil when it has
        // none (an old controller action whose target has one: that target's)
        .addFunction("getParticleSystem", +[](GameObject* self) -> std::optional<ParticleSystem> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()))
                return std::nullopt;
            if (scene->findComponent<ParticleSystemComponent>(self->getEntity()))
                return ParticleSystem(scene, self->getEntity());
            const ActionComponent* action = scene->findComponent<ActionComponent>(self->getEntity());
            if (action && action->target != NULL_ENTITY && scene->isEntityCreated(action->target) &&
                scene->findComponent<ParticleSystemComponent>(action->target))
                return ParticleSystem(scene, action->target);
            return std::nullopt;
        })
        // Unity's GetComponentsInChildren<ParticleSystem>: its own and every one below it, in
        // hierarchy order (a 1-based table)
        .addFunction("getParticleSystemsInChildren", +[](GameObject* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef list = luabridge::newTable(L);
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()))
                return list;
            int n = 0;
            auto add = [&](Entity e){
                if (scene->findComponent<ParticleSystemComponent>(e)) list[++n] = ParticleSystem(scene, e);
            };
            add(self->getEntity());
            auto transforms = scene->getComponentArray<Transform>();
            if (transforms->hasEntity(self->getEntity())){
                const size_t start = transforms->getIndex(self->getEntity());
                const size_t last = scene->findBranchLastIndex(self->getEntity());
                for (size_t i = start + 1; i <= last && i < transforms->size(); i++) add(transforms->getEntity(i));
            }
            return list;
        })
        // Unity's AddComponent<TrailRenderer>(): its Trail Renderer, added when it has none
        .addFunction("addTrailRenderer", +[](GameObject* self) -> std::optional<TrailRenderer> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity())){
                return std::nullopt;
            }
            return TrailRenderer::addTo(scene, self->getEntity());
        })
        // the Canvas Group of the GameObject, nil when it has none (Unity's GetComponent)
        .addFunction("getCanvasGroup", +[](GameObject* self) -> std::optional<CanvasGroup> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<CanvasGroupComponent>(self->getEntity())){
                return std::nullopt;
            }
            return CanvasGroup(scene, self->getEntity());
        })
        .addFunction("hasCanvasGroup", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<CanvasGroupComponent>(self->getEntity()) != nullptr;
        })
        // adds one when the GameObject has none (Unity AddComponent<CanvasGroup>)
        .addFunction("addCanvasGroup", +[](GameObject* self) -> CanvasGroup {
            return CanvasGroup(self->getScene(), self->getEntity());
        })
        .addFunction("removeCanvasGroup", +[](GameObject* self) {
            Scene* scene = self->getScene();
            if (scene && scene->isEntityCreated(self->getEntity()) &&
                scene->findComponent<CanvasGroupComponent>(self->getEntity())){
                scene->removeComponent<CanvasGroupComponent>(self->getEntity());
            }
        })
        // the Camera Shaker of the GameObject (MilkShake's Shaker), nil when it has none
        .addFunction("getCameraShaker", +[](GameObject* self) -> std::optional<CameraShaker> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<CameraShakerComponent>(self->getEntity())){
                return std::nullopt;
            }
            return CameraShaker(scene, self->getEntity());
        })
        .addFunction("hasCameraShaker", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<CameraShakerComponent>(self->getEntity()) != nullptr;
        })
        // the Camera Follow of the GameObject, nil when it has none
        .addFunction("getCameraFollow", +[](GameObject* self) -> std::optional<CameraFollow> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<CameraFollowComponent>(self->getEntity())){
                return std::nullopt;
            }
            return CameraFollow(scene, self->getEntity());
        })
        .addFunction("hasCameraFollow", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<CameraFollowComponent>(self->getEntity()) != nullptr;
        })
        // the Video Player of the GameObject, nil when it has none (Unity's GetComponent<VideoPlayer>)
        .addFunction("getVideoPlayer", +[](GameObject* self) -> std::optional<VideoPlayer> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) ||
                !scene->findComponent<VideoPlayerComponent>(self->getEntity())){
                return std::nullopt;
            }
            return VideoPlayer(scene, self->getEntity());
        })
        .addFunction("hasVideoPlayer", +[](GameObject* self) -> bool {
            Scene* scene = self->getScene();
            return scene && scene->isEntityCreated(self->getEntity()) &&
                   scene->findComponent<VideoPlayerComponent>(self->getEntity()) != nullptr;
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
        .addProperty("transparencyMode", &Camera::getTransparencyMode, &Camera::setTransparencyMode)
        .addProperty("oitWeightScale", &Camera::getOitWeightScale, &Camera::setOitWeightScale)
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
        .addProperty("volumetricIntensity", &Light::getVolumetricIntensity, &Light::setVolumetricIntensity)
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
        // photometric units and colour temperature; the property sets the unit alone,
        // light:setIntensityUnit(unit, true) keeps the light as bright
        .addProperty("intensityUnit", &Light::getIntensityUnit, (void(Light::*)(LightUnit))&Light::setIntensityUnit)
        .addFunction("setIntensityUnit",
            luabridge::overload<LightUnit>((void(Light::*)(LightUnit))&Light::setIntensityUnit),
            luabridge::overload<LightUnit, bool>((void(Light::*)(LightUnit, bool))&Light::setIntensityUnit))
        .addProperty("useColorTemperature", &Light::isUseColorTemperature, &Light::setUseColorTemperature)
        .addProperty("colorTemperature", &Light::getColorTemperature, &Light::setColorTemperature)
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
        .addProperty("shadowType", &Light::getShadowType, &Light::setShadowType)
        .addProperty("shadowStrength", &Light::getShadowStrength, &Light::setShadowStrength)
        .addProperty("shadowBias", &Light::getBias, &Light::setBias)
        .addProperty("shadowNormalBias", &Light::getShadowNormalBias, &Light::setShadowNormalBias)
        .addProperty("shadowResolution", &Light::getShadowMapSize, &Light::setShadowMapSize)
        .addProperty("cascadeSplitMode", &Light::getCascadeSplitMode, &Light::setCascadeSplitMode)
        .addProperty("cascadeSplits", &Light::getCascadeSplits, &Light::setCascadeSplits)
        .addProperty("shadowDistance", &Light::getShadowDistance, &Light::setShadowDistance)
        .addProperty("shadowAngularDiameter", &Light::getShadowAngularDiameter, &Light::setShadowAngularDiameter)
        .addProperty("shadowSourceRadius", &Light::getShadowSourceRadius, &Light::setShadowSourceRadius)
        .addProperty("shadowUpdateMode", &Light::getShadowUpdateMode, &Light::setShadowUpdateMode)
        .addFunction("requestShadowUpdate", &Light::requestShadowUpdate)
        .addProperty("contactShadows", &Light::isContactShadows, &Light::setContactShadows)
        .addProperty("contactShadowLength", &Light::getContactShadowLength, &Light::setContactShadowLength)
        .addProperty("contactShadowThickness", &Light::getContactShadowThickness, &Light::setContactShadowThickness)
        .addProperty("contactShadowSampleCount", &Light::getContactShadowSampleCount, &Light::setContactShadowSampleCount)
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

    // S57's capsule shadows
    luabridge::getGlobalNamespace(L)
        .deriveClass<CapsuleShadowCaster, GameObject>("CapsuleShadowCaster")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &CapsuleShadowCaster::isEnabled, &CapsuleShadowCaster::setEnabled)
        .addProperty("castShadows", &CapsuleShadowCaster::getCastShadows, &CapsuleShadowCaster::setCastShadows)
        .addProperty("capsuleCount", &CapsuleShadowCaster::getCapsuleCount)
        .addFunction("addCapsule", &CapsuleShadowCaster::addCapsule)
        .addFunction("removeCapsule", &CapsuleShadowCaster::removeCapsule)
        .addFunction("clearCapsules", &CapsuleShadowCaster::clearCapsules)
        .addFunction("getCapsuleBone", &CapsuleShadowCaster::getCapsuleBone)
        .addFunction("setCapsuleBone", &CapsuleShadowCaster::setCapsuleBone)
        .addFunction("getCapsuleRadius", &CapsuleShadowCaster::getCapsuleRadius)
        .addFunction("setCapsuleRadius", &CapsuleShadowCaster::setCapsuleRadius)
        .addFunction("getCapsuleLength", &CapsuleShadowCaster::getCapsuleLength)
        .addFunction("setCapsuleLength", &CapsuleShadowCaster::setCapsuleLength)
        .addFunction("getCapsuleOffset", &CapsuleShadowCaster::getCapsuleOffset)
        .addFunction("setCapsuleOffset", &CapsuleShadowCaster::setCapsuleOffset)
        .addFunction("getCapsuleDirection", &CapsuleShadowCaster::getCapsuleDirection)
        .addFunction("setCapsuleDirection", &CapsuleShadowCaster::setCapsuleDirection)
        .addFunction("fitToSkeleton", &CapsuleShadowCaster::fitToSkeleton)
        .endClass();

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
        .deriveClass<LocalVolumetricFog, GameObject>("LocalVolumetricFog")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("shape", &LocalVolumetricFog::getShape, &LocalVolumetricFog::setShape)
        .addProperty("size", &LocalVolumetricFog::getSize, (void(LocalVolumetricFog::*)(Vector3))&LocalVolumetricFog::setSize)
        .addFunction("setSize", (void(LocalVolumetricFog::*)(const float, const float, const float))&LocalVolumetricFog::setSize)
        .addProperty("attenuationDistance", &LocalVolumetricFog::getAttenuationDistance, &LocalVolumetricFog::setAttenuationDistance)
        .addProperty("albedo", &LocalVolumetricFog::getAlbedo, (void(LocalVolumetricFog::*)(Vector3))&LocalVolumetricFog::setAlbedo)
        .addFunction("setAlbedo", (void(LocalVolumetricFog::*)(const float, const float, const float))&LocalVolumetricFog::setAlbedo)
        .addProperty("blendDistance", &LocalVolumetricFog::getBlendDistance, &LocalVolumetricFog::setBlendDistance)
        .addProperty("falloff", &LocalVolumetricFog::getFalloff, &LocalVolumetricFog::setFalloff)
        .addProperty("noiseIntensity", &LocalVolumetricFog::getNoiseIntensity, &LocalVolumetricFog::setNoiseIntensity)
        .addProperty("noiseScale", &LocalVolumetricFog::getNoiseScale, &LocalVolumetricFog::setNoiseScale)
        .addProperty("scrollSpeed", &LocalVolumetricFog::getScrollSpeed, (void(LocalVolumetricFog::*)(Vector3))&LocalVolumetricFog::setScrollSpeed)
        .addFunction("setScrollSpeed", (void(LocalVolumetricFog::*)(const float, const float, const float))&LocalVolumetricFog::setScrollSpeed)
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
        .addProperty("staticShadowCaster", &Mesh::isStaticShadowCaster, &Mesh::setStaticShadowCaster)
        .addProperty("receiveShadows", &Mesh::isReceiveShadows, &Mesh::setReceiveShadows)
        .addProperty("lodEnabled", &Mesh::isLodEnabled, &Mesh::setLodEnabled)
        .addProperty("lodBias", &Mesh::getLodBias, &Mesh::setLodBias)
        .addProperty("allowGpuInstancing", &Mesh::isAllowGpuInstancing, &Mesh::setAllowGpuInstancing)
        .addProperty("fadeMode", &Mesh::getLodFadeMode, &Mesh::setLodFadeMode)
        .addProperty("animateCrossFade", &Mesh::isLodAnimateCrossFading, &Mesh::setLodAnimateCrossFading)
        .addProperty("fadeTransitionWidth", &Mesh::getLodFadeTransitionWidth, &Mesh::setLodFadeTransitionWidth)
        .addProperty("cullBelow", &Mesh::getLodCullBelow, &Mesh::setLodCullBelow)
        .addFunction("forceLod", &Mesh::forceLod)
        .addProperty("forcedLod", &Mesh::getForcedLod)
        .addProperty("currentLod", &Mesh::getCurrentLod)
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
        // hex tiling of the flat projection (anti-repetition), and how far a tile may turn (0..1)
        .addProperty("hexTiling", &TerrainSurfaceLayer::hexTiling, &TerrainSurfaceLayer::hexTiling)
        .addProperty("hexRotation", &TerrainSurfaceLayer::hexRotation, &TerrainSurfaceLayer::hexRotation)
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
        .addFunction("getHumanoidInfo", +[](const Model* self, lua_State* L) -> luabridge::LuaRef {
            const ModelHumanoidInfo info = self->getHumanoidInfo();
            if (!info.valid) return luabridge::LuaRef(L);
            luabridge::LuaRef table = luabridge::newTable(L);
            table["humanoid"] = info.humanoid;
            table["animationType"] = info.animationType;
            table["humanScale"] = info.humanScale;
            luabridge::LuaRef missing = luabridge::newTable(L);
            for (size_t i = 0; i < info.missing.size(); i++) missing[(int)i + 1] = info.missing[i];
            table["missing"] = missing;
            table["extraMapped"] = info.extraMapped;
            if (!info.error.empty()) table["error"] = info.error;
            return table;
        })
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

    // the frame's particle events (main thread, after the simulation): arrays of plain tables
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const std::vector<ParticleCollisionInfo>&)>>("FunctionSubscribe_V_ParticleCollisions")
        .addFunction("add", (bool (FunctionSubscribe<void(const std::vector<ParticleCollisionInfo>&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::vector<ParticleCollisionInfo>&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(const std::vector<ParticleCollisionInfo>&)>::remove)
        .endClass();
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const std::vector<ParticleDeathInfo>&)>>("FunctionSubscribe_V_ParticleDeaths")
        .addFunction("add", (bool (FunctionSubscribe<void(const std::vector<ParticleDeathInfo>&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::vector<ParticleDeathInfo>&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(const std::vector<ParticleDeathInfo>&)>::remove)
        .endClass();
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const std::vector<ParticleSubEmitInfo>&)>>("FunctionSubscribe_V_ParticleSubEmits")
        .addFunction("add", (bool (FunctionSubscribe<void(const std::vector<ParticleSubEmitInfo>&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::vector<ParticleSubEmitInfo>&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(const std::vector<ParticleSubEmitInfo>&)>::remove)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<ParticleSystem, GameObject>("ParticleSystem")
        // ParticleSystem(scene, true): origin/main's "a version 2 system on its own GameObject",
        // which every new S20 system is (false gave its legacy model: an S20 one too)
        .addConstructor(
            [](void* memory, Scene* scene) { return new (memory) ParticleSystem(scene); },
            [](void* memory, Scene* scene, Entity entity) { return new (memory) ParticleSystem(scene, entity); },
            [](void* memory, Scene* scene, bool) { return new (memory) ParticleSystem(scene); })
        .addProperty("enabled", &ParticleSystem::isEnabled, &ParticleSystem::setEnabled)
        // Unity's Play / Pause (withChildren = true): the Particle Systems below it too
        .addFunction("play", +[](ParticleSystem* self, lua_State* L) {
            self->play(lua_gettop(L) < 2 || lua_isnil(L, 2) || lua_toboolean(L, 2) != 0);
        })
        .addFunction("pause", +[](ParticleSystem* self, lua_State* L) {
            self->pause(lua_gettop(L) < 2 || lua_isnil(L, 2) || lua_toboolean(L, 2) != 0);
        })
        // stop(clear?) stops emitting (clear: the particles go too); Unity's stop(withChildren,
        // ParticleSystemStopBehavior) the same with its children chosen
        .addFunction("stop", +[](ParticleSystem* self, lua_State* L) {
            if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)){
                self->stop(lua_toboolean(L, 2) != 0, lua_tointeger(L, 3) == 0);
                return;
            }
            self->stop(true, lua_gettop(L) >= 2 && lua_toboolean(L, 2) != 0);
        })
        .addFunction("restart", &ParticleSystem::restart)
        // emit(count [, {position = Vector3, velocity = Vector3, size = n, color = Vector4, lifetime = n}])
        .addFunction("emit", +[](ParticleSystem* self, lua_State* L) {
            const unsigned int count = (unsigned int)luaL_checkinteger(L, 2);
            if (lua_gettop(L) < 3 || lua_isnil(L, 3)){
                self->emit(count);
                return;
            }
            luaL_checktype(L, 3, LUA_TTABLE);
            particles::EmitOverrides overrides;
            lua_getfield(L, 3, "position");
            Vector3 v;
            if (luaValueArg<Vector3>(L, -1, v)){
                overrides.hasPosition = true;
                overrides.position[0] = v.x; overrides.position[1] = v.y; overrides.position[2] = v.z;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "velocity");
            if (luaValueArg<Vector3>(L, -1, v)){
                overrides.hasVelocity = true;
                overrides.velocity[0] = v.x; overrides.velocity[1] = v.y; overrides.velocity[2] = v.z;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "size");
            if (!lua_isnil(L, -1)){
                overrides.hasSize = true;
                overrides.size = (float)luaL_checknumber(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "color");
            Vector4 c;
            if (luaValueArg<Vector4>(L, -1, c)){
                overrides.hasColor = true;
                overrides.color[0] = c.x; overrides.color[1] = c.y; overrides.color[2] = c.z; overrides.color[3] = c.w;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "lifetime");
            if (!lua_isnil(L, -1)){
                overrides.hasLifetime = true;
                overrides.lifetime = (float)luaL_checknumber(L, -1);
            }
            lua_pop(L, 1);
            self->emit(count, overrides);
        })
        .addFunction("seek", &ParticleSystem::seek)
        .addFunction("step", &ParticleSystem::step)
        .addFunction("sendTrigger", &ParticleSystem::sendTrigger)
        .addProperty("isPlaying", &ParticleSystem::isPlaying)
        .addProperty("isPaused", &ParticleSystem::isPaused)
        .addProperty("isEmitting", &ParticleSystem::isEmitting)
        .addProperty("particleCount", &ParticleSystem::getParticleCount)
        .addProperty("time", &ParticleSystem::getTime)
        .addProperty("playOnAwake", &ParticleSystem::isPlayOnAwake, &ParticleSystem::setPlayOnAwake)
        .addProperty("playbackSpeed", &ParticleSystem::getPlaybackSpeed, &ParticleSystem::setPlaybackSpeed)
        // (ui-scaling-text) the GameObject's Unscaled Time: it plays at Engine.timeScale 0
        .addProperty("useUnscaledTime", &ParticleSystem::isUseUnscaledTime, &ParticleSystem::setUseUnscaledTime)
        .addProperty("seed", &ParticleSystem::getSeed, &ParticleSystem::setSeed)
        .addProperty("effect", &ParticleSystem::getEffect, &ParticleSystem::setEffect)
        .addFunction("setInlineEffect", &ParticleSystem::setInlineEffect)
        .addFunction("getInlineEffect", &ParticleSystem::getInlineEffect)
        .addFunction("setFloat", &ParticleSystem::setFloat)
        .addFunction("setVector3", &ParticleSystem::setVector3)
        .addFunction("setColor", &ParticleSystem::setColor)
        .addFunction("getFloat", &ParticleSystem::getFloat)
        .addFunction("getVector3", &ParticleSystem::getVector3)
        .addFunction("getColor", &ParticleSystem::getColor)
        .addFunction("addCollisionPlane", &ParticleSystem::addCollisionPlane)
        .addFunction("clearCollisionPlanes", &ParticleSystem::clearCollisionPlanes)
        // ps:onCollision(function(events) ... end): a listener of this script (replaces its last)
        .addFunction("onCollision", +[](ParticleSystem* self, lua_State* L) {
            luaL_checktype(L, 2, LUA_TFUNCTION);
            lua_settop(L, 2);
            return self->getComponent<ParticleSystemComponent>().onCollision.add("lua", L);
        })
        .addFunction("onParticleDeath", +[](ParticleSystem* self, lua_State* L) {
            luaL_checktype(L, 2, LUA_TFUNCTION);
            lua_settop(L, 2);
            return self->getComponent<ParticleSystemComponent>().onParticleDeath.add("lua", L);
        })
        .addFunction("onSubEmit", +[](ParticleSystem* self, lua_State* L) {
            luaL_checktype(L, 2, LUA_TFUNCTION);
            lua_settop(L, 2);
            return self->getComponent<ParticleSystemComponent>().onSubEmit.add("lua", L);
        })
        .addFunction("getParticleSystemComponent", &ParticleSystem::getComponent<ParticleSystemComponent>)
        // deprecated: the old misspelt name, kept so existing scripts still run
        .addFunction("getParticlesomponent", &ParticleSystem::getComponent<ParticleSystemComponent>)
        // deprecated: the old Action's and its setters (the effect's old view, written back)
        .addFunction("start", &ParticleSystem::start)
        .addFunction("reset", &ParticleSystem::reset)
        .addProperty("running", &ParticleSystem::isRunning)
        .addFunction("isRunning", &ParticleSystem::isRunning)
        .addFunction("isStopped", &ParticleSystem::isStopped)
        .addFunction("getTimeCount", &ParticleSystem::getTimeCount)
        .addProperty("target", &ParticleSystem::getTarget, (void(ParticleSystem::*)(Entity))&ParticleSystem::setTarget)
        .addProperty("speed", &ParticleSystem::getSpeed, &ParticleSystem::setSpeed)
        .addFunction("setTarget",
            luabridge::overload<GameObject*>(&ParticleSystem::setTarget),
            luabridge::overload<Entity>(&ParticleSystem::setTarget))
        .addProperty("maxParticles", &ParticleSystem::getMaxParticles, &ParticleSystem::setMaxParticles)
        .addProperty("rate", &ParticleSystem::getRate, &ParticleSystem::setRate)
        .addProperty("maxPerUpdate", &ParticleSystem::getMaxPerUpdate, &ParticleSystem::setMaxPerUpdate)
        .addProperty("emitter", &ParticleSystem::isEmitter, &ParticleSystem::setEmitter)
        .addProperty("loop", &ParticleSystem::isLoop, &ParticleSystem::setLoop)
        .addProperty("localSpace", &ParticleSystem::isLocalSpace, &ParticleSystem::setLocalSpace)
        .addProperty("emitterShape", &ParticleSystem::getPositionInitializerShape, &ParticleSystem::setPositionInitializerShape)
        .addFunction("setLifeInitializer", 
            luabridge::overload<float>(&ParticleSystem::setLifeInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setLifeInitializer))
        .addFunction("setPositionInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setPositionInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setPositionInitializer))
        .addFunction("setSpherePositionInitializer",
            luabridge::overload<float>(&ParticleSystem::setSpherePositionInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setSpherePositionInitializer))
        .addFunction("setHemispherePositionInitializer",
            luabridge::overload<float>(&ParticleSystem::setHemispherePositionInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setHemispherePositionInitializer))
        .addFunction("setCirclePositionInitializer",
            luabridge::overload<float>(&ParticleSystem::setCirclePositionInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setCirclePositionInitializer))
        .addFunction("setConePositionInitializer", &ParticleSystem::setConePositionInitializer)
        .addFunction("addBurst",
            luabridge::overload<float, int>(&ParticleSystem::addBurst),
            luabridge::overload<float, int, int>(&ParticleSystem::addBurst))
        .addFunction("setBursts", &ParticleSystem::setBursts)
        .addFunction("clearBursts", &ParticleSystem::clearBursts)
        .addFunction("setPositionModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setPositionModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setPositionModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setPositionModifier))
        .addFunction("setVelocityInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setVelocityInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setVelocityInitializer))
        .addFunction("setVelocityModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setVelocityModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setVelocityModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setVelocityModifier))
        .addFunction("setAccelerationInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setAccelerationInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setAccelerationInitializer))
        .addFunction("setAccelerationModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setAccelerationModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setAccelerationModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setAccelerationModifier))
        .addFunction("setColorInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setColorInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setColorInitializer))
        .addFunction("setColorModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setColorModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setColorModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setColorModifier))
        .addFunction("addColorGradientStop", &ParticleSystem::addColorGradientStop)
        .addFunction("setColorGradient", &ParticleSystem::setColorGradient)
        .addFunction("clearColorGradient", &ParticleSystem::clearColorGradient)
        .addFunction("setColorGradientUseSRGB", &ParticleSystem::setColorGradientUseSRGB)
        .addFunction("setAlphaInitializer", 
            luabridge::overload<float>(&ParticleSystem::setAlphaInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setAlphaInitializer))
        .addFunction("setAlphaModifier", 
            luabridge::overload<float, float, float, float>(&ParticleSystem::setAlphaModifier),
            luabridge::overload<float, float, float, float, EaseType>(&ParticleSystem::setAlphaModifier),
            luabridge::overload<float, float, float, float, Ease>(&ParticleSystem::setAlphaModifier))
        .addFunction("setSizeInitializer", 
            luabridge::overload<float>(&ParticleSystem::setSizeInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setSizeInitializer))
        .addFunction("setSizeModifier", 
            luabridge::overload<float, float, float, float>(&ParticleSystem::setSizeModifier),
            luabridge::overload<float, float, float, float, EaseType>(&ParticleSystem::setSizeModifier),
            luabridge::overload<float, float, float, float, Ease>(&ParticleSystem::setSizeModifier))
        .addFunction("setSpriteIntializer", 
            luabridge::overload<std::vector<int>>(&ParticleSystem::setSpriteIntializer),
            luabridge::overload<int, int>(&ParticleSystem::setSpriteIntializer))
        .addFunction("setSpriteModifier", 
            luabridge::overload<float, float, std::vector<int>>(&ParticleSystem::setSpriteModifier),
            luabridge::overload<float, float, std::vector<int>, EaseType>(&ParticleSystem::setSpriteModifier),
            luabridge::overload<float, float, std::vector<int>, Ease>(&ParticleSystem::setSpriteModifier))
        .addFunction("setRotationInitializer", 
            luabridge::overload<float>(&ParticleSystem::setRotationInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setRotationInitializer),
            luabridge::overload<Quaternion>(&ParticleSystem::setRotationInitializer),
            luabridge::overload<Quaternion, Quaternion>(&ParticleSystem::setRotationInitializer))
        .addFunction("setRotationModifier", 
            luabridge::overload<float, float, float, float>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, float, float, EaseType>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, float, float, Ease>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, Quaternion, Quaternion>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, Quaternion, Quaternion, EaseType>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, Quaternion, Quaternion, Ease>(&ParticleSystem::setRotationModifier))
        .addFunction("setScaleInitializer", 
            luabridge::overload<float>(&ParticleSystem::setScaleInitializer),
            luabridge::overload<Vector3>(&ParticleSystem::setScaleInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setScaleInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setScaleInitializer))
        .addFunction("setScaleModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setScaleModifier),
            luabridge::overload<float, float, float, float>(&ParticleSystem::setScaleModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setScaleModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setScaleModifier))
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
        // a custom-block member that follows a global shader value (Shader.setGlobal*); nil unbinds
        .addFunction("setShaderGlobalBinding", +[](Points* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setShaderGlobalBinding(member, luaBindingGlobal(global, "Points.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const Points* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getShaderGlobalBinding(member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
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
        .addProperty("mode", &LineRenderer::getMode, &LineRenderer::setMode)
        .addProperty("positionCount", &LineRenderer::getPositionCount, &LineRenderer::setPositionCount)
        .addFunction("setPosition", &luaLineSetPosition)
        .addFunction("getPosition", &luaLineGetPosition)
        .addFunction("setPositions", &LineRenderer::setPositions)
        .addFunction("getPositions", &LineRenderer::getPositions)
        .addProperty("loop", &LineRenderer::getLoop, &LineRenderer::setLoop)
        .addProperty("useWorldSpace", &LineRenderer::getUseWorldSpace, &LineRenderer::setUseWorldSpace)
        .addProperty("widthMultiplier", &LineRenderer::getWidthMultiplier, &LineRenderer::setWidthMultiplier)
        .addProperty("startWidth", &LineRenderer::getStartWidth, &LineRenderer::setStartWidth)
        .addProperty("endWidth", &LineRenderer::getEndWidth, &LineRenderer::setEndWidth)
        .addProperty("widthCurve", &LineRenderer::getWidthCurve, &LineRenderer::setWidthCurve)
        .addProperty("widthUnit", &LineRenderer::getWidthUnit, &LineRenderer::setWidthUnit)
        .addProperty("colorGradient", &LineRenderer::getColorGradient, &LineRenderer::setColorGradient)
        .addProperty("startColor", &LineRenderer::getStartColor, &LineRenderer::setStartColor)
        .addProperty("endColor", &LineRenderer::getEndColor, &LineRenderer::setEndColor)
        .addProperty("numCornerVertices", &LineRenderer::getNumCornerVertices, &LineRenderer::setNumCornerVertices)
        .addProperty("numCapVertices", &LineRenderer::getNumCapVertices, &LineRenderer::setNumCapVertices)
        .addProperty("alignment", &LineRenderer::getAlignment, &LineRenderer::setAlignment)
        .addProperty("textureMode", &LineRenderer::getTextureMode, &LineRenderer::setTextureMode)
        .addProperty("textureScale", &LineRenderer::getTextureScale, &LineRenderer::setTextureScale)
        .addProperty("texture", &LineRenderer::getTexture, &LineRenderer::setTexture)
        .addProperty("sortPriority", &LineRenderer::getSortPriority, &LineRenderer::setSortPriority)
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
        // a custom-block member that follows a global shader value (Shader.setGlobal*); nil unbinds
        .addFunction("setShaderGlobalBinding", +[](LineRenderer* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setShaderGlobalBinding(member, luaBindingGlobal(global, "LineRenderer.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const LineRenderer* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getShaderGlobalBinding(member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
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
        .beginNamespace("LineRendererMode")
        .addVariable("Segments", LineRendererMode::Segments)
        .addVariable("Polyline", LineRendererMode::Polyline)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LineWidthUnit")
        .addVariable("World", LineWidthUnit::World)
        .addVariable("Pixels", LineWidthUnit::Pixels)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LineAlignment")
        .addVariable("View", LineAlignment::View)
        .addVariable("TransformZ", LineAlignment::TransformZ)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LineTextureMode")
        .addVariable("Stretch", LineTextureMode::Stretch)
        .addVariable("Tile", LineTextureMode::Tile)
        .addVariable("DistributePerSegment", LineTextureMode::DistributePerSegment)
        .addVariable("RepeatPerSegment", LineTextureMode::RepeatPerSegment)
        .endNamespace();

    // Unity's TrailRenderer: TrailRenderer(scene) is a new GameObject with one,
    // object:addTrailRenderer() adds one to a GameObject (or returns its own), and
    // object:getTrailRenderer() / TrailRenderer(scene, entity) are handles to an existing one (nil
    // / nothing added without one). Positions are the stored points, oldest first
    luabridge::getGlobalNamespace(L)
        .deriveClass<TrailRenderer, GameObject>("TrailRenderer")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &TrailRenderer::isEnabled, &TrailRenderer::setEnabled)
        .addProperty("time", &TrailRenderer::getTime, &TrailRenderer::setTime)
        .addProperty("minVertexDistance", &TrailRenderer::getMinVertexDistance, &TrailRenderer::setMinVertexDistance)
        .addProperty("emitting", &TrailRenderer::isEmitting, &TrailRenderer::setEmitting)
        .addProperty("autodestruct", &TrailRenderer::isAutodestruct, &TrailRenderer::setAutodestruct)
        .addFunction("clear", &TrailRenderer::clear)
        .addProperty("positionCount", &TrailRenderer::getPositionCount)
        .addFunction("getPosition", &luaTrailGetPosition)
        .addFunction("setPosition", &luaTrailSetPosition)
        .addFunction("getPositions", &TrailRenderer::getPositions)
        .addFunction("addPosition", &TrailRenderer::addPosition)
        .addFunction("addPositions", &TrailRenderer::addPositions)
        .addProperty("widthMultiplier", &TrailRenderer::getWidthMultiplier, &TrailRenderer::setWidthMultiplier)
        .addProperty("startWidth", &TrailRenderer::getStartWidth, &TrailRenderer::setStartWidth)
        .addProperty("endWidth", &TrailRenderer::getEndWidth, &TrailRenderer::setEndWidth)
        .addProperty("widthCurve", &TrailRenderer::getWidthCurve, &TrailRenderer::setWidthCurve)
        .addProperty("widthUnit", &TrailRenderer::getWidthUnit, &TrailRenderer::setWidthUnit)
        .addProperty("colorGradient", &TrailRenderer::getColorGradient, &TrailRenderer::setColorGradient)
        .addProperty("startColor", &TrailRenderer::getStartColor, &TrailRenderer::setStartColor)
        .addProperty("endColor", &TrailRenderer::getEndColor, &TrailRenderer::setEndColor)
        .addProperty("numCornerVertices", &TrailRenderer::getNumCornerVertices, &TrailRenderer::setNumCornerVertices)
        .addProperty("numCapVertices", &TrailRenderer::getNumCapVertices, &TrailRenderer::setNumCapVertices)
        .addProperty("alignment", &TrailRenderer::getAlignment, &TrailRenderer::setAlignment)
        .addProperty("textureMode", &TrailRenderer::getTextureMode, &TrailRenderer::setTextureMode)
        .addProperty("textureScale", &TrailRenderer::getTextureScale, &TrailRenderer::setTextureScale)
        .addProperty("texture", &TrailRenderer::getTexture, &TrailRenderer::setTexture)
        .addProperty("sortPriority", &TrailRenderer::getSortPriority, &TrailRenderer::setSortPriority)
        .addProperty("customShader", &TrailRenderer::getCustomShader, &TrailRenderer::setCustomShader)
        .addFunction("setShaderUniform",
            luabridge::overload<const std::string&, const Vector4&>(&TrailRenderer::setShaderUniform),
            luabridge::overload<const std::string&, const Vector3&>(&TrailRenderer::setShaderUniform),
            luabridge::overload<const std::string&, const Vector2&>(&TrailRenderer::setShaderUniform),
            luabridge::overload<const std::string&, float>(&TrailRenderer::setShaderUniform))
        .addFunction("getShaderUniform", &TrailRenderer::getShaderUniform)
        .addFunction("removeShaderUniform", &TrailRenderer::removeShaderUniform)
        .addFunction("setShaderParam",
            luabridge::overload<const std::string&, const Vector4&>(&TrailRenderer::setShaderUniform),
            luabridge::overload<TrailRenderer*, const std::string&, const Vector3&>(+[](TrailRenderer* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                self->setShaderUniform(name, LuaBinding::getShaderParamValue(componentShader(self, ShaderType::LINES), name, value));
            }),
            luabridge::overload<const std::string&, const Vector2&>(&TrailRenderer::setShaderUniform),
            luabridge::overload<const std::string&, float>(&TrailRenderer::setShaderUniform))
        .addFunction("getShaderParam", &TrailRenderer::getShaderParam)
        // a custom-block member that follows a global shader value (Shader.setGlobal*); nil unbinds
        .addFunction("setShaderGlobalBinding", +[](TrailRenderer* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setShaderGlobalBinding(member, luaBindingGlobal(global, "TrailRenderer.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const TrailRenderer* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getShaderGlobalBinding(member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
        .addFunction("removeShaderParam", &TrailRenderer::removeShaderUniform)
        .addFunction("setShaderTexture", &TrailRenderer::setShaderTexture)
        .addFunction("getShaderTexture", &TrailRenderer::getShaderTexture)
        .addFunction("setShaderFeature", &TrailRenderer::setShaderFeature)
        .addFunction("isShaderFeatureEnabled", &TrailRenderer::isShaderFeatureEnabled)
        .addProperty("blendMode", &TrailRenderer::getBlendMode, &TrailRenderer::setBlendMode)
        .addProperty("depthWrite", &TrailRenderer::getDepthWrite, &TrailRenderer::setDepthWrite)
        .addProperty("stencil", &TrailRenderer::getStencil, &TrailRenderer::setStencil)
        .addProperty("colorWrite", &TrailRenderer::getColorWrite, &TrailRenderer::setColorWrite)
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
        // a sheet sprite (S44): setting the sheet shows its frame 0
        .addProperty("sheet", &Sprite::getSheet, &Sprite::setSheet)
        .addProperty("frame", &Sprite::getFrameIndex, (void(Sprite::*)(int))&Sprite::setFrame)
        .addProperty("frameName", &Sprite::getFrameName, (void(Sprite::*)(const std::string&))&Sprite::setFrame)
        .addProperty("frameCount", &Sprite::getFrameCount)
        .addFunction("getSheet", [](const Sprite* self) -> std::optional<LuaSpriteSheet> {
            std::shared_ptr<const SpriteSheetData> data = self->getSheetData();
            if (!data) return std::nullopt;
            return LuaSpriteSheet{self->getSheet(), data};
        })
        .addFunction("playTag", [](Sprite* self, const std::string& name, std::optional<float> speed) -> void { self->playTag(name, speed.value_or(1.0f)); })
        .addFunction("stopTag", &Sprite::stopTag)
        .addProperty("tag", [](const Sprite* self) -> std::optional<std::string> {
            const std::string tag = self->getTag();
            if (tag.empty()) return std::nullopt;
            return tag;
        })
        .addProperty("isTagPlaying", &Sprite::isTagPlaying)
        .addFunction("getTag", [](const Sprite* self, const std::string& name, lua_State* L) -> luabridge::LuaRef {
            std::shared_ptr<const SpriteSheetData> data = self->getSheetData();
            const int index = data ? data->findTag(name) : -1;
            return index < 0 ? luabridge::LuaRef(L) : luaSpriteSheetTag(L, *data, data->tags[(size_t)index]);
        })
        .addFunction("getTags", [](const Sprite* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef t = luabridge::newTable(L);
            std::shared_ptr<const SpriteSheetData> data = self->getSheetData();
            if (data){
                for (size_t i = 0; i < data->tags.size(); i++) t[(int)i + 1] = luaSpriteSheetTag(L, *data, data->tags[i]);
            }
            return t;
        })
        .addFunction("getSlice", [](const Sprite* self, const std::string& name, lua_State* L) -> luabridge::LuaRef {
            std::shared_ptr<const SpriteSheetData> data = self->getSheetData();
            const int index = data ? data->findSlice(name) : -1;
            return index < 0 ? luabridge::LuaRef(L) : luaSpriteSlice(L, *self, *data, data->slices[(size_t)index]);
        })
        .addFunction("getSlices", [](const Sprite* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef t = luabridge::newTable(L);
            std::shared_ptr<const SpriteSheetData> data = self->getSheetData();
            if (data){
                for (size_t i = 0; i < data->slices.size(); i++) t[(int)i + 1] = luaSpriteSlice(L, *self, *data, data->slices[i]);
            }
            return t;
        })
        .addFunction("getUserData", [](const Sprite* self, lua_State* L) -> luabridge::LuaRef {
            std::shared_ptr<const SpriteSheetData> data = self->getSheetData();
            return data ? luaSpriteSheetJson(L, data->userData) : luabridge::LuaRef(L);
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<LuaSpriteSheet>("SpriteSheet")
        .addStaticFunction("load", [](const std::string& path) -> std::optional<LuaSpriteSheet> {
            std::shared_ptr<const SpriteSheetData> data = SpriteSheetPool::get(path);
            if (!data){
                Log::error("SpriteSheet.load: no sprite sheet at %s", path.c_str());
                return std::nullopt;
            }
            return LuaSpriteSheet{path, data};
        })
        .addProperty("path", [](const LuaSpriteSheet* self) -> std::string { return self->path; })
        .addProperty("frameCount", [](const LuaSpriteSheet* self) -> int { return self->data->frameCount(); })
        .addProperty("pageCount", [](const LuaSpriteSheet* self) -> int { return (int)self->data->pages.size(); })
        .addProperty("frameNames", [](const LuaSpriteSheet* self, lua_State* L) -> luabridge::LuaRef {
            std::vector<std::string> names;
            for (const SpriteFrame& f : self->data->frames) names.push_back(f.name);
            return luaSpriteSheetList(L, names);
        })
        .addFunction("findFrame", [](const LuaSpriteSheet* self, const std::string& name) -> int { return self->data->findFrame(name); })
        .addFunction("getFrame", [](const LuaSpriteSheet* self, luabridge::LuaRef which, lua_State* L) -> luabridge::LuaRef {
            const int index = which.isNumber() ? which.unsafe_cast<int>() : which.isString() ? self->data->findFrame(which.unsafe_cast<std::string>()) : -1;
            return luaSpriteSheetFrame(L, *self->data, index);
        })
        .addFunction("getTag", [](const LuaSpriteSheet* self, const std::string& name, lua_State* L) -> luabridge::LuaRef {
            const int index = self->data->findTag(name);
            return index < 0 ? luabridge::LuaRef(L) : luaSpriteSheetTag(L, *self->data, self->data->tags[(size_t)index]);
        })
        .addFunction("getSlice", [](const LuaSpriteSheet* self, const std::string& name, std::optional<int> frame, lua_State* L) -> luabridge::LuaRef {
            const int index = self->data->findSlice(name);
            return index < 0 ? luabridge::LuaRef(L) : luaSpriteSheetSlicePixels(L, self->data->slices[(size_t)index], frame.value_or(0));
        })
        .addProperty("tags", [](const LuaSpriteSheet* self, lua_State* L) -> luabridge::LuaRef {
            std::vector<std::string> names;
            for (const SpriteTag& t : self->data->tags) names.push_back(t.name);
            return luaSpriteSheetList(L, names);
        })
        .addProperty("slices", [](const LuaSpriteSheet* self, lua_State* L) -> luabridge::LuaRef {
            std::vector<std::string> names;
            for (const SpriteSlice& sl : self->data->slices) names.push_back(sl.name);
            return luaSpriteSheetList(L, names);
        })
        .addProperty("properties", [](const LuaSpriteSheet* self, lua_State* L) -> luabridge::LuaRef {
            const nlohmann::json& ud = self->data->userData;
            return luaSpriteSheetJson(L, ud.is_object() && ud.contains("properties") ? ud["properties"] : nlohmann::json());
        })
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
        .addFunction("getInheritedAlpha", &RectTransform::getInheritedAlpha)
        .addFunction("getGroupsAllowInteraction", &RectTransform::getGroupsAllowInteraction)
        .addFunction("getGroupsBlockRaycasts", &RectTransform::getGroupsBlockRaycasts)
        .addFunction("isInteractable", &RectTransform::isInteractable)
        .addFunction("getRectTransformComponent", &RectTransform::getComponent<RectTransformComponent>)
        // the Safe Area component driving this RectTransform, nil when it has none
        .addFunction("getSafeAreaComponent", [] (RectTransform* self) -> SafeAreaComponent* {
            return self->getScene()->findComponent<SafeAreaComponent>(self->getEntity());
        })
        .endClass();

    // Unity's CanvasGroup: fades, disables or makes click-through a UI subtree. The fade options
    // table: {ease = EaseType.*, unscaled = false, deactivate = false, onComplete = function}
    static auto fadeOptions = +[](const std::optional<luabridge::LuaRef>& optionsArg, lua_State* L) -> CanvasGroupFadeOptions {
        CanvasGroupFadeOptions result;
        if (!optionsArg || optionsArg->isNil()){
            return result;
        }
        const luabridge::LuaRef& options = *optionsArg;
        if (!options.isTable()){
            luaL_error(L, "CanvasGroup fade options must be a table {ease, unscaled, deactivate, onComplete}");
            return result;
        }
        const luabridge::LuaRef ease = options["ease"];
        if (ease.isNumber()){
            result.ease = static_cast<EaseType>(ease.unsafe_cast<int>());
        }
        const luabridge::LuaRef unscaled = options["unscaled"];
        if (!unscaled.isNil()){
            result.unscaled = unscaled.unsafe_cast<bool>();
        }
        const luabridge::LuaRef deactivate = options["deactivate"];
        if (!deactivate.isNil()){
            result.deactivate = deactivate.unsafe_cast<bool>();
        }
        const luabridge::LuaRef onComplete = options["onComplete"];
        if (onComplete.isFunction()){
            onComplete.push(L);
            result.onComplete = LuaFunction<void>(L);
        }else if (!onComplete.isNil()){
            luaL_error(L, "CanvasGroup fade option onComplete must be a function");
        }
        return result;
    };

    luabridge::getGlobalNamespace(L)
        .deriveClass<CanvasGroup, EntityHandle>("CanvasGroup")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &CanvasGroup::isEnabled, &CanvasGroup::setEnabled)
        .addProperty("alpha", &CanvasGroup::getAlpha, &CanvasGroup::setAlpha)
        .addProperty("interactable", &CanvasGroup::isInteractable, &CanvasGroup::setInteractable)
        .addProperty("blocksRaycasts", &CanvasGroup::isBlocksRaycasts, &CanvasGroup::setBlocksRaycasts)
        .addProperty("ignoreParentGroups", &CanvasGroup::isIgnoreParentGroups, &CanvasGroup::setIgnoreParentGroups)
        .addProperty("renderMode", &CanvasGroup::getRenderMode, &CanvasGroup::setRenderMode)
        .addFunction("fadeTo", +[](CanvasGroup* self, float alpha, float duration, std::optional<luabridge::LuaRef> options, lua_State* L) -> AlphaTween {
            return self->fadeTo(alpha, duration, fadeOptions(options, L));
        })
        .addFunction("fadeIn", +[](CanvasGroup* self, float duration, std::optional<luabridge::LuaRef> options, lua_State* L) -> AlphaTween {
            return self->fadeIn(duration, fadeOptions(options, L));
        })
        .addFunction("fadeOut", +[](CanvasGroup* self, float duration, std::optional<luabridge::LuaRef> options, lua_State* L) -> AlphaTween {
            return self->fadeOut(duration, fadeOptions(options, L));
        })
        .addFunction("stopFade", +[](CanvasGroup* self, std::optional<bool> jumpToEnd) {
            self->stopFade(jumpToEnd.value_or(false));
        })
        .addFunction("isFading", &CanvasGroup::isFading)
        .addFunction("getGameObject", &CanvasGroup::getGameObject)
        .addFunction("getCanvasGroupComponent", &CanvasGroup::getComponent<CanvasGroupComponent>)
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
        // a custom-block member that follows a global shader value (Shader.setGlobal*); nil unbinds
        .addFunction("setShaderGlobalBinding", +[](Polygon* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setShaderGlobalBinding(member, luaBindingGlobal(global, "Polygon.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const Polygon* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getShaderGlobalBinding(member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
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
        // a custom-block member that follows a global shader value (Shader.setGlobal*); nil unbinds
        .addFunction("setShaderGlobalBinding", +[](Text* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setShaderGlobalBinding(member, luaBindingGlobal(global, "Text.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const Text* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getShaderGlobalBinding(member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
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
        .addProperty("outlineWidth", &Text::getOutlineWidth, &Text::setOutlineWidth)
        .addProperty("outlineColor", &Text::getOutlineColor, &Text::setOutlineColor)
        .addProperty("shadowEnabled", &Text::isShadowEnabled, &Text::setShadowEnabled)
        .addProperty("shadowColor", &Text::getShadowColor, &Text::setShadowColor)
        .addProperty("shadowOffset", &Text::getShadowOffset, &Text::setShadowOffset)
        .addProperty("shadowSoftness", &Text::getShadowSoftness, &Text::setShadowSoftness)
        .addProperty("shadowDilate", &Text::getShadowDilate, &Text::setShadowDilate)
        .addProperty("visibleCharacters", &Text::getVisibleCharacters, &Text::setVisibleCharacters)
        .addFunction("getAABB", &Text::getAABB)
        .addFunction("getWorldAABB", &Text::getWorldAABB)
        .endClass();

    // Text Effect (per-character animation of a Text): enums, a layer's values, the component's
    // script interface
    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextEffectType")
        .addVariable("None", TextEffectType::None)
        .addVariable("Wave", TextEffectType::Wave)
        .addVariable("Bounce", TextEffectType::Bounce)
        .addVariable("Shake", TextEffectType::Shake)
        .addVariable("Pulse", TextEffectType::Pulse)
        .addVariable("Typewriter", TextEffectType::Typewriter)
        .addVariable("FadeIn", TextEffectType::FadeIn)
        .addVariable("Grow", TextEffectType::Grow)
        .addVariable("Unfold", TextEffectType::Unfold)
        .addVariable("Rainbow", TextEffectType::Rainbow)
        .addVariable("Gradient", TextEffectType::Gradient)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextEffectMix")
        .addVariable("Multiply", TextEffectMix::Multiply)
        .addVariable("Add", TextEffectMix::Add)
        .addVariable("Replace", TextEffectMix::Replace)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<TextEffectGradientStop>("TextEffectGradientStop")
        .addConstructor<void(*)()>()
        .addProperty("color", &TextEffectGradientStop::color, &TextEffectGradientStop::color)
        .addProperty("position", &TextEffectGradientStop::position, &TextEffectGradientStop::position)
        .endClass();

    // a layer as values (TextEffectLayer(), then textEffect:setLayer(i, layer))
    luabridge::getGlobalNamespace(L)
        .beginClass<TextEffectLayer>("TextEffectLayer")
        .addConstructor<void(*)()>()
        // (a type's default amplitude follows: 1 for Grow and Unfold)
        .addProperty("type", [] (const TextEffectLayer* self) -> TextEffectType { return self->type; },
                             [] (TextEffectLayer* self, TextEffectType type) { TextEffects::setType(*self, type); })
        .addProperty("enabled", &TextEffectLayer::enabled, &TextEffectLayer::enabled)
        .addProperty("amplitude", &TextEffectLayer::amplitude, &TextEffectLayer::amplitude)
        .addProperty("duration", &TextEffectLayer::duration, &TextEffectLayer::duration)
        .addProperty("spread", &TextEffectLayer::spread, &TextEffectLayer::spread)
        .addProperty("phaseStep", &TextEffectLayer::phaseStep, &TextEffectLayer::phaseStep)
        .addProperty("frequency", &TextEffectLayer::frequency, &TextEffectLayer::frequency)
        .addProperty("charactersPerSecond", &TextEffectLayer::charactersPerSecond, &TextEffectLayer::charactersPerSecond)
        .addProperty("fadeTime", &TextEffectLayer::fadeTime, &TextEffectLayer::fadeTime)
        .addProperty("gradient", &TextEffectLayer::gradient, &TextEffectLayer::gradient)
        .addProperty("mix", &TextEffectLayer::mix, &TextEffectLayer::mix)
        .addProperty("saturation", &TextEffectLayer::saturation, &TextEffectLayer::saturation)
        .addProperty("lightness", &TextEffectLayer::lightness, &TextEffectLayer::lightness)
        .endClass();

    // a running effect's layer (textEffect:getLayer(i)): writes change the effect
    luabridge::getGlobalNamespace(L)
        .beginClass<TextEffectLayerRef>("TextEffectLayerRef")
        .addProperty("type", &TextEffectLayerRef::getType, &TextEffectLayerRef::setType)
        .addProperty("enabled", &TextEffectLayerRef::isEnabled, &TextEffectLayerRef::setEnabled)
        .addProperty("amplitude", &TextEffectLayerRef::getAmplitude, &TextEffectLayerRef::setAmplitude)
        .addProperty("duration", &TextEffectLayerRef::getDuration, &TextEffectLayerRef::setDuration)
        .addProperty("spread", &TextEffectLayerRef::getSpread, &TextEffectLayerRef::setSpread)
        .addProperty("phaseStep", &TextEffectLayerRef::getPhaseStep, &TextEffectLayerRef::setPhaseStep)
        .addProperty("frequency", &TextEffectLayerRef::getFrequency, &TextEffectLayerRef::setFrequency)
        .addProperty("charactersPerSecond", &TextEffectLayerRef::getCharactersPerSecond, &TextEffectLayerRef::setCharactersPerSecond)
        .addProperty("fadeTime", &TextEffectLayerRef::getFadeTime, &TextEffectLayerRef::setFadeTime)
        .addProperty("gradient", &TextEffectLayerRef::getGradient, &TextEffectLayerRef::setGradient)
        .addProperty("mix", &TextEffectLayerRef::getMix, &TextEffectLayerRef::setMix)
        .addProperty("saturation", &TextEffectLayerRef::getSaturation, &TextEffectLayerRef::setSaturation)
        .addProperty("lightness", &TextEffectLayerRef::getLightness, &TextEffectLayerRef::setLightness)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TextEffect>("TextEffect")
        .addConstructor<void(*)(Scene*, Entity)>()
        .addProperty("enabled", &TextEffect::isEnabled, &TextEffect::setEnabled)
        .addProperty("autoPlay", &TextEffect::isAutoPlay, &TextEffect::setAutoPlay)
        .addProperty("loop", &TextEffect::isLoop, &TextEffect::setLoop)
        .addProperty("useUnscaledTime", &TextEffect::isUseUnscaledTime, &TextEffect::setUseUnscaledTime)
        .addProperty("startIndex", &TextEffect::getStartIndex, &TextEffect::setStartIndex)
        .addProperty("length", &TextEffect::getLength, &TextEffect::setLength)
        .addProperty("forWords", &TextEffect::getForWords, &TextEffect::setForWords)
        .addProperty("entity", &TextEffect::getEntity)
        .addFunction("getLayer", &TextEffect::getLayer)
        // a TextEffectLayer, or a table of its fields ({type = TextEffectType.Wave, amplitude = 0.12})
        .addFunction("setLayer", [] (TextEffect* self, int index, luabridge::LuaRef value) {
            TextEffectLayer layer;
            if (value.isInstance<TextEffectLayer>()){
                layer = value.cast<TextEffectLayer>().value();
            }else if (value.isTable()){
                auto number = [&](const char* key, float& target){
                    luabridge::LuaRef field = value[key];
                    if (field.isNumber()) target = field.unsafe_cast<float>();
                };
                luabridge::LuaRef type = value["type"];
                // (a type's default amplitude: 1 for Grow and Unfold)
                if (type.isNumber()) TextEffects::setType(layer, (TextEffectType)type.unsafe_cast<int>());
                luabridge::LuaRef enabled = value["enabled"];
                if (enabled.isBool()) layer.enabled = enabled.unsafe_cast<bool>();
                number("amplitude", layer.amplitude);
                number("duration", layer.duration);
                number("spread", layer.spread);
                number("phaseStep", layer.phaseStep);
                number("frequency", layer.frequency);
                number("charactersPerSecond", layer.charactersPerSecond);
                number("fadeTime", layer.fadeTime);
                number("saturation", layer.saturation);
                number("lightness", layer.lightness);
                luabridge::LuaRef mix = value["mix"];
                if (mix.isNumber()) layer.mix = (TextEffectMix)mix.unsafe_cast<int>();
                luabridge::LuaRef gradient = value["gradient"];
                if (gradient.isTable()){
                    for (int i = 1; i <= gradient.length(); i++){
                        luabridge::LuaRef stop = gradient[i];
                        if (stop.isInstance<TextEffectGradientStop>()){
                            layer.gradient.push_back(stop.cast<TextEffectGradientStop>().value());
                        }else if (stop.isTable()){
                            TextEffectGradientStop entry;
                            luabridge::LuaRef color = stop["color"];
                            if (color.isInstance<Vector4>()) entry.color = color.cast<Vector4>().value();
                            luabridge::LuaRef position = stop["position"];
                            if (position.isNumber()) entry.position = position.unsafe_cast<float>();
                            layer.gradient.push_back(entry);
                        }
                    }
                }
            }else{
                Log::warn("TextEffect.setLayer: expects a TextEffectLayer or a table of its fields");
                return;
            }
            self->setLayer(index, layer);
        })
        .addFunction("play", &TextEffect::play)
        .addFunction("stop", &TextEffect::stop)
        .addFunction("finish", &TextEffect::finish)
        .addFunction("restart", &TextEffect::restart)
        .addFunction("isFinished", &TextEffect::isFinished)
        .addFunction("isPlaying", &TextEffect::isPlaying)
        .addProperty("onComplete", [] (TextEffect* self, lua_State*) { return &self->getOnComplete(); }, [] (TextEffect* self, lua_State* L) { self->getOnComplete() = L; })
        .addProperty("onCharacterRevealed", [] (TextEffect* self, lua_State*) { return &self->getOnCharacterRevealed(); }, [] (TextEffect* self, lua_State* L) { self->getOnCharacterRevealed() = L; })
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
        // a custom-block member that follows a global shader value (Shader.setGlobal*); nil unbinds
        .addFunction("setShaderGlobalBinding", +[](Image* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setShaderGlobalBinding(member, luaBindingGlobal(global, "Image.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const Image* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getShaderGlobalBinding(member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
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

    // ui-multitouch: the on-screen controls (input sources feeding Input actions)
    luabridge::getGlobalNamespace(L)
        .deriveClass<OnScreenStick, Image>("OnScreenStick")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", [] (const OnScreenStick* self) -> bool { return self->getStick().enabled; }, [] (OnScreenStick* self, bool value) { self->getStick().enabled = value; })
        .addProperty("mode", [] (const OnScreenStick* self) -> OnScreenStickMode { return self->getStick().mode; }, [] (OnScreenStick* self, OnScreenStickMode value) { self->getStick().mode = value; })
        .addProperty("background", [] (const OnScreenStick* self) -> Entity { return self->getStick().background; }, [] (OnScreenStick* self, Entity value) { self->getStick().background = value; })
        .addProperty("handle", [] (const OnScreenStick* self) -> Entity { return self->getStick().handle; }, [] (OnScreenStick* self, Entity value) { self->getStick().handle = value; })
        .addProperty("handleRange", [] (const OnScreenStick* self) -> float { return self->getStick().handleRange; }, [] (OnScreenStick* self, float value) { self->getStick().handleRange = std::max(0.0f, value); })
        .addProperty("deadZone", [] (const OnScreenStick* self) -> float { return self->getStick().deadZone; }, [] (OnScreenStick* self, float value) { self->getStick().deadZone = std::max(0.0f, std::min(1.0f, value)); })
        .addProperty("moveThreshold", [] (const OnScreenStick* self) -> float { return self->getStick().moveThreshold; }, [] (OnScreenStick* self, float value) { self->getStick().moveThreshold = std::max(0.0f, value); })
        .addProperty("axes", [] (const OnScreenStick* self) -> OnScreenStickAxes { return self->getStick().axes; }, [] (OnScreenStick* self, OnScreenStickAxes value) { self->getStick().axes = value; })
        .addProperty("snapX", [] (const OnScreenStick* self) -> bool { return self->getStick().snapX; }, [] (OnScreenStick* self, bool value) { self->getStick().snapX = value; })
        .addProperty("snapY", [] (const OnScreenStick* self) -> bool { return self->getStick().snapY; }, [] (OnScreenStick* self, bool value) { self->getStick().snapY = value; })
        .addProperty("hideWhenReleased", [] (const OnScreenStick* self) -> bool { return self->getStick().hideWhenReleased; }, [] (OnScreenStick* self, bool value) { self->getStick().hideWhenReleased = value; })
        .addProperty("clampToArea", [] (const OnScreenStick* self) -> bool { return self->getStick().clampToArea; }, [] (OnScreenStick* self, bool value) { self->getStick().clampToArea = value; })
        .addProperty("horizontalAction", [] (const OnScreenStick* self) -> std::string { return self->getStick().horizontalAction; }, [] (OnScreenStick* self, std::string value) { self->getStick().horizontalAction = value; })
        .addProperty("verticalAction", [] (const OnScreenStick* self) -> std::string { return self->getStick().verticalAction; }, [] (OnScreenStick* self, std::string value) { self->getStick().verticalAction = value; })
        .addProperty("visibility", [] (const OnScreenStick* self) -> OnScreenVisibility { return self->getStick().visibility; }, [] (OnScreenStick* self, OnScreenVisibility value) { self->getStick().visibility = value; })
        .addProperty("value", [] (const OnScreenStick* self) -> Vector2 { return self->getStick().value; })
        .addProperty("pressed", [] (const OnScreenStick* self) -> bool { return self->getStick().pressed; })
        .addProperty("pointerId", [] (const OnScreenStick* self) -> int { return self->getStick().pointerId; })
        .addProperty("onValueChanged", [] (OnScreenStick* self, lua_State* L) { return &self->getStick().onValueChanged; }, [] (OnScreenStick* self, lua_State* L) { self->getStick().onValueChanged = L; })
        .addProperty("onPress", [] (OnScreenStick* self, lua_State* L) { return &self->getStick().onPress; }, [] (OnScreenStick* self, lua_State* L) { self->getStick().onPress = L; })
        .addProperty("onRelease", [] (OnScreenStick* self, lua_State* L) { return &self->getStick().onRelease; }, [] (OnScreenStick* self, lua_State* L) { self->getStick().onRelease = L; })
        .addFunction("getOnScreenStickComponent", &OnScreenStick::getComponent<OnScreenStickComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<OnScreenButton, Image>("OnScreenButton")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", [] (const OnScreenButton* self) -> bool { return self->getControl().enabled; }, [] (OnScreenButton* self, bool value) { self->getControl().enabled = value; })
        .addProperty("action", [] (const OnScreenButton* self) -> std::string { return self->getControl().action; }, [] (OnScreenButton* self, std::string value) { self->getControl().action = value; })
        .addProperty("value", [] (const OnScreenButton* self) -> float { return self->getControl().value; }, [] (OnScreenButton* self, float value) { self->getControl().value = std::max(-1.0f, std::min(1.0f, value)); })
        .addProperty("pressWhenPointerEnters", [] (const OnScreenButton* self) -> bool { return self->getControl().pressWhenPointerEnters; }, [] (OnScreenButton* self, bool value) { self->getControl().pressWhenPointerEnters = value; })
        .addProperty("releaseWhenPointerLeaves", [] (const OnScreenButton* self) -> bool { return self->getControl().releaseWhenPointerLeaves; }, [] (OnScreenButton* self, bool value) { self->getControl().releaseWhenPointerLeaves = value; })
        .addProperty("visibility", [] (const OnScreenButton* self) -> OnScreenVisibility { return self->getControl().visibility; }, [] (OnScreenButton* self, OnScreenVisibility value) { self->getControl().visibility = value; })
        .addProperty("pressed", [] (const OnScreenButton* self) -> bool { return self->getControl().pressed; })
        .addProperty("onPress", [] (OnScreenButton* self, lua_State* L) { return &self->getControl().onPress; }, [] (OnScreenButton* self, lua_State* L) { self->getControl().onPress = L; })
        .addProperty("onRelease", [] (OnScreenButton* self, lua_State* L) { return &self->getControl().onRelease; }, [] (OnScreenButton* self, lua_State* L) { self->getControl().onRelease = L; })
        .addFunction("getOnScreenButtonComponent", &OnScreenButton::getComponent<OnScreenButtonComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<TouchArea, Image>("TouchArea")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", [] (const TouchArea* self) -> bool { return self->getArea().enabled; }, [] (TouchArea* self, bool value) { self->getArea().enabled = value; })
        .addProperty("horizontalAction", [] (const TouchArea* self) -> std::string { return self->getArea().horizontalAction; }, [] (TouchArea* self, std::string value) { self->getArea().horizontalAction = value; })
        .addProperty("verticalAction", [] (const TouchArea* self) -> std::string { return self->getArea().verticalAction; }, [] (TouchArea* self, std::string value) { self->getArea().verticalAction = value; })
        .addProperty("sensitivity", [] (const TouchArea* self) -> float { return self->getArea().sensitivity; }, [] (TouchArea* self, float value) { self->getArea().sensitivity = value; })
        .addProperty("invertX", [] (const TouchArea* self) -> bool { return self->getArea().invertX; }, [] (TouchArea* self, bool value) { self->getArea().invertX = value; })
        .addProperty("invertY", [] (const TouchArea* self) -> bool { return self->getArea().invertY; }, [] (TouchArea* self, bool value) { self->getArea().invertY = value; })
        .addProperty("visibility", [] (const TouchArea* self) -> OnScreenVisibility { return self->getArea().visibility; }, [] (TouchArea* self, OnScreenVisibility value) { self->getArea().visibility = value; })
        .addProperty("delta", [] (const TouchArea* self) -> Vector2 { return self->getArea().delta; })
        .addProperty("pressed", [] (const TouchArea* self) -> bool { return self->getArea().pressed; })
        .addProperty("pointerId", [] (const TouchArea* self) -> int { return self->getArea().pointerId; })
        .addProperty("onDrag", [] (TouchArea* self, lua_State* L) { return &self->getArea().onDrag; }, [] (TouchArea* self, lua_State* L) { self->getArea().onDrag = L; })
        .addFunction("getTouchAreaComponent", &TouchArea::getComponent<TouchAreaComponent>)
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
        // the layer the state plays on (a runtime move: it keeps its time and weight)
        .addProperty("layer", &AnimationState::getLayer, &AnimationState::setLayer)
        // Unity AddMixingTransform: the state then animates only these paths (recursive: and below)
        .addFunction("addMixingTransform",
            luabridge::overload<const std::string&>(&AnimationState::addMixingTransform),
            luabridge::overload<const std::string&, bool>(&AnimationState::addMixingTransform))
        .addFunction("removeMixingTransform", &AnimationState::removeMixingTransform)
        .addFunction("clearMixingTransforms", &AnimationState::clearMixingTransforms)
        // humanoid clips: plays left and right swapped (overrides the entry's Mirror; resetMirror() goes back)
        .addProperty("mirror", &AnimationState::getMirror, &AnimationState::setMirror)
        // humanoid clips: the legs reach the clip's foot goals (overrides the entry's Foot IK; resetFootIK() goes back)
        .addProperty("footIK", &AnimationState::getFootIK, &AnimationState::setFootIK)
        // mirror back to the entry's (or the controller state's) Mirror
        .addFunction("resetMirror", &AnimationState::resetMirror)
        // footIK back to the entry's (or the controller state's) Foot IK
        .addFunction("resetFootIK", &AnimationState::resetFootIK)
        // a humanoid clip, or a generic clip being retargeted
        .addProperty("isHumanoid", &AnimationState::isHumanoid)
        .endClass();

    // upstream's AnimatorStateInfo of the Animation Player (AnimationPlayer:getCurrentStateInfo):
    // ControllerStateInfo here, AnimatorStateInfo is the Animator's (registered below)
    luabridge::getGlobalNamespace(L)
        .beginClass<ControllerStateInfo>("ControllerStateInfo")
        .addProperty("isValid", &ControllerStateInfo::isValid)
        .addProperty("name", &ControllerStateInfo::name)
        .addProperty("fullName", &ControllerStateInfo::fullName)
        .addProperty("normalizedTime", &ControllerStateInfo::normalizedTime)
        .addProperty("length", &ControllerStateInfo::length)
        .addProperty("speed", &ControllerStateInfo::speed)
        .addProperty("loop", &ControllerStateInfo::loop)
        .endClass();

    // Unity's PlayMode: what play / crossFade stop
    luabridge::getGlobalNamespace(L)
        .beginNamespace("PlayMode")
        .addVariable("STOP_SAME_LAYER", PlayMode::StopSameLayer)
        .addVariable("STOP_ALL", PlayMode::StopAll)
        .endNamespace();

    // Unity's legacy Animation component: clips played with fades, blends and queues
    luabridge::getGlobalNamespace(L)
        .deriveClass<AnimationPlayer, EntityHandle>("AnimationPlayer")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addFunction("play",
            luabridge::overload<const std::string&>(&AnimationPlayer::play),
            luabridge::overload<const std::string&, float>(&AnimationPlayer::play),
            luabridge::overload<const std::string&, float, PlayMode>(&AnimationPlayer::play))
        .addFunction("crossFade",
            luabridge::overload<const std::string&>(&AnimationPlayer::crossFade),
            luabridge::overload<const std::string&, float>(&AnimationPlayer::crossFade),
            luabridge::overload<const std::string&, float, PlayMode>(&AnimationPlayer::crossFade))
        // Unity's Animator.Play(name, layer, normalizedTime) / CrossFadeInFixedTime(name, seconds, layer)
        .addFunction("playInLayer",
            luabridge::overload<const std::string&, int>(&AnimationPlayer::playInLayer),
            luabridge::overload<const std::string&, int, float>(&AnimationPlayer::playInLayer))
        .addFunction("crossFadeInLayer", &AnimationPlayer::crossFadeInLayer)
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
        .addFunction("getAnimatedProperties", &AnimationPlayer::getAnimatedProperties)
        .addFunction("addClip",
            luabridge::overload<const std::string&>(&AnimationPlayer::addClip),
            luabridge::overload<const std::string&, const std::string&>(&AnimationPlayer::addClip),
            // addClip(path, name | nil, {retarget, mirror, footIK, layer})
            luabridge::overload<AnimationPlayer*, const std::string&, const luabridge::LuaRef&, const luabridge::LuaRef&>(
                +[](AnimationPlayer* self, const std::string& path, const luabridge::LuaRef& name, const luabridge::LuaRef& options){
                    AnimationClipOptions values;
                    if (!name.isNil() && !name.isString()){
                        throw std::runtime_error("AnimationPlayer:addClip: the name is a string, or nil for the clip's own");
                    }
                    if (!options.isNil() && !options.isTable()){
                        throw std::runtime_error("AnimationPlayer:addClip: the options are a table {retarget, mirror, footIK, layer}");
                    }
                    if (options.isTable()){
                        for (luabridge::Iterator it(options); !it.isNil(); ++it){
                            const luabridge::LuaRef key = it.key();
                            const std::string field = key.isString() ? key.unsafe_cast<std::string>() : std::string();
                            if (field != "retarget" && field != "mirror" && field != "footIK" && field != "layer"){
                                throw std::runtime_error("AnimationPlayer:addClip: unknown option '" + (field.empty() ? key.tostring() : field) +
                                                         "' (options are retarget, mirror, footIK, layer)");
                            }
                        }
                        auto flag = [&](const char* field, bool& out){
                            const luabridge::LuaRef value = options[field];
                            if (value.isNil()) return;
                            if (!value.isBool()) throw std::runtime_error(std::string("AnimationPlayer:addClip: ") + field + " is true or false");
                            out = value.unsafe_cast<bool>();
                        };
                        flag("retarget", values.retarget);
                        flag("mirror", values.mirror);
                        flag("footIK", values.footIK);
                        const luabridge::LuaRef layer = options["layer"];
                        if (!layer.isNil()){
                            const double index = layer.isNumber() ? layer.unsafe_cast<double>() : -1.0;
                            if (!layer.isNumber() || index != std::floor(index) || index < 0.0 || index > 1e6){
                                throw std::runtime_error("AnimationPlayer:addClip: layer is a layer index, a whole number (0 is the base layer)");
                            }
                            values.layer = (int)index;
                        }
                    }
                    return self->addClip(path, name.isString() ? name.unsafe_cast<std::string>() : std::string(), values);
                }))
        .addFunction("removeClip", &AnimationPlayer::removeClip)
        .addFunction("getClipCount", &AnimationPlayer::getClipCount)
        .addFunction("getClipName", &AnimationPlayer::getClipName)
        .addProperty("speed", &AnimationPlayer::getSpeed, &AnimationPlayer::setSpeed)
        .addProperty("defaultClip", &AnimationPlayer::getDefaultClip, &AnimationPlayer::setDefaultClip)
        .addProperty("defaultFadeTime", &AnimationPlayer::getDefaultFadeTime, &AnimationPlayer::setDefaultFadeTime)
        .addProperty("playAutomatically", &AnimationPlayer::isPlayAutomatically, &AnimationPlayer::setPlayAutomatically)
        .addProperty("enabled", &AnimationPlayer::isEnabled, &AnimationPlayer::setEnabled)
        .addFunction("isLoaded", &AnimationPlayer::isLoaded)
        // layers (index 0 is the base layer; a layer is an index or a name)
        .addFunction("getLayerCount", &AnimationPlayer::getLayerCount)
        .addFunction("getLayerName", &AnimationPlayer::getLayerName)
        .addFunction("getLayerIndex", &AnimationPlayer::getLayerIndex)
        .addFunction("addLayer",
            luabridge::overload<const std::string&>(&AnimationPlayer::addLayer),
            luabridge::overload<const std::string&, float>(&AnimationPlayer::addLayer),
            luabridge::overload<const std::string&, float, const std::string&>(&AnimationPlayer::addLayer),
            luabridge::overload<const std::string&, float, const std::string&, const std::string&>(&AnimationPlayer::addLayer))
        // a layer by index or by name, dispatched by hand (LuaBridge's overload sets leak a failed
        // attempt's error text: LeakSanitizer)
        .addFunction("removeLayer", +[](AnimationPlayer* self, lua_State* L) -> bool {
            if (lua_type(L, 2) == LUA_TSTRING) return self->removeLayer(std::string(lua_tostring(L, 2)));
            return self->removeLayer((int)luaL_checkinteger(L, 2));
        })
        .addFunction("setLayerWeight", +[](AnimationPlayer* self, lua_State* L) {
            const float weight = (float)luaL_checknumber(L, 3);
            const bool faded = lua_isnumber(L, 4);
            const float fade = faded ? (float)lua_tonumber(L, 4) : 0.0f;
            if (lua_type(L, 2) == LUA_TSTRING){
                const std::string layer = lua_tostring(L, 2);
                if (faded) self->setLayerWeight(layer, weight, fade);
                else self->setLayerWeight(layer, weight);
                return;
            }
            const int layer = (int)luaL_checkinteger(L, 2);
            if (faded) self->setLayerWeight(layer, weight, fade);
            else self->setLayerWeight(layer, weight);
        })
        .addFunction("getLayerWeight", +[](const AnimationPlayer* self, lua_State* L) -> float {
            if (lua_type(L, 2) == LUA_TSTRING) return self->getLayerWeight(std::string(lua_tostring(L, 2)));
            return self->getLayerWeight((int)luaL_checkinteger(L, 2));
        })
        .addFunction("setLayerMask", +[](AnimationPlayer* self, lua_State* L) {
            const std::string mask = luaL_checkstring(L, 3);
            if (lua_type(L, 2) == LUA_TSTRING) self->setLayerMask(std::string(lua_tostring(L, 2)), mask);
            else self->setLayerMask((int)lua_tointeger(L, 2), mask);
        })
        .addFunction("getLayerMask", +[](const AnimationPlayer* self, lua_State* L) -> std::string {
            if (lua_type(L, 2) == LUA_TSTRING) return self->getLayerMask(std::string(lua_tostring(L, 2)));
            return self->getLayerMask((int)luaL_checkinteger(L, 2));
        })
        .addFunction("setLayerBlendMode", +[](AnimationPlayer* self, lua_State* L) {
            const std::string mode = luaL_checkstring(L, 3);
            if (lua_type(L, 2) == LUA_TSTRING) self->setLayerBlendMode(std::string(lua_tostring(L, 2)), mode);
            else self->setLayerBlendMode((int)lua_tointeger(L, 2), mode);
        })
        .addFunction("getLayerBlendMode", +[](const AnimationPlayer* self, lua_State* L) -> std::string {
            if (lua_type(L, 2) == LUA_TSTRING) return self->getLayerBlendMode(std::string(lua_tostring(L, 2)));
            return self->getLayerBlendMode((int)luaL_checkinteger(L, 2));
        })
        .addFunction("stopLayer", +[](AnimationPlayer* self, lua_State* L) {
            const bool faded = lua_isnumber(L, 3);
            const float fade = faded ? (float)lua_tonumber(L, 3) : 0.0f;
            if (lua_type(L, 2) == LUA_TSTRING){
                const std::string layer = lua_tostring(L, 2);
                if (faded) self->stopLayer(layer, fade);
                else self->stopLayer(layer);
                return;
            }
            const int layer = (int)luaL_checkinteger(L, 2);
            if (faded) self->stopLayer(layer, fade);
            else self->stopLayer(layer);
        })
        // upstream's Animator Controller calls: the GameObject's Animator (object/AnimationPlayer.h)
        .addProperty("controller", &AnimationPlayer::getController, &AnimationPlayer::setController)
        .addFunction("hasController", &AnimationPlayer::hasController)
        .addFunction("setFloat",
            luabridge::overload<const std::string&, float>(&AnimationPlayer::setFloat),
            luabridge::overload<const std::string&, float, float>(&AnimationPlayer::setFloat))
        .addFunction("getFloat", &AnimationPlayer::getFloat)
        .addFunction("setInteger", &AnimationPlayer::setInteger)
        .addFunction("getInteger", &AnimationPlayer::getInteger)
        .addFunction("setBool", &AnimationPlayer::setBool)
        .addFunction("getBool", &AnimationPlayer::getBool)
        .addFunction("setTrigger", &AnimationPlayer::setTrigger)
        .addFunction("resetTrigger", &AnimationPlayer::resetTrigger)
        .addFunction("hasParameter", &AnimationPlayer::hasParameter)
        .addFunction("getParameterType", &AnimationPlayer::getParameterType)
        // an optional layer read by hand (LuaBridge's overload resolution of () / (int) leaks the
        // failed attempt's error text: LeakSanitizer)
        .addFunction("getCurrentStateName", +[](const AnimationPlayer* self, lua_State* L) -> std::string {
            return lua_isnumber(L, 2) ? self->getCurrentStateName((int)lua_tointeger(L, 2)) : self->getCurrentStateName();
        })
        .addFunction("getCurrentStateInfo", +[](const AnimationPlayer* self, lua_State* L) -> ControllerStateInfo {
            return lua_isnumber(L, 2) ? self->getCurrentStateInfo((int)lua_tointeger(L, 2)) : self->getCurrentStateInfo();
        })
        .addFunction("getNextStateInfo", +[](const AnimationPlayer* self, lua_State* L) -> ControllerStateInfo {
            return lua_isnumber(L, 2) ? self->getNextStateInfo((int)lua_tointeger(L, 2)) : self->getNextStateInfo();
        })
        .addFunction("isInTransition", +[](const AnimationPlayer* self, lua_State* L) -> bool {
            return lua_isnumber(L, 2) ? self->isInTransition((int)lua_tointeger(L, 2)) : self->isInTransition();
        })
        // functions called with (layer, state name) when a controller state is entered / exited
        .addProperty("onStateEnter", [] (AnimationPlayer* self, lua_State*) { return &self->getOnStateEnter(); }, [] (AnimationPlayer* self, lua_State* L) { self->getOnStateEnter() = L; })
        .addProperty("onStateExit", [] (AnimationPlayer* self, lua_State*) { return &self->getOnStateExit(); }, [] (AnimationPlayer* self, lua_State* L) { self->getOnStateExit() = L; })
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

    // a layer's current or next state (Animator:getCurrentState / getNextState)
    luabridge::getGlobalNamespace(L)
        .beginClass<AnimatorStateInfo>("AnimatorStateInfo")
        .addProperty("valid", &AnimatorStateInfo::valid)
        .addProperty("name", &AnimatorStateInfo::name)
        .addProperty("fullPath", &AnimatorStateInfo::fullPath)
        .addProperty("tag", &AnimatorStateInfo::tag)
        .addProperty("normalizedTime", &AnimatorStateInfo::normalizedTime)
        .addProperty("length", &AnimatorStateInfo::length)
        .addProperty("loop", &AnimatorStateInfo::loop)
        .addProperty("speed", &AnimatorStateInfo::speed)
        // the state's humanoid playback: left and right swapped / the legs reach the foot goals
        .addProperty("mirror", &AnimatorStateInfo::mirror)
        .addProperty("footIK", &AnimatorStateInfo::footIK)
        .endClass();

    // Unity's Animator: an Animator Controller's parameters, states, layers, montages, IK and root motion
    luabridge::getGlobalNamespace(L)
        .deriveClass<Animator, EntityHandle>("Animator")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addProperty("controller", &Animator::getController, &Animator::setController)
        .addFunction("isLoaded", &Animator::isLoaded)
        .addFunction("setFloat",
            luabridge::overload<const std::string&, float>(&Animator::setFloat),
            luabridge::overload<const std::string&, float, float, float>(&Animator::setFloat))
        .addFunction("getFloat", &Animator::getFloat)
        .addFunction("setInteger", &Animator::setInteger)
        .addFunction("getInteger", &Animator::getInteger)
        .addFunction("setBool", &Animator::setBool)
        .addFunction("getBool", &Animator::getBool)
        .addFunction("setTrigger", &Animator::setTrigger)
        .addFunction("resetTrigger", &Animator::resetTrigger)
        .addFunction("getParameterIndex", &Animator::getParameterIndex)
        .addFunction("getParameterCount", &Animator::getParameterCount)
        .addFunction("getParameterName", &Animator::getParameterName)
        .addFunction("play",
            luabridge::overload<const std::string&>(&Animator::play),
            luabridge::overload<const std::string&, int>(&Animator::play),
            luabridge::overload<const std::string&, int, float>(&Animator::play))
        .addFunction("crossFade",
            luabridge::overload<const std::string&, float>(&Animator::crossFade),
            luabridge::overload<const std::string&, float, int>(&Animator::crossFade),
            luabridge::overload<const std::string&, float, int, float>(&Animator::crossFade))
        .addFunction("crossFadeInFixedTime",
            luabridge::overload<const std::string&, float>(&Animator::crossFadeInFixedTime),
            luabridge::overload<const std::string&, float, int>(&Animator::crossFadeInFixedTime),
            luabridge::overload<const std::string&, float, int, float>(&Animator::crossFadeInFixedTime))
        .addFunction("getCurrentState", &Animator::getCurrentState)
        .addFunction("getNextState", &Animator::getNextState)
        // the layer is optional (0: the base layer), as upstream's controller calls had it
        .addFunction("isInTransition", +[](const Animator* self, lua_State* L) -> bool {
            return self->isInTransition(lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 0);
        })
        // upstream's controller calls (its getAnimator returned the Animation Player that played
        // the controller; getAnimator now returns this Animator): kept so its scripts run as
        // they were written (docs/specs/S21-animation.md "Upstream absorption")
        .addFunction("getCurrentStateName", +[](const Animator* self, lua_State* L) -> std::string {
            return self->getCurrentState(lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 0).name;
        })
        .addFunction("getCurrentStateInfo", +[](const Animator* self, lua_State* L) -> ControllerStateInfo {
            return toControllerStateInfo(self->getCurrentState(lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 0));
        })
        .addFunction("getNextStateInfo", +[](const Animator* self, lua_State* L) -> ControllerStateInfo {
            return toControllerStateInfo(self->getNextState(lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 0));
        })
        .addFunction("playInLayer", +[](Animator* self, const std::string& name, int layer, lua_State* L) {
            if (lua_isnumber(L, 4)) self->play(name, layer, (float)lua_tonumber(L, 4));
            else self->play(name, layer);
        })
        .addFunction("crossFadeInLayer", +[](Animator* self, const std::string& name, float seconds, int layer) { self->crossFade(name, seconds, layer); })
        .addFunction("hasController", +[](const Animator* self) -> bool { return !self->getController().empty(); })
        .addFunction("hasParameter", +[](const Animator* self, const std::string& name) -> bool { return self->getParameterIndex(name) >= 0; })
        .addFunction("getLayerCount", &Animator::getLayerCount)
        .addFunction("getLayerName", &Animator::getLayerName)
        .addFunction("getLayerIndex", &Animator::getLayerIndex)
        // a layer by index or by name (upstream's player-controller scripts name it), an optional
        // fade in seconds
        .addFunction("setLayerWeight", +[](Animator* self, lua_State* L) {
            const int layer = lua_type(L, 2) == LUA_TSTRING ? self->getLayerIndex(lua_tostring(L, 2)) : (int)luaL_checkinteger(L, 2);
            const float weight = (float)luaL_checknumber(L, 3);
            if (lua_isnumber(L, 4)) self->setLayerWeight(layer, weight, (float)lua_tonumber(L, 4));
            else self->setLayerWeight(layer, weight);
        })
        .addFunction("getLayerWeight", +[](const Animator* self, lua_State* L) -> float {
            const int layer = lua_type(L, 2) == LUA_TSTRING ? self->getLayerIndex(lua_tostring(L, 2)) : (int)luaL_checkinteger(L, 2);
            return self->getLayerWeight(layer);
        })
        .addFunction("playMontage",
            luabridge::overload<const std::string&>(&Animator::playMontage),
            luabridge::overload<const std::string&, float>(&Animator::playMontage),
            luabridge::overload<const std::string&, float, const std::string&>(&Animator::playMontage))
        .addFunction("stopMontage",
            luabridge::overload<>(&Animator::stopMontage),
            luabridge::overload<float>(&Animator::stopMontage))
        .addFunction("isMontagePlaying", &Animator::isMontagePlaying)
        .addFunction("getMontage", &Animator::getMontage)
        .addFunction("getMontageSection", &Animator::getMontageSection)
        .addFunction("montageJumpToSection", &Animator::montageJumpToSection)
        .addFunction("montageSetNextSection", &Animator::montageSetNextSection)
        .addFunction("setIKTarget",
            luabridge::overload<const std::string&, const Vector3&>(&Animator::setIKTarget),
            luabridge::overload<const std::string&, const Vector3&, float>(&Animator::setIKTarget))
        .addFunction("clearIKTarget", &Animator::clearIKTarget)
        .addFunction("setIKWeight", &Animator::setIKWeight)
        .addFunction("setLookAt",
            luabridge::overload<const Vector3&>(&Animator::setLookAt),
            luabridge::overload<const Vector3&, float>(&Animator::setLookAt))
        .addFunction("hasBone", &Animator::hasBone)
        .addFunction("getBonePosition", &Animator::getBonePosition)
        .addFunction("getBoneRotation", &Animator::getBoneRotation)
        .addProperty("speed", &Animator::getSpeed, &Animator::setSpeed)
        .addProperty("applyRootMotion", &Animator::isApplyRootMotion, &Animator::setApplyRootMotion)
        .addFunction("getDeltaPosition", &Animator::getDeltaPosition)
        .addFunction("getDeltaRotation", &Animator::getDeltaRotation)
        .addFunction("isHumanoid", &Animator::isHumanoid)
        // a function called with an AnimationEvent when an event fires
        .addProperty("onEvent", [] (Animator* self, lua_State*) { return &self->getOnEvent(); }, [] (Animator* self, lua_State* L) { self->getOnEvent() = L; })
        // functions called with (layer name, state full path)
        .addProperty("onStateEnter", [] (Animator* self, lua_State*) { return &self->getOnStateEnter(); }, [] (Animator* self, lua_State* L) { self->getOnStateEnter() = L; })
        .addProperty("onStateExit", [] (Animator* self, lua_State*) { return &self->getOnStateExit(); }, [] (Animator* self, lua_State* L) { self->getOnStateExit() = L; })
        // a function called with (montage path, interrupted) when a montage ends
        .addProperty("onMontageEnded", [] (Animator* self, lua_State*) { return &self->getOnMontageEnded(); }, [] (Animator* self, lua_State* L) { self->getOnMontageEnded() = L; })
        // a function called with (delta position, delta rotation) in world space: root motion is then not applied
        .addProperty("onAnimatorMove", [] (Animator* self, lua_State*) { return &self->getOnAnimatorMove(); }, [] (Animator* self, lua_State* L) { self->getOnAnimatorMove() = L; })
        .addFunction("getAnimatorComponent", &Animator::getComponent<AnimatorComponent>)
        .endClass();

    // The instances one shake call started (MilkShake's ShakeInstance): one per shaker it reached.
    // Gone instances drop out; with none left it is invalid and its calls do nothing
    luabridge::getGlobalNamespace(L)
        .beginClass<ShakeInstanceHandle>("ShakeInstance")
        // Fades out over `fade` seconds (default: the preset's fade_out; 0: now)
        .addFunction("stop",
            luabridge::overload<>(&ShakeInstanceHandle::stop),
            luabridge::overload<float>(&ShakeInstanceHandle::stop))
        // The noise slows to a stop over `fade` seconds (default 0: at once)
        .addFunction("pause",
            luabridge::overload<>(&ShakeInstanceHandle::pause),
            luabridge::overload<float>(&ShakeInstanceHandle::pause))
        .addFunction("resume",
            luabridge::overload<>(&ShakeInstanceHandle::resume),
            luabridge::overload<float>(&ShakeInstanceHandle::resume))
        .addFunction("isPaused", &ShakeInstanceHandle::isPaused)
        // Nothing of it plays any more (finished, or its shakers are gone)
        .addFunction("isFinished", &ShakeInstanceHandle::isFinished)
        .addFunction("isValid", &ShakeInstanceHandle::isValid)
        // Multiplies the strength (on every shaker it reached)
        .addProperty("strengthScale", &ShakeInstanceHandle::getStrengthScale, &ShakeInstanceHandle::setStrengthScale)
        // Multiplies the noise speed
        .addProperty("roughnessScale", &ShakeInstanceHandle::getRoughnessScale, &ShakeInstanceHandle::setRoughnessScale)
        // "fading_in", "sustained", "fading_out", "paused" or "stopped"
        .addProperty("state", &ShakeInstanceHandle::getState)
        // The live instances (one per shaker)
        .addProperty("count", &ShakeInstanceHandle::getCount)
        .addFunction("__tostring", &ShakeInstanceHandle::toString)
        .endClass();

    // Shakes every global Camera Shaker (MilkShake's Shaker.ShakeAll): enabled, active, Global on,
    // in every running scene. Presets: CameraShake.getPresetNames(), a .shake path or a table
    luabridge::getGlobalNamespace(L)
        .beginClass<CameraShake>("CameraShake")
        .addStaticFunction("shake", &luaCameraShakeShake)
        .addStaticFunction("shakeAt", &luaCameraShakeShakeAt)
        // Adds trauma (0..1, clamped) to every global shaker: shake = trauma^power, decaying linearly
        .addStaticFunction("addTrauma", &CameraShake::addTrauma)
        .addStaticFunction("stopAll",
            luabridge::overload<>(&CameraShake::stopAll),
            luabridge::overload<float>(&CameraShake::stopAll))
        // The built-in preset names
        .addStaticFunction("getPresetNames", &CameraShake::getPresetNames)
        .endClass();

    // Shakes its GameObject (MilkShake's Shaker): a camera through a view offset (its Transform is
    // never written), any other GameObject additively through its Transform
    luabridge::getGlobalNamespace(L)
        .deriveClass<CameraShaker, EntityHandle>("CameraShaker")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addFunction("shake", &luaCameraShakerShake)
        .addFunction("shakeAt", &luaCameraShakerShakeAt)
        // Adds trauma (0..1, clamped): shake = trauma^power, decaying traumaDecay per second
        .addFunction("addTrauma", &CameraShaker::addTrauma)
        .addFunction("stopAll",
            luabridge::overload<>(&CameraShaker::stopAll),
            luabridge::overload<float>(&CameraShaker::stopAll))
        .addFunction("getInstanceCount", &CameraShaker::getInstanceCount)
        // 0..1
        .addProperty("trauma", &CameraShaker::getTrauma, &CameraShaker::setTrauma)
        .addProperty("enabled", &CameraShaker::isEnabled, &CameraShaker::setEnabled)
        // Receives the CameraShake.* calls
        .addProperty("global", &CameraShaker::isGlobal, &CameraShaker::setGlobal)
        .addProperty("unscaledTime", &CameraShaker::isUnscaledTime, &CameraShaker::setUnscaledTime)
        .addProperty("mode", &CameraShaker::getMode, &CameraShaker::setMode)
        .addProperty("positionScale", &CameraShaker::getPositionScale, &CameraShaker::setPositionScale)
        .addProperty("rotationScale", &CameraShaker::getRotationScale, &CameraShaker::setRotationScale)
        // A built-in name or a .shake path played on each enable ("" = none)
        .addProperty("startPreset", &CameraShaker::getStartPreset, &CameraShaker::setStartPreset)
        .addProperty("traumaDecay", &CameraShaker::getTraumaDecay, &CameraShaker::setTraumaDecay)
        .addProperty("traumaPower", &CameraShaker::getTraumaPower, &CameraShaker::setTraumaPower)
        .addProperty("traumaMaxOffset", &CameraShaker::getTraumaMaxOffset, &CameraShaker::setTraumaMaxOffset)
        // degrees
        .addProperty("traumaMaxAngle", &CameraShaker::getTraumaMaxAngle, &CameraShaker::setTraumaMaxAngle)
        .addProperty("traumaFrequency", &CameraShaker::getTraumaFrequency, &CameraShaker::setTraumaFrequency)
        // The position offset applied this frame
        .addProperty("offsetPosition", &CameraShaker::getOffsetPosition)
        // The rotation offset applied this frame (Euler degrees)
        .addProperty("offsetRotation", &CameraShaker::getOffsetRotation)
        .endClass();

    // Keeps its GameObject at an offset from a target with Unity's SmoothDamp, aiming at it; owns
    // the Transform while enabled
    luabridge::getGlobalNamespace(L)
        .deriveClass<CameraFollow, EntityHandle>("CameraFollow")
        .addConstructor <void (*) (Scene*, Entity)> ()
        // Places it on the next update without damping
        .addFunction("snap", &CameraFollow::snap)
        // The GameObject followed (this scene's, or a persistent DontDestroyOnLoad one), or nil
        .addProperty("target",
            +[](const CameraFollow* self, lua_State* L) -> luabridge::LuaRef {
                Scene* scene = self->getTargetScene();
                const Entity target = self->getTarget();
                if (!scene || target == NULL_ENTITY || !scene->isEntityCreated(target)){
                    return luabridge::LuaRef(L);
                }
                return luabridge::LuaRef(L, GameObject(scene, target));
            },
            +[](CameraFollow* self, lua_State* L){
                // the assigned value is on the top of the stack
                const int index = lua_gettop(L);
                if (lua_isnil(L, index)){
                    self->setTarget(NULL_ENTITY);
                    return;
                }
                Scene* scene = nullptr;
                Entity entity = NULL_ENTITY;
                if (!luaObjectArg(L, index, scene, entity)){
                    Log::warn("CameraFollow.target: a GameObject (of this scene or the persistent one), or nil");
                    return;
                }
                self->setTarget(scene, entity);
            })
        .addProperty("offset", &CameraFollow::getOffset, &CameraFollow::setOffset)
        .addProperty("offsetSpace", &CameraFollow::getOffsetSpace, &CameraFollow::setOffsetSpace)
        // SmoothDamp smooth time per world axis, seconds (0 = rigid)
        .addProperty("damping", &CameraFollow::getDamping, &CameraFollow::setDamping)
        // Units per second (0 = unlimited)
        .addProperty("maxSpeed", &CameraFollow::getMaxSpeed, &CameraFollow::setMaxSpeed)
        .addProperty("lookAt", &CameraFollow::isLookAt, &CameraFollow::setLookAt)
        .addProperty("lookAtOffset", &CameraFollow::getLookAtOffset, &CameraFollow::setLookAtOffset)
        // Seconds to cover 99% of a turn (0 = rigid)
        .addProperty("rotationDamping", &CameraFollow::getRotationDamping, &CameraFollow::setRotationDamping)
        // Width and height (world x / y) where target moves don't move an orthographic camera
        .addProperty("deadZone", &CameraFollow::getDeadZone, &CameraFollow::setDeadZone)
        .addProperty("useBounds", &CameraFollow::isUseBounds, &CameraFollow::setUseBounds)
        .addProperty("boundsMin", &CameraFollow::getBoundsMin, &CameraFollow::setBoundsMin)
        .addProperty("boundsMax", &CameraFollow::getBoundsMax, &CameraFollow::setBoundsMax)
        .addProperty("unscaledTime", &CameraFollow::isUnscaledTime, &CameraFollow::setUnscaledTime)
        .addProperty("enabled", &CameraFollow::isEnabled, &CameraFollow::setEnabled)
        .endClass();

    // Unity's VideoPlayer enums (docs/mcp/guide/video.md)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("VideoPlayerSource")
        .addVariable("CLIP", VideoPlayerSource::CLIP)
        .addVariable("URL", VideoPlayerSource::URL)
        .endNamespace()
        .beginNamespace("VideoTimeUpdateMode")
        .addVariable("GAME_TIME", VideoTimeUpdateMode::GAME_TIME)
        .addVariable("UNSCALED_GAME_TIME", VideoTimeUpdateMode::UNSCALED_GAME_TIME)
        .endNamespace()
        .beginNamespace("VideoRenderMode")
        .addVariable("CAMERA_FAR_PLANE", VideoRenderMode::CAMERA_FAR_PLANE)
        .addVariable("CAMERA_NEAR_PLANE", VideoRenderMode::CAMERA_NEAR_PLANE)
        .addVariable("RENDER_TEXTURE", VideoRenderMode::RENDER_TEXTURE)
        .addVariable("MATERIAL_OVERRIDE", VideoRenderMode::MATERIAL_OVERRIDE)
        .addVariable("API_ONLY", VideoRenderMode::API_ONLY)
        .endNamespace()
        .beginNamespace("VideoAspectRatio")
        .addVariable("NO_SCALING", VideoAspectRatio::NO_SCALING)
        .addVariable("FIT_VERTICALLY", VideoAspectRatio::FIT_VERTICALLY)
        .addVariable("FIT_HORIZONTALLY", VideoAspectRatio::FIT_HORIZONTALLY)
        .addVariable("FIT_INSIDE", VideoAspectRatio::FIT_INSIDE)
        .addVariable("FIT_OUTSIDE", VideoAspectRatio::FIT_OUTSIDE)
        .addVariable("STRETCH", VideoAspectRatio::STRETCH)
        .endNamespace()
        .beginNamespace("VideoAudioOutputMode")
        .addVariable("NONE", VideoAudioOutputMode::NONE)
        .addVariable("AUDIO_SOURCE", VideoAudioOutputMode::AUDIO_SOURCE)
        .addVariable("DIRECT", VideoAudioOutputMode::DIRECT)
        .endNamespace();

    // Unity's VideoPlayer: plays a clip or a URL into its render texture, a material, a camera
    // plane or nowhere (API only); its sound directly or through an AudioSource
    luabridge::getGlobalNamespace(L)
        .deriveClass<VideoPlayer, EntityHandle>("VideoPlayer")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &VideoPlayer::isEnabled, &VideoPlayer::setEnabled)
        .addProperty("source", &VideoPlayer::getSource, &VideoPlayer::setSource)
        .addProperty("clip", &VideoPlayer::getClip, &VideoPlayer::setClip)
        .addProperty("url", &VideoPlayer::getUrl, &VideoPlayer::setUrl)
        .addProperty("playOnAwake", &VideoPlayer::isPlayOnAwake, &VideoPlayer::setPlayOnAwake)
        .addProperty("waitForFirstFrame", &VideoPlayer::isWaitForFirstFrame, &VideoPlayer::setWaitForFirstFrame)
        .addProperty("isLooping", &VideoPlayer::isLooping, &VideoPlayer::setLooping)
        .addProperty("skipOnDrop", &VideoPlayer::isSkipOnDrop, &VideoPlayer::setSkipOnDrop)
        .addProperty("playbackSpeed", &VideoPlayer::getPlaybackSpeed, &VideoPlayer::setPlaybackSpeed)
        .addProperty("timeUpdateMode", &VideoPlayer::getTimeUpdateMode, &VideoPlayer::setTimeUpdateMode)
        .addProperty("renderMode", &VideoPlayer::getRenderMode, &VideoPlayer::setRenderMode)
        // a GameObject with a Camera, nil: the main camera
        .addProperty("targetCamera",
            [](const VideoPlayer* self) -> std::optional<GameObject> { return gameObjectOrNil(self->getScene(), self->getTargetCamera()); },
            [](VideoPlayer* self, luabridge::LuaRef value) { self->setTargetCamera(entityOf(value, "targetCamera")); })
        .addProperty("targetCameraAlpha", &VideoPlayer::getTargetCameraAlpha, &VideoPlayer::setTargetCameraAlpha)
        .addProperty("aspectRatio", &VideoPlayer::getAspectRatio, &VideoPlayer::setAspectRatio)
        // a GameObject with a Mesh, Sprite or Image, nil: this one
        .addProperty("targetMaterialRenderer",
            [](const VideoPlayer* self) -> std::optional<GameObject> { return gameObjectOrNil(self->getScene(), self->getTargetMaterialRenderer()); },
            [](VideoPlayer* self, luabridge::LuaRef value) { self->setTargetMaterialRenderer(entityOf(value, "targetMaterialRenderer")); })
        // "baseColorTexture" (default), "emissiveTexture", ... or a shader @texture uniform
        .addProperty("targetMaterialProperty", &VideoPlayer::getTargetMaterialProperty, &VideoPlayer::setTargetMaterialProperty)
        // -1: every submesh
        .addProperty("targetSubmesh", &VideoPlayer::getTargetSubmesh, &VideoPlayer::setTargetSubmesh)
        .addProperty("renderTextureWidth", &VideoPlayer::getRenderTextureWidth, &VideoPlayer::setRenderTextureWidth)
        .addProperty("renderTextureHeight", &VideoPlayer::getRenderTextureHeight, &VideoPlayer::setRenderTextureHeight)
        .addProperty("audioOutputMode", &VideoPlayer::getAudioOutputMode, &VideoPlayer::setAudioOutputMode)
        .addProperty("directVolume", &VideoPlayer::getDirectVolume, &VideoPlayer::setDirectVolume)
        .addProperty("directMute", &VideoPlayer::isDirectMute, &VideoPlayer::setDirectMute)
        // a GameObject with an AudioSource, nil: this one
        .addProperty("targetAudioSource",
            [](const VideoPlayer* self) -> std::optional<GameObject> { return gameObjectOrNil(self->getScene(), self->getTargetAudioSource()); },
            [](VideoPlayer* self, luabridge::LuaRef value) { self->setTargetAudioSource(entityOf(value, "targetAudioSource")); })
        .addProperty("time", &VideoPlayer::getTime, &VideoPlayer::setTime)
        .addProperty("canSetTime", &VideoPlayer::canSetTime)
        .addProperty("frame", &VideoPlayer::getFrame, &VideoPlayer::setFrame)
        .addProperty("frameCount", &VideoPlayer::getFrameCount)
        .addProperty("frameRate", &VideoPlayer::getFrameRate)
        // seconds
        .addProperty("length", &VideoPlayer::getLength)
        .addProperty("width", &VideoPlayer::getWidth)
        .addProperty("height", &VideoPlayer::getHeight)
        .addProperty("isPrepared", &VideoPlayer::isPrepared)
        .addProperty("isPlaying", &VideoPlayer::isPlaying)
        .addProperty("isPaused", &VideoPlayer::isPaused)
        // 0..1: the download of a URL, then the opening
        .addProperty("prepareProgress", &VideoPlayer::getPrepareProgress)
        .addProperty("texture", &VideoPlayer::getTexture)
        .addProperty("backend", &VideoPlayer::getBackend)
        .addProperty("videoCodec", &VideoPlayer::getVideoCodec)
        .addProperty("audioCodec", &VideoPlayer::getAudioCodec)
        // the last error ("" when none)
        .addProperty("error", &VideoPlayer::getError)
        .addFunction("prepare", &VideoPlayer::prepare)
        .addFunction("play", &VideoPlayer::play)
        .addFunction("pause", &VideoPlayer::pause)
        .addFunction("stop", &VideoPlayer::stop)
        // paused: the next frame
        .addFunction("stepForward", &VideoPlayer::stepForward)
        .addFunction("getFramebuffer", &VideoPlayer::getFramebuffer)
        .addStaticFunction("canPlay", &VideoPlayer::canPlay)
        // deletes the media cache's files (of one URL, or all); the number removed
        .addStaticFunction("clearCache",
            luabridge::overload<>(+[]() -> int { return VideoPlayer::clearCache(); }),
            luabridge::overload<const std::string&>(+[](const std::string& url) -> int { return VideoPlayer::clearCache(url); }))
        // function(player) when it can play
        .addProperty("onPrepareCompleted", [] (VideoPlayer* self, lua_State*) { return &self->getOnPrepareCompleted(); }, [] (VideoPlayer* self, lua_State* L) { self->getOnPrepareCompleted() = L; })
        // function(player) when the first frame of a play shows
        .addProperty("onStarted", [] (VideoPlayer* self, lua_State*) { return &self->getOnStarted(); }, [] (VideoPlayer* self, lua_State* L) { self->getOnStarted() = L; })
        // function(player) at each loop and at the end of a play that does not loop
        .addProperty("onLoopPointReached", [] (VideoPlayer* self, lua_State*) { return &self->getOnLoopPointReached(); }, [] (VideoPlayer* self, lua_State* L) { self->getOnLoopPointReached() = L; })
        // function(player, message)
        .addProperty("onErrorReceived", [] (VideoPlayer* self, lua_State*) { return &self->getOnErrorReceived(); }, [] (VideoPlayer* self, lua_State* L) { self->getOnErrorReceived() = L; })
        // function(player) when a seek's frame shows
        .addProperty("onSeekCompleted", [] (VideoPlayer* self, lua_State*) { return &self->getOnSeekCompleted(); }, [] (VideoPlayer* self, lua_State* L) { self->getOnSeekCompleted() = L; })
        // function(player, frameIndex) each frame shown (a call per frame: set it only when needed)
        .addProperty("onFrameReady", [] (VideoPlayer* self, lua_State*) { return &self->getOnFrameReady(); }, [] (VideoPlayer* self, lua_State* L) { self->getOnFrameReady() = L; })
        .addFunction("getVideoPlayerComponent", (VideoPlayerComponent&(VideoPlayer::*)() const)&VideoPlayer::getComponent)
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
        .addProperty("outputGroup", &AudioSource::getOutputGroup, &AudioSource::setOutputGroup)
        .addProperty("loadType", &AudioSource::getLoadType, &AudioSource::setLoadType)
        .addFunction("getResolvedLoadType", &AudioSource::getResolvedLoadType)
        .addProperty("priority", &AudioSource::getPriority, &AudioSource::setPriority)
        .addProperty("maxVoices", &AudioSource::getMaxVoices, &AudioSource::setMaxVoices)
        .addProperty("ignoreListenerPause", &AudioSource::isIgnoreListenerPause, &AudioSource::setIgnoreListenerPause)
        .addProperty("pitch", &AudioSource::getPitch, &AudioSource::setPitch)
#if JASPERENGINE_SOUND_LAYER
        .addProperty("event", &AudioSource::getEvent, &AudioSource::setEvent)
        .addProperty("playOn", &AudioSource::getPlayOn, &AudioSource::setPlayOn)
        .addProperty("minImpactSpeed", &AudioSource::getMinImpactSpeed, &AudioSource::setMinImpactSpeed)
        .addProperty("fullVolumeImpactSpeed", &AudioSource::getFullVolumeImpactSpeed, &AudioSource::setFullVolumeImpactSpeed)
        .addProperty("pitchRandomSemitones", &AudioSource::getPitchRandomSemitones, &AudioSource::setPitchRandomSemitones)
        .addProperty("volumeRandomDb", &AudioSource::getVolumeRandomDb, &AudioSource::setVolumeRandomDb)
#endif
        .addFunction("playOneShot", &luaAudioPlayOneShot)
        .addFunction("fadeIn", &luaAudioFadeIn)
        .addFunction("fadeOut", &luaAudioFadeOut)
        .addFunction("fadeTo", &luaAudioFadeTo)
        .addFunction("isFading", &AudioSource::isFading)
        .addFunction("getVoiceCount", &AudioSource::getVoiceCount)
        .addStaticFunction("playClipAtPoint", &luaAudioPlayClipAtPoint)
        .addFunction("getAudioSourceComponent", &AudioSource::getComponent<AudioSourceComponent>)
        .endClass();

    // CharacterController(2D).move's result, Unity's CollisionFlags
    luabridge::getGlobalNamespace(L)
        .beginNamespace("CollisionFlags")
        .addVariable("NONE", CharacterCollisionFlags::CharacterCollision_None)
        .addVariable("SIDES", CharacterCollisionFlags::CharacterCollision_Sides)
        .addVariable("ABOVE", CharacterCollisionFlags::CharacterCollision_Above)
        .addVariable("BELOW", CharacterCollisionFlags::CharacterCollision_Below)
        .endNamespace();

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Entity, ControllerColliderHit)>>("FunctionSubscribe_V_UCCH")
        .addFunction("__call", &FunctionSubscribe<void(Entity, ControllerColliderHit)>::call)
        .addFunction("call", &FunctionSubscribe<void(Entity, ControllerColliderHit)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Entity, ControllerColliderHit)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Entity, ControllerColliderHit)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ControllerColliderHit>("ControllerColliderHit")
        .addProperty("controller", &ControllerColliderHit::getController)
        .addProperty("entity", &ControllerColliderHit::getEntity)
        .addProperty("point", &ControllerColliderHit::getPoint)
        .addProperty("normal", &ControllerColliderHit::getNormal)
        .addProperty("moveDirection", &ControllerColliderHit::getMoveDirection)
        .addProperty("moveLength", &ControllerColliderHit::getMoveLength)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<CharacterController, EntityHandle>("CharacterController")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &CharacterController::isValid)
        .addProperty("enabled", &CharacterController::isEnabled, &CharacterController::setEnabled)
        .addFunction("move", &CharacterController::move)
        .addFunction("simpleMove", &CharacterController::simpleMove)
        .addFunction("trySetSize", &CharacterController::trySetSize)
        .addProperty("center", &CharacterController::getCenter, &CharacterController::setCenter)
        .addProperty("height", &CharacterController::getHeight, &CharacterController::setHeight)
        .addProperty("radius", &CharacterController::getRadius, &CharacterController::setRadius)
        .addProperty("slopeLimit", &CharacterController::getSlopeLimit, &CharacterController::setSlopeLimit)
        .addProperty("stepOffset", &CharacterController::getStepOffset, &CharacterController::setStepOffset)
        .addProperty("skinWidth", &CharacterController::getSkinWidth, &CharacterController::setSkinWidth)
        .addProperty("minMoveDistance", &CharacterController::getMinMoveDistance, &CharacterController::setMinMoveDistance)
        .addProperty("mass", &CharacterController::getMass, &CharacterController::setMass)
        .addProperty("pushStrength", &CharacterController::getPushStrength, &CharacterController::setPushStrength)
        .addProperty("pushBodies", &CharacterController::isPushBodies, &CharacterController::setPushBodies)
        .addProperty("movingPlatforms", &CharacterController::isMovingPlatforms, &CharacterController::setMovingPlatforms)
        .addProperty("inheritPlatformYaw", &CharacterController::isInheritPlatformYaw, &CharacterController::setInheritPlatformYaw)
        .addProperty("detectCollisions", &CharacterController::isDetectCollisions, &CharacterController::setDetectCollisions)
        .addProperty("characterCollisions", &CharacterController::isCharacterCollisions, &CharacterController::setCharacterCollisions)
        .addProperty("enhancedEdgeRemoval", &CharacterController::isEnhancedEdgeRemoval, &CharacterController::setEnhancedEdgeRemoval)
        .addFunction("setBitsFilter", &CharacterController::setBitsFilter)
        .addProperty("categoryBits", &CharacterController::getCategoryBits)
        .addProperty("maskBits", &CharacterController::getMaskBits)
        .addProperty("isGrounded", &CharacterController::isGrounded)
        .addProperty("isOnSteepSlope", &CharacterController::isOnSteepSlope)
        .addProperty("collisionFlags", &CharacterController::getCollisionFlags)
        .addProperty("velocity", &CharacterController::getVelocity)
        .addProperty("groundNormal", &CharacterController::getGroundNormal)
        .addProperty("groundEntity", &CharacterController::getGroundEntity)
        .addProperty("groundVelocity", &CharacterController::getGroundVelocity)
        .endClass();
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginNamespace("BuoyancyColumns")
        .addVariable("AUTO_2x2", BuoyancyColumns::AUTO_2x2)
        .addVariable("AUTO_3x3", BuoyancyColumns::AUTO_3x3)
        .addVariable("CUSTOM", BuoyancyColumns::CUSTOM)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Buoyancy, EntityHandle>("Buoyancy")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &Buoyancy::isValid)
        .addProperty("enabled", &Buoyancy::isEnabled, &Buoyancy::setEnabled)
        .addProperty("columns", &Buoyancy::getColumns, &Buoyancy::setColumns)
        .addProperty("customPoints", &Buoyancy::getCustomPoints, &Buoyancy::setCustomPoints)
        .addProperty("volumeScale", &Buoyancy::getVolumeScale, &Buoyancy::setVolumeScale)
        .addProperty("dragScale", &Buoyancy::getDragScale, &Buoyancy::setDragScale)
        .endClass();
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .deriveClass<CharacterController2D, EntityHandle>("CharacterController2D")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &CharacterController2D::isValid)
        .addProperty("enabled", &CharacterController2D::isEnabled, &CharacterController2D::setEnabled)
        .addFunction("move", &CharacterController2D::move)
        .addFunction("simpleMove", &CharacterController2D::simpleMove)
        .addProperty("center", &CharacterController2D::getCenter, &CharacterController2D::setCenter)
        .addProperty("height", &CharacterController2D::getHeight, &CharacterController2D::setHeight)
        .addProperty("radius", &CharacterController2D::getRadius, &CharacterController2D::setRadius)
        .addProperty("slopeLimit", &CharacterController2D::getSlopeLimit, &CharacterController2D::setSlopeLimit)
        .addProperty("stepOffset", &CharacterController2D::getStepOffset, &CharacterController2D::setStepOffset)
        .addProperty("skinWidth", &CharacterController2D::getSkinWidth, &CharacterController2D::setSkinWidth)
        .addProperty("minMoveDistance", &CharacterController2D::getMinMoveDistance, &CharacterController2D::setMinMoveDistance)
        .addProperty("pushStrength", &CharacterController2D::getPushStrength, &CharacterController2D::setPushStrength)
        .addProperty("pushBodies", &CharacterController2D::isPushBodies, &CharacterController2D::setPushBodies)
        .addProperty("movingPlatforms", &CharacterController2D::isMovingPlatforms, &CharacterController2D::setMovingPlatforms)
        .addFunction("setBitsFilter", &CharacterController2D::setBitsFilter)
        .addProperty("categoryBits", &CharacterController2D::getCategoryBits)
        .addProperty("maskBits", &CharacterController2D::getMaskBits)
        .addProperty("isGrounded", &CharacterController2D::isGrounded)
        .addProperty("isOnSteepSlope", &CharacterController2D::isOnSteepSlope)
        .addProperty("collisionFlags", &CharacterController2D::getCollisionFlags)
        .addProperty("velocity", &CharacterController2D::getVelocity)
        .addProperty("groundNormal", &CharacterController2D::getGroundNormal)
        .addProperty("groundEntity", &CharacterController2D::getGroundEntity)
        .addProperty("groundVelocity", &CharacterController2D::getGroundVelocity)
        .endClass();
#endif

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
        .addProperty("valid", &Rigidbody::isValid)
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
            luabridge::overload<std::vector<Vector3>, std::vector<uint32_t>>(&Rigidbody::createMeshCollider),
            luabridge::overload<Vector3, Quaternion, std::vector<Vector3>, std::vector<uint32_t>>(&Rigidbody::createMeshCollider))
        .addFunction("createHeightFieldCollider", 
            luabridge::overload<>(&Rigidbody::createHeightFieldCollider),
            luabridge::overload<unsigned int>(&Rigidbody::createHeightFieldCollider))
        .addFunction("getNumColliders", &Rigidbody::getNumColliders)
        .addFunction("getColliderCount", &Rigidbody::getColliderCount)
        .addFunction("getColliderType", &Rigidbody::getColliderType)
        // collider edits: that collider's shape is made again and swapped in (index from 0; out
        // of range: an error)
        .addFunction("removeCollider", &Rigidbody::removeCollider)
        .addFunction("removeAllColliders", &Rigidbody::removeAllColliders)
        .addFunction("setColliderPosition", &Rigidbody::setColliderPosition)
        .addFunction("getColliderPosition", &Rigidbody::getColliderPosition)
        .addFunction("setColliderRotation", &Rigidbody::setColliderRotation)
        .addFunction("getColliderRotation", &Rigidbody::getColliderRotation)
        .addFunction("setColliderBoxSize", &Rigidbody::setColliderBoxSize)
        .addFunction("getColliderBoxSize", &Rigidbody::getColliderBoxSize)
        .addFunction("setColliderRadius", &Rigidbody::setColliderRadius)
        .addFunction("getColliderRadius", &Rigidbody::getColliderRadius)
        .addFunction("setColliderHalfHeight", &Rigidbody::setColliderHalfHeight)
        .addFunction("getColliderHalfHeight", &Rigidbody::getColliderHalfHeight)
        .addFunction("setColliderTaperedRadii", &Rigidbody::setColliderTaperedRadii)
        .addFunction("setColliderVertices",
            luabridge::overload<size_t, std::vector<Vector3>>(&Rigidbody::setColliderVertices),
            luabridge::overload<size_t, std::vector<Vector3>, std::vector<uint32_t>>(&Rigidbody::setColliderVertices))
        .addFunction("getColliderVertices", &Rigidbody::getColliderVertices)
        .addFunction("getColliderIndices", &Rigidbody::getColliderIndices)
        .addFunction("setColliderTrigger", &Rigidbody::setColliderTrigger)
        .addFunction("isColliderTrigger", &Rigidbody::isColliderTrigger)
        .addFunction("setColliderMaterial", +[](Rigidbody* self, size_t index, luabridge::LuaRef value) {
            PhysicsMaterial material;
            std::string path;
            if (!luaMaterialArg(value, material, path)){
                throw std::runtime_error("Rigidbody:setColliderMaterial: a .physicsmaterial path, a PhysicsMaterial or nil");
            }
            if (material.valid()) self->setColliderMaterial(index, material);
            else self->setColliderMaterial(index, path);
        })
        .addFunction("getColliderMaterial", +[](const Rigidbody* self, size_t index) -> std::optional<PhysicsMaterial> {
            return luaMaterialResult(self->getColliderMaterial(index));
        })
        .addFunction("setColliderFriction", &Rigidbody::setColliderFriction)
        .addFunction("setColliderBounciness", &Rigidbody::setColliderBounciness)
        .addFunction("getColliderFriction", &Rigidbody::getColliderFriction)
        .addFunction("getColliderBounciness", &Rigidbody::getColliderBounciness)
        .addProperty("shapeDensity", (float(Rigidbody::*)()const)&Rigidbody::getColliderDensity, (void(Rigidbody::*)(float))&Rigidbody::setColliderDensity)
        .addFunction("setColliderDensity", 
            luabridge::overload<float>(&Rigidbody::setColliderDensity),
            luabridge::overload<size_t, float>(&Rigidbody::setColliderDensity))
        .addFunction("getColliderDensity", 
            luabridge::overload<>(&Rigidbody::getColliderDensity),
            luabridge::overload<size_t>(&Rigidbody::getColliderDensity))
        // a switch a static-only collider refuses logs an error and keeps the type
        .addProperty("type", &Rigidbody::getType, +[](Rigidbody* self, BodyType type) { self->setType(type); })
        .addFunction("setType", &Rigidbody::setType)
        .addProperty("isKinematic", &Rigidbody::isKinematic, &Rigidbody::setKinematic)
        .addProperty("motionQuality", &Rigidbody::getMotionQuality, &Rigidbody::setMotionQuality)
        .addFunction("canBeKinematicOrDynamic", &Rigidbody::canBeKinematicOrDynamic)
        .addFunction("activate", &Rigidbody::activate)
        .addFunction("deactivate", &Rigidbody::deactivate)
        .addProperty("sensor", &Rigidbody::isSensor, &Rigidbody::setIsSensor)
        .addProperty("collideKinematicVsNonDynamic", &Rigidbody::isCollideKinematicVsNonDynamic, &Rigidbody::setCollideKinematicVsNonDynamic)
        .addFunction("setAllowedDOFsAll", &Rigidbody::setAllowedDOFsAll)
        .addFunction("setAllowedDOFs2DPlane", &Rigidbody::setAllowedDOFs2DPlane)
        .addFunction("setAllowedDOFs", &Rigidbody::setAllowedDOFs)
        // Unity's mass fields: setting mass, centerOfMass or inertiaTensor turns its automatic off
        .addProperty("mass", &Rigidbody::getMass, &Rigidbody::setMass)
        .addProperty("automaticMass", &Rigidbody::isAutomaticMass, &Rigidbody::setAutomaticMass)
        .addProperty("centerOfMass", &Rigidbody::getCenterOfMass, &Rigidbody::setCenterOfMass)
        .addProperty("automaticCenterOfMass", &Rigidbody::isAutomaticCenterOfMass, &Rigidbody::setAutomaticCenterOfMass)
        .addFunction("resetCenterOfMass", &Rigidbody::resetCenterOfMass)
        .addProperty("inertiaTensor", &Rigidbody::getInertiaTensor, &Rigidbody::setInertiaTensor)
        .addProperty("inertiaTensorRotation", &Rigidbody::getInertiaTensorRotation, &Rigidbody::setInertiaTensorRotation)
        .addProperty("automaticInertiaTensor", &Rigidbody::isAutomaticInertiaTensor, &Rigidbody::setAutomaticInertiaTensor)
        .addFunction("resetInertiaTensor", &Rigidbody::resetInertiaTensor)
        .addProperty("linearDamping", &Rigidbody::getLinearDamping, &Rigidbody::setLinearDamping)
        .addProperty("angularDamping", &Rigidbody::getAngularDamping, &Rigidbody::setAngularDamping)
        .addProperty("maxLinearVelocity", &Rigidbody::getMaxLinearVelocity, &Rigidbody::setMaxLinearVelocity)
        .addProperty("maxAngularVelocity", &Rigidbody::getMaxAngularVelocity, &Rigidbody::setMaxAngularVelocity)
        .addProperty("interpolation", &Rigidbody::getInterpolation, &Rigidbody::setInterpolation)
        .addFunction("isSleeping", &Rigidbody::isSleeping)
        .addFunction("sleep", &Rigidbody::sleep)
        .addFunction("wakeUp", &Rigidbody::wakeUp)
        // the default material of the colliders without one: a path, a PhysicsMaterial or nil
        .addProperty("material",
            +[](const Rigidbody* self) -> std::optional<PhysicsMaterial> { return luaMaterialResult(self->getMaterial()); },
            +[](Rigidbody* self, luabridge::LuaRef value) {
                PhysicsMaterial material;
                std::string path;
                if (!luaMaterialArg(value, material, path)){
                    throw std::runtime_error("Rigidbody.material: a .physicsmaterial path, a PhysicsMaterial or nil");
                }
                if (material.valid()) self->setMaterial(material);
                else self->setMaterial(path);
            })
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
        .deriveClass<Joint3D, EntityHandle>("Joint3D")
        .addConstructor <void (Scene*), void (Scene*, Entity)> ()
        // as before local anchors: world points and axes, the other fields as Jolt had them
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
        .addProperty("enabled", &Joint3D::isEnabled, &Joint3D::setEnabled)
        // the body the joint is on (Unity: the joint's Rigidbody) and the connected one; set with
        // a GameObject, a Rigidbody or nil (bodyB nil: held to the world, LOCAL joints)
        .addProperty("bodyA",
            +[](const Joint3D* self) { return luaJoint3DBody(self, self->getBodyA()); },
            +[](Joint3D* self, luabridge::LuaRef value) { self->setBodyA(luaJoint3DBodyArg(self, value, value.state(), "bodyA")); })
        .addProperty("bodyB",
            +[](const Joint3D* self) { return luaJoint3DBody(self, self->getBodyB()); },
            +[](Joint3D* self, luabridge::LuaRef value) { self->setBodyB(luaJoint3DBodyArg(self, value, value.state(), "bodyB")); })
        .addProperty("connectedBody",
            +[](const Joint3D* self) { return luaJoint3DBody(self, self->getBodyB()); },
            +[](Joint3D* self, luabridge::LuaRef value) { self->setBodyB(luaJoint3DBodyArg(self, value, value.state(), "connectedBody")); })
        .addProperty("type", &Joint3D::getType, &Joint3D::setType)
        .addProperty("anchorSpace", &Joint3D::getAnchorSpace, &Joint3D::setAnchorSpace)
        .addProperty("anchor", &Joint3D::getAnchor, &Joint3D::setAnchor)
        .addProperty("anchorA", &Joint3D::getAnchorA, &Joint3D::setAnchorA)
        .addProperty("anchorB", &Joint3D::getAnchorB, &Joint3D::setAnchorB)
        .addProperty("connectedAnchor", &Joint3D::getAnchorB, &Joint3D::setAnchorB)
        .addProperty("autoAnchors", &Joint3D::isAutoAnchors, &Joint3D::setAutoAnchors)
        .addProperty("autoConfigureConnectedAnchor", &Joint3D::isAutoAnchors, &Joint3D::setAutoAnchors)
        .addProperty("axis", &Joint3D::getAxis, &Joint3D::setAxis)
        .addProperty("normal", &Joint3D::getNormal, &Joint3D::setNormal)
        .addProperty("secondaryAxis", &Joint3D::getNormal, &Joint3D::setNormal)
        .addProperty("twistAxis", &Joint3D::getTwistAxis, &Joint3D::setTwistAxis)
        .addProperty("planeAxis", &Joint3D::getPlaneAxis, &Joint3D::setPlaneAxis)
        .addProperty("swingAxis", &Joint3D::getPlaneAxis, &Joint3D::setPlaneAxis)
        .addProperty("axisX", &Joint3D::getAxisX, &Joint3D::setAxisX)
        .addProperty("axisY", &Joint3D::getAxisY, &Joint3D::setAxisY)
        .addProperty("swingType", &Joint3D::getSwingType, &Joint3D::setSwingType)
        .addProperty("xMotion", &Joint3D::getXMotion, &Joint3D::setXMotion)
        .addProperty("yMotion", &Joint3D::getYMotion, &Joint3D::setYMotion)
        .addProperty("zMotion", &Joint3D::getZMotion, &Joint3D::setZMotion)
        .addProperty("angularXMotion", &Joint3D::getAngularXMotion, &Joint3D::setAngularXMotion)
        .addProperty("angularYMotion", &Joint3D::getAngularYMotion, &Joint3D::setAngularYMotion)
        .addProperty("angularZMotion", &Joint3D::getAngularZMotion, &Joint3D::setAngularZMotion)
        .addProperty("linearLimitMin", &Joint3D::getLinearLimitMin, &Joint3D::setLinearLimitMin)
        .addProperty("linearLimitMax", &Joint3D::getLinearLimitMax, &Joint3D::setLinearLimitMax)
        .addProperty("angularLimitMin", &Joint3D::getAngularLimitMin, &Joint3D::setAngularLimitMin)
        .addProperty("angularLimitMax", &Joint3D::getAngularLimitMax, &Joint3D::setAngularLimitMax)
        .addProperty("enableCollision", &Joint3D::isEnableCollision, &Joint3D::setEnableCollision)
        .addProperty("breakForce", &Joint3D::getBreakForce, &Joint3D::setBreakForce)
        .addProperty("breakTorque", &Joint3D::getBreakTorque, &Joint3D::setBreakTorque)
        .addProperty("breakAction", &Joint3D::getBreakAction, &Joint3D::setBreakAction)
        .addProperty("useLimits", &Joint3D::isUseLimits, &Joint3D::setUseLimits)
        .addProperty("limitsMin", &Joint3D::getLimitsMin, &Joint3D::setLimitsMin)
        .addProperty("limitsMax", &Joint3D::getLimitsMax, &Joint3D::setLimitsMax)
        .addProperty("limitsSpringFrequency", &Joint3D::getLimitsSpringFrequency, &Joint3D::setLimitsSpringFrequency)
        .addProperty("limitsSpringDamping", &Joint3D::getLimitsSpringDamping, &Joint3D::setLimitsSpringDamping)
        .addProperty("useMotor", &Joint3D::isUseMotor, &Joint3D::setUseMotor)
        .addProperty("motorTargetVelocity", &Joint3D::getMotorTargetVelocity, &Joint3D::setMotorTargetVelocity)
        .addProperty("motorMaxForce", &Joint3D::getMotorMaxForce, &Joint3D::setMotorMaxForce)
        .addProperty("useSpring", &Joint3D::isUseSpring, &Joint3D::setUseSpring)
        .addProperty("springStiffness", &Joint3D::getSpringStiffness, &Joint3D::setSpringStiffness)
        .addProperty("springDamper", &Joint3D::getSpringDamper, &Joint3D::setSpringDamper)
        .addProperty("springTargetPosition", &Joint3D::getSpringTargetPosition, &Joint3D::setSpringTargetPosition)
        .addProperty("maxFriction", &Joint3D::getMaxFriction, &Joint3D::setMaxFriction)
        .addProperty("minDistance", &Joint3D::getMinDistance, &Joint3D::setMinDistance)
        .addProperty("maxDistance", &Joint3D::getMaxDistance, &Joint3D::setMaxDistance)
        .addProperty("coneHalfAngle", &Joint3D::getConeHalfAngle, &Joint3D::setConeHalfAngle)
        .addProperty("normalHalfConeAngle", &Joint3D::getNormalHalfConeAngle, &Joint3D::setNormalHalfConeAngle)
        // Unity swing1Limit turns about the swing axis (planeAxis): Jolt's normal half cone
        .addProperty("swing1Limit", &Joint3D::getNormalHalfConeAngle, &Joint3D::setNormalHalfConeAngle)
        .addProperty("planeHalfConeAngle", &Joint3D::getPlaneHalfConeAngle, &Joint3D::setPlaneHalfConeAngle)
        .addProperty("swing2Limit", &Joint3D::getPlaneHalfConeAngle, &Joint3D::setPlaneHalfConeAngle)
        .addProperty("twistMinAngle", &Joint3D::getTwistMinAngle, &Joint3D::setTwistMinAngle)
        .addProperty("lowTwistLimit", &Joint3D::getTwistMinAngle, &Joint3D::setTwistMinAngle)
        .addProperty("twistMaxAngle", &Joint3D::getTwistMaxAngle, &Joint3D::setTwistMaxAngle)
        .addProperty("highTwistLimit", &Joint3D::getTwistMaxAngle, &Joint3D::setTwistMaxAngle)
        .addProperty("motorTargetRotation", &Joint3D::getMotorTargetRotation, &Joint3D::setMotorTargetRotation)
        .addProperty("motorSpringFrequency", &Joint3D::getMotorSpringFrequency, &Joint3D::setMotorSpringFrequency)
        .addProperty("motorSpringDamping", &Joint3D::getMotorSpringDamping, &Joint3D::setMotorSpringDamping)
        .addProperty("useDrive", &Joint3D::isUseDrive, &Joint3D::setUseDrive)
        .addProperty("driveStiffness", &Joint3D::getDriveStiffness, &Joint3D::setDriveStiffness)
        .addProperty("driveDamping", &Joint3D::getDriveDamping, &Joint3D::setDriveDamping)
        .addProperty("angularDriveStiffness", &Joint3D::getAngularDriveStiffness, &Joint3D::setAngularDriveStiffness)
        .addProperty("angularDriveDamping", &Joint3D::getAngularDriveDamping, &Joint3D::setAngularDriveDamping)
        .addProperty("driveMaxForce", &Joint3D::getDriveMaxForce, &Joint3D::setDriveMaxForce)
        .addProperty("driveMaxTorque", &Joint3D::getDriveMaxTorque, &Joint3D::setDriveMaxTorque)
        .addProperty("targetPosition", &Joint3D::getTargetPosition, &Joint3D::setTargetPosition)
        .addProperty("targetRotation", &Joint3D::getTargetRotation, &Joint3D::setTargetRotation)
        .addProperty("targetVelocity", &Joint3D::getTargetVelocity, &Joint3D::setTargetVelocity)
        .addProperty("targetAngularVelocity", &Joint3D::getTargetAngularVelocity, &Joint3D::setTargetAngularVelocity)
        // read-outs of the live joint (0 without one): N, N m, angles in the engine's unit
        .addProperty("currentForce", &Joint3D::getCurrentForce)
        .addProperty("currentTorque", &Joint3D::getCurrentTorque)
        .addProperty("angle", &Joint3D::getAngle)
        .addProperty("velocity", &Joint3D::getVelocity)
        .addProperty("position", &Joint3D::getPosition)
        .addProperty("speed", &Joint3D::getSpeed)
        .addProperty("currentLength", &Joint3D::getCurrentLength)
        .addProperty("broken", &Joint3D::isBroken)
        .addProperty("live", &Joint3D::isLive)
        .endClass();

    // a character's ragdoll: animated (kinematic parts following the bones) or simulated
    luabridge::getGlobalNamespace(L)
        .deriveClass<Ragdoll, EntityHandle>("Ragdoll")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("enabled", &Ragdoll::isEnabled, &Ragdoll::setEnabled)
        .addProperty("simulated", &Ragdoll::isSimulated, &Ragdoll::setSimulated)
        .addFunction("setSimulated", &Ragdoll::setSimulated)
        .addProperty("inheritVelocity", &Ragdoll::isInheritVelocity, &Ragdoll::setInheritVelocity)
        .addProperty("selfCollision", &Ragdoll::isSelfCollision, &Ragdoll::setSelfCollision)
        .addProperty("totalMass", &Ragdoll::getTotalMass, &Ragdoll::setTotalMass)
        .addProperty("root",
            +[](const Ragdoll* self) -> std::optional<GameObject> {
                Entity root = self->getRoot();
                if (root == NULL_ENTITY || !self->getScene()->isEntityCreated(root)) return std::nullopt;
                return GameObject(self->getScene(), root);
            },
            +[](Ragdoll* self, luabridge::LuaRef value) { self->setRoot(luaEntityArg(self->getScene(), value, value.state(), "Ragdoll.root")); })
        .addProperty("animator",
            +[](const Ragdoll* self) -> std::optional<GameObject> {
                Entity animator = self->getAnimator();
                if (animator == NULL_ENTITY || !self->getScene()->isEntityCreated(animator)) return std::nullopt;
                return GameObject(self->getScene(), animator);
            },
            +[](Ragdoll* self, luabridge::LuaRef value) { self->setAnimator(luaEntityArg(self->getScene(), value, value.state(), "Ragdoll.animator")); })
        .addProperty("partCount", &Ragdoll::getPartCount)
        .addFunction("getParts", &Ragdoll::getParts)
        .addFunction("applyImpulse", &Ragdoll::applyImpulse)
        .addFunction("applyForce", &Ragdoll::applyForce)
        // RegisterEvent(self, ragdoll.onBeforeAnimatedRestore, "fn"): fn(entity) before a ragdoll
        // of the scene goes back to animated
        .addProperty("onBeforeAnimatedRestore", [] (Ragdoll* self, lua_State* L) { return &self->onBeforeAnimatedRestore(); }, [] (Ragdoll* self, lua_State* L) { self->onBeforeAnimatedRestore() = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Contact3D>("Contact3D")
        .addFunction("getBaseOffset", &Contact3D::getBaseOffset)
        .addFunction("getWorldSpaceNormal", &Contact3D::getWorldSpaceNormal)
#if JASPERENGINE_SOUND_LAYER
        .addFunction("getApproachSpeed", &Contact3D::getApproachSpeed)
#endif
        .addFunction("getPenetrationDepth", &Contact3D::getPenetrationDepth)
        .addFunction("getColliderIndex1", &Contact3D::getColliderIndex1)
        .addFunction("getColliderIndex2", &Contact3D::getColliderIndex2)
        .addFunction("getContactPointCount", &Contact3D::getContactPointCount)
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
