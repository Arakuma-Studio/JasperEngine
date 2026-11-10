// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R33, R47, §9 "EngineData variety")
// - Purpose: a text layer's EngineData (the PostScript-like dictionary inside `TySh`'s text
//   descriptor) as JSON, and the first style run the importer uses: text, font PostScript name,
//   size, fill colour, tracking, leading and paragraph alignment. Written from the format's
//   grammar (`<< /Key value >>`, `[ ... ]`, `(UTF-16BE string)`, numbers, true / false, /names).
// - Untrusted input: bounded nesting (64) and length; a malformed file fails with the offset.
// - Thread-safety: pure functions.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    bool parseEngineData(const std::vector<uint8_t>& bytes, titleart::Json& out, std::string& error);

    struct PsTextStyle {
        std::string text;               // newlines as '\n' (EngineData's '\r')
        std::string font;               // PostScript name of the first run's font
        double fontSize = 0;            // points, before the layer transform
        bool hasColor = false;
        double color[3] = {0, 0, 0};    // sRGB 0..1
        double tracking = 0;            // 1/1000 em
        double leading = 0;             // points; 0 = auto
        int justification = 0;          // 0 left, 1 right, 2 center (3.. justified)
        size_t runs = 1;                // style runs in the text
    };
    // the first style run's attributes; false when the dictionary has no text
    bool firstTextStyle(const titleart::Json& engineData, PsTextStyle& out);

}
