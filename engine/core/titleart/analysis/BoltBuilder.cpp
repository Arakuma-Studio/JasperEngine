// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/energy/lightning.ts (findEdges, bolt, lightningBolts,
// edgeArcs, drawBolts) by SafEight, used with the author's permission.

#include "BoltBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;

    // the stream of bolt or arc `i` (b/common.ts rng(hash(seed, i, salt) * 1e6 + i))
    gen::Rng boltStream(double seed, int i, double salt) {
        return gen::Rng(gen::hashN({seed, (double)i, salt}) * 1e6 + (double)i, gen::Rng::Seeding::EffectsB);
    }

    // the first edge nearest (x, y)
    size_t nearestEdge(const std::vector<BoltEdge>& edges, double x, double y) {
        size_t best = 0;
        double bd = std::numeric_limits<double>::infinity();
        for (size_t k = 0; k < edges.size(); k++) {
            const double dx = edges[k].x - x, dy = edges[k].y - y;
            const double d = dx * dx + dy * dy;
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        return best;
    }

    struct EdgeBox {
        double x0 = std::numeric_limits<double>::infinity(), y0 = std::numeric_limits<double>::infinity();
        double x1 = -std::numeric_limits<double>::infinity(), y1 = -std::numeric_limits<double>::infinity();
    };

    EdgeBox boxOf(const std::vector<BoltEdge>& edges) {
        EdgeBox b;
        for (const BoltEdge& e : edges) {
            b.x0 = std::min(b.x0, e.x);
            b.y0 = std::min(b.y0, e.y);
            b.x1 = std::max(b.x1, e.x);
            b.y1 = std::max(b.y1, e.y);
        }
        return b;
    }

    // JavaScript's Math.round
    double jsRound(double v) {
        return std::floor(v + 0.5);
    }

    // Pachislop lightningBolts: outward and crawling bolts. Every r() is its own statement where
    // JavaScript evaluates two in one expression, so the stream is drawn in its order
    std::vector<BoltSegment> lightningBolts(const std::vector<BoltEdge>& edges, const BoltParams& p) {
        const double len = p.length;
        const double jag = p.jaggedness;
        std::vector<BoltSegment> segs;
        // start points by position (not list index), so bolts land in the same place at any scale
        const EdgeBox box = boxOf(edges);
        for (int i = 0; i < p.count; i++) {
            gen::Rng r = boltStream(p.seed, i, 0.5);
            const double qx = r();
            const double qy = r();
            const size_t ei = nearestEdge(edges, box.x0 + qx * (box.x1 - box.x0), box.y0 + qy * (box.y1 - box.y0));
            const BoltEdge& e = edges[ei];
            const bool crawl = p.mode == BoltMode::Crawl || (p.mode == BoltMode::Mixed && r() < 0.45);
            if (crawl) {
                // arc to another edge point roughly `len` away along the edge, bulging outward
                const double tx = -e.ny, ty = e.nx;
                const double sign = r() < 0.5 ? -1.0 : 1.0;
                const double want = len * (0.5 + 0.5 * r());
                const size_t bi = nearestEdge(edges, e.x + tx * sign * want + e.nx * want * 0.2, e.y + ty * sign * want + e.ny * want * 0.2);
                if (bi == ei) continue;
                const BoltEdge& best = edges[bi];
                // bulge toward the outside of the shape (Math.sign(...) || 1)
                const double dx = best.x - e.x, dy = best.y - e.y;
                const double s = -dy * (e.nx + best.nx) + dx * (e.ny + best.ny);
                const double side = s < 0.0 ? -1.0 : 1.0;
                appendBolt(segs, r, e.x, e.y, best.x, best.y, jag, 0.22 * side, 1.0, 1.0, 6, p.branches * 0.12);
            } else {
                const double ang = (r() - 0.5) * 75.0 * DEG;
                const double c = std::cos(ang), s = std::sin(ang);
                const double dx = e.nx * c - e.ny * s;
                const double dy = e.nx * s + e.ny * c;
                const double l = len * (0.55 + 0.45 * r());
                appendBolt(segs, r, e.x, e.y, e.x + dx * l, e.y + dy * l, jag, 0.0, 1.0, 1.0, 6, p.branches * 0.2);
            }
        }
        return segs;
    }

    int64_t cellKey(int64_t gx, int64_t gy) {
        return gx + gy * 65536;
    }

    // Pachislop edgeArcs (Electrify): arcs that walk the edge from cell to cell, hovering just
    // outside it, wrinkled at two scales and forking outward at random steps
    std::vector<BoltSegment> edgeArcs(const std::vector<BoltEdge>& edges, const BoltParams& p) {
        const double cell = p.cell;
        const double step = 2.0 * cell;
        const double len = p.length;
        const double wrinkle = p.wrinkle;
        const double forkP = p.branchiness * 0.3;
        std::unordered_map<int64_t, size_t> grid;
        for (size_t k = 0; k < edges.size(); k++) {
            grid[cellKey((int64_t)std::floor(edges[k].x / cell), (int64_t)std::floor(edges[k].y / cell))] = k;
        }
        const EdgeBox box = boxOf(edges);
        std::vector<BoltSegment> segs;
        for (int i = 0; i < p.arcs; i++) {
            gen::Rng r = boltStream(p.seed, i, 0.25);
            const double rx = r();
            const double ry = r();
            size_t ei = nearestEdge(edges, box.x0 + rx * (box.x1 - box.x0), box.y0 + ry * (box.y1 - box.y0));
            const double sign = r() < 0.5 ? -1.0 : 1.0;
            double tx = -edges[ei].ny * sign;
            double ty = edges[ei].nx * sign;
            const int steps = (int)std::max(2.0, jsRound((len * (0.5 + 0.5 * r())) / step));
            const double inten = 0.7 + 0.3 * r();
            double off = r() * wrinkle * step * 0.75;
            std::vector<double> pts = {edges[ei].x + edges[ei].nx * off, edges[ei].y + edges[ei].ny * off};
            for (int s = 0; s < steps; s++) {
                // the next edge cell nearest one step ahead, never behind
                const BoltEdge& e = edges[ei];
                const double wx = e.x + tx * step;
                const double wy = e.y + ty * step;
                const int64_t gx = (int64_t)std::floor(wx / cell);
                const int64_t gy = (int64_t)std::floor(wy / cell);
                size_t next = edges.size();
                double nd = std::numeric_limits<double>::infinity();
                for (int dy = -2; dy <= 2; dy++) {
                    for (int dx = -2; dx <= 2; dx++) {
                        auto hit = grid.find(cellKey(gx + dx, gy + dy));
                        if (hit == grid.end() || hit->second == ei) continue;
                        const BoltEdge& f = edges[hit->second];
                        if ((f.x - e.x) * tx + (f.y - e.y) * ty <= 0.0) continue;
                        const double fx = f.x - wx, fy = f.y - wy;
                        const double d = fx * fx + fy * fy;
                        if (d < nd) {
                            nd = d;
                            next = hit->second;
                        }
                    }
                }
                if (next == edges.size()) break;
                // the new heading: the step just taken, nudged toward the new cell's tangent
                const BoltEdge& n = edges[next];
                const double hx = n.x - e.x, hy = n.y - e.y;
                double hl = std::hypot(hx, hy);
                if (hl == 0.0) hl = 1.0;
                const double ts = -n.ny * hx + n.nx * hy < 0.0 ? -1.0 : 1.0;
                tx = hx / hl - n.ny * ts;
                ty = hy / hl + n.nx * ts;
                double tl = std::hypot(tx, ty);
                if (tl == 0.0) tl = 1.0;
                tx /= tl;
                ty /= tl;
                ei = next;
                off = std::min(std::max(off + (r() - 0.5) * wrinkle * step * 0.6, 0.0), wrinkle * step * 1.5);
                const double px = n.x + n.nx * off;
                const double py = n.y + n.ny * off;
                pts.push_back(px);
                pts.push_back(py);
                if (r() < forkP) {
                    const double a = (r() - 0.5) * 80.0 * DEG;
                    const double c = std::cos(a), sn = std::sin(a);
                    const double l = step * (1.0 + 3.0 * r()) * (0.5 + wrinkle);
                    appendBolt(segs, r, px, py, px + (n.nx * c - n.ny * sn) * l, py + (n.nx * sn + n.ny * c) * l, 0.8 * wrinkle, 0.0, 0.6, inten * 0.75,
                               4, 0.0);
                }
            }
            if (pts.size() < 4) continue;
            // midpoint jag between the steps, halving per level
            double amp = wrinkle * step * 0.3;
            for (int lvl = 0; lvl < 2; lvl++) {
                std::vector<double> fine = {pts[0], pts[1]};
                for (size_t k = 0; k + 3 < pts.size(); k += 2) {
                    const double ax = pts[k], ay = pts[k + 1], bx = pts[k + 2], by = pts[k + 3];
                    double l = std::hypot(bx - ax, by - ay);
                    if (l == 0.0) l = 1.0;
                    const double o = (r() - 0.5) * 2.0 * amp;
                    fine.push_back((ax + bx) / 2.0 - ((by - ay) / l) * o);
                    fine.push_back((ay + by) / 2.0 + ((bx - ax) / l) * o);
                    fine.push_back(bx);
                    fine.push_back(by);
                }
                pts = std::move(fine);
                amp *= 0.5;
            }
            // one continuous arc, thinning toward both ends
            const int n = (int)(pts.size() / 2) - 1;
            for (int k = 0; k < n; k++) {
                BoltSegment seg;
                seg.ax = pts[2 * k];
                seg.ay = pts[2 * k + 1];
                seg.bx = pts[2 * k + 2];
                seg.by = pts[2 * k + 3];
                seg.width = 1.0 - 0.5 * std::abs((2.0 * (k + 0.5)) / n - 1.0);
                seg.intensity = inten;
                segs.push_back(seg);
            }
        }
        return segs;
    }

}

std::vector<BoltEdge> analysis::findBoltEdges(const std::vector<float>& alpha, int w, int h, double px0, double py0, double cell) {
    std::vector<BoltEdge> edges;
    if (w <= 0 || h <= 0 || alpha.size() < (size_t)w * (size_t)h) return edges;
    auto at = [&](int x, int y) -> double {
        return alpha[(size_t)std::min(h - 1, std::max(0, y)) * (size_t)w + (size_t)std::min(w - 1, std::max(0, x))];
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (at(x, y) < 0.5) continue;
            if (at(x - 1, y) >= 0.5 && at(x + 1, y) >= 0.5 && at(x, y - 1) >= 0.5 && at(x, y + 1) >= 0.5) continue;
            // Sobel gradient of coverage; outward = -gradient
            const double gx = at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1);
            const double gy = at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1);
            const double l = std::hypot(gx, gy);
            if (l < 1e-4) continue;
            edges.push_back({px0 + (x + 0.5) * cell, py0 + (y + 0.5) * cell, -gx / l, -gy / l});
        }
    }
    return edges;
}

std::vector<BoltSegment> analysis::buildBolts(const std::vector<BoltEdge>& edges, const BoltParams& params) {
    if (edges.empty()) return {};
    return params.mode == BoltMode::Edge ? edgeArcs(edges, params) : lightningBolts(edges, params);
}

void analysis::appendBolt(std::vector<BoltSegment>& out, gen::Rng& r, double ax, double ay, double bx, double by, double jag, double bulge,
                          double width, double intensity, int depth, double branchProb) {
    std::vector<double> pts = {ax, ay, bx, by};
    const double len = std::hypot(bx - ax, by - ay);
    if (len < 1.0) return;
    const double px = -(by - ay) / len;
    const double py = (bx - ax) / len;
    double amp = jag * len * 0.35;
    for (int lvl = 0; lvl < depth; lvl++) {
        std::vector<double> next;
        next.reserve(pts.size() * 2);
        for (size_t i = 0; i + 2 < pts.size(); i += 2) {
            const double x0 = pts[i], y0 = pts[i + 1], x1 = pts[i + 2], y1 = pts[i + 3];
            const double off = (r() - 0.5) * 2.0 * amp + (lvl == 0 ? bulge * len : 0.0);
            next.push_back(x0);
            next.push_back(y0);
            next.push_back((x0 + x1) / 2.0 + px * off);
            next.push_back((y0 + y1) / 2.0 + py * off);
        }
        next.push_back(pts[pts.size() - 2]);
        next.push_back(pts[pts.size() - 1]);
        pts = std::move(next);
        amp *= 0.55;
    }
    const int n = (int)(pts.size() / 2);
    for (int i = 0; i < n - 1; i++) {
        // slight taper towards the end of the bolt
        const double f = 1.0 - (0.5 * i) / n;
        out.push_back({pts[i * 2], pts[i * 2 + 1], pts[i * 2 + 2], pts[i * 2 + 3], width * f, intensity});
        if (branchProb > 0.0 && i > 1 && i < n - 3 && r() < branchProb) {
            const double dx = bx - ax;
            const double dy = by - ay;
            const double sign = r() < 0.5 ? -1.0 : 1.0;
            const double rot = sign * (0.35 + r() * 0.5);
            const double c = std::cos(rot);
            const double s = std::sin(rot);
            const double bl = (0.25 + r() * 0.3) * (1.0 - (double)i / n);
            const double sx = pts[i * 2];
            const double sy = pts[i * 2 + 1];
            appendBolt(out, r, sx, sy, sx + (dx * c - dy * s) * bl, sy + (dx * s + dy * c) * bl, jag, 0.0, width * 0.55, intensity * 0.7,
                       std::max(2, depth - 2), 0.0);
        }
    }
}

bool analysis::boltQuad(const BoltSegment& s, double coreWidth, BoltQuad& out) {
    const double dx = s.bx - s.ax, dy = s.by - s.ay;
    const double l = std::hypot(dx, dy);
    if (!(l > 0.0)) return false;
    // the profile reaches ~0 at |across| = 1.6; the quad runs half its width past both ends
    const double hw = std::max(0.7, coreWidth * s.width) * 1.6;
    out.cx = (s.ax + s.bx) * 0.5;
    out.cy = (s.ay + s.by) * 0.5;
    out.length = l + hw;
    out.halfWidth = hw;
    out.rotation = std::atan2(dy, dx);
    return true;
}
