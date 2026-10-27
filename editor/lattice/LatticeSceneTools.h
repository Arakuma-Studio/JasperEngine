// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ecs/Entity.h"

#include <cstdint>

namespace jasperengine::editor{

    class Project;

    // The Scene view's lattice tools that are not drawing or dragging (spec §9.2): the
    // right-click menu, the point selection shortcuts and the falloff toggle. Everything
    // they change goes through LatticeEditorOps.
    class LatticeSceneTools{
    public:
        // the lattice the menu is for: the edited one (the only selected entity), else the
        // only lattice among the selected entities; NULL_ENTITY when none
        static Entity getMenuLattice(Project* project, uint32_t sceneId);

        // the menu's items, between BeginPopup and EndPopup
        static void drawContextMenu(Project* project, uint32_t sceneId, Entity lattice);

        // Esc while the Scene view has the keyboard clears the points (true when there were
        // points to clear). The other point keys are command catalog entries (S25: lattice.*,
        // shortcuts/DefaultCommands.cpp) that run the actions below.
        static bool handleShortcuts(Project* project, uint32_t sceneId);

        enum class PointAction { SelectNone, Invert, Expand, Shrink, ToggleFalloff };
        // a lattice's points are edited in the scene (the catalog's keyGate)
        static bool isEditingPoints(Project* project, uint32_t sceneId);
        // runs it on the edited lattice; false when none is edited
        static bool runPointAction(Project* project, uint32_t sceneId, PointAction action);

        // Ctrl+A (Edit > Select All) in the Scene view: every point of the edited lattice;
        // false when no lattice is edited
        static bool selectAllPoints(Project* project, uint32_t sceneId);
    };

}
