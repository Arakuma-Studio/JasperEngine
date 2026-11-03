// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The cooked scenes (.jscn) and prefabs (.jpfb) of a prebuilt build (docs/specs/S30-shipping.md
// R3, D2): the same GameObjects, components and settings the generated sources of a Compile build
// describe (Factory::createScene, createPrefab), written through the runtime field tables
// (engine/core/serialization/RuntimeFields.h). Prefab instances in a scene are cooked as their
// GameObjects are in the editor (overrides applied, removed members gone): the player builds no
// instance at load, it reads the result.

#include "manager/SceneManager.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    class EntityRegistry;
    namespace cook {
        struct WriteContext;
    }
}

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
    struct BakedPrefab;
}

namespace jasperengine::editor::cook {

    struct CookedFileBytes {
        std::string relativePath;      // "scenes/3.jscn", "prefabs/2.jpfb", "assets/je-default-sky/..."
        std::vector<uint8_t> bytes;
    };

    struct CookedPrefabInfo {
        uint32_t id = 0;               // PrefabManager's id (position in collectAllPrefabs + 1)
        std::string name;              // its path without the extension (what Lua names it by)
        std::string guid;
        std::string file;              // "prefabs/<id>.jpfb"
    };

    class SceneCooker {
    public:
        // One scene as Factory::createScene sees it: its GameObjects in the scene's order, the
        // camera (a system entity when it is none of them), the scene settings and the files
        // preloadScene() loads
        static std::vector<uint8_t> cookScene(Scene* scene, uint32_t sceneId, const std::vector<Entity>& entities, Entity camera,
                                              const SceneResources& resources);
        // A baked prefab as Factory::createPrefab builds it (root, slots, components, hierarchy)
        static std::vector<uint8_t> cookPrefab(const std::filesystem::path& prefabPath, const BakedPrefab& baked,
                                               const std::string& guid, uint32_t id);

        // Every scene of the project that is loaded (the export loads them all first) and every
        // prefab of the build; on the main thread, while the scenes are loaded
        static bool cookProject(Project* project, std::vector<CookedFileBytes>& files, std::vector<CookedPrefabInfo>& prefabs,
                                std::string& error);

        // The default sky's six images (a new 3D scene's Skybox, held in memory by the editor) as
        // the files a prebuilt build ships under assets/ ("assets/je-default-sky/..."): a cooked
        // default-sky Skybox names them, as a player reads only files
        static std::vector<CookedFileBytes> defaultSkyFiles();

        // one component's write context (the flags the scene codec and the exported code look at)
        static void describe(const EntityRegistry* registry, Entity entity, ::jasperengine::cook::WriteContext& ctx);
    };

}
