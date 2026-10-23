// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

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

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;

        void drawTable(SceneProject* scene, bool playing);
        void drawTrace(SceneProject* scene);
        void drawFooter(SceneProject* scene);
    };

}
