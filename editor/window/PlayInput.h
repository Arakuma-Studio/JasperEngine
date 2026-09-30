// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include <cstdint>
#include <set>

namespace jasperengine::editor {

    class Project;

    // Forwards the editor's mouse, touch and keyboard input to the running game.
    // The Game view owns it: the Scene view stays the editor's while the game plays.
    class PlayInput {
    private:
        Project* project;

        // Engine keys sent down during play but not yet released, and the scene the
        // play session belongs to. Engine Input state is static, so a missed key-up
        // would stay pressed into the next play session.
        std::set<int> pressedKeys;
        uint32_t keysSceneId = 0;

        // Same for mouse buttons, which stop being forwarded as soon as the pointer
        // leaves the view, well before the button comes up.
        std::set<int> pressedMouseButtons;
        uint32_t mouseSceneId = 0;
        ImVec2 mousePos = ImVec2(0, 0);

        void forwardKeyboard(ImGuiIO& io, int mods);
        void releaseKeys(int mods);
        void releaseMouseButtons(int mods);

    public:
        explicit PlayInput(Project* project);

        static int currentMods();

        // Forwards this frame's input to the game of sceneId. canvasMin is the screen
        // position of the game image's top-left corner and scale converts ImGui pixels
        // to engine (framebuffer) pixels. hovered: the pointer is over the game image;
        // focused: the view owns the keyboard. Returns true when the game owns the
        // mouse this frame, so the caller keeps its own mouse handling off.
        bool forward(uint32_t sceneId, ImVec2 canvasMin, ImVec2 scale, bool hovered, bool focused);

        // Releases what is still held for sceneId (the view lost the game or play stopped).
        void release(uint32_t sceneId);
        void releaseAll();
    };

}
