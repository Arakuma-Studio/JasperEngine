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
    // A preset judges the material by the shader it draws with: its own, else meshShader (the
    // mesh's customShader, else the scene's default mesh shader, as
    // RenderSystem::getSubmeshShader; "" the built-in Lit). A .material file has no mesh: it
    // is judged by its own shader.
    //
    // Stencil bits are reserved per family, so effects on different objects can be combined
    // in one camera pass (the stencil is cleared at each pass start and never survives it):
    //   0x01        the outline mark: the main material marks its silhouette, which an
    //               outline_stencil pass outlines outside of (Outline (stencil), and Outline
    //               (hull) on a blended material)
    //   0x02        see_through_silhouette (the silhouette pass marks where it blended)
    //   0x0C        draw_once_transparency (ref 4; 8 and 12 for more draw-once objects
    //               that overlap on screen: each needs its own ref to blend once)
    //   0xF0        stencil_mask, sprite_mask, visible_inside_mask, visible_outside_mask
    //               (ref 16; 32, 48 ... 240 for more masks, each reader using its mask's ref)
    // A material has one stencil state. The material-stencil tests (draw-once, the mask
    // readers) replace one another; a visible-only outline mark (Outline (hull) on a blended
    // material) combines with either (one test plus the mark's write of bit 0x01). The
    // through-walls Outline (stencil) also marks the hidden parts (depth-fail replace), which
    // no other test can share: it replaces the material's test, and a test preset removes it.
    // The extra-pass presets (outlines, silhouette) replace each other (one extra pass per
    // material). A mask reader also masks the extra pass (see visible_inside_mask), and a mask
    // (which draws nothing visible) keeps no extra pass.
    struct MaterialPreset {
        std::string id, name, description;
        // meshShader: the shader the material draws with when it names none
        std::function<void(Material& material, const std::string& meshShader)> apply;
    };

    // stencil refs and masks of the families above
    constexpr int PRESET_OUTLINE_STENCIL_BIT = 0x01;
    constexpr int PRESET_SILHOUETTE_STENCIL_BIT = 0x02;
    constexpr int PRESET_DRAW_ONCE_REF = 0x04;
    constexpr int PRESET_DRAW_ONCE_MASK = 0x0C;
    constexpr int PRESET_MASK_REF = 0x10;
    constexpr int PRESET_MASK_BITS = 0xF0;

    // the Pixel FX presets' ramp slot (S48 R9, library:sprite's u_fxRampTexture)
    constexpr const char* PIXEL_FX_RAMP_UNIFORM = "u_fxRampTexture";

    // the presets, in menu order
    const std::vector<MaterialPreset>& getMaterialPresets();
    // the preset with this id, or null
    const MaterialPreset* findMaterialPreset(const std::string& id);

    // whether a material draws blended: its blend mode, else the @state blend (with its
    // features) of the shader it draws with (its own, else meshShader), else the engine's
    // legacy rule (alpha mode Blend, or Auto with a transparent base colour texture or a base
    // colour alpha below 1)
    bool isMaterialBlended(const Material& material, const std::string& meshShader = std::string());

}
