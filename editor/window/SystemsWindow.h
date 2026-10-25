// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // Window > Analysis > Systems: the scheduled systems of the running game's scene (the
    // selected scene in Edit mode) by phase and wave with their timings, the last frame's
    // wave trace, and the scene's ECS statistics. Enabled and Every N are Play-mode debug
    // switches: not scene data, not undoable, gone when Play stops.
    class SystemsWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Systems";

        explicit SystemsWindow(Project* project): project(project) {}

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

        // Where the last drawn frame put each row's Enabled checkbox (screen coordinates), and
        // whether it lay inside the table's scrolled view: MCP focus_window("systems") reports
        // them, so a script can click the real checkbox (AC17).
        struct RowRect {
            std::string phase;
            std::string system;
            float x = 0, y = 0, width = 0, height = 0;
            bool visible = false;
        };
        const std::vector<RowRect>& getRowRects() const { return rowRects; }

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        std::vector<RowRect> rowRects;

        void drawTable(SceneProject* scene, bool playing);
        void drawTrace(SceneProject* scene);
        void drawFooter(SceneProject* scene);
    };

}
