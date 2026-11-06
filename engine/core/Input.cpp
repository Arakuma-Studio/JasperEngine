// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT


#include "Input.h"

#include "Engine.h"
#include "Log.h"
#include "input/InputActions.h"
#include "input/InputControlNames.h"

#include <array>
#include <algorithm>
#include <cmath>

using namespace jasperengine;

double Input::multiTapTime = 0.3;       // UISystem's double-click time
float Input::multiTapDistance = 24.0f;  // canvas px

// The state lives in fixed arrays, so a frame makes no heap allocation: a control's edges are
// counted in three windows (pending, frame, fixed), and the controls with edges in each window
// are listed, so a latch touches only what changed since the last one.
namespace {

    // the longest control name getPressedControl reports ("gamepad_right_bumper", "right_trigger")
    constexpr size_t CONTROL_NAME_SIZE = 32;
    // code points typed between two latches (more are dropped)
    constexpr size_t MAX_TYPED = 256;

    struct EdgeWindow {
        uint16_t presses = 0;
        uint16_t releases = 0;
        uint16_t uiPresses = 0;

        bool empty() const{
            return presses == 0 && releases == 0;
        }

        void add(const EdgeWindow& other){
            presses = (uint16_t)std::min<uint32_t>(0xFFFFu, (uint32_t)presses + other.presses);
            releases = (uint16_t)std::min<uint32_t>(0xFFFFu, (uint32_t)releases + other.releases);
            uiPresses = (uint16_t)std::min<uint32_t>(0xFFFFu, (uint32_t)uiPresses + other.uiPresses);
        }
    };

    void bump(uint16_t& count){
        if (count < 0xFFFF)
            count++;
    }

    struct ControlRecord {
        bool level = false;
        bool uiHeld = false;
        int pressMods = 0;
        bool inPending = false;
        bool inFrame = false;
        bool inFixed = false;
        EdgeWindow pending;
        EdgeWindow frame;
        EdgeWindow fixed;
    };

    template<size_t N>
    struct IndexList {
        std::array<uint16_t, N> items;
        size_t count = 0;

        void push(uint16_t index){
            if (count < N)
                items[count++] = index;
        }
    };

    struct PadSlot {
        int id = -1;
        bool connected = false;
        std::string name;
        float axes[Input::GAMEPAD_AXES] = {};
        bool axisUI[Input::GAMEPAD_AXES] = {};
    };

    struct TouchRecord {
        int pointer = 0;
        Vector2 position;
        Vector2 startPosition;
        Vector2 latchPosition;      // where it was at the last latch (the window's delta starts there)
        double beganAt = 0.0;       // the input clock when it touched the screen
        double movedAt = 0.0;       // ... when a latch last saw it move (deltaTime)
        int tapCount = 1;
        bool beganInPending = false;
        bool hidden = false;        // multiTouchEnabled off: not the first finger
        bool released = false;      // Input::releaseAll reported it Canceled; the platform removes it
    };

    // the last touch that ended (multi-tap)
    struct LastTap {
        bool valid = false;
        double endTime = 0.0;
        Vector2 start;
        int tapCount = 0;
    };

    struct TouchEdgeRecord {
        int pointer = 0;
        bool used = false;
        bool uiHeld = false;
        EdgeWindow pending;
        EdgeWindow frame;
        EdgeWindow fixed;
    };

    constexpr size_t TOUCH_EDGE_RECORDS = D_INPUT_MAX_TOUCHES * 2;
    // a finger can begin and end several times in one window
    constexpr size_t ENDED_TOUCHES = D_INPUT_MAX_TOUCHES * 2;

    float axisRest(int axis){
        return (axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER) ? -1.0f : 0.0f;
    }

    struct InputState {
        ControlRecord controls[Input::CONTROL_COUNT];
        IndexList<Input::CONTROL_COUNT> pendingList;
        IndexList<Input::CONTROL_COUNT> frameList;
        IndexList<Input::CONTROL_COUNT> fixedList;
        uint32_t pendingPressTotal = 0;
        uint32_t framePressTotal = 0;
        uint32_t fixedPressTotal = 0;
        bool fixedPhase = false;

        Vector2 mousePosition;
        Vector2 lastMovePosition;
        bool hasLastMove = false;
        Vector2 pendingDelta;
        Vector2 frameDelta;
        Vector2 mouseScroll;
        Vector2 pendingScroll;
        Vector2 frameScroll;
        Vector2 pendingGameScroll;
        Vector2 frameGameScroll;
        bool mouseEntered = false;
        int modifiers = 0;
        PointerLockState pointerLock = PointerLockState::UNLOCKED;

        PadSlot pads[D_INPUT_MAX_GAMEPADS];
        int padOrder[D_INPUT_MAX_GAMEPADS] = {};   // connected slots, in the order they connected
        size_t padCount = 0;

        TouchRecord touches[D_INPUT_MAX_TOUCHES];
        size_t touchCount = 0;
        Touch endedTouches[ENDED_TOUCHES];
        // when each began and last moved: their times are the window's latch's (deltaTime, time)
        double endedBeganAt[ENDED_TOUCHES] = {};
        double endedMovedAt[ENDED_TOUCHES] = {};
        size_t endedCount = 0;
        Touch frameTouches[D_INPUT_MAX_TOUCHES + ENDED_TOUCHES];
        size_t frameTouchCount = 0;
        TouchEdgeRecord touchEdges[TOUCH_EDGE_RECORDS];
        size_t touchEdgesUsed = 0;
        bool touchLimitWarned = false;
        bool padLimitWarned = false;
        bool multiTouchEnabled = true;
        LastTap lastTap;

        // the input frame and clock (beginFrame(dt))
        uint64_t inputFrame = 1;
        bool inFrame = false;
        double inputTime = 0.0;

        // text typed (pending, then the frame's) and the first control pressed in each window
        uint32_t typed[MAX_TYPED] = {};
        size_t typedCount = 0;
        std::string frameText;
        char pendingPressed[CONTROL_NAME_SIZE] = {};
        char framePressed[CONTROL_NAME_SIZE] = {};
    };

    InputState& state(){
        static InputState s;
        return s;
    }

    void markPending(InputState& s, int control){
        ControlRecord& record = s.controls[control];
        if (!record.inPending){
            record.inPending = true;
            s.pendingList.push((uint16_t)control);
        }
    }

    // the first control pressed in the pending window keeps its name (getPressedControl)
    void notePressed(const std::string& name){
        InputState& s = state();
        if (name.empty() || s.pendingPressed[0] != '\0')
            return;
        const size_t length = std::min(name.size(), CONTROL_NAME_SIZE - 1);
        std::copy(name.begin(), name.begin() + (std::ptrdiff_t)length, s.pendingPressed);
        s.pendingPressed[length] = '\0';
    }

    // a control's control name (keys, mouse buttons, gamepad buttons)
    std::string controlNameOf(int control){
        if (control < Input::MOUSE_CONTROL_BASE)
            return InputControlNames::name({InputDevice::Key, control});
        if (control < Input::GAMEPAD_CONTROL_BASE)
            return InputControlNames::name({InputDevice::Mouse, control - Input::MOUSE_CONTROL_BASE});
        return InputControlNames::name({InputDevice::GamepadButton, (control - Input::GAMEPAD_CONTROL_BASE) % Input::GAMEPAD_BUTTONS});
    }

    void pressControl(int control, int mods = 0){
        if (control < 0 || control >= Input::CONTROL_COUNT)
            return;
        InputState& s = state();
        ControlRecord& record = s.controls[control];
        if (record.level)
            return;
        record.level = true;
        record.uiHeld = false;
        record.pressMods = mods | Input::getHeldModifierKeys();
        bump(record.pending.presses);
        markPending(s, control);
        if (s.pendingPressTotal < 0xFFFFFFFFu)
            s.pendingPressTotal++;
        if (s.pendingPressed[0] == '\0')
            notePressed(controlNameOf(control));
    }

    // the direction an axis is pressed in for getPressedControl: past 0.5 (a trigger: half way)
    int axisDirection(int axis, float value){
        if (axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return ((value + 1.0f) * 0.5f >= 0.5f) ? 1 : 0;
        if (value >= 0.5f) return 1;
        if (value <= -0.5f) return -1;
        return 0;
    }

    void releaseControl(int control){
        if (control < 0 || control >= Input::CONTROL_COUNT)
            return;
        InputState& s = state();
        ControlRecord& record = s.controls[control];
        if (!record.level)
            return;
        record.level = false;
        record.uiHeld = false;
        bump(record.pending.releases);
        markPending(s, control);
    }

    int findPadSlot(int id){
        InputState& s = state();
        for (int i = 0; i < D_INPUT_MAX_GAMEPADS; i++){
            if (s.pads[i].id == id)
                return i;
        }
        return -1;
    }

    int findConnectedPadSlot(int id){
        const int slot = findPadSlot(id);
        return (slot >= 0 && state().pads[slot].connected) ? slot : -1;
    }

    // no edge of the slot's buttons waits in any window
    bool padSlotIdle(int slot){
        InputState& s = state();
        for (int b = 0; b < Input::GAMEPAD_BUTTONS; b++){
            const ControlRecord& record = s.controls[Input::gamepadControl(slot, b)];
            if (record.level || record.inPending || record.inFrame || record.inFixed)
                return false;
        }
        return true;
    }

    int findTouch(int pointer){
        InputState& s = state();
        for (size_t i = 0; i < s.touchCount; i++){
            if (s.touches[i].pointer == pointer)
                return (int)i;
        }
        return -1;
    }

    TouchEdgeRecord* findTouchEdges(int pointer){
        InputState& s = state();
        if (s.touchEdgesUsed == 0)
            return nullptr;
        for (TouchEdgeRecord& record : s.touchEdges){
            if (record.used && record.pointer == pointer)
                return &record;
        }
        return nullptr;
    }

    TouchEdgeRecord* touchEdgesFor(int pointer){
        if (TouchEdgeRecord* record = findTouchEdges(pointer))
            return record;
        InputState& s = state();
        for (TouchEdgeRecord& record : s.touchEdges){
            if (!record.used){
                record = TouchEdgeRecord();
                record.pointer = pointer;
                record.used = true;
                s.touchEdgesUsed++;
                return &record;
            }
        }
        return nullptr;
    }

    // the touch as the next latch reports it (now: the input clock)
    Touch touchOf(const TouchRecord& record, double now){
        Touch touch = {record.pointer, record.position};
        touch.fingerId = record.pointer;
        touch.startPosition = record.startPosition;
        touch.began = record.beganInPending;
        touch.tapCount = record.tapCount;
        if (record.beganInPending){
            touch.phase = TouchPhase::BEGAN;
            touch.delta = record.position - record.startPosition;
            touch.deltaTime = 0.0f;
        }else{
            touch.delta = record.position - record.latchPosition;
            const bool moved = touch.delta.x != 0.0f || touch.delta.y != 0.0f;
            touch.phase = moved ? TouchPhase::MOVED : TouchPhase::STATIONARY;
            touch.deltaTime = (float)(now - record.movedAt);
        }
        touch.deltaPosition = touch.delta;
        touch.time = (float)(now - record.beganAt);
        return touch;
    }

    // the finger's end: reported in the next window (unless releaseAll did it), its release edge
    void reportEnd(TouchRecord& record, TouchPhase phase){
        InputState& s = state();
        Touch ended = touchOf(record, s.inputTime);
        ended.phase = phase;
        if (!record.hidden && s.endedCount < ENDED_TOUCHES){
            s.endedBeganAt[s.endedCount] = record.beganAt;
            s.endedMovedAt[s.endedCount] = record.movedAt;
            s.endedTouches[s.endedCount++] = ended;
        }

        if (TouchEdgeRecord* edges = touchEdgesFor(record.pointer)){
            bump(edges->pending.releases);
            edges->uiHeld = false;
        }

        // multi-tap: a short lift is a tap the next touch may count on from
        if (!record.hidden){
            if (phase == TouchPhase::ENDED && s.inputTime - record.beganAt < Input::multiTapTime){
                s.lastTap.valid = true;
                s.lastTap.endTime = s.inputTime;
                s.lastTap.start = record.startPosition;
                s.lastTap.tapCount = record.tapCount;
            }else{
                s.lastTap.valid = false;
            }
        }
    }

    void endTouch(int pointer, TouchPhase phase){
        InputState& s = state();
        const int index = findTouch(pointer);
        if (index < 0)
            return;
        TouchRecord& record = s.touches[index];
        // releaseAll reported it already: the platform's cancel or lift only takes it away
        if (!record.released)
            reportEnd(record, phase);

        for (size_t i = (size_t)index + 1; i < s.touchCount; i++)
            s.touches[i - 1] = s.touches[i];
        s.touchCount--;
    }

    InputControlEdges edgesOf(bool level, bool uiHeld, const EdgeWindow& window, int pressMods = 0){
        InputControlEdges edges;
        edges.level = level;
        edges.uiHeld = uiHeld;
        edges.pressMods = pressMods;
        edges.presses = window.presses;
        edges.releases = window.releases;
        edges.uiPresses = window.uiPresses;
        return edges;
    }

}

// ---- events (Engine::system*)

void Input::addKeyPressed(int key, bool repeat, int mods){
    const int control = keyControl(key);
    if (control < 0)
        return;
    // a repeat is never a press; it holds a key whose press came before the focus did
    if (repeat){
        state().controls[control].level = true;
        return;
    }
    pressControl(control, mods);
}

void Input::releaseKeyPressed(int key){
    releaseControl(keyControl(key));
}

void Input::addMousePressed(int button, int mods){
    pressControl(mouseControl(button), mods);
}

void Input::releaseMousePressed(int button){
    releaseControl(mouseControl(button));
}

void Input::setMousePosition(float x, float y){
    state().mousePosition = Vector2(x, y);
}

void Input::moveMouse(float x, float y){
    InputState& s = state();
    const Vector2 position(x, y);
    if (s.hasLastMove)
        s.pendingDelta += position - s.lastMovePosition;
    s.lastMovePosition = position;
    s.hasLastMove = true;
    s.mousePosition = position;
}

void Input::warpMouse(float x, float y){
    InputState& s = state();
    s.mousePosition = Vector2(x, y);
    s.lastMovePosition = s.mousePosition;
    s.hasLastMove = true;
}

void Input::addMouseMovement(float x, float y){
    if (!std::isfinite(x) || !std::isfinite(y))
        return;
    InputState& s = state();
    const Vector2 position(x, y);
    if (s.hasLastMove)
        s.pendingDelta += position - s.lastMovePosition;
    s.lastMovePosition = position;
    s.hasLastMove = true;
}

void Input::addMouseDelta(float dx, float dy){
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return;
    state().pendingDelta += Vector2(dx, dy);
}

void Input::resetMouseMovement(){
    state().hasLastMove = false;
}

void Input::setMouseScroll(float xoffset, float yoffset){
    InputState& s = state();
    s.mouseScroll = Vector2(xoffset, yoffset);
    s.pendingScroll += Vector2(xoffset, yoffset);
}

void Input::addMouseScrollForGame(float xoffset, float yoffset){
    state().pendingGameScroll += Vector2(xoffset, yoffset);
}

void Input::addTouch(int pointer, float x, float y){
    InputState& s = state();
    if (findTouch(pointer) >= 0)
        return;
    if (s.touchCount >= D_INPUT_MAX_TOUCHES){
        if (!s.touchLimitWarned){
            s.touchLimitWarned = true;
            Log::warn("Input: more than %i fingers down, the others are ignored", D_INPUT_MAX_TOUCHES);
        }
        return;
    }
    TouchRecord& record = s.touches[s.touchCount++];
    record = TouchRecord();
    record.pointer = pointer;
    record.position = Vector2(x, y);
    record.startPosition = record.position;
    record.latchPosition = record.position;
    record.beganAt = s.inputTime;
    record.movedAt = s.inputTime;
    record.beganInPending = true;
    // a quick tap near the last one's start (that one short too) counts on from it
    if (s.lastTap.valid && s.inputTime - s.lastTap.endTime <= multiTapTime &&
            (record.position - s.lastTap.start).length() <= multiTapDistance)
        record.tapCount = s.lastTap.tapCount + 1;
    // multiTouchEnabled off: a finger while another visible one is down is not reported
    if (!s.multiTouchEnabled){
        for (size_t i = 0; i + 1 < s.touchCount; i++){
            if (!s.touches[i].hidden && !s.touches[i].released){
                record.hidden = true;
                break;
            }
        }
    }

    if (TouchEdgeRecord* edges = touchEdgesFor(pointer)){
        bump(edges->pending.presses);
        edges->uiHeld = false;
    }
}

void Input::setTouchPosition(int pointer, float x, float y){
    const int index = findTouch(pointer);
    if (index >= 0)
        state().touches[index].position = Vector2(x, y);
}

void Input::removeTouch(int pointer){
    endTouch(pointer, TouchPhase::ENDED);
}

void Input::cancelTouch(int pointer){
    endTouch(pointer, TouchPhase::CANCELED);
}

void Input::clearTouches(){
    InputState& s = state();
    while (s.touchCount > 0)
        endTouch(s.touches[s.touchCount - 1].pointer, TouchPhase::CANCELED);
}

void Input::addGamepad(int id, const std::string& name){
    InputState& s = state();
    int slot = findPadSlot(id);
    if (slot < 0){
        for (int i = 0; i < D_INPUT_MAX_GAMEPADS && slot < 0; i++){
            if (s.pads[i].id == -1)
                slot = i;
        }
        for (int i = 0; i < D_INPUT_MAX_GAMEPADS && slot < 0; i++){
            if (!s.pads[i].connected && padSlotIdle(i))
                slot = i;
        }
        if (slot < 0){
            if (!s.padLimitWarned){
                s.padLimitWarned = true;
                Log::warn("Input: more than %i gamepads connected, gamepad %i is ignored", D_INPUT_MAX_GAMEPADS, id);
            }
            return;
        }
    }

    PadSlot& pad = s.pads[slot];
    pad.name = name;
    if (!pad.connected || pad.id != id){
        pad.id = id;
        pad.connected = true;
        // Triggers rest at -1 in the [-1, 1] convention. Platforms seed their own
        // local state to -1 and skip the connect event, so seed the queryable
        // state here too or getGamepadAxis would report a resting trigger as 0.
        for (int axis = 0; axis < GAMEPAD_AXES; axis++){
            pad.axes[axis] = axisRest(axis);
            pad.axisUI[axis] = false;
        }
        s.padOrder[s.padCount++] = slot;
    }
}

void Input::removeGamepad(int id){
    InputState& s = state();
    const int slot = findConnectedPadSlot(id);
    if (slot < 0)
        return;
    // what it held comes up, so nothing stays pressed and the game sees the releases
    for (int b = 0; b < GAMEPAD_BUTTONS; b++)
        releaseControl(gamepadControl(slot, b));
    s.pads[slot].connected = false;
    for (size_t i = 0; i < s.padCount; i++){
        if (s.padOrder[i] == slot){
            for (size_t j = i + 1; j < s.padCount; j++)
                s.padOrder[j - 1] = s.padOrder[j];
            s.padCount--;
            break;
        }
    }
}

void Input::addGamepadButtonPressed(int id, int button){
    pressControl(gamepadControl(findConnectedPadSlot(id), button));
}

void Input::releaseGamepadButtonPressed(int id, int button){
    releaseControl(gamepadControl(findConnectedPadSlot(id), button));
}

void Input::setGamepadAxisValue(int id, int axis, float value){
    const int slot = findConnectedPadSlot(id);
    if (slot < 0 || axis < 0 || axis >= GAMEPAD_AXES)
        return;
    if (!std::isfinite(value))
        value = axisRest(axis);
    float& current = state().pads[slot].axes[axis];
    const int before = axisDirection(axis, current);
    const int after = axisDirection(axis, value);
    current = value;
    // getPressedControl: an axis pushed past half way is a press of "left_x+", a trigger's of "left_trigger"
    if (after != 0 && after != before){
        std::string name = InputControlNames::gamepadAxisName(axis);
        if (axis != D_GAMEPAD_AXIS_LEFT_TRIGGER && axis != D_GAMEPAD_AXIS_RIGHT_TRIGGER)
            name += after > 0 ? "+" : "-";
        notePressed(name);
    }
}

void Input::setGamepadAxisConsumedByUI(int id, int axis, bool consumed){
    const int slot = findConnectedPadSlot(id);
    if (slot >= 0 && axis >= 0 && axis < GAMEPAD_AXES)
        state().pads[slot].axisUI[axis] = consumed;
}

void Input::addMouseEntered(){
    InputState& s = state();
    s.mouseEntered = true;
    // the first move after entering comes from wherever the pointer was outside
    s.hasLastMove = false;
}

void Input::releaseMouseEntered(){
    state().mouseEntered = false;
}

void Input::setModifiers(int mods){
    state().modifiers = mods;
}

void Input::setPressConsumedByUI(int control){
    if (control < 0 || control >= CONTROL_COUNT)
        return;
    ControlRecord& record = state().controls[control];
    if (record.pending.uiPresses < record.pending.presses)
        bump(record.pending.uiPresses);
    record.uiHeld = record.level;
}

void Input::setTouchConsumedByUI(int pointer){
    if (TouchEdgeRecord* edges = findTouchEdges(pointer)){
        if (edges->pending.uiPresses < edges->pending.presses)
            bump(edges->pending.uiPresses);
        edges->uiHeld = findTouch(pointer) >= 0;
    }
}

void Input::setPointerLockState(PointerLockState lockState){
    state().pointerLock = lockState;
}

void Input::addInputCharacter(uint32_t codepoint){
    // Unity's inputString: backspace is "\b", return "\n"; other control characters are left out
    if (codepoint == '\r')
        codepoint = '\n';
    if (codepoint != '\b' && codepoint != '\n' && (codepoint < 32 || codepoint == 0x7f))
        return;
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        return;
    InputState& s = state();
    if (s.typedCount < MAX_TYPED)
        s.typed[s.typedCount++] = codepoint;
}

void Input::releaseAll(){
    InputState& s = state();
    for (int control = 0; control < CONTROL_COUNT; control++)
        releaseControl(control);
    // the fingers are reported Canceled; they stay down until the platform removes them
    for (size_t i = 0; i < s.touchCount; i++){
        TouchRecord& record = s.touches[i];
        if (record.released)
            continue;
        reportEnd(record, TouchPhase::CANCELED);
        record.released = true;
    }
    s.lastTap.valid = false;
    s.modifiers = 0;
    // movement while away is not the player's
    clearPendingDeltas();
}

void Input::reset(){
    resetFrameState();
    InputState& s = state();
    s.multiTouchEnabled = true;
    s.inputFrame = 1;
    s.inFrame = false;
    s.inputTime = 0.0;
}

// ---- windows

void Input::beginFrame(){
    InputState& s = state();

    for (size_t i = 0; i < s.frameList.count; i++){
        ControlRecord& record = s.controls[s.frameList.items[i]];
        record.frame = EdgeWindow();
        record.inFrame = false;
    }
    s.frameList.count = 0;

    for (size_t i = 0; i < s.pendingList.count; i++){
        const uint16_t control = s.pendingList.items[i];
        ControlRecord& record = s.controls[control];
        record.frame = record.pending;
        record.inFrame = true;
        s.frameList.push(control);
        record.fixed.add(record.pending);
        if (!record.inFixed){
            record.inFixed = true;
            s.fixedList.push(control);
        }
        record.pending = EdgeWindow();
        record.inPending = false;
    }
    s.pendingList.count = 0;

    s.framePressTotal = s.pendingPressTotal;
    s.fixedPressTotal += s.pendingPressTotal;
    s.pendingPressTotal = 0;

    s.frameDelta = s.pendingDelta;
    s.pendingDelta = Vector2(0.0f, 0.0f);
    s.frameScroll = s.pendingScroll;
    s.pendingScroll = Vector2(0.0f, 0.0f);
    s.frameGameScroll = s.pendingGameScroll;
    s.pendingGameScroll = Vector2(0.0f, 0.0f);

    // the text and the first control pressed of the window
    if (s.typedCount > 0 || !s.frameText.empty()){
        s.frameText.clear();
        for (size_t i = 0; i < s.typedCount; i++){
            const uint32_t c = s.typed[i];
            if (c < 0x80){
                s.frameText += (char)c;
            }else if (c < 0x800){
                s.frameText += (char)(0xC0 | (c >> 6));
                s.frameText += (char)(0x80 | (c & 0x3F));
            }else if (c < 0x10000){
                s.frameText += (char)(0xE0 | (c >> 12));
                s.frameText += (char)(0x80 | ((c >> 6) & 0x3F));
                s.frameText += (char)(0x80 | (c & 0x3F));
            }else{
                s.frameText += (char)(0xF0 | (c >> 18));
                s.frameText += (char)(0x80 | ((c >> 12) & 0x3F));
                s.frameText += (char)(0x80 | ((c >> 6) & 0x3F));
                s.frameText += (char)(0x80 | (c & 0x3F));
            }
        }
        s.typedCount = 0;
    }
    std::copy(std::begin(s.pendingPressed), std::end(s.pendingPressed), s.framePressed);
    s.pendingPressed[0] = '\0';

    // the frame's touches: the fingers down now, then the ones lifted in the window
    if (s.touchCount > 0 || s.endedCount > 0 || s.frameTouchCount > 0){
        const double now = s.inputTime;
        s.frameTouchCount = 0;
        for (size_t i = 0; i < s.touchCount; i++){
            TouchRecord& record = s.touches[i];
            if (record.released)
                continue;
            const Touch touch = touchOf(record, now);
            if (!record.hidden)
                s.frameTouches[s.frameTouchCount++] = touch;
            // deltaTime counts from the latch that saw it begin or move
            if (touch.phase == TouchPhase::MOVED || record.beganInPending)
                record.movedAt = now;
            record.latchPosition = record.position;
            record.beganInPending = false;
        }
        for (size_t i = 0; i < s.endedCount; i++){
            Touch ended = s.endedTouches[i];
            ended.time = (float)(now - s.endedBeganAt[i]);
            ended.deltaTime = ended.began ? 0.0f : (float)(now - s.endedMovedAt[i]);
            s.frameTouches[s.frameTouchCount++] = ended;
        }
        s.endedCount = 0;
    }

    if (s.touchEdgesUsed > 0){
        for (TouchEdgeRecord& record : s.touchEdges){
            if (!record.used)
                continue;
            record.frame = record.pending;
            record.fixed.add(record.pending);
            record.pending = EdgeWindow();
            if (record.frame.empty() && record.fixed.empty() && findTouch(record.pointer) < 0){
                record.used = false;
                s.touchEdgesUsed--;
            }
        }
    }
}

void Input::beginFrame(double unscaledDeltaTime){
    InputSuppression suppression;
    suppression.keyboard = Engine::isTextInputFocused();
    suppression.gamepadNavigation = Engine::wantsGamepadNavigation();
    suppression.keyboardNavigation = Engine::wantsKeyboardNavigation();
    beginFrame(unscaledDeltaTime, suppression);
}

void Input::beginFrame(double unscaledDeltaTime, const InputSuppression& suppression){
    if (!std::isfinite(unscaledDeltaTime) || unscaledDeltaTime < 0.0)
        unscaledDeltaTime = 0.0;
    InputState& s = state();
    // the clock first: the window's touches are timed at its latch
    s.inputTime += unscaledDeltaTime;
    if (s.inFrame)
        s.inputFrame++;
    s.inFrame = true;
    beginFrame();
    InputActions::setSuppression(suppression);
    InputActions::evaluate((float)unscaledDeltaTime);
    // the actions' edges, after every action is evaluated (a handler reading another action
    // sees this frame's values)
    for (size_t i = 0; i < InputActions::getFrameButtonDownCount(); i++)
        Engine::onButtonDown.call(InputActions::getFrameButtonDown(i));
    for (size_t i = 0; i < InputActions::getFrameButtonUpCount(); i++)
        Engine::onButtonUp.call(InputActions::getFrameButtonUp(i));
}

void Input::endFrame(){
    InputState& s = state();
    InputActions::endFrame();
    if (s.inFrame){
        s.inputFrame++;
        s.inFrame = false;
    }
}

bool Input::isInFrame(){
    return state().inFrame;
}

uint64_t Input::getInputFrame(){
    return state().inputFrame;
}

double Input::getInputTime(){
    return state().inputTime;
}

void Input::beginFixedStep(){
    state().fixedPhase = true;
}

void Input::endFixedStep(){
    InputState& s = state();
    s.fixedPhase = false;
    for (size_t i = 0; i < s.fixedList.count; i++){
        ControlRecord& record = s.controls[s.fixedList.items[i]];
        record.fixed = EdgeWindow();
        record.inFixed = false;
    }
    s.fixedList.count = 0;
    s.fixedPressTotal = 0;
    if (s.touchEdgesUsed > 0){
        for (TouchEdgeRecord& record : s.touchEdges)
            record.fixed = EdgeWindow();
    }
}

void Input::resetFrameState(){
    InputState& s = state();
    for (ControlRecord& record : s.controls)
        record = ControlRecord();
    s.pendingList.count = 0;
    s.frameList.count = 0;
    s.fixedList.count = 0;
    s.pendingPressTotal = 0;
    s.framePressTotal = 0;
    s.fixedPressTotal = 0;
    s.fixedPhase = false;

    s.hasLastMove = false;
    s.pendingDelta = s.frameDelta = Vector2(0.0f, 0.0f);
    s.mouseScroll = s.pendingScroll = s.frameScroll = Vector2(0.0f, 0.0f);
    s.pendingGameScroll = s.frameGameScroll = Vector2(0.0f, 0.0f);
    s.modifiers = 0;

    for (PadSlot& pad : s.pads){
        for (bool& ui : pad.axisUI)
            ui = false;
    }

    s.touchCount = 0;
    s.endedCount = 0;
    s.frameTouchCount = 0;
    for (TouchEdgeRecord& record : s.touchEdges)
        record = TouchEdgeRecord();
    s.touchEdgesUsed = 0;
    s.lastTap = LastTap();

    s.typedCount = 0;
    s.frameText.clear();
    s.pendingPressed[0] = '\0';
    s.framePressed[0] = '\0';
}

void Input::clearPendingDeltas(){
    InputState& s = state();
    s.hasLastMove = false;
    s.pendingDelta = Vector2(0.0f, 0.0f);
    s.pendingScroll = Vector2(0.0f, 0.0f);
    s.pendingGameScroll = Vector2(0.0f, 0.0f);
}

bool Input::isFixedPhase(){
    return state().fixedPhase;
}

// ---- levels

bool Input::getKey(int key){
    const int control = keyControl(key);
    return control >= 0 && state().controls[control].level;
}

bool Input::getMouseButton(int button){
    const int control = mouseControl(button);
    return control >= 0 && state().controls[control].level;
}

bool Input::isKeyPressed(int key){
    return getKey(key);
}

bool Input::isMousePressed(int button){
    return getMouseButton(button);
}

bool Input::isTouch(){
    return state().touchCount > 0;
}

bool Input::isMouseEntered(){
    return state().mouseEntered;
}

// ---- edges

InputControlEdges Input::getControlEdges(int control, bool fixedWindow){
    if (control < 0 || control >= CONTROL_COUNT)
        return InputControlEdges();
    const ControlRecord& record = state().controls[control];
    return edgesOf(record.level, record.uiHeld, fixedWindow ? record.fixed : record.frame, record.pressMods);
}

InputControlEdges Input::getControlEdges(int control){
    return getControlEdges(control, state().fixedPhase);
}

bool Input::getKeyDown(int key){
    return getControlEdges(keyControl(key)).presses > 0;
}

bool Input::getKeyUp(int key){
    return getControlEdges(keyControl(key)).releases > 0;
}

bool Input::getMouseButtonDown(int button){
    return getControlEdges(mouseControl(button)).presses > 0;
}

bool Input::getMouseButtonUp(int button){
    return getControlEdges(mouseControl(button)).releases > 0;
}

bool Input::getGamepadButtonDown(int id, int button){
    // id -1: any pad (one disconnected in the window too)
    if (id < 0){
        for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS; slot++){
            if (getGamepadSlotId(slot) >= 0 && getControlEdges(gamepadControl(slot, button)).presses > 0)
                return true;
        }
        return false;
    }
    return getControlEdges(gamepadControl(getGamepadSlot(id), button)).presses > 0;
}

bool Input::getGamepadButtonUp(int id, int button){
    if (id < 0){
        for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS; slot++){
            if (getGamepadSlotId(slot) >= 0 && getControlEdges(gamepadControl(slot, button)).releases > 0)
                return true;
        }
        return false;
    }
    return getControlEdges(gamepadControl(getGamepadSlot(id), button)).releases > 0;
}

bool Input::anyKeyDown(){
    const InputState& s = state();
    return (s.fixedPhase ? s.fixedPressTotal : s.framePressTotal) > 0;
}

bool Input::isAnyKeyPressed(){
    const InputState& s = state();
    for (const ControlRecord& record : s.controls){
        if (record.level)
            return true;
    }
    return false;
}

bool Input::isAnyKeyDown(){
    return anyKeyDown();
}

std::string Input::getPressedControl(){
    return state().framePressed;
}

std::string Input::getInputString(){
    return state().frameText;
}

Vector2 Input::getMousePosition(){
    return state().mousePosition;
}

Vector2 Input::getMouseDelta(){
    return state().frameDelta;
}

Vector2 Input::getMouseScrollDelta(){
    return state().frameScroll;
}

Vector2 Input::getMouseScroll(){
    return state().mouseScroll;
}

Vector2 Input::getMouseScrollDeltaForGame(){
    return state().frameGameScroll;
}

Vector2 Input::getTouchPosition(int pointer){
    const int index = findTouch(pointer);
    if (index >= 0)
        return state().touches[index].position;

    return Vector2(-1, -1);
}

std::vector<Touch> Input::getTouches(){
    const InputState& s = state();
    const double now = s.inputTime;
    std::vector<Touch> touches;
    touches.reserve(s.touchCount);
    for (size_t i = 0; i < s.touchCount; i++)
        touches.push_back(touchOf(s.touches[i], now));
    return touches;
}

size_t Input::numTouches(){
    return state().touchCount;
}

size_t Input::getTouchCount(){
    return state().frameTouchCount;
}

Touch Input::getTouch(size_t index){
    const InputState& s = state();
    if (index < s.frameTouchCount)
        return s.frameTouches[index];
    Touch none = {-1, Vector2(-1, -1)};
    none.fingerId = -1;
    return none;
}

std::vector<Touch> Input::getFrameTouches(){
    const InputState& s = state();
    return std::vector<Touch>(s.frameTouches, s.frameTouches + s.frameTouchCount);
}

bool Input::findTouchById(int fingerId, Touch& out){
    const InputState& s = state();
    // a finger lifted and down again in the window is listed twice: the one down now wins
    for (size_t i = 0; i < s.frameTouchCount; i++){
        if (s.frameTouches[i].fingerId == fingerId && s.frameTouches[i].phase != TouchPhase::ENDED &&
                s.frameTouches[i].phase != TouchPhase::CANCELED){
            out = s.frameTouches[i];
            return true;
        }
    }
    for (size_t i = 0; i < s.frameTouchCount; i++){
        if (s.frameTouches[i].fingerId == fingerId){
            out = s.frameTouches[i];
            return true;
        }
    }
    return false;
}

void Input::setMultiTouchEnabled(bool enabled){
    state().multiTouchEnabled = enabled;
}

bool Input::isMultiTouchEnabled(){
    return state().multiTouchEnabled;
}

bool Input::isTouchVisible(int fingerId){
    const int index = findTouch(fingerId);
    return index < 0 || !state().touches[index].hidden;
}

InputControlEdges Input::getTouchEdges(int pointer){
    const TouchEdgeRecord* record = findTouchEdges(pointer);
    if (!record)
        return InputControlEdges();
    // a finger releaseAll reported is not down any more for the game
    const int index = findTouch(pointer);
    const bool down = index >= 0 && !state().touches[index].released;
    return edgesOf(down, down && record->uiHeld, state().fixedPhase ? record->fixed : record->frame);
}

bool Input::getTouchDown(int pointer){
    return getTouchEdges(pointer).presses > 0;
}

bool Input::getTouchUp(int pointer){
    return getTouchEdges(pointer).releases > 0;
}

PointerLockState Input::getPointerLockState(){
    return state().pointerLock;
}

bool Input::isGamepadConnected(int id){
    return findConnectedPadSlot(id) >= 0;
}

std::string Input::getGamepadName(int id){
    const int slot = findConnectedPadSlot(id);
    if (slot >= 0)
        return state().pads[slot].name;

    return "";
}

bool Input::isGamepadButtonPressed(int id, int button){
    const int control = gamepadControl(findConnectedPadSlot(id), button);
    return control >= 0 && state().controls[control].level;
}

float Input::getGamepadAxis(int id, int axis){
    const int slot = findConnectedPadSlot(id);
    if (slot >= 0 && axis >= 0 && axis < GAMEPAD_AXES)
        return state().pads[slot].axes[axis];

    return 0.0f;
}

float Input::getGamepadAxisForGame(int id, int axis){
    return getGamepadSlotAxis(findConnectedPadSlot(id), axis, true);
}

std::vector<Gamepad> Input::getGamepads(){
    const InputState& s = state();
    std::vector<Gamepad> gamepads;
    for (size_t i = 0; i < s.padCount; i++){
        const int slot = s.padOrder[i];
        const PadSlot& pad = s.pads[slot];
        Gamepad gamepad;
        gamepad.id = pad.id;
        gamepad.name = pad.name;
        for (int b = 0; b < GAMEPAD_BUTTONS; b++)
            gamepad.buttonPressed[b] = s.controls[gamepadControl(slot, b)].level;
        for (int axis = 0; axis < GAMEPAD_AXES; axis++)
            gamepad.axisValue[axis] = pad.axes[axis];
        gamepads.push_back(gamepad);
    }
    return gamepads;
}

size_t Input::numGamepads(){
    return state().padCount;
}

int Input::getGamepadId(size_t index){
    const InputState& s = state();
    if (index < s.padCount)
        return s.pads[s.padOrder[index]].id;

    return -1;
}

int Input::getModifiers(){
    return state().modifiers;
}

DeviceOrientation Input::getDeviceOrientation(){
    return System::hasInstance() ? System::instance().getDeviceOrientation() : DeviceOrientation::Unknown;
}

int Input::getHeldModifierKeys(){
    const ControlRecord* controls = state().controls;
    int mods = 0;
    if (controls[D_KEY_LEFT_SHIFT].level || controls[D_KEY_RIGHT_SHIFT].level) mods |= D_MODIFIER_SHIFT;
    if (controls[D_KEY_LEFT_CONTROL].level || controls[D_KEY_RIGHT_CONTROL].level) mods |= D_MODIFIER_CONTROL;
    if (controls[D_KEY_LEFT_ALT].level || controls[D_KEY_RIGHT_ALT].level) mods |= D_MODIFIER_ALT;
    if (controls[D_KEY_LEFT_SUPER].level || controls[D_KEY_RIGHT_SUPER].level) mods |= D_MODIFIER_SUPER;
    return mods;
}

size_t Input::findTouchIndex(int pointer){
    const int index = findTouch(pointer);
    return index >= 0 ? (size_t)index : (size_t)-1;
}

size_t Input::findGamepadIndex(int id){
    const InputState& s = state();
    const int slot = findConnectedPadSlot(id);
    for (size_t i = 0; slot >= 0 && i < s.padCount; i++){
        if (s.padOrder[i] == slot)
            return i;
    }

    return (size_t)-1;
}

// ---- controls by index

int Input::keyControl(int key){
    return (key >= 0 && key <= D_KEY_LAST) ? key : -1;
}

int Input::mouseControl(int button){
    return (button >= 0 && button <= D_MOUSE_BUTTON_LAST) ? MOUSE_CONTROL_BASE + button : -1;
}

int Input::getGamepadSlot(int id){
    return findPadSlot(id);
}

int Input::getGamepadSlotId(int slot){
    if (slot < 0 || slot >= D_INPUT_MAX_GAMEPADS)
        return -1;
    return state().pads[slot].id;
}

bool Input::isGamepadSlotConnected(int slot){
    return slot >= 0 && slot < D_INPUT_MAX_GAMEPADS && state().pads[slot].connected;
}

int Input::gamepadControl(int slot, int button){
    if (slot < 0 || slot >= D_INPUT_MAX_GAMEPADS || button < 0 || button >= GAMEPAD_BUTTONS)
        return -1;
    return GAMEPAD_CONTROL_BASE + slot * GAMEPAD_BUTTONS + button;
}

float Input::getGamepadSlotAxis(int slot, int axis, bool forGame){
    if (axis < 0 || axis >= GAMEPAD_AXES)
        return 0.0f;
    // a pad gone (its slot keeps its last edges) is at rest: a trigger at -1, not 0
    if (!isGamepadSlotConnected(slot))
        return axisRest(axis);
    const PadSlot& pad = state().pads[slot];
    if (forGame && pad.axisUI[axis])
        return axisRest(axis);
    return pad.axes[axis];
}
