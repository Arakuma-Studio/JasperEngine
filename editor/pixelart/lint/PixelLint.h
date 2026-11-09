// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pixel-art lint library (docs/specs/S46-pixel-art-cleanup.md R24-R27, §6.2): one call,
// lint(), runs the catalogue on one or more frames and returns S44's LintReport, the family's one
// result shape. S46 (cleanup verdicts, lint_pixel_art, lint-pixel-art), S47 (edit_sprite {lint})
// and S42 (check_pixel_art's cleanup_outputs rule) call it. It depends on no cleanup, session or
// MCP code (D5).
//
// The catalogue, in report order (lintIds()); "craft" lints never FAIL (R26):
//
// | id                     | measured                          | threshold (default)  | status                                   |
// |------------------------|-----------------------------------|----------------------|------------------------------------------|
// | off_palette            | off pixels (S48's lintOffPalette) | 0 pixels             | FAIL > 0; SKIP without a palette         |
// | colors                 | used colours                      | rules.max_colors     | FAIL above; SKIP without the rule        |
// | partial_alpha          | pixels 0 < a < 255 (S48)          | 0                    | FAIL > 0                                 |
// | transparent_background | opaque border pixels              | half the border      | FAIL (>= 3 corners and > half); SKIP     |
// |                        |                                   |                      | without rules.transparent_background     |
// | size                   | frames of a size not allowed      | 0                    | FAIL; SKIP without rules.sizes           |
// | near_duplicates        | closest pair of used colours      | 0.03 OKLab           | WARN when a pair is closer (count: pairs)|
// | orphans                | orphan pixels                     | 1                    | WARN >= 1; SKIP on sheets                |
// | jaggies                | spots                             | 3 (CRAFT_SPOTS)      | WARN >= 3                                |
// | doubles                | spots                             | 3                    | WARN >= 3                                |
// | holes                  | spots                             | 3                    | WARN >= 3                                |
// | flicker                | share of lit pixels (S48)         | 0.06                 | WARN above; SKIP under 3 frames          |
// | contrast               | worst backdrop's faint share      | 0.5                  | WARN above; SKIP without backdrops       |
// | banding                | pixels in band groups             | 12                   | WARN >= 12                               |
// | pillow_shading         | Spearman rho of the worst body    | 0.6                  | WARN (rho >= 0.6, R^2 <= 0.25, balance   |
// |                        |                                   |                      | <= 0.03); SKIP when no body qualifies    |
// | empty_frame            | empty frames of the animations    | 0                    | FAIL > 0; SKIP without animations        |
// | one_frame              | animations of one frame           | 0                    | WARN > 0; SKIP without animations        |
// | frame_scale            | frames off the shared cell        | 0.05 share           | WARN > 0; SKIP without frame cells       |
//
// options.thresholds[id] replaces a lint's threshold and is reported as "options.<id>"; a rules
// value is reported as "rules.<key>"; the backdrops' origin is LintInput::backdropsSource.
// Frames are numbered from 1 in `at`, x and y from the top-left pixel (S44's LintSpot).

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/Rgba8.h"
#include "pixelart/lint/LintTypes.h"

#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::pixelart {
    struct Palette;
}

namespace jasperengine::editor::pixelart::lint {

    struct LintRules {
        std::optional<int> maxColors;                 // rules.max_colors
        std::vector<std::pair<int, int>> sizes;       // rules.sizes: {width, height} allowed
        bool transparentBackground = false;           // rules.transparent_background
    };

    // An animation of the frames (a sheet's clip, an .aseprite tag): empty_frame and one_frame
    struct LintAnimation {
        std::string name;
        std::vector<int> frames;                      // 0-based indices into LintInput::frames
    };

    struct LintInput {
        std::span<const PixelImage> frames;           // one or more, the same size
        const jasperengine::pixelart::Palette* palette = nullptr;   // nullptr: off_palette SKIPs
        std::vector<jasperengine::pixelart::Rgba8> backdrops;       // empty: contrast SKIPs
        std::string backdropsSource = "options.backdrops";          // reported as contrast's threshold_source
        bool loop = true;                             // flicker wraps from the last frame to the first
        bool sheet = false;                           // an animation sheet: orphans SKIPs (sparks are single pixels)
        LintRules rules;
        std::vector<LintAnimation> animations;        // empty: empty_frame and one_frame SKIP
        // frame_scale: each frame's own detected cell (0: unknown) against the shared one (0: SKIP)
        std::vector<double> frameCells;
        double sharedCell = 0;
    };

    struct LintOptions {
        std::vector<std::string> only;                // empty: every lint (inapplicable ones SKIP)
        std::map<std::string, double> thresholds;     // by lint id, reported as options.<id>
    };

    // Pure and deterministic; any thread. Unknown ids in `only` are the caller's to refuse
    // (isLintId); they are ignored here.
    LintReport lint(const LintInput& input, const LintOptions& options = {});

    // The catalogue's ids, in report order
    std::span<const char* const> lintIds();
    bool isLintId(std::string_view id);
    // "off_palette, colors, ..." for an error's fix
    std::string lintIdList();

}
