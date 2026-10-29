// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > AI > Navigation (spec S19 R31): the project's agent types and areas, every surface
// of the open scenes (state, tiles, polys, memory; Bake, Clear, Select; Bake All) and every
// NavGrid (Rebuild). Settings edits are undo steps (NavigationSettingsCmd).

#include "navigation/NavigationProjectSettings.h"

namespace jasperengine::editor {

    class Project;

    class NavigationWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Navigation";

        explicit NavigationWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

    private:
        Project* project;
        bool windowOpen = false;
        // the settings being edited (taken from the project while nothing is being dragged or typed)
        NavigationProjectSettings draft;
        bool dirty = false;

        void drawAgents();
        void drawAreas();
        void drawSurfaces();
        void drawGrids();
        // an edit finished: one undo step
        void commit(const char* name);
    };

}
