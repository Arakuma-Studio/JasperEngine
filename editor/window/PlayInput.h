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
        // Simulate Touch (ui-multitouch): the finger the left button drives (0, or 1 while finger
        // 0 is pinned), and finger 0 pinned by an Alt release: it stays down until Alt goes up
        int touchFinger = 0;
        bool fingerPinned = false;
        ImVec2 pinnedPos = ImVec2(0, 0);
        uint32_t mouseSceneId = 0;
        ImVec2 mousePos = ImVec2(0, 0);

        // An MCP agent drives the pointer (send_input): the real mouse is not forwarded
        // until it moves, scrolls or clicks, so it can't move the game's pointer back
        bool pointerInjected = false;

        // the real pointer is over the game image of mouseInsideSceneId (systemMouseEnter/Leave)
        bool mouseInside = false;
        uint32_t mouseInsideSceneId = 0;
        // ImGui frame whose characters were forwarded (the trail is read once per frame)
        int charactersFrame = -1;
        // a game holding the cursor (captured, confined) gave it back to the editor: Escape, or
        // Release Game Cursor; a click on the game image takes it again
        bool cursorReleased = false;
        // the last ImGui frame the Game view had the focus
        int gameViewFocusFrame = -1;

        void updateMouseInside(uint32_t sceneId, bool inside);
        void forwardCharacters(ImGuiIO& io);

        void forwardKeyboard(ImGuiIO& io, int mods);
        void releaseKeys(int mods);
        void releaseMouseButtons(int mods);
        static int currentMods();
        // Escape went down this frame, whoever owns the key
        static bool escapeWentDown();

    public:
        explicit PlayInput(Project* project);

        // Forwards this frame's input to the game of sceneId. canvasMin is the screen
        // position of the game image's top-left corner and scale converts ImGui pixels
        // to engine (framebuffer) pixels. hovered: the pointer is over the game image;
        // focused: the view owns the keyboard. Returns true when the game owns the
        // mouse this frame, so the caller keeps its own mouse handling off.
        bool forward(uint32_t sceneId, ImVec2 canvasMin, ImVec2 scale, bool hovered, bool focused);

        // Releases what is still held for sceneId (the view lost the game or play stopped).
        void release(uint32_t sceneId);
        void releaseAll();

        void setPointerInjected(bool injected) { pointerInjected = injected; }

        // Gives a cursor the game captured or confined back to the editor (Escape in the Game
        // view does it too, as in Unity); clicking the game image gives it back to the game
        void releaseCursor();
        bool isCursorReleased() const { return cursorReleased; }
        // The editor keeps the cursor: released, or the Game view does not have the focus
        bool wantsEditorCursor() const;

        // The engine key (D_KEY_*) of an ImGui key, D_KEY_UNKNOWN (-1) when the game has none:
        // what forward() sends (Project Settings > Input's Listen captures with it)
        static int engineKey(ImGuiKey key);
        // The engine gamepad button (D_GAMEPAD_BUTTON_*) of an ImGui gamepad key, else -1
        static int engineGamepadButton(ImGuiKey key);
    };

}
