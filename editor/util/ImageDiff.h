// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.11 diff_images, §6.12 golden, AC38)
// - Purpose: one image comparison shared by diff_images, check_title's golden and
//   fallback_parity, and render_title's compare, with the same maths as
//   tools/mcp/pngdiff.py's `premultiplied=True` so tests and tools agree.
// - Maths: both images as 8-bit RGBA; in premultiplied mode each colour channel is
//   round(c * a / 255) first (colour under alpha 0 never counts), straight mode compares the
//   stored channels. Per pixel the difference is the largest channel difference (alpha
//   included); `max` is the largest over the image, `mean` the mean absolute channel difference
//   (codes, over every channel of every pixel), `fraction_over` the share of pixels whose
//   difference is over `tolerance`, `changed_pct` the percentage of pixels that differ at all,
//   `bbox` the box of the pixels over `tolerance` (image pixels). ok: equal sizes, max <=
//   max_diff, mean <= max_mean and fraction_over <= max_fraction.
// - Thread-safety: pure functions.

#pragma once

#include "texsynth/SynthImage.h"

#include <optional>
#include <string>

namespace jasperengine::editor {

    struct ImageDiffRect {
        int x = 0, y = 0, width = 0, height = 0;
        bool empty() const { return width <= 0 || height <= 0; }
    };

    struct ImageDiffOptions {
        bool premultiplied = true;
        int tolerance = 8;
        double maxFraction = 0.001;
        double maxMean = 0.25;
        int maxDiff = 48;
        std::optional<ImageDiffRect> crop;      // compare only this box (image pixels)
    };

    struct ImageDiffResult {
        bool ok = false;
        bool sameSize = false;
        int widthA = 0, heightA = 0, widthB = 0, heightB = 0;
        int max = 0;
        double mean = 0.0;
        double fractionOver = 0.0;
        double changedPct = 0.0;
        ImageDiffRect bbox;
        std::string error;                      // the sizes when they differ
    };

    // 1-4 channels, 8 or 16 bits: to RGBA8 (16-bit rounded, grey replicated, opaque without alpha)
    texsynth::Image toRgba8Image(const texsynth::Image& image);
    ImageDiffResult diffImages(const texsynth::Image& a, const texsynth::Image& b, const ImageDiffOptions& options = {});
    // the three-panel image: a, b, and the difference x8 in red over faint luminance of a
    texsynth::Image diffPanel(const texsynth::Image& a, const texsynth::Image& b, const ImageDiffOptions& options = {});
    // the difference image alone (what save_diff writes)
    texsynth::Image diffImage(const texsynth::Image& a, const texsynth::Image& b, const ImageDiffOptions& options = {});

}
