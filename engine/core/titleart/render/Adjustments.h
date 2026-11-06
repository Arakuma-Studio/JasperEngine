// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R16, R30, §6.1 "Adjustment", §6.4)
// - Purpose: the fourteen adjustment operations, one implementation for adjustment layers and
//   the `adjust` effect: whether an adjustment is identity (nothing runs), the CPU tone LUT of
//   curves, levels and brightness/contrast (Photoshop's 8-bit transfer tables, baked once per
//   adjustment), and the ADJUST pass of the others' closed forms (titleart/render/adjust.glsl).
// - Adjustments run on unpremultiplied colour clamped to 0..1 and sRGB-encoded (Photoshop
//   parity: an adjustment layer clips light above 1); the contrast pivot is fixed at 50 % so a
//   result never depends on content outside the layer.
// - Thread-safety: the CPU functions are pure.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitlePrograms.h"

#include <memory>

namespace jasperengine::titleart {

    class TitleRenderServices;
    struct TitleCacheEntry;

    // true when the adjustment leaves every pixel as it is (`effects_only` included): the
    // runner skips its operation
    JASPERENGINE_API bool adjustmentIsIdentity(const Json& adjustment);
    // true for the operations a tone LUT carries (curves, levels, brightness_contrast)
    JASPERENGINE_API bool adjustmentUsesToneLut(const Json& adjustment);
    // 256 RGBA entries: channel c of entry i = the output of channel c for input level i / 255
    JASPERENGINE_API void bakeToneLut(const Json& adjustment, float out[256 * 4]);

    // p[2..5] of the ADJUST pass and its LUT (a tone LUT or the gradient map's gradient), cached
    // in the renderer's cache; false for an identity or unknown adjustment
    JASPERENGINE_API bool setupAdjustment(const Json& adjustment, TitleRenderServices& services, TitleGpuPass& pass,
                                          std::shared_ptr<TitleCacheEntry>& lut);

}
