// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.6)
// - Purpose: the segment walk every geometry stage shares (remap, offsets, plates, the mask
//   rasterizer, SVG reading): a Path's verbs as contours of segments with their start points,
//   plus the point maths of Bezier segments.
// - Thread-safety: pure inline functions.

#pragma once

#include "titleart/geometry/Path.h"

#include <cmath>

namespace jasperengine::titleart::geometry {

    inline TitleVec2 lerpPoint(const TitleVec2& a, const TitleVec2& b, double t) {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }

    inline double pointDistance(const TitleVec2& a, const TitleVec2& b) {
        return std::hypot(b.x - a.x, b.y - a.y);
    }

    inline TitleVec2 quadPoint(const TitleVec2& p0, const TitleVec2& p1, const TitleVec2& p2, double t) {
        const double u = 1 - t;
        return {u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y};
    }

    inline TitleVec2 cubicPoint(const TitleVec2& p0, const TitleVec2& p1, const TitleVec2& p2, const TitleVec2& p3, double t) {
        const double u = 1 - t;
        const double k0 = u * u * u, k1 = 3 * u * u * t, k2 = 3 * u * t * t, k3 = t * t * t;
        return {k0 * p0.x + k1 * p1.x + k2 * p2.x + k3 * p3.x, k0 * p0.y + k1 * p1.y + k2 * p2.y + k3 * p3.y};
    }

    // Visits a path's commands with their start points. `Visitor` has
    //   move(p), line(from, p), quad(from, c, p), cubic(from, c1, c2, p), close(from, start)
    // where close is called for every contour (closed or not) with `closed` telling which, so
    // stages that treat contours as polygons can add the implicit closing edge.
    template <typename Visitor>
    void walkPath(const Path& path, Visitor&& visit) {
        const std::vector<PathVerb>& verbs = path.verbList();
        const std::vector<TitleVec2>& pts = path.pointList();
        size_t i = 0;
        TitleVec2 current, start;
        bool open = false;
        for (PathVerb verb : verbs) {
            switch (verb) {
                case PathVerb::Move:
                    if (open) visit.close(current, start, false);
                    current = start = pts[i++];
                    visit.move(current);
                    open = true;
                    break;
                case PathVerb::Line:
                    if (!open) {
                        visit.move(current);
                        start = current;
                        open = true;
                    }
                    visit.line(current, pts[i]);
                    current = pts[i++];
                    break;
                case PathVerb::Quad:
                    if (!open) {
                        visit.move(current);
                        start = current;
                        open = true;
                    }
                    visit.quad(current, pts[i], pts[i + 1]);
                    current = pts[i + 1];
                    i += 2;
                    break;
                case PathVerb::Cubic:
                    if (!open) {
                        visit.move(current);
                        start = current;
                        open = true;
                    }
                    visit.cubic(current, pts[i], pts[i + 1], pts[i + 2]);
                    current = pts[i + 2];
                    i += 3;
                    break;
                case PathVerb::Close:
                    if (open) visit.close(current, start, true);
                    current = start;
                    open = false;
                    break;
            }
        }
        if (open) visit.close(current, start, false);
    }

    // the contours of a path as separate paths (each keeps its Close)
    std::vector<Path> splitContours(const Path& path);
    // a contour drawn backwards (same segments, reversed travel)
    Path reverseContour(const Path& contour);

}
