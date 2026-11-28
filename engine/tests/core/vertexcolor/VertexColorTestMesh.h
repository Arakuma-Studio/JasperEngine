// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Tiny meshes for the split, bake and runtime tests: planes, boxes and single vertices, built
// through MeshView::build.

#include "vertexcolor/VertexColorMesh.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vctest {

    using namespace jasperengine;
    using namespace jasperengine::vertexcolor;

    // the derived tables through MeshView::build
    inline void build(MeshView& m) {
        std::string error;
        EXPECT_TRUE(m.build(&error)) << error;
    }

    inline uint32_t addVertex(MeshView& m, const Vector3& p, const Vector2& uv = Vector2()) {
        m.positions.push_back(p);
        m.uv0.push_back(uv);
        return (uint32_t)m.positions.size() - 1;
    }

    // a size x size plane at height y, (cells x cells) quads, normal +Y (or -Y when `down`)
    inline void addPlane(MeshView& m, float size, int cells, float y, bool down = false) {
        const uint32_t first = (uint32_t)m.positions.size();
        for (int j = 0; j <= cells; j++) {
            for (int i = 0; i <= cells; i++) {
                const float u = (float)i / (float)cells, v = (float)j / (float)cells;
                addVertex(m, Vector3((u - 0.5f) * size, y, (v - 0.5f) * size), Vector2(u, v));
            }
        }
        const uint32_t row = (uint32_t)cells + 1;
        for (uint32_t j = 0; j < (uint32_t)cells; j++) {
            for (uint32_t i = 0; i < (uint32_t)cells; i++) {
                const uint32_t a = first + j * row + i, b = a + 1, c = a + row, d = c + 1;
                // counter-clockwise seen from +Y: a, c, b and b, c, d
                if (!down) {
                    m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
                } else {
                    m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
                }
            }
        }
    }

    // an axis-aligned box with a GPU vertex per face corner (hard normals), outward (or inward) winding
    inline void addBox(MeshView& m, const Vector3& lo, const Vector3& hi, bool inward = false) {
        const Vector3 corner[8] = {
            Vector3(lo.x, lo.y, lo.z), Vector3(hi.x, lo.y, lo.z), Vector3(hi.x, hi.y, lo.z), Vector3(lo.x, hi.y, lo.z),
            Vector3(lo.x, lo.y, hi.z), Vector3(hi.x, lo.y, hi.z), Vector3(hi.x, hi.y, hi.z), Vector3(lo.x, hi.y, hi.z)};
        // each face counter-clockwise seen from outside
        const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
        for (const auto& face : faces) {
            const uint32_t base = (uint32_t)m.positions.size();
            for (int k = 0; k < 4; k++) addVertex(m, corner[face[k]]);
            if (!inward) m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            else m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        }
    }

}
