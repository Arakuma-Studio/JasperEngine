// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_BUILTINPIXELFONT_H
#define PIXELART_BUILTINPIXELFONT_H

// `builtin:pixel` (S51 R16): 5x7 glyphs in 6x9 cells, drawn for this engine (no glyph traced or
// copied from another font) and released under its licence. Printable ASCII is drawn; the
// Latin-1 letters U+00C0-U+00FF are composed here from the base letters and five accent marks
// (grave, acute, circumflex, tilde, diaeresis), and the few that no mark makes are drawn.

#include "pixelart/BitmapFont.h"

namespace jasperengine::pixelart {

    inline constexpr int kBuiltinCellWidth = 6;
    inline constexpr int kBuiltinCellHeight = 9;
    inline constexpr int kBuiltinBaseline = 7;

    // the grid source of the font: one 6x9 cell per glyph, ASCII then U+00C0-U+00FF
    JASPERENGINE_API RawFont builtinPixelRaw();
    // the glyph's 9 rows of 5 columns, '#' ink and '.' clear (empty when not covered)
    JASPERENGINE_API std::vector<std::string> builtinPixelGlyphRows(uint32_t codepoint);

}

#endif //PIXELART_BUILTINPIXELFONT_H
