// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/remap.ts by SafEight, used with the author's permission.

#include "Remap.h"

#include "PathUtil.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr int MAX_FIT_DEPTH = 12;

    // appends the mapped image of an original curve at(t), t in [0, 1], as quadratics re-fitted
    // through mapped samples (or lines where the image stays straight)
    template <typename At>
    void fitMapped(Path& out, const At& at, double length, const PointMap& pm) {
        const int pieces = std::max(1, (int)std::ceil(length / pm.span));
        struct Fitter {
            Path& out;
            const At& at;
            const PointMap& pm;
            void fit(double t0, const TitleVec2& p0, double t1, const TitleVec2& p1, int depth) {
                const TitleVec2 m = pm.map(at((t0 + t1) / 2));
                // the quadratic through p0, m (at t = 1/2) and p1
                const double cx = 2 * m.x - (p0.x + p1.x) / 2;
                const double cy = 2 * m.y - (p0.y + p1.y) / 2;
                const TitleVec2 qa = pm.map(at(t0 + (t1 - t0) / 4));
                const TitleVec2 qb = pm.map(at(t0 + (t1 - t0) * 3 / 4));
                const double ea = std::hypot((9 * p0.x + 6 * cx + p1.x) / 16 - qa.x, (9 * p0.y + 6 * cy + p1.y) / 16 - qa.y);
                const double eb = std::hypot((p0.x + 6 * cx + 9 * p1.x) / 16 - qb.x, (p0.y + 6 * cy + 9 * p1.y) / 16 - qb.y);
                if (std::max(ea, eb) > pm.tol && depth < MAX_FIT_DEPTH) {
                    const double tm = (t0 + t1) / 2;
                    fit(t0, p0, tm, m, depth + 1);
                    fit(tm, m, t1, p1, depth + 1);
                    return;
                }
                if (std::hypot(cx - (p0.x + p1.x) / 2, cy - (p0.y + p1.y) / 2) < pm.tol) {
                    out.lineTo(p1.x, p1.y);
                } else {
                    out.quadTo(cx, cy, p1.x, p1.y);
                }
            }
        } fitter{out, at, pm};
        TitleVec2 p0 = pm.map(at(0.0));
        for (int i = 1; i <= pieces; i++) {
            const double t1 = (double)i / pieces;
            const TitleVec2 p1 = pm.map(at(t1));
            fitter.fit((double)(i - 1) / pieces, p0, t1, p1, 0);
            p0 = p1;
        }
    }

    struct Remapper {
        const PointMap& pm;
        Path out;

        void lineSegment(const TitleVec2& a, const TitleVec2& b) {
            if (pm.straight) {
                const TitleVec2 p = pm.map(b);
                out.lineTo(p.x, p.y);
                return;
            }
            fitMapped(out, [&](double t) { return lerpPoint(a, b, t); }, pointDistance(a, b), pm);
        }

        void move(const TitleVec2& p) {
            const TitleVec2 q = pm.map(p);
            out.moveTo(q.x, q.y);
        }
        void line(const TitleVec2& from, const TitleVec2& p) { lineSegment(from, p); }
        void quad(const TitleVec2& from, const TitleVec2& c, const TitleVec2& p) {
            fitMapped(out, [&](double t) { return quadPoint(from, c, p, t); }, pointDistance(from, c) + pointDistance(c, p), pm);
        }
        void cubic(const TitleVec2& from, const TitleVec2& c1, const TitleVec2& c2, const TitleVec2& p) {
            fitMapped(out, [&](double t) { return cubicPoint(from, c1, c2, p, t); },
                      pointDistance(from, c1) + pointDistance(c1, c2) + pointDistance(c2, p), pm);
        }
        // the implicit closing edge is straight in the original but may bend under the map; open
        // contours (SVG strokes) stay open
        void close(const TitleVec2& current, const TitleVec2& start, bool closed) {
            if (!closed) return;
            if (current.x != start.x || current.y != start.y) lineSegment(current, start);
            out.close();
        }
    };

}

Path jasperengine::titleart::geometry::remapPath(const Path& path, const PointMap& map) {
    Remapper remapper{map, {}};
    remapper.out.evenOdd = path.evenOdd;
    walkPath(path, remapper);
    return remapper.out;
}
