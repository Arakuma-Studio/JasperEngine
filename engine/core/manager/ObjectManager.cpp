// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ObjectManager.h"
#include "script/ScriptScheduler.h"
#include "script/AsyncOwner.h"
#include "PrefabManager.h"
#include "PrefabAsset.h"
#include "Engine.h"
#include "Scene.h"
#include "Log.h"
#include "SceneManager.h"
#include "registry/EntityCloner.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"
#include "subsystem/CameraControlSystem.h"
#include "script/ScriptMessages.h"
#include "script/ScriptBase.h"
#include "component/Transform.h"
#include "component/RectTransformComponent.h"
#include "component/ScriptComponent.h"
#include "audio/AudioMarkers.h"

#include <algorithm>
#include <set>
#include <unordered_set>

using namespace jasperengine;

namespace {

ObjectManager::ScriptHook cppInitializeHook = nullptr;
ObjectManager::ScriptHook cppCleanupHook = nullptr;
ObjectManager::ScriptMoveHook cppMoveHook = nullptr;

struct PendingDestroy {
    Scene* scene;
    Entity entity;
    double timeLeft;
};
std::vector<PendingDestroy> pendingDestroys;

std::vector<Scene*> activeScenes;

ObjectManager::DestroyListener destroyListener;
ObjectManager::MoveListener moveListener;

struct WorldPose {
    Matrix4 matrix;
    Quaternion rotation;
    Vector3 scale = Vector3(1, 1, 1);
};

// The world placement from the Transforms as they are now: what the RenderSystem would
// compute this frame, for GameObjects it has not updated yet (one just made)
WorldPose worldPose(Scene* scene, Entity entity, int depth = 0) {
    WorldPose pose;
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || depth > 1024) {
        return pose;
    }
    Vector3 origin = transform->position;
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    if (layout && layout->pivot != Vector2(0, 0)) {
        origin += getUIPivotShift(*layout, transform->rotation, transform->scale);
    }
    const Matrix4 local = Matrix4::translateMatrix(origin) * transform->rotation.getRotationMatrix() * Matrix4::scaleMatrix(transform->scale);
    if (transform->parent != NULL_ENTITY && scene->isEntityCreated(transform->parent)) {
        WorldPose parent = worldPose(scene, transform->parent, depth + 1);
        pose.matrix = parent.matrix * local;
        pose.rotation = parent.rotation * transform->rotation;
        pose.scale = parent.scale * transform->scale;
    } else {
        pose.matrix = local;
        pose.rotation = transform->rotation;
        pose.scale = transform->scale;
    }
    return pose;
}

Vector3 worldPosition(Scene* scene, Entity entity) {
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform) {
        return Vector3(0, 0, 0);
    }
    if (transform->parent != NULL_ENTITY && scene->isEntityCreated(transform->parent)) {
        return worldPose(scene, transform->parent).matrix * transform->position;
    }
    return transform->position;
}

float safeDivide(float value, float by) {
    return by != 0.0f ? value / by : value;
}

// Sets the local placement of `entity` (below `parent`, or at the top when NULL_ENTITY)
// that puts it at this world position, rotation and scale
void setWorldPlacement(Scene* scene, Entity entity, Entity parent, const Vector3& position, const Quaternion& rotation, const Vector3* scale) {
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform) {
        return;
    }
    if (parent != NULL_ENTITY && scene->findComponent<Transform>(parent)) {
        const WorldPose parentPose = worldPose(scene, parent);
        transform->position = parentPose.matrix.inverse() * position;
        transform->rotation = parentPose.rotation.inverse() * rotation;
        if (scale) {
            transform->scale = Vector3(safeDivide(scale->x, parentPose.scale.x), safeDivide(scale->y, parentPose.scale.y), safeDivide(scale->z, parentPose.scale.z));
        }
    } else {
        transform->position = position;
        transform->rotation = rotation;
        if (scale) {
            transform->scale = *scale;
        }
    }
    transform->needUpdate = true;
}

// Puts the new root below its parent (keeping its local placement) and the arrays kept
// in Transform order back in it
void attachRoot(Scene* scene, Entity root, Entity parent, const std::vector<Entity>& entities) {
    if (parent == NULL_ENTITY || !scene->findComponent<Transform>(root)) {
        return;
    }
    if (!scene->isEntityCreated(parent) || !scene->findComponent<Transform>(parent)) {
        Log::error("Instantiate: the parent %u has no Transform, the new GameObject stays at the top", parent);
        return;
    }
    scene->addEntityChild(parent, root, false);
    Signature signature;
    for (Entity entity : entities) {
        signature |= scene->getSignature(entity);
    }
    scene->sortComponentsByTransform(signature);
}

void nameClone(Scene* scene, Entity root) {
    scene->setEntityName(root, scene->getEntityName(root) + "(Clone)");
}

// Unity copies the current values of a script's serialized fields, not the ones it
// started with
void readScriptValues(Scene* scene, const std::vector<Entity>& entities) {
    for (Entity entity : entities) {
        ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
        if (!scripts) {
            continue;
        }
        for (ScriptEntry& entry : scripts->scripts) {
            if (!entry.instance) {
                continue;
            }
            for (ScriptProperty& property : entry.properties) {
                if (property.memberPtr || property.luaRef != 0) {
                    property.syncFromMember();
                }
            }
        }
    }
}

// What the prefab factory made: user ids count up, so everything above the counter as it
// was before, the root first
std::vector<Entity> createdSince(Scene* scene, Entity lastBefore, Entity root) {
    std::vector<Entity> created;
    created.push_back(root);
    const Entity lastAfter = scene->getLastEntity();
    for (Entity e = lastBefore + 1; e <= lastAfter && e != NULL_ENTITY; e++) {
        if (e != root && scene->isEntityCreated(e)) {
            created.push_back(e);
        }
    }
    return created;
}

Entity instantiatePrefab(const PrefabAsset& prefab, Scene* scene, std::vector<Entity>& created) {
    if (!scene) {
        Log::error("Instantiate: no scene to instantiate the prefab in");
        return NULL_ENTITY;
    }
    const uint32_t id = prefab.getId();
    if (id == 0) {
        if (prefab.isEmpty()) {
            Log::error("Instantiate: the prefab is None");
        } else {
            Log::error("Instantiate: prefab '%s' is not in the build", prefab.getPath().c_str());
        }
        return NULL_ENTITY;
    }
    const Entity lastBefore = scene->getLastEntity();
    const Entity root = PrefabManager::instantiate(id, scene);
    if (root == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    created = createdSince(scene, lastBefore, root);
    nameClone(scene, root);
    return root;
}

// The copy's init / onEnable may have destroyed it at once (destroyImmediate): the caller then
// gets no GameObject (Unity's fake null), not the id of a dead one
Entity liveRoot(Scene* scene, Entity root) {
    return scene->isEntityCreated(root) ? root : NULL_ENTITY;
}

Entity cloneObject(Scene* scene, Entity original, Scene* target, std::vector<Entity>& created) {
    if (!scene || original == NULL_ENTITY || !scene->isEntityCreated(original)) {
        Log::error("Instantiate: the GameObject to copy does not exist");
        return NULL_ENTITY;
    }
    std::vector<Entity> sources = EntityCloner::collectHierarchy(scene, original);
    readScriptValues(scene, sources);
    created = EntityCloner::clone(scene, sources, target);
    if (created.empty()) {
        return NULL_ENTITY;
    }
    nameClone(target, created[0]);
    return created[0];
}

}

void ObjectManager::setDestroyListener(DestroyListener listener) {
    destroyListener = std::move(listener);
}

void ObjectManager::setCppScriptMoveHook(ScriptMoveHook hook) {
    cppMoveHook = hook;
}

void ObjectManager::setMoveListener(MoveListener listener) {
    moveListener = std::move(listener);
}

void ObjectManager::setCppScriptHooks(ScriptHook initialize, ScriptHook cleanup) {
    cppInitializeHook = initialize;
    cppCleanupHook = cleanup;
}

Entity ObjectManager::instantiate(const PrefabAsset& prefab, Scene* scene, Entity parent, bool worldSpace) {
    scene = scene ? scene : getActiveScene();
    std::vector<Entity> created;
    const Entity root = instantiatePrefab(prefab, scene, created);
    if (root == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    if (parent != NULL_ENTITY) {
        if (worldSpace) {
            // the prefab root's placement is its world placement
            if (Transform* transform = scene->findComponent<Transform>(root)) {
                const Vector3 position = transform->position;
                const Quaternion rotation = transform->rotation;
                const Vector3 scale = transform->scale;
                setWorldPlacement(scene, root, parent, position, rotation, &scale);
            }
        }
        attachRoot(scene, root, parent, created);
    }
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::spawn(scene, root, prefab.getPath(), prefab.getGuid());
#endif
    initializeScripts(scene, created);
    return liveRoot(scene, root);
}

Entity ObjectManager::instantiate(const PrefabAsset& prefab, Scene* scene, const Vector3& position, const Quaternion& rotation, Entity parent) {
    scene = scene ? scene : getActiveScene();
    std::vector<Entity> created;
    const Entity root = instantiatePrefab(prefab, scene, created);
    if (root == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    setWorldPlacement(scene, root, parent, position, rotation, nullptr);
    attachRoot(scene, root, parent, created);
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::spawn(scene, root, prefab.getPath(), prefab.getGuid());
#endif
    initializeScripts(scene, created);
    return liveRoot(scene, root);
}

Entity ObjectManager::instantiate(Scene* scene, Entity original, Scene* targetScene, Entity parent, bool worldSpace) {
    Scene* target = targetScene ? targetScene : scene;
    std::vector<Entity> created;
    const Entity root = cloneObject(scene, original, target, created);
    if (root == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    if (Transform* source = scene->findComponent<Transform>(original)) {
        if (parent == NULL_ENTITY || worldSpace) {
            // where the original is in the world (its own scale, Unity's localScale)
            const WorldPose pose = worldPose(scene, original);
            const Vector3 position = worldPosition(scene, original);
            const Vector3 scale = parent == NULL_ENTITY ? source->scale : pose.scale;
            setWorldPlacement(target, root, parent, position, pose.rotation, &scale);
        }
    }
    attachRoot(target, root, parent, created);
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::spawn(target, root, "", "");
#endif
    initializeScripts(target, created);
    return liveRoot(target, root);
}

Entity ObjectManager::instantiate(Scene* scene, Entity original, Scene* targetScene, const Vector3& position, const Quaternion& rotation, Entity parent) {
    Scene* target = targetScene ? targetScene : scene;
    std::vector<Entity> created;
    const Entity root = cloneObject(scene, original, target, created);
    if (root == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    setWorldPlacement(target, root, parent, position, rotation, nullptr);
    attachRoot(target, root, parent, created);
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::spawn(target, root, "", "");
#endif
    initializeScripts(target, created);
    return liveRoot(target, root);
}

Scene* ObjectManager::getActiveScene() {
    if (!activeScenes.empty()) {
        return activeScenes.back();
    }
    return Engine::getScene();
}

void ObjectManager::pushActiveScene(Scene* scene) {
    activeScenes.push_back(scene);
}

void ObjectManager::popActiveScene() {
    if (!activeScenes.empty()) {
        activeScenes.pop_back();
    }
}

std::vector<Scene*> ObjectManager::getActiveSceneStack() {
    return activeScenes;
}

namespace {

// GameObjects being destroyed right now: a callback their destruction runs (onDestroy, a
// physics Exit to the GameObject they touched) may destroy them again, which does nothing
std::set<std::pair<Scene*, Entity>> destroying;

bool isBeingDestroyed(Scene* scene, Entity entity) {
    return !destroying.empty() && destroying.count({scene, entity}) > 0;
}

}

void ObjectManager::destroy(Scene* scene, Entity entity, float delay) {
    // one being destroyed now is gone at once (its id may be reused before a deferred destroy)
    if (!scene || entity == NULL_ENTITY || isBeingDestroyed(scene, entity)) {
        return;
    }
    pendingDestroys.push_back({scene, entity, delay > 0.0f ? (double)delay : 0.0});
}

namespace {

// GameObjects of one scene with their children: scripts stopped first, then destroyed
// together (one pass over the component arrays)
void destroyNow(Scene* scene, const std::vector<Entity>& roots) {
    std::vector<Entity> entities;
    std::unordered_set<Entity> seen;
    for (Entity root : roots) {
        if (root == NULL_ENTITY || seen.count(root) || !scene->isEntityCreated(root) || isBeingDestroyed(scene, root)) {
            continue;
        }
        for (Entity entity : EntityCloner::collectHierarchy(scene, root)) {
            if (!isBeingDestroyed(scene, entity) && seen.insert(entity).second) {
                entities.push_back(entity);
            }
        }
    }
    if (entities.empty()) {
        return;
    }
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) {
        std::unordered_set<Entity> marked;
        for (Entity root : roots) {
            if (seen.count(root) && marked.insert(root).second) AudioMarkers::destroy(scene, root);
        }
    }
#endif
    for (Entity entity : entities) {
        destroying.insert({scene, entity});
    }
    ObjectManager::cleanupScripts(scene, entities);
    // children first, a model taking the GameObjects it made along
    std::reverse(entities.begin(), entities.end());
    scene->destroyEntities(entities);
    for (Entity entity : entities) {
        destroying.erase({scene, entity});
    }
    if (destroyListener) {
        destroyListener(scene, entities);
    }
}

}

bool ObjectManager::isDestroyPending(Scene* scene, Entity entity) {
    if (!scene || pendingDestroys.empty()) {
        return false;
    }
    for (int depth = 0; entity != NULL_ENTITY && depth < 4096; depth++) {
        for (const PendingDestroy& pending : pendingDestroys) {
            if (pending.scene == scene && pending.entity == entity && pending.timeLeft <= 0.0) {
                return true;
            }
        }
        const Transform* transform = scene->isEntityCreated(entity) ? scene->findComponent<Transform>(entity) : nullptr;
        entity = transform ? transform->parent : NULL_ENTITY;
    }
    return false;
}

void ObjectManager::destroyImmediate(Scene* scene, Entity entity) {
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
        return;
    }
    // inside a dispatch loop (StructuralDeferScope) it goes when the loop is done
    if (scene->isStructuralDeferActive()) {
        scene->destroyHierarchy(entity);
        return;
    }
    destroyNow(scene, {entity});
}

namespace {

// Which kinds of scripts these GameObjects have
void scriptKinds(Scene* scene, const std::vector<Entity>& entities, bool& lua, bool& cpp) {
    lua = false;
    cpp = false;
    for (Entity entity : entities) {
        if (!scene->isEntityCreated(entity)) {
            continue;
        }
        if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity)) {
            for (const ScriptEntry& entry : scripts->scripts) {
                (entry.type == ScriptType::LUA ? lua : cpp) = true;
            }
        }
    }
}

}

void ObjectManager::initializeScripts(Scene* scene, const std::vector<Entity>& entities) {
    if (!scene || entities.empty()) {
        return;
    }
    bool lua, cpp;
    scriptKinds(scene, entities, lua, cpp);
    if (!lua && !cpp) {
        return;
    }
    // Lua instances first, as initScripts does for a scene, then the C++ ones, then their
    // init and onEnable; what they instantiate goes to their scene
    pushActiveScene(scene);
    if (lua) {
        LuaBinding::createLuaScripts(scene, entities);
    }
    if (cpp && cppInitializeHook) {
        cppInitializeHook(scene, entities.data(), entities.size());
    }
    ScriptRuntime::awakeEntities(scene, entities);
    popActiveScene();
}

void ObjectManager::cleanupScripts(Scene* scene, const std::vector<Entity>& entities) {
    if (!scene || entities.empty()) {
        return;
    }
    bool lua, cpp;
    scriptKinds(scene, entities, lua, cpp);
    if (!lua && !cpp) {
        return;
    }
    pushActiveScene(scene);
    // onDisable, then onDestroy of every script, while the others still run
    ScriptRuntime::destroyEntities(scene, entities);
    if (lua) {
        LuaBinding::cleanupLuaScripts(scene, entities);
    }
    if (cpp && cppCleanupHook) {
        cppCleanupHook(scene, entities.data(), entities.size());
    }
    popActiveScene();
}

std::vector<Entity> ObjectManager::moveToScene(Scene* from, Entity root, Scene* to) {
    if (!from || !to || from == to || root == NULL_ENTITY || !from->isEntityCreated(root)) {
        return {};
    }
    const std::vector<Entity> sources = EntityCloner::collectHierarchy(from, root);
    std::unordered_map<Entity, Entity> map;
    // the copies get the originals' running state; the originals keep none of it
    const std::vector<Entity> copies = EntityCloner::move(from, sources, to, map);
    if (copies.empty()) {
        return {};
    }

    // the scripts follow their GameObjects: Lua self.scene / self.entity (and the wrappers of
    // moved GameObjects they hold), C++ scripts, the lifecycle states, the cached receivers
    LuaBinding::rebindLuaScripts(from, to, map);
    std::vector<Entity> movedOld;
    std::vector<Entity> movedNew;
    for (size_t i = 0; i < sources.size() && i < copies.size(); i++) {
        ScriptComponent* scripts = to->findComponent<ScriptComponent>(copies[i]);
        if (!scripts) {
            continue;
        }
        movedOld.push_back(sources[i]);
        movedNew.push_back(copies[i]);
        for (ScriptEntry& entry : scripts->scripts) {
            if (entry.cppScript) {
                entry.cppScript->rebind(to, copies[i]);
            }
            // its coroutines and timers follow their owner (they keep running)
            const void* owner = entry.type == ScriptType::LUA ? entry.instance : static_cast<const void*>(entry.cppScript);
            if (owner) {
                ScriptScheduler::rebind(owner, to, copies[i]);
            }
        }
    }
    // every moved GameObject (old -> new): the generated code finds the scripts among them and
    // rebinds the wrappers their properties own for any of them (a child without a script too)
    if (cppMoveHook && !movedOld.empty()) {
        const size_t count = std::min(sources.size(), copies.size());
        cppMoveHook(from, to, sources.data(), copies.data(), count);
    }
    ScriptRuntime::remap(from, to, map);
    // the old scene's Camera Follows of a moved GameObject follow it into the persistent scene
    if (auto cameras = from->findSystem<CameraControlSystem>()) cameras->remapTargets(to, map);
    for (size_t i = 0; i < movedOld.size(); i++) {
        ScriptMessages::notifyReceiversChanged(from, movedOld[i]);
        ScriptMessages::notifyReceiversChanged(to, movedNew[i]);
    }

    // GameObject.destroy calls still pending (a delay) go with them
    for (PendingDestroy& pending : pendingDestroys) {
        if (pending.scene != from) {
            continue;
        }
        auto moved = map.find(pending.entity);
        if (moved != map.end()) {
            pending.scene = to;
            pending.entity = moved->second;
        }
    }

    // a scene whose camera moved away draws with its default camera again (cameras draw their
    // own scene: a camera made persistent no longer shows the level)
    if (map.count(from->getCamera())) {
        Log::warn("'%s' was the camera of the scene it left: that scene now draws with a default camera "
                  "(keep a camera in each level and persist a manager that positions it)",
                  from->getEntityName(from->getCamera()).c_str());
        from->useDefaultCamera();
    }

    if (moveListener) {
        moveListener(from, to, map);
    }

    // async work (HTTP requests, native calls) owned by the moved GameObjects or their scripts
    // follows them: their destroyed originals end nothing
    AsyncOwners::entitiesMoved(from, to, map);

    // the originals hold nothing any more (EntityCloner::move): no callback, no sound stops,
    // no instance is released; children first, as destroy does
    std::vector<Entity> originals(sources.rbegin(), sources.rend());
    from->destroyEntities(originals);

    return copies;
}

void ObjectManager::update(double deltatime) {
    // DontDestroyOnLoad / MoveGameObjectToScene asked for this frame: now, between the update
    // and the destroys (a GameObject destroyed before it moved does not move)
    SceneManager::flushPersistentMoves();

    if (pendingDestroys.empty()) {
        return;
    }
    // what is due now; a script stopped here may ask for more, which waits for the next frame
    std::vector<PendingDestroy> due;
    std::vector<PendingDestroy> waiting;
    waiting.reserve(pendingDestroys.size());
    for (PendingDestroy& entry : pendingDestroys) {
        entry.timeLeft -= deltatime;
        if (entry.timeLeft <= 0.0) {
            due.push_back(entry);
        } else {
            waiting.push_back(entry);
        }
    }
    pendingDestroys.swap(waiting);
    // by scene, each scene's in one pass
    while (!due.empty()) {
        Scene* scene = due.front().scene;
        std::vector<Entity> roots;
        std::vector<PendingDestroy> others;
        for (const PendingDestroy& entry : due) {
            (entry.scene == scene ? roots.push_back(entry.entity) : others.push_back(entry));
        }
        destroyNow(scene, roots);
        due.swap(others);
    }
}

void ObjectManager::forgetScene(Scene* scene) {
    SceneManager::forgetPendingMoves(scene);
    activeScenes.erase(std::remove(activeScenes.begin(), activeScenes.end(), scene), activeScenes.end());
    pendingDestroys.erase(std::remove_if(pendingDestroys.begin(), pendingDestroys.end(),
        [scene](const PendingDestroy& entry) { return entry.scene == scene; }), pendingDestroys.end());
}

void ObjectManager::clearAll() {
    pendingDestroys.clear();
    activeScenes.clear();
    destroyListener = nullptr;
    moveListener = nullptr;
}
