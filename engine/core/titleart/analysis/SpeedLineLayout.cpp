// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/lines/speedLines.ts (the focus exits, the boundary tables,
// layout) and src/generators/lines/shared.ts (insideConvex, rayExit, gradientLinear) by SafEight,
// used with the author's permission.

#include "SpeedLineLayout.h"

#include "titleart/effects/TitleFx.h"
#include "titleart/generators/TitleGen.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;
    constexpr double INF = std::numeric_limits<double>::infinity();
    constexpr int TAB = SPEED_LINE_TABLE;

    using Poly = std::vector<TitleVec2>;
    // the distance along the unit ray (ox, oy) + t (dx, dy) at which it last leaves the focus
    // (0 = never inside), marching at most maxT
    using Exit = std::function<double(double ox, double oy, double dx, double dy, double maxT)>;

    // shared.ts clamp
    double clampd(double x, double lo, double hi) {
        return x < lo ? lo : x > hi ? hi : x;
    }

    // JavaScript's Math.round
    double jsRound(double x) {
        return std::floor(x + 0.5);
    }

    // true when (x, y) lies inside the convex polygon (either winding)
    bool insideConvex(const Poly& poly, double x, double y) {
        int sign = 0;
        for (size_t i = 0; i < poly.size(); i++) {
            const TitleVec2& a = poly[i];
            const TitleVec2& b = poly[(i + 1) % poly.size()];
            const double c = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
            if (c == 0.0) continue;
            const int s = c > 0.0 ? 1 : -1;
            if (sign == 0) sign = s;
            else if (s != sign) return false;
        }
        return true;
    }

    // the distance along the unit ray at which it leaves the convex polygon, -1 when it never
    // reaches the inside (t >= 0)
    double rayExit(const Poly& poly, double ox, double oy, double dx, double dy) {
        double cx = 0.0, cy = 0.0;
        for (const TitleVec2& v : poly) {
            cx += v.x / (double)poly.size();
            cy += v.y / (double)poly.size();
        }
        double t0 = -INF, t1 = INF;
        for (size_t i = 0; i < poly.size(); i++) {
            const TitleVec2& a = poly[i];
            const TitleVec2& b = poly[(i + 1) % poly.size()];
            // the edge normal pointing inside (towards the centroid), whatever the winding
            double nx = a.y - b.y, ny = b.x - a.x;
            if (nx * (cx - a.x) + ny * (cy - a.y) < 0.0) {
                nx = -nx;
                ny = -ny;
            }
            const double dist = nx * (ox - a.x) + ny * (oy - a.y);   // > 0 inside this edge
            const double den = nx * dx + ny * dy;
            if (std::abs(den) < 1e-12) {
                if (dist < 0.0) return -1.0;
                continue;
            }
            const double t = -dist / den;
            if (den > 0.0) t0 = std::max(t0, t);
            else t1 = std::min(t1, t);
        }
        return t1 >= std::max(t0, 0.0) ? t1 : -1.0;
    }

    Exit ellipseExit(double cx, double cy, double rx, double ry) {
        return [=](double ox, double oy, double dx, double dy, double) {
            if (rx <= 0.0 || ry <= 0.0) return 0.0;
            const double px = ox - cx, py = oy - cy;
            const double ix = 1.0 / (rx * rx), iy = 1.0 / (ry * ry);
            const double a = dx * dx * ix + dy * dy * iy;
            const double b = 2.0 * (px * dx * ix + py * dy * iy);
            const double c = px * px * ix + py * py * iy - 1.0;
            const double disc = b * b - 4.0 * a * c;
            return disc < 0.0 ? 0.0 : std::max(0.0, (-b + std::sqrt(disc)) / (2.0 * a));
        };
    }

    Exit rectExit(double cx, double cy, double rx, double ry) {
        return [=](double ox, double oy, double dx, double dy, double) {
            double t0 = -INF, t1 = INF;
            const double axes[2][3] = {{ox - cx, dx, rx}, {oy - cy, dy, ry}};
            for (const auto& axis : axes) {
                const double p = axis[0], d = axis[1], r = axis[2];
                if (std::abs(d) < 1e-12) {
                    if (std::abs(p) > r) return 0.0;
                    continue;
                }
                const double a = (-r - p) / d, b = (r - p) / d;
                t0 = std::max(t0, std::min(a, b));
                t1 = std::min(t1, std::max(a, b));
            }
            return t1 >= std::max(t0, 0.0) ? t1 : 0.0;
        };
    }

    // The farthest cell of the followed layer's alpha along the ray (its outer silhouette, the
    // gaps between glyphs ignored) plus `pad`. Pachislop marched render px; the grid here is in
    // canvas px, so `m` takes the layout space there, and the step (half a cell, in layout px)
    // is the same at every px_scale
    Exit gridExit(const Grid& grid, const TitleAffine& m, double k, double pad) {
        const double step = std::max(0.5, (grid.cell * 0.5) / k);
        const Grid* g = &grid;
        return [g, m, step, pad](double ox, double oy, double dx, double dy, double maxT) {
            if (!std::isfinite(maxT)) return 0.0;
            double x = m.a * ox + m.b * oy + m.tx;
            double y = m.c * ox + m.d * oy + m.ty;
            const double sx = (m.a * dx + m.b * dy) * step;
            const double sy = (m.c * dx + m.d * dy) * step;
            double last = -1.0;
            for (double t = 0.0; t <= maxT; t += step, x += sx, y += sy) {
                const double gx = std::floor((x - g->x0) / g->cellComp);
                const double gy = std::floor((y - g->y0) / g->cellComp);
                if (gx >= 0.0 && gy >= 0.0 && gx < g->w && gy < g->h && g->alpha[(size_t)gy * (size_t)g->w + (size_t)gx] >= 0.3f) last = t;
            }
            return last < 0.0 ? 0.0 : last + step * 0.5 + pad;
        };
    }

    // a running max over +-2 samples: closes the notches a grid silhouette leaves between glyphs
    void dilate(std::vector<float>& tab, bool wrap) {
        const std::vector<float> src = tab;
        const int n = (int)src.size();
        for (int i = 0; i < n; i++) {
            float v = src[i];
            for (int j = -2; j <= 2; j++) {
                const int q = wrap ? (i + j + n) % n : std::clamp(i + j, 0, n - 1);
                v = std::max(v, src[q]);
            }
            tab[i] = v;
        }
    }

    // the table at u 0..1, linear between samples (wrapping around for radial tables)
    double lookup(const std::vector<float>& tab, double u, bool wrap) {
        const int n = (int)tab.size();
        if (wrap) {
            const double x = std::fmod(std::fmod(u, 1.0) + 1.0, 1.0) * n;
            const int i = (int)std::floor(x) % n;
            return tab[i] + ((double)tab[(i + 1) % n] - tab[i]) * (x - std::floor(x));
        }
        const double x = clampd(u * n - 0.5, 0.0, n - 1.0);
        const int i = std::min(n - 2, (int)std::floor(x));
        return tab[i] + ((double)tab[i + 1] - tab[i]) * (x - i);
    }

    // jagged teeth (ギザギザ): 0 at the notches, `height` at the tips, `count` teeth over u 0..1
    double jag(double u, int count, double height) {
        if (count <= 0) return 0.0;
        const double f = std::fmod(std::fmod(u * count, 1.0) + 1.0, 1.0);
        return height * (1.0 - std::abs(2.0 * f - 1.0));
    }

    // the point at arc length s along a circular arc from (x, y), heading h, curvature kappa
    TitleVec2 arcPoint(double x, double y, double h, double kappa, double s) {
        if (std::abs(kappa * s) < 1e-6) return {x + std::cos(h) * s, y + std::sin(h) * s};
        return {x + (std::sin(h + kappa * s) - std::sin(h)) / kappa, y + (std::cos(h) - std::cos(h + kappa * s)) / kappa};
    }

    // the arc length at which the arc last leaves the convex frame within maxS, -1 if it is never inside
    double arcExit(const Poly& frame, double x, double y, double h, double kappa, double maxS) {
        if (std::abs(kappa) < 1e-9) return rayExit(frame, x, y, std::cos(h), std::sin(h));
        const int steps = 256;
        const double ds = maxS / steps;
        double last = -1.0;
        bool prev = insideConvex(frame, x, y);
        for (int i = 1; i <= steps; i++) {
            const TitleVec2 p = arcPoint(x, y, h, kappa, i * ds);
            const bool inside = insideConvex(frame, p.x, p.y);
            if (prev && !inside) {
                // the crossing between the two samples, refined
                double a = (i - 1) * ds, b = i * ds;
                for (int j = 0; j < 12; j++) {
                    const double m = 0.5 * (a + b);
                    const TitleVec2 q = arcPoint(x, y, h, kappa, m);
                    if (insideConvex(frame, q.x, q.y)) a = m;
                    else b = m;
                }
                last = b;
            }
            prev = inside;
        }
        return last;
    }

    // the gradient at pos as straight-alpha linear rgba (the lookup textures' encoding)
    void gradientLinear(const TitleGradient& g, double pos, float out[4]) {
        fx::sampleGradient(g, pos, out);
        for (int c = 0; c < 3; c++) out[c] = srgbToLinear(out[c]);
    }

}

TitleVec2 analysis::speedLinePoint(const SpeedLine& line, double curvature, double s) {
    return arcPoint(line.x, line.y, line.heading, curvature, s);
}

SpeedLineLayout analysis::layoutSpeedLines(const SpeedLineInput& in) {
    SpeedLineLayout out;
    const double k = in.k > 0.0 ? in.k : 1.0;
    const Poly& frame = in.frame;
    if (frame.size() < 3) return out;
    const bool radial = in.type != SpeedLineType::Flow;
    const double cx = in.center.x, cy = in.center.y;
    out.center = in.center;

    // the focus shape around the centre; a Layer focus without a silhouette is the ellipse
    const double rx = std::max(0.0, in.radius.x), ry = std::max(0.0, in.radius.y);
    const bool coarse = in.shape == SpeedLineShape::Layer && in.silhouette;
    const Exit focusExit = coarse ? gridExit(*in.silhouette, in.spaceToCanvas, k, std::max(0.0, in.padding))
                      : in.shape == SpeedLineShape::Rect ? rectExit(cx, cy, rx, ry)
                                                         : ellipseExit(cx, cy, rx, ry);
    const int jagN = (int)jsRound(clampd(in.jagCount, 0.0, 64.0));
    const double jagH = std::max(0.0, in.jagHeight);

    // focus boundary tables: radial around the centre, or across the frame for flow (per side)
    std::vector<std::vector<float>> tabs;
    const double fa = in.angle;
    const double dir[2] = {std::cos(fa), std::sin(fa)};
    const double perp[2] = {-dir[1], dir[0]};
    double vMin = 0.0, vMax = 0.0;
    if (radial) {
        std::vector<float> rf(TAB), fr(TAB);
        for (int i = 0; i < TAB; i++) {
            const double th = (2.0 * PI * i) / TAB;
            const double dx = std::cos(th), dy = std::sin(th);
            fr[i] = (float)std::max(0.0, rayExit(frame, cx, cy, dx, dy));
            rf[i] = (float)focusExit(cx, cy, dx, dy, fr[i]);
        }
        if (coarse) dilate(rf, true);
        out.table.assign((size_t)TAB * 4, 0.0f);
        for (int i = 0; i < TAB; i++) {
            out.table[(size_t)i * 4] = (float)(rf[i] + jag((double)i / TAB, jagN, jagH));
            out.table[(size_t)i * 4 + 1] = fr[i];
        }
        tabs.push_back(std::move(rf));
    } else {
        vMin = INF;
        vMax = -INF;
        for (const TitleVec2& c : frame) {
            const double v = (c.x - cx) * perp[0] + (c.y - cy) * perp[1];
            vMin = std::min(vMin, v);
            vMax = std::max(vMax, v);
        }
        for (const double s : {1.0, -1.0}) {
            std::vector<float> rf(TAB);
            for (int i = 0; i < TAB; i++) {
                const double v = vMin + ((i + 0.5) / TAB) * (vMax - vMin);
                const double ox = cx + perp[0] * v, oy = cy + perp[1] * v;
                rf[i] = (float)focusExit(ox, oy, s * dir[0], s * dir[1], std::max(0.0, rayExit(frame, ox, oy, s * dir[0], s * dir[1])));
            }
            if (coarse) dilate(rf, false);
            tabs.push_back(std::move(rf));
        }
    }

    const int n = (int)jsRound(clampd(in.count, 1.0, (double)SPEED_LINE_MAX));
    const int kc = (int)jsRound(clampd(in.clump, 1.0, 20.0));
    const double gap = clampd(in.gap, 0.0, 10.0);
    const int bundles = (n + kc - 1) / kc;
    const double units = n + bundles * gap;
    const double width = std::max(0.0, in.width);
    const double wr = clampd(in.widthRandom / 100.0, 0.0, 1.0);
    const double len = std::max(0.0, in.length / 100.0);
    const double lr = clampd(in.lengthRandom / 100.0, 0.0, 1.0);
    const double jitter = std::max(0.0, in.jitter);
    // curvature (per layout px): +-100 % turns a line by 60 deg over half the frame diagonal, the
    // same for every line so focus lines swirl as one
    double halfDiag = 0.0;
    for (const TitleVec2& a : frame)
        for (const TitleVec2& b : frame) halfDiag = std::max(halfDiag, 0.5 * std::hypot(a.x - b.x, a.y - b.y));
    const double kappa = ((clampd(in.curve, -100.0, 100.0) / 100.0) * (PI / 3.0)) / std::max(halfDiag, 1.0);
    out.curvature = kappa;
    const double margin = 1.5 / k;       // anti-aliasing room around each quad, layout px
    const double over = width + 2.0 / k; // how far frame-anchored lines run past the frame edge
    const double opacity = clampd(in.opacity / 100.0, 0.0, 1.0);

    auto push = [&](int slot, int side, double x, double y, double h, double L, double hw, double cu) {
        if (L * k < 0.5) return;
        SpeedLine line;
        line.slot = slot;
        line.side = side;
        line.x = x;
        line.y = y;
        line.heading = h;
        line.length = L;
        line.halfWidth = hw;
        if (in.fill == SpeedLineFill::Around) {
            float g[4];
            gradientLinear(in.gradient, cu, g);
            const double a = g[3] * opacity;
            line.color[0] = (float)(g[0] * a);
            line.color[1] = (float)(g[1] * a);
            line.color[2] = (float)(g[2] * a);
            line.color[3] = (float)a;
        } else {
            line.color[0] = (float)(in.color.r * opacity);
            line.color[1] = (float)(in.color.g * opacity);
            line.color[2] = (float)(in.color.b * opacity);
            line.color[3] = (float)opacity;
        }
        out.lines.push_back(line);
    };
    // A line anchored at the frame edge: from (ax, ay) (its focus end at full length) along the
    // arc to past the frame edge; shorter lengths keep the outer end and give up the focus end,
    // so the frame is always swept to its edges and only the clear centre varies
    auto toFrame = [&](int slot, int side, double ax, double ay, double h, double lenK, double hw, double cu) {
        const double s = arcExit(frame, ax, ay, h, kappa, 4.0 * halfDiag);
        if (s < 0.0) return;
        const double total = s + over;
        const double L = total * len * lenK;
        const TitleVec2 p = arcPoint(ax, ay, h, kappa, total - L);
        push(slot, side, p.x, p.y, h + kappa * (total - L), L, hw, cu);
    };

    gen::Rng r(in.seed);
    const double spin = r();
    std::vector<double> bundleLen((size_t)bundles);
    for (double& b : bundleLen) b = r();
    const SpeedLineSide side = in.side;
    const SpeedLineType type = in.type;
    for (int i = 0; i < n; i++) {
        const int b = i / kc;
        // its place in its slot (CLIP STUDIO 間隔の乱れ), clumps separated by `gap` empty slots
        const double u = (i + b * gap + 0.5 * gap + 0.15 + 0.7 * r()) / units;
        // six draws per line whatever its side or type, so toggling them keeps the other lines in place
        double draws[6];
        for (double& d : draws) d = r();
        auto lenK = [&](double h) { return 1.0 - lr * (kc > 1 ? 0.6 * bundleLen[(size_t)b] + 0.4 * h : h); };
        const double hj = draws[0], hw = draws[1], hl = draws[2];
        const double halfW = 0.5 * width * (1.0 - wr * hw);
        if (radial) {
            const double au = std::fmod(u + spin, 1.0);
            const double th = 2.0 * PI * au;
            const double rf = lookup(tabs[0], au, true);
            const double rb = rf + jag(au, jagN, jagH) + jitter * hj;
            const double ax = cx + std::cos(th) * rb, ay = cy + std::sin(th) * rb;
            // focus lines run to the frame edge; flash spikes a share of the focus radius
            if (type == SpeedLineType::Focus) toFrame(i, 0, ax, ay, th, lenK(hl), halfW, au);
            else push(i, 0, ax, ay, th, std::max(rf, 20.0) * len * lenK(hl), halfW, au);
            continue;
        }
        const double v = vMin + u * (vMax - vMin);
        const double ox = cx + perp[0] * v, oy = cy + perp[1] * v;
        const double eF = lookup(tabs[0], u, false);
        const double eB = lookup(tabs[1], u, false);
        if (side == SpeedLineSide::Both && eF <= 0.0 && eB <= 0.0) {
            // past the focus a flow line sweeps the whole frame (it has no focus end for the
            // length random to shorten), placed at random along it under 100 % length
            const double h = std::atan2(dir[1], dir[0]);
            const double fb = rayExit(frame, ox, oy, -dir[0], -dir[1]);
            if (fb < 0.0) continue;
            const double sx = ox - dir[0] * (fb + over), sy = oy - dir[1] * (fb + over);
            const double s = arcExit(frame, sx, sy, h, kappa, 4.0 * halfDiag);
            if (s < 0.0) continue;
            const double total = s + over;
            const double L = total * std::min(1.0, len);
            const double off = (total - L) * draws[4];
            const TitleVec2 p = arcPoint(sx, sy, h, kappa, off);
            push(i, 0, p.x, p.y, h + kappa * off, L, -halfW, u);
            continue;
        }
        for (const int sd : {1, -1}) {
            if ((side == SpeedLineSide::Front && sd < 0) || (side == SpeedLineSide::Back && sd > 0)) continue;
            const double* d3 = sd > 0 ? draws : draws + 3;
            const double rb = (sd > 0 ? eF : eB) + jag(u, jagN, jagH) + jitter * d3[0];
            const double dx = sd * dir[0], dy = sd * dir[1];
            toFrame(i, sd, ox + dx * rb, oy + dy * rb, std::atan2(dy, dx), lenK(d3[2]), 0.5 * width * (1.0 - wr * d3[1]), u);
        }
    }

    // segments: one per 2 deg of turn (a straight line is one quad)
    auto segsOf = [&](double L) { return (int)clampd(std::ceil(std::abs(kappa * L) / (2.0 * DEG)), 1.0, 32.0); };
    for (const SpeedLine& line : out.lines) {
        const double L = line.length;
        const int segs = segsOf(L);
        const double halfExt = (std::max(std::abs(line.halfWidth) * k, 0.5) + 1.0) / k;
        for (int j = 0; j < segs; j++) {
            const double s0 = (double)j / segs, s1 = (double)(j + 1) / segs;
            TitleVec2 a = arcPoint(line.x, line.y, line.heading, kappa, L * s0);
            TitleVec2 e = arcPoint(line.x, line.y, line.heading, kappa, L * s1);
            double dl = std::hypot(e.x - a.x, e.y - a.y);
            if (dl == 0.0) dl = 1e-6;
            const double ux = (e.x - a.x) / dl, uy = (e.y - a.y) / dl;
            // only the line's two ends grow for anti-aliasing: inner joints must not overlap (double alpha)
            const double m0 = j == 0 ? margin : 0.0;
            const double m1 = j == segs - 1 ? margin : 0.0;
            a.x -= ux * m0;
            a.y -= uy * m0;
            e.x += ux * m1;
            e.y += uy * m1;
            gen::QuadInstance q;
            q.cx = (float)(0.5 * (a.x + e.x));
            q.cy = (float)(0.5 * (a.y + e.y));
            q.w = (float)(dl + m0 + m1);
            q.h = (float)(2.0 * halfExt);
            q.rotation = (float)std::atan2(uy, ux);
            for (int c = 0; c < 4; c++) q.color[c] = line.color[c];
            q.extra[0] = (float)(s0 - m0 / L);
            q.extra[1] = (float)(s1 + m1 / L);
            q.extra[2] = (float)(line.halfWidth * k);
            q.extra[3] = (float)(L * k);
            out.quads.push_back(q);
        }
    }
    return out;
}

bool analysis::gridInkBox(const Grid& grid, double& x0, double& y0, double& x1, double& y1) {
    if (grid.empty() || grid.alpha.size() < (size_t)grid.w * (size_t)grid.h) return false;
    int gx0 = grid.w, gy0 = grid.h, gx1 = -1, gy1 = -1;
    for (int j = 0; j < grid.h; j++) {
        for (int i = 0; i < grid.w; i++) {
            if (!(grid.alpha[(size_t)j * grid.w + i] > 0.0f)) continue;
            gx0 = std::min(gx0, i);
            gx1 = std::max(gx1, i);
            gy0 = std::min(gy0, j);
            gy1 = std::max(gy1, j);
        }
    }
    if (gx1 < 0) return false;
    // the coverage the outermost column or row reaches (rows run bottom first)
    auto column = [&](int i) {
        float a = 0.0f;
        for (int j = gy0; j <= gy1; j++) a = std::max(a, grid.alpha[(size_t)j * grid.w + i]);
        return std::min(1.0, (double)a);
    };
    auto row = [&](int j) {
        float a = 0.0f;
        for (int i = gx0; i <= gx1; i++) a = std::max(a, grid.alpha[(size_t)j * grid.w + i]);
        return std::min(1.0, (double)a);
    };
    const double c = grid.cellComp;
    x0 = grid.x0 + (gx0 + 1.0 - column(gx0)) * c;
    x1 = grid.x0 + (gx1 + column(gx1)) * c;
    y0 = grid.y0 + (gy0 + 1.0 - row(gy0)) * c;
    y1 = grid.y0 + (gy1 + row(gy1)) * c;
    // a single partly covered column or row: its middle
    if (x1 < x0) x0 = x1 = 0.5 * (x0 + x1);
    if (y1 < y0) y0 = y1 = 0.5 * (y0 + y1);
    return true;
}
