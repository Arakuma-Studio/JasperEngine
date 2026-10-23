// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/EntityEdit.h"
#include "Project.h"
#include "Catalog.h"
#include "command/PrefabEditUndo.h"
#include "util/EntityClipboard.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace jasperengine::editor {

    // Overwrites one component of several GameObjects in one undo step: with a copied
    // component (Paste Component Values) or with its default values (Reset). The entity's
    // own structure stays: a Transform's parent, the child entities a UI control made, the
    // tracks an animation owns and the geometry a sprite, tilemap or model builds.
    class ComponentValuesCmd : public Command, public EntityEdit {
    private:
        struct Target {
            Entity entity = NULL_ENTITY;
            YAML::Node before;          // The component as it was, for undo
        };

        Project* project;
        uint32_t sceneId;
        ComponentType type;
        std::shared_ptr<const ComponentClip> clip; // Null: reset to defaults
        std::vector<Target> targets;
        bool wasModified = false;
        bool differs = false;       // the last execute changed a component's values
        // Unity: the properties a reset or paste changes on a prefab instance's GameObject
        // are overrides of it
        PrefabEditUndo prefabUndo;

    public:
        ComponentValuesCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType type,
                           std::shared_ptr<const ComponentClip> clip);

        bool execute() override;
        void undo() override;

        // "Reset Light (Sun)", "Paste Component Values Light (2 Objects)"; a GameObject's own
        // component values set again (an MCP edit) is "Set Light (Sun)"
        std::string describe() const override;
        const char* typeName() const override { return "ComponentValuesCmd"; }
        bool isNoOp() const override { return !differs; }
        size_t memoryBytes() const override;

        Project* editedProject() const override { return project; }
        uint32_t editedSceneId() const override { return sceneId; }
        std::vector<Entity> editedEntities() const override;
        std::string editedLabel() const override;

        ComponentType getComponentType() const { return type; }

        // --- Inspector component menu ---
        // Each returns null, with a reason for the tooltip, when it cannot apply to all entities.
        static Command* buildPasteValues(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                         ComponentType type, std::shared_ptr<const ComponentClip> clip, std::string* reason = nullptr);
        static Command* buildPasteAsNew(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                        std::shared_ptr<const ComponentClip> clip, std::string* reason = nullptr);
        static Command* buildReset(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                   ComponentType type, std::string* reason = nullptr);
        // The same rules without building anything, for menu items drawn every frame
        static bool canPasteValues(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                   ComponentType type, const ComponentClip* clip, std::string* reason = nullptr);
        static bool canPasteAsNew(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                  const ComponentClip* clip, std::string* reason = nullptr);
        static bool canReset(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                             ComponentType type, std::string* reason = nullptr);
    };

}
