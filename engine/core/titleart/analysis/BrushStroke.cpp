// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/rings/brushSwirl.ts (buildStrokes, ribbonGeometry,
// drawSplatter, reach) by SafEight, used with the author's permission.

#include "BrushStroke.h"

#include "titleart/generators/TitleGen.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;

    // smooth 1D value noise in [0, 1) over b/common.ts hash
    double noise1(double x, double seed) {
        const double i = std::floor(x);
        const double f = x - i;
        const double s = f * f * (3.0 - 2.0 * f);
        return gen::hashN({seed, i}) * (1.0 - s) + gen::hashN({seed, i + 1.0}) * s;
    }

    // the centreline point at path parameter t in [0, 1], `off` px to the side (outward for the
    // round paths, to the left of travel for the slash), relative to the centre
    void pathPoint(BrushPath kind, double t, double R, double a0, double sweep, double off, double& px, double& py) {
        if (kind == BrushPath::Slash) {
            // a long stroke through the centre along a0, bowed slightly to one side
            const double ux = std::cos(a0), uy = std::sin(a0);
            const double s = R * (2.0 * t - 1.0);
            const double n = off + 0.06 * R * std::sin(PI * t);
            px = ux * s - uy * n;
            py = uy * s + ux * n;
            return;
        }
        // clockwise from a0; the spiral winds inward to 30 % of the radius, the ring (円相) goes
        // once round and overshoots its start, drifting outward so the ends overlap side by side
        const double th = a0 - (kind == BrushPath::Ring ? 2.0 * PI + 0.4 : sweep) * t;
        const double r = (R + off) * (kind == BrushPath::Spiral ? 1.0 - 0.7 * t : kind == BrushPath::Ring ? 1.0 + 0.05 * t : 1.0);
        px = r * std::cos(th);
        py = r * std::sin(th);
    }

    int strokeCount(const BrushStrokeParams& p) {
        return (int)std::lround(std::min(12.0, std::max(1.0, (double)p.strokes)));
    }

}

std::vector<BrushStroke> analysis::buildBrushStrokes(const BrushStrokeParams& p) {
    const BrushPath kind = p.path;
    const double R = std::max(1.0, p.radius);
    const double a0 = p.angle * DEG;
    const double sweep = std::max(1.0, p.sweep) * DEG;
    const int n = strokeCount(p);
    const double spacing = p.spacing;
    const double W = std::max(1.0, p.width);
    const double tin = std::clamp(p.taperIn / 100.0, 0.0, 1.0);
    const double tout = std::clamp(p.taperOut / 100.0, 0.0, 1.0);
    // taper: how thin the end gets (a blunt press at 0 %, a hair-fine point at 100 %) and over
    // how much of the stroke it narrows
    const double inStart = 1.0 - 0.9 * tin;
    const double inLen = 0.03 + 0.3 * tin;
    const double outEnd = 1.0 - 0.95 * tout;
    const double outLen = 0.03 + 0.5 * tout;
    const double seed = p.seed;
    gen::Rng rand(seed * 101.0 + 7.0);
    const int mainStroke = (int)std::floor((n - 1) / 2.0);
    const double pathLen = brushPathLength(p);
    std::vector<BrushStroke> strokes;
    strokes.reserve((size_t)n);
    for (int j = 0; j < n; j++) {
        // the middle stroke is the full, widest one; the others are thinner and start / end a
        // little off it, like the separate bristle clumps of one big brush (Pachislop's draws,
        // in its order: none for the main stroke)
        const bool isMain = j == mainStroke;
        const double wf = isMain ? 1.0 : 0.45 + 0.5 * rand();
        const double t0 = isMain ? 0.0 : 0.15 * rand();
        const double t1 = isMain ? 1.0 : 1.0 - 0.3 * rand();
        const double off = (j - (n - 1) / 2.0) * spacing + (isMain ? 0.0 : (rand() - 0.5) * 0.3 * spacing);
        const double sSeed = seed * 13.0 + j * 7.31;
        const double width = W * wf;
        const int M = (int)std::min(1000.0, std::max(16.0, std::ceil(((t1 - t0) * pathLen) / 4.0)));
        BrushStroke s;
        s.x.resize((size_t)M);
        s.y.resize((size_t)M);
        s.tx.resize((size_t)M);
        s.ty.resize((size_t)M);
        s.hw.resize((size_t)M);
        s.len.resize((size_t)M);
        s.width = width;
        s.seed = gen::hashN({sSeed, 3.0}) * 100.0;
        for (int k = 0; k < M; k++) {
            const double u = (double)k / (double)(M - 1);
            // the hand wobbles: a slow sideways drift of about a tenth of the width
            const double wob = 0.2 * width * (noise1(u * 5.0, sSeed) - 0.5);
            double x = 0.0, y = 0.0;
            pathPoint(kind, t0 + (t1 - t0) * u, R, a0, sweep, off + wob, x, y);
            s.x[(size_t)k] = (float)(p.cx + x);
            s.y[(size_t)k] = (float)(p.cy + y);
            // pressure: the press-in swells fast, the lift-off thins out over a long tail
            const double in = 1.0 - std::min(1.0, u / inLen);
            const double swell = inStart + (1.0 - inStart) * (1.0 - in * in);
            const double lift = outEnd + (1.0 - outEnd) * std::pow(std::min(1.0, (1.0 - u) / outLen), 0.8);
            s.hw[(size_t)k] = (float)(0.5 * width * swell * lift * (0.8 + 0.4 * noise1(u * 3.0, sSeed + 1.0)));
        }
        for (int k = 0; k < M; k++) {
            const size_t a = (size_t)std::max(0, k - 1), b = (size_t)std::min(M - 1, k + 1);
            const double dx = (double)s.x[b] - (double)s.x[a];
            const double dy = (double)s.y[b] - (double)s.y[a];
            double d = std::hypot(dx, dy);
            if (d == 0.0) d = 1.0;
            s.tx[(size_t)k] = (float)(dx / d);
            s.ty[(size_t)k] = (float)(dy / d);
            s.len[(size_t)k] = k == 0 ? 0.0f
                                      : (float)((double)s.len[(size_t)k - 1] + std::hypot((double)s.x[(size_t)k] - (double)s.x[(size_t)k - 1],
                                                                                          (double)s.y[(size_t)k] - (double)s.y[(size_t)k - 1]));
        }
        // round caps: the brush tip is round, so both ends close in a half disc of their width
        const double total = s.len[(size_t)M - 1];
        const double h0 = std::max(1e-3, (double)s.hw[0]);
        const double h1 = std::max(1e-3, (double)s.hw[(size_t)M - 1]);
        for (int k = 0; k < M; k++) {
            const double a = 1.0 - std::min(1.0, (double)s.len[(size_t)k] / h0);
            const double b = 1.0 - std::min(1.0, (total - (double)s.len[(size_t)k]) / h1);
            s.hw[(size_t)k] = (float)((double)s.hw[(size_t)k] * std::sqrt(std::max(0.0, 1.0 - a * a)) * std::sqrt(std::max(0.0, 1.0 - b * b)));
        }
        strokes.push_back(std::move(s));
    }
    return strokes;
}

std::vector<BrushRibbonVertex> analysis::brushRibbon(const std::vector<BrushStroke>& strokes, double padPx) {
    size_t quads = 0;
    for (const BrushStroke& s : strokes) quads += s.size() > 1 ? s.size() - 1 : 0;
    std::vector<BrushRibbonVertex> out;
    out.reserve(quads * 6);
    std::vector<BrushRibbonVertex> pair(4);
    for (const BrushStroke& s : strokes) {
        const size_t M = s.size();
        if (M < 2) continue;
        for (size_t k = 0; k < M; k++) {
            const double e = (double)s.hw[k] + padPx;
            for (int side = -1; side <= 1; side += 2) {
                // the normal is the tangent turned left: (-ty, tx)
                BrushRibbonVertex v;
                v.x = (float)((double)s.x[k] - (double)s.ty[k] * e * side);
                v.y = (float)((double)s.y[k] + (double)s.tx[k] * e * side);
                v.u = (float)((double)k / (double)(M - 1));
                v.across = (float)(e * side);
                v.len = s.len[k];
                v.hw = s.hw[k];
                v.seed = (float)s.seed;
                v.width = (float)s.width;
                // slots 0, 1: the previous sample's sides; 2, 3: this one's
                pair[(size_t)(side < 0 ? 2 : 3)] = v;
            }
            if (k > 0) {
                // Pachislop's index order: (a, a+1, a+2), (a+1, a+3, a+2)
                for (int i : {0, 1, 2, 1, 3, 2}) out.push_back(pair[(size_t)i]);
            }
            pair[0] = pair[2];
            pair[1] = pair[3];
        }
    }
    return out;
}

std::vector<BrushDroplet> analysis::brushSplatter(const std::vector<BrushStroke>& strokes, double splatter, double seed, double reveal) {
    std::vector<BrushDroplet> out;
    const int count = (int)std::round(std::clamp(splatter, 0.0, 100.0) / 100.0 * BRUSH_MAX_SPLATS);
    if (count == 0 || strokes.empty()) return out;
    if (std::any_of(strokes.begin(), strokes.end(), [](const BrushStroke& s) { return s.size() == 0; })) return out;
    gen::Rng rand(seed * 31.0 + 5.0);
    const size_t n = strokes.size();
    for (int i = 0; i < count; i++) {
        const BrushStroke& s = strokes[std::min(n - 1, (size_t)std::floor(rand() * (double)n))];
        const size_t M = s.size();
        const double W = s.width;
        double x = 0.0, y = 0.0;
        size_t k = 0;
        double d = W * (0.04 + 0.22 * std::pow(rand(), 3.0));
        if (rand() < 0.7) {
            // sprayed off a side, more toward the fast tail
            k = std::min(M - 1, (size_t)std::floor(std::pow(rand(), 0.7) * (double)M));
            const double side = rand() < 0.5 ? -1.0 : 1.0;
            const double r = rand();
            const double dist = (double)s.hw[k] + W * (0.08 + 1.4 * r * r);
            const double along = (rand() - 0.5) * 0.6 * W;
            x = (double)s.x[k] - (double)s.ty[k] * dist * side + (double)s.tx[k] * along;
            y = (double)s.y[k] + (double)s.tx[k] * dist * side + (double)s.ty[k] * along;
        } else {
            // flicked on past the end of the stroke
            k = M - 1;
            const double ahead = W * (0.3 + 2.2 * rand());
            const double aside = (rand() - 0.5) * 0.5 * W;
            x = (double)s.x[k] + (double)s.tx[k] * ahead - (double)s.ty[k] * aside;
            y = (double)s.y[k] + (double)s.ty[k] * ahead + (double)s.tx[k] * aside;
            d *= 0.7;
        }
        const double stretch = rand() < 0.35 ? 1.5 + 2.0 * rand() : 1.0;
        const double noise = rand() * 100.0;
        // every value of the droplet is drawn before the reveal decides, so the stream (and every
        // later droplet) is the full stroke's
        const float u = M < 2 ? 1.0f : (float)((double)k / (double)(M - 1));
        if (reveal < 1.0 && u > reveal) continue;
        BrushDroplet drop;
        drop.cx = (float)x;
        drop.cy = (float)y;
        drop.w = (float)(d * stretch);
        drop.h = (float)d;
        drop.rotation = (float)std::atan2((double)s.ty[k], (double)s.tx[k]);
        drop.noise = (float)noise;
        drop.u = u;
        out.push_back(drop);
    }
    return out;
}

double analysis::brushPathLength(const BrushStrokeParams& p) {
    const double R = std::max(1.0, p.radius);
    if (p.path == BrushPath::Slash) return 2.0 * R;
    if (p.path == BrushPath::Ring) return (2.0 * PI + 0.4) * R;
    return std::max(1.0, p.sweep) * DEG * R * (p.path == BrushPath::Spiral ? 0.65 : 1.0);
}

double analysis::brushReach(const BrushStrokeParams& p, double splatter) {
    const double R = std::max(1.0, p.radius);
    const int n = strokeCount(p);
    const double W = std::max(1.0, p.width);
    // ring overshoot / slash bow, the outermost stroke's offset and jitter, its half width with
    // pressure swell and wobble, then the splatter's reach (side spray and end flicks)
    const double ink = 1.06 * R + 0.5 * (n - 1) * std::abs(p.spacing) * 1.15 + 0.75 * W;
    return ink + (splatter > 0.0 ? 3.4 * W : 0.0) + 2.0;
}
