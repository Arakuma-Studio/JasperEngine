// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sprite packer (docs/specs/S44-aseprite-import.md R52, the exact contract in §6.10): frames
// of one or more members (an Aseprite file in S44, an atlas entry in S45) go into RGBA8 pages with
// trimming, duplicate sharing, MaxRects Best Short Side Fit, padding, extrusion, Keep Members
// Together, power-of-two/square/block rounding and several pages. Pure and deterministic: no
// project, GPU or globals, safe on any thread, and the same input gives the same layout and page
// bytes on every machine, whatever order the members come in (packing follows their paths).
//
// Coordinates are pixels, origin top-left, y down. A frame's cell in a page is its trimmed rect
// grown by `extrude` on every side plus `padding` to its right and bottom; with blockAlign the
// cell's origin and size are multiples of 4, so no 4x4 block holds two frames. Frames are assigned
// to pages in maxSize bins; each page is then laid out again in the smallest of a fixed, ordered
// list of smaller bins that holds the same frames, so a small sheet gets a small page.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::sprite {

    struct PackSettings {
        int padding = 1;          // pixels right/bottom of each frame; pages keep it from their top and left edges too
        int extrude = 1;          // edge pixels repeated around each frame (written last)
        int maxSize = 2048;       // max page side
        bool trim = true;         // trim fully transparent (alpha 0) borders
        bool keepMembersTogether = true;
        bool powerOfTwo = false;  // page sides rounded up to powers of two
        bool square = false;      // page square
        bool blockAlign = false;  // block-compressed formats: frame cells and page sides rounded up to multiples of 4
    };

    struct PackInputFrame {
        std::string name;
        std::shared_ptr<const pixelart::PixelImage> image;   // the full (untrimmed) frame; null reads as 0x0
        bool noTrim = false;      // frames with a 9-slice border are never trimmed
    };

    struct PackMember {
        std::string path;
        std::vector<PackInputFrame> frames;
    };

    struct PackedFrame {
        int page = 0;
        pixelart::IntRect rect;        // the frame's pixels inside the page (trimmed size; excludes padding/extrusion); {0,0,0,0} for a fully transparent trimmed frame
        pixelart::IntSize sourceSize;  // the untrimmed frame size
        pixelart::IntPoint offset;     // where rect's pixels sit inside the source frame (trim offset)
        // flat index (sheet order) of the identical frame whose rect this one shares; -1 if none.
        // The shared frame is the first in packing order (members by path, frames in order),
        // which is the first in sheet order whenever the members come sorted by path.
        int duplicateOf = -1;
    };

    struct PackLayout {                // the pure layout: function of (sizes in order, settings)
        std::vector<pixelart::IntSize> pages;          // final page sizes (after shrink + POT/square/4 rounding)
        std::vector<PackedFrame> frames;               // flat, in sheet order: members in input order, frames in order
    };

    struct PackResult {
        PackLayout layout;
        std::vector<pixelart::PixelImage> pages;       // RGBA8 page images
        double usage = 0;                              // used pixel area / total page area (trimmed rects only)
        // "too_large" when a frame (plus 2*extrude + padding) cannot fit maxSize, "invalid_settings"
        // for a negative padding or extrusion or a max size out of [1, PixelImage::kMaxSide]; empty on success
        pixelart::PixelProblem error;
        bool ok() const { return error.empty(); }
    };

    // Trims, finds duplicates, lays out (MaxRects BSSF) and draws the pages.
    PackResult pack(const std::vector<PackMember>& members, const PackSettings& settings);

    // The layout only, from prepared rectangles (what pack() calls; callers cache by its inputs).
    // Items of one member share `member`; an item with an empty size takes no space (page 0,
    // position {0, 0}).
    struct LayoutInput {
        int member = 0;
        std::string memberPath;
        std::string name;
        pixelart::IntSize size;   // trimmed size
    };

    // positionOut is the top-left of each item's pixels (inside its extrusion). False with *error
    // set (and the outputs cleared) on too_large or invalid_settings.
    bool layout(const std::vector<LayoutInput>& items, const PackSettings& settings, std::vector<int>& pageOut,
                std::vector<pixelart::IntPoint>& positionOut, std::vector<pixelart::IntSize>& pageSizes,
                pixelart::PixelProblem* error);

    constexpr int kPackerVersion = 1;   // part of import cache keys: bump when the output of the same input changes

}
