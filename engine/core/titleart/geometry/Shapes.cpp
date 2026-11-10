// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/shapes.ts by SafEight, used with the author's permission.

#include "Shapes.h"

#include "PathUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180;
    // 4/3 (sqrt(2) - 1): the cubic quarter circle
    constexpr double KAPPA = 0.5522847498;

    using Pt = TitleVec2;

    // one segment of a closed contour; each starts where the previous one ends
    struct Seg {
        bool cubic = false;
        Pt p0, c1, c2, p1;
    };

    Seg lineSeg(const Pt& a, const Pt& b) { return {false, a, {}, {}, b}; }
    Seg cubicSeg(const Pt& a, const Pt& c1, const Pt& c2, const Pt& b) { return {true, a, c1, c2, b}; }

    void emit(const std::vector<Seg>& segs, Path& out) {
        if (segs.empty()) return;
        out.moveTo(segs[0].p0.x, segs[0].p0.y);
        for (const Seg& s : segs) {
            if (s.cubic) {
                out.cubicTo(s.c1.x, s.c1.y, s.c2.x, s.c2.y, s.p1.x, s.p1.y);
            } else {
                out.lineTo(s.p1.x, s.p1.y);
            }
        }
        out.close();
    }

    // closed polyline (vertices in travel order) -> straight segments
    std::vector<Seg> polySegs(const std::vector<Pt>& pts) {
        std::vector<Seg> out;
        for (size_t i = 0; i < pts.size(); i++) out.push_back(lineSeg(pts[i], pts[(i + 1) % pts.size()]));
        return out;
    }

    // a convex polygon (clockwise, y up) with corner radius[i] at vertex i, as lines and circular
    // arcs; a radius is clamped so its arc uses at most half of each adjacent edge
    std::vector<Seg> roundedPolySegs(const std::vector<Pt>& pts, const std::vector<double>& radius) {
        const size_t n = pts.size();
        struct Corner {
            Pt t1, t2, c1, c2;
            bool arc = false;
        };
        std::vector<Corner> corners(n);
        for (size_t i = 0; i < n; i++) {
            const Pt v = pts[i];
            const Pt prev = pts[(i + n - 1) % n];
            const Pt next = pts[(i + 1) % n];
            const double l1 = pointDistance(v, prev), l2 = pointDistance(v, next);
            const double r = radius[i];
            Corner& c = corners[i];
            c.t1 = c.t2 = c.c1 = c.c2 = v;
            if (!(r > 0) || l1 < 1e-9 || l2 < 1e-9) continue;
            const Pt u1{(prev.x - v.x) / l1, (prev.y - v.y) / l1};
            const Pt u2{(next.x - v.x) / l2, (next.y - v.y) / l2};
            const double interior = std::acos(std::clamp(u1.x * u2.x + u1.y * u2.y, -1.0, 1.0));
            if (interior > PI - 1e-6) continue;     // collinear
            const double tanHalf = std::tan(interior / 2);
            const double d = std::min({r / tanHalf, l1 / 2, l2 / 2});
            const double rEff = d * tanHalf;
            const double k = 4.0 / 3.0 * std::tan((PI - interior) / 4) * rEff;
            c.t1 = {v.x + u1.x * d, v.y + u1.y * d};
            c.t2 = {v.x + u2.x * d, v.y + u2.y * d};
            c.c1 = {c.t1.x - u1.x * k, c.t1.y - u1.y * k};
            c.c2 = {c.t2.x - u2.x * k, c.t2.y - u2.y * k};
            c.arc = true;
        }
        std::vector<Seg> segs;
        for (size_t i = 0; i < n; i++) {
            const Corner& c = corners[i];
            const Corner& next = corners[(i + 1) % n];
            if (c.arc) segs.push_back(cubicSeg(c.t1, c.c1, c.c2, c.t2));
            if (pointDistance(c.t2, next.t1) > 1e-9) segs.push_back(lineSeg(c.t2, next.t1));
        }
        return segs;
    }

    // a polyline scaled (and optionally centred) so its bounds are exactly w x h
    std::vector<Pt> fitBox(const std::vector<Pt>& pts, double w, double h, bool centre) {
        double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
        for (const Pt& p : pts) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        std::vector<Pt> out;
        out.reserve(pts.size());
        if (!centre) {
            // keep the origin: scale so the farthest point on each axis touches the box
            const double sx = w / 2 / std::max({std::abs(x0), std::abs(x1), 1e-9});
            const double sy = h / 2 / std::max({std::abs(y0), std::abs(y1), 1e-9});
            for (const Pt& p : pts) out.push_back({p.x * sx, p.y * sy});
            return out;
        }
        const double sx = w / std::max(x1 - x0, 1e-9), sy = h / std::max(y1 - y0, 1e-9);
        const double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        for (const Pt& p : pts) out.push_back({(p.x - mx) * sx, (p.y - my) * sy});
        return out;
    }

    // ---------- tails (speech / shout) ----------

    Pt segPoint(const Seg& s, double t) {
        if (!s.cubic) return lerpPoint(s.p0, s.p1, t);
        return cubicPoint(s.p0, s.c1, s.c2, s.p1, t);
    }

    // the part of a segment between parameters t0 < t1 (de Casteljau for cubics)
    Seg segPart(const Seg& s, double t0, double t1) {
        if (!s.cubic) return lineSeg(lerpPoint(s.p0, s.p1, t0), lerpPoint(s.p0, s.p1, t1));
        // split at t1, keep the head; then split the head at t0 / t1, keep the tail
        const Pt ab = lerpPoint(s.p0, s.c1, t1), bc = lerpPoint(s.c1, s.c2, t1), cd = lerpPoint(s.c2, s.p1, t1);
        const Pt abc = lerpPoint(ab, bc, t1), bcd = lerpPoint(bc, cd, t1);
        const Seg head = cubicSeg(s.p0, ab, abc, lerpPoint(abc, bcd, t1));
        const double r = t1 > 1e-12 ? t0 / t1 : 0;
        const Pt e = lerpPoint(head.p0, head.c1, r), f = lerpPoint(head.c1, head.c2, r), g = lerpPoint(head.c2, head.p1, r);
        const Pt ef = lerpPoint(e, f, r), fg = lerpPoint(f, g, r);
        return cubicSeg(lerpPoint(ef, fg, r), fg, g, head.p1);
    }

    struct Hit {
        size_t i = 0;
        double t = 0;
        Pt p;
    };

    // where the ray from the origin at angle `phi` leaves a contour that is star-shaped about the
    // origin and travels clockwise (polar angle decreasing along travel)
    std::optional<Hit> rayHit(const std::vector<Seg>& segs, double phi) {
        constexpr int N = 24;
        const auto off = [phi](const Pt& p) {
            const double a = std::atan2(p.y, p.x) - phi;
            return a - 2 * PI * std::round(a / (2 * PI));   // wrapped to [-pi, pi]
        };
        for (size_t i = 0; i < segs.size(); i++) {
            double ta = 0;
            double da = off(segPoint(segs[i], 0));
            for (int k = 1; k <= N; k++) {
                const double tb = (double)k / N;
                const double db = off(segPoint(segs[i], tb));
                if (da >= 0 && db < 0 && da - db < PI) {
                    double lo = ta, hi = tb;
                    for (int it = 0; it < 30; it++) {
                        const double mid = (lo + hi) / 2;
                        if (off(segPoint(segs[i], mid)) >= 0) {
                            lo = mid;
                        } else {
                            hi = mid;
                        }
                    }
                    const double t = (lo + hi) / 2;
                    return Hit{i, t, segPoint(segs[i], t)};
                }
                ta = tb;
                da = db;
            }
        }
        return std::nullopt;
    }

    // the union of a star-shaped body and a triangular tail as ONE contour (no overlapping rims):
    // the boundary between the two base points is replaced by base -> tip -> base; a tip inside
    // the body adds no tail
    std::vector<Seg> withTail(const std::vector<Seg>& segs, const Pt& tip, double baseWidth) {
        const double theta = std::atan2(tip.y, tip.x);
        const std::optional<Hit> hit = rayHit(segs, theta);
        if (!hit) return segs;
        const double rho = std::hypot(hit->p.x, hit->p.y);
        if (std::hypot(tip.x, tip.y) <= rho * 1.02) return segs;
        const double delta = std::clamp(std::atan2(baseWidth / 2, rho), 0.01, 0.9);
        const std::optional<Hit> a = rayHit(segs, theta + delta);   // reached first along clockwise travel
        const std::optional<Hit> b = rayHit(segs, theta - delta);
        if (!a || !b) return segs;
        std::vector<Seg> out;
        const size_t n = segs.size();
        if (b->t < 1 - 1e-9) out.push_back(segPart(segs[b->i], b->t, 1));
        for (size_t k = 1; k <= n; k++) {
            const size_t i = (b->i + k) % n;
            if (i == a->i) {
                if (a->t > 1e-9) out.push_back(segPart(segs[i], 0, a->t));
                break;
            }
            out.push_back(segs[i]);
        }
        out.push_back(lineSeg(a->p, tip));
        out.push_back(lineSeg(tip, b->p));
        return out;
    }

    // ---------- shapes ----------

    // rounded rectangle body, clockwise from the top-left corner
    std::vector<Seg> rectSegs(double w, double h, double r) {
        const std::vector<Pt> pts = {{-w / 2, h / 2}, {w / 2, h / 2}, {w / 2, -h / 2}, {-w / 2, -h / 2}};
        return roundedPolySegs(pts, {r, r, r, r});
    }

    std::vector<Seg> ellipseSegs(double rx, double ry) {
        const double k = KAPPA;
        const Pt top{0, ry}, right{rx, 0}, bottom{0, -ry}, left{-rx, 0};
        return {
            cubicSeg(top, {rx * k, ry}, {rx, ry * k}, right),
            cubicSeg(right, {rx, -ry * k}, {rx * k, -ry}, bottom),
            cubicSeg(bottom, {-rx * k, -ry}, {-rx, -ry * k}, left),
            cubicSeg(left, {-rx, ry * k}, {-rx * k, ry}, top),
        };
    }

    // a regular polygon with a flat bottom edge, fitted to w x h
    std::vector<Seg> polygonSegs(double w, double h, int sides, double r) {
        std::vector<Pt> pts;
        // the bottom edge spans the angles -90 deg +- 180/n deg; vertices run clockwise
        for (int i = 0; i < sides; i++) {
            const double a = -PI / 2 - PI / sides - 2 * PI * i / sides;
            pts.push_back({std::cos(a), std::sin(a)});
        }
        const std::vector<Pt> fitted = fitBox(pts, w, h, true);
        return roundedPolySegs(fitted, std::vector<double>(fitted.size(), r));
    }

    // a regular star, first point straight up, fitted to w x h
    std::vector<Seg> starSegs(double w, double h, int points, double inner) {
        std::vector<Pt> pts;
        for (int i = 0; i < points * 2; i++) {
            const double a = PI / 2 - PI * i / points;
            const double rad = i % 2 == 0 ? 1 : inner;
            pts.push_back({std::cos(a) * rad, std::sin(a) * rad});
        }
        return polySegs(fitBox(pts, w, h, true));
    }

    // a jagged starburst around an ellipse: spike tips on the ellipse (radius 1), valleys at
    // `inner`; jitter shortens spikes and shifts them sideways (seeded) within their own sector,
    // so the outline stays star-shaped about the origin (the shout tail needs that)
    std::vector<Pt> burstPoints(int points, double inner, double jitter, int64_t seed) {
        Mulberry32 rand(seed);
        const double step = 2 * PI / points;
        struct Tip {
            double a, r;
        };
        std::vector<Tip> tips;
        for (int i = 0; i < points; i++) {
            const double a = PI / 2 - i * step + (rand.next() - 0.5) * jitter * step * 0.7;
            const double r = inner + (1 - inner) * (1 - 0.8 * jitter * rand.next());
            tips.push_back({a, r});
        }
        std::vector<Pt> pts;
        for (int i = 0; i < points; i++) {
            const Tip t = tips[i];
            const Tip next = tips[(i + 1) % points];
            const double nextA = i + 1 < points ? next.a : next.a - 2 * PI;
            const double va = (t.a + nextA) / 2;
            const double vr = std::min({inner * (1 + (rand.next() - 0.5) * jitter * 0.2), t.r * 0.97, next.r * 0.97});
            pts.push_back({std::cos(t.a) * t.r, std::sin(t.a) * t.r});
            pts.push_back({std::cos(va) * vr, std::sin(va) * vr});
        }
        return pts;
    }

    // an emblem (heater) shield: flat top with rounded corners, straight sides, curved to a point
    std::vector<Seg> shieldSegs(double w, double h, double corner) {
        const double top = h / 2;
        const double shoulder = top - h * 0.42;     // where the sides start curving in
        const double r = std::clamp(corner, 0.0, std::min(w / 2, h * 0.42));
        const double k = KAPPA * r;
        const Pt tip{0, -h / 2};
        const Pt tl{-w / 2 + r, top}, tr{w / 2 - r, top};
        const Pt rt{w / 2, top - r}, lt{-w / 2, top - r};
        const Pt rs{w / 2, shoulder}, ls{-w / 2, shoulder};
        std::vector<Seg> segs;
        if (pointDistance(tl, tr) > 1e-9) segs.push_back(lineSeg(tl, tr));
        if (r > 0) segs.push_back(cubicSeg(tr, {tr.x + k, top}, {w / 2, rt.y + k}, rt));
        if (pointDistance(rt, rs) > 1e-9) segs.push_back(lineSeg(rt, rs));
        segs.push_back(cubicSeg(rs, {w / 2, shoulder - h * 0.3}, {w * 0.2, tip.y + h * 0.13}, tip));
        segs.push_back(cubicSeg(tip, {-w * 0.2, tip.y + h * 0.13}, {-w / 2, shoulder - h * 0.3}, ls));
        if (pointDistance(ls, lt) > 1e-9) segs.push_back(lineSeg(ls, lt));
        if (r > 0) segs.push_back(cubicSeg(lt, {-w / 2, lt.y + k}, {tl.x - k, top}, tl));
        return segs;
    }

    // a banner: the front band on top, and at each end a tail one band-height lower that tucks
    // behind the band and ends in a V notch; separate overlapping contours of the same winding,
    // whose stepped union reads as the fold
    Path ribbonPath(double w, double h, double notch, double foldLen, double slant) {
        const double tail = std::clamp(foldLen, 0.0, w * 0.45);   // visible tail beyond the band end
        const double band = tail > 1e-6 ? h * 0.74 : h;           // height of the strip
        const double xb = w / 2 - tail;                           // band half-width
        const double tuck = std::min(tail * 0.45, xb);            // how far each tail runs under the band
        const double bandTop = h / 2, bandBottom = h / 2 - band;
        const double tailTop = -h / 2 + band, tailBottom = -h / 2;
        const double v = std::min(std::clamp(notch, 0.0, 1.0) * band, tail * 0.95);
        const double k = std::tan(std::clamp(slant, -60.0, 60.0) * DEG);
        const auto shear = [k](std::vector<Pt> pts) {
            for (Pt& p : pts) p.x += k * p.y;
            return pts;
        };
        Path out;
        if (tail > 1e-6) {
            const double midY = (tailTop + tailBottom) / 2;
            std::vector<Pt> left = {{-w / 2, tailTop}, {-xb + tuck, tailTop}, {-xb + tuck, tailBottom}, {-w / 2, tailBottom}};
            if (v > 1e-6) left.push_back({-w / 2 + v, midY});
            std::vector<Pt> right = {{xb - tuck, tailTop}, {w / 2, tailTop}};
            if (v > 1e-6) right.push_back({w / 2 - v, midY});
            right.push_back({w / 2, tailBottom});
            right.push_back({xb - tuck, tailBottom});
            emit(polySegs(shear(left)), out);
            emit(polySegs(shear(right)), out);
        }
        emit(polySegs(shear({{-xb, bandTop}, {xb, bandTop}, {xb, bandBottom}, {-xb, bandBottom}})), out);
        return out;
    }

}

Mulberry32::Mulberry32(int64_t seed) : state((uint32_t)(int32_t)std::llround((double)seed) ^ 0x9e3779b9u) {}

double Mulberry32::next() {
    state += 0x6d2b79f5u;
    uint32_t t = state;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

ShapeParams jasperengine::titleart::geometry::decodeShape(const Json& o) {
    ShapeParams s;
    if (!o.is_object()) return s;
    s.shape = o.value("shape", std::string("rect"));
    s.size = o.value("size", 200.0);
    s.aspect = o.value("aspect", 1.0);
    s.corner = o.value("corner", 0.0);
    s.slant = o.value("slant", 0.0);
    s.points = o.value("points", 5);
    s.inner = o.value("inner", 0.5);
    s.jitter = o.value("jitter", 0.0);
    s.seed = o.value("seed", (int64_t)1);
    if (o.contains("tail") && o["tail"].is_object()) {
        const Json& tail = o["tail"];
        s.tail = {tail.value("x", 0.0), tail.value("y", -60.0)};
        s.tailWidth = tail.value("width", 20.0);
    }
    return s;
}

Path jasperengine::titleart::geometry::shapePath(const ShapeParams& src) {
    Path out;
    const double h = src.size;
    if (!(h > 0)) return out;
    const double w = h * std::max(src.aspect, 0.01);
    const double corner = std::max(src.corner, 0.0) / 100 * h;
    const double inner = std::clamp(src.inner, 0.05, 1.0);
    const double jitter = std::clamp(src.jitter, 0.0, 1.0);
    const Pt tip{src.tail.x / 100 * h, src.tail.y / 100 * h};
    const double tailWidth = std::max(src.tailWidth, 1.0) / 100 * h;
    const std::string& shape = src.shape;
    if (shape == "parallelogram") {
        // the total width stays w: the lean shifts the top edge right and the bottom edge left
        const double off = std::clamp(h * std::tan(std::clamp(src.slant, -80.0, 80.0) * DEG), -w * 0.95, w * 0.95);
        const double a = std::abs(off);
        const double s = off > 0 ? 1 : off < 0 ? -1 : 0;
        const std::vector<Pt> pts = {
            {-w / 2 + (s > 0 ? a : 0), h / 2},
            {w / 2 - (s < 0 ? a : 0), h / 2},
            {w / 2 - (s > 0 ? a : 0), -h / 2},
            {-w / 2 + (s < 0 ? a : 0), -h / 2},
        };
        emit(roundedPolySegs(pts, {corner, corner, corner, corner}), out);
    } else if (shape == "ellipse" || shape == "orb") {
        // orb: the 2D silhouette of the 3D sphere / ellipsoid
        emit(ellipseSegs(w / 2, h / 2), out);
    } else if (shape == "polygon") {
        emit(polygonSegs(w, h, std::clamp(src.points, 3, 64), corner), out);
    } else if (shape == "star") {
        emit(starSegs(w, h, std::clamp(src.points, 3, 100), inner), out);
    } else if (shape == "burst") {
        emit(polySegs(fitBox(burstPoints(std::clamp(src.points, 3, 200), inner, jitter, src.seed), w, h, true)), out);
    } else if (shape == "shield") {
        emit(shieldSegs(w, h, corner), out);
    } else if (shape == "speech") {
        emit(withTail(rectSegs(w, h, corner), tip, tailWidth), out);
    } else if (shape == "shout") {
        // the burst stays centred on the origin (not on its bounds), so the tail tip is relative
        // to the bubble's centre and the splice sees a star-shaped body
        const std::vector<Pt> pts = burstPoints(std::clamp(src.points, 3, 200), inner, jitter, src.seed);
        emit(withTail(polySegs(fitBox(pts, w, h, false)), tip, tailWidth), out);
    } else if (shape == "ribbon") {
        return ribbonPath(w, h, src.inner, std::max(src.tail.x, 0.0) / 100 * h, src.slant);
    } else {
        emit(rectSegs(w, h, corner), out);
    }
    return out;
}
