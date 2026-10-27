// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/PrefabEditUndo.h"
#include "command/type/LatticeBindingsCmd.h"
#include "Project.h"
#include "component/LatticeModifierComponent.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // The scene side of a lattice bake (LatticeEditorOps::bake, spec §9.4), one undo step: the
    // baked bindings leave their Lattice Modifiers (LatticeBindingsCmd each) and every baked
    // Model switches to the file the bake wrote. The switch reloads the geometry in place, as a
    // scene reopening does (the node, mesh part, bone and animation entities are kept, with
    // their components and the submesh edits), because the baked file has the same nodes as the
    // original. Undo loads the original file back and restores the bindings; the baked file stays
    // on disk. On a Prefab instance the file is a `filename` override of the Model.
    class LatticeBakeCmd: public Command{
    public:
        struct ModelChange{
            Entity entity = NULL_ENTITY;
            std::string filename; // as ModelComponent::filename stores it
        };
        struct BindingsChange{
            Entity entity = NULL_ENTITY;
            std::vector<LatticeBinding> bindings;
            bool includeChildren = false;
        };

    private:
        struct ModelState{
            Entity entity = NULL_ENTITY;
            std::string newFilename;
            std::string oldFilename;
        };

        Project* project;
        uint32_t sceneId;
        std::vector<ModelState> models;
        std::vector<std::unique_ptr<LatticeBindingsCmd>> bindingCmds;
        bool wasModified;
        PrefabEditUndo prefabUndo;

        // loads `filename` into the Model of `entity` keeping its generated entities; false
        // when the file cannot be loaded (the Model is then left without geometry)
        bool loadModel(Entity entity, const std::string& filename);

    public:
        LatticeBakeCmd(Project* project, uint32_t sceneId, const std::vector<ModelChange>& models,
                       const std::vector<BindingsChange>& bindings);

        bool execute() override;
        void undo() override;

        // "Bake Lattice (Rock)", "Bake Lattice (3 Objects)": the Models baked
        std::string describe() const override;
        const char* typeName() const override { return "LatticeBakeCmd"; }
        size_t memoryBytes() const override;
    };

}
