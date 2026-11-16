// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Tiny meshes for the vertex color tests, with MeshView's derived tables filled here by brute
// force: the file, split, bake and runtime tests check their own code, not MeshView::build.

#include "vertexcolor/VertexColorMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vctest {

    using namespace jasperengine;
    using namespace jasperengine::vertexcolor;

    // logical vertices (exact position match), CSR tables, face normals and areas, logical normals
    // (area-weighted corner normals) and edge neighbours, as R4 / VertexColorMesh.h describe them
    inline void deriveByHand(MeshView& m) {
        const uint32_t n = (uint32_t)m.positions.size();
        const uint32_t triangles = (uint32_t)(m.indices.size() / 3);
        m.bounds = AABB();
        for (const Vector3& p : m.positions) m.bounds.merge(p);

        m.logical.assign(n, 0);
        m.logicalPositions.clear();
        for (uint32_t v = 0; v < n; v++) {
            uint32_t found = UINT32_MAX;
            for (uint32_t l = 0; l < m.logicalPositions.size(); l++) {
                if (m.logicalPositions[l] == m.positions[v]) {
                    found = l;
                    break;
                }
            }
            if (found == UINT32_MAX) {
                found = (uint32_t)m.logicalPositions.size();
                m.logicalPositions.push_back(m.positions[v]);
            }
            m.logical[v] = found;
        }
        const uint32_t logicalCount = (uint32_t)m.logicalPositions.size();
        m.memberStart.assign(logicalCount + 1, 0);
        m.members.clear();
        for (uint32_t l = 0; l < logicalCount; l++) {
            for (uint32_t v = 0; v < n; v++)
                if (m.logical[v] == l) m.members.push_back(v);
            m.memberStart[l + 1] = (uint32_t)m.members.size();
        }
        m.vertexCornerStart.assign(n + 1, 0);
        m.vertexCorners.clear();
        for (uint32_t v = 0; v < n; v++) {
            for (uint32_t c = 0; c < m.indices.size(); c++)
                if (m.indices[c] == v) m.vertexCorners.push_back(c);
            m.vertexCornerStart[v + 1] = (uint32_t)m.vertexCorners.size();
        }

        m.faceNormals.assign(triangles, Vector3());
        m.faceAreas.assign(triangles, 0.0f);
        std::vector<Vector3> sums(logicalCount, Vector3());
        std::vector<std::vector<uint32_t>> adjacent(logicalCount);
        for (uint32_t t = 0; t < triangles; t++) {
            const uint32_t a = m.indices[t * 3], b = m.indices[t * 3 + 1], c = m.indices[t * 3 + 2];
            const Vector3 cross = (m.positions[b] - m.positions[a]).crossProduct(m.positions[c] - m.positions[a]);
            const float length = std::sqrt(cross.squaredLength());
            m.faceAreas[t] = 0.5f * length;
            m.faceNormals[t] = length > 0.0f ? cross / length : Vector3();
            const uint32_t corners[3] = {a, b, c};
            for (uint32_t k = 0; k < 3; k++) {
                const Vector3 normal = m.normals.empty() ? m.faceNormals[t] : m.normals[corners[k]];
                sums[m.logical[corners[k]]] += normal * m.faceAreas[t];
                const uint32_t la = m.logical[corners[k]], lb = m.logical[corners[(k + 1) % 3]];
                if (la != lb) {
                    adjacent[la].push_back(lb);
                    adjacent[lb].push_back(la);
                }
            }
        }
        m.logicalNormals.assign(logicalCount, Vector3());
        m.neighborStart.assign(logicalCount + 1, 0);
        m.neighbors.clear();
        for (uint32_t l = 0; l < logicalCount; l++) {
            const float length = std::sqrt(sums[l].squaredLength());
            m.logicalNormals[l] = length > 0.0f ? sums[l] / length : Vector3();
            std::sort(adjacent[l].begin(), adjacent[l].end());
            adjacent[l].erase(std::unique(adjacent[l].begin(), adjacent[l].end()), adjacent[l].end());
            m.neighbors.insert(m.neighbors.end(), adjacent[l].begin(), adjacent[l].end());
            m.neighborStart[l + 1] = (uint32_t)m.neighbors.size();
        }
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
