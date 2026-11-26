// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexColorMesh.h"

#include "vertexcolor/VertexColorInternal.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::vertexcolor {

namespace {

    // the weld hash grid's cell of a GPU vertex
    struct MeshWeldEntry {
        int32_t cx, cy, cz;
        uint32_t vertex;
    };

    bool meshWeldCellLess(const MeshWeldEntry& a, int32_t x, int32_t y, int32_t z) {
        if (a.cx != x) return a.cx < x;
        if (a.cy != y) return a.cy < y;
        return a.cz < z;
    }

    int32_t meshWeldCell(float offset, float cell) {
        const float c = std::floor(offset / cell);
        if (c < -2.0e9f) return -2000000000;
        if (c > 2.0e9f) return 2000000000;
        return (int32_t)c;
    }

    // CSR from (key, value) pairs packed as key << 32 | value, sorted and unique
    void meshCsrFromPairs(std::vector<uint64_t>& pairs, uint32_t keyCount, std::vector<uint32_t>& start, std::vector<uint32_t>& items) {
        std::sort(pairs.begin(), pairs.end());
        pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
        start.assign((size_t)keyCount + 1, 0);
        for (uint64_t pair : pairs) start[(size_t)(pair >> 32) + 1]++;
        for (uint32_t k = 0; k < keyCount; k++) start[k + 1] += start[k];
        items.resize(pairs.size());
        for (size_t i = 0; i < pairs.size(); i++) items[i] = (uint32_t)(pairs[i] & 0xffffffffu);
    }

    bool meshFail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

}

bool MeshView::build(std::string* error) {
    logical.clear();
    memberStart.clear();
    members.clear();
    vertexCornerStart.clear();
    vertexCorners.clear();
    neighborStart.clear();
    neighbors.clear();
    faceNeighborStart.clear();
    faceNeighbors.clear();
    faceNormals.clear();
    faceAreas.clear();
    logicalPositions.clear();
    logicalNormals.clear();
    bounds = AABB();
    gridStart.clear();
    gridItems.clear();
    gridDim[0] = gridDim[1] = gridDim[2] = 0;

    const uint32_t vertexTotal = vertexCount();
    if (indices.size() % 3 != 0) return meshFail(error, "the index count " + std::to_string(indices.size()) + " is not a multiple of 3");
    if (!normals.empty() && normals.size() != vertexTotal) return meshFail(error, "normals has " + std::to_string(normals.size()) + " entries for " + std::to_string(vertexTotal) + " vertices");
    if (!uv0.empty() && uv0.size() != vertexTotal) return meshFail(error, "uv0 has " + std::to_string(uv0.size()) + " entries for " + std::to_string(vertexTotal) + " vertices");
    if (!uv1.empty() && uv1.size() != vertexTotal) return meshFail(error, "uv1 has " + std::to_string(uv1.size()) + " entries for " + std::to_string(vertexTotal) + " vertices");
    if (!assetColors.empty() && assetColors.size() != vertexTotal) return meshFail(error, "assetColors has " + std::to_string(assetColors.size()) + " entries for " + std::to_string(vertexTotal) + " vertices");
    for (size_t i = 0; i < indices.size(); i++) {
        if (indices[i] >= vertexTotal) return meshFail(error, "index " + std::to_string(i) + " is " + std::to_string(indices[i]) + ", past the " + std::to_string(vertexTotal) + " vertices");
    }
    for (uint32_t v = 0; v < vertexTotal; v++) {
        const Vector3& p = positions[v];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return meshFail(error, "position " + std::to_string(v) + " is not finite");
    }
    if (vertexTotal == 0) return true;

    Vector3 lo = positions[0], hi = positions[0];
    for (const Vector3& p : positions) {
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
    }
    bounds.setExtents(lo, hi);
    const float diagonal = detail::length3(hi - lo);

    // ---- R4: weld by position. Each vertex joins the lowest representative within the tolerance
    // (representatives are the first vertex of each logical vertex, in vertex order), so the
    // grouping does not depend on any container order.
    const float tolerance = 1e-6f * diagonal;
    const float tolerance2 = tolerance * tolerance;
    const float cell = tolerance > 0.0f ? 2.0f * tolerance : 1.0f;
    std::vector<MeshWeldEntry> entries(vertexTotal);
    for (uint32_t v = 0; v < vertexTotal; v++) {
        const Vector3 offset = positions[v] - lo;
        entries[v] = MeshWeldEntry{meshWeldCell(offset.x, cell), meshWeldCell(offset.y, cell), meshWeldCell(offset.z, cell), v};
    }
    std::vector<MeshWeldEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const MeshWeldEntry& a, const MeshWeldEntry& b) {
        if (a.cx != b.cx) return a.cx < b.cx;
        if (a.cy != b.cy) return a.cy < b.cy;
        if (a.cz != b.cz) return a.cz < b.cz;
        return a.vertex < b.vertex;
    });
    logical.assign(vertexTotal, 0);
    std::vector<uint8_t> representative(vertexTotal, 0);
    uint32_t logicalTotal = 0;
    for (uint32_t v = 0; v < vertexTotal; v++) {
        const MeshWeldEntry& self = entries[v];
        uint32_t best = UINT32_MAX;
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -1; dz <= 1; dz++) {
                    const int32_t x = self.cx + dx, y = self.cy + dy, z = self.cz + dz;
                    auto it = std::lower_bound(sorted.begin(), sorted.end(), 0, [&](const MeshWeldEntry& e, int) { return meshWeldCellLess(e, x, y, z); });
                    for (; it != sorted.end() && it->cx == x && it->cy == y && it->cz == z; ++it) {
                        if (it->vertex >= v || it->vertex >= best) break;
                        if (!representative[it->vertex]) continue;
                        const Vector3 d = positions[it->vertex] - positions[v];
                        if (detail::dot3(d, d) <= tolerance2) {
                            best = it->vertex;
                            break;
                        }
                    }
                }
            }
        }
        if (best == UINT32_MAX) {
            representative[v] = 1;
            logical[v] = logicalTotal++;
        } else {
            logical[v] = logical[best];
        }
    }

    memberStart.assign((size_t)logicalTotal + 1, 0);
    for (uint32_t v = 0; v < vertexTotal; v++) memberStart[logical[v] + 1]++;
    for (uint32_t l = 0; l < logicalTotal; l++) memberStart[l + 1] += memberStart[l];
    members.resize(vertexTotal);
    {
        std::vector<uint32_t> fill(memberStart.begin(), memberStart.end() - 1);
        for (uint32_t v = 0; v < vertexTotal; v++) members[fill[logical[v]]++] = v;
    }

    const uint32_t cornerTotal = (uint32_t)indices.size();
    vertexCornerStart.assign((size_t)vertexTotal + 1, 0);
    for (uint32_t c = 0; c < cornerTotal; c++) vertexCornerStart[indices[c] + 1]++;
    for (uint32_t v = 0; v < vertexTotal; v++) vertexCornerStart[v + 1] += vertexCornerStart[v];
    vertexCorners.resize(cornerTotal);
    {
        std::vector<uint32_t> fill(vertexCornerStart.begin(), vertexCornerStart.end() - 1);
        for (uint32_t c = 0; c < cornerTotal; c++) vertexCorners[fill[indices[c]]++] = c;
    }

    const uint32_t triangleTotal = triangleCount();
    // ---- edge adjacency through logical vertices: seams do not cut it
    {
        std::vector<uint64_t> pairs;
        pairs.reserve((size_t)triangleTotal * 6);
        for (uint32_t t = 0; t < triangleTotal; t++) {
            for (int k = 0; k < 3; k++) {
                const uint32_t a = logical[indices[t * 3 + k]];
                const uint32_t b = logical[indices[t * 3 + (k + 1) % 3]];
                if (a == b) continue;
                pairs.push_back(((uint64_t)a << 32) | b);
                pairs.push_back(((uint64_t)b << 32) | a);
            }
        }
        meshCsrFromPairs(pairs, logicalTotal, neighborStart, neighbors);
    }
    {
        struct EdgeFace {
            uint64_t edge;
            uint32_t triangle;
        };
        std::vector<EdgeFace> edges;
        edges.reserve((size_t)triangleTotal * 3);
        for (uint32_t t = 0; t < triangleTotal; t++) {
            for (int k = 0; k < 3; k++) {
                const uint32_t a = logical[indices[t * 3 + k]];
                const uint32_t b = logical[indices[t * 3 + (k + 1) % 3]];
                if (a == b) continue;
                edges.push_back(EdgeFace{((uint64_t)std::min(a, b) << 32) | std::max(a, b), t});
            }
        }
        std::sort(edges.begin(), edges.end(), [](const EdgeFace& x, const EdgeFace& y) {
            return x.edge != y.edge ? x.edge < y.edge : x.triangle < y.triangle;
        });
        std::vector<uint64_t> pairs;
        for (size_t i = 0; i < edges.size();) {
            size_t j = i;
            while (j < edges.size() && edges[j].edge == edges[i].edge) j++;
            for (size_t a = i; a < j; a++) {
                for (size_t b = i; b < j; b++) {
                    if (edges[a].triangle != edges[b].triangle) pairs.push_back(((uint64_t)edges[a].triangle << 32) | edges[b].triangle);
                }
            }
            i = j;
        }
        meshCsrFromPairs(pairs, triangleTotal, faceNeighborStart, faceNeighbors);
    }

    faceNormals.resize(triangleTotal);
    faceAreas.resize(triangleTotal);
    for (uint32_t t = 0; t < triangleTotal; t++) {
        const Vector3& p0 = positions[indices[t * 3]];
        const Vector3& p1 = positions[indices[t * 3 + 1]];
        const Vector3& p2 = positions[indices[t * 3 + 2]];
        const Vector3 n = detail::cross3(p1 - p0, p2 - p0);
        const float len = detail::length3(n);
        faceAreas[t] = 0.5f * len;
        faceNormals[t] = len > 0.0f ? Vector3(n.x / len, n.y / len, n.z / len) : Vector3(0.0f, 0.0f, 0.0f);
    }

    // each corner's angle, so a quad's normal counts the same whichever way it was triangulated
    std::vector<float> cornerAngle(cornerTotal);
    for (uint32_t t = 0; t < triangleTotal; t++) {
        for (uint32_t k = 0; k < 3; k++) {
            const Vector3& p = positions[indices[t * 3 + k]];
            const Vector3 a = positions[indices[t * 3 + (k + 1) % 3]] - p;
            const Vector3 b = positions[indices[t * 3 + (k + 2) % 3]] - p;
            cornerAngle[t * 3 + k] = (float)detail::detAtan2(detail::length3(detail::cross3(a, b)), detail::dot3(a, b));
        }
    }
    logicalPositions.resize(logicalTotal);
    logicalNormals.resize(logicalTotal);
    for (uint32_t l = 0; l < logicalTotal; l++) {
        logicalPositions[l] = positions[members[memberStart[l]]];
        Vector3 sum(0.0f, 0.0f, 0.0f);
        Vector3 unweighted(0.0f, 0.0f, 0.0f);
        for (uint32_t m = memberStart[l]; m < memberStart[l + 1]; m++) {
            const uint32_t v = members[m];
            for (uint32_t k = vertexCornerStart[v]; k < vertexCornerStart[v + 1]; k++) {
                const uint32_t c = vertexCorners[k];
                sum += (normals.empty() ? faceNormals[c / 3] : normals[v]) * cornerAngle[c];
            }
            if (!normals.empty()) unweighted += normals[v];
        }
        Vector3 n = detail::normalize3(sum);
        if (n == Vector3(0.0f, 0.0f, 0.0f)) n = detail::normalize3(unweighted);
        logicalNormals[l] = n;
    }

    buildGrid();
    return true;
}

void MeshView::buildGrid() {
    gridStart.clear();
    gridItems.clear();
    gridDim[0] = gridDim[1] = gridDim[2] = 0;
    const uint32_t count = logicalCount();
    if (count == 0) return;
    const Vector3 lo = bounds.getMinimum();
    const Vector3 extent = bounds.getMaximum() - lo;
    const float diagonal = detail::length3(extent);
    // about two logical vertices per cell; the cell size only changes which candidates a query
    // tests, never its answer, so it may use libm
    const float target = std::max(1.0f, (float)count / 2.0f);
    float product = 1.0f;
    int axes = 0;
    for (int a = 0; a < 3; a++) {
        if (extent[a] > diagonal * 1e-6f) {
            product *= extent[a];
            axes++;
        }
    }
    float size = axes == 0 ? 1.0f : std::pow(product / target, 1.0f / (float)axes);
    if (!(size > 0.0f) || !std::isfinite(size)) size = diagonal > 0.0f ? diagonal : 1.0f;
    const uint64_t cellLimit = std::max<uint64_t>(64, (uint64_t)count * 4);
    for (;;) {
        uint64_t cells = 1;
        for (int a = 0; a < 3; a++) {
            const float n = std::ceil(extent[a] / size);
            gridDim[a] = (uint32_t)std::max(1.0f, std::min(1024.0f, n));
            cells *= gridDim[a];
        }
        if (cells <= cellLimit) break;
        size *= 1.25f;
    }
    gridCell = size;
    gridOrigin = lo;
    const uint32_t cells = gridDim[0] * gridDim[1] * gridDim[2];
    std::vector<uint32_t> cellOf(count);
    gridStart.assign((size_t)cells + 1, 0);
    for (uint32_t l = 0; l < count; l++) {
        const Vector3 offset = logicalPositions[l] - gridOrigin;
        uint32_t c[3];
        for (int a = 0; a < 3; a++) {
            const float f = std::floor(offset[a] / gridCell);
            c[a] = f <= 0.0f ? 0u : std::min(gridDim[a] - 1, (uint32_t)f);
        }
        cellOf[l] = (c[2] * gridDim[1] + c[1]) * gridDim[0] + c[0];
        gridStart[cellOf[l] + 1]++;
    }
    for (uint32_t c = 0; c < cells; c++) gridStart[c + 1] += gridStart[c];
    gridItems.resize(count);
    std::vector<uint32_t> fill(gridStart.begin(), gridStart.end() - 1);
    for (uint32_t l = 0; l < count; l++) gridItems[fill[cellOf[l]]++] = l;
}

void MeshView::queryLogical(const Vector3& center, float radius, std::vector<uint32_t>& out) const {
    if (gridDim[0] == 0 || !(radius >= 0.0f)) return;
    const size_t first = out.size();
    const float radius2 = radius * radius;
    uint32_t lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        const float minC = std::floor((center[a] - radius - gridOrigin[a]) / gridCell);
        const float maxC = std::floor((center[a] + radius - gridOrigin[a]) / gridCell);
        if (maxC < 0.0f || minC > (float)(gridDim[a] - 1)) return;
        lo[a] = minC <= 0.0f ? 0u : (uint32_t)minC;
        hi[a] = std::min(gridDim[a] - 1, maxC <= 0.0f ? 0u : (uint32_t)maxC);
    }
    for (uint32_t z = lo[2]; z <= hi[2]; z++) {
        for (uint32_t y = lo[1]; y <= hi[1]; y++) {
            for (uint32_t x = lo[0]; x <= hi[0]; x++) {
                const uint32_t cellIndex = (z * gridDim[1] + y) * gridDim[0] + x;
                for (uint32_t i = gridStart[cellIndex]; i < gridStart[cellIndex + 1]; i++) {
                    const uint32_t l = gridItems[i];
                    const Vector3 d = logicalPositions[l] - center;
                    if (detail::dot3(d, d) <= radius2) out.push_back(l);
                }
            }
        }
    }
    std::sort(out.begin() + (std::ptrdiff_t)first, out.end());
}

void MeshView::queryLogicalBox(const AABB& box, std::vector<uint32_t>& out) const {
    if (gridDim[0] == 0 || box.isNull()) return;
    const size_t first = out.size();
    const Vector3& bmin = box.getMinimum();
    const Vector3& bmax = box.getMaximum();
    uint32_t lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        const float minC = std::floor((bmin[a] - gridOrigin[a]) / gridCell);
        const float maxC = std::floor((bmax[a] - gridOrigin[a]) / gridCell);
        if (maxC < 0.0f || minC > (float)(gridDim[a] - 1)) return;
        lo[a] = minC <= 0.0f ? 0u : (uint32_t)minC;
        hi[a] = std::min(gridDim[a] - 1, maxC <= 0.0f ? 0u : (uint32_t)maxC);
    }
    for (uint32_t z = lo[2]; z <= hi[2]; z++) {
        for (uint32_t y = lo[1]; y <= hi[1]; y++) {
            for (uint32_t x = lo[0]; x <= hi[0]; x++) {
                const uint32_t cellIndex = (z * gridDim[1] + y) * gridDim[0] + x;
                for (uint32_t i = gridStart[cellIndex]; i < gridStart[cellIndex + 1]; i++) {
                    const uint32_t l = gridItems[i];
                    const Vector3& p = logicalPositions[l];
                    if (p.x >= bmin.x && p.x <= bmax.x && p.y >= bmin.y && p.y <= bmax.y && p.z >= bmin.z && p.z <= bmax.z) out.push_back(l);
                }
            }
        }
    }
    std::sort(out.begin() + (std::ptrdiff_t)first, out.end());
}

Fingerprint MeshView::fingerprint() const {
    Fingerprint fp;
    fp.vertexCount = vertexCount();
    fp.indexCount = (uint32_t)indices.size();
    uint64_t topology = detail::kFnvOffset;
    for (uint32_t index : indices) topology = detail::fnvU32(topology, index);
    fp.topologyHash = topology;
    uint64_t position = detail::kFnvOffset;
    if (!positions.empty()) {
        Vector3 lo = positions[0], hi = positions[0];
        for (const Vector3& p : positions) {
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }
        const float step = 1e-4f * detail::length3(hi - lo);
        for (const Vector3& p : positions) {
            for (int a = 0; a < 3; a++) {
                const float q = step > 0.0f ? std::floor((p[a] - lo[a]) / step + 0.5f) : 0.0f;
                position = detail::fnvU32(position, (uint32_t)(int32_t)std::max(-2.0e9f, std::min(2.0e9f, q)));
            }
        }
    }
    fp.positionHash = position;
    return fp;
}

Vector4 valueAt(const TargetData& data, const MeshView& mesh, uint32_t triangle, const Vector3& barycentric) {
    if (triangle >= mesh.triangleCount()) return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 a = data.cornerValue(triangle * 3, mesh.indices);
    const Vector4 b = data.cornerValue(triangle * 3 + 1, mesh.indices);
    const Vector4 c = data.cornerValue(triangle * 3 + 2, mesh.indices);
    return Vector4(a.x * barycentric.x + b.x * barycentric.y + c.x * barycentric.z,
                   a.y * barycentric.x + b.y * barycentric.y + c.y * barycentric.z,
                   a.z * barycentric.x + b.z * barycentric.y + c.z * barycentric.z,
                   a.w * barycentric.x + b.w * barycentric.y + c.w * barycentric.z);
}

}
