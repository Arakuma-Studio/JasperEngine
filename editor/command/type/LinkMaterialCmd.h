// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include "Catalog.h"
#include <string>
#include <functional>

namespace jasperengine::editor{

    class LinkMaterialCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        ComponentType componentType;
        std::string propertyName;
        unsigned int submeshIndex;
        bool wasModified;
        PrefabEditUndo prefabUndo;
        std::function<void()> onValueChanged;

        Material newMaterial;
        Material oldMaterial;
        std::string oldLinkedFilePath;
        uint64_t oldOverrideFields = 0;

    public:
        LinkMaterialCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType componentType,
                        std::string propertyName, unsigned int submeshIndex,
                        Material newMaterial, std::function<void()> onValueChanged = nullptr);

        bool execute() override;
        void undo() override;

    };

}
