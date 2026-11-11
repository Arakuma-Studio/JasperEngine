// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Path.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr double PI = 3.14159265358979323846;

    struct Extent {
        double x0 = std::numeric_limits<double>::infinity(), y0 = x0;
        double x1 = -std::numeric_limits<double>::infinity(), y1 = x1;
        void add(const TitleVec2& p) {
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
        TitleRect rect() const {
            if (x1 < x0) return {};
            return {x0, y0, x1 - x0, y1 - y0};
        }
    };

    TitleVec2 lerp(const TitleVec2& a, const TitleVec2& b, double t) {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }

    TitleVec2 quadAt(const TitleVec2& p0, const TitleVec2& p1, const TitleVec2& p2, double t) {
        return lerp(lerp(p0, p1, t), lerp(p1, p2, t), t);
    }

    TitleVec2 cubicAt(const TitleVec2& p0, const TitleVec2& p1, const TitleVec2& p2, const TitleVec2& p3, double t) {
        const TitleVec2 a = lerp(p0, p1, t), b = lerp(p1, p2, t), c = lerp(p2, p3, t);
        return lerp(lerp(a, b, t), lerp(b, c, t), t);
    }

    // the roots in (0, 1) of the derivative of a 1D quadratic Bezier
    void quadExtrema(double p0, double p1, double p2, std::vector<double>& out) {
        const double den = p0 - 2 * p1 + p2;
        if (std::abs(den) > 1e-12) {
            const double t = (p0 - p1) / den;
            if (t > 0 && t < 1) out.push_back(t);
        }
    }

    // the roots in (0, 1) of the derivative of a 1D cubic Bezier
    void cubicExtrema(double p0, double p1, double p2, double p3, std::vector<double>& out) {
        const double a = -p0 + 3 * p1 - 3 * p2 + p3;
        const double b = 2 * (p0 - 2 * p1 + p2);
        const double c = p1 - p0;
        if (std::abs(a) < 1e-12) {
            if (std::abs(b) > 1e-12) {
                const double t = -c / b;
                if (t > 0 && t < 1) out.push_back(t);
            }
            return;
        }
        const double disc = b * b - 4 * a * c;
        if (disc < 0) return;
        const double s = std::sqrt(disc);
        for (double t : {(-b + s) / (2 * a), (-b - s) / (2 * a)}) {
            if (t > 0 && t < 1) out.push_back(t);
        }
    }

    // segments for a curve whose control polygon spans `extent`, within `tolerance`
    int segmentsFor(double extent, double tolerance) {
        if (tolerance <= 0) return 16;
        return std::clamp((int)std::ceil(std::sqrt(extent / tolerance)), 1, 256);
    }

}

Affine Affine::after(const Affine& o) const {
    Affine r;
    r.a = a * o.a + c * o.b;
    r.b = b * o.a + d * o.b;
    r.c = a * o.c + c * o.d;
    r.d = b * o.c + d * o.d;
    r.e = a * o.e + c * o.f + e;
    r.f = b * o.e + d * o.f + f;
    return r;
}

Affine Affine::translate(double x, double y) {
    Affine m;
    m.e = x;
    m.f = y;
    return m;
}

Affine Affine::scale(double sx, double sy) {
    Affine m;
    m.a = sx;
    m.d = sy;
    return m;
}

Affine Affine::rotateDegrees(double degrees) {
    const double r = degrees * PI / 180.0;
    Affine m;
    m.a = std::cos(r);
    m.b = std::sin(r);
    m.c = -m.b;
    m.d = m.a;
    return m;
}

void Path::moveTo(double x, double y) {
    verbs.push_back(PathVerb::Move);
    points.push_back({x, y});
}

void Path::lineTo(double x, double y) {
    verbs.push_back(PathVerb::Line);
    points.push_back({x, y});
}

void Path::quadTo(double cx, double cy, double x, double y) {
    verbs.push_back(PathVerb::Quad);
    points.push_back({cx, cy});
    points.push_back({x, y});
}

void Path::cubicTo(double c1x, double c1y, double c2x, double c2y, double x, double y) {
    verbs.push_back(PathVerb::Cubic);
    points.push_back({c1x, c1y});
    points.push_back({c2x, c2y});
    points.push_back({x, y});
}

void Path::close() {
    if (!verbs.empty() && verbs.back() != PathVerb::Close) verbs.push_back(PathVerb::Close);
}

void Path::append(const Path& other) {
    verbs.insert(verbs.end(), other.verbs.begin(), other.verbs.end());
    points.insert(points.end(), other.points.begin(), other.points.end());
}

void Path::clear() {
    verbs.clear();
    points.clear();
}

size_t Path::contourCount() const {
    return (size_t)std::count(verbs.begin(), verbs.end(), PathVerb::Move);
}

TitleRect Path::controlBounds() const {
    Extent e;
    for (const TitleVec2& p : points) e.add(p);
    return e.rect();
}

TitleRect Path::bounds() const {
    Extent e;
    size_t i = 0;
    TitleVec2 last;
    for (PathVerb verb : verbs) {
        switch (verb) {
            case PathVerb::Move:
            case PathVerb::Line:
                last = points[i++];
                e.add(last);
                break;
            case PathVerb::Quad: {
                const TitleVec2 p1 = points[i], p2 = points[i + 1];
                std::vector<double> ts;
                quadExtrema(last.x, p1.x, p2.x, ts);
                quadExtrema(last.y, p1.y, p2.y, ts);
                for (double t : ts) e.add(quadAt(last, p1, p2, t));
                e.add(p2);
                last = p2;
                i += 2;
                break;
            }
            case PathVerb::Cubic: {
                const TitleVec2 p1 = points[i], p2 = points[i + 1], p3 = points[i + 2];
                std::vector<double> ts;
                cubicExtrema(last.x, p1.x, p2.x, p3.x, ts);
                cubicExtrema(last.y, p1.y, p2.y, p3.y, ts);
                for (double t : ts) e.add(cubicAt(last, p1, p2, p3, t));
                e.add(p3);
                last = p3;
                i += 3;
                break;
            }
            case PathVerb::Close:
                break;
        }
    }
    return e.rect();
}

void Path::transform(const Affine& m) {
    for (TitleVec2& p : points) p = m.apply(p);
}

std::vector<std::vector<TitleVec2>> Path::flatten(double tolerance) const {
    std::vector<std::vector<TitleVec2>> out;
    size_t i = 0;
    TitleVec2 last;
    for (PathVerb verb : verbs) {
        switch (verb) {
            case PathVerb::Move:
                last = points[i++];
                out.push_back({last});
                break;
            case PathVerb::Line:
                last = points[i++];
                if (out.empty()) out.push_back({});
                out.back().push_back(last);
                break;
            case PathVerb::Quad: {
                const TitleVec2 p1 = points[i], p2 = points[i + 1];
                if (out.empty()) out.push_back({last});
                const double extent = std::hypot(p1.x - last.x, p1.y - last.y) + std::hypot(p2.x - p1.x, p2.y - p1.y);
                const int n = segmentsFor(extent, tolerance);
                for (int k = 1; k <= n; k++) out.back().push_back(quadAt(last, p1, p2, (double)k / n));
                last = p2;
                i += 2;
                break;
            }
            case PathVerb::Cubic: {
                const TitleVec2 p1 = points[i], p2 = points[i + 1], p3 = points[i + 2];
                if (out.empty()) out.push_back({last});
                const double extent = std::hypot(p1.x - last.x, p1.y - last.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) +
                                      std::hypot(p3.x - p2.x, p3.y - p2.y);
                const int n = segmentsFor(extent, tolerance);
                for (int k = 1; k <= n; k++) out.back().push_back(cubicAt(last, p1, p2, p3, (double)k / n));
                last = p3;
                i += 3;
                break;
            }
            case PathVerb::Close:
                break;
        }
    }
    return out;
}

double Path::signedArea(double tolerance) const {
    double area = 0;
    for (const std::vector<TitleVec2>& contour : flatten(tolerance)) {
        for (size_t k = 0; k < contour.size(); k++) {
            const TitleVec2& a = contour[k];
            const TitleVec2& b = contour[(k + 1) % contour.size()];
            area += a.x * b.y - b.x * a.y;
        }
    }
    return area * 0.5;
}
