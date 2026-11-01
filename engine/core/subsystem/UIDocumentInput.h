// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENTINPUT_H
#define UIDOCUMENTINPUT_H

// The input types the engine's routing hands the HTML UI documents (Engine::system*,
// UIDocumentSystem), in every build: a build without the HTML UI routes its input the same way
// (UIDocumentSystemNone.h)

namespace jasperengine{

    // phase of a pointer event handed to texture-mode documents (dispatchWorldPointer)
    enum class PointerPhase { DOWN, MOVE, UP, CANCEL };

    // What a gamepad gives documents (phase-4 p4-input; Engine::systemGamepad*): its buttons, the four
    // directions of the left stick and the two triggers, each a key of keydown/keyup events (DOM `code`
    // "GamepadDPadUp", "GamepadA", ...; RmlKeyMap::gamepadDomCode)
    enum class GamepadUIKey {
        DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
        LEFT_STICK_UP, LEFT_STICK_DOWN, LEFT_STICK_LEFT, LEFT_STICK_RIGHT,
        A, B, X, Y, LEFT_SHOULDER, RIGHT_SHOULDER, LEFT_TRIGGER, RIGHT_TRIGGER,
        VIEW, MENU, GUIDE, LEFT_THUMB, RIGHT_THUMB,
        COUNT   // none
    };

}

#endif //UIDOCUMENTINPUT_H
