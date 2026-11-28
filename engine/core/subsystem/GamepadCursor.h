// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GAMEPADCURSOR_H
#define GAMEPADCURSOR_H

// The gamepad cursor (docs/specs/text-entry-gamepad-cursor-back-button.md part B,
// docs/mcp/guide/html-ui.md "Gamepad cursor"): while the player uses a gamepad and a shown,
// interactive, screen-mode UIDocument has Gamepad Cursor on, the left stick moves a drawn pointer,
// A clicks and drags through the documents' mouse pointer, the right stick scrolls and the d-pad
// moves the focus, which the cursor jumps to. It never reaches Input, the game's pointer events or
// the input scheme. Off, it costs one cached boolean per frame.

#include "Export.h"
#include "math/Vector2.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;

    // Project Settings > Input > Gamepad Cursor
    struct GamepadCursorSettings {
        float speed = 1000.0f;            // canvas px/s at a 1080 px tall canvas (scaled by the canvas height)
        float accelerationTime = 0.15f;   // seconds from rest to full speed while the stick stays deflected
        float slowOverControls = 0.5f;    // speed factor over an element that takes pointer input
        float scrollSpeed = 20.0f;        // wheel lines per second at full deflection
        float deadzone = 0.2f;            // radial, of each stick
        std::string graphic;              // asset path of a texture; empty: the built-in arrow
        float size = 32.0f;               // CSS px
        Vector2 hotSpot = Vector2(0.0f, 0.0f);   // CSS px from the graphic's top-left

        bool operator==(const GamepadCursorSettings& other) const {
            return speed == other.speed && accelerationTime == other.accelerationTime && slowOverControls == other.slowOverControls &&
                   scrollSpeed == other.scrollSpeed && deadzone == other.deadzone && graphic == other.graphic && size == other.size &&
                   hotSpot == other.hotSpot;
        }
        bool operator!=(const GamepadCursorSettings& other) const { return !(*this == other); }
    };

    class JASPERENGINE_API GamepadCursor {
    public:
        // the project's values (editor Play, the generated game, the player's game.json)
        static void setDefaults(const GamepadCursorSettings& settings);
        static const GamepadCursorSettings& getDefaults();

        // Lua (GamepadCursor.*): runtime overrides, back to the project's values at Play start and stop
        static void setEnabled(bool enabled);
        static bool isEnabled();
        static void setSpeed(float speed);
        static float getSpeed();
        static void setScrollSpeed(float speed);
        static float getScrollSpeed();

        // the cursor shows and owns the sticks, A, B and the d-pad (as of the last update)
        static bool isActive();
        // canvas px; a set warps it, also while inactive (the next activation starts there)
        static Vector2 getPosition();
        static void setPosition(Vector2 position);

        // ---- Engine
        // once a frame after the input latch (unscaled seconds): activation, movement, scroll, focus
        static void update(double deltaTime);
        // A while active: pointer down / up at the cursor; true when the cursor took it
        static bool pressA(int gamepad);
        static bool releaseA(int gamepad);
        // the d-pad while active: nothing focused, the element under the cursor takes the focus first
        static void prepareNavigation();
        // after the d-pad moved the focus: the cursor jumps to it now
        static void followFocus();
        // Play start and stop (Engine::resetApplicationState): overrides, position, press
        static void resetSession();
        // the element under the cursor took a press or the cursor moved over it: not a focus change to follow
        static void noteFocusBaseline();

    private:
        // Engine's input scenes, top layer first, and the cancel of the cursor's press (Engine befriends this class)
        static void scenesTopFirst(std::vector<Scene*>& out);
        static void cancelPress();
        friend struct GamepadCursorAccess;
    };

}

#endif // GAMEPADCURSOR_H
