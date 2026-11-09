// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_BITMAPFONTPARSERS_H
#define PIXELART_BITMAPFONTPARSERS_H

// The three sources of a bitmap font (S51 R2-R4), each turned into a RawFont: a grid sheet, an
// AngelCode BMFont file (text, XML or binary v3; the parser is ours) and a pixel TrueType font
// rasterised at its native size without antialiasing. Plain C++ over stb_truetype and tinyxml2.

#include "pixelart/BitmapFont.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {
    class FontFace;
}

namespace jasperengine::pixelart {

    // ---- grid (R2)

    struct GridOptions {
        int cellWidth = 0, cellHeight = 0;      // 0: detect (file name, then gutters)
        int marginX = 0, marginY = 0;
        int gapX = 0, gapY = 0;
        std::vector<uint32_t> chars;            // row-major, used when rows is empty
        std::vector<std::vector<uint32_t>> rows;
        std::string imagePath;                  // its name may carry @WxH
    };

    // "hud@6x9.png" -> 6, 9
    JASPERENGINE_API bool cellFromFileName(const std::string& path, int& width, int& height);
    // fully transparent columns and rows at a regular period: the cell (period minus the gutter,
    // cells starting at 0 and no gutter after the last). False when no period fits
    JASPERENGINE_API bool detectGridCell(const uint8_t* rgba, int width, int height, int& cellWidth, int& cellHeight, int& gapX, int& gapY);
    JASPERENGINE_API bool parseGrid(const uint8_t* rgba, int width, int height, const GridOptions& options, RawFont& out,
                                    std::vector<FontError>& errors);

    // ---- BMFont (R3)

    struct BMFontChar {
        uint32_t id = 0;
        int x = 0, y = 0, width = 0, height = 0;
        int xoffset = 0, yoffset = 0, xadvance = 0;
        int page = 0;
        int chnl = 15;
    };

    struct BMFontFile {
        enum class Encoding { TEXT, XML, BINARY };
        Encoding encoding = Encoding::TEXT;
        int size = 0;
        int lineHeight = 0;
        int base = 0;
        int scaleW = 0, scaleH = 0;
        bool packed = false;
        std::vector<std::string> pages;         // by id, relative to the .fnt
        std::vector<BMFontChar> chars;
        std::vector<BitmapFontSettings::Kern> kerning;
    };

    // detected by content: "BMF" + 3 binary, "<" XML, else text
    JASPERENGINE_API bool parseBMFont(const std::vector<uint8_t>& bytes, BMFontFile& out, std::vector<FontError>& errors);
    // the chars cut from their pages (RGBA, `pages[i]` the decoded image of page i)
    struct DecodedPage {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    JASPERENGINE_API bool bmfontToRaw(const BMFontFile& file, const std::vector<DecodedPage>& pages, RawFont& out,
                                      std::vector<FontError>& errors);

    // ---- TrueType (R4)

    struct TrueTypeOptions {
        int pixelSize = 0;                      // 0: detect
        float threshold = 0.5f;
        std::vector<uint32_t> chars;
    };

    // R4: the largest unit u with at least 90 % of the ASCII outline coordinates multiples of it,
    // pixel size = unitsPerEm / u in 5..64; 0 when none, with the best candidates
    JASPERENGINE_API int detectPixelSize(const FontFace& face, std::vector<int>* candidates = nullptr);
    JASPERENGINE_API bool rasterTrueType(const FontFace& face, const TrueTypeOptions& options, RawFont& out,
                                         std::vector<FontError>& errors);

    // PNG (or any stb_image format) bytes to RGBA8
    JASPERENGINE_API bool decodeImage(const std::vector<uint8_t>& bytes, DecodedPage& out);

}

#endif //PIXELART_BITMAPFONTPARSERS_H
