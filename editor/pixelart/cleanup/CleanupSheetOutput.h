// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/spritesheet.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Writing animations out (docs/specs/S46-pixel-art-cleanup.md R23): a PNG sheet, each animation
// from a new row, with Aseprite's json-hash JSON beside it, or one `.aseprite` through S44's
// document, with one tag per animation. The file writes are the session's (R31): nothing here
// touches the disk.

#pragma once

#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/cleanup/CleanupAnimation.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    struct SheetLayout {
        int columns = 1;              // frames per row; an animation's frame count (or more) makes a strip
        int spacing = 0;              // empty pixels between frames and between animations
    };

    struct AtlasRect {
        int clip = 0, frame = 0;
        int x = 0, y = 0, w = 0, h = 0;
        bool operator==(const AtlasRect& other) const = default;
    };

    struct Atlas {
        PixelArt art;
        std::vector<AtlasRect> rects;
    };

    // Every animation's frames on one picture, each animation from a new row. Frames share one palette.
    Atlas atlasOf(const std::vector<AtlasClip>& clips, const SheetLayout& layout);

    struct SpritesheetInfo {
        std::string image;            // the sheet picture's file name, as the JSON refers to it
        const std::vector<AtlasClip>* clips = nullptr;
        const Atlas* atlas = nullptr;
        int scale = 1;                // each pixel of the picture written this many times
        nlohmann::json recipe;        // written as meta.jasperengine.recipe unless null (R31)
    };

    // Aseprite's json-hash: `frames` keyed "<animation>-<n>" (n from 0) in play order, with `frame`,
    // `duration`; `animations` (Pixi's); `meta` with `app` "jasperengine", `size`, `frameTags`.
    // Text, not a json value: the frames' order is what frameTags index, and a json object would
    // sort "walk-10" before "walk-2".
    std::string spritesheetJson(const SpritesheetInfo& info);

    // R23's `png`: one frame per column and 1 px spacing
    Atlas pngSheet(const std::vector<AtlasClip>& clips);

    // R23's `aseprite`: indexed when the shared palette has at most 255 colours (index 0
    // transparent), else RGBA; one layer "Cleanup", one frame per placed frame, durations from
    // the clips, one tag per named clip. The canvas is the largest clip's size, clips top-left.
    aseprite::Document asepriteDocument(const std::vector<AtlasClip>& clips);

}
