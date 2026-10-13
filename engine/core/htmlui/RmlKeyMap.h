// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLKEYMAP_H
#define RMLKEYMAP_H

#include "Export.h"

#include <RmlUi/Core/Input.h>

#include <string>

namespace jasperengine {

    // Engine keys and modifiers (Input.h, GLFW codes) to RmlUi's, and the DOM KeyboardEvent
    // `key`/`code` strings scripts see (phase-1 spec §3.6.2, §3.9.3).
    class JASPERENGINE_INTERNAL RmlKeyMap {
    public:
        // every D_KEY_*; KI_UNKNOWN for D_KEY_UNKNOWN, WORLD_1/2, F25 and anything else
        static Rml::Input::KeyIdentifier toRmlKey(int dKey);
        // the engine key an RmlUi key comes from (keydown events carry only the RmlUi key);
        // D_KEY_UNKNOWN for KI_UNKNOWN and keys the engine has not
        static int fromRmlKey(Rml::Input::KeyIdentifier key);

        // D_MODIFIER_* to Rml::Input::KM_*. With the Apple mapping, SUPER (Cmd) also sets
        // KM_CTRL: RmlUi's text widget reads only ctrl for copy/cut/paste/select all
        static int toRmlModifiers(int dMods);
        // KM_* back to D_MODIFIER_* (KM_CTRL stays CONTROL, also under the Apple mapping)
        static int fromRmlModifiers(int rmlMods);

        // DOM `code`: the physical key ("KeyA", "Digit1", "Space", "ShiftLeft", "Numpad0",
        // "ArrowLeft", "F5"); "" for an unknown key
        static std::string domCode(int dKey);
        // DOM `key`: the character the key produces on a US layout with shift (and caps
        // lock for letters) applied ("a", "A", "!", " "), else its name ("Enter", "Escape",
        // "ArrowLeft", "Backspace", "Tab", "Shift", "F5"); "Unidentified" when unknown
        static std::string domKey(int dKey, int dMods);

        // p4-input: the keys of a gamepad (GamepadUIKey, UIDocumentSystem.h, as an int). `code`: the
        // Xbox gamepad key names browsers on Xbox and Windows (UWP) give key events ("GamepadA",
        // "GamepadDPadUp", "GamepadLeftThumbstickUp", "GamepadLeftShoulder", "GamepadView",
        // "GamepadMenu", ...; "GamepadGuide" for the guide button); "" for none. `key`: what the key
        // means: the d-pad and the left stick "ArrowUp" ..., A "Enter", B "Escape", the others their code.
        // The engine key: D_KEY_UP ..., D_KEY_ENTER, D_KEY_ESCAPE, else D_KEY_UNKNOWN
        static std::string gamepadDomCode(int gamepadKey);
        static std::string gamepadDomKey(int gamepadKey);
        static int gamepadEngineKey(int gamepadKey);

        // the Cmd → Ctrl mapping of toRmlModifiers: on by default on __APPLE__ only; tests
        // switch it to check both platforms
        static void setAppleModifiers(bool enabled);
        static bool isAppleModifiers();
    };

}

#endif //RMLKEYMAP_H
