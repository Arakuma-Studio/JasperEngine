// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/inflate.ts by SafEight, used with the author's permission.

#include "InflateMesher.h"

#include "TitleEarcut.h"
#include "titleart/TitleArtHash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    constexpr int SMOOTH_PASSES = 2;
    const double CREASE_COS = std::cos(32.0 * 3.14159265358979323846 / 180.0);
    constexpr double PHASE = 0.3183098861837907;    // grid phase (fraction of the step): outline vertices off the lines
    constexpr int DEGENERATE_RETRIES = 6;

    struct Piece {
        std::vector<uint32_t> pts;
        double sIn = 0, sOut = 0;   // cell-border parameters (0..4, counter-clockwise from the bottom-left corner)
    };

    std::shared_ptr<InflateField> buildField(const ShapeContours& shape, double h, double phase) {
        std::vector<const Contour*> contours = {&shape.outer};
        for (const Contour& c : shape.holes) contours.push_back(&c);
        double bx0 = INFINITY, by0 = INFINITY, bx1 = -INFINITY, by1 = -INFINITY;
        for (const Contour* c : contours) {
            for (const TitleVec2& p : *c) {
                bx0 = std::min(bx0, p.x);
                bx1 = std::max(bx1, p.x);
                by0 = std::min(by0, p.y);
                by1 = std::max(by1, p.y);
            }
        }
        const double x0 = bx0 - h * phase;
        const double y0 = by0 - h * phase;
        const long nx = (long)std::floor((bx1 - x0) / h) + 2;
        const long ny = (long)std::floor((by1 - y0) / h) + 2;
        auto X = [&](long i) { return x0 + (double)i * h; };
        auto Y = [&](long j) { return y0 + (double)j * h; };
        auto offLine = [&](double v, double o) {
            const double f = (v - o) / h;
            return std::abs(f - std::round(f)) > 1e-7;
        };
        for (const Contour* c : contours) {
            for (const TitleVec2& p : *c) {
                if (!offLine(p.x, x0) || !offLine(p.y, y0)) return nullptr;
            }
        }

        auto field = std::make_shared<InflateField>();
        std::vector<double>& xy = field->xy;
        // per vertex: crossing kind (0 on vertical line `line`, 1 on a horizontal one), -1 outline vertex, -2 grid node
        std::vector<int> kind;
        std::vector<long> line;
        auto addVertex = [&](double x, double y, int k, long l) {
            xy.push_back(x);
            xy.push_back(y);
            kind.push_back(k);
            line.push_back(l);
            return (uint32_t)(kind.size() - 1);
        };
        std::vector<std::vector<double>> rowCross((size_t)ny);
        struct Crossing {
            double t;
            int k;
            long l;
            double x, y;
        };
        std::vector<Crossing> crossings;
        for (const Contour* c : contours) {
            std::vector<uint32_t> ring;
            const size_t n = c->size();
            for (size_t e = 0; e < n; e++) {
                const TitleVec2& a = (*c)[e];
                const TitleVec2& b = (*c)[(e + 1) % n];
                ring.push_back(addVertex(a.x, a.y, -1, -1));
                crossings.clear();
                if (a.x != b.x) {
                    const long lo = (long)std::ceil((std::min(a.x, b.x) - x0) / h);
                    const long hi = (long)std::floor((std::max(a.x, b.x) - x0) / h);
                    for (long i = lo; i <= hi; i++) {
                        const double t = (X(i) - a.x) / (b.x - a.x);
                        if (t <= 0 || t >= 1) continue;
                        crossings.push_back({t, 0, i, X(i), a.y + t * (b.y - a.y)});
                    }
                }
                if (a.y != b.y) {
                    const long lo = (long)std::ceil((std::min(a.y, b.y) - y0) / h);
                    const long hi = (long)std::floor((std::max(a.y, b.y) - y0) / h);
                    for (long j = lo; j <= hi; j++) {
                        const double t = (Y(j) - a.y) / (b.y - a.y);
                        if (t <= 0 || t >= 1) continue;
                        crossings.push_back({t, 1, j, a.x + t * (b.x - a.x), Y(j)});
                    }
                }
                std::stable_sort(crossings.begin(), crossings.end(), [](const Crossing& p, const Crossing& q) { return p.t < q.t; });
                for (size_t q = 0; q < crossings.size(); q++) {
                    const Crossing& cr = crossings[q];
                    if (q > 0 && cr.t - crossings[q - 1].t < 1e-12) return nullptr;     // through a grid node
                    ring.push_back(addVertex(cr.x, cr.y, cr.k, cr.l));
                    if (cr.k == 1) rowCross[(size_t)cr.l].push_back(cr.x);
                }
            }
            field->rings.push_back(std::move(ring));
        }
        const size_t boundaryCount = kind.size();
        for (auto& r : rowCross) std::sort(r.begin(), r.end());

        // outline pieces per cell (cell id = row x nx + column)
        std::map<long, std::vector<Piece>> pieces;
        std::map<long, std::vector<std::vector<uint32_t>>> fulls;
        auto borderS = [&](uint32_t v, long ci, long cj) -> double {
            const double x = xy[2 * v], y = xy[2 * v + 1];
            if (kind[v] == 0) {
                if (line[v] == ci) return 3 + (Y(cj + 1) - y) / h;
                if (line[v] == ci + 1) return 1 + (y - Y(cj)) / h;
            } else if (kind[v] == 1) {
                if (line[v] == cj) return (x - X(ci)) / h;
                if (line[v] == cj + 1) return 2 + (X(ci + 1) - x) / h;
            }
            return NAN;
        };
        for (const std::vector<uint32_t>& ring : field->rings) {
            const size_t n = ring.size();
            long start = -1;
            for (size_t k = 0; k < n; k++) {
                if (kind[ring[k]] >= 0) {
                    start = (long)k;
                    break;
                }
            }
            auto mid = [&](size_t k) {
                const uint32_t a = ring[k % n], b = ring[(k + 1) % n];
                return (long)std::floor(((xy[2 * a + 1] + xy[2 * b + 1]) / 2 - y0) / h) * nx +
                       (long)std::floor(((xy[2 * a] + xy[2 * b]) / 2 - x0) / h);
            };
            if (start < 0) {
                fulls[mid(0)].push_back(ring);
                continue;
            }
            size_t k = (size_t)start;
            do {
                const long cell = mid(k);
                Piece piece;
                piece.pts.push_back(ring[k]);
                size_t m = k + 1;
                while (kind[ring[m % n]] < 0) piece.pts.push_back(ring[m++ % n]);
                piece.pts.push_back(ring[m % n]);
                const long ci = cell % nx;
                const long cj = (cell - ci) / nx;
                piece.sIn = borderS(piece.pts.front(), ci, cj);
                piece.sOut = borderS(piece.pts.back(), ci, cj);
                if (std::isnan(piece.sIn) || std::isnan(piece.sOut)) return nullptr;
                pieces[cell].push_back(std::move(piece));
                k = m % n;
            } while (k != (size_t)start);
        }

        // grid nodes
        std::vector<int64_t> nodeVertex((size_t)(nx * ny), -1);
        auto node = [&](long i, long j) {
            const size_t id = (size_t)(j * nx + i);
            if (nodeVertex[id] < 0) {
                nodeVertex[id] = addVertex(X(i), Y(j), -2, -1);
                field->nodes++;
            }
            return (uint32_t)nodeVertex[id];
        };
        auto nodeInside = [&](long i, long j) {
            const std::vector<double>& row = rowCross[(size_t)j];
            const double x = X(i);
            const size_t lo = (size_t)(std::lower_bound(row.begin(), row.end(), x) - row.begin());
            return (lo & 1) == 1;
        };

        // triangles
        std::vector<uint32_t>& tris = field->tris;
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            const double ax = xy[2 * a], ay = xy[2 * a + 1];
            const double cross = (xy[2 * b] - ax) * (xy[2 * c + 1] - ay) - (xy[2 * b + 1] - ay) * (xy[2 * c] - ax);
            if (cross > 0) tris.insert(tris.end(), {a, b, c});
            else if (cross < 0) tris.insert(tris.end(), {a, c, b});
        };
        auto vec = [&](uint32_t v) { return TitleVec2{xy[2 * v], xy[2 * v + 1]}; };
        auto polyArea = [&](const std::vector<uint32_t>& poly) {
            double s = 0;
            for (size_t i = 0, n = poly.size(); i < n; i++) {
                const uint32_t a = poly[i], b = poly[(i + 1) % n];
                s += xy[2 * a] * xy[2 * b + 1] - xy[2 * b] * xy[2 * a + 1];
            }
            return s / 2;
        };
        auto polyContains = [&](const std::vector<uint32_t>& poly, double x, double y) {
            bool inside = false;
            for (size_t i = 0, n = poly.size(), j = n - 1; i < n; j = i++) {
                const double ax = xy[2 * poly[i]], ay = xy[2 * poly[i] + 1];
                const double bx = xy[2 * poly[j]], by = xy[2 * poly[j] + 1];
                if ((ay > y) != (by > y) && x < ((bx - ax) * (y - ay)) / (by - ay) + ax) inside = !inside;
            }
            return inside;
        };
        for (long cj = 0; cj < ny - 1; cj++) {
            for (long ci = 0; ci < nx - 1; ci++) {
                const long cell = cj * nx + ci;
                auto listIt = pieces.find(cell);
                auto fullIt = fulls.find(cell);
                const std::vector<std::vector<uint32_t>>* full = fullIt != fulls.end() ? &fullIt->second : nullptr;
                std::vector<std::vector<uint32_t>> polys;
                if (listIt == pieces.end()) {
                    if (!nodeInside(ci, cj)) {
                        if (!full) continue;
                    } else if (!full) {
                        // whole cell inside: alternate the diagonal so the facets have no common direction
                        const uint32_t a = node(ci, cj), b = node(ci + 1, cj), c = node(ci + 1, cj + 1), d = node(ci, cj + 1);
                        if ((ci + cj) & 1) {
                            tri(a, b, c);
                            tri(a, c, d);
                        } else {
                            tri(a, b, d);
                            tri(b, c, d);
                        }
                        continue;
                    } else {
                        polys.push_back({node(ci, cj), node(ci + 1, cj), node(ci + 1, cj + 1), node(ci, cj + 1)});
                    }
                } else {
                    // trace cell & shape: an outline piece, then counter-clockwise along the border
                    // (picking up corners) to the next piece's entry, until back at the first piece
                    const std::vector<Piece>& list = listIt->second;
                    auto corner = [&](long c) {
                        return c == 0 ? node(ci, cj) : c == 1 ? node(ci + 1, cj) : c == 2 ? node(ci + 1, cj + 1) : node(ci, cj + 1);
                    };
                    std::vector<char> used(list.size(), 0);
                    for (size_t p0 = 0; p0 < list.size(); p0++) {
                        if (used[p0]) continue;
                        std::vector<uint32_t> poly;
                        size_t p = p0;
                        for (;;) {
                            used[p] = 1;
                            poly.insert(poly.end(), list[p].pts.begin(), list[p].pts.end());
                            const double s = list[p].sOut;
                            long next = -1;
                            double gap = INFINITY;
                            for (size_t q = 0; q < list.size(); q++) {
                                double d = list[q].sIn - s;
                                if (d <= 0) d += 4;
                                if (d < gap) {
                                    gap = d;
                                    next = (long)q;
                                }
                            }
                            for (int k = 1; k <= 4; k++) {
                                const long c = (long)std::floor(s) + k;
                                if ((double)c - s >= gap) break;
                                poly.push_back(corner(c % 4));
                            }
                            if (next == (long)p0) break;
                            if (used[(size_t)next]) return nullptr;
                            p = (size_t)next;
                        }
                        polys.push_back(std::move(poly));
                    }
                }
                // contours wholly inside the cell: islands are polygons, holes go into the smallest polygon around them
                std::vector<std::vector<std::vector<uint32_t>>> holes(polys.size());
                if (full) {
                    for (const auto& f : *full) {
                        if (polyArea(f) > 0) {
                            polys.push_back(f);
                            holes.emplace_back();
                        }
                    }
                    for (const auto& f : *full) {
                        if (polyArea(f) > 0) continue;
                        const double x = xy[2 * f[0]], y = xy[2 * f[0] + 1];
                        long best = -1;
                        double bestArea = INFINITY;
                        for (size_t k = 0; k < polys.size(); k++) {
                            const double a = polyArea(polys[k]);
                            if (a < bestArea && polyContains(polys[k], x, y)) {
                                best = (long)k;
                                bestArea = a;
                            }
                        }
                        if (best >= 0) holes[(size_t)best].push_back(f);
                    }
                }
                for (size_t k = 0; k < polys.size(); k++) {
                    std::vector<uint32_t> ids = polys[k];
                    Contour outer;
                    for (uint32_t v : polys[k]) outer.push_back(vec(v));
                    std::vector<Contour> hc;
                    for (const auto& hl : holes[k]) {
                        Contour c;
                        for (uint32_t v : hl) {
                            c.push_back(vec(v));
                            ids.push_back(v);
                        }
                        hc.push_back(std::move(c));
                    }
                    const std::vector<uint32_t> faces = triangulatePolygon(outer, hc);
                    for (size_t t = 0; t + 2 < faces.size(); t += 3) tri(ids[faces[t]], ids[faces[t + 1]], ids[faces[t + 2]]);
                }
            }
        }

        // the distance of the grid nodes to the outline
        const size_t nv = kind.size();
        std::vector<double>& dist = field->dist;
        dist.assign(nv, 0.0);
        std::vector<double> segs;
        for (const Contour* c : contours) {
            for (size_t i = 0, n = c->size(); i < n; i++) {
                segs.insert(segs.end(), {(*c)[i].x, (*c)[i].y, (*c)[(i + 1) % n].x, (*c)[(i + 1) % n].y});
            }
        }
        const double bucket = std::max(4 * h, 1e-6);
        const long bw = std::max(1L, (long)std::ceil((bx1 - bx0) / bucket) + 1);
        const long bh = std::max(1L, (long)std::ceil((by1 - by0) / bucket) + 1);
        std::vector<std::vector<size_t>> buckets((size_t)(bw * bh));
        auto bClamp = [](double v, long n) { return std::min(n - 1, std::max(0L, (long)std::floor(v))); };
        for (size_t s = 0; s < segs.size(); s += 4) {
            const long i0 = bClamp((std::min(segs[s], segs[s + 2]) - bx0) / bucket, bw);
            const long i1 = bClamp((std::max(segs[s], segs[s + 2]) - bx0) / bucket, bw);
            const long j0 = bClamp((std::min(segs[s + 1], segs[s + 3]) - by0) / bucket, bh);
            const long j1 = bClamp((std::max(segs[s + 1], segs[s + 3]) - by0) / bucket, bh);
            for (long j = j0; j <= j1; j++) {
                for (long i = i0; i <= i1; i++) buckets[(size_t)(j * bw + i)].push_back(s);
            }
        }
        auto segT = [&](size_t s, double px, double py) {
            const double ax = segs[s], ay = segs[s + 1];
            const double ex = segs[s + 2] - ax, ey = segs[s + 3] - ay;
            const double l2 = ex * ex + ey * ey;
            return l2 > 0 ? std::clamp(((px - ax) * ex + (py - ay) * ey) / l2, 0.0, 1.0) : 0.0;
        };
        auto segDist2 = [&](size_t s, double px, double py) {
            const double t = segT(s, px, py);
            const double dx = segs[s] + t * (segs[s + 2] - segs[s]) - px, dy = segs[s + 1] + t * (segs[s + 3] - segs[s + 1]) - py;
            return dx * dx + dy * dy;
        };
        const long maxRing = std::max(bw, bh);
        std::vector<double>& grad = field->grad;
        grad.assign(nv * 2, 0.0);
        for (size_t v = boundaryCount; v < nv; v++) {
            const double px = xy[2 * v], py = xy[2 * v + 1];
            const long bi = bClamp((px - bx0) / bucket, bw);
            const long bj = bClamp((py - by0) / bucket, bh);
            double best = INFINITY;
            long bs = -1;
            for (long r = 0; r <= maxRing; r++) {
                for (long j = bj - r; j <= bj + r; j++) {
                    if (j < 0 || j >= bh) continue;
                    const bool edge = j == bj - r || j == bj + r;
                    for (long i = bi - r; i <= bi + r; i += edge ? 1 : std::max(1L, 2 * r)) {
                        if (i < 0 || i >= bw) continue;
                        for (size_t s : buckets[(size_t)(j * bw + i)]) {
                            const double d = segDist2(s, px, py);
                            if (d < best) {
                                best = d;
                                bs = (long)s;
                            }
                        }
                    }
                }
                if (best <= (double)r * bucket * (double)r * bucket) break;
            }
            dist[v] = std::sqrt(best);
            if (bs >= 0 && best > 0) {
                const size_t s = (size_t)bs;
                const double t = segT(s, px, py);
                grad[2 * v] = (px - segs[s] - t * (segs[s + 2] - segs[s])) / dist[v];
                grad[2 * v + 1] = (py - segs[s + 1] - t * (segs[s + 3] - segs[s + 1])) / dist[v];
            }
        }
        // on the outline: the inward normal averaged over the two edges (the shape is on the left)
        for (const std::vector<uint32_t>& ring : field->rings) {
            const size_t k = ring.size();
            for (size_t i = 0; i < k; i++) {
                const uint32_t p = ring[(i + k - 1) % k], v = ring[i], q = ring[(i + 1) % k];
                const double dx1 = xy[2 * v] - xy[2 * p], dy1 = xy[2 * v + 1] - xy[2 * p + 1];
                const double dx2 = xy[2 * q] - xy[2 * v], dy2 = xy[2 * q + 1] - xy[2 * v + 1];
                double l1 = std::hypot(dx1, dy1), l2 = std::hypot(dx2, dy2);
                if (l1 == 0) l1 = 1;
                if (l2 == 0) l2 = 1;
                const double gx = -dy1 / l1 - dy2 / l2, gy = dx1 / l1 + dx2 / l2;
                double l = std::hypot(gx, gy);
                if (l == 0) l = 1;
                grad[2 * v] = gx / l;
                grad[2 * v + 1] = gy / l;
            }
        }

        // relax the field (outline fixed at 0): rounds the ridges along the medial axis
        std::vector<double> sum(nv), cnt(nv);
        for (int pass = 0; pass < SMOOTH_PASSES; pass++) {
            std::fill(sum.begin(), sum.end(), 0.0);
            std::fill(cnt.begin(), cnt.end(), 0.0);
            for (size_t t = 0; t < tris.size(); t += 3) {
                for (int e = 0; e < 3; e++) {
                    const uint32_t a = tris[t + e], b = tris[t + (e + 1) % 3];
                    sum[a] += dist[b];
                    cnt[a]++;
                    sum[b] += dist[a];
                    cnt[b]++;
                }
            }
            for (size_t v = boundaryCount; v < nv; v++) {
                if (cnt[v] > 0) dist[v] = 0.5 * dist[v] + 0.5 * sum[v] / cnt[v];
            }
        }
        for (size_t v = boundaryCount; v < nv; v++) field->maxDist = std::max(field->maxDist, dist[v]);
        return field;
    }

    uint64_t shapeKey(const ShapeContours& shape, double step) {
        uint64_t h = hashShapes({shape});
        return hashCombine(h, std::hash<double>{}(step));
    }

}

double jasperengine::titleart::three_d::inflateScale(double outputScale) {
    return std::pow(2.0, std::round(std::log2(std::max(outputScale, 1e-6))));
}

double jasperengine::titleart::three_d::inflateStep(const Shapes& shapes, double outputScale, int detail, size_t maxNodes) {
    double area = 0, scale = 0;
    for (const ShapeContours& s : shapes) {
        double a = std::abs(signedArea(s.outer));
        double p = contourLength(s.outer);
        for (const Contour& h : s.holes) {
            a -= std::abs(signedArea(h));
            p += contourLength(h);
        }
        if (a <= 0 || p <= 0) continue;
        area += a;
        scale = std::max(scale, 1.5 * a / p);
    }
    if (area <= 0) return 1;
    return std::max({scale / (INFLATE_NODES_PER_INSET * std::clamp(detail, 1, 4)), std::sqrt(area / (double)std::max<size_t>(1, maxNodes)),
                     INFLATE_MIN_STEP_PX / std::max(outputScale, 1e-6)});
}

std::shared_ptr<const InflateField> jasperengine::titleart::three_d::buildInflateField(const ShapeContours& shape, double step) {
    // a crossing exactly on a grid node or an outline vertex exactly on a grid line has no
    // well-defined cell: shift the grid and try again
    for (int k = 0; k < DEGENERATE_RETRIES; k++) {
        if (auto field = buildField(shape, step, PHASE + k * 0.1372)) return field;
    }
    return std::make_shared<InflateField>();
}

InflateFieldCache::InflateFieldCache(size_t capacity) : capacity(capacity) {}

std::shared_ptr<const InflateField> InflateFieldCache::get(const ShapeContours& shape, double step) {
    const uint64_t key = shapeKey(shape, step);
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->key == key) {
                entries.splice(entries.begin(), entries, it);
                hitCount++;
                return entries.front().field;
            }
        }
        missCount++;
    }
    std::shared_ptr<const InflateField> field = buildInflateField(shape, step);
    std::lock_guard<std::mutex> guard(lock);
    entries.push_front({key, field});
    while (entries.size() > capacity) entries.pop_back();
    return field;
}

uint64_t InflateFieldCache::hits() const {
    std::lock_guard<std::mutex> guard(lock);
    return hitCount;
}

uint64_t InflateFieldCache::misses() const {
    std::lock_guard<std::mutex> guard(lock);
    return missCount;
}

void InflateFieldCache::clear() {
    std::lock_guard<std::mutex> guard(lock);
    entries.clear();
}

double jasperengine::titleart::three_d::inflateWallTop(double height, const TitleCurve& profile) {
    return height * (profile.evaluate(0) - 1);
}

void jasperengine::titleart::three_d::emitInflate(RawMesh& mesh, const InflateField& f, const InflateSpec& spec) {
    const size_t nv = f.dist.size();
    if (!nv) return;
    const std::vector<double>& xy = f.xy;
    const std::vector<uint32_t>& tris = f.tris;
    const double m = spec.maxDist > 0 ? spec.maxDist : 1;
    std::vector<double> z(nv);
    for (size_t v = 0; v < nv; v++) z[v] = spec.height * (spec.profile.evaluate(std::min(1.0, f.dist[v] / m)) - 1);
    const double wallTop = inflateWallTop(spec.height, spec.profile);

    std::vector<double> n(nv * 3, 0.0);
    if (spec.smoothness > 0) {
        // exact normals: the profile's slope along the distance gradient (z rises inward)
        for (size_t v = 0; v < nv; v++) {
            const double t = f.dist[v] / m;
            double slope = 0;
            if (t < 1) slope = spec.height * spec.profile.derivative(t) / m;
            const double gx = -slope * f.grad[2 * v], gy = -slope * f.grad[2 * v + 1];
            const double l = std::sqrt(gx * gx + gy * gy + 1);
            n[3 * v] = gx / l;
            n[3 * v + 1] = gy / l;
            n[3 * v + 2] = 1 / l;
        }
        // the gradient flips across the medial axis: average with the neighbours (outline fixed)
        std::vector<double> acc(nv * 3);
        const int passes = (int)std::ceil(spec.smoothness / 10);
        for (int pass = 0; pass < passes; pass++) {
            acc = n;
            for (size_t t = 0; t < tris.size(); t += 3) {
                for (int e = 0; e < 3; e++) {
                    const uint32_t a = tris[t + e], b = tris[t + (e + 1) % 3];
                    for (int c = 0; c < 3; c++) {
                        acc[3 * a + c] += n[3 * b + c];
                        acc[3 * b + c] += n[3 * a + c];
                    }
                }
            }
            for (size_t v = 0; v < nv; v++) {
                if (f.dist[v] <= 0) continue;
                double l = std::sqrt(acc[3 * v] * acc[3 * v] + acc[3 * v + 1] * acc[3 * v + 1] + acc[3 * v + 2] * acc[3 * v + 2]);
                if (l == 0) l = 1;
                for (int c = 0; c < 3; c++) n[3 * v + c] = acc[3 * v + c] / l;
            }
        }
    } else {
        // area-weighted vertex normals over the surface
        for (size_t t = 0; t < tris.size(); t += 3) {
            const uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
            const double ux = xy[2 * b] - xy[2 * a], uy = xy[2 * b + 1] - xy[2 * a + 1], uz = z[b] - z[a];
            const double vx = xy[2 * c] - xy[2 * a], vy = xy[2 * c + 1] - xy[2 * a + 1], vz = z[c] - z[a];
            const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            for (uint32_t k : {a, b, c}) {
                n[3 * k] += cx;
                n[3 * k + 1] += cy;
                n[3 * k + 2] += cz;
            }
        }
        for (size_t v = 0; v < nv; v++) {
            double l = std::sqrt(n[3 * v] * n[3 * v] + n[3 * v + 1] * n[3 * v + 1] + n[3 * v + 2] * n[3 * v + 2]);
            if (l == 0) l = 1;
            for (int c = 0; c < 3; c++) n[3 * v + c] /= l;
        }
    }
    for (uint32_t v : tris) mesh.push(spec.faceSlot, xy[2 * v], xy[2 * v + 1], z[v], n[3 * v], n[3 * v + 1], n[3 * v + 2]);

    // wall: through every outline vertex and crossing, smooth across shallow turns
    const double zb = spec.zBack;
    if (wallTop - zb > 1e-4) {
        for (const std::vector<uint32_t>& ring : f.rings) {
            std::vector<uint32_t> pts;
            for (size_t i = 0; i < ring.size(); i++) {
                const uint32_t v = ring[i], w = ring[(i + 1) % ring.size()];
                if (std::abs(xy[2 * v] - xy[2 * w]) > 1e-9 || std::abs(xy[2 * v + 1] - xy[2 * w + 1]) > 1e-9) pts.push_back(v);
            }
            const size_t k = pts.size();
            if (k < 3) continue;
            std::vector<std::array<double, 2>> en(k);
            std::vector<double> arc(k + 1, 0.0);
            for (size_t i = 0; i < k; i++) {
                const uint32_t a = pts[i], b = pts[(i + 1) % k];
                const double dx = xy[2 * b] - xy[2 * a], dy = xy[2 * b + 1] - xy[2 * a + 1];
                double l = std::hypot(dx, dy);
                arc[i + 1] = arc[i] + l;
                if (l == 0) l = 1;
                en[i] = {dy / l, -dx / l};
            }
            auto vn = [&](size_t i, size_t e) -> std::array<double, 2> {
                const auto& p = en[(i + k - 1) % k];
                const auto& q = en[i % k];
                if (p[0] * q[0] + p[1] * q[1] < CREASE_COS) return en[e];
                const double sx = p[0] + q[0], sy = p[1] + q[1];
                double l = std::hypot(sx, sy);
                if (l == 0) l = 1;
                return {sx / l, sy / l};
            };
            for (size_t i = 0; i < k; i++) {
                const size_t j = (i + 1) % k;
                const uint32_t a = pts[i], b = pts[j];
                const auto na = vn(i, i), nb = vn(j, i);
                const double ax = xy[2 * a], ay = xy[2 * a + 1], bx = xy[2 * b], by = xy[2 * b + 1];
                const double aa = arc[i], ab = arc[i + 1];
                mesh.push(spec.wallSlot, ax, ay, wallTop, na[0], na[1], 0, aa);
                mesh.push(spec.wallSlot, ax, ay, zb, na[0], na[1], 0, aa);
                mesh.push(spec.wallSlot, bx, by, zb, nb[0], nb[1], 0, ab);
                mesh.push(spec.wallSlot, ax, ay, wallTop, na[0], na[1], 0, aa);
                mesh.push(spec.wallSlot, bx, by, zb, nb[0], nb[1], 0, ab);
                mesh.push(spec.wallSlot, bx, by, wallTop, nb[0], nb[1], 0, ab);
            }
        }
    }

    // back: the same rings, facing -z
    if (f.rings.empty()) return;
    auto vec = [&](uint32_t v) { return TitleVec2{xy[2 * v], xy[2 * v + 1]}; };
    Contour outer;
    for (uint32_t v : f.rings[0]) outer.push_back(vec(v));
    std::vector<Contour> holes;
    std::vector<TitleVec2> all = outer;
    for (size_t r = 1; r < f.rings.size(); r++) {
        Contour c;
        for (uint32_t v : f.rings[r]) c.push_back(vec(v));
        all.insert(all.end(), c.begin(), c.end());
        holes.push_back(std::move(c));
    }
    const std::vector<uint32_t> faces = triangulatePolygon(outer, holes);
    for (size_t t = 0; t + 2 < faces.size(); t += 3) {
        const TitleVec2 &a = all[faces[t]], &b = all[faces[t + 1]], &c = all[faces[t + 2]];
        const bool ccw = ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) >= 0;
        const uint32_t order[3] = {faces[t], ccw ? faces[t + 2] : faces[t + 1], ccw ? faces[t + 1] : faces[t + 2]};
        for (uint32_t v : order) mesh.push(spec.backSlot, all[v].x, all[v].y, zb, 0, 0, -1);
    }
}
