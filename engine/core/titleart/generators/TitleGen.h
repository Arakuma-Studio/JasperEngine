// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.7, R9-R11, R15, D7)
// - Purpose: what S83's generator definitions (generators/*.cpp, one file per family) share,
//   Pachislop's src/generators/common.ts ported: the generator programs (a number space of their
//   own, TITLE_VARIANT_GENERATOR: titleart_gen.vert / titleart_gen.frag), their pass skeleton
//   with the drawing space's transforms, Pachislop's seeded streams (mulberry32, its two seed
//   hashes and its integer hash, bit for bit so its presets place every element as tuned), the
//   follow offset (drawFollowed), the light glow (glowInto) and the param builders S83's tables
//   use (L / X animation marks, percent params).
// - Drawing spaces: a free generator draws in its layer space (canvas px from its position,
//   +y up: ctx.layerToTarget); a following one draws in document space where the followed layer
//   is (ctx.documentToTarget) and its own transform is an offset on top (drawFollowed). genPass
//   writes the space of a pass into the block: p[2] = target px -> space (a, b, c, d), p[3] =
//   (tx, ty, target px per space px, px_scale), p[4] = space -> target px (a, b, c, d), p[5] =
//   (tx, ty, QuadBatch's min_px, 0); titleart/gen/common.glsl reads them and the vertex stage
//   of the instanced programs gets p[0..5] as u_vs_titleart.
// - Randomness: every random value comes from Rng (Pachislop's two mulberry32 seedings) or
//   hashN; the element seed is its `seed` param plus the document's seed - 1 (S80 Deviation 39),
//   so the default document draws Pachislop's streams. No clock is read anywhere (R16).
// - Thread-safety: main thread inside a render call; Rng and hashN are pure.

#pragma once

#include "Export.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TitleFx.h"
#include "titleart/effects/TitleFxParams.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePasses.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine::titleart::gen {

    // titleart/gen/programs.glsl's TA_GEN_* numbers (append-only: cached shader keys)
    enum class GenProgram : uint8_t {
        Place = 1,              // a target resampled through an affine (the follow offset)
        GlowAdd = 2,            // a blurred source x amount added as light
        Grid = 3,               // analysis/ReadGrid's box means packed into RGBA8
        Shockwave = 10,
        LensFlare = 11,
        Sunburst = 12,
        FractalNoise = 13,
        Fire = 14,
        Smoke = 15,
        AuraTendrils = 16,
        AuraRising = 17,
        AuraNormals = 18,       // the content field's gradient, blurred for Aura's outward normals
        BacklightSource = 19,
        BacklightRadial = 20,
        BacklightCompose = 21,
        MagicCircle = 22,
        LightningCompose = 23,
        LightStreakCompose = 24,
        SpeedLinesBeta = 25,
        GlintsCompose = 34,     // Glints' stars as light and the hero's anamorphic cross
        // the mobile tier's Energy passes (§6.7.11, §6.7.12): the baked upstream distance, and
        // Fire and Smoke reading it with the tiling noise texture
        UpstreamField = 42,
        FireMobile = 43,
        SmokeMobile = 44,
        // 26 stays free: Brush Swirl draws with MeshBrushSwirl and QuadsSplatter only
        // instanced quads (QuadBatch): titleart/gen/tfx_quads.glsl in the vertex stage
        QuadsParticles = 64,
        QuadsGlints = 65,
        QuadsInk = 66,
        QuadsSpeedLines = 67,
        QuadsLightStreaks = 68,
        QuadsLightning = 69,
        QuadsSplatter = 70,
        QuadsTest = 71,
        QuadsLensFlare = 78,    // the mobile tier's Lens Flare (§6.7.9): core, streak and ghosts as quads
        // vertex meshes (QuadBatch::drawMesh): a ribbon of vertices pulled from the data texture
        MeshBrushSwirl = 96,
    };

    JASPERENGINE_API const char* genProgramName(GenProgram program);
    // every generator program (warm-up lists, TitleArtShaderBuild)
    JASPERENGINE_API const std::vector<GenProgram>& genPrograms();
    // the variant bits a program needs (TITLE_VARIANT_GENERATOR, and the RGBA8 encodings of the
    // distance fields it reads on a device with these caps)
    JASPERENGINE_API uint64_t genProgramVariant(GenProgram program, const TitleArtCaps& caps);
    // the ShaderPool key of a program variant
    JASPERENGINE_API ShaderKey genProgramKey(GenProgram program, uint64_t variant);

    // a pass of a generator program into `output`, drawing in the space `spaceToTarget` maps
    // from (the block's p[2..5], above)
    JASPERENGINE_API TitlePass genPass(const GeneratorContext& ctx, GenProgram program, const char* label, TargetRef& output,
                                       const TitleAffine& spaceToTarget);
    // the space a generator draws in: document space while following a layer, else its layer space
    JASPERENGINE_API TitleAffine drawSpace(const GeneratorContext& ctx);
    // target px per space px of an affine (Pachislop's layerPxScale)
    JASPERENGINE_API double spaceScale(const TitleAffine& spaceToTarget);
    // the target px a drawing-space box covers (x0, y0, x1, y1 in canvas px, +y up), grown by
    // one pixel and cut to the target: the scissor of a pass that draws only there (Pachislop's
    // drawBox)
    JASPERENGINE_API TitleRectI targetBox(const GeneratorContext& ctx, const TitleAffine& spaceToTarget, double x0, double y0, double x1, double y1);

    // an EffectContext over the generator's target (the effect library's blurs and fields)
    JASPERENGINE_API EffectContext effectContext(const GeneratorContext& ctx);

    // Pachislop's light glow of a generator layer (common.ts glowInto): a Gaussian of `src` of
    // radius ~3 sigma = radiusCanvasPx canvas px times `amount`, added as light onto `target`
    JASPERENGINE_API void glowInto(const GeneratorContext& ctx, const TargetRef& src, TargetRef& target, float radiusCanvasPx, float amount);

    // A following generator draws where the followed layer is (document space); its own
    // transform then moves the drawing (common.ts drawFollowed): at the identity `draw` writes
    // straight into `target`, else into a scratch resampled through the offset
    JASPERENGINE_API void drawFollowed(GeneratorContext& ctx, TargetRef& target, const std::function<void(TargetRef& out)>& draw);
    // true when the generator's transform moves its drawing off the followed layer
    JASPERENGINE_API bool hasFollowOffset(const GeneratorContext& ctx);

    // the element seed: the `key` param plus the document's seed - 1 (S80 Deviation 39)
    JASPERENGINE_API double seedOf(const GeneratorContext& ctx, const ParamValues& p, const char* key = "seed");

    // Pachislop's mulberry32 streams, bit for bit. Generators seeds hash as
    // src/generators/common.ts rng (floor(seed x 4096) ^ 0x9e3779b9, times 0x85ebca6b); the
    // effects' group B as src/effects/b/common.ts rng (floor(seed x 7919) ^ 0x9e3779b9)
    class JASPERENGINE_API Rng {
    public:
        enum class Seeding : uint8_t { Generators, EffectsB };
        explicit Rng(double seed, Seeding seeding = Seeding::Generators);
        // the next value in [0, 1)
        double next();
        double operator()() { return next(); }

    private:
        uint32_t state = 0;
        bool generators = true;
    };

    // Pachislop's integer hash of several numbers into [0, 1) (src/effects/b/common.ts hash)
    JASPERENGINE_API double hashN(std::initializer_list<double> values);

    // JavaScript's Math.imul and >>> on 32-bit values, for ports of Pachislop's integer maths
    inline int32_t imul(int32_t a, int32_t b) {
        return (int32_t)((uint32_t)a * (uint32_t)b);
    }

    // ---- parameter builders (S83 §6.3's marks) ----

    // a percent of a stated base, stored 0..100 (unit None, as S80 Deviation 21)
    inline ParamDef percent(const char* key, const char* label, double value, const char* doc, double min = 0.0, double max = 100.0) {
        return fxparams::number(key, label, value, min, max, ParamUnit::None, doc, 1.0);
    }
    // L: changes per frame through the live variant (D10)
    inline ParamDef live(ParamDef p) {
        p.anim = FxAnim::Live;
        return p;
    }
    // X: changing it re-runs the CPU layout or analysis
    inline ParamDef relayout(ParamDef p) {
        p.anim = FxAnim::Relayout;
        return p;
    }
    // a colour or gradient of light (S84's schemes recolour it, R55)
    inline ParamDef light(ParamDef p) {
        p.light = true;
        return p;
    }
    // S85 combines intensities of light by multiplying (§6.12)
    inline ParamDef multiplies(ParamDef p) {
        p.combine = ParamCombine::Multiply;
        return p;
    }
    // a Vec2 position in canvas px (+y up)
    JASPERENGINE_API ParamDef point(const char* key, const char* label, double x, double y, const char* doc);
    // a layer's `seed`: an integer 0..999 the document's seed adds to, stepped
    inline ParamDef seedParam(const char* doc = "Reshuffles every random element (the document's seed adds to it)") {
        return fxparams::seed("seed", "Seed", doc);
    }

    // the static cost model's row (§6.4): `passes` passes worth `fre` full-resolution
    // equivalents, `taps` the most taps one pixel reads, plus `instances` quads and the CPU estimate
    JASPERENGINE_API FxCost passCost(int passes, double fre, int taps, int instances = 0, double cpuMs = 0.0);

}
