// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The mask recipe (fill + stroke of 2 x grow with the rim's join and miter limit 4, outer
// contours for fill_counters, the flood from the border, SVG strokes widened by grow) is ported
// from Pachislop src/engine/layer2d/Title2DRenderer.ts (updateMask, floodFillCounters) and
// outline2d.ts (fillGroup) by SafEight, used with the author's permission; Canvas2D is replaced
// by ThorVG's software rasterizer.

#include "TitleGeometry.h"

#include "titleart/geometry/Offset.h"
#include "titleart/geometry/PathUtil.h"

#include "lunasvg.h"
#include <thorvg.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr double MITER_LIMIT = 4.0;

    // ThorVG's software engine: initialised once for the process (its own counter keeps it alive
    // beside the HTML UI's ThorVGRuntime), no worker threads, so the calling thread rasterizes
    // and masks are deterministic. Its memory pool is shared: one rasterization at a time.
    std::mutex& thorvgMutex() {
        static std::mutex mutex;
        return mutex;
    }

    bool thorvgReady() {
        static const bool ready = tvg::Initializer::init(0) == tvg::Result::Success;
        return ready;
    }

    // ---------- fill groups ----------

    // a contour as a polyline for winding tests (curves in 6 chords, Pachislop's)
    std::vector<TitleVec2> windingPolyline(const Path& contour) {
        struct Flat {
            std::vector<TitleVec2> pts;
            void move(const TitleVec2& p) { pts.push_back(p); }
            void line(const TitleVec2&, const TitleVec2& p) { pts.push_back(p); }
            void quad(const TitleVec2& a, const TitleVec2& c, const TitleVec2& p) {
                for (int i = 1; i <= 6; i++) pts.push_back(quadPoint(a, c, p, i / 6.0));
            }
            void cubic(const TitleVec2& a, const TitleVec2& c1, const TitleVec2& c2, const TitleVec2& p) {
                for (int i = 1; i <= 6; i++) pts.push_back(cubicPoint(a, c1, c2, p, i / 6.0));
            }
            void close(const TitleVec2&, const TitleVec2&, bool) {}
        } flat;
        walkPath(contour, flat);
        return flat.pts;
    }

    double polylineArea(const std::vector<TitleVec2>& p) {
        double a = 0;
        for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) a += p[j].x * p[i].y - p[i].x * p[j].y;
        return a / 2;
    }

    int winding(double x, double y, const std::vector<TitleVec2>& p) {
        int w = 0;
        for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
            const double x0 = p[j].x, y0 = p[j].y, x1 = p[i].x, y1 = p[i].y;
            if (y0 <= y) {
                if (y1 > y && (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) > 0) w++;
            } else if (y1 <= y && (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) < 0) {
                w--;
            }
        }
        return w;
    }

    // the outer contours of a fill group: those with a point outside the fill of all the others
    // (under the group's rule), each turned counter-clockwise, so a non-zero fill of them is the
    // shape with every counter filled; overlapping strokes (common in CJK fonts) stay outer
    Path outerContours(const Path& group) {
        std::vector<Path> contours = splitContours(group);
        contours.erase(std::remove_if(contours.begin(), contours.end(),
                                      [](const Path& c) {
                                          return std::none_of(c.verbList().begin(), c.verbList().end(), [](PathVerb v) {
                                              return v != PathVerb::Move && v != PathVerb::Close;
                                          });
                                      }),
                       contours.end());
        std::vector<std::vector<TitleVec2>> polys;
        for (const Path& c : contours) polys.push_back(windingPolyline(c));
        Path out;
        for (size_t i = 0; i < contours.size(); i++) {
            const std::vector<TitleVec2>& p = polys[i];
            if (p.size() < 2) continue;
            const size_t step = std::max<size_t>(1, p.size() / 12);
            bool isOuter = false;
            for (size_t k = 0; k < p.size() && !isOuter; k += step) {
                int w = 0;
                for (size_t j = 0; j < polys.size(); j++) {
                    if (j != i && polys[j].size() >= 2) w += winding(p[k].x, p[k].y, polys[j]);
                }
                const bool inside = group.evenOdd ? (w & 1) != 0 : w != 0;
                if (!inside) isOuter = true;
            }
            if (!isOuter) continue;
            out.append(polylineArea(p) < 0 ? reverseContour(contours[i]) : contours[i]);
        }
        return out;
    }

    // ---------- ThorVG ----------

    struct PixelMap {
        double scale;
        double ox, oy;      // the layer point at pixel (0, 0)'s corner, minus the element offset
        float x(double lx) const { return (float)((lx - ox) * scale); }
        float y(double ly) const { return (float)((oy - ly) * scale); }
    };

    // ThorVG draws a lone axis-aligned rectangle (one contour of five commands) by its rounded
    // bounding box, without antialiasing (its fast track); a mask needs the exact coverage the
    // distance-field seeds read, so a one-contour polygon of up to four edges gets a collinear
    // midpoint on its first edge, which takes it off that track
    bool mayFastTrack(const Path& path) {
        const std::vector<PathVerb>& v = path.verbList();
        if (v.size() < 2 || v.size() > 6 || v[0] != PathVerb::Move || v[1] != PathVerb::Line) return false;
        for (size_t i = 2; i < v.size(); i++) {
            if (v[i] != PathVerb::Line && !(v[i] == PathVerb::Close && i + 1 == v.size())) return false;
        }
        return true;
    }

    void appendPath(tvg::Shape* shape, const Path& path, const PixelMap& m) {
        if (mayFastTrack(path)) {
            const std::vector<TitleVec2>& p = path.pointList();
            shape->moveTo(m.x(p[0].x), m.y(p[0].y));
            shape->lineTo(m.x((p[0].x + p[1].x) / 2), m.y((p[0].y + p[1].y) / 2));
            for (size_t i = 1; i < p.size(); i++) shape->lineTo(m.x(p[i].x), m.y(p[i].y));
            if (path.verbList().back() == PathVerb::Close) shape->close();
            return;
        }
        struct Appender {
            tvg::Shape* shape;
            const PixelMap& m;
            void move(const TitleVec2& p) { shape->moveTo(m.x(p.x), m.y(p.y)); }
            void line(const TitleVec2&, const TitleVec2& p) { shape->lineTo(m.x(p.x), m.y(p.y)); }
            // ThorVG has cubics only: the exact degree elevation of the quadratic
            void quad(const TitleVec2& a, const TitleVec2& c, const TitleVec2& p) {
                const TitleVec2 c1{a.x + 2.0 / 3.0 * (c.x - a.x), a.y + 2.0 / 3.0 * (c.y - a.y)};
                const TitleVec2 c2{p.x + 2.0 / 3.0 * (c.x - p.x), p.y + 2.0 / 3.0 * (c.y - p.y)};
                shape->cubicTo(m.x(c1.x), m.y(c1.y), m.x(c2.x), m.y(c2.y), m.x(p.x), m.y(p.y));
            }
            void cubic(const TitleVec2&, const TitleVec2& c1, const TitleVec2& c2, const TitleVec2& p) {
                shape->cubicTo(m.x(c1.x), m.y(c1.y), m.x(c2.x), m.y(c2.y), m.x(p.x), m.y(p.y));
            }
            void close(const TitleVec2&, const TitleVec2&, bool closed) {
                if (closed) shape->close();
            }
        } appender{shape, m};
        walkPath(path, appender);
    }

    tvg::StrokeJoin tvgJoin(StrokeJoin join) {
        return join == StrokeJoin::Round ? tvg::StrokeJoin::Round : join == StrokeJoin::Bevel ? tvg::StrokeJoin::Bevel : tvg::StrokeJoin::Miter;
    }

    tvg::StrokeCap tvgCap(StrokeCap cap) {
        return cap == StrokeCap::Round ? tvg::StrokeCap::Round : cap == StrokeCap::Square ? tvg::StrokeCap::Square : tvg::StrokeCap::Butt;
    }

    // ---------- pictures ----------

    // premultiplied RGBA of an image at a texel, transparent outside
    void texel(const TitleImageData& img, int x, int y, double out[4]) {
        if (x < 0 || y < 0 || x >= img.width || y >= img.height) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        }
        const uint8_t* p = &img.rgba[((size_t)y * img.width + x) * 4];
        const double a = p[3] / 255.0;
        out[0] = p[0] / 255.0 * a;
        out[1] = p[1] / 255.0 * a;
        out[2] = p[2] / 255.0 * a;
        out[3] = a;
    }

    void bilinear(const TitleImageData& img, double u, double v, double out[4]) {
        // texel centres at integer + 0.5
        const double fx = u - 0.5, fy = v - 0.5;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const double tx = fx - x0, ty = fy - y0;
        double a[4], b[4], c[4], d[4];
        texel(img, x0, y0, a);
        texel(img, x0 + 1, y0, b);
        texel(img, x0, y0 + 1, c);
        texel(img, x0 + 1, y0 + 1, d);
        for (int k = 0; k < 4; k++) out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
    }

    TitleImageData resampleImage(const TitleImageData& img, const TitleRect& rect, bool nearest, const MaskFrame& frame, const TitleVec2& offset) {
        TitleImageData out;
        out.width = frame.width;
        out.height = frame.height;
        out.rgba.assign((size_t)frame.width * frame.height * 4, 0);
        if (img.width <= 0 || img.height <= 0 || rect.width <= 0 || rect.height <= 0 || frame.scale <= 0 ||
            img.rgba.size() < (size_t)img.width * img.height * 4) {
            return out;
        }
        // image texels per frame pixel along each axis
        const double ku = img.width / (rect.width * frame.scale), kv = img.height / (rect.height * frame.scale);
        const int taps = nearest ? 1 : std::clamp((int)std::ceil(std::max(ku, kv)), 1, 8);
        const double top = rect.y + rect.height;
        for (int py = 0; py < frame.height; py++) {
            for (int px = 0; px < frame.width; px++) {
                double acc[4] = {0, 0, 0, 0};
                for (int sy = 0; sy < taps; sy++) {
                    for (int sx = 0; sx < taps; sx++) {
                        const double lx = frame.origin.x + (px + (sx + 0.5) / taps) / frame.scale - offset.x;
                        const double ly = frame.origin.y - (py + (sy + 0.5) / taps) / frame.scale - offset.y;
                        const double u = (lx - rect.x) / rect.width * img.width;
                        const double v = (top - ly) / rect.height * img.height;
                        double s[4];
                        if (nearest) {
                            texel(img, (int)std::floor(u), (int)std::floor(v), s);
                        } else {
                            bilinear(img, u, v, s);
                        }
                        for (int k = 0; k < 4; k++) acc[k] += s[k];
                    }
                }
                uint8_t* o = &out.rgba[((size_t)py * frame.width + px) * 4];
                const double n = (double)taps * taps;
                const double a = acc[3] / n;
                if (a <= 0) continue;
                for (int k = 0; k < 3; k++) o[k] = (uint8_t)std::lround(std::clamp(acc[k] / n / a, 0.0, 1.0) * 255);
                o[3] = (uint8_t)std::lround(std::clamp(a, 0.0, 1.0) * 255);
            }
        }
        return out;
    }

    TitleImageData rasterizeArtwork(const std::string& svg, const TitleRect& rect, const MaskFrame& frame, const TitleVec2& offset) {
        TitleImageData out;
        out.width = frame.width;
        out.height = frame.height;
        out.rgba.assign((size_t)frame.width * frame.height * 4, 0);
        if (frame.width <= 0 || frame.height <= 0) return out;
        std::unique_ptr<lunasvg::Document> doc = lunasvg::Document::loadFromData(svg);
        if (!doc || doc->width() <= 0 || doc->height() <= 0 || rect.width <= 0) return out;
        // the document's viewport (its own px) onto `rect`, then into frame pixels
        const double k = rect.width / doc->width() * frame.scale;
        const double e = (rect.x + offset.x - frame.origin.x) * frame.scale;
        const double f = (frame.origin.y - (rect.y + rect.height + offset.y)) * frame.scale;
        lunasvg::Bitmap bitmap(frame.width, frame.height);
        bitmap.clear(0x00000000);
        doc->render(bitmap, lunasvg::Matrix((float)k, 0, 0, (float)k, (float)e, (float)f));
        bitmap.convertToRGBA();
        const uint8_t* data = bitmap.data();
        for (int y = 0; y < frame.height; y++) {
            std::copy(data + (size_t)y * bitmap.stride(), data + (size_t)y * bitmap.stride() + (size_t)frame.width * 4,
                      out.rgba.begin() + (size_t)y * frame.width * 4);
        }
        return out;
    }

}

void jasperengine::titleart::geometry::fillCounters(CoverageMask& mask) {
    const int w = mask.width, h = mask.height;
    if (w <= 0 || h <= 0 || mask.pixels.size() < (size_t)w * h) return;
    std::vector<uint8_t> outside((size_t)w * h, 0);
    std::vector<size_t> stack;
    const auto push = [&](size_t i) {
        if (!outside[i] && mask.pixels[i] < 128) {
            outside[i] = 1;
            stack.push_back(i);
        }
    };
    for (int x = 0; x < w; x++) {
        push((size_t)x);
        push((size_t)(h - 1) * w + x);
    }
    for (int y = 0; y < h; y++) {
        push((size_t)y * w);
        push((size_t)y * w + w - 1);
    }
    while (!stack.empty()) {
        const size_t i = stack.back();
        stack.pop_back();
        const size_t x = i % w;
        if (x > 0) push(i - 1);
        if (x + 1 < (size_t)w) push(i + 1);
        if (i >= (size_t)w) push(i - w);
        if (i < (size_t)w * (h - 1)) push(i + w);
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (!outside[i]) mask.pixels[i] = 255;
    }
}

TitleRect jasperengine::titleart::geometry::elementBounds(const TitleElement& e) {
    double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    const auto add = [&](const TitleRect& r) {
        if (r.width <= 0 && r.height <= 0) return;
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.width);
        y1 = std::max(y1, r.y + r.height);
    };
    bool pathed = false;
    for (const Path& p : e.outlines) {
        if (!p.empty()) pathed = true;
        add(p.controlBounds());
    }
    double strokePad = 0;
    for (const StrokedPath& s : e.strokes) {
        TitleRect b = s.path.controlBounds();
        b = {b.x - s.width / 2, b.y - s.width / 2, b.width + s.width, b.height + s.width};
        add(b);
        pathed = true;
        strokePad = std::max(strokePad, s.width / 2 * (MITER_LIMIT - 1));
    }
    if (!e.source.empty()) add(e.source.rect);
    if (x1 < x0) return {};
    const double pad = (e.join == StrokeJoin::Miter && pathed ? e.grow * MITER_LIMIT : std::max(e.grow, e.close)) + strokePad;
    return {x0 - pad + e.offset.x, y0 - pad + e.offset.y, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad};
}

namespace {

    struct SpikeGroup {
        Polygons polygons;
        bool evenOdd = false;
    };

    bool insideGroup(const SpikeGroup& g, const TitleVec2& p) {
        int winding = 0, crossings = 0;
        for (const Polygon& poly : g.polygons) {
            for (size_t i = 0; i < poly.size(); i++) {
                const TitleVec2& a = poly[i];
                const TitleVec2& b = poly[(i + 1) % poly.size()];
                const double cross = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
                if (a.y <= p.y && b.y > p.y && cross > 0) {
                    winding++;
                    crossings++;
                }
                if (a.y > p.y && b.y <= p.y && cross < 0) {
                    winding--;
                    crossings++;
                }
            }
        }
        return g.evenOdd ? (crossings & 1) != 0 : winding != 0;
    }

    double segmentDistance(const TitleVec2& p, const TitleVec2& a, const TitleVec2& b) {
        const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        const double t = len2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
        return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
    }

}

MitreSpike jasperengine::titleart::geometry::mitreSpike(const TitleElement& element) {
    MitreSpike worst;
    if (element.kind != ElementKind::Rim || !(element.grow > 0) || element.join != StrokeJoin::Miter || !element.source.empty()) {
        return worst;
    }
    // the outlines as the mask strokes them (fill_counters: the outer contours), flattened finely
    // enough that a curve's chords turn by little (their mitres stay at the band)
    TitleRect box;
    for (const Path& p : element.outlines) {
        const TitleRect b = p.controlBounds();
        box = box.empty() ? b : TitleRect{std::min(box.x, b.x), std::min(box.y, b.y),
                                          std::max(box.x + box.width, b.x + b.width) - std::min(box.x, b.x),
                                          std::max(box.y + box.height, b.y + b.height) - std::min(box.y, b.y)};
    }
    const double tol = std::max(1e-4, std::max(box.width, box.height) * 0.0005);
    std::vector<SpikeGroup> groups;
    for (const Path& group : element.outlines) {
        SpikeGroup g;
        g.polygons = flattenPath(element.fillCounters ? outerContours(group) : group, tol);
        g.evenOdd = !element.fillCounters && group.evenOdd;
        for (Polygon& poly : g.polygons) {
            // repeated points (a contour closed onto its start) have no direction
            Polygon clean;
            for (const TitleVec2& v : poly) {
                if (clean.empty() || std::hypot(v.x - clean.back().x, v.y - clean.back().y) > tol * 0.01) clean.push_back(v);
            }
            while (clean.size() > 1 && std::hypot(clean.front().x - clean.back().x, clean.front().y - clean.back().y) <= tol * 0.01) clean.pop_back();
            poly = std::move(clean);
        }
        if (!g.polygons.empty()) groups.push_back(std::move(g));
    }
    auto inside = [&](const TitleVec2& p) {
        for (const SpikeGroup& g : groups) {
            if (insideGroup(g, p)) return true;
        }
        return false;
    };
    auto outlineDistance = [&](const TitleVec2& p) {
        double d = std::numeric_limits<double>::infinity();
        for (const SpikeGroup& g : groups) {
            for (const Polygon& poly : g.polygons) {
                for (size_t i = 0; i < poly.size(); i++) d = std::min(d, segmentDistance(p, poly[i], poly[(i + 1) % poly.size()]));
            }
        }
        return d;
    };
    const double grow = element.grow;
    for (const SpikeGroup& g : groups) {
        for (const Polygon& poly : g.polygons) {
            const size_t n = poly.size();
            if (n < 3) continue;
            for (size_t i = 0; i < n; i++) {
                const TitleVec2& prev = poly[(i + n - 1) % n];
                const TitleVec2& v = poly[i];
                const TitleVec2& next = poly[(i + 1) % n];
                const double l1 = std::hypot(v.x - prev.x, v.y - prev.y), l2 = std::hypot(next.x - v.x, next.y - v.y);
                if (!(l1 > 0) || !(l2 > 0)) continue;
                const TitleVec2 d1{(v.x - prev.x) / l1, (v.y - prev.y) / l1}, d2{(next.x - v.x) / l2, (next.y - v.y) / l2};
                const double turn = d1.x * d2.y - d1.y * d2.x;
                const double cosHalf = std::sqrt(std::max(0.0, (1.0 + d1.x * d2.x + d1.y * d2.y) / 2.0));
                // a mitre within the limit (beyond it the join is a bevel, which stays at the band)
                if (std::fabs(turn) < 1e-9 || cosHalf <= 0.0 || 1.0 / cosHalf > element.miterLimit) continue;
                // the outside of the turn: right of a left turn, left of a right one
                const double s = turn > 0 ? -1.0 : 1.0;
                TitleVec2 dir{s * (-d1.y - d2.y), s * (d1.x + d2.x)};
                const double dl = std::hypot(dir.x, dir.y);
                if (!(dl > 0)) continue;
                dir = {dir.x / dl, dir.y / dl};
                // a convex vertex mitres away from the outline: the regular corner of the band
                if (!inside({v.x + dir.x * tol * 4, v.y + dir.y * tol * 4})) continue;
                const TitleVec2 tip{v.x + dir.x * grow / cosHalf, v.y + dir.y * grow / cosHalf};
                if (inside(tip)) continue;
                const double beyond = outlineDistance(tip) - grow;
                if (beyond > worst.length) worst = {beyond, tip};
            }
        }
    }
    return worst;
}

TitleImageData jasperengine::titleart::geometry::rasterizeSource(const ElementSource& source, const MaskFrame& frame) {
    if (source.image) return resampleImage(*source.image, source.rect, source.nearest, frame, {});
    if (source.svg) return rasterizeArtwork(*source.svg, source.rect, frame, {});
    TitleImageData out;
    out.width = frame.width;
    out.height = frame.height;
    out.rgba.assign((size_t)std::max(0, frame.width) * std::max(0, frame.height) * 4, 0);
    return out;
}

CoverageMask jasperengine::titleart::geometry::rasterizeElement(const TitleElement& element, const MaskFrame& frame) {
    CoverageMask mask;
    mask.width = std::max(0, frame.width);
    mask.height = std::max(0, frame.height);
    mask.pixels.assign((size_t)mask.width * mask.height, 0);
    if (mask.width == 0 || mask.height == 0 || !(frame.scale > 0)) return mask;

    if (!element.source.empty()) {
        const TitleImageData pic = element.source.image
                                       ? resampleImage(*element.source.image, element.source.rect, element.source.nearest, frame, element.offset)
                                       : rasterizeArtwork(*element.source.svg, element.source.rect, frame, element.offset);
        for (size_t i = 0; i < mask.pixels.size(); i++) mask.pixels[i] = pic.rgba[i * 4 + 3];
        if (element.fillCounters) fillCounters(mask);
        return mask;
    }

    std::lock_guard<std::mutex> lock(thorvgMutex());
    if (!thorvgReady()) return mask;
    std::vector<uint32_t> buffer((size_t)mask.width * mask.height, 0u);
    std::unique_ptr<tvg::SwCanvas> canvas(tvg::SwCanvas::gen());
    if (!canvas || canvas->target(buffer.data(), (uint32_t)mask.width, (uint32_t)mask.width, (uint32_t)mask.height,
                                  tvg::ColorSpace::ABGR8888) != tvg::Result::Success) {
        return mask;
    }
    const PixelMap m{frame.scale, frame.origin.x - element.offset.x, frame.origin.y - element.offset.y};
    const bool grown = element.grow > 0;
    for (const Path& group : element.outlines) {
        const Path fill = element.fillCounters ? outerContours(group) : group;
        if (fill.empty()) continue;
        tvg::Shape* shape = tvg::Shape::gen();
        appendPath(shape, fill, m);
        shape->fill(255, 255, 255, 255);
        shape->fillRule(!element.fillCounters && group.evenOdd ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero);
        if (grown) {
            shape->strokeWidth((float)(element.grow * 2 * frame.scale));
            shape->strokeJoin(tvgJoin(element.join));
            shape->strokeMiterlimit((float)element.miterLimit);
            shape->strokeFill(255, 255, 255, 255);
        }
        canvas->add(shape);
    }
    for (const StrokedPath& s : element.strokes) {
        tvg::Shape* shape = tvg::Shape::gen();
        appendPath(shape, s.path, m);
        shape->strokeWidth((float)((s.width + element.grow * 2) * frame.scale));
        shape->strokeJoin(tvgJoin(grown ? element.join : s.join));
        shape->strokeMiterlimit((float)(grown ? element.miterLimit : s.miterLimit));
        shape->strokeCap(tvgCap(s.cap));
        shape->strokeFill(255, 255, 255, 255);
        canvas->add(shape);
    }
    canvas->draw(true);
    canvas->sync();
    for (size_t i = 0; i < buffer.size(); i++) mask.pixels[i] = (uint8_t)(buffer[i] >> 24);
    // gaps between glyphs that the grown outline closes off become solid plate too
    if (element.fillCounters && grown) fillCounters(mask);
    return mask;
}
