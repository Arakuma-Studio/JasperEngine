// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/trace.ts by SafEight, used with the author's permission.

#include "Trace.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    // most alpha samples along the image's longer side
    constexpr int MAX_SAMPLES = 1024;
    // alpha samples per layer unit of the fitted size (headroom for 2x exports) below the cap
    constexpr double SAMPLES_PER_PX = 2;
    // the least simplify tolerance, in samples: collinear runs go, every pixel step stays
    constexpr double MIN_TOLERANCE = 0.3;
    // a vertex is a corner when its deflection x shorter adjacent segment exceeds this x tolerance
    // (curves stay <= 8)
    constexpr double CORNER_K = 12;
    constexpr int CHAIKIN_PASSES = 2;

    struct AlphaGrid {
        int sw = 0, sh = 0;             // sample counts (unpadded)
        std::vector<double> a;          // (sw + 2) x (sh + 2), rows top first, one transparent sample all round
    };

    // the weights of an area-average resample from `src` to `dst` samples along one axis
    struct AxisWeights {
        std::vector<int> first;
        std::vector<std::vector<double>> weights;
    };

    AxisWeights axisWeights(int src, int dst) {
        AxisWeights out;
        const double scale = (double)src / dst;
        for (int i = 0; i < dst; i++) {
            const double s0 = i * scale, s1 = (i + 1) * scale;
            const int k0 = (int)std::floor(s0), k1 = std::min(src, (int)std::ceil(s1));
            out.first.push_back(k0);
            std::vector<double> w;
            for (int k = k0; k < k1; k++) w.push_back((std::min(s1, (double)k + 1) - std::max(s0, (double)k)) / scale);
            out.weights.push_back(std::move(w));
        }
        return out;
    }

    AlphaGrid alphaGrid(const TitleImageData& img, int n) {
        const int w = img.width, h = img.height;
        const double k = (double)n / std::max({w, h, 1});
        AlphaGrid g;
        g.sw = std::max(1, (int)std::lround(w * k));
        g.sh = std::max(1, (int)std::lround(h * k));
        const AxisWeights wx = axisWeights(w, g.sw), wy = axisWeights(h, g.sh);
        // columns first, then rows
        std::vector<double> rows((size_t)g.sw * h, 0.0);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < g.sw; x++) {
                double v = 0;
                for (size_t j = 0; j < wx.weights[x].size(); j++) {
                    v += wx.weights[x][j] * img.rgba[((size_t)y * w + wx.first[x] + j) * 4 + 3];
                }
                rows[(size_t)y * g.sw + x] = v;
            }
        }
        const int gw = g.sw + 2;
        g.a.assign((size_t)gw * (g.sh + 2), 0.0);
        for (int y = 0; y < g.sh; y++) {
            for (int x = 0; x < g.sw; x++) {
                double v = 0;
                for (size_t j = 0; j < wy.weights[y].size(); j++) v += wy.weights[y][j] * rows[(wy.first[y] + j) * g.sw + x];
                // the canvas Pachislop reads back stores whole alpha codes
                g.a[(size_t)(y + 1) * gw + x + 1] = std::round(v);
            }
        }
        return g;
    }

    using Loop = std::vector<double>;   // x, y interleaved

    // marching squares at alpha t -> closed loops in unit space (y up, centred, larger side 1),
    // outer loops counter-clockwise, holes clockwise
    std::vector<Loop> marchingSquares(const AlphaGrid& g, double t, double ux, double uy) {
        const int gw = g.sw + 2, gh = g.sh + 2;
        const std::vector<double>& a = g.a;
        // edge ids: 2p = the horizontal edge from grid point p to p + 1, 2p + 1 = the vertical
        // edge from p to p + gw
        std::vector<int32_t> next((size_t)gw * gh * 2, -1);
        for (int cy = 0; cy < gh - 1; cy++) {
            for (int cx = 0; cx < gw - 1; cx++) {
                const int p = cy * gw + cx;
                const int tl = a[p] >= t ? 8 : 0;
                const int tr = a[p + 1] >= t ? 4 : 0;
                const int br = a[p + gw + 1] >= t ? 2 : 0;
                const int bl = a[p + gw] >= t ? 1 : 0;
                const int code = tl | tr | br | bl;
                if (code == 0 || code == 15) continue;
                const int T = 2 * p, L = 2 * p + 1, B = 2 * (p + gw), R = 2 * (p + 1) + 1;
                // inside on the LEFT of travel as drawn (image rows run down, so the screen's left
                // too): counter-clockwise around islands, clockwise around holes once y is flipped
                switch (code) {
                    case 8: next[L] = T; break;
                    case 4: next[T] = R; break;
                    case 2: next[R] = B; break;
                    case 1: next[B] = L; break;
                    case 7: next[T] = L; break;
                    case 11: next[R] = T; break;
                    case 13: next[B] = R; break;
                    case 14: next[L] = B; break;
                    case 12: next[L] = R; break;
                    case 6: next[T] = B; break;
                    case 3: next[R] = L; break;
                    case 9: next[B] = T; break;
                    case 10:    // tl + br inside
                        if (a[p] + a[p + 1] + a[p + gw] + a[p + gw + 1] >= 4 * t) {
                            next[R] = T;
                            next[L] = B;
                        } else {
                            next[L] = T;
                            next[R] = B;
                        }
                        break;
                    case 5:     // tr + bl inside
                        if (a[p] + a[p + 1] + a[p + gw] + a[p + gw + 1] >= 4 * t) {
                            next[T] = L;
                            next[B] = R;
                        } else {
                            next[T] = R;
                            next[B] = L;
                        }
                        break;
                    default: break;
                }
            }
        }
        // grid point (x, y) is sample (x - 1, y - 1), centred at image u = (x - 0.5) / sw
        const double fx = ux / g.sw, fy = uy / g.sh;
        std::vector<Loop> loops;
        for (size_t start = 0; start < next.size(); start++) {
            if (next[start] < 0) continue;
            Loop pts;
            int32_t e = (int32_t)start;
            while (next[e] >= 0) {
                const int p = e >> 1;
                const int x = p % gw, y = (p - x) / gw;
                const double a0 = a[p];
                double gx = x, gy = y;
                if (e & 1) {
                    gy += (t - a0) / (a[p + gw] - a0);
                } else {
                    gx += (t - a0) / (a[p + 1] - a0);
                }
                const double px = (gx - 0.5) * fx - ux / 2;
                const double py = uy / 2 - (gy - 0.5) * fy;
                // a sample exactly at the threshold puts the crossings of its edges on it: one vertex
                if (pts.empty() || px != pts[pts.size() - 2] || py != pts[pts.size() - 1]) {
                    pts.push_back(px);
                    pts.push_back(py);
                }
                const int32_t n = next[e];
                next[e] = -1;
                e = n;
            }
            if (pts.size() > 2 && pts[0] == pts[pts.size() - 2] && pts[1] == pts[pts.size() - 1]) pts.resize(pts.size() - 2);
            if (pts.size() >= 6) loops.push_back(std::move(pts));
        }
        return loops;
    }

    double signedArea(const Loop& p) {
        double s = 0;
        const size_t n = p.size();
        for (size_t i = 0, j = n - 2; i < n; j = i, i += 2) s += p[j] * p[i + 1] - p[i] * p[j + 1];
        return s / 2;
    }

    // whether vertex i comes before vertex j in (x, y) order: every choice below goes by this
    // order, never by index, so the result depends on the loop's points alone
    bool before(const Loop& p, size_t i, size_t j) {
        return p[2 * i] < p[2 * j] || (p[2 * i] == p[2 * j] && p[2 * i + 1] < p[2 * j + 1]);
    }

    // squared distance from vertex i to the segment between vertices a and b, bit-identical for
    // (a, b) and (b, a)
    double segDist2(const Loop& p, size_t i, size_t a, size_t b) {
        if (before(p, b, a)) std::swap(a, b);
        const double ax = p[2 * a], ay = p[2 * a + 1];
        const double px = p[2 * i] - ax, py = p[2 * i + 1] - ay;
        const double dx = p[2 * b] - ax, dy = p[2 * b + 1] - ay;
        const double l2 = dx * dx + dy * dy;
        const double s = l2 > 0 ? std::clamp((px * dx + py * dy) / l2, 0.0, 1.0) : 0;
        const double ex = px - s * dx, ey = py - s * dy;
        return ex * ex + ey * ey;
    }

    // Douglas-Peucker on a closed loop without consecutive duplicates. The loop is split at its
    // first vertex in (x, y) order and the vertex farthest from it, each chain is simplified with
    // ties going to the first vertex in that order, and an anchor left on a straight run is let
    // go: the kept set is the same for every start vertex and both walk directions
    Loop simplifyClosed(const Loop& p, double tol) {
        const size_t n = p.size() / 2;
        if (n < 4) return p;
        size_t a = 0;
        for (size_t i = 1; i < n; i++) {
            if (before(p, i, a)) a = i;
        }
        size_t b = a;
        double best = -1;
        for (size_t i = 0; i < n; i++) {
            const double d = (p[2 * i] - p[2 * a]) * (p[2 * i] - p[2 * a]) + (p[2 * i + 1] - p[2 * a + 1]) * (p[2 * i + 1] - p[2 * a + 1]);
            if (d > best || (d == best && before(p, i, b))) {
                best = d;
                b = i;
            }
        }
        const double tol2 = tol * tol;
        std::vector<uint8_t> keep(n, 0);
        keep[a] = 1;
        keep[b] = 1;
        // positions along the loop from a (position k = vertex (a + k) % n; n = a again)
        const size_t mid = (b + n - a) % n;
        std::vector<size_t> stack = {0, mid, mid, n};
        while (!stack.empty()) {
            const size_t j = stack.back();
            stack.pop_back();
            const size_t i = stack.back();
            stack.pop_back();
            if (j < i + 2) continue;
            const size_t vi = (a + i) % n, vj = (a + j) % n;
            double dmax = -1;
            size_t k = 0;
            bool found = false;
            for (size_t m = i + 1; m < j; m++) {
                const size_t v = (a + m) % n;
                const double d = segDist2(p, v, vi, vj);
                if (d > dmax || (found && d == dmax && before(p, v, (a + k) % n))) {
                    dmax = d;
                    k = m;
                    found = true;
                }
            }
            if (dmax > tol2) {
                keep[(a + k) % n] = 1;
                stack.insert(stack.end(), {i, k, k, j});
            }
        }
        size_t kept = 0;
        for (size_t i = 0; i < n; i++) kept += keep[i];
        for (size_t v : {a, b}) {
            if (kept <= 3) break;
            size_t u = v;
            do u = (u + n - 1) % n;
            while (!keep[u]);
            size_t w = v;
            do w = (w + 1) % n;
            while (!keep[w]);
            bool straight = true;
            for (size_t m = (u + 1) % n; straight && m != w; m = (m + 1) % n) straight = segDist2(p, m, u, w) <= tol2;
            if (straight) {
                keep[v] = 0;
                kept--;
            }
        }
        Loop out;
        for (size_t i = 0; i < n; i++) {
            if (keep[i]) {
                out.push_back(p[2 * i]);
                out.push_back(p[2 * i + 1]);
            }
        }
        return out;
    }

    // Chaikin corner cutting of a closed loop, except at its real corners
    Loop smoothClosed(const Loop& p, double tol) {
        const size_t n = p.size() / 2;
        std::vector<uint8_t> corner(n, 0);
        for (size_t i = 0; i < n; i++) {
            const size_t a = ((i + n - 1) % n) * 2, c = ((i + 1) % n) * 2, v = i * 2;
            const double e1x = p[v] - p[a], e1y = p[v + 1] - p[a + 1];
            const double e2x = p[c] - p[v], e2y = p[c + 1] - p[v + 1];
            const double l1 = std::hypot(e1x, e1y), l2 = std::hypot(e2x, e2y);
            if (l1 < 1e-12 || l2 < 1e-12) {
                corner[i] = 1;
                continue;
            }
            const double theta = std::acos(std::clamp((e1x * e2x + e1y * e2y) / (l1 * l2), -1.0, 1.0));
            corner[i] = theta * std::min(l1, l2) > CORNER_K * tol ? 1 : 0;
        }
        Loop pts = p;
        for (int pass = 0; pass < CHAIKIN_PASSES; pass++) {
            const size_t m = pts.size() / 2;
            Loop out;
            std::vector<uint8_t> oc;
            for (size_t i = 0; i < m; i++) {
                const size_t v = i * 2;
                if (corner[i]) {
                    out.push_back(pts[v]);
                    out.push_back(pts[v + 1]);
                    oc.push_back(1);
                    continue;
                }
                const size_t a = ((i + m - 1) % m) * 2, c = ((i + 1) % m) * 2;
                out.push_back(0.75 * pts[v] + 0.25 * pts[a]);
                out.push_back(0.75 * pts[v + 1] + 0.25 * pts[a + 1]);
                out.push_back(0.75 * pts[v] + 0.25 * pts[c]);
                out.push_back(0.75 * pts[v + 1] + 0.25 * pts[c + 1]);
                oc.push_back(0);
                oc.push_back(0);
            }
            pts = std::move(out);
            corner = std::move(oc);
        }
        return pts;
    }

}

TraceParams jasperengine::titleart::geometry::decodeTrace(const Json& trace) {
    TraceParams t;
    if (!trace.is_object()) return t;
    t.threshold = trace.value("threshold", 128);
    t.smooth = trace.value("smooth", 0.0);
    t.despeckle = trace.value("despeckle", 0.0);
    return t;
}

TitleVec2 jasperengine::titleart::geometry::imageHalfExtents(int width, int height, double size) {
    const double k = size / std::max({width, height, 1});
    return {width * k / 2, height * k / 2};
}

Path jasperengine::titleart::geometry::traceImage(const TitleImageData& img, double size, const TraceParams& trace) {
    Path out;
    const int w = img.width, h = img.height;
    if (w <= 0 || h <= 0 || !(size > 0) || img.rgba.size() < (size_t)w * h * 4) return out;
    const int n = std::max(1, std::min({MAX_SAMPLES, std::max(w, h), (int)std::ceil(size * SAMPLES_PER_PX)}));
    const double m = std::max({w, h, 1});
    const double t = std::clamp(trace.threshold, 1, 255);
    const double minArea = std::max(0.0, trace.despeckle) / 100;
    const double smooth = std::max(0.0, trace.smooth) / 100;
    const double tol = std::max(smooth, MIN_TOLERANCE / n);
    for (const Loop& loop : marchingSquares(alphaGrid(img, n), t, w / m, h / m)) {
        const double area = signedArea(loop);
        if (std::abs(area) < minArea) continue;
        Loop p = simplifyClosed(loop, tol);
        if (smooth > 0 && p.size() >= 6) p = smoothClosed(p, tol);
        // a loop that collapsed or flipped while simplifying is a sliver: drop it
        if (p.size() < 6 || (signedArea(p) > 0) != (area > 0)) continue;
        // start every loop at its first vertex in (x, y) order
        size_t s = 0;
        for (size_t i = 1; i < p.size() / 2; i++) {
            if (before(p, i, s)) s = i;
        }
        std::rotate(p.begin(), p.begin() + 2 * s, p.end());
        for (size_t i = 0; i < p.size(); i += 2) {
            if (i == 0) {
                out.moveTo(p[i] * size, p[i + 1] * size);
            } else {
                out.lineTo(p[i] * size, p[i + 1] * size);
            }
        }
        out.close();
    }
    return out;
}
