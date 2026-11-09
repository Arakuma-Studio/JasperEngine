// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MaterialPresets.h"

#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "util/Color.h"

#include <algorithm>

namespace jasperengine::editor {

namespace {

    const char* const OUTLINE_SHADER = "library:outline";
    const char* const OUTLINE_STENCIL_SHADER = "library:outline_stencil";
    const char* const SILHOUETTE_SHADER = "library:silhouette";
    const char* const STENCIL_MASK_SHADER = "library:stencil_mask";
    const char* const SPRITE_MASK_SHADER = "library:sprite_mask";
    // outline_stencil's VISIBILITY keyword values
    const char* const VISIBILITY_ALWAYS = "VISIBILITY_ALWAYS";
    const char* const VISIBILITY_VISIBLE_ONLY = "VISIBILITY_VISIBLE_ONLY";

    // How a Material holds its extra pass (out of line, MaterialExtraPass), in one place.
    // Reading never allocates (an unset pass reads as MaterialPass()); only editing does, so a
    // preset that merely looks at the pass leaves a material without one unchanged.
    const MaterialPass& extraPassOf(const Material& material){
        return *material.extraPass;
    }

    MaterialPass& editExtraPass(Material& material){
        return material.extraPass.edit();
    }

    void setExtraPassValue(Material& material, const MaterialPass& pass){
        material.extraPass = pass;
    }

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

    // What the outline_stencil preset makes the material write: bit 0x01 over its whole
    // silhouette, hidden parts included, which its pass outlines outside of
    MaterialStencil outlineStencil(){
        return makeStencil(MaterialStencilCompare::ALWAYS, PRESET_OUTLINE_STENCIL_BIT, 0xFF,
                           PRESET_OUTLINE_STENCIL_BIT, MaterialStencilOp::REPLACE, MaterialStencilOp::REPLACE);
    }

    bool isEqualTest(MaterialStencilCompare compare){
        return compare == MaterialStencilCompare::EQUAL || compare == MaterialStencilCompare::NOT_EQUAL;
    }

    // draw-once's test: not equal on bits 0x0C, replace (with or without the outline mark)
    bool isDrawOnceTest(const MaterialStencil& stencil){
        return stencil.compare == MaterialStencilCompare::NOT_EQUAL && stencil.readMask == PRESET_DRAW_ONCE_MASK &&
               (stencil.ref & PRESET_DRAW_ONCE_MASK) != 0 && stencil.pass == MaterialStencilOp::REPLACE;
    }

    // a mask reader's test: equal / not equal on bits 0xF0 (with or without the outline mark)
    bool isMaskReaderTest(const MaterialStencil& stencil){
        return isEqualTest(stencil.compare) && stencil.readMask == PRESET_MASK_BITS && (stencil.ref & PRESET_MASK_BITS) != 0;
    }

    // whether the material stencil writes the outline mark (bit 0x01, replace), alone or over a test
    bool hasOutlineMark(const MaterialStencil& stencil){
        return stencil.compare != MaterialStencilCompare::AUTO && stencil.compare != MaterialStencilCompare::DISABLED &&
               (stencil.writeMask & PRESET_OUTLINE_STENCIL_BIT) != 0 && (stencil.ref & PRESET_OUTLINE_STENCIL_BIT) != 0 &&
               stencil.pass == MaterialStencilOp::REPLACE &&
               (stencil.compare == MaterialStencilCompare::ALWAYS || (stencil.readMask & PRESET_OUTLINE_STENCIL_BIT) == 0);
    }

    // The material's test without the outline mark: what stays when its outline pass goes
    // (draw-once or a mask reader as their presets make them; the mark alone: no stencil)
    MaterialStencil withoutOutlineMark(const MaterialStencil& stencil){
        if (!hasOutlineMark(stencil))
            return stencil;
        if (isDrawOnceTest(stencil))
            return makeStencil(MaterialStencilCompare::NOT_EQUAL, stencil.ref & PRESET_DRAW_ONCE_MASK, PRESET_DRAW_ONCE_MASK,
                               PRESET_DRAW_ONCE_MASK, MaterialStencilOp::REPLACE);
        if (isMaskReaderTest(stencil))
            return makeStencil(stencil.compare, stencil.ref & PRESET_MASK_BITS, PRESET_MASK_BITS, PRESET_MASK_BITS,
                               MaterialStencilOp::KEEP);
        return MaterialStencil();
    }

    // The material's test plus the mark a visible-only outline needs: draw-once and the mask
    // readers keep their test and also write bit 0x01 where they draw (one stencil state: the
    // test on its bits, replace writing the test's bits and the mark); any other stencil becomes
    // the plain mark. Without depth-fail replace when combined: its write mask would write the
    // draw-once bits where the object is hidden, and a visible-only outline needs no hidden marks.
    MaterialStencil withVisibleOutlineMark(const MaterialStencil& stencil){
        const MaterialStencil test = withoutOutlineMark(stencil);
        if (isDrawOnceTest(test))
            return makeStencil(MaterialStencilCompare::NOT_EQUAL, test.ref | PRESET_OUTLINE_STENCIL_BIT, PRESET_DRAW_ONCE_MASK,
                               PRESET_DRAW_ONCE_MASK | PRESET_OUTLINE_STENCIL_BIT, MaterialStencilOp::REPLACE);
        if (isMaskReaderTest(test))
            return makeStencil(test.compare, test.ref | PRESET_OUTLINE_STENCIL_BIT, PRESET_MASK_BITS, PRESET_OUTLINE_STENCIL_BIT,
                               MaterialStencilOp::REPLACE);
        return outlineStencil();
    }

    bool hasFeature(const MaterialPass& pass, const char* define){
        return std::find(pass.shaderFeatures.begin(), pass.shaderFeatures.end(), define) != pass.shaderFeatures.end();
    }

    void setFeature(MaterialPass& pass, const char* define, bool on){
        std::vector<std::string>& features = pass.shaderFeatures;
        features.erase(std::remove(features.begin(), features.end(), define), features.end());
        if (on)
            features.push_back(define);
    }

    // Outline (hull) on a blended material: outline_stencil drawn only where visible
    bool isVisibleOnlyOutline(const Material& material){
        const MaterialPass& pass = extraPassOf(material);
        return pass.shader == OUTLINE_STENCIL_SHADER && hasFeature(pass, VISIBILITY_VISIBLE_ONLY);
    }

    // An extra pass drawing shader in order. A pass that already uses the shader keeps its
    // parameters, textures, features and state (re-applying a preset only fixes its order);
    // any other pass is replaced whole, so no state of another shader leaks into it. Replacing
    // an outline_stencil pass also drops the silhouette mark the material wrote for it, which
    // would otherwise hide other objects' stencil outlines wherever this one covers them (a
    // test the mark was combined with stays).
    void setExtraPass(Material& material, const char* shader, MaterialPassOrder order){
        if (extraPassOf(material).shader != shader){
            if (extraPassOf(material).shader == OUTLINE_STENCIL_SHADER)
                material.stencil = withoutOutlineMark(material.stencil);
            MaterialPass pass;
            pass.shader = shader;
            setExtraPassValue(material, pass);
        }
        editExtraPass(material).order = order;
    }

    // A preset setting the material's own stencil test (draw-once, the mask readers). A
    // visible-only outline keeps its mark over the new test. A through-walls outline_stencil
    // pass goes: it outlines outside the silhouette its mark wrote, hidden parts included
    // (depth-fail replace), which no test can share, and without that mark it would cover the
    // whole object, through walls.
    void setMaterialTest(Material& material, const MaterialStencil& test){
        if (isVisibleOnlyOutline(material)){
            material.stencil = withVisibleOutlineMark(test);
            return;
        }
        material.stencil = test;
        if (extraPassOf(material).shader == OUTLINE_STENCIL_SHADER)
            setExtraPassValue(material, MaterialPass());
    }

    // the outline values one outline pass hands to the other (the two shaders share their
    // parameters, textures and width features), without outline_stencil's visibility
    void copyOutlineLook(const MaterialPass& from, MaterialPass& to){
        to.shaderParams = from.shaderParams;
        to.shaderTextures = from.shaderTextures;
        to.shaderFeatures = from.shaderFeatures;
        setFeature(to, VISIBILITY_ALWAYS, false);
        setFeature(to, VISIBILITY_VISIBLE_ONLY, false);
    }

    // Outline (hull) on an opaque material: the inverted hull After it (the object's depth
    // rejects the hidden hull early); a visible-only outline it had keeps its look
    void makeHullOutline(Material& material){
        const bool wasVisibleOnly = isVisibleOnlyOutline(material);
        const MaterialPass old = extraPassOf(material);
        setExtraPass(material, OUTLINE_SHADER, MaterialPassOrder::AFTER);
        if (wasVisibleOnly)
            copyOutlineLook(old, editExtraPass(material));
    }

    // Outline (hull) on a blended material. A hull's back faces fill the object's whole shape,
    // which a blended object (no depth write) hides from neither side (Before: seen through it;
    // After: over it). So the material marks its silhouette (bit 0x01, over its own test) and an
    // After library:outline_stencil pass, Visible only (depth tested), draws the pushed-out mesh
    // outside the mark. A hull outline it had keeps its look; a new one starts from the hull's
    // defaults, so the preset looks alike on opaque and blended materials.
    void makeVisibleOutline(Material& material){
        const MaterialPass old = extraPassOf(material);
        if (old.shader != OUTLINE_STENCIL_SHADER){
            MaterialPass pass;
            pass.shader = OUTLINE_STENCIL_SHADER;
            std::shared_ptr<const ShaderManifest> hull = ShaderManifests::get(OUTLINE_SHADER);
            std::shared_ptr<const ShaderManifest> stencil = ShaderManifests::get(OUTLINE_STENCIL_SHADER);
            if (hull && stencil){
                for (const ShaderManifestProperty& property : hull->properties){
                    Vector4 value;
                    if (stencil->getDefault(property.member, value) && value != property.defaultValue)
                        ShaderUniforms::set(pass.shaderParams, property.member, property.defaultValue);
                }
            }
            if (old.shader == OUTLINE_SHADER){
                for (const auto& [member, value] : old.shaderParams)
                    ShaderUniforms::set(pass.shaderParams, member, value);
                pass.shaderTextures = old.shaderTextures;
                pass.shaderFeatures = old.shaderFeatures;
            }
            setExtraPassValue(material, pass);
        }
        MaterialPass& pass = editExtraPass(material);
        setFeature(pass, VISIBILITY_ALWAYS, false);
        setFeature(pass, VISIBILITY_VISIBLE_ONLY, true);
        pass.order = MaterialPassOrder::AFTER;
        material.stencil = withVisibleOutlineMark(material.stencil);
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
        // an invisible mask draws no outline or silhouette either
        setExtraPassValue(material, MaterialPass());
        material.sortPriority = std::min(material.sortPriority, -1);
    }

    // A reader of the masks: keeps a mask ref the material already tests (another mask's,
    // 32 ... 240) when switching between inside and outside, else the default 16
    void readMask(Material& material, MaterialStencilCompare compare){
        int ref = PRESET_MASK_REF;
        const MaterialStencil current = withoutOutlineMark(material.stencil);
        if (isEqualTest(current.compare) && current.readMask == PRESET_MASK_BITS && (current.ref & PRESET_MASK_BITS) != 0){
            ref = current.ref & PRESET_MASK_BITS;
        }
        setMaterialTest(material, makeStencil(compare, ref, PRESET_MASK_BITS, PRESET_MASK_BITS, MaterialStencilOp::KEEP));
    }

    // a pass stencil maskExtraPass set: a test on the mask bits (0xF0) with a mask ref
    bool isPassMaskTest(const MaterialStencil& stencil){
        return isEqualTest(stencil.compare) && (stencil.readMask & PRESET_MASK_BITS) == PRESET_MASK_BITS &&
               (stencil.ref & PRESET_MASK_BITS) != 0 && (stencil.ref & ~PRESET_MASK_BITS) == 0;
    }

    // whether the pass tests the stencil itself: its own explicit test, else its shader's @state
    bool passHasOwnStencil(const MaterialPass& pass){
        if (pass.stencil.compare != MaterialStencilCompare::AUTO)
            return !isPassMaskTest(pass.stencil);
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(pass.shader);
        return manifest && manifest->present && manifest->resolveState(manifest->resolveFeatures(pass.shaderFeatures)).hasStencil();
    }

    // A mask reader masks its extra pass too, so an outline or silhouette of a masked object
    // shows only where the object may. Run after every preset: a
    // material that reads no mask (any more) puts the mask test it gave its pass back to Auto.
    // - A pass with no stencil test of its own (Outline (hull), most passes): the reader's test.
    // - Silhouette (not equal 2 on bit 0x02, replace: blends once): inside a mask, equal to the
    //   mask ref on the mask bits and bit 0x02, inverting bit 0x02 (set where it blended);
    //   outside a mask, the mask test alone ("mask differs and bit clear" is no single stencil
    //   test): there its overlapping hidden parts blend more than once.
    // - A visible-only outline (outline_stencil: not equal 1 on bit 0x01): inside a mask,
    //   equal to the mask ref on the mask bits and bit 0x01; outside a mask it keeps its own
    //   test (the same reason): its ring is not clipped where it crosses the mask.
    // - Any other pass with a stencil test of its own keeps it.
    void maskExtraPass(Material& material){
        MaterialPass* passIfSet = material.extraPass.editIfSet();
        if (!passIfSet || passIfSet->shader.empty())
            return;
        MaterialPass& pass = *passIfSet;
        const MaterialStencil& test = material.stencil;
        const bool reader = isMaskReaderTest(test);
        const bool inside = test.compare == MaterialStencilCompare::EQUAL;
        const int ref = test.ref & PRESET_MASK_BITS;
        MaterialStencil masked; // AUTO: the pass shader's own rule
        if (pass.shader == OUTLINE_STENCIL_SHADER){
            // (a through-walls one never has a reader with it: setMaterialTest removes it)
            if (reader && inside && isVisibleOnlyOutline(material))
                masked = makeStencil(MaterialStencilCompare::EQUAL, ref, PRESET_MASK_BITS | PRESET_OUTLINE_STENCIL_BIT, 0,
                                     MaterialStencilOp::KEEP);
        } else if (pass.shader == SILHOUETTE_SHADER){
            if (reader){
                masked = inside
                    ? makeStencil(MaterialStencilCompare::EQUAL, ref, PRESET_MASK_BITS | PRESET_SILHOUETTE_STENCIL_BIT,
                                  PRESET_SILHOUETTE_STENCIL_BIT, MaterialStencilOp::INVERT)
                    : makeStencil(MaterialStencilCompare::NOT_EQUAL, ref, PRESET_MASK_BITS, 0, MaterialStencilOp::KEEP);
            }
        } else if (!passHasOwnStencil(pass)){
            if (reader)
                masked = makeStencil(test.compare, ref, PRESET_MASK_BITS, 0, MaterialStencilOp::KEEP);
        } else {
            return;
        }
        // only a stencil this set before (or none) is replaced: a pass's own explicit test stays
        if (pass.stencil.compare == MaterialStencilCompare::AUTO || isPassMaskTest(pass.stencil))
            pass.stencil = masked;
    }

    // S48 R38: the sprite material the Pixel FX presets build on: library:sprite, keeping its
    // features and values when it already draws with it (a preset changes only what it sets)
    constexpr const char* PIXEL_FX_SHADER = "library:sprite";

    void usePixelFxShader(Material& material){
        if (material.shader.path != PIXEL_FX_SHADER){
            material.shader = PIXEL_FX_SHADER;
            // another shader's features mean nothing to the sprite's
            material.shaderFeatures.clear();
        }
    }

    void setPixelFxFeature(Material& material, const char* define){
        ShaderLibraryValues::setFeature(material.shaderFeatures, PIXEL_FX_SHADER, define, true);
    }

    void setPixelFxParam(Material& material, const char* member, const Vector4& value){
        for (auto& entry : material.shaderParams){
            if (entry.first == member){
                entry.second = value;
                return;
            }
        }
        material.shaderParams.push_back({member, value});
    }

    Vector4 pixelFxNumber(float value){
        return Vector4(value, 0.0f, 0.0f, 0.0f);
    }

    // a built-in palette's ramp (S43): the editor writes the project's copy of the palette when
    // the preset is applied and points the slot at it (resolvePresetPalettes)
    void setPixelFxRamp(Material& material, const char* builtinRamp){
        ShaderLibraryValues::setTexture(material.shaderTextures, PIXEL_FX_RAMP_UNIFORM, builtinRamp);
    }

    void setPixelFxOutline(Material& material, bool selective){
        setPixelFxFeature(material, "OUTLINE");
        ShaderLibraryValues::setFeature(material.shaderFeatures, PIXEL_FX_SHADER, "OUTLINE_STYLE_PIXEL4", true);
        setPixelFxParam(material, "outlineWidth", pixelFxNumber(1.0f));
        setPixelFxParam(material, "outlineColorFrom", pixelFxNumber(selective ? 2.0f : 1.0f));
        if (selective)
            setPixelFxParam(material, "outlineRampSteps", pixelFxNumber(1.0f));
    }

    void setPixelFxRecolor(Material& material, const char* builtinRamp, float bevel, float pulse){
        usePixelFxShader(material);
        setPixelFxFeature(material, "RAMP_RECOLOR");
        setPixelFxParam(material, "rampAmount", pixelFxNumber(1.0f));
        setPixelFxParam(material, "rampBevel", pixelFxNumber(bevel));
        setPixelFxParam(material, "rampPulse", pixelFxNumber(pulse));
        setPixelFxRamp(material, builtinRamp);
    }

    void addPixelFxPresets(std::vector<MaterialPreset>& presets){
        presets.push_back({"hit_flash", "Hit Flash",
            "library:sprite with Hit flash at amount 0, white: PixelFX.play(go, \"flash\") or the flashAmount value flashes it.",
            [](Material& material, const std::string&){
                usePixelFxShader(material);
                setPixelFxFeature(material, "FLASH");
                setPixelFxParam(material, "flashColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f));
                setPixelFxParam(material, "flashAmount", pixelFxNumber(0.0f));
            }});
        presets.push_back({"pixel_outline", "Pixel Outline",
            "library:sprite with a one-texel outline on the 4 neighbours (Outline style PIXEL4) in the ramp's darkest colour; "
            "the quad grows so it is never cut at the frame edge.",
            [](Material& material, const std::string&){
                usePixelFxShader(material);
                setPixelFxOutline(material, false);
            }});
        presets.push_back({"selout_outline", "Sel-Out Outline",
            "library:sprite with a one-texel selective outline (sel-out): each outline texel takes the colour of the art it "
            "touches, one ramp step darker.",
            [](Material& material, const std::string&){
                usePixelFxShader(material);
                setPixelFxOutline(material, true);
            }});
        presets.push_back({"drop_shadow", "Drop Shadow",
            "library:sprite with a drop shadow one texel right and down, half-transparent black, behind the sprite and its outline.",
            [](Material& material, const std::string&){
                usePixelFxShader(material);
                setPixelFxFeature(material, "DROP_SHADOW");
                setPixelFxParam(material, "shadowOffset", Vector4(1.0f, -1.0f, 0.0f, 0.0f));
                setPixelFxParam(material, "shadowColor", Color::sRGBToLinear(Vector4(0.0f, 0.0f, 0.0f, 0.5f)));
            }});
        presets.push_back({"teleport_fade", "Teleport Fade",
            "library:sprite with Dither Fade at 0 (shown): PixelFX fade_out, fade_in and blink, or ditherFadeAmount, fade it "
            "out through a Bayer 4x4 pattern without partial alpha.",
            [](Material& material, const std::string&){
                usePixelFxShader(material);
                setPixelFxFeature(material, "DITHER_FADE");
                setPixelFxParam(material, "ditherFadeAmount", pixelFxNumber(0.0f));
            }});
        presets.push_back({"burn_away", "Burn Away",
            "library:sprite with Dissolve at 0 on the fire ramp (S43's built-in fire palette): PixelFX dissolve or "
            "dissolveProgress burns it from the feet up along a glowing edge.",
            [](Material& material, const std::string&){
                usePixelFxShader(material);
                setPixelFxFeature(material, "DISSOLVE");
                setPixelFxParam(material, "dissolveProgress", pixelFxNumber(0.0f));
                setPixelFxParam(material, "dissolveEdgeGlow", Vector4(1.5f, 0.6f, 0.1f, 1.0f));
                setPixelFxRamp(material, "builtin:fire#ramp:fire");
            }});
        presets.push_back({"petrify", "Petrify",
            "library:sprite with Ramp Recolour on the stone ramp and a bevel: the art turns to stone, its brightness kept "
            "as ramp steps.",
            [](Material& material, const std::string&){
                setPixelFxRecolor(material, "builtin:stone#ramp:stone", 1.0f, 0.0f);
            }});
        presets.push_back({"frozen", "Frozen",
            "library:sprite with Ramp Recolour on the ice ramp and a bevel: the frozen status look.",
            [](Material& material, const std::string&){
                setPixelFxRecolor(material, "builtin:ice#ramp:ice", 1.0f, 0.0f);
            }});
        presets.push_back({"poison", "Poison",
            "library:sprite with Ramp Recolour on the poison ramp, half the bevel and a pulse of 2 steps a second.",
            [](Material& material, const std::string&){
                setPixelFxRecolor(material, "builtin:poison#ramp:poison", 0.5f, 2.0f);
            }});
        presets.push_back({"pixel_enemy", "Enemy",
            "library:sprite for an enemy: Hit flash at 0, Dissolve at 0 on the fire ramp and a one-texel PIXEL4 outline in the "
            "ramp's darkest colour.",
            [](Material& material, const std::string&){
                usePixelFxShader(material);
                setPixelFxFeature(material, "FLASH");
                setPixelFxFeature(material, "DISSOLVE");
                setPixelFxParam(material, "flashColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f));
                setPixelFxParam(material, "flashAmount", pixelFxNumber(0.0f));
                setPixelFxParam(material, "dissolveProgress", pixelFxNumber(0.0f));
                setPixelFxOutline(material, false);
                setPixelFxRamp(material, "builtin:fire#ramp:fire");
            }});
    }

    std::vector<MaterialPreset> makeMaterialPresets(){
        std::vector<MaterialPreset> presets;

        presets.push_back({"outline_hull", "Outline (hull)",
            "Outline as an extra pass, keeping the material's own shader. On an opaque material an inverted "
            "hull (library:outline) After it: its depth rejects the hidden hull early. On a blended material "
            "(its blend, else its shader's) a hull would fill the see-through shape, so the material marks its "
            "silhouette in stencil bit 1 (over its draw-once or mask test, if any) and an After "
            "library:outline_stencil pass, Visibility Visible only, draws the outline outside it. Apply it "
            "again after changing the material's blend; the outline keeps its colour and width.",
            [](Material& material, const std::string& meshShader){
                if (isMaterialBlended(material, meshShader))
                    makeVisibleOutline(material);
                else
                    makeHullOutline(material);
            }});

        presets.push_back({"outline_stencil", "Outline (stencil)",
            "Outline that never covers its object, visible through walls (QuickOutline style): the material "
            "writes stencil bit 1 over its whole silhouette (always, replace, also where the depth test fails) "
            "and an After extra pass (library:outline_stencil, Visibility Always) draws the pushed-out mesh "
            "outside it. Sort priority at least 1 so occluders draw first. Keeps the material's shader; "
            "replaces its stencil (draw-once, mask readers).",
            [](Material& material, const std::string&){
                material.stencil = outlineStencil();
                setExtraPass(material, OUTLINE_STENCIL_SHADER, MaterialPassOrder::AFTER);
                setFeature(editExtraPass(material), VISIBILITY_VISIBLE_ONLY, false);
                drawAfterOccluders(material);
            }});

        presets.push_back({"see_through_silhouette", "See-through silhouette",
            "Shows the object's hidden parts in a colour where other objects cover it: a Before extra pass "
            "(library:silhouette, depth test greater, stencil bit 2 so it blends once). Sort priority at "
            "least 1 so occluders draw first. Keeps the material's shader.",
            [](Material& material, const std::string&){
                setExtraPass(material, SILHOUETTE_SHADER, MaterialPassOrder::BEFORE);
                drawAfterOccluders(material);
            }});

        presets.push_back({"draw_once_transparency", "Draw-once transparency",
            "Each pixel of this transparent object blends once, so its overlapping parts do not stack: "
            "stencil not equal 4 and replace on bits 0x0C. Makes an opaque material alpha blended (an Outline "
            "(hull) on it becomes the blended kind). Overlapping draw-once objects need different refs (4, 8, 12).",
            [](Material& material, const std::string& meshShader){
                setMaterialTest(material, makeStencil(MaterialStencilCompare::NOT_EQUAL, PRESET_DRAW_ONCE_REF,
                                                      PRESET_DRAW_ONCE_MASK, PRESET_DRAW_ONCE_MASK, MaterialStencilOp::REPLACE));
                if (!isMaterialBlended(material, meshShader))
                    material.blendMode = MaterialBlendMode::ALPHA;
                // now blended: a hull would fill it
                if (extraPassOf(material).shader == OUTLINE_SHADER)
                    makeVisibleOutline(material);
            }});

        presets.push_back({"stencil_mask", "Stencil mask",
            "Turns the object into an invisible mask (library:stencil_mask): writes stencil 16 on bits 0xF0, "
            "no colour, no depth, sort priority -1 so it draws before its readers. Objects with Visible "
            "inside mask / Visible outside mask show only inside / outside it.",
            [](Material& material, const std::string&){
                makeMask(material, STENCIL_MASK_SHADER);
            }});

        presets.push_back({"visible_inside_mask", "Visible inside mask",
            "The object shows only where a Stencil mask or Sprite mask drew (stencil equal 16 on bits 0xF0). "
            "Its extra pass (outline, silhouette) is masked too. Keeps the material's shader.",
            [](Material& material, const std::string&){
                readMask(material, MaterialStencilCompare::EQUAL);
            }});

        presets.push_back({"visible_outside_mask", "Visible outside mask",
            "The object shows only where no Stencil mask or Sprite mask drew (stencil not equal 16 on bits "
            "0xF0). Its extra pass is masked too, except a blended object's outline ring (one stencil test "
            "cannot check the mask and the outline mark); a silhouette then blends more than once where its "
            "hidden parts overlap. Keeps the material's shader.",
            [](Material& material, const std::string&){
                readMask(material, MaterialStencilCompare::NOT_EQUAL);
            }});

        presets.push_back({"sprite_mask", "Sprite mask",
            "Turns a Sprite into a Unity-style sprite mask (library:sprite_mask): writes stencil 16 on bits "
            "0xF0 in the shape of its image (alpha cutoff), no colour, sort priority -1. Sprites with "
            "Visible inside mask / Visible outside mask show only inside / outside it; in a 2D camera "
            "(hierarchy order) place the mask above them.",
            [](Material& material, const std::string&){
                makeMask(material, SPRITE_MASK_SHADER);
            }});

        // The extended lobes of the built-in Lit (S11): a library shader has none of them, so
        // these draw with the built-in Lit (a fork of it keeps them, as does the mesh's)
        auto litLobes = [](Material& material) -> MaterialPbrExtensions& {
            if (ShaderReference::isLibrary(material.shader)){
                material.shader.clear();
                material.shaderFeatures.clear();
            }
            return material.pbr.edit();
        };

        presets.push_back({"car_paint", "Car Paint",
            "Glossy paint under a clear coat (KHR_materials_clearcoat): a smooth dielectric coat over a "
            "metallic flake base. Keeps the base colour; draws with the built-in Lit.",
            [litLobes](Material& material, const std::string&){
                MaterialPbrExtensions& pbr = litLobes(material);
                pbr.clearcoat.enabled = true;
                pbr.clearcoat.factor = 1.0f;
                pbr.clearcoat.roughness = 0.03f;
                material.metallicFactor = 0.6f;
                material.roughnessFactor = 0.45f;
            }});

        presets.push_back({"velvet", "Velvet",
            "Cloth with a soft rim (KHR_materials_sheen): a Charlie sheen lobe over a rough dielectric. "
            "The sheen takes the base colour, brightened.",
            [litLobes](Material& material, const std::string&){
                MaterialPbrExtensions& pbr = litLobes(material);
                pbr.sheen.enabled = true;
                const Vector3 base(material.baseColorFactor.x, material.baseColorFactor.y, material.baseColorFactor.z);
                pbr.sheen.color = Vector3(std::min(base.x * 1.5f + 0.2f, 1.0f), std::min(base.y * 1.5f + 0.2f, 1.0f), std::min(base.z * 1.5f + 0.2f, 1.0f));
                pbr.sheen.roughness = 0.5f;
                material.metallicFactor = 0.0f;
                material.roughnessFactor = 0.9f;
            }});

        presets.push_back({"brushed_metal", "Brushed Metal",
            "Metal with highlights stretched along its tangent (KHR_materials_anisotropy). Needs tangents "
            "or UVs for the direction; Rotation turns it.",
            [litLobes](Material& material, const std::string&){
                MaterialPbrExtensions& pbr = litLobes(material);
                pbr.anisotropy.enabled = true;
                pbr.anisotropy.strength = 0.8f;
                pbr.anisotropy.rotation = 0.0f;
                material.metallicFactor = 1.0f;
                material.roughnessFactor = 0.35f;
            }});

        presets.push_back({"soap_bubble", "Soap Bubble",
            "A thin film's rainbow (KHR_materials_iridescence) on a clear, smooth surface: alpha blended, "
            "a 250 to 650 nm film. Stripped from the Reduced mobile tier.",
            [litLobes](Material& material, const std::string&){
                MaterialPbrExtensions& pbr = litLobes(material);
                pbr.iridescence.enabled = true;
                pbr.iridescence.factor = 1.0f;
                pbr.iridescence.ior = 1.33f;
                pbr.iridescence.thicknessNm = Vector2(250.0f, 650.0f);
                material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 0.15f);
                material.alphaMode = MaterialAlphaMode::BLEND;
                material.metallicFactor = 0.0f;
                material.roughnessFactor = 0.02f;
            }});

        presets.push_back({"clear_glass", "Clear Glass",
            "Glass that refracts the scene behind it (KHR_materials_transmission and _volume, IOR 1.5): "
            "drawn after the opaque objects over a copy of them. Objects drawn after it do not show "
            "through (the copy holds the opaque scene).",
            [litLobes](Material& material, const std::string&){
                MaterialPbrExtensions& pbr = litLobes(material);
                pbr.transmission.enabled = true;
                pbr.transmission.factor = 1.0f;
                pbr.volume.enabled = true;
                pbr.volume.thickness = 0.1f;
                pbr.specular.enabled = true;
                pbr.specular.ior = 1.5f;
                material.metallicFactor = 0.0f;
                material.roughnessFactor = 0.05f;
            }});

        presets.push_back({"tiled_floor", "Tiled Floor",
            "Hex tiling: the textures of UV set 0 sampled on three randomly rotated copies, which hides "
            "the grid a repeating texture shows from a distance (three texture taps per map).",
            [litLobes](Material& material, const std::string&){
                MaterialPbrExtensions& pbr = litLobes(material);
                pbr.hex.enabled = true;
                pbr.hex.rotation = 1.0f;
                pbr.hex.contrast = 0.6f;
            }});

        // S48 R38: the Pixel FX presets on library:sprite, in a block of their own
        addPixelFxPresets(presets);

        // every preset leaves the extra pass masked as the material's stencil says
        for (MaterialPreset& preset : presets){
            preset.apply = [apply = preset.apply](Material& material, const std::string& meshShader){
                apply(material, meshShader);
                maskExtraPass(material);
            };
        }
        return presets;
    }

}

bool isMaterialBlended(const Material& material, const std::string& meshShader){
    MaterialBlendMode blend = material.blendMode;
    const std::string& shader = material.shader.empty() ? meshShader : material.shader.path;
    if (blend == MaterialBlendMode::AUTO && !shader.empty()){
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
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
    static const std::vector<MaterialPreset> presets = makeMaterialPresets();
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
