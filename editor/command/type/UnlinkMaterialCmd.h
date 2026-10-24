// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include "Catalog.h"
#include <string>
#include <map>
#include <functional>

namespace jasperengine::editor{

    class UnlinkMaterialCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        ComponentType componentType;
        std::string propertyName;
        unsigned int submeshIndex;
        bool wasModified;
        PrefabEditUndo prefabUndo;
        std::function<void()> onValueChanged;

        struct EntityData {
            Material oldMaterial;
            std::string linkedFilePath;
            uint32_t oldOverrideFields = 0;
        };
        std::map<Entity, EntityData> entities;

    public:
        UnlinkMaterialCmd(Project* project, uint32_t sceneId, ComponentType componentType,
                          std::string propertyName, unsigned int submeshIndex,
                          const std::vector<Entity>& entityList,
                          std::function<void()> onValueChanged = nullptr);

        bool execute() override;
        void undo() override;

        std::string describe() const override {
            std::vector<Entity> targets;
            for (const auto& [entity, data] : entities) {
                targets.push_back(entity);
            }
            return "Unlink Material (" + CommandNames::entities(project, sceneId, targets) + ")";
        }
        const char* typeName() const override { return "UnlinkMaterialCmd"; }

    };

}
