// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.4, R41, R46)
// - Purpose: the static cost model of a document (estimateTitleFxCost, mirroring
//   estimatePostProcessingCost): each enabled effect and each visible generator layer through its
//   definition's `estimate` (a definition without one is one pass of 1 FRE), at the bake size
//   (the canvas at px_scale 1) on the desktop tier or a 1024 x 512 surface on the mobile tier,
//   with predicted milliseconds per tier, the class the FRE and taps fall in, and totals.
//   render_title {options: {cost}} returns it; S82's badges read the same rows.
// - Predicted ms = FRE x the tier's cost of one FRE + instances x the per-instance cost + the
//   CPU estimate. The desktop constant is S83's uncalibrated default until TitleFxBench.Calibrate
//   runs on server H (AC15); mobile figures are predictions (AC22).
// - Thread-safety: pure function of the document and the registry.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleEffectRegistry.h"

namespace jasperengine::editor {

    constexpr double TITLE_FX_DESKTOP_MS_PER_FRE = 0.012;     // 1920 x 1080 on the reference GPU (S83 §6.4)
    constexpr double TITLE_FX_MOBILE_MS_PER_FRE = 0.35;       // 1024 x 512, predicted
    constexpr double TITLE_FX_DESKTOP_MS_PER_INSTANCE = 0.0004;
    constexpr double TITLE_FX_MOBILE_MS_PER_INSTANCE = 0.002;

    // the class §6.4's static limits put a cost in (C0 <= 1 FRE and 16 taps, C1 <= 3 and 64,
    // C2 <= 10 and 200, C3 <= 40 FRE or SDF / analysis, C4 above)
    titleart::CostClass titleFxCostClass(const titleart::FxCost& cost);

    // {tier, total: {class, passes, fre, desktop_ms, mobile_ms}, items: [{layer, path, type, entry,
    // class, declared, passes, fre, taps, instances, cpu_ms, sdf, analysis, readback, desktop_ms,
    // mobile_ms, live: {kind, params}}]}; `tier` "desktop" or "mobile" chooses the size and the
    // estimates the class is read from
    titleart::Json estimateTitleFxCost(const titleart::TitleArtDoc& doc, titleart::FxTier tier);
    // one line: "cost: C3 total, desktop 3.1 ms, mobile 11.8 ms predicted; Title: light_glow C3 2.4 ms, ..."
    std::string titleFxCostSummary(const titleart::Json& cost);

}
