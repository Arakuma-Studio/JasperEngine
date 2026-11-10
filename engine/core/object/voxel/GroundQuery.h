// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_GROUND_QUERY_H
#define VOXEL_GROUND_QUERY_H

// T6 R45: for callers that want the ground wherever it is. T1's query stays terrain-only (D5); this sits
// above it: the terrain when not a hole, else a downward ray on any volume over that column. Always
// compiled: with the module off it is the plain terrain query.

#include "Export.h"
#include "math/Vector3.h"

namespace jasperengine {

    class Scene;

    namespace voxel {

        enum class GroundSource { None, Terrain, Voxel };

        struct GroundSample {
            float height = 0.0f;
            Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
            GroundSource source = GroundSource::None;
        };

        // World x, z; yHint where a downward ray starts
        JASPERENGINE_API GroundSample sampleGround(Scene* scene, float x, float z, float yHint);

    }

}

#endif
