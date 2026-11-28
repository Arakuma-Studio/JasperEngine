// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "input/InputScheme.h"

#include "Input.h"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cmath>

using namespace jasperengine;

FunctionSubscribe<void(const std::string&, const std::string&)> InputSchemeTracker::onSchemeChanged;

namespace {

    // R4: mouse motion counts once it adds up to this many canvas px within the window
    constexpr float kMouseMotionPx = 12.0f;
    constexpr double kMouseMotionWindow = 0.25;
    // R5: the UI navigation's thresholds (Engine.cpp, gamepadAxisToDocuments)
    constexpr float kStickPress = 0.5f;
    constexpr float kTriggerPress = 0.0f;    // triggers rest at -1: past half way
    // R6(c): a finger from the device that sent a key this recently is an emulator's mouse
    constexpr double kKeyDeviceWindow = 10.0;
    // R6(d): a compatibility mouse event of a touch
    constexpr double kTouchEchoTime = 0.3;
    constexpr float kTouchEchoPx = 24.0f;

    // what onSchemeChanged passes: made once, no allocation per change
    const std::string kSchemeNames[3] = {"Keyboard&Mouse", "Gamepad", "Touch"};

    struct PadFamily {
        int id = -1;
        GamepadFamily family = GamepadFamily::Generic;
    };

    struct SchemeState {
        // the evidence of the pending window
        InputScheme tracked = InputScheme::KeyboardMouse;
        InputScheme trackedPointer = InputScheme::KeyboardMouse;
        int trackedGamepad = -1;
        // as of the last latch
        InputScheme current = InputScheme::KeyboardMouse;
        InputScheme pointer = InputScheme::KeyboardMouse;
        int lastGamepad = -1;
        GamepadFamily family = GamepadFamily::Generic;
        bool overridden = false;
        InputScheme overrideScheme = InputScheme::KeyboardMouse;
        uint32_t changes = 0;
        uint32_t revision = 0;

        // the event being sent
        InputSourceTag sourceTag = InputSourceTag::None;
        int sourceDevice = -1;

        // R6(a): non-modifier keys held
        std::bitset<D_KEY_LAST + 1> held;
        // R6(c)
        int keyDevice = -1;
        double keyDeviceTime = -1e9;
        // R4: the mouse's accumulated motion
        bool mouseKnown = false;
        float mouseX = 0.0f, mouseY = 0.0f;
        float motion = 0.0f;
        double motionStart = 0.0;
        // R6(d)
        bool touchEnded = false;
        float touchEndX = 0.0f, touchEndY = 0.0f;
        double touchEndTime = 0.0;

        PadFamily pads[D_INPUT_MAX_GAMEPADS];
        int padCount = 0;
    };

    SchemeState& schemeState(){
        static SchemeState s;
        return s;
    }

    bool isSchemeModifierKey(int key){
        switch (key){
            case D_KEY_LEFT_SHIFT: case D_KEY_RIGHT_SHIFT:
            case D_KEY_LEFT_CONTROL: case D_KEY_RIGHT_CONTROL:
            case D_KEY_LEFT_ALT: case D_KEY_RIGHT_ALT:
            case D_KEY_LEFT_SUPER: case D_KEY_RIGHT_SUPER:
            case D_KEY_CAPS_LOCK:
                return true;
            default:
                return false;
        }
    }

    void keyboardMouse(SchemeState& s){
        s.tracked = InputScheme::KeyboardMouse;
        s.trackedPointer = InputScheme::KeyboardMouse;
    }

    // a mouse event right after a touch ended there: a platform's compatibility event
    bool touchEcho(const SchemeState& s, float x, float y, double now){
        return s.touchEnded && now - s.touchEndTime <= kTouchEchoTime && std::fabs(x - s.touchEndX) <= kTouchEchoPx &&
               std::fabs(y - s.touchEndY) <= kTouchEchoPx;
    }

    const PadFamily* findPad(const SchemeState& s, int id){
        for (int i = 0; i < s.padCount; i++){
            if (s.pads[i].id == id)
                return &s.pads[i];
        }
        return nullptr;
    }

    GamepadFamily familyNow(const SchemeState& s){
        if (const PadFamily* pad = findPad(s, s.lastGamepad))
            return pad->family;
        return s.padCount > 0 ? s.pads[0].family : GamepadFamily::Generic;
    }

    bool nameHas(const std::string& text, const char* word){
        return text.find(word) != std::string::npos;
    }

    int vendorHexDigit(char c){
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    // four hex digits at `at`, -1 when not
    int vendorHex4(const std::string& text, size_t at){
        if (at + 4 > text.size())
            return -1;
        int value = 0;
        for (size_t i = at; i < at + 4; i++){
            const int digit = vendorHexDigit(text[i]);
            if (digit < 0)
                return -1;
            value = value * 16 + digit;
        }
        return value;
    }

    GamepadFamily familyOfVendor(int vendor){
        switch (vendor){
            case 0x045e: return GamepadFamily::Xbox;
            case 0x054c: return GamepadFamily::PlayStation;
            case 0x057e: return GamepadFamily::Nintendo;
            default: return GamepadFamily::Generic;
        }
    }

}

void InputSchemeTracker::setSource(InputSourceTag tag, int deviceId){
    SchemeState& s = schemeState();
    s.sourceTag = tag;
    s.sourceDevice = deviceId;
}

InputSourceTag InputSchemeTracker::getSourceTag(){
    return schemeState().sourceTag;
}

int InputSchemeTracker::getSourceDevice(){
    return schemeState().sourceDevice;
}

double InputSchemeTracker::clock(){
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

// ---- evidence

void InputSchemeTracker::noteKey(int key, bool repeat, bool typing, double now){
    SchemeState& s = schemeState();
    if (s.sourceTag == InputSourceTag::Synthetic || key < 0 || key > D_KEY_LAST || isSchemeModifierKey(key))
        return;
    s.held.set((size_t)key);
    if (s.sourceDevice >= 0){
        s.keyDevice = s.sourceDevice;
        s.keyDeviceTime = now;
    }
    // typing is scheme-neutral: a phone's soft keyboard never switches
    if (!repeat && !typing)
        keyboardMouse(s);
}

void InputSchemeTracker::noteKeyUp(int key){
    if (key >= 0 && key <= D_KEY_LAST)
        schemeState().held.reset((size_t)key);
}

void InputSchemeTracker::noteMouseButton(float x, float y, double now){
    SchemeState& s = schemeState();
    if (!touchEcho(s, x, y, now))
        keyboardMouse(s);
}

void InputSchemeTracker::noteMouseScroll(){
    keyboardMouse(schemeState());
}

void InputSchemeTracker::noteMouseMove(float x, float y, double now){
    SchemeState& s = schemeState();
    const float dx = x - s.mouseX, dy = y - s.mouseY;
    const bool known = s.mouseKnown;
    s.mouseKnown = true;
    s.mouseX = x;
    s.mouseY = y;
    if (!known || touchEcho(s, x, y, now))
        return;
    if (now - s.motionStart > kMouseMotionWindow){
        s.motion = 0.0f;
        s.motionStart = now;
    }
    s.motion += std::sqrt(dx * dx + dy * dy);
    if (s.motion >= kMouseMotionPx){
        s.motion = 0.0f;
        s.motionStart = now;
        keyboardMouse(s);
    }
}

void InputSchemeTracker::noteTouchStart(float x, float y, double now){
    (void)x;
    (void)y;
    SchemeState& s = schemeState();
    // R6(b, c): a mouse the platform delivers as a finger
    const bool emulatorMouse = s.sourceDevice >= 0 && s.sourceDevice == s.keyDevice && now - s.keyDeviceTime <= kKeyDeviceWindow;
    if (s.sourceTag == InputSourceTag::Mouse || emulatorMouse){
        keyboardMouse(s);
        return;
    }
    s.trackedPointer = InputScheme::Touch;
    // R6(a): looking with a finger while walking with a key keeps the keyboard. A key whose up
    // never came (focus loss, Input::releaseAll) is not held any more
    if (s.tracked == InputScheme::KeyboardMouse && s.held.any()){
        bool held = false;
        for (size_t key = 0; key < s.held.size(); key++){
            if (!s.held.test(key))
                continue;
            if (Input::isKeyPressed((int)key))
                held = true;
            else
                s.held.reset(key);
        }
        if (held)
            return;
    }
    s.tracked = InputScheme::Touch;
}

void InputSchemeTracker::noteTouchEnd(float x, float y, double now){
    SchemeState& s = schemeState();
    if (s.sourceTag == InputSourceTag::Mouse)
        return;
    s.touchEnded = true;
    s.touchEndX = x;
    s.touchEndY = y;
    s.touchEndTime = now;
}

void InputSchemeTracker::noteGamepadButton(int gamepad){
    SchemeState& s = schemeState();
    s.tracked = InputScheme::Gamepad;
    s.trackedGamepad = gamepad;
}

void InputSchemeTracker::noteGamepadAxis(int gamepad, int axis, float previous, float value){
    const bool trigger = axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER;
    const bool crossed = trigger ? (previous <= kTriggerPress && value > kTriggerPress)
                                 : (std::fabs(previous) < kStickPress && std::fabs(value) >= kStickPress);
    if (crossed)
        noteGamepadButton(gamepad);
}

void InputSchemeTracker::noteGamepadConnected(int gamepad, const std::string& name){
    SchemeState& s = schemeState();
    const GamepadFamily family = familyOf(name);
    for (int i = 0; i < s.padCount; i++){
        if (s.pads[i].id == gamepad){
            s.pads[i].family = family;
            return;
        }
    }
    if (s.padCount < D_INPUT_MAX_GAMEPADS)
        s.pads[s.padCount++] = PadFamily{gamepad, family};
}

void InputSchemeTracker::noteGamepadDisconnected(int gamepad){
    SchemeState& s = schemeState();
    for (int i = 0; i < s.padCount; i++){
        if (s.pads[i].id == gamepad){
            std::copy(s.pads + i + 1, s.pads + s.padCount, s.pads + i);
            s.padCount--;
            return;
        }
    }
}

// ---- the frame

void InputSchemeTracker::latch(){
    SchemeState& s = schemeState();
    const InputScheme previous = s.current;
    const InputScheme previousPointer = s.pointer;
    const GamepadFamily previousFamily = s.family;

    s.current = s.overridden ? s.overrideScheme : s.tracked;
    s.pointer = s.trackedPointer;
    if (s.overridden && s.overrideScheme != InputScheme::Gamepad)
        s.pointer = s.overrideScheme;
    if (s.trackedGamepad >= 0)
        s.lastGamepad = s.trackedGamepad;
    s.family = familyNow(s);

    if (s.current != previous || s.pointer != previousPointer || s.family != previousFamily)
        s.revision++;
    if (s.current != previous){
        s.changes++;
        if (onSchemeChanged.hasSubscribers())
            onSchemeChanged.call(kSchemeNames[(int)s.current], kSchemeNames[(int)previous]);
    }
}

void InputSchemeTracker::reset(bool touchDevice){
    SchemeState& s = schemeState();
    const InputScheme initial = touchDevice ? InputScheme::Touch : InputScheme::KeyboardMouse;
    s.tracked = initial;
    s.trackedPointer = initial;
    s.trackedGamepad = -1;
    s.current = s.overridden ? s.overrideScheme : initial;
    s.pointer = (s.overridden && s.overrideScheme != InputScheme::Gamepad) ? s.overrideScheme : initial;
    s.lastGamepad = -1;
    s.family = familyNow(s);
    s.changes = 0;
    s.revision++;
    s.held.reset();
    s.keyDevice = -1;
    s.keyDeviceTime = -1e9;
    s.mouseKnown = false;
    s.motion = 0.0f;
    s.touchEnded = false;
    s.sourceTag = InputSourceTag::None;
    s.sourceDevice = -1;
}

// ---- state

InputScheme InputSchemeTracker::getCurrent(){
    return schemeState().current;
}

InputScheme InputSchemeTracker::getPointer(){
    return schemeState().pointer;
}

int InputSchemeTracker::getLastGamepad(){
    return schemeState().lastGamepad;
}

GamepadFamily InputSchemeTracker::getGamepadFamily(){
    return schemeState().family;
}

GamepadFamily InputSchemeTracker::familyOf(const std::string& padName){
    std::string lower = padName;
    for (char& c : lower)
        c = (char)std::tolower(static_cast<unsigned char>(c));
    // a browser's id: Chrome's "(... Vendor: 054c Product: 0ce6)", Firefox's "054c-0ce6-..."
    const size_t vendorAt = lower.find("vendor: ");
    int vendor = vendorAt != std::string::npos ? vendorHex4(lower, vendorAt + 8) : -1;
    if (vendor < 0 && lower.size() > 10 && lower[4] == '-' && lower[9] == '-' && vendorHex4(lower, 5) >= 0)
        vendor = vendorHex4(lower, 0);
    if (vendor >= 0 && familyOfVendor(vendor) != GamepadFamily::Generic)
        return familyOfVendor(vendor);
    if (nameHas(lower, "xbox") || nameHas(lower, "xinput") || nameHas(lower, "microsoft"))
        return GamepadFamily::Xbox;
    if (nameHas(lower, "playstation") || nameHas(lower, "dualshock") || nameHas(lower, "dualsense") || nameHas(lower, "ps4") ||
        nameHas(lower, "ps5") || nameHas(lower, "sony"))
        return GamepadFamily::PlayStation;
    if (nameHas(lower, "nintendo") || nameHas(lower, "switch") || nameHas(lower, "pro controller") || nameHas(lower, "joy-con"))
        return GamepadFamily::Nintendo;
    return GamepadFamily::Generic;
}

GamepadFamily InputSchemeTracker::familyOfPad(int gamepad){
    const PadFamily* pad = findPad(schemeState(), gamepad);
    return pad ? pad->family : GamepadFamily::Generic;
}

void InputSchemeTracker::setOverride(std::optional<InputScheme> scheme){
    SchemeState& s = schemeState();
    s.overridden = scheme.has_value();
    s.overrideScheme = scheme.value_or(InputScheme::KeyboardMouse);
}

std::optional<InputScheme> InputSchemeTracker::getOverride(){
    const SchemeState& s = schemeState();
    if (!s.overridden)
        return std::nullopt;
    return s.overrideScheme;
}

uint32_t InputSchemeTracker::getChangeCount(){
    return schemeState().changes;
}

uint32_t InputSchemeTracker::getRevision(){
    return schemeState().revision;
}

const char* InputSchemeTracker::groupName(InputScheme scheme){
    switch (scheme){
        case InputScheme::Gamepad: return "Gamepad";
        case InputScheme::Touch: return "Touch";
        case InputScheme::KeyboardMouse: break;
    }
    return "Keyboard&Mouse";
}

const char* InputSchemeTracker::htmlName(InputScheme scheme){
    switch (scheme){
        case InputScheme::Gamepad: return "gamepad";
        case InputScheme::Touch: return "touch";
        case InputScheme::KeyboardMouse: break;
    }
    return "keyboard-mouse";
}

const char* InputSchemeTracker::familyName(GamepadFamily family){
    switch (family){
        case GamepadFamily::Xbox: return "Xbox";
        case GamepadFamily::PlayStation: return "PlayStation";
        case GamepadFamily::Nintendo: return "Nintendo";
        case GamepadFamily::Generic: break;
    }
    return "Generic";
}

const char* InputSchemeTracker::familyKey(GamepadFamily family){
    switch (family){
        case GamepadFamily::Xbox: return "xbox";
        case GamepadFamily::PlayStation: return "playstation";
        case GamepadFamily::Nintendo: return "nintendo";
        case GamepadFamily::Generic: break;
    }
    return "generic";
}

bool InputSchemeTracker::parseScheme(const std::string& text, InputScheme& out){
    std::string lower = text;
    for (char& c : lower)
        c = (char)std::tolower(static_cast<unsigned char>(c));
    if (lower == "keyboard&mouse" || lower == "keyboard-mouse" || lower == "keyboard_mouse"){
        out = InputScheme::KeyboardMouse;
        return true;
    }
    if (lower == "gamepad"){
        out = InputScheme::Gamepad;
        return true;
    }
    if (lower == "touch"){
        out = InputScheme::Touch;
        return true;
    }
    return false;
}
