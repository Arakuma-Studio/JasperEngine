// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MaterialPresets.h"

#include "shader/ShaderManifest.h"

#include <algorithm>

namespace jasperengine::editor {

namespace {

    const char* const OUTLINE_SHADER = "library:outline";
    const char* const OUTLINE_STENCIL_SHADER = "library:outline_stencil";
    const char* const SILHOUETTE_SHADER = "library:silhouette";
    const char* const STENCIL_MASK_SHADER = "library:stencil_mask";
    const char* const SPRITE_MASK_SHADER = "library:sprite_mask";

    MaterialStencil makeStencil(MaterialStencilCompare compare, int ref, int readMask, int writeMask,
                                MaterialStencilOp pass, MaterialStencilOp depthFail = MaterialStencilOp::KEEP){
        MaterialStencil stencil;
        stencil.compare = compare;
        stencil.ref = ref;
        stencil.readMask = readMask;
        stencil.writeMask = writeMask;
        stencil.pass = pass;
        stencil.fail = MaterialStencilOp::KEEP;
        stencil.depthFail = depthFail;
        return stencil;
    }

    // An extra pass drawing shader in order. A pass that already uses the shader keeps its
    // parameters, textures, features and state (re-applying a preset only fixes its order);
    // any other pass is replaced whole, so no state of another shader leaks into it.
    void setExtraPass(Material& material, const char* shader, MaterialPassOrder order){
        if (material.extraPass.shader != shader){
            material.extraPass = MaterialPass();
            material.extraPass.shader = shader;
        }
        material.extraPass.order = order;
    }

    // Effects that draw over what is already on screen (through-walls outline, silhouette)
    // need the object's occluders drawn first: the opaque queue draws sortPriority ascending
    // before distance, so a priority above the default 0 puts the object after them.
    void drawAfterOccluders(Material& material){
        material.sortPriority = std::max(material.sortPriority, 1);
    }

    // The material becomes a mask drawn by a mask shader: its manifest's state (stencil
    // write, no colour, no depth write, opaque queue) applies, so the explicit values that
    // would override it go back to Auto; the old shader's features and lighting do not
    // carry over. Masks draw before their readers (sortPriority -1).
    void makeMask(Material& material, const char* shader){
        material.shader = shader;
        material.shaderFeatures.clear();
        material.lighting = MaterialLighting::AUTO;
        material.blendMode = MaterialBlendMode::AUTO;
        material.depthWrite = MaterialDepthWrite::AUTO;
        material.colorWrite = MATERIAL_COLOR_WRITE_AUTO;
        material.stencil = MaterialStencil();
        material.sortPriority = std::min(material.sortPriority, -1);
    }

    // A reader of the masks: keeps a mask ref the material already tests (another mask's,
    // 32 ... 240) when switching between inside and outside, else the default 16
    void readMask(Material& material, MaterialStencilCompare compare){
        int ref = PRESET_MASK_REF;
        const MaterialStencil& current = material.stencil;
        if ((current.compare == MaterialStencilCompare::EQUAL || current.compare == MaterialStencilCompare::NOT_EQUAL) &&
            current.readMask == PRESET_MASK_BITS && (current.ref & PRESET_MASK_BITS) != 0){
            ref = current.ref & PRESET_MASK_BITS;
        }
        material.stencil = makeStencil(compare, ref, PRESET_MASK_BITS, PRESET_MASK_BITS, MaterialStencilOp::KEEP);
    }

    std::vector<MaterialPreset> makePresets(){
        std::vector<MaterialPreset> presets;

        presets.push_back({"outline_hull", "Outline (hull)",
            "Inverted-hull outline as an extra pass (library:outline), keeping the material's own shader. "
            "The pass goes After an opaque material (its depth rejects the hidden hull early) and Before a "
            "blended one (which writes no depth, so a hull after it would cover it); apply it again after "
            "changing the material's blend to fix the order.",
            [](Material& material){
                setExtraPass(material, OUTLINE_SHADER,
                             isMaterialBlended(material) ? MaterialPassOrder::BEFORE : MaterialPassOrder::AFTER);
            }});

        presets.push_back({"outline_stencil", "Outline (stencil)",
            "Outline that never covers its object, visible through walls by default (QuickOutline style): "
            "the material writes stencil bit 1 over its whole silhouette (always, replace, also where the "
            "depth test fails) and an After extra pass (library:outline_stencil) draws the pushed-out mesh "
            "outside it. Sort priority at least 1 so occluders draw first. Keeps the material's shader.",
            [](Material& material){
                material.stencil = makeStencil(MaterialStencilCompare::ALWAYS, PRESET_OUTLINE_STENCIL_BIT, 0xFF,
                                               PRESET_OUTLINE_STENCIL_BIT, MaterialStencilOp::REPLACE, MaterialStencilOp::REPLACE);
                setExtraPass(material, OUTLINE_STENCIL_SHADER, MaterialPassOrder::AFTER);
                drawAfterOccluders(material);
            }});

        presets.push_back({"see_through_silhouette", "See-through silhouette",
            "Shows the object's hidden parts in a colour where other objects cover it: a Before extra pass "
            "(library:silhouette, depth test greater, stencil bit 2 so it blends once). Sort priority at "
            "least 1 so occluders draw first. Keeps the material's shader.",
            [](Material& material){
                setExtraPass(material, SILHOUETTE_SHADER, MaterialPassOrder::BEFORE);
                drawAfterOccluders(material);
            }});

        presets.push_back({"draw_once_transparency", "Draw-once transparency",
            "Each pixel of this transparent object blends once, so its overlapping parts do not stack: "
            "stencil not equal 4 and replace on bits 0x0C. Makes an opaque material alpha blended. "
            "Overlapping draw-once objects need different refs (4, 8, 12).",
            [](Material& material){
                material.stencil = makeStencil(MaterialStencilCompare::NOT_EQUAL, PRESET_DRAW_ONCE_REF, PRESET_DRAW_ONCE_MASK,
                                               PRESET_DRAW_ONCE_MASK, MaterialStencilOp::REPLACE);
                if (!isMaterialBlended(material))
                    material.blendMode = MaterialBlendMode::ALPHA;
            }});

        presets.push_back({"stencil_mask", "Stencil mask",
            "Turns the object into an invisible mask (library:stencil_mask): writes stencil 16 on bits 0xF0, "
            "no colour, no depth, sort priority -1 so it draws before its readers. Objects with Visible "
            "inside mask / Visible outside mask show only inside / outside it.",
            [](Material& material){
                makeMask(material, STENCIL_MASK_SHADER);
            }});

        presets.push_back({"visible_inside_mask", "Visible inside mask",
            "The object shows only where a Stencil mask or Sprite mask drew (stencil equal 16 on bits 0xF0). "
            "Keeps the material's shader.",
            [](Material& material){
                readMask(material, MaterialStencilCompare::EQUAL);
            }});

        presets.push_back({"visible_outside_mask", "Visible outside mask",
            "The object shows only where no Stencil mask or Sprite mask drew (stencil not equal 16 on bits "
            "0xF0). Keeps the material's shader.",
            [](Material& material){
                readMask(material, MaterialStencilCompare::NOT_EQUAL);
            }});

        presets.push_back({"sprite_mask", "Sprite mask",
            "Turns a Sprite into a Unity-style sprite mask (library:sprite_mask): writes stencil 16 on bits "
            "0xF0 in the shape of its image (alpha cutoff), no colour, sort priority -1. Sprites with "
            "Visible inside mask / Visible outside mask show only inside / outside it; in a 2D camera "
            "(hierarchy order) place the mask above them.",
            [](Material& material){
                makeMask(material, SPRITE_MASK_SHADER);
            }});

        return presets;
    }

}

bool isMaterialBlended(const Material& material){
    MaterialBlendMode blend = material.blendMode;
    if (blend == MaterialBlendMode::AUTO && !material.shader.empty()){
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(material.shader);
        if (manifest && manifest->present)
            blend = manifest->resolveState(manifest->resolveFeatures(material.shaderFeatures)).blend;
    }
    if (blend == MaterialBlendMode::AUTO){
        // RenderSystem's legacy rule (usesAlphaBlend)
        return material.alphaMode == MaterialAlphaMode::BLEND ||
               (material.alphaMode == MaterialAlphaMode::AUTO &&
                (material.baseColorTexture.isTransparent() || material.baseColorFactor.w != 1.0f));
    }
    return blend != MaterialBlendMode::NONE;
}

const std::vector<MaterialPreset>& getMaterialPresets(){
    static const std::vector<MaterialPreset> presets = makePresets();
    return presets;
}

const MaterialPreset* findMaterialPreset(const std::string& id){
    for (const MaterialPreset& preset : getMaterialPresets()){
        if (preset.id == id)
            return &preset;
    }
    return nullptr;
}

}
