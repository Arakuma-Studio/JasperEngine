// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.4, R41, R46, D13)
// - Purpose: the static cost model of a document, as estimatePostProcessingCost does for the post
//   stack: each enabled effect's `estimate` (full-resolution equivalents, taps, instances, CPU),
//   the shared distance field counted once per layer as its own row, the class each falls in
//   (C0-C4) and predicted milliseconds on the desktop tier (the editor's path at the canvas size)
//   and the mobile tier (its reductions on a 1024 x 512 surface). Mobile figures are predictions
//   until a device measures them (AC22). render_title {options: {cost}} and the Designer's badges
//   read it.
// - Thread-safety: a pure function of the document and the registry.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TfxCommon.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // the cost of one FRE (ms) and of one instance on each tier (§6.4): the desktop constant is
    // TitleFxBench.Calibrate's on the RTX 3080 Ti server, the mobile one an estimate
    // (titleart/effects/TfxCommon.h holds them: S81's runtime live budget predicts with them too)
    constexpr double TITLE_FX_DESKTOP_MS_PER_FRE = titleart::tfx::DESKTOP_MS_PER_FRE;
    constexpr double TITLE_FX_MOBILE_MS_PER_FRE = titleart::tfx::MOBILE_MS_PER_FRE;
    constexpr double TITLE_FX_DESKTOP_MS_PER_INSTANCE = titleart::tfx::DESKTOP_MS_PER_INSTANCE;
    constexpr double TITLE_FX_MOBILE_MS_PER_INSTANCE = titleart::tfx::MOBILE_MS_PER_INSTANCE;

    struct TitleFxCostRow {
        std::string layer, path;            // the layer id and the effect's field path ("fx:light_glow")
        std::string type;                   // "light_glow", or "sdf" for the shared distance field
        titleart::CostClass costClass = titleart::CostClass::C0;
        titleart::FxCost cost;              // at the tier
        double desktopMs = 0, mobileMs = 0;
        titleart::LiveKind liveKind = titleart::LiveKind::None;
        std::vector<std::string> liveParams;
    };

    struct TitleFxCost {
        std::vector<TitleFxCostRow> rows;
        titleart::CostClass total = titleart::CostClass::C0;
        double desktopMs = 0, mobileMs = 0;
        titleart::Json toJson() const;      // §6.4's readout
        std::string summary() const;        // "cost: C3 total, desktop 3.1 ms, mobile 11.8 ms predicted; ..."
    };

    TitleFxCost estimateTitleFxCost(const titleart::TitleArtDoc& doc, titleart::FxTier tier);

}
