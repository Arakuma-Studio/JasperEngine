// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
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
    };

}
