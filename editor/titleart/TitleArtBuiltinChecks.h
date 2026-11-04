// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R23, R56, §6.12)
// - Purpose: S80's rows of the check registry (TitleArtChecks.h). Document checks (no GPU):
//   document_valid, assets_present, glyphs_present, exposed_targets, bake_fresh. Pixel checks
//   (through the context's render function, SKIP without one): not_flat, not_clipped,
//   alpha_bounds, legibility_contrast, counter_survival, noop_effects, clipped_highlights, halo,
//   deterministic, fallback_parity, golden.
// - Each result echoes the threshold it used, says how it measured, carries crops (image space of
//   the canvas) and, where one exists, a `fix` ({ops: [...]} ready for edit_title). Thresholds are
//   starting points (S84 calibrates them on the library).
// - Missing glyphs (R23): a code point no face of the chain (font, fallback fonts, the built-in
//   face) covers; the result lists {index, codepoint, char} and the project fonts that have it.
// - Thread-safety: main thread.

#pragma once

#include "TitleArtChecks.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // registers S80's checks once (later calls do nothing)
    void registerBuiltinTitleChecks();

    // the ids S80 registers, in registration order
    const std::vector<std::string>& builtinTitleCheckIds();

    // the missing glyphs of a document (no GPU): one problem `glyph_missing` per text layer with
    // chars that no face of its chain covers (TitleProblem::detail {chars, fonts_with_it})
    std::vector<titleart::TitleProblem> titleArtMissingGlyphs(const titleart::TitleArtDoc& doc, const titleart::AssetResolver& assets);
    // AssetRefs that do not resolve: `asset_missing` problems with the field and the kind
    std::vector<titleart::TitleProblem> titleArtMissingAssets(const titleart::TitleArtDoc& doc, const titleart::AssetResolver& assets);

}
