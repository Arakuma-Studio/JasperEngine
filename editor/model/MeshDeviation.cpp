// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshDeviation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

using namespace jasperengine::editor;

namespace {

    struct V3 { double x, y, z; };
    V3 sub(V3 a, V3 b){ return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    double dot(V3 a, V3 b){ return a.x * b.x + a.y * b.y + a.z * b.z; }
    V3 madd(V3 a, V3 b, double s){ return {a.x + b.x * s, a.y + b.y * s, a.z + b.z * s}; }

    // the closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5)
    double distanceToTriangle(V3 p, V3 a, V3 b, V3 c){
        const V3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
        const double d1 = dot(ab, ap), d2 = dot(ac, ap);
        V3 q;
        if (d1 <= 0 && d2 <= 0){
            q = a;
        }else{
            const V3 bp = sub(p, b);
            const double d3 = dot(ab, bp), d4 = dot(ac, bp);
            const V3 cp = sub(p, c);
            const double d5 = dot(ab, cp), d6 = dot(ac, cp);
            const double vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
            if (d3 >= 0 && d4 <= d3) q = b;
            else if (vc <= 0 && d1 >= 0 && d3 <= 0) q = madd(a, ab, d1 / (d1 - d3));
            else if (d6 >= 0 && d5 <= d6) q = c;
            else if (vb <= 0 && d2 >= 0 && d6 <= 0) q = madd(a, ac, d2 / (d2 - d6));
            else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) q = madd(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
            else{
                const double denom = 1.0 / (va + vb + vc);
                q = madd(madd(a, ab, vb * denom), ac, vc * denom);
            }
        }
        const V3 d = sub(p, q);
        return std::sqrt(dot(d, d));
    }

}

double MeshDeviation::surfaceDeviation(const std::vector<float>& positions, const std::vector<uint32_t>& source,
                                       const std::vector<uint32_t>& simplified, size_t maxSamples){
    const size_t vertexCount = positions.size() / 3;
    const size_t triangles = simplified.size() / 3;
    if (source.size() < 3 || triangles == 0)
        return 0.0;
    auto at = [&](uint32_t i) -> V3 {
        return {positions[(size_t)i * 3], positions[(size_t)i * 3 + 1], positions[(size_t)i * 3 + 2]};
    };
    for (uint32_t i : simplified)
        if (i >= vertexCount) return 0.0;
    for (uint32_t i : source)
        if (i >= vertexCount) return 0.0;

    // a uniform grid over the simplified triangles
    V3 lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    V3 hi{-lo.x, -lo.y, -lo.z};
    for (uint32_t i : source){
        const V3 p = at(i);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const double extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-12});
    const int cells = std::clamp((int)std::cbrt((double)triangles), 1, 64);
    const double h = extent / cells;
    auto cellOf = [&](double v, double base){ return std::clamp((int)std::floor((v - base) / h), 0, cells - 1); };
    auto keyOf = [&](int x, int y, int z){ return ((int64_t)x * cells + y) * cells + z; };
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    for (size_t t = 0; t < triangles; t++){
        const V3 a = at(simplified[t * 3]), b = at(simplified[t * 3 + 1]), c = at(simplified[t * 3 + 2]);
        const int x0 = cellOf(std::min({a.x, b.x, c.x}), lo.x), x1 = cellOf(std::max({a.x, b.x, c.x}), lo.x);
        const int y0 = cellOf(std::min({a.y, b.y, c.y}), lo.y), y1 = cellOf(std::max({a.y, b.y, c.y}), lo.y);
        const int z0 = cellOf(std::min({a.z, b.z, c.z}), lo.z), z1 = cellOf(std::max({a.z, b.z, c.z}), lo.z);
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                for (int z = z0; z <= z1; z++)
                    grid[keyOf(x, y, z)].push_back((uint32_t)t);
    }
    std::vector<uint32_t> seen(triangles, 0);
    uint32_t stamp = 0;
    auto nearest = [&](V3 p){
        stamp++;
        const int cx = cellOf(p.x, lo.x), cy = cellOf(p.y, lo.y), cz = cellOf(p.z, lo.z);
        double best = std::numeric_limits<double>::max();
        for (int ring = 0; ring <= cells; ring++){
            for (int x = cx - ring; x <= cx + ring; x++){
                for (int y = cy - ring; y <= cy + ring; y++){
                    for (int z = cz - ring; z <= cz + ring; z++){
                        if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) != ring)
                            continue;
                        if (x < 0 || y < 0 || z < 0 || x >= cells || y >= cells || z >= cells)
                            continue;
                        auto it = grid.find(keyOf(x, y, z));
                        if (it == grid.end())
                            continue;
                        for (uint32_t t : it->second){
                            if (seen[t] == stamp)
                                continue;
                            seen[t] = stamp;
                            best = std::min(best, distanceToTriangle(p, at(simplified[t * 3]), at(simplified[t * 3 + 1]), at(simplified[t * 3 + 2])));
                        }
                    }
                }
            }
            // every cell further out is at least ring * h away
            if (best <= ring * h)
                break;
        }
        return best;
    };

    const size_t sourceTriangles = source.size() / 3;
    const size_t step = std::max<size_t>(1, sourceTriangles * 2 / std::max<size_t>(maxSamples, 1));
    double worst = 0.0;
    for (size_t t = 0; t < sourceTriangles; t += step){
        const V3 a = at(source[t * 3]), b = at(source[t * 3 + 1]), c = at(source[t * 3 + 2]);
        const V3 centre{(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, (a.z + b.z + c.z) / 3.0};
        worst = std::max(worst, nearest(a));
        worst = std::max(worst, nearest(centre));
    }
    return worst;
}
