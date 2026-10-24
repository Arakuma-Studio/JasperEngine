// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Rewrites chosen reference fields (S04 R16 "Retarget selected" / "Clear"), named by owner
// file and field locator (the dependency index's edges). A loaded owner (an open scene, a
// prefab stage, a loaded prefab asset) is edited in memory and marked modified; a closed one
// is edited in its YAML tree at the field's node and written back, so data this editor
// does not read survives. What was there before is kept for the undo (the components and
// scene settings of a loaded owner, the bytes of a closed file).

#pragma once

#include "asset/AssetRef.h"
#include "asset/AssetType.h"
#include "Catalog.h"
#include "SceneSettings.h"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct AssetRefTarget {
        std::string owner;      // project-relative file ("project.yaml" for the project)
        std::string locator;    // the field locator (DepEdge::field)
        uint32_t ordinal = 0;   // cube face, 0 vert / 1 frag of a shader
    };

    class AssetRefEditor {
    public:
        struct ComponentSnapshot {
            Entity entity = NULL_ENTITY;
            ComponentType type = ComponentType::Transform;
            YAML::Node before;
        };

        struct Undo {
            std::string owner;
            bool closedFile = false;
            std::string bytes;                          // a closed file's previous content
            bool wasModified = false;                   // a loaded owner's modified state
            std::vector<ComponentSnapshot> components;  // a loaded owner's components as they were
            bool hasSettings = false;                   // its scene settings as they were
            ShaderRef defaultShaders[5];
            std::vector<PostProcessPass> passes;
            PostProcessingSettings postProcessing;
        };

        // newRef: the asset, by project-relative path and GUID (empty: clear the fields)
        static bool apply(Project& project, const std::vector<AssetRefTarget>& targets, const AssetRef& newRef,
                          std::vector<Undo>& undo, std::vector<std::string>& filesWritten, std::string& error);
        static void revert(Project& project, const std::vector<Undo>& undo);

        // S04 R35: a closed scene, prefab, material or project.yaml rewritten in the new form:
        // every reference that resolves gets its GUID and current path hint. written tells
        // whether the file changed (a migrated file is left as it is: idempotent).
        static bool migrateFile(Project& project, const std::string& relativePath, bool& written, std::string& error);
    };

}
