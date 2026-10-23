// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The resolver of the texture synthesis (internal to editor/texsynth: TextureSynthesis.cpp
// prepares the job from a Request). A port of upstream's lib/src/ms.rs with a deterministic
// batch scheme, a uniform-grid k-NN instead of the R*-tree grid, and no per-pixel allocation.

#include "TextureSynthesis.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor::texsynth::detail {

    struct CoreExample {
        int width = 0;
        int height = 0;
        std::vector<Image> levels;        // RGBA8 blur pyramid, coarsest first, last = the example
        std::vector<Image> guideLevels;   // RGBA8, same levels (empty without guides)
        std::vector<uint8_t> allowed;     // width * height, 1 = may be copied (empty: all)
        bool sampleable = true;
    };

    struct CoreJob {
        int width = 0;
        int height = 0;
        bool tiling = false;
        std::vector<CoreExample> examples;
        std::vector<Image> targetGuideLevels;   // RGBA8 at the output size (empty without guides)
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

    // false: cancelled (out is incomplete)
    bool runSynthesis(const CoreJob& job, const Control& control, CoreResult& out);

    // The stage pixel counts: stage i of S + 1 resolves round-down(p^(S - i) * total), the last
    // one all of them (exposed for the tests)
    std::vector<int64_t> stagePixelCounts(int64_t total, const Params& params);

    // Exact k nearest resolved pixels on a uniform grid of point lists (exposed for the tests).
    // With tiling, distances wrap around the output and the neighbours come back as offsets that
    // may point outside it (the caller wraps them).
    class KnnGrid {
    public:
        struct Neighbor {
            int dx;
            int dy;
            int d2;
        };

        void init(int width, int height, bool tiling, int cellSize);
        void insert(int x, int y);
        int64_t size() const { return count; }
        // The (up to) k nearest inserted points to (qx, qy), ascending by distance, ties by (dy,
        // dx); `keys` is scratch space (reused, grows as needed). Returns how many.
        int query(int qx, int qy, int k, std::vector<uint64_t>& keys, Neighbor* out) const;

    private:
        int width = 0, height = 0, cell = 1, gridW = 0, gridH = 0, slack = 0;
        bool tiling = false;
        int64_t count = 0;
        std::vector<uint32_t> points;   // per cell slots: cell i holds points[start[i], start[i] + used[i])
        std::vector<uint32_t> start;
        std::vector<uint32_t> used;
    };

    // The same k nearest for dense point sets: offsets within a disc, sorted by the k-NN key, tried
    // against a bitmap of the points; the first k present are the k nearest. -1 when the disc
    // holds fewer than k (the caller asks the grid then).
    class DenseKnn {
    public:
        explicit DenseKnn(int radius = 40);
        int query(const uint8_t* present, int width, int height, bool tiling, int qx, int qy, int k, KnnGrid::Neighbor* out) const;

    private:
        std::vector<KnnGrid::Neighbor> offsets;
    };

}
