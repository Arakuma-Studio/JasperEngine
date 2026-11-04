// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/offset.ts by SafEight, used with the author's permission.

#include "Offset.h"

#include "PathUtil.h"

#include "clipper2/clipper.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    Clipper2Lib::PathsD toClipper(const Polygons& polygons) {
        Clipper2Lib::PathsD out;
        out.reserve(polygons.size());
        for (const Polygon& polygon : polygons) {
            Clipper2Lib::PathD path;
            path.reserve(polygon.size());
            for (const TitleVec2& p : polygon) path.emplace_back(p.x, p.y);
            out.push_back(std::move(path));
        }
        return out;
    }

    Polygons fromClipper(const Clipper2Lib::PathsD& paths) {
        Polygons out;
        out.reserve(paths.size());
        for (const Clipper2Lib::PathD& path : paths) {
            if (path.size() < 3) continue;
            Polygon polygon;
            polygon.reserve(path.size());
            for (const Clipper2Lib::PointD& p : path) polygon.push_back({p.x, p.y});
            out.push_back(std::move(polygon));
        }
        return out;
    }

    // contours as polylines with whether each was closed (strokes keep open ones open)
    struct Flattener {
        double tol;
        Polygons polygons;
        std::vector<bool> closed;
        Polygon current;

        void finish(bool isClosed, size_t minPoints) {
            if (current.size() >= minPoints) {
                polygons.push_back(std::move(current));
                closed.push_back(isClosed);
            }
            current.clear();
        }
        void move(const TitleVec2& p) { current.push_back(p); }
        void line(const TitleVec2&, const TitleVec2& p) { current.push_back(p); }
        void quad(const TitleVec2& from, const TitleVec2& c, const TitleVec2& p) {
            const double d = std::hypot(from.x - 2 * c.x + p.x, from.y - 2 * c.y + p.y);
            const int n = std::max(1, (int)std::ceil(std::sqrt(d / (4 * tol))));
            for (int i = 1; i <= n; i++) current.push_back(quadPoint(from, c, p, (double)i / n));
        }
        void cubic(const TitleVec2& from, const TitleVec2& c1, const TitleVec2& c2, const TitleVec2& p) {
            const double d = std::max(std::hypot(from.x - 2 * c1.x + c2.x, from.y - 2 * c1.y + c2.y),
                                      std::hypot(c1.x - 2 * c2.x + p.x, c1.y - 2 * c2.y + p.y));
            const int n = std::max(1, (int)std::ceil(std::sqrt(3 * d / (4 * tol))));
            for (int i = 1; i <= n; i++) current.push_back(cubicPoint(from, c1, c2, p, (double)i / n));
        }
        void close(const TitleVec2&, const TitleVec2&, bool isClosed) { finish(isClosed, isClosed ? 3 : 2); }
    };

}

double jasperengine::titleart::geometry::flattenTolerance(double size) {
    return std::max(std::abs(size) * 5e-5, 1e-3);
}

Polygons jasperengine::titleart::geometry::flattenPath(const Path& path, double tol) {
    Flattener f{tol, {}, {}, {}};
    walkPath(path, f);
    // a fill closes every contour (Canvas2D's and the non-zero rule's view of an open one)
    Polygons out;
    for (Polygon& polygon : f.polygons) {
        if (polygon.size() >= 3) out.push_back(std::move(polygon));
    }
    return out;
}

Path jasperengine::titleart::geometry::polygonsToPath(const Polygons& polygons) {
    Path out;
    for (const Polygon& polygon : polygons) {
        if (polygon.size() < 3) continue;
        out.moveTo(polygon[0].x, polygon[0].y);
        for (size_t i = 1; i < polygon.size(); i++) out.lineTo(polygon[i].x, polygon[i].y);
        out.close();
    }
    return out;
}

Polygons jasperengine::titleart::geometry::unionPolygons(const Polygons& polygons, bool evenOdd) {
    if (polygons.empty()) return polygons;
    const Clipper2Lib::PathsD result = Clipper2Lib::Union(
        toClipper(polygons), evenOdd ? Clipper2Lib::FillRule::EvenOdd : Clipper2Lib::FillRule::NonZero, CLIPPER_PRECISION);
    return fromClipper(result);
}

Polygons jasperengine::titleart::geometry::roundOffset(const Polygons& polygons, double delta, double tol) {
    if (polygons.empty() || delta == 0) return polygons;
    const Clipper2Lib::PathsD result = Clipper2Lib::InflatePaths(
        toClipper(polygons), delta, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon, 2.0, CLIPPER_PRECISION, tol);
    return fromClipper(result);
}

Path jasperengine::titleart::geometry::weightPath(const Path& path, double delta, double size) {
    if (delta == 0 || path.empty()) return path;
    const double tol = flattenTolerance(size);
    return polygonsToPath(roundOffset(unionPolygons(flattenPath(path, tol), path.evenOdd), delta, tol));
}

Path jasperengine::titleart::geometry::strokeOutline(const Path& path, double width, OffsetJoin join, OffsetCap cap,
                                                     double miterLimit, double size) {
    if (!(width > 0) || path.empty()) return {};
    const double tol = flattenTolerance(size);
    Flattener f{tol, {}, {}, {}};
    walkPath(path, f);
    const Clipper2Lib::JoinType jt = join == OffsetJoin::Round ? Clipper2Lib::JoinType::Round
                                   : join == OffsetJoin::Bevel ? Clipper2Lib::JoinType::Bevel
                                                               : Clipper2Lib::JoinType::Miter;
    const Clipper2Lib::EndType openEnd = cap == OffsetCap::Round ? Clipper2Lib::EndType::Round
                                       : cap == OffsetCap::Square ? Clipper2Lib::EndType::Square
                                                                  : Clipper2Lib::EndType::Butt;
    Polygons closedContours, openContours;
    for (size_t i = 0; i < f.polygons.size(); i++) {
        (f.closed[i] ? closedContours : openContours).push_back(f.polygons[i]);
    }
    Clipper2Lib::PathsD outline;
    const double limit = std::max(miterLimit, 1.0);
    if (!closedContours.empty()) {
        // a closed contour's stroke is the ring between its outline grown and shrunk by width / 2
        const Clipper2Lib::PathsD ring = Clipper2Lib::InflatePaths(toClipper(closedContours), width / 2, jt,
                                                                   Clipper2Lib::EndType::Joined, limit, CLIPPER_PRECISION, tol);
        outline.insert(outline.end(), ring.begin(), ring.end());
    }
    if (!openContours.empty()) {
        const Clipper2Lib::PathsD band = Clipper2Lib::InflatePaths(toClipper(openContours), width / 2, jt, openEnd, limit,
                                                                   CLIPPER_PRECISION, tol);
        outline.insert(outline.end(), band.begin(), band.end());
    }
    const Clipper2Lib::PathsD merged = Clipper2Lib::Union(outline, Clipper2Lib::FillRule::NonZero, CLIPPER_PRECISION);
    return polygonsToPath(fromClipper(merged));
}
