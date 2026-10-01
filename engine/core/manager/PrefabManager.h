// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PREFABMANAGER_H
#define PREFABMANAGER_H

#include "Export.h"
#include "Entity.h"
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace jasperengine {

    class Scene;

    // The prefabs of the build: each one registered with an id, its name (the project
    // relative path without extension, "enemies/EnemyShip"), its asset GUID and a factory.
    // The exported game registers its generated factories in main.cpp, the editor registers
    // its own when Play starts; both number the prefabs the same way (by path), so an id is
    // the same in Play and in the exported game.
    //
    // A factory receives (Scene*, Entity root) and builds the prefab on that bare root: the
    // root becomes the prefab's root GameObject (name, state, components, children). It
    // returns true on success; on failure what it created is rolled back.
    //
    // Scripts do not call the factories: they instantiate with GameObject::instantiate
    // (Lua GameObject.instantiate), which uses PrefabManager::instantiate and runs the
    // scripts of the new GameObjects.
    class JASPERENGINE_API PrefabManager {
    private:
        struct PrefabEntry {
            uint32_t id;
            std::string name;
            std::string guid;
            std::function<bool(Scene*, Entity)> factory;
        };

        static std::vector<PrefabEntry> entries;
        static std::unordered_map<uint32_t, size_t> byId;
        static std::unordered_map<std::string, size_t> byName;
        static std::unordered_map<std::string, size_t> byGuid;
        static uint32_t revision;

        static const PrefabEntry* findEntry(uint32_t id);
        static void rebuildIndex();

    public:
        // The factory must return true on success. Void-returning callables are accepted
        // and treated as always-successful.
        static void registerPrefab(uint32_t id, const std::string& name, const std::string& guid, std::function<bool(Scene*, Entity)> factory);
        static void registerPrefab(uint32_t id, const std::string& name, std::function<bool(Scene*, Entity)> factory);

        template<typename Factory>
        static auto registerPrefab(uint32_t id, const std::string& name, const std::string& guid, Factory&& factory)
            -> std::enable_if_t<std::is_invocable_v<Factory&, Scene*, Entity> &&
                                !std::is_same_v<std::decay_t<Factory>, std::function<bool(Scene*, Entity)>>, void>
        {
            using Result = std::invoke_result_t<Factory&, Scene*, Entity>;
            if constexpr (std::is_void_v<Result>) {
                registerPrefab(id, name, guid, std::function<bool(Scene*, Entity)>(
                    [factory = std::forward<Factory>(factory)](Scene* scene, Entity root) mutable -> bool {
                        factory(scene, root);
                        return true;
                    }));
            } else {
                registerPrefab(id, name, guid, std::function<bool(Scene*, Entity)>(
                    [factory = std::forward<Factory>(factory)](Scene* scene, Entity root) mutable -> bool {
                        return static_cast<bool>(factory(scene, root));
                    }));
            }
        }

        template<typename Factory>
        static auto registerPrefab(uint32_t id, const std::string& name, Factory&& factory)
            -> std::enable_if_t<std::is_invocable_v<Factory&, Scene*, Entity> &&
                                !std::is_same_v<std::decay_t<Factory>, std::function<bool(Scene*, Entity)>>, void>
        {
            registerPrefab(id, name, std::string(), std::forward<Factory>(factory));
        }

        // The id of a prefab by GUID, by name ("enemies/EnemyShip"), by path with extension
        // ("enemies/EnemyShip.prefab") or by file name ("EnemyShip") when only one prefab has
        // it; 0 when there is none
        static uint32_t findPrefab(const std::string& nameOrGuid);
        static uint32_t findPrefabByGuid(const std::string& guid);

        // Builds the prefab on a new root GameObject of `scene`, at the prefab root's own
        // placement, at the top of the hierarchy. Scripts are not started: see
        // GameObject::instantiate. NULL_ENTITY on failure (logged).
        static Entity instantiate(uint32_t id, Scene* scene);

        static bool hasPrefab(uint32_t id);
        static uint32_t getPrefabId(const std::string& name);
        static std::string getPrefabName(uint32_t id);
        static std::string getPrefabGuid(uint32_t id);
        static std::vector<std::string> getPrefabNames();
        static int getPrefabCount();

        // Changes on every register/clear, so cached lookups (PrefabAsset) know to look again
        static uint32_t getRevision();

        static void clearAll();
    };

} // namespace jasperengine

#endif // PREFABMANAGER_H
