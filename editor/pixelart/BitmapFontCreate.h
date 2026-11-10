// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Create › Bitmap Font (S51 R24): a .bitmapfont over a PNG sheet, a BMFont .fnt or a pixel
// TrueType font, with what detection finds written into the file once (D8: loading never
// detects again, so a saved font never changes when the heuristics improve).
#pragma once

#include <filesystem>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    // what Create › Bitmap Font would write for `source` (absolute): the .bitmapfont's JSON, or
    // false with why (a TrueType font without a pixel grid, a sheet with no cell size)
    bool bitmapFontFor(const std::filesystem::path& source, const std::filesystem::path& assetsRoot, std::string& json, std::string& error);
    // a sheet named like one (*@WxH.png): the cell, for the texture Inspector's banner
    bool looksLikeFontSheet(const std::string& path, int& cellWidth, int& cellHeight);
    // a TTF/OTF whose outlines sit on a pixel grid: its pixel size, else 0
    int detectedPixelFontSize(const std::filesystem::path& source, const std::filesystem::path& assetsRoot);
    // writes <name>.bitmapfont next to the source (a free name), one undo step; its path, or empty
    // with why
    std::filesystem::path createBitmapFont(Project* project, const std::filesystem::path& source, std::string& error);

}
