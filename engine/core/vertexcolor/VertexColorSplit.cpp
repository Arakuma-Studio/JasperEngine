// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R11-R13: split vertices for targets with corner overrides, the LOD
// rule, and the value bytes of the color stream

#include "vertexcolor/VertexColorSplit.h"

#include "vertexcolor/VertexColorInternal.h"

#include "texture/TexturePrepare.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::vertexcolor {

    namespace {

        bool splitSameValue(const Vector4& a, const Vector4& b) {
            return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
        }

        Vector3 splitFaceNormal(const std::vector<Vector3>& positions, uint32_t a, uint32_t b, uint32_t c) {
            return detail::normalize3(detail::cross3(positions[b] - positions[a], positions[c] - positions[a]));
        }

        // The output vertices: originals first, then one copy per distinct (vertex, value) pair in the
        // order they are first needed (corner order of LOD0, then each LOD list), so the same data
        // always gives the same buffers (R13 pooling)
        class SplitBuilder {
        public:
            SplitBuilder(SplitResult& result, uint32_t vertexCount): result(result), originals(vertexCount), firstCopy(vertexCount, -1) {}

            uint32_t vertexFor(uint32_t vertex, const Vector4& value) {
                if (splitSameValue(result.values[vertex], value)) return vertex;
                for (int32_t copy = firstCopy[vertex]; copy >= 0; copy = nextCopy[(size_t)copy]) {
                    const uint32_t output = originals + (uint32_t)copy;
                    if (splitSameValue(result.values[output], value)) return output;
                }
                const uint32_t output = (uint32_t)result.values.size();
                result.values.push_back(value);
                result.sourceVertex.push_back(vertex);
                // copies of one vertex chain newest first; the scan above is over a handful of values
                nextCopy.push_back(firstCopy[vertex]);
                firstCopy[vertex] = (int32_t)(nextCopy.size() - 1);
                result.added++;
                return output;
            }

        private:
            SplitResult& result;
            uint32_t originals;
            std::vector<int32_t> firstCopy;   // per GPU vertex: its newest copy (index into nextCopy), -1
            std::vector<int32_t> nextCopy;    // per copy: the previous copy of the same vertex, -1
        };

    }

    SplitResult buildSplit(const TargetData& data, const MeshView& mesh, std::span<const std::vector<uint32_t>> lodIndexLists) {
        SplitResult result;
        const uint32_t vertexCount = mesh.vertexCount();
        // a vertex past the stored base (a target not initialized yet) draws white (R6's "never
        // transparent black by accident")
        result.values.assign(vertexCount, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        std::copy_n(data.base.begin(), std::min<size_t>(data.base.size(), vertexCount), result.values.begin());
        result.sourceVertex.resize(vertexCount);
        for (uint32_t v = 0; v < vertexCount; v++) result.sourceVertex[v] = v;
        if (data.corners.empty()) return result;

        const std::vector<uint32_t>& indices = mesh.indices;
        const uint32_t cornerCount = (uint32_t)(indices.size() / 3 * 3);
        const uint32_t triangleCount = cornerCount / 3;

        // the value of every LOD0 corner (overrides are sorted by corner)
        std::vector<Vector4> cornerValues(cornerCount);
        std::vector<uint8_t> vertexOverridden(vertexCount, 0);
        size_t next = 0;
        for (uint32_t c = 0; c < cornerCount; c++) {
            const uint32_t v = indices[c];
            while (next < data.corners.size() && data.corners[next].corner < c) next++;
            const bool overridden = next < data.corners.size() && data.corners[next].corner == c && v < vertexCount;
            cornerValues[c] = overridden ? data.corners[next].value : (v < vertexCount ? result.values[v] : Vector4(1.0f, 1.0f, 1.0f, 1.0f));
            if (overridden && !splitSameValue(cornerValues[c], result.values[v])) vertexOverridden[v] = 1;
        }

        // overrides that all equal their base (none should) leave the shared geometry
        if (std::find(vertexOverridden.begin(), vertexOverridden.end(), (uint8_t)1) == vertexOverridden.end()) return result;

        result.split = true;
        SplitBuilder builder(result, vertexCount);
        result.indices.resize(cornerCount);
        for (uint32_t t = 0; t < triangleCount; t++) {
            const uint32_t* tri = &indices[(size_t)t * 3];
            // a corner past the vertices makes the triangle degenerate (PickGeometry's rule)
            if (tri[0] >= vertexCount || tri[1] >= vertexCount || tri[2] >= vertexCount) {
                for (int k = 0; k < 3; k++) result.indices[(size_t)t * 3 + (size_t)k] = 0;
                continue;
            }
            for (uint32_t k = 0; k < 3; k++) {
                const uint32_t c = t * 3 + k;
                result.indices[c] = builder.vertexFor(indices[c], cornerValues[c]);
            }
        }

        result.lodIndices.resize(lodIndexLists.size());
        if (lodIndexLists.empty()) return result;

        // GPU vertex -> its LOD0 corners (CSR, ascending corner, so ascending triangle)
        std::vector<uint32_t> cornerStart(vertexCount + 1, 0);
        for (uint32_t c = 0; c < cornerCount; c++)
            if (indices[c] < vertexCount) cornerStart[indices[c] + 1]++;
        for (uint32_t v = 0; v < vertexCount; v++) cornerStart[v + 1] += cornerStart[v];
        std::vector<uint32_t> fill(cornerStart.begin(), cornerStart.end() - 1);
        std::vector<uint32_t> corners(cornerStart[vertexCount]);
        for (uint32_t c = 0; c < cornerCount; c++)
            if (indices[c] < vertexCount) corners[fill[indices[c]]++] = c;

        std::vector<Vector3> lod0Normals(triangleCount);
        for (uint32_t t = 0; t < triangleCount; t++) {
            const uint32_t* tri = &indices[(size_t)t * 3];
            if (tri[0] < vertexCount && tri[1] < vertexCount && tri[2] < vertexCount)
                lod0Normals[t] = splitFaceNormal(mesh.positions, tri[0], tri[1], tri[2]);
        }

        for (size_t level = 0; level < lodIndexLists.size(); level++) {
            const std::vector<uint32_t>& lod = lodIndexLists[level];
            std::vector<uint32_t>& out = result.lodIndices[level];
            const size_t lodCorners = lod.size() / 3 * 3;
            out.resize(lodCorners);
            for (size_t t = 0; t < lodCorners; t += 3) {
                const uint32_t* tri = &lod[t];
                if (tri[0] >= vertexCount || tri[1] >= vertexCount || tri[2] >= vertexCount) {
                    out[t] = out[t + 1] = out[t + 2] = 0;
                    continue;
                }
                bool needsNormal = false;
                for (int k = 0; k < 3; k++) needsNormal = needsNormal || vertexOverridden[tri[k]];
                const Vector3 normal = needsNormal ? splitFaceNormal(mesh.positions, tri[0], tri[1], tri[2]) : Vector3();
                for (size_t k = 0; k < 3; k++) {
                    const uint32_t v = tri[k];
                    if (!vertexOverridden[v]) {
                        out[t + k] = v;
                        continue;
                    }
                    // R12 LOD rule: v's corner in the LOD0 triangle with the closest face normal, ties
                    // to the lowest triangle (corners ascend, the comparison is strict)
                    Vector4 value = result.values[v];
                    float best = -2.0f;
                    for (uint32_t i = cornerStart[v]; i < cornerStart[v + 1]; i++) {
                        const uint32_t c = corners[i];
                        const float closeness = lod0Normals[c / 3].dotProduct(normal);
                        if (closeness > best) {
                            best = closeness;
                            value = cornerValues[c];
                        }
                    }
                    out[t + k] = builder.vertexFor(v, value);
                }
            }
        }
        return result;
    }

    std::vector<uint8_t> packValues(std::span<const Vector4> values, Format format) {
        std::vector<uint8_t> bytes;
        if (format == Format::RGBA8) {
            bytes.resize(values.size() * 4);
            for (size_t i = 0; i < values.size(); i++) {
                const float c[4] = {values[i].x, values[i].y, values[i].z, values[i].w};
                for (size_t k = 0; k < 4; k++) {
                    float channel = c[k];
                    if (!(channel > 0.0f)) channel = 0.0f;   // NaN too
                    if (channel > 1.0f) channel = 1.0f;
                    bytes[i * 4 + k] = (uint8_t)std::floor(channel * 255.0f + 0.5f);
                }
            }
        } else {
            bytes.resize(values.size() * 8);
            for (size_t i = 0; i < values.size(); i++) {
                const float c[4] = {values[i].x, values[i].y, values[i].z, values[i].w};
                for (size_t k = 0; k < 4; k++) {
                    const uint16_t half = floatToHalf(c[k]);
                    bytes[i * 8 + k * 2] = (uint8_t)half;
                    bytes[i * 8 + k * 2 + 1] = (uint8_t)(half >> 8);
                }
            }
        }
        return bytes;
    }

}
