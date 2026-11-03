// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTACTIONASSETJSON_H
#define INPUTACTIONASSETJSON_H

// The JSON form of an .inputactions asset and its edit operations (docs/specs/S22-gameplay.md R8,
// R44), for the engine, the editor and the tests. Kept apart from input/InputActionAsset.h so the
// scripts' engine API (JasperEngine.h) does not take the JSON library.

#include "input/InputActionAsset.h"
#include "json.hpp"

namespace jasperengine {

    using InputJson = nlohmann::ordered_json;

    // false + error for JSON that is not an asset; a binding without an id gets one
    JASPERENGINE_API bool inputActionAssetFromJson(const InputJson& json, InputActionAssetDesc& out, std::string& error);
    JASPERENGINE_API InputJson inputActionAssetToJson(const InputActionAssetDesc& asset);

    // ---- edit operations (the editor's commands and edit_input_actions)
    //
    // An op is JSON: {op, ...}; apply throws std::invalid_argument with the reason when it
    // cannot apply (nothing is changed then). Addresses: map "Map", action "Map/Action",
    // binding by id.
    //   add_map {name, priority?, enabled?, consume?, exclusive?, ignore_when_ui_consumes?, index?}
    //   add_input_manager {index?}   (the "Input Manager" map with Unity's eleven default axes and buttons)
    //   add_action {map, name, type?, press_point?, hold_time?, deadzone?, index?}
    //   add_binding {action, path, scale?, invert?, deadzone?, required_mods?, forbidden_mods?, groups?, index?}
    //   add_composite {action, composite: axis1d|dpad2d, parts: {part: path | {path, ...} | [...]}, groups?,
    //                  normalize?, gamepad?, smoothing?, index?}
    //   add_part {binding (a composite's id), part, path | {path, ...}, index?}  (another control of a role)
    //   set {map | action | binding, values: {key: value}}   (the asset's JSON keys; a binding's
    //        gravity, sensitivity, dead and snap set its smoothing, smoothing: null removes it)
    //   remove {map | action | binding}                      (a composite's part too)
    //   rename {map | action, name}
    //   move {map | action | binding, index}                 (within its parent)
    //   duplicate {map | action | binding}                   (fresh ids)
    // Returns a human name for the undo step ("Add Action Gameplay/Jump").
    JASPERENGINE_API std::string applyInputActionsOp(InputActionAssetDesc& asset, const InputJson& op);

}

#endif // INPUTACTIONASSETJSON_H
