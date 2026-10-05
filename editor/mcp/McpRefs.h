// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include "Catalog.h"
#include "Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    // Scene / GameObject / component / asset references in tool arguments (DESIGN.md,
    // Addressing). Every resolver fills `error` with a message that tells the agent how
    // to fix the reference (listing the valid choices) and returns null / false.
    class McpRefs {
    public:
        // Scene: null / missing → the selected scene; integer (or digit string) → scene id;
        // string → scene name, or a project-relative .scene path. Includes open prefab stages.
        static SceneProject* resolveScene(Project* project, const Json& ref, std::string& error);

        // GameObject in `scene`: integer (or digit string) → entity id; string → a path of
        // '/'-joined names from the scene root, "Name#N" being the Nth earlier sibling with
        // that name (the scene-dump scheme). A bare name matching one GameObject anywhere
        // resolves too; several matches are an error listing their paths.
        static Entity resolveEntity(SceneProject* scene, const Json& ref, std::string& error);

        // The path resolveEntity accepts for this GameObject
        static std::string entityPath(SceneProject* scene, Entity entity);
        // {id, name, path}
        static Json entityJson(SceneProject* scene, Entity entity);
        // {id, name, path (project-relative .scene, "" if never saved), type, opened, modified, play_state}
        static Json sceneJson(Project* project, const SceneProject& scene);
        static std::string sceneTypeName(SceneType type);
        static std::string playStateName(ScenePlayState state);

        // Component type by Catalog name (transform, mesh, rigidbody2d, ...; a "Component"
        // suffix and any case are accepted). Unknown → error listing the valid names.
        static bool resolveComponent(const Json& ref, ComponentType& out, std::string& error);
        // Valid component names (Catalog::getComponentName without the suffix, lower case)
        static std::vector<std::string> componentTypeNames();

        // Project-relative path → absolute, sandboxed to the project (no '..' escaping it,
        // no absolute paths). mustExist adds an existence check.
        static bool resolveProjectPath(Project* project, const std::string& relative,
                                       std::filesystem::path& absolute, std::string& error, bool mustExist = false);
    };

}
