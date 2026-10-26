// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PlayInput.h"

#include "Project.h"
#include "Engine.h"
#include "Input.h"

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

bool editor::PlayInput::forward(uint32_t sceneId, ImVec2 canvasMin, ImVec2 scale, bool hovered, bool focused) {
    ImGuiIO& io = ImGui::GetIO();
    const int mods = currentMods();
    const ImVec2 mouseDelta = io.MouseDelta;

    // A captured cursor reports a virtual position that leaves the view,
    // so the mouse follows window focus like the keyboard below.
    const bool capturedMouse = Engine::getMouseMode() == MouseMode::CAPTURED;
    bool forwardMouse = capturedMouse ? focused : hovered;
    // the game's pointer is locked while the Game view has the focus (Input::getPointerLockState)
    if (capturedMouse) {
        Engine::systemPointerLockChanged(focused);
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
            // the left button is one finger, which never hovers
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                Engine::systemTouchStart(0, x, y);
                if (accepted) pressedMouseButtons.insert(ImGuiMouseButton_Left);
            } else if (pressedMouseButtons.count(ImGuiMouseButton_Left) && (mouseDelta.x != 0 || mouseDelta.y != 0)) {
                Engine::systemTouchMove(0, x, y);
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && pressedMouseButtons.count(ImGuiMouseButton_Left)) {
                Engine::systemTouchEnd(0, x, y);
                if (accepted) pressedMouseButtons.erase(ImGuiMouseButton_Left);
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

void editor::PlayInput::releaseAll() {
    releaseKeys(0);
    releaseMouseButtons(0);
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
            Engine::systemTouchEnd(0, x, y);
        } else {
            Engine::systemMouseUp(button, x, y, mods);
        }
    }
    pressedMouseButtons.clear();
}
