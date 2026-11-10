// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R25, R30, R31)
// - Purpose: what a document gives Glyph Atlas: its dynamic text layer (runtime.glyph_layer, else
//   the first text layer the exposed `text` targets), whether it must fall back to Realtime (no
//   such layer, Dimension 3D text, an ancestor group that blends other than normal), and the
//   effects and style members that do not apply per glyph (`unsafe`: skipped) or apply only
//   approximately, each with the ops that make it safe where there are some.
// - Glyph safety comes from the registry's definitions (S83 sets the values, S80 R31's defaults)
//   and S81's table for the style members (rims with hull or box plates, warp, distort, masks).
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    struct JASPERENGINE_API TitleGlyphAtlasItem {
        std::string label;          // "Warp", "Plate (Hull)", "Outer Glow"
        std::string path;           // the field path in the layer ("warp.style", "fx:outer_glow")
        Json fix;                   // {ops: [...]} ready for edit_title, or null
    };

    struct JASPERENGINE_API TitleGlyphAtlasPlan {
        std::string textLayer;      // the dynamic layer's id ("" when none)
        std::string fallback;       // "" or the glyph_atlas_unsupported reason ("3D text", "group 'Back' blends multiply")
        std::vector<TitleGlyphAtlasItem> unsafe;
        std::vector<TitleGlyphAtlasItem> approximate;

        std::vector<std::string> unsafeLabels() const;
        std::vector<std::string> approximateLabels() const;
    };

    JASPERENGINE_API TitleGlyphAtlasPlan planTitleGlyphAtlas(const TitleArtDoc& doc);

}
