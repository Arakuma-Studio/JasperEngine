// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PathUtil.h"

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

std::vector<Path> jasperengine::titleart::geometry::splitContours(const Path& path) {
    struct Splitter {
        std::vector<Path> out;
        void move(const TitleVec2& p) {
            out.emplace_back();
            out.back().evenOdd = evenOdd;
            out.back().moveTo(p.x, p.y);
        }
        void line(const TitleVec2&, const TitleVec2& p) { out.back().lineTo(p.x, p.y); }
        void quad(const TitleVec2&, const TitleVec2& c, const TitleVec2& p) { out.back().quadTo(c.x, c.y, p.x, p.y); }
        void cubic(const TitleVec2&, const TitleVec2& c1, const TitleVec2& c2, const TitleVec2& p) {
            out.back().cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
        }
        void close(const TitleVec2&, const TitleVec2&, bool closed) {
            if (closed) out.back().close();
        }
        bool evenOdd = false;
    } splitter;
    splitter.evenOdd = path.evenOdd;
    walkPath(path, splitter);
    return splitter.out;
}

Path jasperengine::titleart::geometry::reverseContour(const Path& contour) {
    struct Segment {
        PathVerb verb;
        TitleVec2 from, c1, c2;
    };
    struct Collector {
        std::vector<Segment> segments;
        TitleVec2 start, end;
        bool closed = false;
        void move(const TitleVec2& p) { start = end = p; }
        void line(const TitleVec2& from, const TitleVec2& p) {
            segments.push_back({PathVerb::Line, from, {}, {}});
            end = p;
        }
        void quad(const TitleVec2& from, const TitleVec2& c, const TitleVec2& p) {
            segments.push_back({PathVerb::Quad, from, c, {}});
            end = p;
        }
        void cubic(const TitleVec2& from, const TitleVec2& c1, const TitleVec2& c2, const TitleVec2& p) {
            segments.push_back({PathVerb::Cubic, from, c1, c2});
            end = p;
        }
        void close(const TitleVec2&, const TitleVec2&, bool isClosed) { closed = closed || isClosed; }
    } c;
    walkPath(contour, c);
    Path out;
    out.evenOdd = contour.evenOdd;
    out.moveTo(c.start.x, c.start.y);
    // the implicit closing edge first, so the reversed contour visits the same corners
    if (c.end.x != c.start.x || c.end.y != c.start.y) out.lineTo(c.end.x, c.end.y);
    for (size_t i = c.segments.size(); i-- > 0;) {
        const Segment& s = c.segments[i];
        switch (s.verb) {
            case PathVerb::Line: out.lineTo(s.from.x, s.from.y); break;
            case PathVerb::Quad: out.quadTo(s.c1.x, s.c1.y, s.from.x, s.from.y); break;
            case PathVerb::Cubic: out.cubicTo(s.c2.x, s.c2.y, s.c1.x, s.c1.y, s.from.x, s.from.y); break;
            default: break;
        }
    }
    if (c.closed) out.close();
    return out;
}
