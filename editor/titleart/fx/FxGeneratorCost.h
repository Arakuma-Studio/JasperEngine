// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.4, R37, R46)
// - Purpose: the generator layers' rows of estimateTitleFxCost (FxCost.h): a visible generator
//   layer through its definition's `estimate` at the desktop size or on the mobile surface, its
//   class, predicted ms on both tiers and its live variant. A hidden layer or one of an unknown
//   type adds no row (R37: what does not draw costs nothing).
// - Thread-safety: a pure function of the layer and the registry.

#pragma once

#include "FxCost.h"

namespace jasperengine::editor {

    void appendTitleGeneratorCostRow(const titleart::Json& layer, const titleart::FxSize& desktop, const titleart::FxSize& mobile, titleart::FxTier tier,
                                     TitleFxCost& out);

}
