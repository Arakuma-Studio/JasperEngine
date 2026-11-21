// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GamepadCursor.h"

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/ButtonComponent.h"
#include "input/InputScheme.h"
#include "subsystem/UIDocumentSystem.h"
#include "subsystem/UISystem.h"

#include <algorithm>
#include <cmath>
#include <optional>

using namespace jasperengine;

namespace jasperengine {

    // the file's helpers reach the Engine internals through GamepadCursor's friendship
    struct GamepadCursorAccess {
        static void scenes(std::vector<Scene*>& out){ GamepadCursor::scenesTopFirst(out); }
        static void cancelPress(){ GamepadCursor::cancelPress(); }
    };

}

void GamepadCursor::scenesTopFirst(std::vector<Scene*>& out){
    Engine::getInputScenesTopFirst(out);
}

void GamepadCursor::cancelPress(){
    Engine::cancelUIPointer(D_MOUSE_BUTTON_LEFT, false);
}

namespace {

    GamepadCursorSettings cursorDefaults;
    bool cursorEnabled = true;
    std::optional<float> speedOverride;
    std::optional<float> scrollSpeedOverride;

    bool cursorActive = false;
    // a position this session (an earlier activation or a warp): the next activation starts there
    bool cursorHasPosition = false;
    Vector2 cursorPosition(0.0f, 0.0f);
    // seconds the left stick has been past the deadzone (the acceleration ramp)
    double stickHeld = 0.0;
    // wheel lines the right stick moved that make no whole line yet
    Vector2 scrollLines(0.0f, 0.0f);
    // the pad whose A holds the pointer down (-1: none)
    int pressPad = -1;
    // the focus the cursor last saw: another one (the d-pad, a script, autofocus) moves the cursor to it
    const void* seenFocus = nullptr;

    float cursorFinite(float value, float fallback){
        return std::isfinite(value) ? value : fallback;
    }

    // the input scenes, top layer first, in a buffer reused every frame (no allocation per frame)
    const std::vector<Scene*>& cursorScenes(){
        static std::vector<Scene*> buffer;
        GamepadCursorAccess::scenes(buffer);
        return buffer;
    }

    // the focused element of the top scene that has one, its centre
    bool focusCenter(const std::vector<Scene*>& scenes, Vector2& center, const void*& focus){
        for (Scene* scene : scenes){
            float x = 0.0f, y = 0.0f;
            if (scene->getSystem<UIDocumentSystem>()->getFocusCenter(x, y, focus)){
                center = Vector2(x, y);
                return true;
            }
        }
        focus = nullptr;
        return false;
    }

    bool overControl(const std::vector<Scene*>& scenes){
        for (Scene* scene : scenes){
            if (scene->getSystem<UIDocumentSystem>()->isMouseOverControl())
                return true;
            const auto& pointers = scene->getSystem<UISystem>()->getPointers();
            auto it = pointers.find(-1);
            if (it != pointers.end() && it->second.hover != NULL_ENTITY && scene->findComponent<ButtonComponent>(it->second.hover))
                return true;
        }
        return false;
    }

    void clampToCanvas(Vector2& point){
        point.x = std::clamp(point.x, 0.0f, (float)std::max(Engine::getCanvasWidth(), 0));
        point.y = std::clamp(point.y, 0.0f, (float)std::max(Engine::getCanvasHeight(), 0));
    }

    // the shown overlay goes in the top scene with a Gamepad Cursor document, hidden elsewhere
    void placeOverlay(const std::vector<Scene*>& scenes, bool show){
        bool placed = false;
        const GamepadCursorSettings& s = cursorDefaults;
        for (Scene* scene : scenes){
            UIDocumentSystem* documents = scene->getSystem<UIDocumentSystem>().get();
            const bool here = show && !placed && documents->hasCursorDocument();
            documents->setCursorOverlay(here, cursorPosition.x, cursorPosition.y, s.graphic, s.size, s.hotSpot.x, s.hotSpot.y);
            placed = placed || here;
        }
    }

    // the pad whose stick is pushed furthest: any pad drives the one cursor
    Vector2 strongestStick(int axisX, int axisY){
        Vector2 best(0.0f, 0.0f);
        float bestLength = 0.0f;
        for (size_t i = 0; i < Input::numGamepads(); i++){
            const int id = Input::getGamepadId(i);
            const Vector2 value(cursorFinite(Input::getGamepadAxis(id, axisX), 0.0f), cursorFinite(Input::getGamepadAxis(id, axisY), 0.0f));
            const float length = std::sqrt(value.x * value.x + value.y * value.y);
            if (length > bestLength){
                bestLength = length;
                best = value;
            }
        }
        return best;
    }

    // a focus the cursor did not make (the d-pad, a script, autofocus): it jumps there
    bool jumpToNewFocus(const std::vector<Scene*>& scenes){
        const void* focus = nullptr;
        Vector2 center;
        const bool found = focusCenter(scenes, center, focus);
        const bool jump = found && focus != seenFocus;
        if (jump){
            cursorPosition = center;
            clampToCanvas(cursorPosition);
        }
        seenFocus = focus;
        return jump;
    }

    void deactivateCursor(){
        cursorActive = false;
        stickHeld = 0.0;
        // a held press ends without a click
        if (pressPad >= 0){
            pressPad = -1;
            GamepadCursorAccess::cancelPress();
        }
        placeOverlay(cursorScenes(), false);
    }

}

void GamepadCursor::setDefaults(const GamepadCursorSettings& settings){
    cursorDefaults = settings;
    cursorDefaults.speed = std::max(cursorFinite(settings.speed, 1000.0f), 0.0f);
    cursorDefaults.accelerationTime = std::max(cursorFinite(settings.accelerationTime, 0.15f), 0.0f);
    cursorDefaults.slowOverControls = std::clamp(cursorFinite(settings.slowOverControls, 0.5f), 0.0f, 1.0f);
    cursorDefaults.scrollSpeed = std::max(cursorFinite(settings.scrollSpeed, 20.0f), 0.0f);
    cursorDefaults.deadzone = std::clamp(cursorFinite(settings.deadzone, 0.2f), 0.0f, 0.95f);
    cursorDefaults.size = std::max(cursorFinite(settings.size, 32.0f), 1.0f);
}

const GamepadCursorSettings& GamepadCursor::getDefaults(){
    return cursorDefaults;
}

void GamepadCursor::setEnabled(bool enabled){
    cursorEnabled = enabled;
}

bool GamepadCursor::isEnabled(){
    return cursorEnabled;
}

void GamepadCursor::setSpeed(float speed){
    speedOverride = std::max(cursorFinite(speed, cursorDefaults.speed), 0.0f);
}

float GamepadCursor::getSpeed(){
    return speedOverride.value_or(cursorDefaults.speed);
}

void GamepadCursor::setScrollSpeed(float speed){
    scrollSpeedOverride = std::max(cursorFinite(speed, cursorDefaults.scrollSpeed), 0.0f);
}

float GamepadCursor::getScrollSpeed(){
    return scrollSpeedOverride.value_or(cursorDefaults.scrollSpeed);
}

bool GamepadCursor::isActive(){
    return cursorActive;
}

Vector2 GamepadCursor::getPosition(){
    return cursorPosition;
}

void GamepadCursor::setPosition(Vector2 position){
    cursorPosition = Vector2(cursorFinite(position.x, 0.0f), cursorFinite(position.y, 0.0f));
    clampToCanvas(cursorPosition);
    cursorHasPosition = true;
    if (!cursorActive)
        return;
    Engine::dispatchCursorPointer(Engine::CURSOR_MOVE, cursorPosition.x, cursorPosition.y);
    placeOverlay(cursorScenes(), true);
}

void GamepadCursor::resetSession(){
    if (cursorActive)
        deactivateCursor();
    cursorEnabled = true;
    speedOverride.reset();
    scrollSpeedOverride.reset();
    cursorHasPosition = false;
    cursorPosition = Vector2(0.0f, 0.0f);
    pressPad = -1;
    seenFocus = nullptr;
}

void GamepadCursor::noteFocusBaseline(){
    if (!cursorActive)
        return;
    Vector2 center;
    focusCenter(cursorScenes(), center, seenFocus);
}

void GamepadCursor::update(double deltaTime){
    // off (no Gamepad Cursor document, another scheme, disabled): one cached look
    const bool wanted = cursorEnabled && InputSchemeTracker::getCurrent() == InputScheme::Gamepad && Engine::isGamepadCursorShown();
    if (!wanted){
        if (cursorActive)
            deactivateCursor();
        return;
    }
    const std::vector<Scene*>& scenes = cursorScenes();
    const float dt = (std::isfinite(deltaTime) && deltaTime > 0.0) ? (float)deltaTime : 0.0f;
    const float canvasHeight = (float)Engine::getCanvasHeight();

    bool moved = false;
    if (!cursorActive){
        // at the focused element, else where it was this session, else the canvas centre
        cursorActive = true;
        stickHeld = 0.0;
        Vector2 center;
        if (focusCenter(scenes, center, seenFocus))
            cursorPosition = center;
        else if (!cursorHasPosition)
            cursorPosition = Vector2(Engine::getCanvasWidth() * 0.5f, canvasHeight * 0.5f);
        cursorHasPosition = true;
        clampToCanvas(cursorPosition);
        moved = true;
    }else if (jumpToNewFocus(scenes)){
        moved = true;
    }

    const float deadzone = cursorDefaults.deadzone;
    // the left stick moves it: speed x |stick|^2 (fine aim near the centre), ramping up over the
    // acceleration time, slower over controls; canvas px scaled by the canvas height
    const Vector2 stick = strongestStick(D_GAMEPAD_AXIS_LEFT_X, D_GAMEPAD_AXIS_LEFT_Y);
    const float stickLength = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (stickLength > deadzone){
        stickHeld += dt;
        const float magnitude = std::min(stickLength, 1.0f);
        const float accel = cursorDefaults.accelerationTime;
        const float ramp = accel > 0.0f ? (float)std::min(stickHeld / accel, 1.0) : 1.0f;
        float speed = getSpeed() * (canvasHeight / 1080.0f) * magnitude * magnitude * ramp;
        if (overControl(scenes))
            speed *= cursorDefaults.slowOverControls;
        cursorPosition = cursorPosition + Vector2(stick.x / stickLength, stick.y / stickLength) * (speed * dt);
        clampToCanvas(cursorPosition);
        moved = true;
    }else{
        stickHeld = 0.0;
    }

    if (moved){
        Engine::dispatchCursorPointer(Engine::CURSOR_MOVE, cursorPosition.x, cursorPosition.y);
        placeOverlay(scenes, true);
    }

    // the right stick scrolls at the cursor through the wheel, a whole line (a wheel notch) at a
    // time: up and left are positive, as a wheel's
    const Vector2 scroll = strongestStick(D_GAMEPAD_AXIS_RIGHT_X, D_GAMEPAD_AXIS_RIGHT_Y);
    const float scrollLength = std::sqrt(scroll.x * scroll.x + scroll.y * scroll.y);
    if (scrollLength > deadzone && dt > 0.0f){
        const float rate = getScrollSpeed() * dt;
        const float ax = std::min(std::fabs(scroll.x), 1.0f);
        const float ay = std::min(std::fabs(scroll.y), 1.0f);
        scrollLines = scrollLines + Vector2(-std::copysign(ax * ax * rate, scroll.x), -std::copysign(ay * ay * rate, scroll.y));
        const Vector2 whole(std::trunc(scrollLines.x), std::trunc(scrollLines.y));
        if (whole.x != 0.0f || whole.y != 0.0f){
            scrollLines = scrollLines - whole;
            Engine::dispatchCursorWheel(whole.x, whole.y);
        }
    }else{
        scrollLines = Vector2(0.0f, 0.0f);
    }
}

bool GamepadCursor::pressA(int gamepad){
    if (!cursorActive || pressPad >= 0)
        return cursorActive;
    pressPad = gamepad;
    Engine::dispatchCursorPointer(Engine::CURSOR_DOWN, cursorPosition.x, cursorPosition.y);
    // a click focuses what it hits: not a focus to jump to
    noteFocusBaseline();
    return true;
}

bool GamepadCursor::releaseA(int gamepad){
    if (pressPad != gamepad)
        return false;
    pressPad = -1;
    Engine::dispatchCursorPointer(Engine::CURSOR_UP, cursorPosition.x, cursorPosition.y);
    noteFocusBaseline();
    return true;
}

void GamepadCursor::prepareNavigation(){
    if (!cursorActive)
        return;
    for (Scene* scene : cursorScenes()){
        UIDocumentSystem* documents = scene->getSystem<UIDocumentSystem>().get();
        float x, y;
        const void* focus = nullptr;
        // a focus already: the navigation starts from it
        if (documents->getFocusCenter(x, y, focus))
            return;
        if (documents->focusHoveredElement()){
            // the d-pad's move from it is the focus change the cursor follows
            noteFocusBaseline();
            return;
        }
    }
}

void GamepadCursor::followFocus(){
    if (!cursorActive)
        return;
    const std::vector<Scene*>& scenes = cursorScenes();
    if (!jumpToNewFocus(scenes))
        return;
    Engine::dispatchCursorPointer(Engine::CURSOR_MOVE, cursorPosition.x, cursorPosition.y);
    placeOverlay(scenes, true);
}
