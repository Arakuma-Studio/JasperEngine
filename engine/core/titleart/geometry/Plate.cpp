// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/plate.ts by SafEight, used with the author's permission.

#include "Plate.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr double PI = 3.14159265358979323846;

    double cross(const TitleVec2& o, const TitleVec2& a, const TitleVec2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    }

    // a box around `pts` with sides parallel to the lean (x = u + k y, k = tan lean), top and
    // bottom horizontal, each corner cut by a straight chamfer `chamfer` along both edges (clamped
    // to half the shorter side); counter-clockwise from the bottom-left
    Polygon leanBox(const Polygon& pts, double leanDeg, double chamfer) {
        const double k = std::tan(leanDeg * PI / 180);
        double u0 = std::numeric_limits<double>::infinity(), u1 = -u0, y0 = u0, y1 = -u0;
        for (const TitleVec2& p : pts) {
            const double u = p.x - k * p.y;
            u0 = std::min(u0, u);
            u1 = std::max(u1, u);
            y0 = std::min(y0, p.y);
            y1 = std::max(y1, p.y);
        }
        const Polygon corners = {{u0 + k * y0, y0}, {u1 + k * y0, y0}, {u1 + k * y1, y1}, {u0 + k * y1, y1}};
        const double side = std::min(u1 - u0, (y1 - y0) * std::sqrt(1 + k * k));
        const double c = std::min(std::max(chamfer, 0.0), side / 2);
        if (c <= 1e-6) return corners;
        Polygon out;
        for (int i = 0; i < 4; i++) {
            const TitleVec2 p = corners[i];
            const TitleVec2 prev = corners[(i + 3) % 4];
            const TitleVec2 next = corners[(i + 1) % 4];
            const auto toward = [&](const TitleVec2& q) {
                const double l = std::hypot(q.x - p.x, q.y - p.y);
                return TitleVec2{p.x + (q.x - p.x) * c / l, p.y + (q.y - p.y) * c / l};
            };
            out.push_back(toward(prev));
            out.push_back(toward(next));
        }
        return out;
    }

}

PlateParams jasperengine::titleart::geometry::decodePlate(const Json& plate) {
    PlateParams p;
    if (!plate.is_object()) return p;
    const std::string kind = plate.value("kind", std::string("contour"));
    p.kind = kind == "hull" ? PlateKind::Hull : kind == "box" ? PlateKind::Box : PlateKind::Contour;
    p.close = plate.value("close", 0.0);
    p.corner = plate.value("corner", 4.0);
    p.slant = plate.value("slant", 0.0);
    return p;
}

bool jasperengine::titleart::geometry::plateIsIdentity(const PlateParams& plate) {
    return plate.kind == PlateKind::Contour && !(plate.close > 0);
}

Polygon jasperengine::titleart::geometry::convexHull(Polygon p) {
    std::sort(p.begin(), p.end(), [](const TitleVec2& a, const TitleVec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    if (p.size() < 3) return p;
    Polygon lower, upper;
    for (const TitleVec2& q : p) {
        while (lower.size() >= 2 && cross(lower[lower.size() - 2], lower.back(), q) <= 0) lower.pop_back();
        lower.push_back(q);
    }
    for (size_t i = p.size(); i-- > 0;) {
        const TitleVec2& q = p[i];
        while (upper.size() >= 2 && cross(upper[upper.size() - 2], upper.back(), q) <= 0) upper.pop_back();
        upper.push_back(q);
    }
    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

Path jasperengine::titleart::geometry::plateBase(const Path& outline, const PlateParams& plate, double size, double shear) {
    if (outline.empty() || plateIsIdentity(plate)) return outline;
    const double tol = 4 * flattenTolerance(size);
    const Polygons polys = flattenPath(outline, tol);
    if (plate.kind == PlateKind::Contour) {
        const double r = plate.close / 100 * size;
        return polygonsToPath(roundOffset(roundOffset(unionPolygons(polys, outline.evenOdd), r, tol), -r, tol));
    }
    Polygon pts;
    for (const Polygon& polygon : polys) pts.insert(pts.end(), polygon.begin(), polygon.end());
    if (pts.empty()) return {};
    const Polygon ring = plate.kind == PlateKind::Hull ? convexHull(pts) : leanBox(pts, plate.slant + shear, plate.corner / 100 * size);
    return ring.size() >= 3 ? polygonsToPath({ring}) : Path();
}
