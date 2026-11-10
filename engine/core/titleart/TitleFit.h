// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R8-R11, D13)
// - Purpose: auto-fit of a text layer to its `fit.box` (layer space, centred on the text block's
//   anchor, moving, turning and scaling with the layer) by geometry, never by pixels: the
//   measured extent is the ink box of the glyph outlines after warp, distort and weight, grown
//   on each side by the cumulative width of the enabled rims plus each rim's offset; effects may
//   bleed outside the box. Exact for round and bevel joins; mitre spikes are absorbed by the 2 %
//   tolerance of the overflow check.
// - Modes: `shrink` (the largest uniform scale in [min_scale, 1] that fits, to 1/256: the
//   extent scales with the size, so the first measure predicts the answer and at most 9 measures
//   confirm it), `condense` (h_scale x c, c in [min_h_scale, 1], then shrink), `wrap` (line
//   breaks that make the longest line as short as possible for 1..max_lines lines, the fewest
//   lines that fit, then shrink; breaks at spaces, U+3000, existing newlines and between CJK
//   characters with the kinsoku rules of R10). Vertical text measures the column length against
//   box[1].
// - The result writes into the layer: `size` x scale, `h_scale` x c, the wrapped text (a space
//   becomes a newline; a CJK break inserts one and moves the CharAdjust keys after it), and is
//   cached per (layer, resolved text and members) hash.
// - Thread-safety: functions are pure apart from the result cache (a mutex).

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class AssetResolver;

    constexpr double TITLE_FIT_TOLERANCE = 0.02;    // overflow beyond the box that still fits (mitre spikes)

    struct TitleFitResult {
        bool active = false;            // a mode other than none with a box
        double scale = 1.0;             // uniform scale applied to the size
        double hScale = 1.0;            // condense factor applied to h_scale
        int lines = 1;
        bool fits = true;
        double overflowPx = 0.0;        // how far the extent passes the box (layer px, the larger side)
        TitleVec2 extent;               // the measured extent after fit (layer px, width x height)
        TitleVec2 box;
        int measures = 0;               // geometry measures this fit took (<= 9 per stage)
        std::string text;               // the text after wrapping
    };

    // the extent of a text layer (layer space, centred on its anchor): 2 x the farthest ink on
    // each axis, rims included (R9)
    JASPERENGINE_API TitleVec2 measureTitleTextExtent(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets);
    // the CJK break opportunities of R10 between code points `before` and `after`
    JASPERENGINE_API bool titleFitCanBreakBetween(uint32_t before, uint32_t after);

    // fit one text layer (its `fit` resolved for the language); `layer` is not changed
    JASPERENGINE_API TitleFitResult computeTitleFit(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets);
    // writes a result into the layer (size, h_scale, text and its CharAdjust keys)
    JASPERENGINE_API void applyTitleFit(Json& layer, const TitleFitResult& result);
    // fit every text layer of `doc` that has one; the results by layer id (the layers without fit
    // are absent)
    JASPERENGINE_API std::vector<std::pair<std::string, TitleFitResult>> applyTitleFits(TitleArtDoc& doc, const AssetResolver* assets);
    // a document has a text layer with fit.mode other than none and a box
    JASPERENGINE_API bool titleHasFit(const Json& root);

    // tests: drop the cache
    JASPERENGINE_API void clearTitleFitCache();

}
