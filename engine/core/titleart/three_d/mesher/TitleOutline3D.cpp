// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/outline.ts by SafEight, used with the author's permission.

#include "TitleOutline3D.h"

#include "titleart/TitleArtHash.h"
#include "titleart/geometry/PathUtil.h"

#include "clipper2/clipper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    using Clipper2Lib::Path64;
    using Clipper2Lib::Paths64;
    using Clipper2Lib::PolyPath64;
    using Clipper2Lib::PolyTree64;

    // Clipper's decimal API scales by a power of two (precision 2 is a 1/128 grid), so the outlines
    // go through the integer API on an exact 0.01 grid
    constexpr double OUTLINE_SCALE = 1.0 / OUTLINE_GRID;

    Path64 toPath(const Contour& c) {
        Path64 out;
        out.reserve(c.size());
        for (const TitleVec2& p : c) out.emplace_back(std::llround(p.x * OUTLINE_SCALE), std::llround(p.y * OUTLINE_SCALE));
        return out;
    }

    double dist2(const TitleVec2& a, const TitleVec2& b) {
        const double dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    }

    // a Clipper output ring as a clean contour (near-collinear vertices dropped)
    Contour cleanRing(const Path64& poly, double eps) {
        const Path64 simple = Clipper2Lib::SimplifyPath(poly, OUTLINE_SIMPLIFY * eps * OUTLINE_SCALE, true);
        Contour c;
        c.reserve(simple.size());
        for (const auto& p : simple) c.push_back({(double)p.x / OUTLINE_SCALE, (double)p.y / OUTLINE_SCALE});
        return cleanContour(c, eps);
    }

    void collect(const PolyPath64& node, Shapes& out, double eps) {
        for (size_t i = 0; i < node.Count(); i++) {
            const PolyPath64* outerNode = node.Child(i);
            Contour outer = cleanRing(outerNode->Polygon(), eps);
            if (outer.size() >= 3) {
                if (signedArea(outer) < 0) std::reverse(outer.begin(), outer.end());
                ShapeContours shape;
                for (size_t k = 0; k < outerNode->Count(); k++) {
                    const PolyPath64* holeNode = outerNode->Child(k);
                    Contour hole = cleanRing(holeNode->Polygon(), eps);
                    if (hole.size() >= 3) {
                        if (signedArea(hole) > 0) std::reverse(hole.begin(), hole.end());
                        shape.holes.push_back(std::move(hole));
                    }
                    collect(*holeNode, out, eps);   // islands inside the hole
                }
                shape.outer = std::move(outer);
                out.push_back(std::move(shape));
            }
        }
    }

    Shapes unionPaths(const Paths64& paths, Clipper2Lib::FillRule rule, double eps) {
        Shapes out;
        if (paths.empty()) return out;
        PolyTree64 tree;
        Clipper2Lib::BooleanOp(Clipper2Lib::ClipType::Union, rule, paths, Paths64(), tree);
        collect(tree, out, eps);
        return out;
    }

    Paths64 shapesToPaths(const Shapes& shapes, bool withHoles) {
        Paths64 out;
        for (const ShapeContours& s : shapes) {
            out.push_back(toPath(s.outer));
            if (withHoles) {
                for (const Contour& h : s.holes) out.push_back(toPath(h));
            }
        }
        return out;
    }

    struct ChordFlattener {
        int segments;
        std::vector<Contour>* out;
        Contour current;

        void flush() {
            if (!current.empty()) out->push_back(std::move(current));
            current.clear();
        }
        void move(const TitleVec2& p) {
            flush();
            current.push_back(p);
        }
        void line(const TitleVec2&, const TitleVec2& p) { current.push_back(p); }
        void quad(const TitleVec2& from, const TitleVec2& c, const TitleVec2& p) {
            for (int s = 1; s <= segments; s++) current.push_back(geometry::quadPoint(from, c, p, (double)s / segments));
        }
        void cubic(const TitleVec2& from, const TitleVec2& c1, const TitleVec2& c2, const TitleVec2& p) {
            for (int s = 1; s <= segments; s++) current.push_back(geometry::cubicPoint(from, c1, c2, p, (double)s / segments));
        }
        void close(const TitleVec2&, const TitleVec2&, bool) { flush(); }
    };

}

double jasperengine::titleart::three_d::outlineEps(double size) {
    return std::max(1e-4, std::abs(size) * 1e-4);
}

double jasperengine::titleart::three_d::signedArea(const Contour& c) {
    double a = 0;
    const size_t n = c.size();
    for (size_t i = 0; i < n; i++) {
        const TitleVec2& p = c[i];
        const TitleVec2& q = c[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

double jasperengine::titleart::three_d::contourLength(const Contour& c) {
    double l = 0;
    for (size_t i = 0, n = c.size(); i < n; i++) l += std::sqrt(dist2(c[i], c[(i + 1) % n]));
    return l;
}

Contour jasperengine::titleart::three_d::cleanContour(const Contour& c, double eps) {
    Contour out;
    out.reserve(c.size());
    const double e2 = eps * eps;
    for (const TitleVec2& p : c) {
        if (out.empty() || dist2(out.back(), p) > e2) out.push_back(p);
    }
    while (out.size() > 1 && dist2(out.front(), out.back()) <= e2) out.pop_back();
    return out;
}

Shapes jasperengine::titleart::three_d::normalizeContours(const std::vector<Contour>& raw, bool evenOdd, double eps) {
    Paths64 paths;
    for (const Contour& c : raw) {
        if (c.size() >= 3) paths.push_back(toPath(c));
    }
    return unionPaths(paths, evenOdd ? Clipper2Lib::FillRule::EvenOdd : Clipper2Lib::FillRule::NonZero, eps);
}

Shapes jasperengine::titleart::three_d::unionShapes(const Shapes& shapes, double eps) {
    return unionPaths(shapesToPaths(shapes, true), Clipper2Lib::FillRule::NonZero, eps);
}

Shapes jasperengine::titleart::three_d::offsetShapes(const Shapes& shapes, double delta, bool roundJoin, bool fillCounters, double eps) {
    const Paths64 paths = shapesToPaths(shapes, !fillCounters);
    if (paths.empty()) return {};
    if (std::abs(delta) < 1e-6) return unionPaths(paths, Clipper2Lib::FillRule::NonZero, eps);
    const Paths64 grown = Clipper2Lib::InflatePaths(paths, delta * OUTLINE_SCALE,
                                                    roundJoin ? Clipper2Lib::JoinType::Round : Clipper2Lib::JoinType::Miter,
                                                    Clipper2Lib::EndType::Polygon, OUTLINE_MITER_LIMIT);
    return unionPaths(grown, Clipper2Lib::FillRule::NonZero, eps);
}

Shapes jasperengine::titleart::three_d::subtractContours(const Contour& outer, const std::vector<Contour>& holes, double eps) {
    auto ccw = [](const Contour& c) {
        if (signedArea(c) >= 0) return toPath(c);
        Contour r(c.rbegin(), c.rend());
        return toPath(r);
    };
    Paths64 clipIn;
    for (const Contour& h : holes) clipIn.push_back(ccw(h));
    // one grid step apart, so the difference never keeps slivers along shared edges
    const Paths64 subject = Clipper2Lib::InflatePaths(Paths64{ccw(outer)}, -1.0, Clipper2Lib::JoinType::Miter,
                                                      Clipper2Lib::EndType::Polygon, OUTLINE_MITER_LIMIT);
    const Paths64 clip = Clipper2Lib::InflatePaths(clipIn, 1.0, Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon,
                                                   OUTLINE_MITER_LIMIT);
    PolyTree64 tree;
    Clipper2Lib::BooleanOp(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero, subject, clip, tree);
    Shapes out;
    collect(tree, out, eps);
    return out;
}

TitleVec2 jasperengine::titleart::three_d::interiorPoint(const ShapeContours& shape) {
    const Contour& outer = shape.outer;
    size_t best = 0;
    double bestLen = -1;
    for (size_t i = 0, n = outer.size(); i < n; i++) {
        const double len = dist2(outer[i], outer[(i + 1) % n]);
        if (len > bestLen) {
            bestLen = len;
            best = i;
        }
    }
    const TitleVec2& a = outer[best];
    const TitleVec2& b = outer[(best + 1) % outer.size()];
    const double len = bestLen > 0 ? std::sqrt(bestLen) : 1.0;
    // inward normal = (-d.y, d.x)
    const double step = std::min(0.05, len * 0.25);
    return {(a.x + b.x) / 2 - ((b.y - a.y) / len) * step, (a.y + b.y) / 2 + ((b.x - a.x) / len) * step};
}

int jasperengine::titleart::three_d::windingNumber(const std::vector<Contour>& contours, double x, double y) {
    int w = 0;
    for (const Contour& c : contours) {
        for (size_t i = 0, n = c.size(); i < n; i++) {
            const TitleVec2& a = c[i];
            const TitleVec2& b = c[(i + 1) % n];
            const double cross = (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
            if (a.y <= y) {
                if (b.y > y && cross > 0) w++;
            } else if (b.y <= y && cross < 0) {
                w--;
            }
        }
    }
    return w;
}

bool jasperengine::titleart::three_d::shapeContains(const ShapeContours& shape, double x, double y) {
    int w = windingNumber({shape.outer}, x, y);
    if (w == 0) return false;
    w += windingNumber(shape.holes, x, y);
    return w != 0;
}

std::vector<int> jasperengine::titleart::three_d::shapeOwners(const Shapes& shapes, const std::vector<std::vector<Contour>>& groups) {
    std::vector<int> out;
    out.reserve(shapes.size());
    for (const ShapeContours& shape : shapes) {
        const TitleVec2 p = interiorPoint(shape);
        int owner = -1;
        for (int g = (int)groups.size() - 1; g >= 0; g--) {
            if (windingNumber(groups[g], p.x, p.y) != 0) {
                owner = g;
                break;
            }
        }
        out.push_back(owner);
    }
    return out;
}

TitleRect jasperengine::titleart::three_d::shapesBounds(const Shapes& shapes) {
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const ShapeContours& s : shapes) {
        for (const TitleVec2& p : s.outer) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    }
    if (x0 > x1) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

std::vector<Contour> jasperengine::titleart::three_d::flattenContours(const geometry::Path& path, int curveSegments) {
    std::vector<Contour> out;
    ChordFlattener f{std::max(1, curveSegments), &out, {}};
    geometry::walkPath(path, f);
    f.flush();
    return out;
}

geometry::Path jasperengine::titleart::three_d::shapesToPath(const Shapes& shapes) {
    geometry::Path out;
    auto add = [&](const Contour& c) {
        for (size_t i = 0; i < c.size(); i++) {
            if (i == 0) out.moveTo(c[i].x, c[i].y);
            else out.lineTo(c[i].x, c[i].y);
        }
        out.close();
    };
    for (const ShapeContours& s : shapes) {
        add(s.outer);
        for (const Contour& h : s.holes) add(h);
    }
    return out;
}

uint64_t jasperengine::titleart::three_d::hashShapes(const Shapes& shapes) {
    uint64_t h = fnv1a64("shapes");
    auto add = [&](const Contour& c) {
        h = hashCombine(h, c.size());
        if (!c.empty()) h = fnv1a64(std::string_view(reinterpret_cast<const char*>(c.data()), c.size() * sizeof(TitleVec2)), h);
    };
    for (const ShapeContours& s : shapes) {
        add(s.outer);
        h = hashCombine(h, s.holes.size());
        for (const Contour& c : s.holes) add(c);
    }
    return h;
}
