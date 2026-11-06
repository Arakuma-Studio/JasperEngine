// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "util/YamlFootprint.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include "util/PrimitiveParameters.h"
#include "yaml-cpp/yaml.h"

namespace jasperengine{
    struct MeshComponent;
}

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

        std::string describe() const override { return "Change Mesh (" + CommandNames::entity(project, sceneId, entity) + ")"; }
        const char* typeName() const override { return "MeshChangeCmd"; }
        size_t memoryBytes() const override { return sizeof(*this) + yamlFootprint(oldMesh) + yamlFootprint(newMesh); }
        // Off-thread release: the destructor only frees YAML trees and override records; no editor/engine/GPU state (S24 R8).
        bool releaseOffThread() const override { return true; }
    };

}