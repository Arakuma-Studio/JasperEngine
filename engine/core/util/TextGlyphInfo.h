// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTGLYPHINFO_H
#define TEXTGLYPHINFO_H

#include <cstdint>

namespace jasperengine {

    // one glyph quad of STBText::createText: the codepoint index of its cluster (logical
    // order) and its line, so per-character effects find their glyphs
    struct TextGlyphInfo {
        uint32_t cluster = 0;
        uint32_t line = 0;
    };

}

#endif // TEXTGLYPHINFO_H
