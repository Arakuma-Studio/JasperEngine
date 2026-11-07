// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef ON_SCREEN_CONTROL_COMPONENTS_H
#define ON_SCREEN_CONTROL_COMPONENTS_H

#include "Entity.h"
#include "math/Vector2.h"
#include "util/FunctionSubscribe.h"

#include <climits>
#include <string>
#include <vector>

namespace jasperengine{

    // On-screen controls of the component UI (ui-multitouch spec 7.4): input sources, not
    // scripts. They write into the Input actions (Project Settings > Input) through
    // InputActions::setSourceValue / addSourceDelta while a pointer holds them, and clear what
    // they wrote on release, disable, destroy and scene unload, so Input.getAxis("Horizontal")
    // sums the keyboard, a gamepad and the stick as in Unity. Each needs an Image on its own
    // GameObject (or a child) to be hit; the first pointer that presses one owns it.

    // Joystick Pack's Fixed / Floating / Dynamic (Unity's Variable)
    enum class OnScreenStickMode { FIXED, FLOATING, DYNAMIC };
    enum class OnScreenStickAxes { BOTH, HORIZONTAL, VERTICAL };
    enum class OnScreenVisibility { ALWAYS, TOUCH_DEVICES_ONLY };

    struct JASPERENGINE_API OnScreenStickComponent{
        bool enabled = true;
        OnScreenStickMode mode = OnScreenStickMode::FIXED;
        // child Images: the base circle (its width is the stick's diameter) and the knob. The
        // area a press may start in is this GameObject's rect
        Entity background = NULL_ENTITY;
        Entity handle = NULL_ENTITY;
        float handleRange = 1.0f;       // how far the knob travels, in radii
        float deadZone = 0.0f;          // |input| at or below reads 0 (radii)
        float moveThreshold = 1.0f;     // Dynamic: the base follows past this (radii)
        OnScreenStickAxes axes = OnScreenStickAxes::BOTH;
        bool snapX = false;             // -1, 0 or 1 (Joystick Pack's 22.5 / 67.5 degree sectors)
        bool snapY = false;
        bool hideWhenReleased = true;   // Floating / Dynamic: the base shows only while held
        bool clampToArea = true;        // Floating / Dynamic: the base stays inside the area
        std::string horizontalAction = "Horizontal";
        std::string verticalAction = "Vertical";
        OnScreenVisibility visibility = OnScreenVisibility::ALWAYS;

        // runtime, never serialized
        Vector2 value = Vector2(0, 0);  // -1..1, y up
        bool pressed = false;
        int pointerId = INT_MIN;        // the pointer holding it (PointerEventData ids), INT_MIN: none
        Vector2 backgroundRest = Vector2(0, 0);  // the base's own position (restored on release)
        Vector2 handleRest = Vector2(0, 0);      // the knob's own position (the base's centre)
        bool restKnown = false;
        Vector2 center = Vector2(0, 0); // the base's centre while held (canvas px)

        FunctionSubscribe<void(float, float)> onValueChanged;   // x, y
        FunctionSubscribe<void()> onPress;
        FunctionSubscribe<void()> onRelease;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onValueChanged); f(onPress); f(onRelease); }
    };

    struct JASPERENGINE_API OnScreenButtonComponent{
        bool enabled = true;
        std::string action = "Fire1";
        float value = 1.0f;                     // what it writes while pressed (e.g. -1 on "Horizontal")
        bool pressWhenPointerEnters = false;    // a finger already down sliding onto it presses it
        bool releaseWhenPointerLeaves = false;  // a finger sliding off it releases it
        OnScreenVisibility visibility = OnScreenVisibility::ALWAYS;

        // runtime, never serialized
        bool pressed = false;
        std::vector<int> pointers;              // the pointers pressing it (several fingers can)

        FunctionSubscribe<void()> onPress;
        FunctionSubscribe<void()> onRelease;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onPress); f(onRelease); }
    };

    struct JASPERENGINE_API TouchAreaComponent{
        bool enabled = true;
        std::string horizontalAction = "Mouse X";
        std::string verticalAction = "Mouse Y";
        // canvas px to action units: the default Mouse X/Y sensitivity, so touch and mouse feel alike
        float sensitivity = 0.1f;
        bool invertX = false;
        bool invertY = false;
        OnScreenVisibility visibility = OnScreenVisibility::ALWAYS;

        // runtime, never serialized
        Vector2 delta = Vector2(0, 0);          // this frame's drag, canvas px, y down
        bool pressed = false;
        int pointerId = INT_MIN;

        FunctionSubscribe<void(float, float)> onDrag;   // dx, dy (canvas px), once a frame while it moves

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onDrag); }
    };

}

#endif //ON_SCREEN_CONTROL_COMPONENTS_H
