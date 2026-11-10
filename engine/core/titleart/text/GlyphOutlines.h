// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R21, §6.6 "Outlines")
// - Purpose: the module's reader of glyph outlines: hb_font_draw_glyph_or_fail on a HarfBuzz
//   (sub-)font into geometry::Path, in font units (the face's scale is its units per em), y up,
//   the font's own winding; TrueType quadratics stay quadratics, CFF and CFF2 cubics stay
//   cubics, variable fonts draw at the sub-font's coordinates. TitleTextLayout uses it with the
//   same sub-fonts it shapes with, so a variable font's outlines match its advances.
// - Caching: per (font, glyph), for this object's lifetime or until clear(); the fonts must
//   outlive their entries.
// - Thread-safety: one instance per thread (HarfBuzz is built without thread safety).

#pragma once

#include "titleart/geometry/Path.h"

#include <cstdint>
#include <map>
#include <utility>

struct hb_font_t;

namespace jasperengine::titleart::text {

    class GlyphOutlines {
    public:
        // the outline of `glyph` in `font` (empty for a glyph without ink, such as a space)
        const geometry::Path& outline(hb_font_t* font, uint32_t glyph);
        void clear();

    private:
        std::map<std::pair<hb_font_t*, uint32_t>, geometry::Path> cache;
    };

}
