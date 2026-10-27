// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputRebind.h"

#include "input/InputControlPath.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // an action's view of an axis: triggers 0..1, the others -1..1
    float actionAxis(int slot, int axis){
        const float value = Input::getGamepadSlotAxis(slot, axis, false);
        if (axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return (value + 1.0f) * 0.5f;
        return value;
    }

    bool isStickAxis(int axis){
        return axis == D_GAMEPAD_AXIS_LEFT_X || axis == D_GAMEPAD_AXIS_LEFT_Y ||
               axis == D_GAMEPAD_AXIS_RIGHT_X || axis == D_GAMEPAD_AXIS_RIGHT_Y;
    }

}

void InputRebind::begin(InputRebindState& state){
    state.elapsed = 0.0f;
    for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS; slot++){
        for (int axis = 0; axis < Input::GAMEPAD_AXES; axis++)
            state.axisBaseline[slot][axis] = actionAxis(slot, axis);
    }
}

bool InputRebind::accepts(const InputRebindOptions& options, const std::string& path){
    const InputControlPath control = InputControlPath::parse(path);
    if (!control.isValid())
        return false;
    if (!options.groups.empty() &&
        std::find(options.groups.begin(), options.groups.end(), std::string(control.deviceGroup())) == options.groups.end())
        return false;
    for (const std::string& pattern : options.exclude){
        if (inputPathMatches(pattern, path))
            return false;
    }
    return true;
}

bool InputRebind::scan(InputRebindState& state, float dt, InputRebindResult& result){
    result = InputRebindResult();
    result.action = state.action;
    result.bindingId = state.bindingId;

    // the candidates of the window in a fixed order: keys, mouse buttons, gamepad buttons, axes
    std::string found;
    auto consider = [&](const std::string& path){
        if (!found.empty())
            return;
        if (!state.options.cancel.empty() && InputControlPath::parse(state.options.cancel) == InputControlPath::parse(path)){
            found = path;
            return;
        }
        if (accepts(state.options, path))
            found = path;
    };

    for (int key = 0; key <= D_KEY_LAST && found.empty(); key++){
        if (Input::getControlEdges(Input::keyControl(key), false).presses > 0)
            consider(InputControlPath{InputControlKind::KEY, key}.toString());
    }
    for (int button = 0; button <= D_MOUSE_BUTTON_LAST && found.empty(); button++){
        if (Input::getControlEdges(Input::mouseControl(button), false).presses > 0)
            consider(InputControlPath{InputControlKind::MOUSE_BUTTON, button}.toString());
    }
    for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS && found.empty(); slot++){
        for (int button = 0; button < Input::GAMEPAD_BUTTONS && found.empty(); button++){
            if (Input::getControlEdges(Input::gamepadControl(slot, button), false).presses > 0)
                consider(InputControlPath{InputControlKind::GAMEPAD_BUTTON, button}.toString());
        }
    }
    for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS; slot++){
        for (int axis = 0; axis < Input::GAMEPAD_AXES; axis++){
            const float value = Input::isGamepadSlotConnected(slot) ? actionAxis(slot, axis) : 0.0f;
            const bool crossed = std::fabs(value) > 0.5f && std::fabs(state.axisBaseline[slot][axis]) <= 0.5f;
            state.axisBaseline[slot][axis] = value;
            if (!crossed || !found.empty())
                continue;
            InputControlPath control;
            if (state.wantsStick && isStickAxis(axis)){
                control.kind = InputControlKind::GAMEPAD_STICK;
                control.index = (axis == D_GAMEPAD_AXIS_LEFT_X || axis == D_GAMEPAD_AXIS_LEFT_Y) ? 0 : 1;
            }else{
                control.kind = InputControlKind::GAMEPAD_AXIS;
                control.index = axis;
            }
            consider(control.toString());
        }
    }

    if (!found.empty()){
        if (!state.options.cancel.empty() && InputControlPath::parse(state.options.cancel) == InputControlPath::parse(found)){
            result.status = InputRebindStatus::CANCELED;
        }else{
            result.status = InputRebindStatus::COMPLETED;
            result.path = found;
        }
        return true;
    }

    state.elapsed += dt;
    if (state.options.timeout > 0.0f && state.elapsed >= state.options.timeout){
        result.status = InputRebindStatus::TIMED_OUT;
        return true;
    }
    return false;
}
