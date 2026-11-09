// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Parts ported from Pixelorama (https://github.com/Orama-Interactive/Pixelorama):
//
//   MIT License
//
//   Copyright (c) 2019-present Orama Interactive and contributors
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
//   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "DrawPrimitives.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <unordered_set>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        int64_t pointKey(IntPoint p) {
            return (static_cast<int64_t>(p.x) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(p.y));
        }

        // the leftmost and rightmost x of the shape on each row of a border
        std::map<int, std::pair<int, int>> rowSpans(const std::vector<IntPoint>& border) {
            std::map<int, std::pair<int, int>> rows;
            for (const IntPoint& p : border) {
                auto it = rows.find(p.y);
                if (it == rows.end()) rows[p.y] = {p.x, p.x};
                else {
                    it->second.first = std::min(it->second.first, p.x);
                    it->second.second = std::max(it->second.second, p.x);
                }
            }
            return rows;
        }

        void emitSpan(std::vector<IntPoint>& out, int y, int from, int to, const IntRect& clip) {
            if (y < clip.y || y >= clip.y + clip.h) return;
            const int a = std::max(from, clip.x), b = std::min(to, clip.x + clip.w - 1);
            for (int x = a; x <= b; x++) out.push_back({x, y});
        }

        // the rows of the rounded rect, as [start, end] per row (DrawingAlgos.gd get_rounded_rect_points_filled)
        std::pair<int, int> roundedSpan(const IntRect& rect, int radius, int y) {
            const int left = rect.x, right = rect.x + rect.w - 1, top = rect.y, bottom = rect.y + rect.h - 1;
            int d = 0;
            if (radius > 0 && y < top + radius) d = (top + radius) - y;
            else if (radius > 0 && y > bottom - radius) d = y - (bottom - radius);
            if (d == 0) return {left, right};
            const int dx = static_cast<int>(std::floor(std::sqrt(static_cast<double>(radius * radius - d * d))));
            return {left + radius - dx, right - radius + dx};
        }

    }

    void dedupe(std::vector<IntPoint>& points) {
        std::unordered_set<int64_t> seen;
        seen.reserve(points.size());
        size_t kept = 0;
        for (size_t i = 0; i < points.size(); i++) {
            if (seen.insert(pointKey(points[i])).second) points[kept++] = points[i];
        }
        points.resize(kept);
    }

    void bresenham(IntPoint a, IntPoint b, std::vector<IntPoint>& out) {
        int x = a.x, y = a.y;
        const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
        const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            out.push_back({x, y});
            if (x == b.x && y == b.y) break;
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y += sy;
            }
        }
    }

    std::vector<IntPoint> pixelPerfectFilter(std::span<const IntPoint> stroke) {
        std::vector<IntPoint> out;
        out.reserve(stroke.size());
        for (const IntPoint& p : stroke) {
            out.push_back(p);
            const size_t n = out.size();
            if (n < 3) continue;
            const IntPoint corner = out[n - 3], neighbour = out[n - 2];
            const int cx = p.x - corner.x, cy = p.y - corner.y;
            const int nx = p.x - neighbour.x, ny = p.y - neighbour.y;
            const bool diagonal = std::abs(cx) == 1 && std::abs(cy) == 1;
            const bool orthogonal = std::abs(nx) + std::abs(ny) == 1;
            if (diagonal && orthogonal) {
                out[n - 2] = p;
                out.pop_back();
            }
        }
        return out;
    }

    int64_t strokeLength(std::span<const IntPoint> points) {
        int64_t total = 0;
        for (size_t i = 1; i < points.size(); i++) {
            total += std::max(std::llabs(static_cast<int64_t>(points[i].x) - points[i - 1].x), std::llabs(static_cast<int64_t>(points[i].y) - points[i - 1].y)) + 1;
        }
        return total;
    }

    std::vector<IntPoint> polyline(std::span<const IntPoint> points, bool pixelPerfect) {
        std::vector<IntPoint> stroke;
        if (points.empty()) return stroke;
        if (points.size() == 1) {
            stroke.push_back(points[0]);
            return stroke;
        }
        std::vector<IntPoint> segment;
        for (size_t i = 1; i < points.size(); i++) {
            segment.clear();
            bresenham(points[i - 1], points[i], segment);
            // a joint is drawn once: each later segment starts after its first pixel
            const size_t from = i == 1 ? 0 : 1;
            stroke.insert(stroke.end(), segment.begin() + static_cast<std::ptrdiff_t>(from), segment.end());
        }
        return pixelPerfect ? pixelPerfectFilter(stroke) : stroke;
    }

    std::vector<IntPoint> ellipseBorder(const IntRect& rect, const IntRect& clip) {
        std::vector<IntPoint> out;
        if (rect.empty()) return out;
        // Zingl, "A Rasterizing Algorithm for Drawing Curves" (plotEllipseRect), as Pixelorama
        // writes it; 64-bit terms keep 4096-wide ellipses exact
        int64_t x0 = rect.x, x1 = rect.x + rect.w - 1, y0 = rect.y, y1 = rect.y + rect.h - 1;
        int64_t a = std::llabs(x1 - x0), b = std::llabs(y1 - y0), b1 = b & 1;
        int64_t dx = 4 * (1 - a) * b * b, dy = 4 * (b1 + 1) * a * a;
        int64_t err = dx + dy + b1 * a * a, e2 = 0;
        if (x0 > x1) {
            x0 = x1;
            x1 += a;
        }
        if (y0 > y1) y0 = y1;
        y0 += (b + 1) / 2;
        y1 = y0 - b1;
        a *= 8 * a;
        b1 = 8 * b * b;
        while (x0 <= x1) {
            out.push_back({static_cast<int>(x1), static_cast<int>(y0)});
            out.push_back({static_cast<int>(x0), static_cast<int>(y0)});
            out.push_back({static_cast<int>(x0), static_cast<int>(y1)});
            out.push_back({static_cast<int>(x1), static_cast<int>(y1)});
            e2 = 2 * err;
            if (e2 <= dy) {
                y0++;
                y1--;
                dy += a;
                err += dy;
            }
            if (e2 >= dx || 2 * err > dy) {
                x0++;
                x1--;
                dx += b1;
                err += dx;
            }
        }
        while (y0 - y1 < b) {
            out.push_back({static_cast<int>(x0 - 1), static_cast<int>(y0)});
            out.push_back({static_cast<int>(x1 + 1), static_cast<int>(y0)});
            out.push_back({static_cast<int>(x0 - 1), static_cast<int>(y1)});
            out.push_back({static_cast<int>(x1 + 1), static_cast<int>(y1)});
            y0++;
            y1--;
        }
        out.erase(std::remove_if(out.begin(), out.end(), [&](const IntPoint& p) { return !clip.contains(p.x, p.y); }), out.end());
        dedupe(out);
        return out;
    }

    std::vector<IntPoint> ellipseFilled(const IntRect& rect, const IntRect& clip) {
        std::vector<IntPoint> out;
        for (const auto& [y, span] : rowSpans(ellipseBorder(rect))) emitSpan(out, y, span.first, span.second, clip);
        return out;
    }

    std::vector<IntPoint> ellipseRing(const IntRect& rect, int thickness, const IntRect& clip) {
        if (thickness <= 1) return ellipseBorder(rect, clip);
        const IntRect inner{rect.x + thickness, rect.y + thickness, rect.w - 2 * thickness, rect.h - 2 * thickness};
        if (inner.empty()) return ellipseFilled(rect, clip);
        const std::map<int, std::pair<int, int>> outerRows = rowSpans(ellipseBorder(rect));
        const std::map<int, std::pair<int, int>> innerRows = rowSpans(ellipseBorder(inner));
        std::vector<IntPoint> out;
        for (const auto& [y, span] : outerRows) {
            auto hole = innerRows.find(y);
            if (hole == innerRows.end()) {
                emitSpan(out, y, span.first, span.second, clip);
            } else {
                emitSpan(out, y, span.first, hole->second.first - 1, clip);
                emitSpan(out, y, hole->second.second + 1, span.second, clip);
            }
        }
        return out;
    }

    std::vector<IntPoint> roundedRectFilled(const IntRect& rect, int radius, const IntRect& clip) {
        std::vector<IntPoint> out;
        if (rect.empty()) return out;
        radius = std::min({radius, rect.w / 2, rect.h / 2});
        const int from = std::max(rect.y, clip.y), to = std::min(rect.y + rect.h - 1, clip.y + clip.h - 1);
        for (int y = from; y <= to; y++) {
            const auto [start, end] = roundedSpan(rect, radius, y);
            emitSpan(out, y, start, end, clip);
        }
        return out;
    }

    std::vector<IntPoint> roundedRectOutline(const IntRect& rect, int radius, int thickness, const IntRect& clip) {
        std::vector<IntPoint> out;
        if (rect.empty()) return out;
        thickness = std::max(1, thickness);
        radius = std::min({radius, rect.w / 2, rect.h / 2});
        const IntRect inner{rect.x + thickness, rect.y + thickness, rect.w - 2 * thickness, rect.h - 2 * thickness};
        if (inner.empty()) return roundedRectFilled(rect, radius, clip);
        const int innerRadius = std::min({std::max(0, radius - thickness), inner.w / 2, inner.h / 2});
        const int from = std::max(rect.y, clip.y), to = std::min(rect.y + rect.h - 1, clip.y + clip.h - 1);
        for (int y = from; y <= to; y++) {
            const auto [start, end] = roundedSpan(rect, radius, y);
            if (y < inner.y || y >= inner.y + inner.h) {
                emitSpan(out, y, start, end, clip);
                continue;
            }
            const auto [innerStart, innerEnd] = roundedSpan(inner, innerRadius, y);
            emitSpan(out, y, start, innerStart - 1, clip);
            emitSpan(out, y, innerEnd + 1, end, clip);
        }
        return out;
    }

    std::vector<IntPoint> polygonEdges(std::span<const IntPoint> points) {
        std::vector<IntPoint> out;
        for (size_t i = 0; i < points.size(); i++) bresenham(points[i], points[(i + 1) % points.size()], out);
        dedupe(out);
        return out;
    }

    std::vector<IntPoint> polygonFilled(std::span<const IntPoint> points, const IntRect& clip) {
        std::vector<IntPoint> out;
        if (points.size() < 3) return out;
        int minY = points[0].y, maxY = points[0].y;
        for (const IntPoint& p : points) {
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        minY = std::max(minY, clip.y);
        maxY = std::min(maxY, clip.y + clip.h - 1);
        std::vector<double> crossings;
        for (int y = minY; y <= maxY; y++) {
            crossings.clear();
            // pixel centres sit on integer coordinates; an edge counts on [y0, y1)
            for (size_t i = 0; i < points.size(); i++) {
                const IntPoint a = points[i], b = points[(i + 1) % points.size()];
                if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
                    crossings.push_back(a.x + static_cast<double>(y - a.y) * (b.x - a.x) / static_cast<double>(b.y - a.y));
                }
            }
            std::sort(crossings.begin(), crossings.end());
            for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
                const int from = std::max(clip.x, static_cast<int>(std::ceil(crossings[k])));
                const int to = std::min(clip.x + clip.w - 1, static_cast<int>(std::floor(crossings[k + 1])));
                for (int x = from; x <= to; x++) out.push_back({x, y});
            }
        }
        std::vector<IntPoint> edges = polygonEdges(points);
        for (const IntPoint& p : edges) {
            if (clip.contains(p.x, p.y)) out.push_back(p);
        }
        dedupe(out);
        return out;
    }

    std::vector<IntPoint> stamp(std::span<const IntPoint> pixels, int thickness, const IntRect& clip) {
        std::vector<IntPoint> out;
        if (thickness <= 1) {
            for (const IntPoint& p : pixels) {
                if (clip.contains(p.x, p.y)) out.push_back(p);
            }
            return out;
        }
        // a square brush centred on the pixel (even sizes lean up-left, as Pixelorama's stamps)
        const int before = (thickness - 1) / 2, after = thickness / 2;
        for (const IntPoint& p : pixels) {
            for (int dy = -before; dy <= after; dy++) {
                for (int dx = -before; dx <= after; dx++) {
                    if (clip.contains(p.x + dx, p.y + dy)) out.push_back({p.x + dx, p.y + dy});
                }
            }
        }
        dedupe(out);
        return out;
    }

    std::vector<uint8_t> floodFill(int w, int h, IntPoint seed, bool eightConnected, const std::function<bool(int, int)>& similar) {
        std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
        if (seed.x < 0 || seed.y < 0 || seed.x >= w || seed.y >= h || !similar(seed.x, seed.y)) return mask;
        auto open = [&](int x, int y) { return mask[static_cast<size_t>(y) * w + x] == 0 && similar(x, y); };
        std::vector<IntPoint> stack{seed};
        while (!stack.empty()) {
            const IntPoint p = stack.back();
            stack.pop_back();
            if (!open(p.x, p.y)) continue;
            int left = p.x, right = p.x;
            while (left - 1 >= 0 && open(left - 1, p.y)) left--;
            while (right + 1 < w && open(right + 1, p.y)) right++;
            for (int x = left; x <= right; x++) mask[static_cast<size_t>(p.y) * w + x] = 1;
            const int from = eightConnected ? std::max(0, left - 1) : left;
            const int to = eightConnected ? std::min(w - 1, right + 1) : right;
            for (int ny : {p.y - 1, p.y + 1}) {
                if (ny < 0 || ny >= h) continue;
                bool inRun = false;
                for (int x = from; x <= to; x++) {
                    const bool o = open(x, ny);
                    if (o && !inRun) stack.push_back({x, ny});
                    inRun = o;
                }
            }
        }
        return mask;
    }

    std::vector<int> bayerMatrix(int n) {
        std::vector<int> m{0};
        int side = 1;
        while (side < n) {
            const int next = side * 2;
            std::vector<int> grown(static_cast<size_t>(next) * next);
            for (int x = 0; x < side; x++) {
                for (int y = 0; y < side; y++) {
                    const int v = 4 * m[static_cast<size_t>(x) * side + y];
                    grown[static_cast<size_t>(x) * next + y] = v;
                    grown[static_cast<size_t>(x) * next + y + side] = v + 2;
                    grown[static_cast<size_t>(x + side) * next + y] = v + 3;
                    grown[static_cast<size_t>(x + side) * next + y + side] = v + 1;
                }
            }
            m.swap(grown);
            side = next;
        }
        return m;
    }

    std::vector<IntPoint> neighbourhood(int width, NeighbourShape shape) {
        std::vector<IntPoint> out;
        for (int i = -width; i <= width; i++) {
            int reach = width;
            if (shape == NeighbourShape::Diamond) reach = width - std::abs(i);
            else if (shape == NeighbourShape::Circle) {
                const double r = width + 0.5;
                reach = static_cast<int>(std::floor(std::sqrt(r * r - static_cast<double>(i) * i)));
            }
            for (int j = -reach; j <= reach; j++) {
                if (i == 0 && j == 0) continue;
                out.push_back({i, j});
            }
        }
        return out;
    }

}
