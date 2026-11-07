// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/extrude.ts by SafEight, used with the author's permission.

#include "RingExtruder.h"

#include "TitleEarcut.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <utility>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    constexpr double MAX_MITER = 2.0;                   // Clipper's miter limit for bands (TitleOutline3D)
    const double CREASE_COS = std::cos(32.0 * 3.14159265358979323846 / 180.0);
    constexpr int UNTANGLE_STEPS = 8;                   // halving rounds before offending vertices are pinned
    constexpr double SAME_PT = 1e-7;                    // ring points closer than this are one point
    constexpr double RAY_CLAMP = 0.45;

    struct V2 {
        double x = 0, y = 0;
    };

    std::array<double, 2> norm2(double a, double b) {
        double l = std::hypot(a, b);
        if (l == 0) l = 1;
        return {a / l, b / l};
    }

    struct Ring {
        Contour pts;
        std::vector<V2> miter, smoothN, edgeN;     // edgeN[i]: the normal of edge i -> i+1
        std::vector<char> crease;
        std::vector<double> maxIn, maxOut;
        std::vector<double> arc;                   // arc length at each vertex along the base contour
        double length = 0;
    };

    Ring makeRing(const Contour& pts) {
        Ring r;
        r.pts = pts;
        const size_t n = pts.size();
        r.edgeN.resize(n);
        r.arc.resize(n);
        for (size_t i = 0; i < n; i++) {
            const TitleVec2& a = pts[i];
            const TitleVec2& b = pts[(i + 1) % n];
            const double dx = b.x - a.x, dy = b.y - a.y;
            double len = std::hypot(dx, dy);
            r.arc[i] = r.length;
            r.length += len;
            if (len == 0) len = 1;
            r.edgeN[i] = {dy / len, -dx / len};     // outward for the normalised orientation
        }
        r.miter.resize(n);
        r.smoothN.resize(n);
        r.crease.resize(n);
        for (size_t i = 0; i < n; i++) {
            const V2 n0 = r.edgeN[(i + n - 1) % n];
            const V2 n1 = r.edgeN[i];
            V2 bis{n0.x + n1.x, n0.y + n1.y};
            r.crease[i] = n0.x * n1.x + n0.y * n1.y < CREASE_COS;
            const double l2 = bis.x * bis.x + bis.y * bis.y;
            if (l2 < 1e-10) {
                // 180 degree spike: push along the incoming edge direction
                const V2 dir{-n0.y, n0.x};
                r.smoothN[i] = dir;
                r.miter[i] = {dir.x * MAX_MITER, dir.y * MAX_MITER};
                continue;
            }
            const double l = std::sqrt(l2);
            bis = {bis.x / l, bis.y / l};
            r.smoothN[i] = bis;
            const double k = 1 / std::max(bis.x * n0.x + bis.y * n0.y, 1 / MAX_MITER);
            r.miter[i] = {bis.x * k, bis.y * k};
        }
        r.maxIn.assign(n, INFINITY);
        r.maxOut.assign(n, INFINITY);
        return r;
    }

    // the ray from vertex `i` of ring `self` along (dx, dy) against edge k of ring q: its distance
    // or infinity (the adjacent edges of the vertex excluded)
    inline double hitEdge(const Ring& q, size_t k, bool sameRing, size_t i, const TitleVec2& p, double dx, double dy) {
        const size_t qn = q.pts.size();
        if (sameRing && (k == i || (k + 1) % qn == i)) return INFINITY;
        const TitleVec2& a = q.pts[k];
        const TitleVec2& b = q.pts[(k + 1) % qn];
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double den = dx * ey - dy * ex;
        if (std::abs(den) < 1e-12) return INFINITY;
        const double wx = a.x - p.x, wy = a.y - p.y;
        const double t = (wx * ey - wy * ex) / den;
        const double u = (wx * dy - wy * dx) / den;
        if (t > 1e-6 && u >= 0 && u <= 1) return t;
        return INFINITY;
    }

    double rayHitBrute(const std::vector<Ring>& rings, size_t self, size_t i, double dx, double dy) {
        const TitleVec2& p = rings[self].pts[i];
        double best = INFINITY;
        for (size_t r = 0; r < rings.size(); r++) {
            for (size_t k = 0; k < rings[r].pts.size(); k++) best = std::min(best, hitEdge(rings[r], k, r == self, i, p, dx, dy));
        }
        return best;
    }

    // The edges of a shape bucketed on a uniform grid (cell = the mean edge length): a ray walks
    // its cells in order and stops once the nearest hit lies before the next cell
    class EdgeGrid {
    public:
        explicit EdgeGrid(const std::vector<Ring>& rings) : rings(rings) {
            double x1 = -INFINITY, y1 = -INFINITY, total = 0;
            size_t edges = 0;
            for (const Ring& r : rings) {
                for (const TitleVec2& p : r.pts) {
                    x0 = std::min(x0, p.x);
                    y0 = std::min(y0, p.y);
                    x1 = std::max(x1, p.x);
                    y1 = std::max(y1, p.y);
                }
                total += r.length;
                edges += r.pts.size();
            }
            if (!edges) return;
            const double extent = std::max(x1 - x0, y1 - y0);
            cell = std::max({total / (double)edges, extent / 1024.0, 1e-9});
            nx = (int)std::floor((x1 - x0) / cell) + 1;
            ny = (int)std::floor((y1 - y0) / cell) + 1;
            cells.assign((size_t)nx * (size_t)ny, {});
            for (size_t r = 0; r < rings.size(); r++) {
                const Contour& pts = rings[r].pts;
                for (size_t k = 0; k < pts.size(); k++) {
                    const TitleVec2& a = pts[k];
                    const TitleVec2& b = pts[(k + 1) % pts.size()];
                    const int i0 = col(std::min(a.x, b.x)), i1 = col(std::max(a.x, b.x));
                    const int j0 = row(std::min(a.y, b.y)), j1 = row(std::max(a.y, b.y));
                    for (int j = j0; j <= j1; j++) {
                        for (int i = i0; i <= i1; i++) cells[(size_t)j * nx + i].push_back({(uint32_t)r, (uint32_t)k});
                    }
                }
            }
        }

        double hit(size_t self, size_t i, double dx, double dy) const {
            if (cells.empty()) return INFINITY;
            const TitleVec2& p = rings[self].pts[i];
            int cx = col(p.x), cy = row(p.y);
            const int stepX = dx > 0 ? 1 : dx < 0 ? -1 : 0;
            const int stepY = dy > 0 ? 1 : dy < 0 ? -1 : 0;
            auto boundary = [&](double o, double origin, int c, int step, double d) -> double {
                if (step == 0) return INFINITY;
                const double edge = origin + (c + (step > 0 ? 1 : 0)) * cell;
                return (edge - o) / d;
            };
            double tMaxX = boundary(p.x, x0, cx, stepX, dx);
            double tMaxY = boundary(p.y, y0, cy, stepY, dy);
            const double tDeltaX = stepX ? cell / std::abs(dx) : INFINITY;
            const double tDeltaY = stepY ? cell / std::abs(dy) : INFINITY;
            double best = INFINITY;
            for (;;) {
                for (const Entry& e : cells[(size_t)cy * nx + cx]) {
                    best = std::min(best, hitEdge(rings[e.ring], e.edge, e.ring == self, i, p, dx, dy));
                }
                const double next = std::min(tMaxX, tMaxY);
                if (best <= next) return best;
                if (tMaxX < tMaxY) {
                    cx += stepX;
                    tMaxX += tDeltaX;
                } else {
                    cy += stepY;
                    tMaxY += tDeltaY;
                }
                if (cx < 0 || cy < 0 || cx >= nx || cy >= ny) return best;
            }
        }

    private:
        struct Entry {
            uint32_t ring, edge;
        };
        const std::vector<Ring>& rings;
        double x0 = INFINITY, y0 = INFINITY, cell = 1;
        int nx = 0, ny = 0;
        std::vector<std::vector<Entry>> cells;

        int col(double x) const { return std::clamp((int)std::floor((x - x0) / cell), 0, nx - 1); }
        int row(double y) const { return std::clamp((int)std::floor((y - y0) / cell), 0, ny - 1); }
    };

    // Per-vertex offset limits: the miter ray reaching the opposite side (45 % of it) and an edge
    // collapsing to zero length (both its vertices stop there)
    void computeLimits(std::vector<Ring>& rings, bool inward, bool outward, bool rayClamp) {
        std::unique_ptr<EdgeGrid> grid;
        if (rayClamp && (inward || outward)) grid = std::make_unique<EdgeGrid>(rings);
        for (size_t ri = 0; ri < rings.size(); ri++) {
            Ring& r = rings[ri];
            const size_t n = r.pts.size();
            r.maxIn.assign(n, INFINITY);
            r.maxOut.assign(n, INFINITY);
            if (grid) {
                for (size_t i = 0; i < n; i++) {
                    const V2 m = r.miter[i];
                    double ml = std::hypot(m.x, m.y);
                    if (ml == 0) ml = 1;
                    if (inward) r.maxIn[i] = RAY_CLAMP * grid->hit(ri, i, -m.x / ml, -m.y / ml) / ml;
                    if (outward) r.maxOut[i] = RAY_CLAMP * grid->hit(ri, i, m.x / ml, m.y / ml) / ml;
                }
            }
            // edge i -> j at offset d has length^2 |e|^2 + d (dm . e): it vanishes at d* = -|e|^2 / (dm . e)
            for (size_t i = 0; i < n; i++) {
                const size_t j = (i + 1) % n;
                const double ex = r.pts[j].x - r.pts[i].x, ey = r.pts[j].y - r.pts[i].y;
                const double k = (r.miter[j].x - r.miter[i].x) * ex + (r.miter[j].y - r.miter[i].y) * ey;
                if (std::abs(k) < 1e-12) continue;
                const double d = -(ex * ex + ey * ey) / k;
                if (d > 0 && outward) {
                    r.maxOut[i] = std::min(r.maxOut[i], d);
                    r.maxOut[j] = std::min(r.maxOut[j], d);
                } else if (d < 0 && inward) {
                    r.maxIn[i] = std::min(r.maxIn[i], -d);
                    r.maxIn[j] = std::min(r.maxIn[j], -d);
                }
            }
        }
    }

    // the effective offset of ring vertex i (clamped to the local stroke / counter size)
    inline double eff(const Ring& r, size_t i, double d) {
        return d < 0 ? std::max(d, -r.maxIn[i]) : std::min(d, r.maxOut[i]);
    }

    // whether segments ab and cd meet, touching and collinear overlap included
    bool meets(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) {
        const double d1 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        const double d2 = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
        if (d1 == 0 && d2 == 0) {
            const bool useX = std::abs(bx - ax) >= std::abs(by - ay);
            const double a0 = useX ? std::min(ax, bx) : std::min(ay, by), a1 = useX ? std::max(ax, bx) : std::max(ay, by);
            const double c0 = useX ? std::min(cx, dx) : std::min(cy, dy), c1 = useX ? std::max(cx, dx) : std::max(cy, dy);
            return a0 <= c1 && c0 <= a1;
        }
        const double d3 = (dx - cx) * (ay - cy) - (dy - cy) * (ax - cx);
        const double d4 = (dx - cx) * (by - cy) - (dy - cy) * (bx - cx);
        return d1 * d2 <= 0 && d3 * d4 <= 0;
    }

    // Tightens the limits until the rings at offset d neither cross nor touch each other or
    // themselves (meeting cap rings break the cap's ear clipping)
    void untangle(std::vector<Ring>& rings, double d) {
        if (d == 0) return;
        struct Edge {
            size_t r, i, j;
            double x0, x1, y0, y1, ax, ay, bx, by;
        };
        std::vector<Edge> edges;
        for (int round = 0;; round++) {
            edges.clear();
            for (size_t ri = 0; ri < rings.size(); ri++) {
                const Ring& r = rings[ri];
                const size_t n = r.pts.size();
                for (size_t i = 0; i < n; i++) {
                    const size_t j = (i + 1) % n;
                    const double ei = eff(r, i, d), ej = eff(r, j, d);
                    const double ax = r.pts[i].x + r.miter[i].x * ei, ay = r.pts[i].y + r.miter[i].y * ei;
                    const double bx = r.pts[j].x + r.miter[j].x * ej, by = r.pts[j].y + r.miter[j].y * ej;
                    if (std::abs(ax - bx) < SAME_PT && std::abs(ay - by) < SAME_PT) continue;
                    edges.push_back({ri, i, j, std::min(ax, bx), std::max(ax, bx), std::min(ay, by), std::max(ay, by), ax, ay, bx, by});
                }
            }
            std::stable_sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.x0 < b.x0; });
            std::map<size_t, std::set<size_t>> bad;
            for (size_t s = 0; s < edges.size(); s++) {
                const Edge& e = edges[s];
                for (size_t t = s + 1; t < edges.size() && edges[t].x0 <= e.x1; t++) {
                    const Edge& f = edges[t];
                    if (f.y0 > e.y1 || f.y1 < e.y0) continue;
                    if (e.r == f.r) {
                        if (e.i == f.j || e.j == f.i) continue;
                        if ((std::abs(e.ax - f.bx) < SAME_PT && std::abs(e.ay - f.by) < SAME_PT) ||
                            (std::abs(e.bx - f.ax) < SAME_PT && std::abs(e.by - f.ay) < SAME_PT)) {
                            continue;
                        }
                    }
                    if (meets(e.ax, e.ay, e.bx, e.by, f.ax, f.ay, f.bx, f.by)) {
                        bad[e.r].insert(e.i);
                        bad[e.r].insert(e.j);
                        bad[f.r].insert(f.i);
                        bad[f.r].insert(f.j);
                    }
                }
            }
            if (bad.empty()) return;
            const bool pin = round >= UNTANGLE_STEPS;
            bool moved = false;
            for (const auto& [ri, verts] : bad) {
                Ring& r = rings[ri];
                for (size_t k : verts) {
                    const double cur = std::abs(eff(r, k, d));
                    if (cur == 0) continue;
                    moved = true;
                    const double limit = pin ? 0 : cur / 2;
                    if (d < 0) r.maxIn[k] = limit;
                    else r.maxOut[k] = limit;
                }
            }
            if (!moved) return;
        }
    }

    // quad / triangle scratch: corners (x, y, z), normals and arc lengths
    struct Corners {
        double P[12];
        double N[12];
        double A[4];
    };

    void emitQuad(RawMesh& mesh, int slot, const Corners& c) {
        const double* P = c.P;
        const double* N = c.N;
        const double ax = P[3] - P[0], ay = P[4] - P[1], az = P[5] - P[2];
        const double bx = P[6] - P[0], by = P[7] - P[1], bz = P[8] - P[2];
        const double cx = P[9] - P[0], cy = P[10] - P[1], cz = P[11] - P[2];
        // (a x b) + (b x c): the area normal of the (possibly non-planar) quad
        const double nx = ay * bz - az * by + (by * cz - bz * cy);
        const double ny = az * bx - ax * bz + (bz * cx - bx * cz);
        const double nz = ax * by - ay * bx + (bx * cy - by * cx);
        const double ndot = nx * (N[0] + N[3] + N[6] + N[9]) + ny * (N[1] + N[4] + N[7] + N[10]) + nz * (N[2] + N[5] + N[8] + N[11]);
        static const int FRONT[6] = {0, 1, 2, 0, 2, 3};
        static const int FLIPPED[6] = {0, 2, 1, 0, 3, 2};
        const int* order = ndot >= 0 ? FRONT : FLIPPED;
        for (int q = 0; q < 6; q++) {
            const int o = order[q] * 3;
            mesh.push(slot, P[o], P[o + 1], P[o + 2], N[o], N[o + 1], N[o + 2], c.A[order[q]]);
        }
    }

    void emitTri(RawMesh& mesh, int slot, const Corners& c) {
        const double* P = c.P;
        const double* N = c.N;
        const double ax = P[3] - P[0], ay = P[4] - P[1], az = P[5] - P[2];
        const double bx = P[6] - P[0], by = P[7] - P[1], bz = P[8] - P[2];
        const double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const double ndot = nx * (N[0] + N[3] + N[6]) + ny * (N[1] + N[4] + N[7]) + nz * (N[2] + N[5] + N[8]);
        static const int FRONT[3] = {0, 1, 2};
        static const int FLIPPED[3] = {0, 2, 1};
        const int* order = ndot >= 0 ? FRONT : FLIPPED;
        for (int q = 0; q < 3; q++) {
            const int o = order[q] * 3;
            mesh.push(slot, P[o], P[o + 1], P[o + 2], N[o], N[o + 1], N[o + 2], c.A[order[q]]);
        }
    }

    double triArea(const TitleVec2& a, const TitleVec2& b, const TitleVec2& c) {
        return ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    }

    // A flat cap at z facing nz over a ring set (outer first, then its holes), triangulated on
    // `tri` (its own points, or the outline's for the red control)
    // The winding is the triangulation's as a whole (its total signed area), never per triangle:
    // a sliver that a clamped ring folds keeps its neighbours' orientation, so the cap stays a
    // consistently oriented surface whose edges meet the strips'
    double polygonArea(const std::vector<Contour>& contours) {
        double a = 0;
        for (size_t i = 0; i < contours.size(); i++) a += (i == 0 ? 1 : -1) * std::abs(signedArea(contours[i]));
        return a;
    }

    double coveredArea(const std::vector<Contour>& contours, const std::vector<uint32_t>& tris) {
        std::vector<TitleVec2> pts;
        for (const Contour& c : contours) pts.insert(pts.end(), c.begin(), c.end());
        double a = 0;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) a += std::abs(triArea(pts[tris[t]], pts[tris[t + 1]], pts[tris[t + 2]]));
        return a;
    }

    // whether every ring edge is a triangle edge: earcut filters out the duplicate and collinear
    // vertices a pinned ring has, and the cap's boundary then skips vertices the strips use
    bool keepsRingEdges(const std::vector<Contour>& contours, const std::vector<uint32_t>& tris) {
        std::set<std::pair<uint32_t, uint32_t>> edges;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            for (int e = 0; e < 3; e++) {
                const uint32_t a = tris[t + e], b = tris[t + (e + 1) % 3];
                edges.insert({std::min(a, b), std::max(a, b)});
            }
        }
        uint32_t first = 0;
        for (const Contour& c : contours) {
            const uint32_t n = (uint32_t)c.size();
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t a = first + i, b = first + (i + 1) % n;
                if (!edges.count({std::min(a, b), std::max(a, b)})) return false;
            }
            first += n;
        }
        return true;
    }

    // `fallback`: the triangulation source when the ring's own does not cover its area or skips
    // some of its vertices (a ring that touches itself where untangle pinned it: earcut then drops
    // triangles or vertices and the cap opens)
    void emitCap(RawMesh& mesh, const std::vector<Contour>& ring, const std::vector<Contour>& own, int slot, double z, int nz,
                 const std::vector<Contour>* fallback = nullptr) {
        if (ring.empty() || ring[0].size() < 3) return;
        const std::vector<Contour>* tri = &own;
        std::vector<uint32_t> tris = triangulatePolygon(own[0], std::vector<Contour>(own.begin() + 1, own.end()));
        const double expected = polygonArea(own);
        if (fallback && (std::abs(coveredArea(own, tris) - expected) > 1e-3 * std::abs(expected) || !keepsRingEdges(own, tris))) {
            tri = fallback;
            tris = triangulatePolygon((*fallback)[0], std::vector<Contour>(fallback->begin() + 1, fallback->end()));
        }
        std::vector<TitleVec2> base, pts;
        for (const Contour& c : *tri) base.insert(base.end(), c.begin(), c.end());
        for (const Contour& c : ring) pts.insert(pts.end(), c.begin(), c.end());
        double area = 0;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) area += triArea(base[tris[t]], base[tris[t + 1]], base[tris[t + 2]]);
        const bool swap = (area >= 0) != (nz > 0);
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            uint32_t ia = tris[t], ib = tris[t + 1], ic = tris[t + 2];
            if (swap) std::swap(ib, ic);
            for (uint32_t v : {ia, ib, ic}) mesh.push(slot, pts[v].x, pts[v].y, z, 0, 0, nz);
        }
    }

    std::vector<Contour> ringPoints(const std::vector<Ring>& rings, double off) {
        std::vector<Contour> out;
        out.reserve(rings.size());
        for (const Ring& r : rings) {
            Contour c;
            c.reserve(r.pts.size());
            for (size_t i = 0; i < r.pts.size(); i++) {
                const double d = eff(r, i, off);
                c.push_back({r.pts[i].x + r.miter[i].x * d, r.pts[i].y + r.miter[i].y * d});
            }
            out.push_back(std::move(c));
        }
        return out;
    }

    std::array<double, 2> profileNormal(const ProfilePoint& p, const std::array<double, 2>& faceted) {
        return p.hasNormal ? std::array<double, 2>{p.nr, p.nz} : faceted;
    }

    // quad strips between profile points A and B along every ring
    void emitStrip(RawMesh& mesh, const std::vector<Ring>& rings, int slot, const ProfilePoint& A, const ProfilePoint& B) {
        const auto nf = norm2(A.z - B.z, B.off - A.off);    // faceted (radial, z)
        const auto nA = profileNormal(A, nf);
        const auto nB = profileNormal(B, nf);
        Corners c;
        for (const Ring& r : rings) {
            const size_t n = r.pts.size();
            for (size_t i = 0; i < n; i++) {
                const size_t j = (i + 1) % n;
                const V2 ni = r.crease[i] ? r.edgeN[i] : r.smoothN[i];
                const V2 nj = r.crease[j] ? r.edgeN[i] : r.smoothN[j];
                const TitleVec2 &pi = r.pts[i], &pj = r.pts[j];
                const V2 mi = r.miter[i], mj = r.miter[j];
                const double iA = eff(r, i, A.off), jA = eff(r, j, A.off), iB = eff(r, i, B.off), jB = eff(r, j, B.off);
                double* P = c.P;
                double* N = c.N;
                P[0] = pi.x + mi.x * iA; P[1] = pi.y + mi.y * iA; P[2] = A.z;
                P[3] = pj.x + mj.x * jA; P[4] = pj.y + mj.y * jA; P[5] = A.z;
                P[6] = pj.x + mj.x * jB; P[7] = pj.y + mj.y * jB; P[8] = B.z;
                P[9] = pi.x + mi.x * iB; P[10] = pi.y + mi.y * iB; P[11] = B.z;
                N[0] = ni.x * nA[0]; N[1] = ni.y * nA[0]; N[2] = nA[1];
                N[3] = nj.x * nA[0]; N[4] = nj.y * nA[0]; N[5] = nA[1];
                N[6] = nj.x * nB[0]; N[7] = nj.y * nB[0]; N[8] = nB[1];
                N[9] = ni.x * nB[0]; N[10] = ni.y * nB[0]; N[11] = nB[1];
                const double ai = r.arc[i], aj = j == 0 ? r.length : r.arc[j];
                c.A[0] = ai; c.A[1] = aj; c.A[2] = aj; c.A[3] = ai;
                emitQuad(mesh, slot, c);
            }
        }
    }

    // ---- outward bands ----

    struct OutRing {
        Shapes shapes;
        std::vector<Ring> rings;    // outers CCW, holes CW: edge normals point the way the offset grows
        std::vector<char> outer;
    };

    std::shared_ptr<OutRing> outRing(Shapes shapes) {
        auto out = std::make_shared<OutRing>();
        for (const ShapeContours& s : shapes) {
            out->rings.push_back(makeRing(s.outer));
            out->outer.push_back(1);
            for (const Contour& h : s.holes) {
                out->rings.push_back(makeRing(h));
                out->outer.push_back(0);
            }
        }
        out->shapes = std::move(shapes);
        return out;      // unclamped: every ring set is a clean offset already
    }

    // whether every vertex of ring set a lies within r of ring set b's edges (b is a grown
    // without a change of shape); edges bucketed on a grid of cells >= r
    bool within(const OutRing& a, const OutRing& b, double r) {
        double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (const Ring& ring : b.rings) {
            for (const TitleVec2& p : ring.pts) {
                x0 = std::min(x0, p.x);
                x1 = std::max(x1, p.x);
                y0 = std::min(y0, p.y);
                y1 = std::max(y1, p.y);
            }
        }
        const double h = std::max({r, (x1 - x0) / 512, (y1 - y0) / 512, 1e-6});
        const long cols = (long)std::floor((x1 - x0) / h) + 1;
        std::vector<double> segs;
        std::map<long, std::vector<size_t>> grid;
        for (const Ring& ring : b.rings) {
            const size_t n = ring.pts.size();
            for (size_t i = 0; i < n; i++) {
                const TitleVec2& p = ring.pts[i];
                const TitleVec2& q = ring.pts[(i + 1) % n];
                const size_t s = segs.size();
                segs.insert(segs.end(), {p.x, p.y, q.x, q.y});
                const long i0 = (long)std::floor((std::min(p.x, q.x) - x0) / h), i1 = (long)std::floor((std::max(p.x, q.x) - x0) / h);
                const long j0 = (long)std::floor((std::min(p.y, q.y) - y0) / h), j1 = (long)std::floor((std::max(p.y, q.y) - y0) / h);
                for (long j = j0; j <= j1; j++) {
                    for (long k = i0; k <= i1; k++) grid[j * cols + k].push_back(s);
                }
            }
        }
        const double r2 = r * r;
        for (const Ring& ring : a.rings) {
            for (const TitleVec2& p : ring.pts) {
                const long ci = (long)std::floor((p.x - x0) / h);
                const long cj = (long)std::floor((p.y - y0) / h);
                bool found = false;
                for (long j = cj - 1; j <= cj + 1 && !found; j++) {
                    for (long k = ci - 1; k <= ci + 1 && !found; k++) {
                        if (k < 0 || k >= cols) continue;
                        auto it = grid.find(j * cols + k);
                        if (it == grid.end()) continue;
                        for (size_t s : it->second) {
                            const double ax = segs[s], ay = segs[s + 1];
                            const double ex = segs[s + 2] - ax, ey = segs[s + 3] - ay;
                            const double len2 = ex * ex + ey * ey;
                            const double t = len2 > 0 ? std::clamp(((p.x - ax) * ex + (p.y - ay) * ey) / len2, 0.0, 1.0) : 0.0;
                            const double dx = ax + ex * t - p.x, dy = ay + ey * t - p.y;
                            if (dx * dx + dy * dy <= r2) {
                                found = true;
                                break;
                            }
                        }
                    }
                }
                if (!found) return false;
            }
        }
        return true;
    }

    // even-odd point in polygon against one contour
    bool insideContour(const Contour& c, double x, double y) {
        bool inside = false;
        for (size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) {
            const TitleVec2& a = c[i];
            const TitleVec2& b = c[j];
            if ((a.y > y) != (b.y > y) && x < ((b.x - a.x) * (y - a.y)) / (b.y - a.y) + a.x) inside = !inside;
        }
        return inside;
    }

    double contourAreaAbs(const Contour& c) {
        double s = 0;
        for (size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) s += c[j].x * c[i].y - c[i].x * c[j].y;
        return std::abs(s) / 2;
    }

    // the in-plane normal of ring vertex i in a triangle centred at (cx, cy): at a crease, the
    // normal of the edge on the triangle's side of the miter
    V2 vertexNormal(const Ring& r, size_t i, double cx, double cy) {
        if (!r.crease[i]) return r.smoothN[i];
        const size_t n = r.pts.size();
        const size_t h = (i + n - 1) % n;
        const TitleVec2& p = r.pts[i];
        const TitleVec2& q = r.pts[h];
        const V2 m = r.miter[i];
        auto side = [&](double x, double y) { return m.x * (y - p.y) - m.y * (x - p.x); };
        return side(cx, cy) * side(q.x, q.y) > 0 ? r.edgeN[h] : r.edgeN[i];
    }

    struct AnnulusContour {
        const Ring* ring = nullptr;
        double z = 0;
        std::array<double, 2> n{0, 0};
        int set = 0;
        bool outer = false;
        double area = 0;
        AnnulusContour* parent = nullptr;
        std::vector<AnnulusContour*> children;
    };

    struct AnnulusVertex {
        TitleVec2 p;
        const AnnulusContour* o;
        size_t i;
    };

    double trianglesArea(const std::vector<AnnulusVertex>& verts, const std::vector<uint32_t>& tris) {
        double s = 0;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) s += std::abs(triArea(verts[tris[t]].p, verts[tris[t + 1]].p, verts[tris[t + 2]].p));
        return s;
    }

    AnnulusVertex nearestRingVertex(const std::vector<const AnnulusContour*>& contours, const TitleVec2& p) {
        AnnulusVertex best{p, contours[0], 0};
        double bestD = INFINITY;
        for (const AnnulusContour* o : contours) {
            const Contour& pts = o->ring->pts;
            for (size_t i = 0, n = pts.size(); i < n; i++) {
                const TitleVec2& a = pts[i];
                const TitleVec2& b = pts[(i + 1) % n];
                const double ex = b.x - a.x, ey = b.y - a.y;
                const double len2 = ex * ex + ey * ey;
                const double t = len2 > 0 ? std::clamp(((p.x - a.x) * ex + (p.y - a.y) * ey) / len2, 0.0, 1.0) : 0.0;
                const double x = a.x + ex * t, y = a.y + ey * t;
                const double d = std::hypot(x - p.x, y - p.y);
                if (d < bestD) {
                    bestD = d;
                    best = {{x, y}, o, t < 0.5 ? i : (i + 1) % n};
                }
            }
        }
        return best;
    }

    void emitAnnulusTris(RawMesh& mesh, const std::vector<AnnulusVertex>& verts, const std::vector<uint32_t>& tris, int slot) {
        Corners c;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            double cx = 0, cy = 0;
            for (int k = 0; k < 3; k++) {
                cx += verts[tris[t + k]].p.x / 3;
                cy += verts[tris[t + k]].p.y / 3;
            }
            for (int k = 0; k < 3; k++) {
                const AnnulusVertex& v = verts[tris[t + k]];
                const V2 d = vertexNormal(*v.o->ring, v.i, cx, cy);
                c.P[k * 3] = v.p.x;
                c.P[k * 3 + 1] = v.p.y;
                c.P[k * 3 + 2] = v.o->z;
                c.N[k * 3] = d.x * v.o->n[0];
                c.N[k * 3 + 1] = d.y * v.o->n[0];
                c.N[k * 3 + 2] = v.o->n[1];
                c.A[k] = v.o->ring->arc[v.i];
            }
            emitTri(mesh, slot, c);
        }
    }

    // the band between ring sets a (inner, at z0) and b (outer, at z1): every region inside b and
    // outside a, nested by containment
    void emitAnnulus(RawMesh& mesh, const OutRing& a, const OutRing& b, int slot, double z0, double z1, std::array<double, 2> n0,
                     std::array<double, 2> n1, double eps, ExtrudeStats* stats, bool forceFallback) {
        std::vector<AnnulusContour> all;
        all.reserve(a.rings.size() + b.rings.size());
        auto add = [&](const OutRing& set, int k, double z, std::array<double, 2> n) {
            for (size_t i = 0; i < set.rings.size(); i++) {
                AnnulusContour c;
                c.ring = &set.rings[i];
                c.z = z;
                c.n = n;
                c.set = k;
                c.outer = set.outer[i] != 0;
                c.area = contourAreaAbs(set.rings[i].pts);
                all.push_back(c);
            }
        };
        add(a, 0, z0, n0);
        add(b, 1, z1, n1);
        // the rings never cross, so one vertex decides containment; the parent is the smallest container
        for (AnnulusContour& c : all) {
            const TitleVec2& p = c.ring->pts[0];
            for (AnnulusContour& d : all) {
                if (&d != &c && d.area > c.area && (!c.parent || d.area < c.parent->area) && insideContour(d.ring->pts, p.x, p.y)) c.parent = &d;
            }
        }
        for (AnnulusContour& c : all) {
            if (c.parent) c.parent->children.push_back(&c);
        }
        std::function<bool(const AnnulusContour*, int)> inSet = [&](const AnnulusContour* c, int k) -> bool {
            if (!c) return false;
            return c->set == k ? c->outer : inSet(c->parent, k);
        };
        for (const AnnulusContour& c : all) {
            if (!inSet(&c, 1) || inSet(&c, 0)) continue;
            std::vector<const AnnulusContour*> contours = {&c};
            contours.insert(contours.end(), c.children.begin(), c.children.end());
            std::vector<Contour> holes;
            for (size_t k = 1; k < contours.size(); k++) holes.push_back(contours[k]->ring->pts);
            const std::vector<uint32_t> tris = triangulatePolygon(c.ring->pts, holes);
            std::vector<AnnulusVertex> verts;
            for (const AnnulusContour* h : contours) {
                for (size_t i = 0; i < h->ring->pts.size(); i++) verts.push_back({h->ring->pts[i], h, i});
            }
            double area = c.area;
            for (const AnnulusContour* h : c.children) area -= h->area;
            if (!forceFallback && std::abs(trianglesArea(verts, tris) - area) <= 1e-3 * area) {
                emitAnnulusTris(mesh, verts, tris, slot);
                continue;
            }
            // two contours touch: cut the region out with Clipper and triangulate its clean pieces;
            // every vertex takes z and normal from its nearest ring
            if (stats) stats->fallbackUsed = true;
            for (const ShapeContours& piece : subtractContours(c.ring->pts, holes, eps)) {
                const std::vector<uint32_t> pieceTris = triangulatePolygon(piece.outer, piece.holes);
                std::vector<AnnulusVertex> pv;
                for (const TitleVec2& p : piece.outer) pv.push_back(nearestRingVertex(contours, p));
                for (const Contour& h : piece.holes) {
                    for (const TitleVec2& p : h) pv.push_back(nearestRingVertex(contours, p));
                }
                emitAnnulusTris(mesh, pv, pieceTris, slot);
            }
        }
    }

    // Ring offsets of an outward profile: offsets closer than tol to the previous ring or to the
    // outermost one join the nearest ring (their spans become risers)
    std::function<double(double)> ringLevels(const std::vector<ProfileSpan>& spans, double tol) {
        std::vector<double> offs;
        for (const ProfileSpan& s : spans) {
            offs.push_back(s.a.off);
            offs.push_back(s.b.off);
        }
        std::sort(offs.begin(), offs.end());
        offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
        const double last = offs.back();
        std::vector<double> levels = {offs.front()};
        for (double o : offs) {
            if (o - levels.back() >= tol && last - o >= tol) levels.push_back(o);
        }
        if (last > offs.front()) levels.push_back(last);
        return [levels](double off) {
            double best = levels.front();
            for (double l : levels) {
                if (std::abs(l - off) < std::abs(best - off)) best = l;
            }
            return best;
        };
    }

    constexpr double PI = 3.14159265358979323846;

}

ProfilePoint jasperengine::titleart::three_d::bandSpans(std::vector<ProfileSpan>& out, int slot, double off0, double z0, double width,
                                                       double depth, const BevelShape& shape, int segments) {
    const std::vector<UnitPiece> pieces = unitBevel(shape, segments);
    // analytic normals need a non-degenerate scaled tangent
    const bool smooth = width > 1e-6 && depth > 1e-6;
    ProfilePoint end{off0, z0};
    for (const UnitPiece& piece : pieces) {
        std::vector<ProfilePoint> pts;
        pts.reserve(piece.size());
        for (const UnitPoint& u : piece) {
            ProfilePoint p;
            p.off = off0 + width * u.o;
            p.z = z0 - depth * u.h;
            if (u.smooth && smooth) {
                // tangent (w do, -d dh) in (radial, z) -> outward normal (d dh, w do)
                const auto n = norm2(depth * u.th, width * u.to);
                p.hasNormal = true;
                p.nr = n[0];
                p.nz = n[1];
            }
            pts.push_back(p);
        }
        for (size_t k = 0; k + 1 < pts.size(); k++) {
            const ProfilePoint& a = pts[k];
            const ProfilePoint& b = pts[k + 1];
            if (std::abs(b.off - a.off) < 1e-6 && std::abs(b.z - a.z) < 1e-6) continue;
            out.push_back({slot, a, b});
        }
        if (!pts.empty()) end = pts.back();
    }
    end.hasNormal = false;
    return end;
}

void jasperengine::titleart::three_d::extrudeSolid(RawMesh& mesh, const Shapes& shapes, const SolidSpec& spec) {
    bool needIn = spec.capOff < 0, needOut = spec.capOff > 0;
    for (const ProfileSpan& s : spec.spans) {
        needIn = needIn || s.a.off < 0 || s.b.off < 0;
        needOut = needOut || s.a.off > 0 || s.b.off > 0;
    }
    ProfilePoint last;
    if (!spec.spans.empty()) {
        last = spec.spans.back().b;
    } else {
        last.off = spec.capOff;
        last.z = spec.capZ;
    }
    const bool rayClamp = (spec.testFlags & MESHER_TEST_NO_RAY_CLAMP) == 0;
    for (const ShapeContours& shape : shapes) {
        std::vector<Ring> rings;
        rings.reserve(1 + shape.holes.size());
        rings.push_back(makeRing(shape.outer));
        for (const Contour& h : shape.holes) rings.push_back(makeRing(h));
        computeLimits(rings, needIn, needOut, rayClamp);
        if (rayClamp) {
            untangle(rings, spec.capOff);
            untangle(rings, last.off);
        }

        // caps: front at the cap ring, back at the wall ring, each triangulated on its own ring
        // (the outline's triangulation when the ring's misses part of it)
        const bool onOutline = (spec.testFlags & MESHER_TEST_CAP_ON_OUTLINE) != 0;
        const std::vector<Contour> base = ringPoints(rings, 0.0);
        const std::vector<Contour> front = ringPoints(rings, spec.capOff);
        emitCap(mesh, front, onOutline ? base : front, spec.capSlot, spec.capZ, 1, &base);
        if (!spec.open) {
            const std::vector<Contour> back = ringPoints(rings, last.off);
            emitCap(mesh, back, onOutline ? base : back, spec.backSlot, spec.zBack, -1, &base);
        }
        for (const ProfileSpan& s : spec.spans) emitStrip(mesh, rings, s.slot, s.a, s.b);
        if (!spec.open && last.z - spec.zBack > 1e-4) {
            ProfilePoint top{last.off, last.z, true, 1, 0};
            ProfilePoint bottom{last.off, spec.zBack, true, 1, 0};
            emitStrip(mesh, rings, spec.wallSlot, top, bottom);
        }
    }
}

void jasperengine::titleart::three_d::extrudeOutward(RawMesh& mesh, const Shapes& shapes, const OutwardSpec& spec, ExtrudeStats* stats) {
    if (shapes.empty() || spec.spans.empty()) return;
    const ProfilePoint last = spec.spans.back().b;
    std::vector<TitleVec2> marks;
    marks.reserve(shapes.size());
    for (const ShapeContours& s : shapes) marks.push_back(interiorPoint(s));
    const double tol = 4 * spec.eps + 0.02;    // simplification + Clipper's 0.01 grid
    const auto level = ringLevels(spec.spans, tol);
    for (const ShapeContours& component : offsetShapes(shapes, last.off, false, false, spec.eps)) {
        Shapes group;
        for (size_t k = 0; k < shapes.size(); k++) {
            if (shapeContains(component, marks[k].x, marks[k].y)) group.push_back(shapes[k]);
        }
        if (group.empty()) continue;
        if (spec.span) mesh.span = spec.span(component);
        std::map<double, std::shared_ptr<OutRing>> rings;
        rings[0.0] = outRing(group);
        rings[last.off] = outRing({component});
        auto ringAt = [&](double off) -> const OutRing& {
            auto it = rings.find(off);
            if (it == rings.end()) it = rings.emplace(off, outRing(offsetShapes(group, off, false, false, spec.eps))).first;
            return *it->second;
        };
        // ring offsets from o0 (excluded) to o1, halving steps whose rings differ in shape
        std::function<void(double, double, std::vector<double>&)> steps = [&](double o0, double o1, std::vector<double>& out) {
            if (o1 - o0 > std::max(spec.step, 2 * tol) && !within(ringAt(o0), ringAt(o1), MAX_MITER * (o1 - o0) * 1.05 + tol)) {
                const double m = (o0 + o1) / 2;
                steps(o0, m, out);
                steps(m, o1, out);
            } else {
                out.push_back(o1);
            }
        };
        for (const ProfileSpan& s : spec.spans) {
            const auto nf = norm2(s.a.z - s.b.z, s.b.off - s.a.off);
            const auto nA = profileNormal(s.a, nf);
            const auto nB = profileNormal(s.b, nf);
            const double o0 = level(s.a.off);
            const double o1 = level(s.b.off);
            if (o1 - o0 < 1e-9) {
                // a riser, or a span narrower than the ring precision: along one ring
                emitStrip(mesh, ringAt(o0).rings, s.slot, ProfilePoint{0, s.a.z, true, nA[0], nA[1]}, ProfilePoint{0, s.b.z, true, nB[0], nB[1]});
                continue;
            }
            struct At {
                double z;
                std::array<double, 2> n;
            };
            auto at = [&](double off) -> At {
                const double t = (off - o0) / (o1 - o0);
                return {s.a.z + (s.b.z - s.a.z) * t, norm2(nA[0] + (nB[0] - nA[0]) * t, nA[1] + (nB[1] - nA[1]) * t)};
            };
            std::vector<double> offs = {o0};
            steps(o0, o1, offs);
            for (size_t k = 1; k < offs.size(); k++) {
                const At p0 = k == 1 ? At{s.a.z, nA} : at(offs[k - 1]);
                const At p1 = k == offs.size() - 1 ? At{s.b.z, nB} : at(offs[k]);
                emitAnnulus(mesh, ringAt(offs[k - 1]), ringAt(offs[k]), s.slot, p0.z, p1.z, p0.n, p1.n, spec.eps, stats,
                            (spec.testFlags & MESHER_TEST_FORCE_FALLBACK) != 0);
                if (stats) stats->ringLevels++;
            }
        }
        const OutRing& outer = ringAt(last.off);
        if (last.z - spec.zBack > 1e-4) {
            emitStrip(mesh, outer.rings, spec.wallSlot, ProfilePoint{0, last.z, true, 1, 0}, ProfilePoint{0, spec.zBack, true, 1, 0});
        }
        for (const ShapeContours& s : outer.shapes) {
            std::vector<Contour> ring = {s.outer};
            ring.insert(ring.end(), s.holes.begin(), s.holes.end());
            emitCap(mesh, ring, ring, spec.backSlot, spec.zBack, -1);
        }
    }
}

int jasperengine::titleart::three_d::orbGridFor(double radiusPx) {
    // facet angle <= (pi/2)/n near a face centre; sagitta ~ r theta^2 / 8 <= 1/8 -> n >= (pi/2) sqrt(r)
    const double n = (PI / 2) * std::sqrt(std::max(0.0, radiusPx));
    return std::max(24, std::min(128, 8 * (int)std::ceil(n / 8)));
}

void jasperengine::titleart::three_d::extrudeOrb(RawMesh& mesh, int slot, double rx, double ry, double rz, double cx, double cy, double cz,
                                                int grid) {
    // cube faces: outward axis n and in-face axes u, v with u x v = n (counter-clockwise from outside)
    static const int FACES[6][3][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    const int n = std::max(1, grid);
    const int side = n + 1;
    std::vector<double> pos((size_t)side * side * 3), nrm((size_t)side * side * 3);
    for (const auto& face : FACES) {
        const int* f = face[0];
        const int* u = face[1];
        const int* v = face[2];
        for (int j = 0; j <= n; j++) {
            for (int i = 0; i <= n; i++) {
                // grid parameters from integers, so faces sharing a seam compute it bit for bit
                const double a = (double)(2 * i - n) / n;
                const double b = (double)(2 * j - n) / n;
                const double x = f[0] + a * u[0] + b * v[0];
                const double y = f[1] + a * u[1] + b * v[1];
                const double z = f[2] + a * u[2] + b * v[2];
                // spherified cube: far more even than normalising the cube point
                const double x2 = x * x, y2 = y * y, z2 = z * z;
                const double sx = x * std::sqrt(std::max(0.0, 1 - y2 / 2 - z2 / 2 + (y2 * z2) / 3));
                const double sy = y * std::sqrt(std::max(0.0, 1 - z2 / 2 - x2 / 2 + (z2 * x2) / 3));
                const double sz = z * std::sqrt(std::max(0.0, 1 - x2 / 2 - y2 / 2 + (x2 * y2) / 3));
                const size_t k = ((size_t)j * side + i) * 3;
                pos[k] = rx * sx + cx;
                pos[k + 1] = ry * sy + cy;
                pos[k + 2] = rz * sz + cz;
                // ellipsoid normal ~ (x / rx^2, y / ry^2, z / rz^2) at (rx sx, ry sy, rz sz)
                const double nx = sx / rx, ny = sy / ry, nz = sz / rz;
                double l = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (l == 0) l = 1;
                nrm[k] = nx / l;
                nrm[k + 1] = ny / l;
                nrm[k + 2] = nz / l;
            }
        }
        auto put = [&](int i, int j) {
            const size_t k = ((size_t)j * side + i) * 3;
            mesh.push(slot, pos[k], pos[k + 1], pos[k + 2], nrm[k], nrm[k + 1], nrm[k + 2]);
        };
        for (int j = 0; j < n; j++) {
            for (int i = 0; i < n; i++) {
                // the diagonal symmetric about the face centre (no directional facet pattern)
                if ((2 * i < n) == (2 * j < n)) {
                    put(i, j); put(i + 1, j); put(i + 1, j + 1);
                    put(i, j); put(i + 1, j + 1); put(i, j + 1);
                } else {
                    put(i, j); put(i + 1, j); put(i, j + 1);
                    put(i + 1, j); put(i + 1, j + 1); put(i, j + 1);
                }
            }
        }
    }
}

RingLimits jasperengine::titleart::three_d::rayLimits(const std::vector<Contour>& contours, bool bruteForce) {
    std::vector<Ring> rings;
    for (const Contour& c : contours) rings.push_back(makeRing(c));
    RingLimits out;
    std::unique_ptr<EdgeGrid> grid;
    if (!bruteForce) grid = std::make_unique<EdgeGrid>(rings);
    for (size_t ri = 0; ri < rings.size(); ri++) {
        const Ring& r = rings[ri];
        std::vector<double> in(r.pts.size()), outw(r.pts.size());
        for (size_t i = 0; i < r.pts.size(); i++) {
            const V2 m = r.miter[i];
            double ml = std::hypot(m.x, m.y);
            if (ml == 0) ml = 1;
            const double hin = bruteForce ? rayHitBrute(rings, ri, i, -m.x / ml, -m.y / ml) : grid->hit(ri, i, -m.x / ml, -m.y / ml);
            const double hout = bruteForce ? rayHitBrute(rings, ri, i, m.x / ml, m.y / ml) : grid->hit(ri, i, m.x / ml, m.y / ml);
            in[i] = RAY_CLAMP * hin / ml;
            outw[i] = RAY_CLAMP * hout / ml;
        }
        out.maxIn.push_back(std::move(in));
        out.maxOut.push_back(std::move(outw));
    }
    return out;
}
