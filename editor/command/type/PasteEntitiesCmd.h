// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/PlayChangeReview.h"
#include "util/YamlFootprint.h"
#include "Project.h"
#include "util/EntityClipboard.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace jasperengine::editor{

    // Pastes copied GameObjects into a scene as one undo step (see EntityClipboard)
    class PasteEntitiesCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        std::shared_ptr<const GameObjectsClip> clip;
        EntityClipboard::Placement placement;
        Entity anchor;

        std::vector<Entity> createdEntities;    // Everything the paste made, in creation order
        std::vector<Entity> createdRoots;       // The pasted GameObjects, in Hierarchy order
        std::vector<Entity> lastSelected;
        bool wasModified = false;

        // Copied entity -> the id its first paste got. A redo decodes the copy with these ids,
        // so it reclaims them and the commands after this one still find their entities.
        std::unordered_map<Entity, Entity> firstIds;

        void destroyCreated(SceneProject* sceneProject);
        bool hasCamera(Scene* scene) const;

    public:
        PasteEntitiesCmd(Project* project, uint32_t sceneId, std::shared_ptr<const GameObjectsClip> clip,
                         EntityClipboard::Placement placement, Entity anchor);
        ~PasteEntitiesCmd() override;

        bool execute() override;
        void undo() override;


        const std::vector<Entity>& getCreatedRoots() const { return createdRoots; }

        // "Paste Player", "Paste 3 Objects"
        std::string describe() const override { return "Paste " + CommandNames::entities(project, sceneId, createdRoots); }
        const char* typeName() const override { return "PasteEntitiesCmd"; }
        void collectKeepCommands(KeepContext& context) const override {
            for (Entity root : createdRoots) {
                context.addCreated(root);
            }
        }
        size_t memoryBytes() const override {
            return sizeof(*this) + (clip ? yamlFootprint(clip->roots) : 0) +
                (createdEntities.capacity() + createdRoots.capacity() + lastSelected.capacity()) * sizeof(Entity) +
                firstIds.size() * 2 * sizeof(Entity);
        }
        uint32_t getSceneId() const { return sceneId; }
    };

}
