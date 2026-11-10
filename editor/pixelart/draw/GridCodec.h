// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The text grids of the sprite tools (docs/specs/S47-agent-sprite-drawing.md R6-R8): what
// read_sprite_pixels, the edit_sprite echo and diff_sprite return, and what the paste op takes.
//
//   chars   one character per palette index: '.' = 0, '1'-'9' = 1-9, 'a'-'z' = 10-35,
//           'A'-'Z' = 36-61 (each index is its own digit; '0' reads as 0 too)
//   hex2    two lowercase hex digits per index, 00-ff (palettes past 62 entries)
//   legend  RGBA: the chars alphabet without '.', given in order of first appearance, with a
//           legend {char: "#rrggbbaa"}; '.' is any pixel of alpha 0
//   hex8    eight hex digits per RGBA pixel (past 61 colours in a region)
//
// Keep (paste only): '-' and ' ' in chars and legend, "--" in hex2, "--------" in hex8.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"

#include "json.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    enum class GridEncoding : uint8_t { Auto, Chars, Hex2, Legend, Hex8 };

    inline constexpr int kMaxAlphabetIndex = 61;
    inline constexpr int kMaxGridRows = 4096;
    inline constexpr int kMaxGridRowPixels = 4096;

    const char* gridEncodingName(GridEncoding encoding);
    bool parseGridEncoding(std::string_view text, GridEncoding& out);

    // '.', '1'..'9', 'a'..'z', 'A'..'Z' for 0..61; '?' past it
    char alphabetChar(int index);
    // 0..61 for the alphabet ('0' and '.' both 0), -1 for anything else
    int alphabetIndex(char c);
    // "'.' or '0' = 0, 1-9, a-z = 10-35, A-Z = 36-61, '-' or space keeps"
    std::string alphabetHint(GridEncoding encoding);

    struct EncodedGrid {
        GridEncoding encoding = GridEncoding::Chars;
        std::vector<std::string> rows;
        // chars/hex2 of an indexed image: {"5": "#3b5dc9"} for the indices present (the clear
        // index left out); legend: {char: "#rrggbbaa"}; hex8: empty
        nlohmann::json colors = nlohmann::json::object();
        bool indexed() const { return encoding == GridEncoding::Chars || encoding == GridEncoding::Hex2; }
    };

    // The rect of an image (clipped to it by the caller) as rows. Indexed images (with a plane)
    // read as chars/hex2 by default, RGBA ones as legend/hex8; Chars on an index past 61 becomes
    // Hex2, Legend past 61 colours becomes Hex8, and an indexed encoding of an RGBA image reads as
    // Legend/Hex8. `palette` (RGBA, 4 bytes an entry) names the indices in `colors`; `clearIndex`
    // (-1 none) is left out of it.
    EncodedGrid encodeRegion(const PixelImage& image, const IntRect& rect, GridEncoding want, std::span<const uint8_t> palette, int clearIndex);

    // A decoded paste patch: w x h, rows top first; keep[i] leaves that pixel as it is
    struct PixelPatch {
        int w = 0, h = 0;
        bool indexed = false;                 // values are palette indices (chars/hex2), else RGBA
        std::vector<uint8_t> keep;
        std::vector<uint8_t> index;
        std::vector<PixelRgba> rgba;
    };

    // Rows to a patch. Auto is Legend when a legend is given, else Chars on an indexed sprite and
    // Hex8 on an RGBA one. Short or missing rows keep the rest. False with bad_grid_char (row,
    // column, the valid characters), grid_too_wide or region_too_large (rows past 4,096).
    bool decodeRows(std::span<const std::string> rows, GridEncoding encoding, const nlohmann::json& legend, bool spriteIndexed,
                    PixelPatch& out, PixelProblem& problem);

}
