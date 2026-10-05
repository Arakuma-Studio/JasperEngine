// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include "Catalog.h"
#include "Project.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor::mcp {

    // Scene / GameObject / component / asset references in tool arguments (DESIGN.md,
    // Addressing). Every resolver fills `error` with a message that tells the agent how
    // to fix the reference (listing the valid choices) and returns null / false.
    class McpRefs {
    public:
        // Scene: null / missing → the selected scene; integer (or digit string) → scene id
        // (Play copies of child scenes included); string → scene name, or a project-relative
        // .scene path. Includes open prefab stages. An object {"id"} / {"name"} / {"path"}
        // (what sceneJson returns) works too.
        static SceneProject* resolveScene(Project* project, const Json& ref, std::string& error);

        // GameObject in `scene`: integer (or digit string) → entity id; string → a path of
        // '/'-joined names from the scene root, "Name#N" being the Nth earlier sibling with
        // that name (the scene-dump scheme). A bare name matching one GameObject anywhere
        // resolves too; several matches are an error listing their paths. Objects:
        // {"id": n} (what entityJson returns: id wins), {"path": "A/B"}, {"name": "B"}
        // (any depth; ambiguity is an error listing the candidates).
        static Entity resolveEntity(SceneProject* scene, const Json& ref, std::string& error);
        // One ref or an array of refs (duplicates dropped, order kept). Empty array → error.
        static bool resolveEntities(SceneProject* scene, const Json& refs, std::vector<Entity>& out, std::string& error);
        // `entities` (array) or else `entity` (one ref) of a tool's arguments
        static bool resolveEntityArgs(SceneProject* scene, const Json& args, std::vector<Entity>& out, std::string& error,
                                      const char* manyKey = "entities", const char* oneKey = "entity");

        // The path resolveEntity accepts for this GameObject
        static std::string entityPath(SceneProject* scene, Entity entity);
        // Every GameObject's path in one pass (EntityPath::of for each is quadratic)
        static std::unordered_map<Entity, std::string> entityPaths(SceneProject* scene);
        // {id, name, path}
        static Json entityJson(SceneProject* scene, Entity entity);
        // {id, name, path} with a path from a prebuilt entityPaths() map
        static Json entityJson(SceneProject* scene, Entity entity, const std::unordered_map<Entity, std::string>& paths);
        // {id, name, path (project-relative .scene, "" if never saved), type, opened, modified, play_state}
        static Json sceneJson(Project* project, const SceneProject& scene);
        static std::string sceneTypeName(SceneType type);
        // "3D" / "2D" / "UI" (any case, also "3d_scene"...) → SceneType
        static bool parseSceneType(const std::string& name, SceneType& out);
        static std::string playStateName(ScenePlayState state);

        // Play mode: the scene the running game uses for `scene` (its Play copy for a child
        // scene that runs as a copy), else `scene` itself. Tools reading runtime state use it.
        static SceneProject* runtimeScene(Project* project, SceneProject* scene);
        static uint32_t runtimeSceneId(Project* project, uint32_t sceneId);

        // Component type by Catalog name (transform, mesh, rigidbody2d, ...; a "Component"
        // suffix and any case are accepted). Unknown → error listing the valid names.
        static bool resolveComponent(const Json& ref, ComponentType& out, std::string& error);
        // Valid component names (Catalog::getComponentName without the suffix, lower case)
        static std::vector<std::string> componentTypeNames();
        // The name tools use for a component type: lower case, no suffix ("rigidbody2d")
        static std::string componentName(ComponentType type);
        // Whether the GameObject has the component
        static bool hasComponent(SceneProject* scene, Entity entity, ComponentType type);

        // Project-relative path → absolute, sandboxed to the project (no '..' escaping it,
        // no absolute paths). mustExist adds an existence check.
        static bool resolveProjectPath(Project* project, const std::string& relative,
                                       std::filesystem::path& absolute, std::string& error, bool mustExist = false);
    };

}
