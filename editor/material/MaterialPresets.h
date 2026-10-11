// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "texture/Material.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // One-click material setups (shader library phase 2, section 2.5): data that sets a
    // material's shader, extra pass and render state, not engine features. apply changes
    // only the fields the effect needs and keeps the rest (an outline keeps the main
    // shader and its parameters; re-applying a preset keeps the parameters of a pass that
    // already uses its shader). The inspector's Presets menu and the MCP tools
    // list_material_presets / apply_material_preset call these on a submesh material.
    //
    // Stencil bits are reserved per family, so effects on different objects can be combined
    // in one camera pass (the stencil is cleared at each pass start and never survives it):
    //   0x01        outline_stencil  (the main material marks its silhouette, ref 1)
    //   0x02        see_through_silhouette (the silhouette pass marks where it blended)
    //   0x0C        draw_once_transparency (ref 4; 8 and 12 for more draw-once objects
    //               that overlap on screen: each needs its own ref to blend once)
    //   0xF0        stencil_mask, sprite_mask, visible_inside_mask, visible_outside_mask
    //               (ref 16; 32, 48 ... 240 for more masks, each reader using its mask's ref)
    // A material has one stencil state, so the presets that set the material's own stencil
    // (outline_stencil, draw_once_transparency, the masks and their readers) replace one
    // another; the extra-pass presets (outline_hull, see_through_silhouette) combine with
    // any of them except each other (one extra pass per material).
    struct MaterialPreset {
        std::string id, name, description;
        std::function<void(Material&)> apply;
    };

    // stencil refs and masks of the families above
    constexpr int PRESET_OUTLINE_STENCIL_BIT = 0x01;
    constexpr int PRESET_SILHOUETTE_STENCIL_BIT = 0x02;
    constexpr int PRESET_DRAW_ONCE_REF = 0x04;
    constexpr int PRESET_DRAW_ONCE_MASK = 0x0C;
    constexpr int PRESET_MASK_REF = 0x10;
    constexpr int PRESET_MASK_BITS = 0xF0;

    // the presets, in menu order
    const std::vector<MaterialPreset>& getMaterialPresets();
    // the preset with this id, or null
    const MaterialPreset* findMaterialPreset(const std::string& id);

    // whether a material draws blended: its blend mode, else its shader manifest's @state
    // blend (with its features), else the engine's legacy rule (alpha mode Blend, or Auto
    // with a transparent base colour texture or a base colour alpha below 1). A material
    // without its own shader is judged as the built-in Lit (a mesh-level custom shader is
    // not the material's).
    bool isMaterialBlended(const Material& material);

}
