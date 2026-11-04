// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/distort.ts by SafEight, used with the author's permission.

#include "Distort.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    // the smallest homography denominator kept: points past the vanishing line stay finite
    constexpr double MIN_W = 0.02;

    std::function<TitleVec2(const TitleVec2&)> cornersMap(const TitleRect& f, const DistortParams& d) {
        const double w = f.width, h = f.height;
        const double fx0 = f.x, fy0 = f.y, fx1 = f.x + f.width, fy1 = f.y + f.height;
        const TitleVec2 tl = d.corners[0], tr = d.corners[1], br = d.corners[2], bl = d.corners[3];
        // unit square (u right, v down from the top edge) -> quad TL (0,0), TR (1,0), BR (1,1), BL (0,1)
        const double x0 = fx0 + tl.x * w, y0 = fy1 + tl.y * h;
        const double x1 = fx1 + tr.x * w, y1 = fy1 + tr.y * h;
        const double x2 = fx1 + br.x * w, y2 = fy0 + br.y * h;
        const double x3 = fx0 + bl.x * w, y3 = fy0 + bl.y * h;
        // Heckbert's square-to-quad
        const double dx3 = x0 - x1 + x2 - x3;
        const double dy3 = y0 - y1 + y2 - y3;
        double a, b, c, e, g = 0, k = 0;
        if (std::abs(dx3) < 1e-9 && std::abs(dy3) < 1e-9) {
            a = x1 - x0;
            b = x2 - x1;
            c = y1 - y0;
            e = y2 - y1;
        } else {
            const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
            const double det = dx1 * dy2 - dx2 * dy1;
            if (std::abs(det) > 1e-12) {
                g = (dx3 * dy2 - dx2 * dy3) / det;
                k = (dx1 * dy3 - dx3 * dy1) / det;
            }
            a = x1 - x0 + g * x1;
            b = x3 - x0 + k * x3;
            c = y1 - y0 + g * y1;
            e = y3 - y0 + k * y3;
        }
        return [=](const TitleVec2& p) {
            const double u = (p.x - fx0) / w;
            const double v = (fy1 - p.y) / h;
            const double den = std::max(g * u + k * v + 1, MIN_W);
            return TitleVec2{(a * u + b * v + x0) / den, (c * u + e * v + y0) / den};
        };
    }

    // cubic Bernstein weights and their derivatives at t
    void cubicWeights(double t, double b[4], double db[4]) {
        const double s = 1 - t;
        b[0] = s * s * s;
        b[1] = 3 * t * s * s;
        b[2] = 3 * t * t * s;
        b[3] = t * t * t;
        db[0] = -3 * s * s;
        db[1] = 3 * s * s - 6 * t * s;
        db[2] = 6 * t * s - 3 * t * t;
        db[3] = 3 * t * t;
    }

    std::function<TitleVec2(const TitleVec2&)> meshMap(const TitleRect& f, const DistortParams& d) {
        const double w = f.width, h = f.height;
        const double fx0 = f.x, fy1 = f.y + f.height;
        std::array<double, 16> xs{}, ys{};
        for (int i = 0; i < 16; i++) {
            const TitleVec2 o = d.mesh[i];
            xs[i] = fx0 + (i % 4) / 3.0 * w + o.x * w;
            ys[i] = fy1 - (i / 4) / 3.0 * h + o.y * h;
        }
        return [=](const TitleVec2& p) {
            const double u = (p.x - fx0) / w;
            const double v = (fy1 - p.y) / h;
            const double uc = std::clamp(u, 0.0, 1.0), vc = std::clamp(v, 0.0, 1.0);
            double bu[4], du[4], bv[4], dv[4];
            cubicWeights(uc, bu, du);
            cubicWeights(vc, bv, dv);
            // S + du Su + dv Sv + du dv Suv: the patch inside, its C1 linear extension outside
            const double eu = u - uc, ev = v - vc;
            double x = 0, y = 0;
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    const double kk = bu[c] * bv[r] + eu * du[c] * bv[r] + ev * bu[c] * dv[r] + eu * ev * du[c] * dv[r];
                    x += kk * xs[r * 4 + c];
                    y += kk * ys[r * 4 + c];
                }
            }
            return TitleVec2{x, y};
        };
    }

}

DistortParams jasperengine::titleart::geometry::decodeDistort(const Json& distort) {
    DistortParams p;
    if (!distort.is_object()) return p;
    const std::string mode = distort.value("mode", std::string("none"));
    p.mode = mode == "corners" ? DistortMode::Corners : mode == "mesh" ? DistortMode::Mesh : DistortMode::None;
    const Json& corners = distort.contains("corners") ? distort["corners"] : Json();
    if (corners.is_array()) {
        for (size_t i = 0; i < 4 && i < corners.size(); i++) p.corners[i] = decodeVec2(corners[i]);
    }
    const Json& mesh = distort.contains("mesh") ? distort["mesh"] : Json();
    if (mesh.is_array()) {
        for (size_t i = 0; i < 16 && i < mesh.size(); i++) p.mesh[i] = decodeVec2(mesh[i]);
    }
    return p;
}

bool jasperengine::titleart::geometry::distortIsIdentity(const DistortParams& d) {
    const auto zero = [](const TitleVec2& v) { return v.x == 0 && v.y == 0; };
    if (d.mode == DistortMode::Corners) return std::all_of(d.corners.begin(), d.corners.end(), zero);
    if (d.mode == DistortMode::Mesh) return std::all_of(d.mesh.begin(), d.mesh.end(), zero);
    return true;
}

std::optional<PointMap> jasperengine::titleart::geometry::makeDistorter(const TitleRect& frame, const DistortParams& d) {
    if (!(frame.width > 1e-6) || !(frame.height > 1e-6) || distortIsIdentity(d)) return std::nullopt;
    const double size = std::max(frame.width, frame.height);
    PointMap pm;
    pm.map = d.mode == DistortMode::Corners ? cornersMap(frame, d) : meshMap(frame, d);
    pm.tol = std::max(size * 1.5e-4, 1e-3);
    pm.span = size / 8;
    pm.straight = d.mode == DistortMode::Corners;
    return pm;
}

TitleVec2 jasperengine::titleart::geometry::distortPoint(const TitleRect& frame, const DistortParams& d, const TitleVec2& p) {
    const std::optional<PointMap> pm = makeDistorter(frame, d);
    return pm ? pm->map(p) : p;
}

Path jasperengine::titleart::geometry::distortPath(const Path& path, const TitleRect& frame, const DistortParams& d) {
    const std::optional<PointMap> pm = makeDistorter(frame, d);
    return pm ? remapPath(path, *pm) : path;
}
