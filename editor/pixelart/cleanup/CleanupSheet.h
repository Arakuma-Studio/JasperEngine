// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/sheet.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Animation sheets (docs/specs/S46-pixel-art-cleanup.md R19-R21): an image generator's sheet of
// frames (a painted grid, a box around each frame, labels under them) is cleared of its
// background, cut into one box per frame and grouped into animations, one per row. Every frame
// is then fixed on one cell size (each at its own offset, since generators draw every frame on
// its own) and placed on one canvas so its anchor (the feet, by default) stays still.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    inline SheetFrame frameOf(const FrameBox& box) { return SheetFrame{box, true, {0, 0}}; }
    // an animation of these frames, exported, at the default speed, looping
    SheetAnimation animationOf(const std::string& name, std::vector<int> frames);

    // Boxes in reading order: row by row, left to right
    std::vector<FrameBox> readingOrder(const std::vector<FrameBox>& boxes);
    // One animation per row ("animation-1", ...), a row across two side-by-side blocks split where
    // they meet
    std::vector<SheetAnimation> groupFrames(const std::vector<FrameBox>& boxes);

    // The sheet edits, which keep every frame in exactly one animation
    Sheet withFrames(const Sheet& sheet, const std::vector<SheetFrame>& frames);
    Sheet insertFrame(const Sheet& sheet, int index, const SheetFrame& frame);
    Sheet removeFrame(const Sheet& sheet, int index);
    Sheet splitAnimation(const Sheet& sheet, int animation, int at);
    Sheet joinAnimation(const Sheet& sheet, int animation);
    // letters, digits, "-", "_" and ".", as games look an animation up
    bool isAnimationName(std::string_view name);
    Sheet renameAnimation(const Sheet& sheet, int animation, const std::string& name);
    struct AnimationChange {
        std::optional<bool> include;
        std::optional<double> fps;
        std::optional<Loop> loop;
    };
    Sheet setAnimation(const Sheet& sheet, int animation, const AnimationChange& change);

    // ---- clearing the background

    // The colours covering much of the border (a painted grid gives two), most common first, at most 4
    std::vector<Rgba8> sheetBackground(const RgbaImage& image);
    // How far (OKLab) the fill strays from itself on the border: the 95th percentile
    double fillNoise(const RgbaImage& image, Rgba8 fill);
    struct Unlined {
        RgbaImage image;
        std::vector<Rgba8> colors;     // the colours still to clear (the line colours gone)
    };
    // Paints a painted grid's lines in the fill where they run over the background, never the art
    Unlined removeGridLines(const RgbaImage& image, const std::vector<Rgba8>& colors, double tolerance);
    struct Cleared {
        RgbaImage image;
        std::vector<uint8_t> mask;     // 1: cleared
    };
    // The background flooded from the border through colours within `tolerance` (OKLab), made transparent
    Cleared clearSheet(const RgbaImage& image, const std::vector<Rgba8>& colors, double tolerance);

    // ---- finding the frames

    // One box per frame of a cleared sheet, in reading order; boxes reach halfway into the gaps
    std::vector<FrameBox> findFrames(const RgbaImage& image);
    // The part of a picture inside a box, clamped to the picture
    RgbaImage cropImage(const RgbaImage& image, const FrameBox& box);

    // ---- anchors and one canvas

    // The largest 4-connected group of opaque pixels (indices into art.indices)
    std::vector<int> bodyOf(const PixelArt& art);
    // The body's lowest row (feet, ground) or middle row (center) at its average column; nullopt
    // for Box and for an empty frame
    std::optional<Point> anchorOf(const PixelArt& art, Anchor anchor);
    // One palette for many pictures: every colour any uses, once, in the order first met
    std::vector<PixelArt> sharePalette(const std::vector<PixelArt>& arts);

    struct Placed {
        PixelArt art;
        Point anchor;                  // where the frame's anchor is on the canvas
    };
    struct Layout {
        std::vector<Placed> frames;
        int width = 0, height = 0;
    };
    // Every frame on the smallest canvas holding their opaque pixels (plus padding), each frame's
    // (0, 0) at offsets[i] from the anchor spot
    Layout layOut(const std::vector<PixelArt>& arts, const std::vector<Point>& offsets, int padding);
    // start to end, or there and back without repeating the ends
    std::vector<int> playOrder(int count, Loop loop);
    // At least two boxes, three in four of them alike in mass and width
    bool looksAnimated(const RgbaImage& image, const std::vector<FrameBox>& boxes);

}
