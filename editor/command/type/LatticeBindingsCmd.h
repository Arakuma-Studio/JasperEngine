// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/PrefabEditUndo.h"
#include "Project.h"
#include "component/LatticeModifierComponent.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor{

    // Sets the bindings and Include Children of an entity's LatticeModifierComponent in one
    // undo step, adding the component when the entity has none (LatticeEditorOps::bind and
    // unbind). Prefab instance overrides: the added component, or what changed of it.
    class LatticeBindingsCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;

        std::vector<LatticeBinding> oldBindings;
        bool oldIncludeChildren = false;
        std::vector<LatticeBinding> newBindings;
        bool newIncludeChildren = false;

        bool addedComponent = false;
        bool wasModified;
        PrefabEditUndo prefabUndo;

    public:
        LatticeBindingsCmd(Project* project, uint32_t sceneId, Entity target, const std::vector<LatticeBinding>& bindings, bool includeChildren);

        bool execute() override;
        void undo() override;

        // a component added shows in the Hierarchy's icons
        bool affectsStructure() const override { return addedComponent; }
    };

}
