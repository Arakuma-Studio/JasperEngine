// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// jaggies, doubles, holes and contrast are ported from ritmo (packages/tools/src/fx/core/craft.ts,
// check.ts), and colors, transparent_background, size, near_duplicates and orphans from its
// pixel/core/lint.ts: MIT License, Copyright (c) 2026 Sandro Maglione.

// The lints of S46's catalogue that S46 owns (docs/specs/S46-pixel-art-cleanup.md R26, R27): the
// rule lints (colors, transparent_background, size), the craft lints (near_duplicates, orphans,
// jaggies, doubles, holes, contrast, banding, pillow_shading; they never FAIL) and the sheet lints
// (empty_frame, one_frame, frame_scale). off_palette, partial_alpha and flicker are S48's
// (EffectLints.h). lint() (PixelLint.h) runs them all; each is also callable alone. Every function
// is pure (frames in, LintResult out) and runs on any thread.
//
// Frames are PixelImage (S44), one pixel per art pixel. "Drawn" is alpha > 0, as in S48's lints
// (ritmo's frames are palette indices, 0 empty). Frames count from 1 in the spots.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/Rgba8.h"
#include "pixelart/lint/EffectLints.h"
#include "pixelart/lint/LintTypes.h"
#include "pixelart/lint/PixelLint.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::pixelart::lint {

    // check.ts CRAFT_SPOTS: a craft rule warns from this many spots (one or two can be a deliberate corner or notch)
    constexpr int CRAFT_SPOTS = 3;

    // A tunable threshold and where its value came from ("default", "options.<id>", "rules.<key>", a setting)
    struct LintThreshold {
        double value = 0.0;
        std::string source = "default";
    };

    // ---- colors: the colours used (distinct RGB of the drawn pixels, every frame together) against
    // rules.max_colors. FAIL above; SKIP without a maximum.
    LintResult lintColors(const FrameView& frames, const LintThreshold* maxColors);

    // ---- transparent_background (lint.ts opaque-background): a frame whose corners are drawn (>= 3
    // of 4) and more than half of whose border is drawn still has its background. measured: the drawn
    // border pixels of the worst frame; threshold: half its border. FAIL; SKIP when not required.
    LintResult lintTransparentBackground(const FrameView& frames, bool required);

    // ---- size: frames whose size is none of `sizes`. measured: those frames; FAIL > 0; SKIP without sizes.
    LintResult lintSize(const FrameView& frames, std::span<const std::pair<int, int>> sizes);

    // ---- near_duplicates (lint.ts near-duplicate-colors): pairs of used colours closer than the
    // threshold (0.03 OKLab). measured: the closest pair's distance; count: the pairs. WARN when a pair
    // is closer; SKIP with fewer than 2 colours.
    LintResult lintNearDuplicates(const FrameView& frames, const LintThreshold& threshold);

    // ---- orphans (lint.ts stray-pixel): a drawn pixel none of whose 4 neighbours has its colour,
    // while 5 or more of its 8 neighbours share one other colour (empty counts as a colour, as
    // ritmo's TRANSPARENT index does). measured: those pixels; WARN at >= the threshold (1). SKIP on
    // animation sheets, where sparks are single pixels on purpose (report.ts).
    LintResult lintOrphans(const FrameView& frames, bool sheet, const LintThreshold& threshold);

    // ---- jaggies (craft.ts): along every edge chain of the silhouette (top, bottom, left and right
    // edges), a run at least 2 shorter or longer than both runs beside it (3, 1, 3 or 1, 3, 1); a
    // chain's end runs are not judged. measured: spots; WARN at >= the threshold (CRAFT_SPOTS).
    LintResult lintJaggies(const FrameView& frames, const LintThreshold& threshold);

    // ---- doubles (craft.ts): a drawn pixel of a shape (8-connected) of >= 5 pixels whose only two
    // 8-neighbours sit at a right angle to it. measured: spots; WARN at >= the threshold.
    LintResult lintDoubles(const FrameView& frames, const LintThreshold& threshold);

    // ---- holes (craft.ts): an empty pixel with drawn pixels on all four sides. measured: spots;
    // WARN at >= the threshold.
    LintResult lintHoles(const FrameView& frames, const LintThreshold& threshold);

    // ---- contrast (craft.ts faintOn): per backdrop, the share of drawn pixels within 0.12 OKLab of
    // it. measured: the worst backdrop's share; WARN above the threshold (0.5); SKIP without backdrops.
    LintResult lintContrast(const FrameView& frames, std::span<const jasperengine::pixelart::Rgba8> backdrops, const LintThreshold& threshold);

    // ---- banding (R27): ramp runs (>= 3 runs of opaque colours along a row, column or diagonal,
    // strictly monotone in OKLab lightness, all of one length <= 2) on >= 3 consecutive scanlines with
    // the same colours, shifted by one constant (0 or +-1) from line to line. measured: the pixels in
    // band groups; count: the groups (their first pixels in `at`). WARN at >= the threshold (12).
    LintResult lintBanding(const FrameView& frames, const LintThreshold& threshold);

    // ---- pillow_shading (R27): on each drawn body (4-connected) of >= 64 pixels with >= 3 distinct
    // lightness levels, Spearman's rho between OKLab L and the 4-connected distance to the outside,
    // the R^2 of the plane L = a + b x + c y and the spread of the edge pixels' mean L over the four
    // quadrants around the centroid. Pillow when rho >= the threshold (0.6), R^2 <= 0.25 and the
    // spread <= 0.03. measured: the rho of the worst body; WARN when a body is pillow-shaded; SKIP
    // when no body is large enough or has enough tones.
    LintResult lintPillowShading(const FrameView& frames, const LintThreshold& threshold);

    // ---- the sheet lints (report.ts). An animation (PixelLint.h) names frames (0-based) of `frames`.
    // empty_frame: a frame of an animation with nothing drawn. FAIL > 0; SKIP without animations.
    LintResult lintEmptyFrame(const FrameView& frames, std::span<const LintAnimation> animations);
    // one_frame: an animation of one frame. WARN > 0; SKIP without animations.
    LintResult lintOneFrame(std::span<const LintAnimation> animations);
    // frame_scale: a frame whose own cell (frameCells[i] > 0) differs from the shared one by more than
    // the threshold share (0.05). WARN > 0; SKIP without a shared cell or frame cells.
    LintResult lintFrameScale(std::span<const double> frameCells, double sharedCell, const LintThreshold& threshold);

}
