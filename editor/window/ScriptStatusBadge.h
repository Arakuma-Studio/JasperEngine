// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The main toolbar's script badge (S23 R40): one icon for the scripts' state (green ok, a
// spinner while building or reloading, amber stale or a warning, red failed or refused), a
// tooltip with one line per language, and a popover with the details, the first errors as
// links to their source and Rebuild C++ / Cancel Build / Show in Console. The Console opens
// when a state newly fails.

namespace jasperengine::editor {

    class Project;

    class ScriptStatusBadge {
    public:
        explicit ScriptStatusBadge(Project* project);
        // Draws the badge at the cursor (the toolbar row's height)
        void draw(float rowHeight);
        // The badge's width at that height (the toolbar lays out around it)
        float width(float rowHeight) const;

    private:
        void drawPopover();
        Project* project;
    };

}
