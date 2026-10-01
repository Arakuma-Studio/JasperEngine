// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include "util/PrimitiveParameters.h"
#include "yaml-cpp/yaml.h"

namespace jasperengine::editor{

    class MeshChangeCmd: public Command{

    private:
        YAML::Node oldMesh;
        YAML::Node newMesh;

        Project* project;
        uint32_t sceneId;
        Entity entity;

        bool wasModified;
        PrefabEditUndo prefabUndo;

    public:
        MeshChangeCmd(Project* project, uint32_t sceneId, Entity entity, MeshComponent mesh);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}