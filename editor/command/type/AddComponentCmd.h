// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/PlayChangeReview.h"
#include "Project.h"
#include "Catalog.h"
#include "command/PrefabEditUndo.h"

namespace jasperengine::editor {
    class AddComponentCmd : public Command {
    private:
        Project* project;
        size_t sceneId;
        std::vector<Entity> entities;
        ComponentType componentType;

        bool wasModified;
        // Unity: a component added to a prefab instance's GameObject is an added component
        // override of that instance
        PrefabEditUndo prefabUndo;

    public:
        AddComponentCmd(Project* project, size_t sceneId, Entity entity, ComponentType componentType);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // "Add Component Rigidbody (Player)"
        std::string describe() const override {
            return "Add Component " + CommandNames::component(componentType) + " (" + CommandNames::entities(project, (uint32_t)sceneId, entities) + ")";
        }
        const char* typeName() const override { return "AddComponentCmd"; }
        // Added again with the values it has now
        void collectKeepCommands(KeepContext& context) const override {
            for (Entity entity : entities) {
                context.addComponentValues(entity, componentType, true);
            }
        }

        uint32_t getSceneId() const { return (uint32_t)sceneId; }
        ComponentType getComponentType() const { return componentType; }
        const std::vector<Entity>& getEntities() const { return entities; }
    };
}