// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.6.1, R19, D11, D14)
// - Purpose: Light Glow's scattering kernel, shared by Light Glow and Neon Tube (and S83's light
//   generators): the power-law point-spread function as a scale mixture of unit-energy Gaussians
//   one octave apart, weights ∝ sigma^(2 - p) normalised to 1 (Pachislop lightGlow.ts), blurred on a
//   box pyramid and accumulated at full resolution (the editor and desktop) or cascaded down the
//   pyramid to half resolution (the mobile tier, D11).
// - Units: radius in canvas px, sigma in render px (radius x px_scale), the light in the targets
//   divided by the headroom of TfxPrograms.h (D14).
// - Thread-safety: main thread, inside an apply call.

#pragma once

#include "Export.h"
#include "titleart/effects/TitleFx.h"

#include <vector>

namespace jasperengine::titleart::tfx {

    struct GlowOctave {
        double sigma[3] = {0.0, 0.0, 0.0};      // per channel, render px
        double weight = 0.0;                    // energy weight (the octaves sum to 1)
        double t = 0.0;                         // 0 the innermost octave .. 1 the outermost (tints)
    };

    // Falloff 0 % -> long haze (p = 1.6) .. 100 % -> tight (p = 3.2)
    JASPERENGINE_API double falloffExponent(double falloffPct);
    // the octaves of radius (canvas px, the top sigma = radius / 3), exponent p, chroma -1..1;
    // maxOctaves > 0 spreads the span over at most that many
    JASPERENGINE_API std::vector<GlowOctave> psfOctaves(double radius, double p, double chroma, int maxOctaves, double pxScale);
    // the variance (render px^2) the pyramid adds to an octave blurred at `level`
    JASPERENGINE_API double glowLevelVariance(int level, bool cascade);
    // the pyramid level an octave is blurred on: the coarsest whose texels keep its narrowest
    // sigma >= 2 texels (half resolution at least on the cascade)
    JASPERENGINE_API int glowLevel(const GlowOctave& octave, int width, int height, bool cascade);

    // the over-exposure response of the compose (light.glsl tfx_overexpose), on the CPU: the
    // identity, bit for bit, where no channel exceeds 1 (R19)
    JASPERENGINE_API void lightResponse(const float in[3], float shift, float white, float out[3]);

    struct ScatterOptions {
        float gain = 1.0f;              // 2^exposure
        float threshold = 0.0f, knee = 0.0f;
        bool custom = false;            // the glow takes `color` instead of the layer's colours
        TitleColor color{1.0f, 1.0f, 1.0f};     // linear
        std::vector<GlowOctave> octaves;
        float intensity = 1.0f;
        TitleColor tintInner{1.0f, 1.0f, 1.0f}, tintOuter{1.0f, 1.0f, 1.0f};
        float tintAmount = 0.0f;
    };

    // the headroom scatterLight divides by: D14's, or an encoded input's own when it is larger (a
    // light accumulator's light reaches past D14's 4)
    JASPERENGINE_API float scatterHeadroom(const EffectContext& ctx);
    // the scattered light of `input` (its emitting part x gain, through the octaves), RGB only,
    // divided by scatterHeadroom: full size, or half size on the mobile cascade (sample it by uv)
    JASPERENGINE_API fx::Scratch scatterLight(EffectContext& ctx, const TargetRef& input, const ScatterOptions& options);
    // its static cost
    JASPERENGINE_API FxCost estimateScatter(const std::vector<GlowOctave>& octaves, const FxSize& size, FxTier tier);

}
