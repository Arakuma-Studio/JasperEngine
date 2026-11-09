// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// render_sprite's contact sheet (docs/specs/S47-agent-sprite-drawing.md R27, R28): frames at an
// integer scale, nearest only, on one row of panels per backdrop, with onion skin, a pixel grid,
// labels and the last edit outlined. CPU code, any thread, deterministic.

#pragma once

#include "SpriteDoc.h"
#include "SpriteEdit.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    struct SheetBackdrop {
        bool checker = false;
        PixelRgba color{0, 0, 0, 255};
        std::string name;              // "checker", "dark", "light", "#rrggbb"
    };

    struct SheetOptions {
        std::vector<int> frames;       // from 0
        // "visible" (default), "all", or these layers
        bool allLayers = false;
        std::vector<int> layers;
        int scale = 0;                 // 0: the largest that fits maxWidth
        int columns = 0;               // 0: auto, at most 8
        int maxWidth = 1024;
        std::vector<SheetBackdrop> backdrops;
        int onionBefore = 0, onionAfter = 0;
        double onionOpacity = 0.3;
        bool grid = false;
        bool labels = true;
        std::vector<DirtyRect> highlight;   // outlined in magenta in every panel they touch
    };

    // the longest side a sheet may be; render_sprite refuses a scale that passes it
    inline constexpr int kMaxSheetSide = 8192;
    // the largest scale that keeps the sheet within kMaxSheetSide (0: none)
    int largestSheetScale(const SpriteDoc& sprite, const SheetOptions& options);

    // The sheet as RGBA (empty when it would pass kMaxSheetSide). `layout` is {scale, layout: [{frame (from 1), backdrop, x, y, w, h}]}.
    PixelImage renderSheet(const SpriteDoc& sprite, const SheetOptions& options, nlohmann::json& layout);

    // A sheet as a PNG, quick and deterministic: palette PNG when it has at most 256 colours
    // (sheets of pixel art nearly always do), else RGBA; fast deflate (R38)
    std::string encodeSheetPng(const PixelImage& image);

    // Two frames side by side (a diff's image), the changes' bounds outlined; `frames` from 0 of each
    PixelImage renderDiffImage(const PixelImage& before, const PixelImage& after, const IntRect& bbox, int maxWidth);

}
