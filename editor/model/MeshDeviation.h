// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// How far a simplified mesh moved from its source (spec S53 R16's "Differs by at most"): the
// largest distance from points sampled on the source's triangles (their corners and centres) to
// the nearest triangle of the simplified index list, both over the same vertex positions. A
// measured number, independent of the simplifier's own error (which mixes in normal and uv terms).

#pragma once

#include <cstdint>
#include <vector>

namespace jasperengine::editor {

    class MeshDeviation {
    public:
        // positions: xyz per vertex; source, simplified: triangle lists over them. 0 when either
        // list is empty.
        static double surfaceDeviation(const std::vector<float>& positions, const std::vector<uint32_t>& source,
                                       const std::vector<uint32_t>& simplified, size_t maxSamples = 4096);
    };

}
