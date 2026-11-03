// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The resolver of the texture synthesis (internal to editor/texsynth: TextureSynthesis.cpp
// prepares the job from a Request). A port of upstream's lib/src/ms.rs with a deterministic
// batch scheme, a uniform-grid k-NN instead of the R*-tree grid, and no per-pixel allocation.

#include "TextureSynthesis.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace jasperengine::editor::texsynth::detail {

    // The blur pyramid upstream builds (img_pyramid.rs): level i of L is the image scaled down by
    // 2^(L-1-i) and back up, the last level the image itself; the factor stops where a side would
    // drop under 2 pixels (upstream crashed on large backtrack_stages). Only the scaled-down images
    // are kept: the resolver uses one level at a time and scales it back up when it gets there
    // (level()), the same resize as storing every level at full size (byte-identical results) at a
    // fraction of the memory (an 8192 x 8192 example with 10 levels: ~0.4 GB instead of 2.7 GB).
    struct Pyramid {
        Image full;                   // RGBA8, the image itself (the last level)
        std::vector<Image> reduced;   // per level: the image scaled down, empty where the factor is 1
        bool fullHasAlpha = false;    // some alpha of `full` is not 255

        bool empty() const { return reduced.empty(); }
        int levels() const { return static_cast<int>(reduced.size()); }
        // false (out incomplete) when `cancel` is set during the build
        static bool build(Image rgba, int levels, Pyramid& out, const std::atomic<bool>* cancel = nullptr);
        // Level i at full resolution: `full`, or reduced[i] scaled back up into `scratch`
        const Image& level(int i, Image& scratch) const;
    };

    struct CoreExample {
        int width = 0;
        int height = 0;
        Pyramid levels;                   // RGBA8 blur pyramid, coarsest first, last = the example
        Pyramid guideLevels;              // RGBA8, same levels (empty without guides)
        std::vector<uint8_t> allowed;     // width * height, 1 = may be copied (empty: all)
        bool sampleable = true;
    };

    struct CoreJob {
        int width = 0;
        int height = 0;
        bool tiling = false;
        std::vector<CoreExample> examples;
        Pyramid targetGuideLevels;              // RGBA8 at the output size (empty without guides)
        // width * height (or empty): 1 = kept, copied from examples[0] at the same position and
        // never re-resolved (Inpaint, MakeTileable; examples[0] has the output size then)
        std::vector<uint8_t> locked;
        int pyramidLevels = 1;
        Params params;
        uint64_t seed = 0;
        int threads = 1;
    };

    struct CoreResult {
        std::vector<SourceCoord> coords;
        std::vector<float> uncertainty;
        Image rgba;                       // the final colours (examples' full-resolution RGBA8)
        std::vector<double> stageSeconds;
    };

    // false: cancelled (out is incomplete). Exceptions (std::bad_alloc) propagate, also from the
    // worker threads; fewer workers when threads can't be created (the output never depends on it).
    bool runSynthesis(const CoreJob& job, const Control& control, CoreResult& out);

}
