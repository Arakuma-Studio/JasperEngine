// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PICKING_TEST_UTIL_H
#define PICKING_TEST_UTIL_H

#include "Scene.h"
#include "picking/MeshBvh.h"
#include "picking/PickGeometry.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace jasperengine::test {

    // A triangle soup in [-extent, extent]^3: triangles of random size (up to `size`) around
    // random centres, split over `submeshes` submeshes
    inline PickGeometry randomGeometry(uint32_t triangles, uint32_t seed, float extent = 10.0f, float size = 1.0f, uint32_t submeshes = 1){
        PickGeometry geometry;
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> centre(-extent, extent);
        std::uniform_real_distribution<float> offset(-size, size);
        geometry.positions.reserve((size_t)triangles * 3);
        geometry.indices.reserve((size_t)triangles * 3);
        for (uint32_t t = 0; t < triangles; t++){
            const Vector3 c(centre(rng), centre(rng), centre(rng));
            for (int k = 0; k < 3; k++){
                geometry.positions.push_back(c + Vector3(offset(rng), offset(rng), offset(rng)));
                geometry.indices.push_back((uint32_t)geometry.positions.size() - 1);
            }
        }
        for (uint32_t s = 0; s < submeshes; s++){
            geometry.submeshFirstTriangle.push_back((uint32_t)((uint64_t)triangles * s / submeshes));
        }
        geometry.submeshFirstTriangle.push_back(triangles);
        return geometry;
    }

    // a regular grid in the XZ plane (y = 0), counter-clockwise seen from +Y
    inline PickGeometry gridGeometry(uint32_t cells, float size){
        PickGeometry geometry;
        const float step = size / (float)cells;
        for (uint32_t z = 0; z <= cells; z++){
            for (uint32_t x = 0; x <= cells; x++){
                geometry.positions.push_back(Vector3(-size * 0.5f + x * step, 0.0f, -size * 0.5f + z * step));
            }
        }
        const uint32_t row = cells + 1;
        for (uint32_t z = 0; z < cells; z++){
            for (uint32_t x = 0; x < cells; x++){
                const uint32_t a = z * row + x, b = a + 1, c = a + row, d = c + 1;
                geometry.indices.insert(geometry.indices.end(), {a, c, b, b, c, d});
            }
        }
        geometry.submeshFirstTriangle = {0u, cells * cells * 2u};
        return geometry;
    }

    struct TestRay {
        Vector3 origin;
        Vector3 direction;
    };

    // rays from outside a box of half size `extent` toward random points in it
    inline std::vector<TestRay> randomRays(uint32_t count, uint32_t seed, float extent = 10.0f){
        std::vector<TestRay> rays;
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> inside(-extent, extent);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        for (uint32_t i = 0; i < count; i++){
            Vector3 dir(unit(rng), unit(rng), unit(rng));
            if (dir.squaredLength() < 1e-6f){
                dir = Vector3(0, 0, 1);
            }
            dir.normalize();
            const Vector3 target(inside(rng), inside(rng), inside(rng));
            rays.push_back({target - dir * (extent * 4.0f), dir});
        }
        return rays;
    }

    // the transforms and world bounds a frame computes (no view is loaded in CPU suites)
    inline void settleScene(Scene& scene){
        scene.getSystem<MeshSystem>()->update(0);
        scene.getSystem<RenderSystem>()->update(0);
    }

}

#endif // PICKING_TEST_UTIL_H
