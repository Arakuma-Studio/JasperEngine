// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "Project.h"
#include "AppSettings.h"
#include "window/PlayInput.h"

namespace jasperengine::editor {

    // Unity-style Game view. While a scene plays the game renders here: the engine
    // draws it into this view's framebuffer at this view's size, even while the tab
    // is hidden behind another, and the view forwards game input while it is hovered
    // or focused. When stopped it shows the selected scene through its game camera
    // (a secondary view of the Scene view's frame, see App::engineRender).
    class GameWindow {
    private:
        Project* project;
        PlayInput* playInput;

        bool windowOpen = true;
        bool focusRequested = false;
        // Drawn in the last show(): the view the engine should size the game for
        bool visible = false;
        bool maximizedShown = false;
        bool focused = false;
        bool cursorInView = false;
        bool hostedLastFrame = false;
        // Frames waited for a Scene view to dock next to on first launch
        int dockWaitFrames = 0;

        GameViewSettings settings;
        bool settingsLoaded = false;

        // Game image size in engine pixels, and the scale from ImGui to engine pixels
        int width = 0;
        int height = 0;
        ImVec2 engineScale = ImVec2(1.0f, 1.0f);

        // Scene drawn by the edit-mode pass and whether it needs drawing again
        uint32_t editPassSceneId = NULL_PROJECT_SCENE;
        bool editPassDirty = true;
        bool editPassRendered = false;
        // what the view shows: the running game, or the edit-mode pass
        Framebuffer framebuffer;

        void loadSettings();
        void saveSettings();
        void drawToolbar();
        void drawContent(uint32_t sceneId);
        void drawStats(ImVec2 imageMin, ImVec2 imageMax);
        bool computeAspect(float& aspect) const;

    public:
        static constexpr const char* WINDOW_NAME = "Game";

        GameWindow(Project* project, PlayInput* playInput);

        void show();

        void setOpen(bool open);
        bool isOpen() const;
        // Brings the Game tab to front, opening it if needed (play start)
        void requestFocus();

        // Scene the view shows: the play session's main scene, else the selected one
        uint32_t getTargetSceneId() const;
        // sceneId is the running game: it renders into this view's framebuffer at
        // getRenderWidth() x getRenderHeight(), whether the view is shown or not
        bool hostsScene(uint32_t sceneId) const;
        // sceneId is stopped and needs the edit-mode pass through its game camera
        bool wantsEditPass(uint32_t sceneId) const;
        bool isEditPassDirty() const;
        void markEditPassRendered(uint32_t sceneId);
        Framebuffer* getFramebuffer();

        // drawn in the last show()
        bool isVisible() const;
        // size of the game image as last laid out (0 before the view was ever shown)
        int getWidth() const;
        int getHeight() const;
        // size the game renders at: the laid out one, or the project canvas before any
        int getRenderWidth() const;
        int getRenderHeight() const;
        bool isGameCursorInView() const;
        // The game image has keyboard focus (keys go to the game while playing)
        bool hasKeyboardFocus() const;
        bool isMaximizedOnPlay();

        void resetProjectState();
    };

}
