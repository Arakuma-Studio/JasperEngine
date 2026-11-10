// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.7, D12)
// - Purpose: what the generators' mobile-tier paths (GeneratorContext::tier == FxTier::Mobile)
//   share: drawing at a fraction of the target's resolution (Fire, Smoke, Aura at half,
//   Light Streaks' halos and Backlight at quarter), the tiling noise texture that replaces
//   Fire's and Smoke's value noises, and the upstream distance E baked into a field once
//   instead of marched per flame pixel. Each definition's `estimate` for FxTier::Mobile counts
//   exactly the passes these helpers run.
// - Reduced contexts: the same drawing at 1/divisor of the target's pixels. Every transform and
//   px_scale is scaled by the reduced size over the full one, so a reduced target sampled at
//   ta_uv() (bilinearly) lines up with the full one pixel for pixel; analysis keys change with
//   the divisor, since the reduced inputs are other pixels.
// - Noise texture: 256 x 256 RGBA8, uploaded once through TitlePassRunner::lut (cached by
//   content). R and G are two independent fbm-like fields (four octaves of periodic value noise,
//   mean 0.5) repeating every 255 texels, so a lookup at fract(p / 16) x 255 + 0.5 texels is
//   seamless with LINEAR filtering and clamped edges (texel 255 holds texel 0's value); one
//   lattice unit of Pachislop's noise is 255 / 16 texels (titleart/gen/tfx_noise_tex.glsl).
// - Upstream field: E / length (Pachislop's upstream march, tfx_upstream.glsl) per pixel of
//   the context's target, clamped to 0..1, in a TitleBuffer::Distance target: R16F, or the
//   RGBA8 fallback's 16-bit fixed point in RG (titleart/gen/energy_mobile.glsl reads both).
// - Thread-safety: main thread inside a render call (the noise table is built once, thread-safe).

#pragma once

#include "Export.h"
#include "titleart/effects/TitleFx.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePasses.h"

#include <functional>

namespace jasperengine::titleart::gen {

    // `ctx` at 1/divisor of its resolution: its size rounded up, px_scale, frame and both
    // transforms scaled by the reduced size over the full one; no followed layer (drawReduced
    // supplies the content), no problems yet
    JASPERENGINE_API GeneratorContext reducedContext(const GeneratorContext& ctx, int divisor);

    // `draw` into a target of reducedContext(ctx, divisor) given the followed layer's content
    // downsampled to it (followContent; null when ctx has none), then that target resampled
    // into all of `target`: one downsample copy at the reduced size and one full-size copy
    // besides what `draw` runs. The reduced context's problems and canvas reads carry over
    JASPERENGINE_API void drawReduced(GeneratorContext& ctx, TargetRef& target, int divisor,
                                      const std::function<void(GeneratorContext& reduced, TargetRef& out)>& draw);

    // the tiling noise texture (above), built once
    JASPERENGINE_API const TitleLutData& mobileNoiseData();

    // the upstream distance from the content's distance field `field` (ctx's size), marched
    // against `flow` (a unit direction in ctx's drawing space, y up) over `lengthPx` target px
    // with sideways distances weighted by `side`, as E / lengthPx (above); empty on failure
    JASPERENGINE_API fx::Scratch upstreamField(const GeneratorContext& ctx, const TargetRef& field, const TitleVec2& flow, double lengthPx, double side);

}
