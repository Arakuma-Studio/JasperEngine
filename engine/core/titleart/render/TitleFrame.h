// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R39, §6.3 "Frame space and the origin pitfall")
// - Purpose: the one place that maps document space (canvas pixels, origin at the centre, +y up)
//   to frame pixels, and the region-of-interest alignment.
// - Frame space: every title-art target is addressed in memory rows on every backend (the fluid
//   convention, engine/shaderlib/fluid.frag): gl_FragCoord.xy is (column, row) with row 0 the
//   first memory row, which is the top row of the PNG after readback. So x_px = (x + W/2) * s -
//   region.x and y_px = (H/2 - y) * s - region.y, where W x H is the canvas, s the px_scale and
//   region the output-pixel rectangle the target covers. No pass ever flips; only the editor's
//   display of a target does. Pachislop formulas that use a direction convert through
//   ta_dir() in titleart/render/common.glsl, never by flipping a target.
// - Thread-safety: values.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::titleart {

    // ROIs are aligned outward to this grid from the region origin (blur pyramids keep one texel
    // grid whatever the region, Pachislop's ALIGN)
    constexpr int TITLE_ROI_ALIGN = 64;

    struct TitleFrame {
        int canvasWidth = 0;
        int canvasHeight = 0;
        double pxScale = 1.0;           // output pixels per canvas pixel
        TitleRectI region;              // output pixels of the scaled canvas the target covers (image space)

        // a frame for the whole canvas at `scale`
        static TitleFrame canvas(int width, int height, double scale) {
            TitleFrame frame;
            frame.canvasWidth = width;
            frame.canvasHeight = height;
            frame.pxScale = scale;
            frame.region = {0, 0, (int)std::ceil(width * scale), (int)std::ceil(height * scale)};
            return frame;
        }

        // document point -> frame pixel (x right, y down from the region's top-left)
        TitleVec2 toFrame(const TitleVec2& p) const {
            return {(p.x + canvasWidth * 0.5) * pxScale - region.x, (canvasHeight * 0.5 - p.y) * pxScale - region.y};
        }
        // frame pixel -> document point
        TitleVec2 toDocument(const TitleVec2& px) const {
            return {(px.x + region.x) / pxScale - canvasWidth * 0.5, canvasHeight * 0.5 - (px.y + region.y) / pxScale};
        }
        // the frame pixels covering a document rectangle, rounded outward
        TitleRectI toFrameRect(const TitleRect& r) const {
            const TitleVec2 topLeft = toFrame(TitleVec2{r.x, r.y + r.height});
            const TitleVec2 bottomRight = toFrame(TitleVec2{r.x + r.width, r.y});
            const int x0 = (int)std::floor(topLeft.x), y0 = (int)std::floor(topLeft.y);
            const int x1 = (int)std::ceil(bottomRight.x), y1 = (int)std::ceil(bottomRight.y);
            return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
        }
        // a canvas-pixel rectangle (image space, top-left origin) in frame pixels, rounded outward
        TitleRectI canvasToFrame(const TitleRectI& r) const {
            const int x0 = (int)std::floor(r.x * pxScale) - region.x, y0 = (int)std::floor(r.y * pxScale) - region.y;
            const int x1 = (int)std::ceil((r.x + r.width) * pxScale) - region.x, y1 = (int)std::ceil((r.y + r.height) * pxScale) - region.y;
            return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
        }
    };

    inline TitleRectI intersectRect(const TitleRectI& a, const TitleRectI& b) {
        const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
        const int x1 = std::min(a.x + a.width, b.x + b.width), y1 = std::min(a.y + a.height, b.y + b.height);
        if (x1 <= x0 || y1 <= y0) return {};
        return {x0, y0, x1 - x0, y1 - y0};
    }

    inline TitleRectI unionRect(const TitleRectI& a, const TitleRectI& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        const int x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    // A node's region of interest (R13): `bounds` (frame pixels) aligned outward to `align` from the
    // region origin (0, 0 of the frame), then intersected with the frame's extent; empty when it
    // misses the frame (the node is skipped: 0 passes)
    inline TitleRectI alignRegionOfInterest(const TitleRectI& bounds, const TitleFrame& frame, int align = TITLE_ROI_ALIGN) {
        if (bounds.empty()) return {};
        const auto floorTo = [align](int v) { return v >= 0 ? v / align * align : -((-v + align - 1) / align) * align; };
        const auto ceilTo = [&](int v) { return -floorTo(-v); };
        const int x0 = floorTo(bounds.x), y0 = floorTo(bounds.y);
        const int x1 = ceilTo(bounds.x + bounds.width), y1 = ceilTo(bounds.y + bounds.height);
        return intersectRect({x0, y0, x1 - x0, y1 - y0}, {0, 0, frame.region.width, frame.region.height});
    }

}
