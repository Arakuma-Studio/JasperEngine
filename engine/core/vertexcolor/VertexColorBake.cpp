// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R41-R43, R60: the value-field generators. Deterministic on every
// platform (R27): fixed sample patterns, a hashed sequence for the rays, arithmetic and sqrt only on
// the paths that decide values, and ties broken by index.

#include "vertexcolor/VertexColorBake.h"

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
            return 0.5f * std::sqrt((b - a).crossProduct(c - a).squaredLength());
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

        Vector3 bakeDirectionToWorld(const Matrix4& model, const Vector3& d) {
            const Vector4 w = model * Vector4(d, 0.0f);
            return Vector3(w.x, w.y, w.z);
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
                    cell = (float)std::pow(measure / (double)points.size(), 1.0 / (double)axes);
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
            const Vector3 worldOrigin = scene ? options.model * origin : Vector3();
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
                    worldDirection = bakeDirectionToWorld(options.model, direction);
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
        const float cosAngle = normalFilter ? (float)std::cos((double)op.normalAngle * 3.14159265358979323846 / 180.0) : -1.0f;
        const Matrix4 normalMatrix = options.model.inverse().transpose();

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
            const Vector3 world = options.model * mesh.positions[v];
            Vector3 normal;
            if (vertexNormals) normal = mesh.normals[v];
            else if (hasLogicalNormals) normal = mesh.logicalNormals[mesh.logical[v]];
            normal = bakeDirectionToWorld(normalMatrix, normal);
            const float length = std::sqrt(normal.squaredLength());
            const bool hasNormal = normalFilter && length > 0.0f;
            if (hasNormal) normal = normal / length;

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
        bakeProgress(options, 1, 1);
        return true;
    }

}
