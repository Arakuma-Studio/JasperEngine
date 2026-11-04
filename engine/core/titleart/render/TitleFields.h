// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R27, R43, §6.7 "Blur loops")
// - Purpose: the plans and pass set-ups the jump-flood distance fields and the Gaussian blurs
//   share between the planner's recorded steps and the effect context's immediate helpers, so
//   both draw the same passes.
// - Flooding (R27, R43): for a reach R the steps run 2^(k-1), ..., 2, 1 and one more 1 (JFA+1),
//   k = ceil(log2(R + 2)): the flood reaches 2^k - 1 >= R + 1 px in k + 1 passes; an unbounded
//   field (R <= 0) takes the frame's longer side for R, Pachislop's count.
// - Blur: sigma = radius / 3; below 0.3 a copy; else the pyramid level l <= 5 where sigma / 2^l
//   <= 6, reached by bilinear halvings, a separable pass of at most 24 taps a side there, and a
//   bilinear step back up.
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/render/TitleGpu.h"

#include <utility>
#include <vector>

namespace jasperengine::titleart {

    JASPERENGINE_API std::vector<float> floodSteps(float reachPx, int width, int height);

    struct TitleBlurPlan {
        bool copy = false;                          // sigma < 0.3: no blur
        int levels = 0;                             // pyramid halvings
        float sigma = 0.0f;                         // at the smallest level, in its texels
        std::vector<std::pair<int, int>> sizes;     // the levels' sizes, largest first
    };
    JASPERENGINE_API TitleBlurPlan blurPlan(float radiusPx, int width, int height);

    // the whole of `src` resampled over a target of toW x toH (a pyramid step), x opacity
    void setupResample(TitleGpuPass& pass, TextureRender* src, int toW, int toH, float opacity = 1.0f);
    // one separable Gaussian pass over a same-sized target
    void setupBlur(TitleGpuPass& pass, TextureRender* src, int width, int height, bool horizontal, float sigma);
    // seeds of a coverage (red, element frames) or a layer target's alpha at `threshold`
    void setupSeed(TitleGpuPass& pass, TextureRender* src, bool alpha, float threshold, const std::array<float, 4>& seedsOut);
    void setupFlood(TitleGpuPass& pass, TextureRender* seeds, float step, const std::array<float, 4>& seedsEncoding);
    void setupResolve(TitleGpuPass& pass, TextureRender* seeds, TextureRender* coverage, bool alpha, float reachPx, float threshold,
                      const std::array<float, 4>& seedsEncoding, const std::array<float, 4>& distanceOut);

}
