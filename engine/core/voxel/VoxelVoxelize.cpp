// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelVoxelize.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine::voxel;

namespace {

    struct V3 {
        double x, y, z;
    };

    V3 sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
    double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    // The squared distance to a triangle (Ericson, Real-Time Collision Detection 5.1.5)
    double pointTriangleSq(const V3& p, const V3& a, const V3& b, const V3& c) {
        const V3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
        const double d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0.0 && d2 <= 0.0) return dot(ap, ap);
        const V3 bp = sub(p, b);
        const double d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0.0 && d4 <= d3) return dot(bp, bp);
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
            const double v = d1 / (d1 - d3);
            const V3 q = sub(ap, V3{ab.x * v, ab.y * v, ab.z * v});
            return dot(q, q);
        }
        const V3 cp = sub(p, c);
        const double d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0.0 && d5 <= d6) return dot(cp, cp);
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
            const double w = d2 / (d2 - d6);
            const V3 q = sub(ap, V3{ac.x * w, ac.y * w, ac.z * w});
            return dot(q, q);
        }
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
            const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            const V3 bc = sub(c, b);
            const V3 q = sub(bp, V3{bc.x * w, bc.y * w, bc.z * w});
            return dot(q, q);
        }
        const double denom = 1.0 / (va + vb + vc);
        const double v = vb * denom, w = vc * denom;
        const V3 q = sub(ap, V3{ab.x * v + ac.x * w, ab.y * v + ac.y * w, ab.z * v + ac.z * w});
        return dot(q, q);
    }

    // Where a column along `axis` through (u, v) crosses a triangle, in samples; false if it does not.
    // The half-open edge rule keeps a column through a shared edge from counting twice.
    bool columnHit(const double* a, const double* b, const double* c, int axisU, int axisV, int axisW, double u, double v, double& w) {
        const double au = a[axisU] - u, av = a[axisV] - v;
        const double bu = b[axisU] - u, bv = b[axisV] - v;
        const double cu = c[axisU] - u, cv = c[axisV] - v;
        const double e0 = bu * cv - bv * cu;
        const double e1 = cu * av - cv * au;
        const double e2 = au * bv - av * bu;
        const bool positive = e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0 && (e0 > 0.0 || e1 > 0.0 || e2 > 0.0);
        const bool negative = e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0 && (e0 < 0.0 || e1 < 0.0 || e2 < 0.0);
        if (!positive && !negative) return false;
        const double sum = e0 + e1 + e2;
        if (sum == 0.0) return false;
        w = (e0 * a[axisW] + e1 * b[axisW] + e2 * c[axisW]) / sum;
        return true;
    }

}

std::shared_ptr<StampField> jasperengine::voxel::voxelizeMesh(std::span<const float> positions, std::span<const uint32_t> indices, const VoxelizeOptions& options) {
    auto field = std::make_shared<StampField>();
    const size_t vertexCount = positions.size() / 3;
    const size_t triangleCount = indices.size() / 3;
    field->triangleCount = static_cast<int>(triangleCount);
    field->voxelSize = options.voxelSize;
    if (vertexCount == 0 || triangleCount == 0) return field;

    // In samples of the grid, whose sample 0 is at `low`
    std::vector<V3> vertices(vertexCount);
    V3 low{1e30, 1e30, 1e30}, high{-1e30, -1e30, -1e30};
    for (size_t i = 0; i < vertexCount; i++) {
        V3 p{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
        if (options.transform) {
            const double* m = options.transform;
            p = V3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
        }
        vertices[i] = p;
        low = V3{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = V3{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const double band = options.bandVoxels;
    const double padding = (band + 1.0) * options.voxelSize;
    const V3 origin{low.x - padding, low.y - padding, low.z - padding};
    const Int3 size{static_cast<int>(std::ceil((high.x - origin.x + padding) / options.voxelSize)) + 1,
                    static_cast<int>(std::ceil((high.y - origin.y + padding) / options.voxelSize)) + 1,
                    static_cast<int>(std::ceil((high.z - origin.z + padding) / options.voxelSize)) + 1};
    field->size = size;
    // The field's origin is its own corner: a placed stamp's center is this offset's position
    field->originOffset[0] = -origin.x;
    field->originOffset[1] = -origin.y;
    field->originOffset[2] = -origin.z;
    for (V3& p : vertices) p = V3{(p.x - origin.x) / options.voxelSize, (p.y - origin.y) / options.voxelSize, (p.z - origin.z) / options.voxelSize};

    const size_t samples = static_cast<size_t>(size.x) * static_cast<size_t>(size.y) * static_cast<size_t>(size.z);
    const auto at = [&](int x, int y, int z) { return static_cast<size_t>(x) + static_cast<size_t>(size.x) * (static_cast<size_t>(y) + static_cast<size_t>(size.y) * static_cast<size_t>(z)); };

    std::vector<float> distanceSq(samples, static_cast<float>((band + 1.0) * (band + 1.0)));
    // Ray columns per axis: every crossing of a triangle, on the sample lattice
    std::vector<std::vector<float>> columns[3];
    const int dims[3] = {size.x, size.y, size.z};
    const int axisU[3] = {1, 0, 0}, axisV[3] = {2, 2, 1};
    for (int axis = 0; axis < 3; axis++) columns[axis].resize(static_cast<size_t>(dims[axisU[axis]]) * static_cast<size_t>(dims[axisV[axis]]));

    for (size_t t = 0; t < triangleCount; t++) {
        const V3& a = vertices[indices[t * 3]];
        const V3& b = vertices[indices[t * 3 + 1]];
        const V3& c = vertices[indices[t * 3 + 2]];
        const double lo[3] = {std::min(a.x, std::min(b.x, c.x)), std::min(a.y, std::min(b.y, c.y)), std::min(a.z, std::min(b.z, c.z))};
        const double hi[3] = {std::max(a.x, std::max(b.x, c.x)), std::max(a.y, std::max(b.y, c.y)), std::max(a.z, std::max(b.z, c.z))};
        const int x0 = std::max(static_cast<int>(std::floor(lo[0] - band)), 0), x1 = std::min(static_cast<int>(std::ceil(hi[0] + band)), size.x - 1);
        const int y0 = std::max(static_cast<int>(std::floor(lo[1] - band)), 0), y1 = std::min(static_cast<int>(std::ceil(hi[1] + band)), size.y - 1);
        const int z0 = std::max(static_cast<int>(std::floor(lo[2] - band)), 0), z1 = std::min(static_cast<int>(std::ceil(hi[2] + band)), size.z - 1);
        for (int z = z0; z <= z1; z++) {
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    const float d = static_cast<float>(pointTriangleSq(V3{static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)}, a, b, c));
                    float& best = distanceSq[at(x, y, z)];
                    if (d < best) best = d;
                }
            }
        }
        const double pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z}, pc[3] = {c.x, c.y, c.z};
        for (int axis = 0; axis < 3; axis++) {
            const int u = axisU[axis], v = axisV[axis];
            // floor and ceil: the columns are nudged off the lattice, so one just outside the box may be inside
            const int u0 = std::max(static_cast<int>(std::floor(lo[u])), 0), u1 = std::min(static_cast<int>(std::ceil(hi[u])), dims[u] - 1);
            const int v0 = std::max(static_cast<int>(std::floor(lo[v])), 0), v1 = std::min(static_cast<int>(std::ceil(hi[v])), dims[v] - 1);
            for (int cv = v0; cv <= v1; cv++) {
                for (int cu = u0; cu <= u1; cu++) {
                    double w = 0.0;
                    // A small nudge keeps columns off vertices and edges of a mesh on the lattice
                    if (columnHit(pa, pb, pc, u, v, axis, cu + 1e-4, cv + 2e-4, w)) {
                        columns[axis][static_cast<size_t>(cu) + static_cast<size_t>(dims[u]) * static_cast<size_t>(cv)].push_back(static_cast<float>(w));
                    }
                }
            }
        }
    }

    // A column with an odd number of crossings has a hole: it says nothing about any sample on it
    std::vector<uint8_t> inside[3];
    std::vector<uint8_t> known[3];
    for (int axis = 0; axis < 3; axis++) {
        inside[axis].assign(samples, 0);
        known[axis].assign(samples, 1);
        const int u = axisU[axis], v = axisV[axis];
        for (int cv = 0; cv < dims[v]; cv++) {
            for (int cu = 0; cu < dims[u]; cu++) {
                std::vector<float>& hits = columns[axis][static_cast<size_t>(cu) + static_cast<size_t>(dims[u]) * static_cast<size_t>(cv)];
                if (hits.empty()) continue;
                std::sort(hits.begin(), hits.end());
                const bool closed = hits.size() % 2 == 0;
                size_t next = 0;
                bool in = false;
                for (int w = 0; w < dims[axis]; w++) {
                    while (next < hits.size() && hits[next] <= static_cast<float>(w)) {
                        in = !in;
                        next++;
                    }
                    int xyz[3];
                    xyz[axis] = w;
                    xyz[u] = cu;
                    xyz[v] = cv;
                    const size_t index = at(xyz[0], xyz[1], xyz[2]);
                    inside[axis][index] = in ? 1 : 0;
                    if (!closed) known[axis][index] = 0;
                }
            }
        }
    }

    field->sdf.resize(samples);
    int64_t disagreements = 0;
    for (size_t i = 0; i < samples; i++) {
        const int votesIn = inside[0][i] + inside[1][i] + inside[2][i];
        const bool solid = votesIn >= 2;
        if ((votesIn != 0 && votesIn != 3) || !known[0][i] || !known[1][i] || !known[2][i]) disagreements++;
        const double distance = std::sqrt(static_cast<double>(distanceSq[i]));
        const double voxelUnits = distance > band + 1.0 ? band + 1.0 : distance;
        field->sdf[i] = encodeSdf((solid ? -voxelUnits : voxelUnits));
    }
    field->nonWatertight = static_cast<double>(disagreements) > static_cast<double>(samples) * 0.01;
    return field;
}
