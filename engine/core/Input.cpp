// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT


#include "Input.h"

#include "Engine.h"
#include "input/InputActions.h"
#include "input/InputControlNames.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

ControlState Input::keyState[D_KEY_LAST + 1];
ControlState Input::mouseState[D_MOUSE_BUTTON_LAST + 1];
Vector2 Input::mousePosition;
Vector2 Input::mouseScroll;
std::vector<Touch> Input::touches;
std::vector<Gamepad> Input::gamepads;
bool Input::mousedEntered;
int Input::modifiers;
double Input::multiTapTime = 0.3;       // UISystem's double-click time
float Input::multiTapDistance = 24.0f;  // canvas px

namespace {

    // the input frame: the frame that runs (inFrame) or runs next
    uint64_t inputFrame = 1;
    bool inFrame = false;
    double inputTime = 0.0;

    // this frame's published values and what accumulates for the next frame
    Vector2 mouseDelta;
    Vector2 pendingMouseDelta;
    Vector2 scrollDelta;
    Vector2 pendingScroll;
    Vector2 lastMouseMovement;
    bool lastMouseMovementValid = false;
    std::string inputString;
    std::u32string pendingCharacters;

    // the first control pressed, per frame (a press during a frame is stamped for the next one)
    struct PressedControl { uint64_t frame = 0; std::string name; };
    PressedControl pressedControls[2];

    // a disconnected pad keeps its release edges until a frame saw them
    struct RemovedGamepad { Gamepad pad; uint64_t frame = 0; };
    std::vector<RemovedGamepad> removedGamepads;

    // a finger from its start until the frame that reports its end
    struct TouchRecord {
        Touch touch;                 // as published
        Vector2 position;            // now
        Vector2 framePosition;       // at the last publication
        double beginTime = 0.0;
        double lastChangeTime = 0.0;
        bool published = false;      // its Began was published
        bool ended = false;          // lifted or interrupted, not yet reported
        TouchPhase endPhase = TouchPhase::Ended;
        bool reportedEnd = false;    // reported in this frame: dropped by endFrame
        bool hidden = false;         // multiTouchEnabled off: not the first finger
    };
    std::vector<TouchRecord> touchRecords;
    std::vector<Touch> frameTouches;
    bool multiTouchEnabled = true;

    // the last touch that ended (multi-tap)
    struct LastTap { bool valid = false; double endTime = 0.0; Vector2 start; int tapCount = 0; };
    LastTap lastTap;

    TouchRecord* activeRecord(int fingerId) {
        for (TouchRecord& record : touchRecords) {
            if (record.touch.fingerId == fingerId && !record.ended) return &record;
        }
        return nullptr;
    }

    std::string toUtf8(const std::u32string& text) {
        std::string out;
        for (char32_t codepoint : text) {
            if (codepoint < 0x80) {
                out += static_cast<char>(codepoint);
            } else if (codepoint < 0x800) {
                out += static_cast<char>(0xC0 | (codepoint >> 6));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else if (codepoint < 0x10000) {
                out += static_cast<char>(0xE0 | (codepoint >> 12));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (codepoint >> 18));
                out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
        }
        return out;
    }

    bool validKey(int key) { return key >= 0 && key <= D_KEY_LAST; }
    bool validMouseButton(int button) { return button >= 0 && button <= D_MOUSE_BUTTON_LAST; }
    bool validGamepadButton(int button) { return button >= 0 && button <= D_GAMEPAD_BUTTON_LAST; }
    bool validGamepadAxis(int axis) { return axis >= 0 && axis <= D_GAMEPAD_AXIS_LAST; }

    bool isTrigger(int axis) { return axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER; }

    // the direction an axis is pressed in for getPressedControl: past 0.5 (triggers: half way)
    int axisDirection(int axis, float value) {
        if (isTrigger(axis)) return ((value + 1.0f) * 0.5f >= 0.5f) ? 1 : 0;
        if (value >= 0.5f) return 1;
        if (value <= -0.5f) return -1;
        return 0;
    }

}

uint64_t Input::stampFrame(){
    // a change during a frame belongs to the next one: this frame's edges stay what they were
    return inputFrame + (inFrame ? 1 : 0);
}

void Input::notePressedControl(const std::string& name){
    if (name.empty()) return;
    const uint64_t frame = stampFrame();
    PressedControl& slot = pressedControls[frame & 1];
    if (slot.frame != frame) {
        slot.frame = frame;
        slot.name = name;
    }
}

namespace {

    // a press: an edge only when it was up (a platform's repeated down is a repeat)
    bool pressControl(ControlState& state, uint64_t frame){
        if (state.held) return false;
        state.held = true;
        state.downFrame = frame;
        return true;
    }

    bool releaseControl(ControlState& state, uint64_t frame){
        if (!state.held) return false;
        state.held = false;
        state.upFrame = frame;
        return true;
    }

}

void Input::addKeyPressed(int key, bool repeat){
    if (!validKey(key)) return;
    // key repeat never creates an edge
    if (repeat && keyState[key].held) return;
    if (pressControl(keyState[key], stampFrame()))
        notePressedControl(InputControlNames::keyName(key));
}

void Input::releaseKeyPressed(int key){
    if (!validKey(key)) return;
    releaseControl(keyState[key], stampFrame());
}

void Input::addMousePressed(int button){
    if (!validMouseButton(button)) return;
    if (pressControl(mouseState[button], stampFrame()))
        notePressedControl(InputControlNames::mouseButtonName(button));
}

void Input::releaseMousePressed(int button){
    if (!validMouseButton(button)) return;
    releaseControl(mouseState[button], stampFrame());
}

void Input::setMousePosition(float x, float y){
    mousePosition.x = x;
    mousePosition.y = y;
}

void Input::setMouseScroll(float xoffset, float yoffset){
    mouseScroll.x = xoffset;
    mouseScroll.y = yoffset;
    pendingScroll.x += xoffset;
    pendingScroll.y += yoffset;
}

void Input::addMouseMovement(float x, float y){
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (lastMouseMovementValid) {
        pendingMouseDelta.x += x - lastMouseMovement.x;
        pendingMouseDelta.y += y - lastMouseMovement.y;
    }
    lastMouseMovement = Vector2(x, y);
    lastMouseMovementValid = true;
}

void Input::addMouseDelta(float dx, float dy){
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    pendingMouseDelta.x += dx;
    pendingMouseDelta.y += dy;
}

void Input::resetMouseMovement(){
    lastMouseMovementValid = false;
}

void Input::addTouch(int pointer, float x, float y){
    if (findTouchIndex(pointer) == (size_t)-1) {
        Touch legacy;
        legacy.fingerId = pointer;
        legacy.position = Vector2(x, y);
        legacy.startPosition = legacy.position;
        touches.push_back(legacy);
    }
    if (activeRecord(pointer))
        return;

    const Vector2 position(x, y);
    TouchRecord record;
    record.touch.fingerId = pointer;
    record.touch.phase = TouchPhase::Began;
    record.touch.position = position;
    record.touch.startPosition = position;
    record.position = position;
    record.framePosition = position;
    record.beginTime = inputTime;
    record.lastChangeTime = inputTime;

    // a quick tap near the last one's start (that one short too) counts on from it
    int tapCount = 1;
    if (lastTap.valid && inputTime - lastTap.endTime <= multiTapTime &&
            (position - lastTap.start).length() <= multiTapDistance) {
        tapCount = lastTap.tapCount + 1;
    }
    record.touch.tapCount = tapCount;

    if (!multiTouchEnabled) {
        for (const TouchRecord& other : touchRecords) {
            if (!other.ended && !other.hidden) {
                record.hidden = true;
                break;
            }
        }
    }
    touchRecords.push_back(record);
}

void Input::setTouchPosition(int pointer, float x, float y){
    size_t index = findTouchIndex(pointer);
    if (index < touches.size())
        touches[index].position = Vector2(x, y);
    if (TouchRecord* record = activeRecord(pointer))
        record->position = Vector2(x, y);
}

void Input::endTouch(int pointer, TouchPhase phase){
    size_t index = findTouchIndex(pointer);
    if (index != (size_t)-1)
        touches.erase(touches.begin() + index);

    TouchRecord* record = activeRecord(pointer);
    if (!record)
        return;
    record->ended = true;
    record->endPhase = phase;

    if (phase == TouchPhase::Ended && !record->hidden && inputTime - record->beginTime < multiTapTime) {
        lastTap.valid = true;
        lastTap.endTime = inputTime;
        lastTap.start = record->touch.startPosition;
        lastTap.tapCount = record->touch.tapCount;
    } else if (!record->hidden) {
        lastTap.valid = false;
    }
}

void Input::removeTouch(int pointer){
    endTouch(pointer, TouchPhase::Ended);
}

void Input::cancelTouch(int pointer){
    endTouch(pointer, TouchPhase::Canceled);
}

void Input::clearTouches(){
    std::vector<int> ids;
    for (const TouchRecord& record : touchRecords) {
        if (!record.ended) ids.push_back(record.touch.fingerId);
    }
    for (int id : ids)
        cancelTouch(id);
    touches.clear();
}

void Input::addGamepad(int id, const std::string& name){
    size_t index = findGamepadIndex(id);
    if (index == (size_t)-1){
        Gamepad gamepad;
        gamepad.id = id;
        gamepad.name = name;
        // Triggers rest at -1 in the [-1, 1] convention. Platforms seed their own
        // local state to -1 and skip the connect event, so seed the queryable
        // state here too or getGamepadAxis would report a resting trigger as 0.
        gamepad.axes[D_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
        gamepad.axes[D_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
        gamepads.push_back(gamepad);
    }else{
        gamepads[index].name = name;
    }
}

void Input::removeGamepad(int id){
    size_t index = findGamepadIndex(id);
    if (index == (size_t)-1)
        return;

    // its held buttons are released: the frame that sees the disconnect sees their ups
    RemovedGamepad removed;
    removed.pad = gamepads[index];
    removed.frame = stampFrame();
    bool anyReleased = false;
    for (ControlState& button : removed.pad.buttons) {
        if (releaseControl(button, removed.frame)) anyReleased = true;
    }
    if (anyReleased)
        removedGamepads.push_back(removed);

    gamepads.erase(gamepads.begin() + index);
}

void Input::addGamepadButtonPressed(int id, int button){
    size_t index = findGamepadIndex(id);
    if (index != (size_t)-1 && validGamepadButton(button)) {
        if (pressControl(gamepads[index].buttons[button], stampFrame()))
            notePressedControl(InputControlNames::gamepadButtonName(button));
    }
}

void Input::releaseGamepadButtonPressed(int id, int button){
    size_t index = findGamepadIndex(id);
    if (index != (size_t)-1 && validGamepadButton(button))
        releaseControl(gamepads[index].buttons[button], stampFrame());
}

void Input::setGamepadAxisValue(int id, int axis, float value){
    size_t index = findGamepadIndex(id);
    if (index == (size_t)-1 || !validGamepadAxis(axis))
        return;
    if (!std::isfinite(value))
        value = isTrigger(axis) ? -1.0f : 0.0f;
    float& current = gamepads[index].axes[axis];
    const int before = axisDirection(axis, current);
    const int after = axisDirection(axis, value);
    current = value;
    if (after != 0 && after != before) {
        std::string name = InputControlNames::gamepadAxisName(axis);
        if (!isTrigger(axis)) name += (after > 0) ? "+" : "-";
        notePressedControl(name);
    }
}

void Input::addMouseEntered(){
    mousedEntered = true;
}

void Input::releaseMouseEntered(){
    mousedEntered = false;
}

void Input::setModifiers(int mods){
    modifiers = mods;
}

void Input::addInputCharacter(uint32_t codepoint){
    // Unity's inputString: backspace is "\b", return "\n"; other control characters are left out
    if (codepoint == '\r') codepoint = '\n';
    if (codepoint != '\b' && codepoint != '\n' && (codepoint < 32 || codepoint == 0x7f))
        return;
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        return;
    pendingCharacters.push_back(static_cast<char32_t>(codepoint));
}

// ---- input frame

void Input::beginFrame(double unscaledDeltaTime){
    InputSuppression suppression;
    suppression.keyboard = Engine::isTextInputFocused();
    suppression.gamepadNavigation = Engine::wantsGamepadNavigation();
    beginFrame(unscaledDeltaTime, suppression);
}

void Input::beginFrame(double unscaledDeltaTime, const InputSuppression& suppression){
    if (!std::isfinite(unscaledDeltaTime) || unscaledDeltaTime < 0.0)
        unscaledDeltaTime = 0.0;
    inFrame = true;
    inputTime += unscaledDeltaTime;

    mouseDelta = pendingMouseDelta;
    pendingMouseDelta = Vector2(0, 0);
    scrollDelta = pendingScroll;
    pendingScroll = Vector2(0, 0);
    inputString = toUtf8(pendingCharacters);
    pendingCharacters.clear();

    // touches: Began once published, then Moved / Stationary, then Ended / Canceled the frame after
    frameTouches.clear();
    for (TouchRecord& record : touchRecords) {
        Touch& touch = record.touch;
        if (!record.published) {
            record.published = true;
            touch.phase = TouchPhase::Began;
            touch.position = record.position;
            touch.deltaPosition = Vector2(0, 0);
            touch.deltaTime = 0.0f;
            record.framePosition = record.position;
            record.lastChangeTime = inputTime;
        } else {
            const Vector2 delta = record.position - record.framePosition;
            const bool moved = delta.x != 0.0f || delta.y != 0.0f;
            touch.position = record.position;
            touch.deltaPosition = delta;
            touch.deltaTime = static_cast<float>(inputTime - record.lastChangeTime);
            if (record.ended) {
                touch.phase = record.endPhase;
                record.reportedEnd = true;
            } else {
                touch.phase = moved ? TouchPhase::Moved : TouchPhase::Stationary;
            }
            if (moved) {
                record.lastChangeTime = inputTime;
                record.framePosition = record.position;
            }
        }
        if (!record.hidden)
            frameTouches.push_back(touch);
    }

    InputActions::update(unscaledDeltaTime, suppression);

    // the actions' edges, after every action is updated (a handler reading another action
    // sees this frame's values)
    for (const std::string& name : InputActions::getFrameButtonDowns())
        Engine::onButtonDown.call(name);
    for (const std::string& name : InputActions::getFrameButtonUps())
        Engine::onButtonUp.call(name);
}

void Input::endFrame(){
    touchRecords.erase(std::remove_if(touchRecords.begin(), touchRecords.end(),
                                      [](const TouchRecord& record) { return record.reportedEnd; }),
                       touchRecords.end());
    frameTouches.erase(std::remove_if(frameTouches.begin(), frameTouches.end(), [](const Touch& touch) {
                           return touch.phase == TouchPhase::Ended || touch.phase == TouchPhase::Canceled;
                       }),
                       frameTouches.end());
    for (Touch& touch : frameTouches) {
        touch.deltaPosition = Vector2(0, 0);
        if (touch.phase == TouchPhase::Moved) touch.phase = TouchPhase::Stationary;
    }

    removedGamepads.erase(std::remove_if(removedGamepads.begin(), removedGamepads.end(),
                                         [](const RemovedGamepad& removed) { return removed.frame <= inputFrame; }),
                          removedGamepads.end());

    mouseDelta = Vector2(0, 0);
    scrollDelta = Vector2(0, 0);
    inputString.clear();

    InputActions::endFrame();

    inputFrame++;
    inFrame = false;
}

uint64_t Input::getInputFrame(){
    return inputFrame;
}

bool Input::isInFrame(){
    return inFrame;
}

double Input::getInputTime(){
    return inputTime;
}

void Input::releaseAll(){
    const uint64_t frame = stampFrame();
    for (ControlState& state : keyState) releaseControl(state, frame);
    for (ControlState& state : mouseState) releaseControl(state, frame);
    for (Gamepad& gamepad : gamepads) {
        for (ControlState& state : gamepad.buttons) releaseControl(state, frame);
    }
    clearTouches();
    modifiers = 0;
    // movement while away is not the player's
    pendingMouseDelta = Vector2(0, 0);
    pendingScroll = Vector2(0, 0);
    lastMouseMovementValid = false;
}

void Input::reset(){
    for (ControlState& state : keyState) state = ControlState();
    for (ControlState& state : mouseState) state = ControlState();
    for (Gamepad& gamepad : gamepads) {
        for (ControlState& state : gamepad.buttons) state = ControlState();
    }
    removedGamepads.clear();
    touches.clear();
    touchRecords.clear();
    frameTouches.clear();
    lastTap = LastTap();
    multiTouchEnabled = true;
    modifiers = 0;
    mouseDelta = pendingMouseDelta = Vector2(0, 0);
    scrollDelta = pendingScroll = Vector2(0, 0);
    lastMouseMovementValid = false;
    inputString.clear();
    pendingCharacters.clear();
    pressedControls[0] = PressedControl();
    pressedControls[1] = PressedControl();
    inFrame = false;
}

// ---- queries

bool Input::isKeyPressed(int key){
    return validKey(key) && keyState[key].held;
}

bool Input::isMousePressed(int button){
    return validMouseButton(button) && mouseState[button].held;
}

bool Input::isTouch(){
    return (touches.size() > 0);
}

bool Input::isMouseEntered(){
    return mousedEntered;
}

bool Input::getKeyDown(int key){
    return validKey(key) && keyState[key].downFrame == inputFrame;
}

bool Input::getKeyUp(int key){
    return validKey(key) && keyState[key].upFrame == inputFrame;
}

bool Input::getMouseButtonDown(int button){
    return validMouseButton(button) && mouseState[button].downFrame == inputFrame;
}

bool Input::getMouseButtonUp(int button){
    return validMouseButton(button) && mouseState[button].upFrame == inputFrame;
}

bool Input::getGamepadButtonDown(int id, int button){
    if (!validGamepadButton(button)) return false;
    for (const Gamepad& gamepad : gamepads) {
        if ((id < 0 || gamepad.id == id) && gamepad.buttons[button].downFrame == inputFrame) return true;
    }
    return false;
}

bool Input::getGamepadButtonUp(int id, int button){
    if (!validGamepadButton(button)) return false;
    for (const Gamepad& gamepad : gamepads) {
        if ((id < 0 || gamepad.id == id) && gamepad.buttons[button].upFrame == inputFrame) return true;
    }
    for (const RemovedGamepad& removed : removedGamepads) {
        if ((id < 0 || removed.pad.id == id) && removed.pad.buttons[button].upFrame == inputFrame) return true;
    }
    return false;
}

bool Input::isAnyKeyPressed(){
    for (const ControlState& state : keyState) if (state.held) return true;
    for (const ControlState& state : mouseState) if (state.held) return true;
    for (const Gamepad& gamepad : gamepads) {
        for (const ControlState& state : gamepad.buttons) if (state.held) return true;
    }
    return false;
}

bool Input::isAnyKeyDown(){
    for (const ControlState& state : keyState) if (state.downFrame == inputFrame) return true;
    for (const ControlState& state : mouseState) if (state.downFrame == inputFrame) return true;
    for (const Gamepad& gamepad : gamepads) {
        for (const ControlState& state : gamepad.buttons) if (state.downFrame == inputFrame) return true;
    }
    return false;
}

std::string Input::getPressedControl(){
    const PressedControl& slot = pressedControls[inputFrame & 1];
    return slot.frame == inputFrame ? slot.name : std::string();
}

std::string Input::getInputString(){
    return inputString;
}

Vector2 Input::getMouseDelta(){
    return mouseDelta;
}

Vector2 Input::getMouseScrollDelta(){
    return scrollDelta;
}

Vector2 Input::getMousePosition(){
    return mousePosition;
}

Vector2 Input::getMouseScroll(){
    return mouseScroll;
}

Vector2 Input::getTouchPosition(int pointer){
    size_t index = findTouchIndex(pointer);
    if (index < touches.size())
        return touches[index].position;

    return Vector2(-1, -1);
}

std::vector<Touch> Input::getTouches(){
    // the fingers down now, with what the frame published of them
    std::vector<Touch> list = touches;
    for (Touch& touch : list) {
        for (const TouchRecord& record : touchRecords) {
            if (record.touch.fingerId == touch.fingerId && !record.ended) {
                const Vector2 position = touch.position;
                touch = record.touch;
                touch.position = position;
                break;
            }
        }
    }
    return list;
}

size_t Input::numTouches(){
    return touches.size();
}

int Input::getTouchCount(){
    return static_cast<int>(frameTouches.size());
}

Touch Input::getTouch(int index){
    if (index >= 0 && index < static_cast<int>(frameTouches.size()))
        return frameTouches[index];
    Touch none;
    none.fingerId = -1;
    return none;
}

const std::vector<Touch>& Input::getFrameTouches(){
    return frameTouches;
}

bool Input::findTouchById(int fingerId, Touch& out){
    // a finger that ended and began again in one frame is listed twice: the one down wins
    bool found = false;
    for (const Touch& touch : frameTouches) {
        if (touch.fingerId != fingerId) continue;
        out = touch;
        found = true;
        if (touch.phase != TouchPhase::Ended && touch.phase != TouchPhase::Canceled) break;
    }
    return found;
}

void Input::setMultiTouchEnabled(bool enabled){
    multiTouchEnabled = enabled;
}

bool Input::isMultiTouchEnabled(){
    return multiTouchEnabled;
}

bool Input::isTouchVisible(int fingerId){
    const TouchRecord* record = activeRecord(fingerId);
    return record && !record->hidden;
}

bool Input::isGamepadConnected(int id){
    return (findGamepadIndex(id) != (size_t)-1);
}

std::string Input::getGamepadName(int id){
    size_t index = findGamepadIndex(id);
    if (index != (size_t)-1)
        return gamepads[index].name;

    return "";
}

bool Input::isGamepadButtonPressed(int id, int button){
    size_t index = findGamepadIndex(id);
    if (index != (size_t)-1 && validGamepadButton(button))
        return gamepads[index].buttons[button].held;

    return false;
}

float Input::getGamepadAxis(int id, int axis){
    size_t index = findGamepadIndex(id);
    if (index != (size_t)-1 && validGamepadAxis(axis))
        return gamepads[index].axes[axis];

    return 0.0f;
}

std::vector<Gamepad> Input::getGamepads(){
    return gamepads;
}

size_t Input::numGamepads(){
    return gamepads.size();
}

int Input::getGamepadId(size_t index){
    if (index < gamepads.size())
        return gamepads[index].id;

    return -1;
}

int Input::getModifiers(){
    return modifiers;
}

size_t Input::findTouchIndex(int pointer){
    size_t index = -1;
    for (size_t i = 0; i < touches.size(); i++){
        if (touches[i].fingerId == pointer)
            index = i;
    }

    return index;
}

size_t Input::findGamepadIndex(int id){
    size_t index = -1;
    for (size_t i = 0; i < gamepads.size(); i++){
        if (gamepads[i].id == id)
            index = i;
    }

    return index;
}
