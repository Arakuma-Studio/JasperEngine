// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R41-R43, R60: the value-field generators. Deterministic on every
// platform (R27): fixed sample patterns, a hashed sequence for the rays, arithmetic and sqrt only on
// the paths that decide values (VertexColorInternal.h's maths, never the contracted engine/core/math
// out-of-line operations), and ties broken by index.

#include "vertexcolor/VertexColorBake.h"

#include "vertexcolor/VertexColorInternal.h"

#include "math/HashRandom.h"
#include "picking/MeshBvh.h"
#include "picking/PickGeometry.h"
#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jasperengine::vertexcolor::bake {

    namespace {

        bool bakeCancelled(const ApplyOptions& options) {
            return options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed);
        }

        void bakeProgress(const ApplyOptions& options, size_t done, size_t total) {
            if (options.progress && total > 0) options.progress((float)((double)done / (double)total));
        }

        // the derived tables of MeshView::build an op reads; false (with the reason) when they are missing
        bool bakeNeedsBuiltMesh(const MeshView& mesh, std::string& error) {
            if (mesh.indices.size() % 3 != 0) {
                error = "the mesh's index count is not a multiple of 3";
                return false;
            }
            const uint32_t vertexCount = mesh.vertexCount();
            for (uint32_t index : mesh.indices) {
                if (index >= vertexCount) {
                    error = "the mesh has an index past its vertices";
                    return false;
                }
            }
            if (!mesh.built() || mesh.logical.size() != vertexCount || mesh.logicalPositions.size() != mesh.logicalCount() ||
                mesh.logicalNormals.size() != mesh.logicalCount()) {
                error = "the mesh view is not built";
                return false;
            }
            return true;
        }

        // OKLab ΔE ×100 (the scale ColorMatch and seamDeltaE use)
        float bakeDeltaE(const pixelart::Lab& a, const pixelart::Lab& b) {
            const float dl = a.l - b.l, da = a.a - b.a, db = a.b - b.b;
            return 100.0f * std::sqrt(dl * dl + da * da + db * db);
        }

        pixelart::Lab bakeLab(const Vector4& v) { return pixelart::linearToOklab(v.x, v.y, v.z); }

        float bakeTriangleArea(const Vector3& a, const Vector3& b, const Vector3& c) {
            return 0.5f * detail::length3(detail::cross3(b - a, c - a));
        }

        // Barycentrics of the samples of corner 0's area: the part of the triangle where corner 0
        // weighs most, the kite corner - mid(01) - centroid - mid(02). Its two halves (corner, mid01,
        // centroid) and (corner, centroid, mid02) are each cut into m x m congruent triangles whose
        // centroids are the samples: stratified, deterministic, equal area per sample
        std::vector<Vector3> bakeCornerPattern(int samples) {
            int m = 1;
            while (2 * m * m < samples) m++;
            const Vector3 corner(1.0f, 0.0f, 0.0f);
            const Vector3 mid01(0.5f, 0.5f, 0.0f);
            const Vector3 mid02(0.5f, 0.0f, 0.5f);
            const Vector3 centroid(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
            const Vector3 halves[2][3] = {{corner, mid01, centroid}, {corner, centroid, mid02}};
            std::vector<Vector3> pattern;
            pattern.reserve((size_t)(2 * m * m));
            const float inv = 1.0f / (float)m;
            for (const auto& half : halves) {
                const Vector3 e1 = half[1] - half[0];
                const Vector3 e2 = half[2] - half[0];
                for (int i = 0; i < m; i++) {
                    for (int j = 0; i + j < m; j++) {
                        // the upward cell and, where there is one, the downward cell next to it
                        const float s0 = ((float)i + 1.0f / 3.0f) * inv, t0 = ((float)j + 1.0f / 3.0f) * inv;
                        pattern.push_back(half[0] + e1 * s0 + e2 * t0);
                        if (i + j < m - 1) {
                            const float s1 = ((float)i + 2.0f / 3.0f) * inv, t1 = ((float)j + 2.0f / 3.0f) * inv;
                            pattern.push_back(half[0] + e1 * s1 + e2 * t1);
                        }
                    }
                }
            }
            return pattern;
        }

        // Duff et al., "Building an Orthonormal Basis, Revisited": arithmetic only, so every platform
        // builds the same frame
        void bakeBasis(const Vector3& n, Vector3& b1, Vector3& b2) {
            const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
            const float a = -1.0f / (sign + n.z);
            const float b = n.x * n.y * a;
            b1 = Vector3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
            b2 = Vector3(b, sign + n.y * n.y * a, -n.y);
        }

        // a ray direction through the model's linear part (not normalized: its length is the scale)
        Vector3 bakeLinear(const detail::Space& space, const Vector3& d) {
            if (space.identity) return d;
            const float* m = space.m;
            return Vector3(m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z, m[2] * d.x + m[6] * d.y + m[10] * d.z);
        }

        // A uniform grid over the transfer source's points (D6) with an exact nearest-point query
        class TransferGrid {
        public:
            explicit TransferGrid(const std::vector<Vector3>& points): points(points) {
                if (points.empty()) return;
                Vector3 lo = points[0], hi = points[0];
                for (const Vector3& p : points) {
                    lo = Vector3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                    hi = Vector3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
                }
                origin = lo;
                const Vector3 extent = hi - lo;
                const float largest = std::max(extent.x, std::max(extent.y, extent.z));
                // about one point per cell over the axes the points spread along; the cell size only
                // changes the speed, never the answer
                cell = 1.0f;
                if (largest > 0.0f) {
                    double measure = 1.0;
                    int axes = 0;
                    for (int a = 0; a < 3; a++) {
                        if (extent[(unsigned)a] > largest * 1e-3f) {
                            measure *= extent[(unsigned)a];
                            axes++;
                        }
                    }
                    cell = (float)detail::detPow(measure / (double)points.size(), 1.0 / (double)axes);
                    cell = std::max(cell, largest / 1024.0f);
                    const uint64_t maxCells = (uint64_t)points.size() * 4u + 64u;
                    for (;;) {
                        uint64_t total = 1;
                        for (int a = 0; a < 3; a++) {
                            dims[a] = (int)std::min(std::floor(extent[(unsigned)a] / cell) + 1.0f, 1024.0f);
                            total *= (uint64_t)dims[a];
                        }
                        if (total <= maxCells) break;
                        cell *= 1.25f;
                    }
                }
                const size_t cells = (size_t)dims[0] * (size_t)dims[1] * (size_t)dims[2];
                start.assign(cells + 1, 0);
                std::vector<uint32_t> cellOf(points.size());
                for (size_t i = 0; i < points.size(); i++) {
                    int c[3];
                    cellCoords(points[i], c);
                    cellOf[i] = (uint32_t)cellIndex(c[0], c[1], c[2]);
                    start[cellOf[i] + 1]++;
                }
                for (size_t c = 0; c < cells; c++) start[c + 1] += start[c];
                std::vector<uint32_t> fill(start.begin(), start.end() - 1);
                items.resize(points.size());
                for (size_t i = 0; i < points.size(); i++) items[fill[cellOf[i]]++] = (uint32_t)i;
            }

            // The nearest point within `limit` that `accept` takes; ties to the lowest index.
            // Rings of cells grow until the unvisited cells are farther than the best point
            template <typename Accept>
            bool nearest(const Vector3& q, float limit, Accept accept, uint32_t& bestIndex, float& bestD2) const {
                if (points.empty() || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return false;
                int c[3];
                cellCoords(q, c);
                const float limit2 = limit * limit;
                bool found = false;
                bestD2 = std::numeric_limits<float>::infinity();
                bestIndex = 0;
                for (int r = 0;; r++) {
                    const int lo[3] = {std::max(c[0] - r, 0), std::max(c[1] - r, 0), std::max(c[2] - r, 0)};
                    const int hi[3] = {std::min(c[0] + r, dims[0] - 1), std::min(c[1] + r, dims[1] - 1), std::min(c[2] + r, dims[2] - 1)};
                    for (int x = lo[0]; x <= hi[0]; x++) {
                        for (int y = lo[1]; y <= hi[1]; y++) {
                            const bool shellXY = std::abs(x - c[0]) == r || std::abs(y - c[1]) == r;
                            for (int z = lo[2]; z <= hi[2]; z++) {
                                if (!shellXY && std::abs(z - c[2]) != r) {
                                    // inside the shell: jump to its far face
                                    if (z < c[2] + r) z = c[2] + r - 1;
                                    continue;
                                }
                                const size_t cellId = cellIndex(x, y, z);
                                for (uint32_t i = start[cellId]; i < start[cellId + 1]; i++) {
                                    const uint32_t index = items[i];
                                    const float d2 = (points[index] - q).squaredLength();
                                    if (d2 > limit2 || d2 > bestD2 || (d2 == bestD2 && index > bestIndex)) continue;
                                    if (!accept(index)) continue;
                                    bestD2 = d2;
                                    bestIndex = index;
                                    found = true;
                                }
                            }
                        }
                    }
                    // the unvisited points are at least `gap` away
                    float gap = std::numeric_limits<float>::infinity();
                    for (int a = 0; a < 3; a++) {
                        if (c[a] + r + 1 <= dims[a] - 1) gap = std::min(gap, origin[(unsigned)a] + (float)(c[a] + r + 1) * cell - q[(unsigned)a]);
                        if (c[a] - r - 1 >= 0) gap = std::min(gap, q[(unsigned)a] - (origin[(unsigned)a] + (float)(c[a] - r) * cell));
                    }
                    if (gap == std::numeric_limits<float>::infinity()) break;   // every cell visited
                    gap = std::max(gap, 0.0f);
                    if (gap > limit) break;
                    if (found && bestD2 <= gap * gap) break;
                }
                return found;
            }

            // every point within `radius` of q, in cell order
            template <typename Visit>
            void within(const Vector3& q, float radius, Visit visit) const {
                if (points.empty() || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return;
                int lo[3], hi[3];
                cellCoords(q - Vector3(radius, radius, radius), lo);
                cellCoords(q + Vector3(radius, radius, radius), hi);
                const float radius2 = radius * radius;
                for (int z = lo[2]; z <= hi[2]; z++) {
                    for (int y = lo[1]; y <= hi[1]; y++) {
                        for (int x = lo[0]; x <= hi[0]; x++) {
                            const size_t cellId = cellIndex(x, y, z);
                            for (uint32_t i = start[cellId]; i < start[cellId + 1]; i++) {
                                if ((points[items[i]] - q).squaredLength() <= radius2) visit(items[i]);
                            }
                        }
                    }
                }
            }

        private:
            void cellCoords(const Vector3& p, int c[3]) const {
                for (int a = 0; a < 3; a++) {
                    const float f = std::floor((p[(unsigned)a] - origin[(unsigned)a]) / cell);
                    c[a] = !(f > 0.0f) ? 0 : f >= (float)(dims[a] - 1) ? dims[a] - 1 : (int)f;
                }
            }
            size_t cellIndex(int x, int y, int z) const {
                return ((size_t)z * (size_t)dims[1] + (size_t)y) * (size_t)dims[0] + (size_t)x;
            }

            const std::vector<Vector3>& points;
            Vector3 origin;
            float cell = 1.0f;
            int dims[3] = {1, 1, 1};
            std::vector<uint32_t> start;
            std::vector<uint32_t> items;
        };

        // a corner's stand-in point: inside its triangle, a quarter of the way to the centroid, so the
        // corners of one vertex on either side of a hard edge are told apart by the face they lie in
        constexpr float kTransferWedge = 0.25f;

        Vector3 transferWedge(const Vector3& corner, const Vector3& b, const Vector3& c) {
            const Vector3 centroid((corner.x + b.x + c.x) / 3.0f, (corner.y + b.y + c.y) / 3.0f, (corner.z + b.z + c.z) / 3.0f);
            return corner + (centroid - corner) * kTransferWedge;
        }

        // squared distance from p to triangle (a, b, c), by the region of its closest point
        float transferTriangleDistance2(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c) {
            const Vector3 ab = b - a, ac = c - a, ap = p - a;
            const float d1 = detail::dot3(ab, ap), d2 = detail::dot3(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f) return ap.squaredLength();
            const Vector3 bp = p - b;
            const float d3 = detail::dot3(ab, bp), d4 = detail::dot3(ac, bp);
            if (d3 >= 0.0f && d4 <= d3) return bp.squaredLength();
            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return (p - (a + ab * (d1 / (d1 - d3)))).squaredLength();
            const Vector3 cp = p - c;
            const float d5 = detail::dot3(ab, cp), d6 = detail::dot3(ac, cp);
            if (d6 >= 0.0f && d5 <= d6) return cp.squaredLength();
            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return (p - (a + ac * (d2 / (d2 - d6)))).squaredLength();
            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return (p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))))).squaredLength();
            const float sum = va + vb + vc;
            if (!(sum > 0.0f)) return ap.squaredLength();   // degenerate
            return (p - (a + ab * (vb / sum) + ac * (vc / sum))).squaredLength();
        }

        // R60 with the source's corners. A vertex's corners choose among the corners of the source
        // vertices at its nearest source vertex's place (within op.maxDistance; seams make several):
        // each takes the one whose triangle is nearest its stand-in point (face normals within the
        // angle when cosAngle > -1, ties to the lower corner). Nothing in reach: its vertex's value.
        bool transferCorners(const MeshView& mesh, const Op& op, const ApplyOptions& options, const TransferSource& source, const TransferGrid& grid,
                             float cosAngle, Field& out, std::string& error) {
            const size_t sourceCorners = source.indices.size();
            const size_t sourceVertices = source.positions.size();
            // source vertex -> its corners (CSR)
            std::vector<uint32_t> cornerStart(sourceVertices + 1, 0);
            for (uint32_t index : source.indices) cornerStart[index + 1]++;
            for (size_t v = 0; v < sourceVertices; v++) cornerStart[v + 1] += cornerStart[v];
            std::vector<uint32_t> cornersOf(sourceCorners);
            {
                std::vector<uint32_t> fill(cornerStart.begin(), cornerStart.end() - 1);
                for (uint32_t c = 0; c < (uint32_t)sourceCorners; c++) cornersOf[fill[source.indices[c]]++] = c;
            }
            std::vector<Vector3> faceNormals(sourceCorners / 3);
            Vector3 lo = source.positions.empty() ? Vector3() : source.positions[0], hi = lo;
            for (const Vector3& p : source.positions) {
                lo = Vector3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                hi = Vector3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
            }
            for (size_t t = 0; t < faceNormals.size(); t++) {
                const Vector3& a = source.positions[source.indices[t * 3]];
                faceNormals[t] = detail::normalize3(detail::cross3(source.positions[source.indices[t * 3 + 1]] - a, source.positions[source.indices[t * 3 + 2]] - a));
            }
            // R4's tolerance: source vertices this close to the nearest one share its place
            const float together = 1e-6f * detail::length3(hi - lo);
            const detail::Space space = detail::Space::of(options.model);

            out.cornerValues.resize(mesh.indices.size());
            for (size_t c = 0; c < mesh.indices.size(); c++) out.cornerValues[c] = out.vertexValues[mesh.indices[c]];
            std::vector<uint32_t> candidates;
            for (uint32_t v = 0; v < mesh.vertexCount(); v++) {
                if ((v & 1023u) == 0 && bakeCancelled(options)) {
                    error = "cancelled";
                    return false;
                }
                if (mesh.vertexCornerStart[v + 1] == mesh.vertexCornerStart[v]) continue;
                const Vector3 p = space.point(mesh.positions[v]);
                uint32_t nearest = 0;
                float d2 = 0.0f;
                if (!grid.nearest(p, op.maxDistance, [](uint32_t) { return true; }, nearest, d2)) continue;
                candidates.clear();
                grid.within(p, std::sqrt(d2) + together, [&](uint32_t s) {
                    for (uint32_t k = cornerStart[s]; k < cornerStart[s + 1]; k++) candidates.push_back(cornersOf[k]);
                });
                std::sort(candidates.begin(), candidates.end());
                for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++) {
                    const uint32_t c = mesh.vertexCorners[k];
                    const uint32_t t = c / 3, at = c % 3;
                    const Vector3 b = space.point(mesh.positions[mesh.indices[t * 3 + (at + 1) % 3]]);
                    const Vector3 e = space.point(mesh.positions[mesh.indices[t * 3 + (at + 2) % 3]]);
                    const Vector3 wedge = transferWedge(p, b, e);
                    const Vector3 normal = detail::normalize3(detail::cross3(b - p, e - p));
                    float best = std::numeric_limits<float>::infinity();
                    bool found = false;
                    uint32_t chosen = 0;
                    for (uint32_t candidate : candidates) {
                        const uint32_t st = candidate / 3;
                        if (cosAngle > -1.0f && normal.squaredLength() > 0.0f && faceNormals[st].squaredLength() > 0.0f &&
                            detail::dot3(normal, faceNormals[st]) < cosAngle)
                            continue;
                        const float d = transferTriangleDistance2(wedge, source.positions[source.indices[st * 3]], source.positions[source.indices[st * 3 + 1]],
                                                                  source.positions[source.indices[st * 3 + 2]]);
                        if (d < best) {
                            best = d;
                            chosen = candidate;
                            found = true;
                        }
                    }
                    if (found) out.cornerValues[c] = source.cornerValues[chosen];
                }
            }
            out.perCorner = true;
            out.exactCorners = true;
            return true;
        }

    }

    bool fromTexture(const MeshView& mesh, const Op& op, const ApplyOptions& options, Field& out, OpResult& result, std::string& error) {
        const Image* image = op.image.get();
        if (image == nullptr || image->width <= 0 || image->height <= 0 || image->pixels.size() != (size_t)image->width * (size_t)image->height) {
            error = "from_texture: no texture pixels";
            return false;
        }
        if (op.uvSet != 0 && op.uvSet != 1) {
            error = "from_texture: uv_set must be 0 or 1";
            return false;
        }
        const std::vector<Vector2>& uvs = op.uvSet == 0 ? mesh.uv0 : mesh.uv1;
        const uint32_t vertexCount = mesh.vertexCount();
        if (uvs.size() != vertexCount || vertexCount == 0) {
            error = "from_texture: the mesh has no UV set " + std::to_string(op.uvSet);
            return false;
        }
        if (mesh.indices.size() % 3 != 0) {
            error = "the mesh's index count is not a multiple of 3";
            return false;
        }
        for (uint32_t index : mesh.indices) {
            if (index >= vertexCount) {
                error = "the mesh has an index past its vertices";
                return false;
            }
        }

        // R41: at least 16 samples per corner
        const std::vector<Vector3> pattern = bakeCornerPattern(std::max(op.samples, 16));
        const size_t cornerCount = mesh.indices.size();
        const uint32_t triangleCount = mesh.triangleCount();
        out = Field();
        out.perCorner = true;
        out.cornerValues.resize(cornerCount);
        std::vector<Vector4> vertexSum(vertexCount, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        std::vector<Vector4> vertexPlain(vertexCount, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        std::vector<float> vertexWeight(vertexCount, 0.0f);
        std::vector<uint32_t> vertexCorners(vertexCount, 0);
        std::vector<float> cornerDeltaE(cornerCount, 0.0f);
        std::vector<Vector4> samples(pattern.size());
        std::vector<pixelart::Lab> sampleLabs(pattern.size());
        const float texels = (float)image->width * (float)image->height;
        uint32_t underSampled = 0;

        for (uint32_t t = 0; t < triangleCount; t++) {
            if ((t & 255u) == 0) {
                if (bakeCancelled(options)) {
                    error = "cancelled";
                    return false;
                }
                bakeProgress(options, t, triangleCount);
            }
            const uint32_t tri[3] = {mesh.indices[(size_t)t * 3], mesh.indices[(size_t)t * 3 + 1], mesh.indices[(size_t)t * 3 + 2]};
            const Vector2 uv[3] = {uvs[tri[0]], uvs[tri[1]], uvs[tri[2]]};
            const float uvArea = 0.5f * std::fabs((uv[1].x - uv[0].x) * (uv[2].y - uv[0].y) - (uv[2].x - uv[0].x) * (uv[1].y - uv[0].y));
            // R41: more than 64 texels per vertex
            if (uvArea * texels / 3.0f > 64.0f) underSampled++;
            // each corner's area is a third of the triangle
            const float cornerArea = bakeTriangleArea(mesh.positions[tri[0]], mesh.positions[tri[1]], mesh.positions[tri[2]]) / 3.0f;

            for (uint32_t k = 0; k < 3; k++) {
                const Vector2& a = uv[k];
                const Vector2& b = uv[(k + 1) % 3];
                const Vector2& c = uv[(k + 2) % 3];
                Vector4 sum(0.0f, 0.0f, 0.0f, 0.0f);
                for (size_t s = 0; s < pattern.size(); s++) {
                    const Vector3& w = pattern[s];
                    samples[s] = image->sample(a.x * w.x + b.x * w.y + c.x * w.z, a.y * w.x + b.y * w.y + c.y * w.z);
                    sum = sum + samples[s];
                }
                const Vector4 mean = sum / (float)pattern.size();
                const size_t corner = (size_t)t * 3 + k;
                out.cornerValues[corner] = mean;

                // ΔE of each sample against the corner's value: flat texture areas repeat a value, so a
                // repeat reuses the last conversion
                const pixelart::Lab meanLab = bakeLab(mean);
                float deltaSum = 0.0f;
                for (size_t s = 0; s < samples.size(); s++) {
                    if (s == 0 || samples[s] != samples[s - 1]) sampleLabs[s] = bakeLab(samples[s]);
                    else sampleLabs[s] = sampleLabs[s - 1];
                    deltaSum += bakeDeltaE(sampleLabs[s], meanLab);
                }
                cornerDeltaE[corner] = deltaSum / (float)samples.size();

                const uint32_t v = tri[k];
                vertexSum[v] = vertexSum[v] + mean * cornerArea;
                vertexWeight[v] += cornerArea;
                vertexPlain[v] = vertexPlain[v] + mean;
                vertexCorners[v]++;
            }
        }

        // a GPU vertex takes the area-weighted average of its corners (a plain one when they all have no area)
        out.vertexValues.resize(vertexCount);
        bool allUsed = true;
        for (uint32_t v = 0; v < vertexCount; v++) {
            if (vertexWeight[v] > 0.0f) out.vertexValues[v] = vertexSum[v] / vertexWeight[v];
            else if (vertexCorners[v] > 0) out.vertexValues[v] = vertexPlain[v] / (float)vertexCorners[v];
            else allUsed = false;
        }
        if (!allUsed) {
            out.valid.resize(vertexCount);
            for (uint32_t v = 0; v < vertexCount; v++) out.valid[v] = vertexCorners[v] > 0 ? 1 : 0;
        }

        result.underSampledFaces = underSampled;
        result.meanDeltaE = 0.0f;
        result.p95DeltaE = 0.0f;
        if (!cornerDeltaE.empty()) {
            double total = 0.0;
            for (float d : cornerDeltaE) total += d;
            result.meanDeltaE = (float)(total / (double)cornerDeltaE.size());
            // nearest rank
            std::vector<float> sorted = cornerDeltaE;
            const size_t rank = (size_t)std::ceil(0.95 * (double)sorted.size());
            const size_t at = rank > 0 ? rank - 1 : 0;
            std::nth_element(sorted.begin(), sorted.begin() + (std::ptrdiff_t)at, sorted.end());
            result.p95DeltaE = sorted[at];
        }
        bakeProgress(options, 1, 1);
        return true;
    }

    bool ambientOcclusion(const MeshView& mesh, const Op& op, const ApplyOptions& options, bool thickness, Field& out, std::string& error) {
        if (!bakeNeedsBuiltMesh(mesh, error)) return false;
        if (op.rays < 1) {
            error = std::string(thickness ? "bake_thickness" : "bake_ao") + ": rays must be at least 1";
            return false;
        }
        if (!(op.distance > 0.0f)) {
            error = std::string(thickness ? "bake_thickness" : "bake_ao") + ": distance must be above 0";
            return false;
        }

        PickGeometry geometry;
        geometry.positions = mesh.positions;
        geometry.indices = mesh.indices;
        geometry.submeshFirstTriangle = {0u, mesh.triangleCount()};
        const MeshBvh bvh = MeshBvh::build(geometry);

        const uint32_t logicalCount = mesh.logicalCount();
        Vector3 lo, hi;
        for (uint32_t l = 0; l < logicalCount; l++) {
            const Vector3& p = mesh.logicalPositions[l];
            lo = l == 0 ? p : Vector3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = l == 0 ? p : Vector3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        // off the surface by a ten-thousandth of the size, so a ray does not hit its own triangles
        const float epsilon = std::max(std::sqrt((hi - lo).squaredLength()) * 1e-4f, 1e-7f);
        const bool scene = op.sceneScope && options.sceneOccluded;
        const detail::Space space = detail::Space::of(options.model);

        std::vector<float> logicalValue(logicalCount, 1.0f);
        for (uint32_t l = 0; l < logicalCount; l++) {
            if ((l & 63u) == 0) {
                if (bakeCancelled(options)) {
                    error = "cancelled";
                    return false;
                }
                bakeProgress(options, l, logicalCount);
            }
            Vector3 normal = mesh.logicalNormals[l];
            if (normal.squaredLength() <= 0.0f) continue;   // degenerate: open
            if (thickness) normal = -normal;
            Vector3 b1, b2;
            bakeBasis(normal, b1, b2);
            const Vector3 origin = mesh.logicalPositions[l] + normal * epsilon;
            const Vector3 worldOrigin = scene ? space.point(origin) : Vector3();
            // one sequence per (seed, logical vertex): the same rays on every platform and run
            const uint64_t sequence = ((uint64_t)op.seed << 32) | (uint64_t)l;
            int64_t draw = 0;
            uint32_t hits = 0;
            for (int i = 0; i < op.rays; i++) {
                // cosine-weighted (Malley): a uniform point of the unit disk lifted to the hemisphere
                float x, y, r2;
                do {
                    x = (float)(2.0 * hashUnit(sequence, draw++) - 1.0);
                    y = (float)(2.0 * hashUnit(sequence, draw++) - 1.0);
                    r2 = x * x + y * y;
                } while (r2 >= 1.0f);
                const float z = std::sqrt(1.0f - r2);
                const Vector3 direction = b1 * x + b2 * y + normal * z;

                // op.distance counts in world units when the ops do (ApplyOptions::world)
                float localMax = op.distance;
                float worldMax = op.distance;
                Vector3 worldDirection;
                if (scene || options.world) {
                    worldDirection = bakeLinear(space, direction);
                    const float scale = std::sqrt(worldDirection.squaredLength());
                    if (scale > 0.0f) {
                        if (options.world) localMax = op.distance / scale;
                        else worldMax = op.distance * scale;
                        worldDirection = worldDirection / scale;
                    }
                }
                MeshBvhRay ray;
                ray.origin = origin;
                ray.direction = direction;
                ray.tMax = localMax;
                MeshBvhHit hit;
                if (bvh.raycast(ray, hit)) {
                    hits++;
                } else if (scene && options.sceneOccluded(worldOrigin, worldDirection, worldMax)) {
                    hits++;
                }
            }
            logicalValue[l] = 1.0f - (float)hits / (float)op.rays;
        }

        out = Field();
        out.vertexValues.resize(mesh.vertexCount());
        for (uint32_t v = 0; v < mesh.vertexCount(); v++) {
            const float value = logicalValue[mesh.logical[v]];
            out.vertexValues[v] = Vector4(value, value, value, value);
        }
        bakeProgress(options, 1, 1);
        return true;
    }

    bool curvature(const MeshView& mesh, const Op& op, Field& out, std::string& error) {
        if (!bakeNeedsBuiltMesh(mesh, error)) return false;
        const uint32_t logicalCount = mesh.logicalCount();
        if (mesh.neighborStart.size() != (size_t)logicalCount + 1) {
            error = "the mesh view is not built";
            return false;
        }
        // Signed convexity per logical vertex: the mean over its edges of (n_j - n_i) . (p_j - p_i) / |p_j - p_i|,
        // about the angle in radians the normal turns along the edge, positive where the surface bends
        // away (convex), 0 on flat ground
        std::vector<float> convexity(logicalCount, 0.0f);
        for (uint32_t l = 0; l < logicalCount; l++) {
            float sum = 0.0f;
            uint32_t count = 0;
            for (uint32_t i = mesh.neighborStart[l]; i < mesh.neighborStart[l + 1]; i++) {
                const uint32_t j = mesh.neighbors[i];
                const Vector3 edge = mesh.logicalPositions[j] - mesh.logicalPositions[l];
                const float length = std::sqrt(edge.squaredLength());
                if (!(length > 0.0f)) continue;
                sum += (mesh.logicalNormals[j] - mesh.logicalNormals[l]).dotProduct(edge) / length;
                count++;
            }
            convexity[l] = count > 0 ? sum / (float)count : 0.0f;
        }
        // blur: each ring averages a vertex with its neighbours (op.iterations rings, R43 radius in edges)
        std::vector<float> next(logicalCount);
        for (int it = 0; it < op.iterations; it++) {
            for (uint32_t l = 0; l < logicalCount; l++) {
                float sum = convexity[l];
                for (uint32_t i = mesh.neighborStart[l]; i < mesh.neighborStart[l + 1]; i++) sum += convexity[mesh.neighbors[i]];
                next[l] = sum / (float)(1u + mesh.neighborStart[l + 1] - mesh.neighborStart[l]);
            }
            convexity.swap(next);
        }
        // 0.5 flat; a turn of 1 radian per edge reaches 0 or 1 at contrast 0; contrast scales it by 1 + contrast
        const float gain = std::max(0.0f, 1.0f + op.contrast);
        out = Field();
        out.vertexValues.resize(mesh.vertexCount());
        for (uint32_t v = 0; v < mesh.vertexCount(); v++) {
            const float value = std::clamp(0.5f + 0.5f * gain * convexity[mesh.logical[v]], 0.0f, 1.0f);
            out.vertexValues[v] = Vector4(value, value, value, value);
        }
        return true;
    }

    bool transfer(const MeshView& mesh, const Op& op, const ApplyOptions& options, Field& out, OpResult& result, std::string& error) {
        const TransferSource* source = op.source.get();
        if (source == nullptr) {
            error = "transfer: no source";
            return false;
        }
        if (source->values.size() != source->positions.size()) {
            error = "transfer: the source has " + std::to_string(source->positions.size()) + " positions and " +
                    std::to_string(source->values.size()) + " values";
            return false;
        }
        if (!source->normals.empty() && source->normals.size() != source->positions.size()) {
            error = "transfer: the source has " + std::to_string(source->positions.size()) + " positions and " +
                    std::to_string(source->normals.size()) + " normals";
            return false;
        }
        const bool sourceCorners = !source->indices.empty() || !source->cornerValues.empty();
        if (sourceCorners) {
            if (source->cornerValues.size() != source->indices.size() || source->indices.size() % 3 != 0) {
                error = "transfer: the source has " + std::to_string(source->indices.size()) + " indices and " +
                        std::to_string(source->cornerValues.size()) + " corner values";
                return false;
            }
            for (uint32_t index : source->indices) {
                if (index >= source->positions.size()) {
                    error = "transfer: the source has an index past its " + std::to_string(source->positions.size()) + " positions";
                    return false;
                }
            }
        }
        if (!(op.maxDistance >= 0.0f)) {
            error = "transfer: max_distance must be 0 or more";
            return false;
        }
        const uint32_t vertexCount = mesh.vertexCount();
        const bool vertexNormals = mesh.normals.size() == vertexCount;
        const bool hasLogicalNormals = mesh.logical.size() == vertexCount && mesh.logicalNormals.size() == mesh.logicalCount();

        std::vector<Vector3> sourceNormals(source->normals.size());
        for (size_t i = 0; i < source->normals.size(); i++) {
            const float length = std::sqrt(source->normals[i].squaredLength());
            sourceNormals[i] = length > 0.0f ? source->normals[i] / length : Vector3();
        }
        const bool normalFilter = !sourceNormals.empty() && op.normalAngle < 180.0f;
        const float cosAngle = normalFilter ? (float)detail::detCos((double)op.normalAngle * detail::kPi / 180.0) : -1.0f;
        const detail::Space space = detail::Space::of(options.model);

        const TransferGrid grid(source->positions);
        out = Field();
        out.vertexValues.assign(vertexCount, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        out.valid.assign(vertexCount, 0);
        uint32_t unmatched = 0;
        uint32_t matched = 0;
        double distanceSum = 0.0;
        for (uint32_t v = 0; v < vertexCount; v++) {
            if ((v & 1023u) == 0) {
                if (bakeCancelled(options)) {
                    error = "cancelled";
                    return false;
                }
                bakeProgress(options, v, vertexCount);
            }
            const Vector3 world = space.point(mesh.positions[v]);
            Vector3 normal;
            if (vertexNormals) normal = mesh.normals[v];
            else if (hasLogicalNormals) normal = mesh.logicalNormals[mesh.logical[v]];
            normal = space.direction(normal);
            const bool hasNormal = normalFilter && normal.squaredLength() > 0.0f;

            uint32_t index = 0;
            float d2 = 0.0f;
            const bool found = grid.nearest(world, op.maxDistance, [&](uint32_t i) {
                if (!hasNormal || sourceNormals[i].squaredLength() <= 0.0f) return true;
                return normal.dotProduct(sourceNormals[i]) >= cosAngle;
            }, index, d2);
            bool use = found;
            if (!found) {
                unmatched++;
                if (op.unmatched == Unmatched::Nearest)
                    use = grid.nearest(world, std::numeric_limits<float>::infinity(), [](uint32_t) { return true; }, index, d2);
            }
            if (!use) continue;
            out.vertexValues[v] = source->values[index];
            out.valid[v] = 1;
            distanceSum += std::sqrt((double)d2);
            matched++;
        }
        result.unmatched = unmatched;
        result.meanDistance = matched > 0 ? (float)(distanceSum / (double)matched) : 0.0f;
        // the corners' filter compares face normals, which every source has
        const float cornerCos = op.normalAngle < 180.0f ? (float)detail::detCos((double)op.normalAngle * detail::kPi / 180.0) : -1.0f;
        if (sourceCorners && !transferCorners(mesh, op, options, *source, grid, cornerCos, out, error)) return false;
        bakeProgress(options, 1, 1);
        return true;
    }

}
