// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PREFABMANAGER_H
#define PREFABMANAGER_H

#include "Export.h"
#include "Entity.h"
#include "EntityHandle.h"
#include <string>
#include <functional>
#include <vector>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace jasperengine {

    class Scene;

    // PrefabManager registers named prefabs and instantiates/destroys them at runtime.
    // A factory receives (Scene*, Entity root) and must return true on success. Failed
    // factories are not recorded; entities created during the call are rolled back.
    //
    // Each call creates its own root, so a prefab can be spawned any number of times. Entity IDs
    // are scene-local, so parent by name in the destination scene:
    //   PrefabManager::createPrefab("enemies/EnemyShip", mainScene, "spawn");
    //
    // Usage from C++ (standalone generated code):
    //   PrefabManager::registerPrefab(1, "enemies/EnemyShip", create_prefab_enemies_EnemyShip);
    //   Entity root = PrefabManager::createPrefab("enemies/EnemyShip", scene);
    //   PrefabManager::destroyPrefab(scene, root);
    //
    // Usage from Lua:
    //   local root = PrefabManager.createPrefab("enemies/EnemyShip", scene)
    //   PrefabManager.destroyPrefab(scene, root)

    class JASPERENGINE_API PrefabManager {
    private:
        struct PrefabEntry {
            uint32_t id;
            std::string name;
            std::function<bool(Scene*, Entity)> factory;
            std::function<bool(Scene*, Entity)> destroyer;
        };

        struct PrefabInstance {
            Entity rootEntity;
            Scene* scene;
            uint32_t prefabId;
            std::vector<Entity> entities; // all entities including root
        };

        static std::vector<PrefabEntry> entries;
        static std::vector<PrefabInstance> instances;

        static PrefabEntry* findEntry(uint32_t id);
        static PrefabEntry* findEntry(const std::string& name);
        static Entity instantiate(uint32_t id, Scene* scene, Entity parent);

    public:
        // The factory must return true on success. Void-returning callables are accepted
        // and treated as always-successful.
        static void registerPrefab(uint32_t id, const std::string& name, std::function<bool(Scene*, Entity)> factory, std::function<bool(Scene*, Entity)> destroyer = nullptr);

        template<typename Factory>
        static auto registerPrefab(uint32_t id, const std::string& name, Factory&& factory, std::function<bool(Scene*, Entity)> destroyer = nullptr)
            -> std::enable_if_t<std::is_invocable_v<Factory&, Scene*, Entity>, void>
        {
            using Result = std::invoke_result_t<Factory&, Scene*, Entity>;
            if constexpr (std::is_void_v<Result>) {
                registerPrefab(id, name, std::function<bool(Scene*, Entity)>(
                    [factory = std::forward<Factory>(factory)](Scene* scene, Entity root) mutable -> bool {
                        factory(scene, root);
                        return true;
                    }), std::move(destroyer));
            } else {
                registerPrefab(id, name, std::function<bool(Scene*, Entity)>(
                    [factory = std::forward<Factory>(factory)](Scene* scene, Entity root) mutable -> bool {
                        return static_cast<bool>(factory(scene, root));
                    }), std::move(destroyer));
            }
        }

        static Entity createPrefab(const std::string& name, Scene* scene);
        static Entity createPrefab(uint32_t id, Scene* scene);

        // Every overload creates the instance root; the third argument is the entity it is
        // parented to, looked up by name in `scene` for the string form.
        static Entity createPrefab(const std::string& name, Scene* scene, const std::string& parentName);
        static Entity createPrefab(uint32_t id, Scene* scene, const std::string& parentName);

        // For objects that already carry their own scene (GameObject, Button, ...)
        static Entity createPrefab(const std::string& name, const EntityHandle& parent);
        static Entity createPrefab(uint32_t id, const EntityHandle& parent);

        static Entity createPrefab(const std::string& name, Scene* scene, Entity parent);
        static Entity createPrefab(uint32_t id, Scene* scene, Entity parent);

        static bool destroyPrefab(Scene* scene, Entity rootEntity);

        static uint32_t getPrefabId(const std::string& name);
        static std::string getPrefabName(uint32_t id);
        static std::vector<std::string> getPrefabNames();
        static int getPrefabCount();

        static void destroyAllInstances(Scene* scene);
        static void clearAll();
    };

} // namespace jasperengine

#endif // PREFABMANAGER_H
