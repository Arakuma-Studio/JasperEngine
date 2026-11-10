// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R32, R47, §6.9 Pattern overlay)
// - Purpose: Photoshop style libraries (`.asl`: version 2, '8BSL', the pattern block, then per
//   style a name / id descriptor and a style descriptor whose `Lefx` holds the layer effects) and
//   bitmap patterns (an ASL's pattern block, a PSD's Patt / Pat2 / Pat3), the patterns as straight
//   RGBA so a pattern overlay with no procedural equivalent becomes a `custom` pattern image.
//   Formats from the Adobe Photoshop File Formats Specification; the style-block layout follows
//   Pachislop src/io/psStyle.ts (aslStyles) by SafEight, used with the author's permission.
// - Untrusted input: bounded reads, at most 4,096 styles and patterns, 8,192 px a pattern side.
// - Thread-safety: pure functions.

#pragma once

#include "PsdDescriptor.h"

#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    struct PsPattern {
        std::string name;
        std::string id;                 // the pattern's unique id (styles reference it by `Idnt`)
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;      // straight, 8-bit, rows top first
    };

    struct AslStyle {
        std::string name;
        Json effects;                   // the `Lefx` descriptor (null when the style has none)
        Json style;                     // the whole style descriptor (blending options)
    };

    struct AslFile {
        std::vector<AslStyle> styles;
        std::vector<PsPattern> patterns;
        std::vector<std::string> skipped;   // styles that could not be read, one line each
    };

    bool readAsl(const uint8_t* data, size_t size, AslFile& out, std::string& error);
    // a pattern block (a sequence of length-prefixed patterns); patterns that do not decode are
    // left out with a line in `skipped`
    void readPatterns(const uint8_t* data, size_t size, std::vector<PsPattern>& out, std::vector<std::string>* skipped);

}
