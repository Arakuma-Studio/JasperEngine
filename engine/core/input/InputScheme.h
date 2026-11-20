// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTSCHEME_H
#define INPUTSCHEME_H

// The current input scheme (docs/specs/input-device-awareness.md, docs/mcp/guide/input.md "Input
// scheme"): Keyboard&Mouse, Gamepad or Touch, switched by what the player last really used. The
// platform entry points (Engine::system*) note the evidence before the UI routes the event; the
// frame's latch (Input::beginFrame) applies the last scheme of the window and fires
// onSchemeChanged once. It is information, never a filter: every binding stays live.

#include "Export.h"
#include "util/FunctionSubscribe.h"

#include <cstdint>
#include <optional>
#include <string>

namespace jasperengine {

    // named in Lua and MCP by the binding groups ("Keyboard&Mouse", "Gamepad", "Touch"), in HTML by
    // keyboard-mouse, gamepad, touch
    enum class InputScheme : uint8_t { KeyboardMouse = 0, Gamepad = 1, Touch = 2 };
    // the pad's maker, for prompts (labels and glyph keys)
    enum class GamepadFamily : uint8_t { Generic = 0, Xbox, PlayStation, Nintendo };
    // what the platform knows of the event it is about to send (Engine::setInputSource)
    enum class InputSourceTag : uint8_t {
        None = 0,
        Mouse,          // a pointer the platform delivers as a finger is a mouse (Android, iOS)
        Synthetic       // made by the engine (Android Back's Escape, soft-keyboard edit keys): never evidence
    };

    class JASPERENGINE_API InputSchemeTracker {
    public:
        // ---- the event about to be sent (Engine::setInputSource; reset after each system* call)
        static void setSource(InputSourceTag tag, int deviceId = -1);
        static InputSourceTag getSourceTag();
        static int getSourceDevice();
        // seconds on a monotonic clock (what Engine::system* pass as `now`)
        static double clock();

        // ---- evidence (Engine::system*; tests). Positions are canvas pixels
        // typing: a text field has the keyboard focus (typing never switches)
        static void noteKey(int key, bool repeat, bool typing, double now);
        static void noteKeyUp(int key);
        static void noteMouseButton(float x, float y, double now);
        static void noteMouseScroll();
        static void noteMouseMove(float x, float y, double now);
        static void noteTouchStart(float x, float y, double now);
        static void noteTouchEnd(float x, float y, double now);
        static void noteGamepadButton(int gamepad);
        static void noteGamepadAxis(int gamepad, int axis, float previous, float value);
        static void noteGamepadConnected(int gamepad, const std::string& name);
        static void noteGamepadDisconnected(int gamepad);

        // ---- the frame (Input::beginFrame): applies the window, fires the change once
        static void latch();
        // engine start, Play start, InputActions::resetSession: Touch on a touch device, else
        // Keyboard&Mouse (the override, when set, instead); no change event
        static void reset(bool touchDevice);

        // ---- state (as of the last latch)
        static InputScheme getCurrent();
        // KeyboardMouse or Touch: the last pointer device used (a gamepad never changes it)
        static InputScheme getPointer();
        // the pad that last switched or held the Gamepad scheme, -1 before any
        static int getLastGamepad();
        // of the last pad, else of the first connected one, else Generic
        static GamepadFamily getGamepadFamily();
        // the family of a pad's name (a browser's "Vendor: 054c" first, then the name's words)
        static GamepadFamily familyOf(const std::string& padName);
        // the family of a connected pad (Generic for an unknown id)
        static GamepadFamily familyOfPad(int gamepad);
        // forces the scheme (and the pointer for Touch and Keyboard&Mouse) from the next latch;
        // the evidence is still tracked underneath. nullopt: back to the tracked value
        static void setOverride(std::optional<InputScheme> scheme);
        static std::optional<InputScheme> getOverride();
        // scheme changes since the last reset
        static uint32_t getChangeCount();
        // bumped by every change of the scheme, the pointer or the family (UI that follows them)
        static uint32_t getRevision();

        static const char* groupName(InputScheme scheme);     // "Keyboard&Mouse", "Gamepad", "Touch"
        static const char* htmlName(InputScheme scheme);      // "keyboard-mouse", "gamepad", "touch"
        static const char* familyName(GamepadFamily family);  // "Generic", "Xbox", "PlayStation", "Nintendo"
        static const char* familyKey(GamepadFamily family);   // "generic", "xbox", "playstation", "nintendo"
        // a group name, an html name or an MCP name (keyboard_mouse), case-insensitive
        static bool parseScheme(const std::string& text, InputScheme& out);

        // (scheme, previous) as group names, once per frame that changed it
        static FunctionSubscribe<void(const std::string&, const std::string&)> onSchemeChanged;
    };

}

#endif // INPUTSCHEME_H
