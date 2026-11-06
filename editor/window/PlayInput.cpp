// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PlayInput.h"

#include "Project.h"
#include "Engine.h"
#include "Input.h"
#include "window/StatusBar.h"

#include "imgui_internal.h"

#include <algorithm>

using namespace jasperengine;

editor::PlayInput::PlayInput(Project* project): project(project) {
}

int editor::PlayInput::currentMods() {
    ImGuiIO& io = ImGui::GetIO();
    int mods = 0;
    if (io.KeyShift) mods |= D_MODIFIER_SHIFT;
    if (io.KeyCtrl) mods |= D_MODIFIER_CONTROL;
    if (io.KeyAlt) mods |= D_MODIFIER_ALT;
    if (io.KeySuper) mods |= D_MODIFIER_SUPER;
    return mods;
}

bool editor::PlayInput::escapeWentDown() {
    // the key's own state: ImGui's keyboard navigation reads Escape first and may own it this frame,
    // which hides the press from ImGui::IsKeyPressed
    const ImGuiKeyData* escape = ImGui::GetKeyData(ImGuiKey_Escape);
    return escape && escape->Down && escape->DownDuration == 0.0f;
}

bool editor::PlayInput::forward(uint32_t sceneId, ImVec2 canvasMin, ImVec2 scale, bool hovered, bool focused) {
    ImGuiIO& io = ImGui::GetIO();
    const int mods = currentMods();
    const ImVec2 mouseDelta = io.MouseDelta;

    // ImGui's keyboard navigation takes Escape as "leave the child window" before the Game view
    // draws: the frame of that press the view has just lost the focus it had the frame before
    const bool hadFocus = focused || gameViewFocusFrame == ImGui::GetFrameCount() - 1;
    gameViewFocusFrame = focused ? ImGui::GetFrameCount() : gameViewFocusFrame;
    // A game holding the cursor gives it back on Escape (Unity), the key still reaching the
    // game; a click on the game image takes it again
    const MouseMode mouseMode = Engine::getMouseMode();
    const bool holdsCursor = mouseMode == MouseMode::CAPTURED || mouseMode == MouseMode::CONFINED;
    if (!holdsCursor) {
        cursorReleased = false;
    } else if (hadFocus && !cursorReleased && escapeWentDown()) {
        releaseCursor();
    } else if (cursorReleased && hovered && focused && ImGui::IsAnyMouseDown()) {
        cursorReleased = false;
    }

    // A captured cursor reports a virtual position that leaves the view,
    // so the mouse follows window focus like the keyboard below.
    const bool capturedMouse = mouseMode == MouseMode::CAPTURED;
    bool forwardMouse = capturedMouse ? focused && !cursorReleased : hovered;
    // the game's pointer is locked while the Game view has the focus (Input::getPointerLockState)
    if (capturedMouse) {
        Engine::systemPointerLockChanged(focused && !cursorReleased);
    }

    // The pointer belongs to an MCP agent until the real mouse does something here
    if (pointerInjected) {
        const bool realActivity = mouseDelta.x != 0 || mouseDelta.y != 0 || io.MouseWheel != 0 ||
            ImGui::IsAnyMouseDown();
        if (forwardMouse && realActivity) {
            pointerInjected = false;
        } else {
            forwardMouse = false;
        }
    }

    // A drag that leaves the view keeps being forwarded until its button comes up
    if (mouseSceneId == sceneId) {
        for (int button : pressedMouseButtons) {
            if (ImGui::IsMouseDown(button)) {
                forwardMouse = true;
                break;
            }
        }
    }

    // enter/leave follow the real pointer, not an agent's
    if (!pointerInjected) {
        updateMouseInside(sceneId, hovered);
    }

    // the game's cursor (component UI, documents, Engine.mouseCursor) over the game image
    if (hovered && Engine::getMouseMode() == MouseMode::NORMAL) {
        switch (Engine::getMouseCursor()) {
            case CursorType::IBEAM: ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput); break;
            case CursorType::POINTING_HAND: ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); break;
            case CursorType::RESIZE_EW: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
            case CursorType::RESIZE_NS: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
            case CursorType::RESIZE_NWSE: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
            case CursorType::RESIZE_NESW: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
            case CursorType::RESIZE_ALL:
            case CursorType::GRABBING: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
            case CursorType::GRAB: ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); break;
            case CursorType::NOT_ALLOWED: ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed); break;
            case CursorType::WAIT: ImGui::SetMouseCursor(ImGuiMouseCursor_Wait); break;
            case CursorType::PROGRESS: ImGui::SetMouseCursor(ImGuiMouseCursor_Progress); break;
            // ImGui has neither a crosshair nor a help cursor
            default: break;
        }
    }

    if (forwardMouse) {
        float x = (io.MousePos.x - canvasMin.x) * scale.x;
        float y = (io.MousePos.y - canvasMin.y) * scale.y;

        // Engine::transformCoordPos maps the view rect onto the canvas and drops what
        // falls outside, so a button released out there is still down and needs the
        // synthetic up below. LETTERBOX insets the rect, so test it and not the widget.
        const Rect view = Engine::getViewRect();
        const bool accepted = capturedMouse || Engine::isAllowEventsOutCanvas() ||
            (x >= view.getX() && x <= view.getX() + view.getWidth() &&
             y >= view.getY() && y <= view.getY() + view.getHeight());

        if (accepted) {
            mouseSceneId = sceneId;
            mousePos = ImVec2(x, y);
        }

        if (project->isSimulateTouch()) {
            // the left button is one finger, which never hovers. Released with Alt held, finger 0
            // stays down where it is (pinned) and the next press is finger 1: one mouse holds a
            // stick and looks around. Releasing Alt lifts the pinned finger.
            if (fingerPinned && !io.KeyAlt) {
                Engine::systemTouchEnd(0, pinnedPos.x, pinnedPos.y);
                fingerPinned = false;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                touchFinger = fingerPinned ? 1 : 0;
                Engine::systemTouchStart(touchFinger, x, y);
                if (accepted) pressedMouseButtons.insert(ImGuiMouseButton_Left);
            } else if (pressedMouseButtons.count(ImGuiMouseButton_Left) && (mouseDelta.x != 0 || mouseDelta.y != 0)) {
                Engine::systemTouchMove(touchFinger, x, y);
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && pressedMouseButtons.count(ImGuiMouseButton_Left)) {
                if (touchFinger == 0 && io.KeyAlt && !fingerPinned) {
                    fingerPinned = true;
                    pinnedPos = ImVec2(x, y);
                } else {
                    Engine::systemTouchEnd(touchFinger, x, y);
                }
                if (accepted) pressedMouseButtons.erase(ImGuiMouseButton_Left);
                touchFinger = 0;
            }
        } else {
            Engine::systemMouseMove(x, y, mods);

            // ImGui and the engine share GLFW's signs: y > 0 up, x > 0 left
            if (io.MouseWheel != 0 || io.MouseWheelH != 0) {
                Engine::systemMouseScroll(io.MouseWheelH, io.MouseWheel, mods);
            }

            for (int i = 0; i < 5; i++) {
                if (ImGui::IsMouseClicked(i)) {
                    Engine::systemMouseDown(i, x, y, mods);
                    if (accepted) pressedMouseButtons.insert(i);
                }
                if (ImGui::IsMouseReleased(i)) {
                    Engine::systemMouseUp(i, x, y, mods);
                    if (accepted) pressedMouseButtons.erase(i);
                }
            }
        }
    } else if (mouseSceneId == sceneId) {
        releaseMouseButtons(mods);
    }

    // Keyboard follows window focus, not mouse hover, so held keys keep
    // working while the mouse drifts out of the window. On focus loss the
    // key-up events would never arrive, so release held keys explicitly.
    if (focused) {
        keysSceneId = sceneId;
        forwardKeyboard(io, mods);
    } else if (keysSceneId == sceneId) {
        releaseKeys(mods);
    }

    return forwardMouse || hovered;
}

void editor::PlayInput::updateMouseInside(uint32_t sceneId, bool inside) {
    if (inside && (!mouseInside || mouseInsideSceneId != sceneId)) {
        mouseInside = true;
        mouseInsideSceneId = sceneId;
        Engine::systemMouseEnter();
    } else if (!inside && mouseInside && mouseInsideSceneId == sceneId) {
        mouseInside = false;
        Engine::systemMouseLeave();
    }
}

void editor::PlayInput::release(uint32_t sceneId) {
    if (mouseInside && mouseInsideSceneId == sceneId) {
        mouseInside = false;
        Engine::systemMouseLeave();
    }
    const int mods = currentMods();
    if (keysSceneId == sceneId) releaseKeys(mods);
    if (mouseSceneId == sceneId) releaseMouseButtons(mods);
}

void editor::PlayInput::releaseCursor() {
    const MouseMode mouseMode = Engine::getMouseMode();
    if (cursorReleased || (mouseMode != MouseMode::CAPTURED && mouseMode != MouseMode::CONFINED)) {
        return;
    }
    cursorReleased = true;
    if (mouseMode == MouseMode::CAPTURED) {
        Engine::systemPointerLockChanged(false);
    }
    StatusBar::setTransientMessage("Cursor released: click the Game view to give it back to the game", 4.0);
}

bool editor::PlayInput::wantsEditorCursor() const {
    // the Game view drew this frame or the last with the focus, and the game keeps the cursor
    return cursorReleased || gameViewFocusFrame < ImGui::GetFrameCount() - 1;
}

void editor::PlayInput::releaseAll() {
    cursorReleased = false;
    releaseKeys(0);
    releaseMouseButtons(0);
    // and whatever else the game holds (keys pressed by an agent, gamepad buttons, touches):
    // their ups are seen by the next frame
    Input::releaseAll();
}

void editor::PlayInput::forwardCharacters(ImGuiIO& io){
    // ImGui's queue holds 16-bit characters (IMGUI_USE_WCHAR32 is off), where a character
    // beyond the BMP is already U+FFFD; the trail of this frame's input events keeps the
    // whole code point. Where wchar_t is 16 bits it goes as a surrogate pair, which
    // Engine::systemCharInput combines.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (charactersFrame != g.FrameCount){
        charactersFrame = g.FrameCount;
        for (const ImGuiInputEvent& event : g.InputEventsTrail){
            if (event.Type != ImGuiInputEventType_Text)
                continue;
            const unsigned int codepoint = event.Text.Char;
            // Other control characters (Ctrl+letter, Delete's 0x7f) would show up as boxes
            if ((codepoint < 32 || codepoint == 0x7f) && codepoint != '\t' && codepoint != '\b' &&
                codepoint != '\r' && codepoint != '\n' && codepoint != 0x1b)
                continue;
            if (sizeof(wchar_t) == 2 && codepoint > 0xFFFF){
                const unsigned int value = codepoint - 0x10000;
                Engine::systemCharInput(static_cast<wchar_t>(0xD800 + (value >> 10)));
                Engine::systemCharInput(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
            }else{
                Engine::systemCharInput(static_cast<wchar_t>(codepoint));
            }
        }
    }
    io.InputQueueCharacters.resize(0);
}

void editor::PlayInput::forwardKeyboard(ImGuiIO& io, int mods){
    forwardCharacters(io);

    auto forwardKey = [&](ImGuiKey imguiKey, int engineKey, wchar_t charInput = 0, bool allowRepeat = true){
#ifndef __APPLE__
        // The backends queue these keys' control characters with the text (forwardCharacters),
        // repeats included; only imgui_impl_osx leaves them out
        charInput = 0;
#endif
        if (ImGui::IsKeyPressed(imguiKey, false)){
            if (charInput != 0){
                Engine::systemCharInput(charInput);
            }
            Engine::systemKeyDown(engineKey, false, mods);
            pressedKeys.insert(engineKey);
        }else if (allowRepeat && ImGui::IsKeyPressed(imguiKey, true)){
            if (charInput != 0){
                Engine::systemCharInput(charInput);
            }
            Engine::systemKeyDown(engineKey, true, mods);
            pressedKeys.insert(engineKey);
        }

        if (ImGui::IsKeyReleased(imguiKey)){
            Engine::systemKeyUp(engineKey, false, mods);
            pressedKeys.erase(engineKey);
        }
    };

    auto forwardKeyRange = [&](ImGuiKey imguiFirst, int engineFirst, int count){
        for (int i = 0; i < count; i++){
            forwardKey(static_cast<ImGuiKey>(static_cast<int>(imguiFirst) + i), engineFirst + i);
        }
    };

    forwardKey(ImGuiKey_Tab, D_KEY_TAB, L'\t', false);
    forwardKey(ImGuiKey_Backspace, D_KEY_BACKSPACE, L'\b');
    forwardKey(ImGuiKey_Enter, D_KEY_ENTER, L'\r', false);
    forwardKey(ImGuiKey_KeypadEnter, D_KEY_KP_ENTER, L'\r', false);
    forwardKey(ImGuiKey_Escape, D_KEY_ESCAPE, L'\x1b', false);
    forwardKey(ImGuiKey_Space, D_KEY_SPACE);
    forwardKey(ImGuiKey_LeftArrow, D_KEY_LEFT);
    forwardKey(ImGuiKey_RightArrow, D_KEY_RIGHT);
    forwardKey(ImGuiKey_UpArrow, D_KEY_UP);
    forwardKey(ImGuiKey_DownArrow, D_KEY_DOWN);
    forwardKey(ImGuiKey_Insert, D_KEY_INSERT, 0, false);
    forwardKey(ImGuiKey_Home, D_KEY_HOME, 0, false);
    forwardKey(ImGuiKey_End, D_KEY_END, 0, false);
    forwardKey(ImGuiKey_PageUp, D_KEY_PAGE_UP);
    forwardKey(ImGuiKey_PageDown, D_KEY_PAGE_DOWN);
    forwardKey(ImGuiKey_Delete, D_KEY_DELETE);

    forwardKey(ImGuiKey_Apostrophe, D_KEY_APOSTROPHE);
    forwardKey(ImGuiKey_Comma, D_KEY_COMMA);
    forwardKey(ImGuiKey_Minus, D_KEY_MINUS);
    forwardKey(ImGuiKey_Period, D_KEY_PERIOD);
    forwardKey(ImGuiKey_Slash, D_KEY_SLASH);
    forwardKey(ImGuiKey_Semicolon, D_KEY_SEMICOLON);
    forwardKey(ImGuiKey_Equal, D_KEY_EQUAL);
    forwardKey(ImGuiKey_LeftBracket, D_KEY_LEFT_BRACKET);
    forwardKey(ImGuiKey_Backslash, D_KEY_BACKSLASH);
    forwardKey(ImGuiKey_RightBracket, D_KEY_RIGHT_BRACKET);
    forwardKey(ImGuiKey_GraveAccent, D_KEY_GRAVE_ACCENT);

    forwardKey(ImGuiKey_CapsLock, D_KEY_CAPS_LOCK, 0, false);
    forwardKey(ImGuiKey_ScrollLock, D_KEY_SCROLL_LOCK, 0, false);
    forwardKey(ImGuiKey_NumLock, D_KEY_NUM_LOCK, 0, false);
    forwardKey(ImGuiKey_PrintScreen, D_KEY_PRINT_SCREEN, 0, false);
    forwardKey(ImGuiKey_Pause, D_KEY_PAUSE, 0, false);

    forwardKey(ImGuiKey_LeftShift, D_KEY_LEFT_SHIFT, 0, false);
    forwardKey(ImGuiKey_LeftCtrl, D_KEY_LEFT_CONTROL, 0, false);
    forwardKey(ImGuiKey_LeftAlt, D_KEY_LEFT_ALT, 0, false);
    forwardKey(ImGuiKey_LeftSuper, D_KEY_LEFT_SUPER, 0, false);
    forwardKey(ImGuiKey_RightShift, D_KEY_RIGHT_SHIFT, 0, false);
    forwardKey(ImGuiKey_RightCtrl, D_KEY_RIGHT_CONTROL, 0, false);
    forwardKey(ImGuiKey_RightAlt, D_KEY_RIGHT_ALT, 0, false);
    forwardKey(ImGuiKey_RightSuper, D_KEY_RIGHT_SUPER, 0, false);
    forwardKey(ImGuiKey_Menu, D_KEY_MENU, 0, false);

    forwardKeyRange(ImGuiKey_0, D_KEY_0, 10);
    forwardKeyRange(ImGuiKey_A, D_KEY_A, 26);
    forwardKeyRange(ImGuiKey_F1, D_KEY_F1, 24);
    forwardKeyRange(ImGuiKey_Keypad0, D_KEY_KP_0, 10);

    forwardKey(ImGuiKey_KeypadDecimal, D_KEY_KP_DECIMAL);
    forwardKey(ImGuiKey_KeypadDivide, D_KEY_KP_DIVIDE);
    forwardKey(ImGuiKey_KeypadMultiply, D_KEY_KP_MULTIPLY);
    forwardKey(ImGuiKey_KeypadSubtract, D_KEY_KP_SUBTRACT);
    forwardKey(ImGuiKey_KeypadAdd, D_KEY_KP_ADD);
    forwardKey(ImGuiKey_KeypadEqual, D_KEY_KP_EQUAL);
}

void editor::PlayInput::releaseKeys(int mods){
    for (int key : pressedKeys){
        Engine::systemKeyUp(key, false, mods);
    }
    pressedKeys.clear();
}

void editor::PlayInput::releaseMouseButtons(int mods){
    // The engine drops events outside the view rect, so keep the synthetic release inside it
    const Rect view = Engine::getViewRect();
    const float x = std::clamp(mousePos.x, view.getX(), view.getX() + std::max(0.0f, view.getWidth()));
    const float y = std::clamp(mousePos.y, view.getY(), view.getY() + std::max(0.0f, view.getHeight()));
    for (int button : pressedMouseButtons){
        if (project->isSimulateTouch()) {
            Engine::systemTouchEnd(touchFinger, x, y);
        } else {
            Engine::systemMouseUp(button, x, y, mods);
        }
    }
    pressedMouseButtons.clear();
    touchFinger = 0;
    if (fingerPinned) {
        Engine::systemTouchEnd(0, pinnedPos.x, pinnedPos.y);
        fingerPinned = false;
    }
}

int editor::PlayInput::engineKey(ImGuiKey key) {
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return D_KEY_0 + (key - ImGuiKey_0);
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return D_KEY_A + (key - ImGuiKey_A);
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) return D_KEY_F1 + (key - ImGuiKey_F1);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return D_KEY_KP_0 + (key - ImGuiKey_Keypad0);
    switch (key) {
        case ImGuiKey_Tab: return D_KEY_TAB;
        case ImGuiKey_Backspace: return D_KEY_BACKSPACE;
        case ImGuiKey_Enter: return D_KEY_ENTER;
        case ImGuiKey_KeypadEnter: return D_KEY_KP_ENTER;
        case ImGuiKey_Escape: return D_KEY_ESCAPE;
        case ImGuiKey_Space: return D_KEY_SPACE;
        case ImGuiKey_LeftArrow: return D_KEY_LEFT;
        case ImGuiKey_RightArrow: return D_KEY_RIGHT;
        case ImGuiKey_UpArrow: return D_KEY_UP;
        case ImGuiKey_DownArrow: return D_KEY_DOWN;
        case ImGuiKey_Insert: return D_KEY_INSERT;
        case ImGuiKey_Home: return D_KEY_HOME;
        case ImGuiKey_End: return D_KEY_END;
        case ImGuiKey_PageUp: return D_KEY_PAGE_UP;
        case ImGuiKey_PageDown: return D_KEY_PAGE_DOWN;
        case ImGuiKey_Delete: return D_KEY_DELETE;
        case ImGuiKey_Apostrophe: return D_KEY_APOSTROPHE;
        case ImGuiKey_Comma: return D_KEY_COMMA;
        case ImGuiKey_Minus: return D_KEY_MINUS;
        case ImGuiKey_Period: return D_KEY_PERIOD;
        case ImGuiKey_Slash: return D_KEY_SLASH;
        case ImGuiKey_Semicolon: return D_KEY_SEMICOLON;
        case ImGuiKey_Equal: return D_KEY_EQUAL;
        case ImGuiKey_LeftBracket: return D_KEY_LEFT_BRACKET;
        case ImGuiKey_Backslash: return D_KEY_BACKSLASH;
        case ImGuiKey_RightBracket: return D_KEY_RIGHT_BRACKET;
        case ImGuiKey_GraveAccent: return D_KEY_GRAVE_ACCENT;
        case ImGuiKey_CapsLock: return D_KEY_CAPS_LOCK;
        case ImGuiKey_ScrollLock: return D_KEY_SCROLL_LOCK;
        case ImGuiKey_NumLock: return D_KEY_NUM_LOCK;
        case ImGuiKey_PrintScreen: return D_KEY_PRINT_SCREEN;
        case ImGuiKey_Pause: return D_KEY_PAUSE;
        case ImGuiKey_LeftShift: return D_KEY_LEFT_SHIFT;
        case ImGuiKey_LeftCtrl: return D_KEY_LEFT_CONTROL;
        case ImGuiKey_LeftAlt: return D_KEY_LEFT_ALT;
        case ImGuiKey_LeftSuper: return D_KEY_LEFT_SUPER;
        case ImGuiKey_RightShift: return D_KEY_RIGHT_SHIFT;
        case ImGuiKey_RightCtrl: return D_KEY_RIGHT_CONTROL;
        case ImGuiKey_RightAlt: return D_KEY_RIGHT_ALT;
        case ImGuiKey_RightSuper: return D_KEY_RIGHT_SUPER;
        case ImGuiKey_Menu: return D_KEY_MENU;
        case ImGuiKey_KeypadDecimal: return D_KEY_KP_DECIMAL;
        case ImGuiKey_KeypadDivide: return D_KEY_KP_DIVIDE;
        case ImGuiKey_KeypadMultiply: return D_KEY_KP_MULTIPLY;
        case ImGuiKey_KeypadSubtract: return D_KEY_KP_SUBTRACT;
        case ImGuiKey_KeypadAdd: return D_KEY_KP_ADD;
        case ImGuiKey_KeypadEqual: return D_KEY_KP_EQUAL;
        default: return D_KEY_UNKNOWN;
    }
}

int editor::PlayInput::engineGamepadButton(ImGuiKey key) {
    // ImGui names the face buttons by position: Down = A, Right = B, Left = X, Up = Y
    switch (key) {
        case ImGuiKey_GamepadFaceDown: return D_GAMEPAD_BUTTON_A;
        case ImGuiKey_GamepadFaceRight: return D_GAMEPAD_BUTTON_B;
        case ImGuiKey_GamepadFaceLeft: return D_GAMEPAD_BUTTON_X;
        case ImGuiKey_GamepadFaceUp: return D_GAMEPAD_BUTTON_Y;
        case ImGuiKey_GamepadL1: return D_GAMEPAD_BUTTON_LEFT_BUMPER;
        case ImGuiKey_GamepadR1: return D_GAMEPAD_BUTTON_RIGHT_BUMPER;
        case ImGuiKey_GamepadBack: return D_GAMEPAD_BUTTON_BACK;
        case ImGuiKey_GamepadStart: return D_GAMEPAD_BUTTON_START;
        case ImGuiKey_GamepadL3: return D_GAMEPAD_BUTTON_LEFT_THUMB;
        case ImGuiKey_GamepadR3: return D_GAMEPAD_BUTTON_RIGHT_THUMB;
        case ImGuiKey_GamepadDpadUp: return D_GAMEPAD_BUTTON_DPAD_UP;
        case ImGuiKey_GamepadDpadRight: return D_GAMEPAD_BUTTON_DPAD_RIGHT;
        case ImGuiKey_GamepadDpadDown: return D_GAMEPAD_BUTTON_DPAD_DOWN;
        case ImGuiKey_GamepadDpadLeft: return D_GAMEPAD_BUTTON_DPAD_LEFT;
        default: return -1;
    }
}
