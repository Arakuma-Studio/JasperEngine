// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef OBJECTMANAGER_H
#define OBJECTMANAGER_H

#include "Export.h"
#include "Entity.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include <cstddef>
#include <functional>
#include <vector>

namespace jasperengine {

    class Scene;
    class PrefabAsset;

    // Unity's Object.Instantiate / Destroy / DestroyImmediate behind GameObject::instantiate,
    // GameObject::destroy and GameObject::destroyImmediate (Lua: GameObject.instantiate,
    // GameObject.destroy, GameObject.destroyImmediate).
    //
    // Instantiate makes a GameObject from a prefab (PrefabManager) or copies one (with its
    // children, components and scripts with their property values; references among the
    // copied GameObjects go to the copies), names its root "<name>(Clone)", places it, and
    // starts the scripts of every new GameObject (instances, C++ constructors, init) before it
    // returns (init and onEnable ran; start waits for the next frame). Destroy removes a
    // GameObject and its children at the end of the frame or after a delay, stopping their
    // scripts first (onDisable, onDestroy, then the instances: C++ destructors).
    class JASPERENGINE_API ObjectManager {
    public:
        // C++ scripts are compiled with the game: the game (generated main.cpp) or the
        // editor's Play (its script library) hand over the functions that create and delete
        // the script instances of some GameObjects
        using ScriptHook = void (*)(Scene* scene, const Entity* entities, size_t count);
        static void setCppScriptHooks(ScriptHook initialize, ScriptHook cleanup);

        // Told of the GameObjects destroy/destroyImmediate removed (after they are gone): the
        // editor's Play drops them from what it shows
        using DestroyListener = std::function<void(Scene* scene, const std::vector<Entity>& entities)>;
        static void setDestroyListener(DestroyListener listener);

        // A prefab, in `scene` (the active scene when null). With a parent the root goes
        // below it, keeping the prefab root's local placement, or its world placement with
        // worldSpace. NULL_ENTITY on failure (logged).
        static Entity instantiate(const PrefabAsset& prefab, Scene* scene, Entity parent = NULL_ENTITY, bool worldSpace = false);
        // ...at a world position and rotation
        static Entity instantiate(const PrefabAsset& prefab, Scene* scene, const Vector3& position, const Quaternion& rotation, Entity parent = NULL_ENTITY);

        // A copy of `original` (of `scene`) in `targetScene` (its own when null). Without a
        // parent the copy is at the top of the hierarchy where the original is in the world;
        // with one, below it at the original's local placement, or at its world placement
        // with worldSpace.
        static Entity instantiate(Scene* scene, Entity original, Scene* targetScene = nullptr, Entity parent = NULL_ENTITY, bool worldSpace = false);
        static Entity instantiate(Scene* scene, Entity original, Scene* targetScene, const Vector3& position, const Quaternion& rotation, Entity parent = NULL_ENTITY);

        // Destroys the GameObject and its children after the update of this frame, or
        // `delay` seconds of game time later
        static void destroy(Scene* scene, Entity entity, float delay = 0.0f);
        // ...now
        static void destroyImmediate(Scene* scene, Entity entity);

        // The scene a prefab goes to when none is given (Unity's active scene): the scene
        // whose scripts are starting (their init() runs before the scene is shown), else
        // the Engine's main scene (Engine::getScene); null when there is none
        static Scene* getActiveScene();
        static void pushActiveScene(Scene* scene);
        static void popActiveScene();
        // The scenes pushed now, bottom first (scenes whose scripts are starting)
        static std::vector<Scene*> getActiveSceneStack();

        // Starts the scripts of these GameObjects: their Lua instances, then the C++ ones, then
        // (ScriptRuntime::awakeEntities) init and onEnable of the active ones; stops them:
        // onDisable and onDestroy (ScriptRuntime::destroyEntities), then the instances go
        static void initializeScripts(Scene* scene, const std::vector<Entity>& entities);
        static void cleanupScripts(Scene* scene, const std::vector<Entity>& entities);

        // Engine: once a frame, after the update
        static void update(double deltatime);
        // Engine: a scene going away takes its pending destroys with it
        static void forgetScene(Scene* scene);
        static void clearAll();
    };

}

#endif // OBJECTMANAGER_H
