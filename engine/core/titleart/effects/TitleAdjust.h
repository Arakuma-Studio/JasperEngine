// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.1 Adjustment, §6.4 adjustment layers, R30)
// - Purpose: the one implementation of the fourteen adjustment operations, shared by the
//   `adjust` effect and adjustment layers (the renderer's Adjustments pass calls
//   applyAdjustment with its own EffectContext). The operation runs on unpremultiplied colour
//   clamped to 0..1 and sRGB-encoded (Photoshop parity: an adjustment clips light above 1), alpha
//   unchanged; Brightness/Contrast, Levels and Curves are one 256-entry tone lookup baked on the
//   CPU, the others closed form in titleart/adjust.glsl.
// - Input: an `adjustment` object as TitleArtSchema normalizes it ({type, ...members}, every
//   member present); an unknown type adjusts nothing.
// - Thread-safety: the bake functions are pure; applyAdjustment runs passes (main thread).

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/render/TitleEffectContext.h"

#include <vector>

namespace jasperengine::titleart {

    // adjust.glsl's op numbers
    enum class TitleAdjustOp : int {
        Lut = 0, Exposure = 1, Vibrance = 2, HueSaturation = 3, ColorBalance = 4, BlackWhite = 5, PhotoFilter = 6,
        GradientMap = 7, Invert = 8, Posterize = 9, Threshold = 10, None = -1
    };

    // the op an adjustment type runs (None: effects_only and unknown types)
    JASPERENGINE_API TitleAdjustOp titleAdjustOp(const Json& adjustment);
    // true when it leaves every pixel as it is (the renderer and the effect skip the pass)
    JASPERENGINE_API bool titleAdjustmentIsIdentity(const Json& adjustment);
    // Brightness/Contrast, Levels or Curves as 256 x 3 outputs (R, G, B for input level i / 255,
    // 0..1); false for the other types. Curves: each channel's curve, then the composite's;
    // Levels: the composite, then each channel's; Brightness/Contrast: modern (never clips),
    // contrast pivoting at 50 % grey
    JASPERENGINE_API bool bakeTitleToneCurve(const Json& adjustment, std::vector<float>& rgb);
    // the tone lookup of a tone-curve type in `format` (RGBA16F, or RGBA8 on the fallback)
    JASPERENGINE_API TitleLutData titleToneLut(const Json& adjustment, ColorFormat format);

    // src adjusted into dst (same size); false when a pass failed (the runner says why). Never
    // called for identity adjustments: the caller copies or skips
    JASPERENGINE_API bool applyTitleAdjustment(EffectContext& ctx, const TargetRef& src, const Json& adjustment, TargetRef& dst);

}
