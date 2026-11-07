// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S80 R26 (AC22): element coverage masks by ThorVG, checked pixel by pixel: fills and their
// rules, rims as fill + stroke of 2 x grow with the rim's join (the mitre against round red
// control), fill_counters (outer contours and the flood), offsets and scale, SVG strokes,
// pictures of Image and artwork layers, and determinism.

#include "gtest/gtest.h"

#include "titleart/support/TitleTestAssets.h"

#include "titleart/geometry/TitleGeometry.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;
using namespace jasperengine::titleart::test;

namespace {

    Path square(double cx, double cy, double half, bool clockwise = false) {
        Path p;
        p.moveTo(cx - half, cy - half);
        if (clockwise) {
            p.lineTo(cx - half, cy + half);
            p.lineTo(cx + half, cy + half);
            p.lineTo(cx + half, cy - half);
        } else {
            p.lineTo(cx + half, cy - half);
            p.lineTo(cx + half, cy + half);
            p.lineTo(cx - half, cy + half);
        }
        p.close();
        return p;
    }

    // a w x h frame at `scale` mask px per layer unit, centred on the layer origin
    MaskFrame frameOf(int w, int h, double scale = 1.0) {
        MaskFrame f;
        f.width = w;
        f.height = h;
        f.scale = scale;
        f.origin = {-w / 2.0 / scale, h / 2.0 / scale};
        return f;
    }

    uint8_t at(const CoverageMask& m, int x, int y) { return m.pixels[(size_t)y * m.width + x]; }

    double coverage(const CoverageMask& m) {
        return std::accumulate(m.pixels.begin(), m.pixels.end(), 0.0) / 255.0;
    }

    TitleElement face(std::vector<Path> outlines) {
        TitleElement e;
        e.outlines = std::move(outlines);
        return e;
    }

    TitleElement rimOf(std::vector<Path> outlines, double grow, StrokeJoin join, bool fillCounters = false) {
        TitleElement e = face(std::move(outlines));
        e.kind = ElementKind::Rim;
        e.grow = grow;
        e.join = join;
        e.fillCounters = fillCounters;
        return e;
    }

}

TEST(MaskRasterTest, FillsCoverTheirArea) {
    const CoverageMask m = rasterizeElement(face({square(0, 0, 10)}), frameOf(40, 40));
    ASSERT_EQ(m.width, 40);
    ASSERT_EQ(m.height, 40);
    ASSERT_EQ(m.pixels.size(), 1600u);
    EXPECT_EQ(at(m, 20, 20), 255);
    EXPECT_EQ(at(m, 10, 10), 255) << "the first covered pixel";
    EXPECT_EQ(at(m, 9, 20), 0);
    EXPECT_EQ(at(m, 30, 20), 0);
    EXPECT_NEAR(coverage(m), 400, 2);
    // an edge through the middle of a pixel covers half of it (antialiasing the seeds read)
    const CoverageMask half = rasterizeElement(face({square(0.5, 0, 10)}), frameOf(40, 40));
    EXPECT_NEAR(at(half, 10, 20), 128, 16);
    EXPECT_NEAR(at(half, 30, 20), 128, 16);
    // y up: a shape above the origin lands in the upper rows
    const CoverageMask up = rasterizeElement(face({square(0, 10, 5)}), frameOf(40, 40));
    EXPECT_EQ(at(up, 20, 10), 255);
    EXPECT_EQ(at(up, 20, 30), 0);
    // twice the scale, twice the pixels per side
    EXPECT_NEAR(coverage(rasterizeElement(face({square(0, 0, 10)}), frameOf(80, 80, 2.0))), 1600, 4);
}

TEST(MaskRasterTest, FillRulesFollowTheOutline) {
    Path nested = square(0, 0, 15);
    nested.append(square(0, 0, 5));     // same winding: non-zero fills it, even-odd punches it
    EXPECT_EQ(at(rasterizeElement(face({nested}), frameOf(40, 40)), 20, 20), 255);
    nested.evenOdd = true;
    EXPECT_EQ(at(rasterizeElement(face({nested}), frameOf(40, 40)), 20, 20), 0);
    // a counter (reversed contour) is a hole under non-zero too
    Path counter = square(0, 0, 15);
    counter.append(square(0, 0, 5, true));
    EXPECT_EQ(at(rasterizeElement(face({counter}), frameOf(40, 40)), 20, 20), 0);
}

TEST(MaskRasterTest, RimsStrokeTwiceTheirGrowWithTheirJoin) {
    // a 20 x 20 square grown by 5: 30 x 30, the corners as the join makes them
    const CoverageMask miter = rasterizeElement(rimOf({square(0, 0, 10)}, 5, StrokeJoin::Miter), frameOf(40, 40));
    const CoverageMask round = rasterizeElement(rimOf({square(0, 0, 10)}, 5, StrokeJoin::Round), frameOf(40, 40));
    const CoverageMask bevel = rasterizeElement(rimOf({square(0, 0, 10)}, 5, StrokeJoin::Bevel), frameOf(40, 40));
    for (const CoverageMask* m : {&miter, &round, &bevel}) {
        EXPECT_EQ(at(*m, 20, 20), 255) << "the fill is part of the rim";
        EXPECT_EQ(at(*m, 6, 20), 255) << "grown by 5 on each side";
        EXPECT_EQ(at(*m, 4, 20), 0);
    }
    // the corner pixel at (5.5, 5.5) lies 6.4 from the square's corner: inside the mitre only
    EXPECT_EQ(at(miter, 5, 5), 255);
    EXPECT_EQ(at(round, 5, 5), 0);
    EXPECT_EQ(at(bevel, 5, 5), 0);
    EXPECT_NEAR(coverage(miter), 900, 3);
    EXPECT_NEAR(coverage(round), 900 - 4 * (25 - 3.14159265 * 25 / 4), 4);
    // miter limit 4: an acute spike is cut, not drawn to its far tip
    Path wedge;
    wedge.moveTo(-15, -5);
    wedge.lineTo(15, 0);
    wedge.lineTo(-15, 5);
    wedge.close();
    const CoverageMask spike = rasterizeElement(rimOf({wedge}, 3, StrokeJoin::Miter), frameOf(160, 40));
    int right = 0;
    for (int x = 0; x < spike.width; x++) {
        if (at(spike, x, 20) > 0) right = x;
    }
    // the mitre of a 19 deg tip is 1 / sin(9.5 deg) = 6 times the half width: beyond 4, bevelled
    EXPECT_LT(right - 80, 15 + 3 * 4);
    EXPECT_GE(right - 80, 14);
}

// Canvas2D's stroke (the HTML canvas "trace a path" rules Pachislop's rims are drawn with): every
// edge's rectangle of half width `grow`, and at every vertex of a closed contour the triangle
// joining the two rectangle corners on the outside of the turn, plus the mitre's triangle up to
// its tip while the mitre length / line width stays within the limit (beyond it: that bevel).
// The outside of a turn at a concave glyph vertex lies inside the glyph, so a mitre there can
// pass through a stroke thinner than it and come out on the far side: Canvas2D draws that.
namespace {

    struct Vec { double x, y; };

    bool inTriangle(Vec p, Vec a, Vec b, Vec c) {
        const auto side = [](Vec p0, Vec p1, Vec q) { return (p1.x - p0.x) * (q.y - p0.y) - (p1.y - p0.y) * (q.x - p0.x); };
        const double d1 = side(a, b, p), d2 = side(b, c, p), d3 = side(c, a, p);
        return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
    }

    bool insideNonZero(const std::vector<Vec>& poly, Vec p) {
        int winding = 0;
        for (size_t i = 0; i < poly.size(); i++) {
            const Vec a = poly[i], b = poly[(i + 1) % poly.size()];
            const double cross = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
            if (a.y <= p.y && b.y > p.y && cross > 0) winding++;
            if (a.y > p.y && b.y <= p.y && cross < 0) winding--;
        }
        return winding != 0;
    }

    // a closed polygon filled and stroked with width 2 x grow, mitre joins, limit `limit`
    bool canvasRim(const std::vector<Vec>& poly, double grow, double limit, Vec p) {
        if (insideNonZero(poly, p)) return true;
        const size_t n = poly.size();
        for (size_t i = 0; i < n; i++) {
            const Vec a = poly[i], b = poly[(i + 1) % n];
            const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
            const double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / (len * len);
            const double across = ((p.y - a.y) * dx - (p.x - a.x) * dy) / len;
            if (t >= 0 && t <= 1 && std::fabs(across) <= grow) return true;
        }
        for (size_t i = 0; i < n; i++) {
            const Vec prev = poly[(i + n - 1) % n], v = poly[i], next = poly[(i + 1) % n];
            const double l1 = std::hypot(v.x - prev.x, v.y - prev.y), l2 = std::hypot(next.x - v.x, next.y - v.y);
            const Vec d1{(v.x - prev.x) / l1, (v.y - prev.y) / l1}, d2{(next.x - v.x) / l2, (next.y - v.y) / l2};
            const double turn = d1.x * d2.y - d1.y * d2.x;
            if (turn == 0) continue;
            // the corners on the outside of the turn: right of a left turn, left of a right one
            const double s = turn > 0 ? -1.0 : 1.0;
            const Vec c1{v.x + s * -d1.y * grow, v.y + s * d1.x * grow}, c2{v.x + s * -d2.y * grow, v.y + s * d2.x * grow};
            if (inTriangle(p, v, c1, c2)) return true;
            const double cosHalf = std::sqrt(std::max(0.0, (1 + d1.x * d2.x + d1.y * d2.y) / 2));
            if (cosHalf <= 0 || 1 / cosHalf > limit) continue;
            const double bx = c1.x + c2.x - 2 * v.x, by = c1.y + c2.y - 2 * v.y, bl = std::hypot(bx, by);
            const Vec tip{v.x + bx / bl * grow / cosHalf, v.y + by / bl * grow / cosHalf};
            if (inTriangle(p, c1, tip, c2)) return true;
        }
        return false;
    }

    // Roboto Regular's N and V (the engine's built-in font, engine/core/util/DefaultFont.h), font units
    const std::vector<Vec> ROBOTO_N = {{1289, 1456}, {1289, 0}, {1095, 0}, {362, 1123}, {362, 0},
                                       {169, 0},     {169, 1456}, {362, 1456}, {1098, 330}, {1098, 1456}};
    const std::vector<Vec> ROBOTO_V = {{238, 1456}, {651, 256}, {1068, 1456}, {1277, 1456}, {738, 0}, {567, 0}, {29, 1456}};

}

TEST(MaskRasterTest, MitresFollowCanvas2DAtConcaveVertices) {
    // the classic look's outermost rim on the built-in font at 286 px (grow 21.5 px): the
    // notches of N (33 deg) and V (38 deg) mitre at 3.5 and 3.1 x the half width, within the
    // limit 4, so Canvas2D draws their tips through the thin strokes below and above them
    const double scale = 0.14, grow = 215;
    for (const auto* glyph : {&ROBOTO_N, &ROBOTO_V}) {
        Path path;
        for (size_t i = 0; i < glyph->size(); i++) {
            if (i == 0) path.moveTo((*glyph)[i].x - 650, (*glyph)[i].y - 728);
            else path.lineTo((*glyph)[i].x - 650, (*glyph)[i].y - 728);
        }
        path.close();
        std::vector<Vec> poly;
        for (const Vec& v : *glyph) poly.push_back({v.x - 650, v.y - 728});
        const MaskFrame frame = frameOf(480, 480, scale);
        const CoverageMask m = rasterizeElement(rimOf({path}, grow, StrokeJoin::Miter), frame);
        int disagree = 0, covered = 0;
        for (int y = 0; y < m.height; y++) {
            for (int x = 0; x < m.width; x++) {
                int hits = 0;
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        const Vec p{frame.origin.x + (x + (sx + 0.5) / 4) / scale, frame.origin.y - (y + (sy + 0.5) / 4) / scale};
                        if (canvasRim(poly, grow, 4.0, p)) hits++;
                    }
                }
                covered += hits > 8;
                if (std::fabs(at(m, x, y) / 255.0 - hits / 16.0) > 0.5) disagree++;
            }
        }
        // antialiased edges aside, the mask is Canvas2D's shape
        EXPECT_GT(covered, 20000);
        EXPECT_LE(disagree, covered / 500) << disagree << " of " << covered << " pixels differ from Canvas2D's stroke";
    }
    // the V's notch mitre comes out 25 px below the rim's bottom edge, as in Canvas2D
    std::vector<Vec> v;
    Path path;
    for (const Vec& p : ROBOTO_V) v.push_back({p.x - 650, p.y - 728});
    path.moveTo(v[0].x, v[0].y);
    for (size_t i = 1; i < v.size(); i++) path.lineTo(v[i].x, v[i].y);
    path.close();
    const MaskFrame frame = frameOf(480, 480, scale);
    const CoverageMask m = rasterizeElement(rimOf({path}, grow, StrokeJoin::Miter), frame);
    const Vec below{651 - 650, -728 - grow - 20 / scale};
    const int px = (int)((below.x - frame.origin.x) * scale), py = (int)((frame.origin.y - below.y) * scale);
    EXPECT_TRUE(canvasRim(v, grow, 4.0, below));
    EXPECT_EQ(at(m, px, py), 255);
    // a bevelled join there would leave that point empty
    EXPECT_FALSE(canvasRim(v, grow, 3.0, below));
}

TEST(MaskRasterTest, FillCountersFloodsEnclosedPockets) {
    Path ring = square(0, 0, 15);
    ring.append(square(0, 0, 6, true));
    const CoverageMask open = rasterizeElement(rimOf({ring}, 1, StrokeJoin::Miter), frameOf(40, 40));
    EXPECT_EQ(at(open, 20, 20), 0) << "the counter stays open";
    const CoverageMask solid = rasterizeElement(rimOf({ring}, 1, StrokeJoin::Miter, true), frameOf(40, 40));
    EXPECT_EQ(at(solid, 20, 20), 255) << "fill_counters: a solid plate";
    EXPECT_EQ(at(solid, 1, 1), 0);
    // a pocket the growth closes between two glyphs fills too: two facing brackets
    Path left, right;
    left.moveTo(-14, -10);
    left.lineTo(-2, -10);
    left.lineTo(-2, -7);
    left.lineTo(-11, -7);
    left.lineTo(-11, 7);
    left.lineTo(-2, 7);
    left.lineTo(-2, 10);
    left.lineTo(-14, 10);
    left.close();
    right = left;
    right.transform(Affine::scale(-1, 1));
    const CoverageMask pocket = rasterizeElement(rimOf({left, right}, 2.5, StrokeJoin::Miter, true), frameOf(40, 40));
    EXPECT_EQ(at(pocket, 20, 20), 255) << "the gap between the brackets closed by the rim is plate";
    const CoverageMask notFilled = rasterizeElement(rimOf({left, right}, 2.5, StrokeJoin::Miter, false), frameOf(40, 40));
    EXPECT_EQ(at(notFilled, 20, 20), 0);
    // fillCounters() itself: idempotent, the outside untouched
    CoverageMask again = pocket;
    fillCounters(again);
    EXPECT_EQ(again.pixels, pocket.pixels);
}

TEST(MaskRasterTest, OverlappingStrokesStayOuter) {
    // a cross drawn as two overlapping bars in one glyph: both bars are outer contours, so a
    // fill_counters rim fills the cross, not the cross minus its middle
    Path cross;
    cross.moveTo(-15, -3);
    cross.lineTo(15, -3);
    cross.lineTo(15, 3);
    cross.lineTo(-15, 3);
    cross.close();
    cross.moveTo(-3, -15);
    cross.lineTo(3, -15);
    cross.lineTo(3, 15);
    cross.lineTo(-3, 15);
    cross.close();
    const CoverageMask m = rasterizeElement(rimOf({cross}, 1, StrokeJoin::Miter, true), frameOf(40, 40));
    EXPECT_EQ(at(m, 20, 20), 255);
    EXPECT_EQ(at(m, 5, 20), 255);
    EXPECT_EQ(at(m, 20, 5), 255);
    EXPECT_EQ(at(m, 8, 8), 0);
}

TEST(MaskRasterTest, OffsetsAndStrokes) {
    TitleElement moved = rimOf({square(0, 0, 4)}, 1, StrokeJoin::Miter);
    moved.offset = {0, -10};    // +y up: the rim drops
    const CoverageMask m = rasterizeElement(moved, frameOf(40, 40));
    EXPECT_EQ(at(m, 20, 30), 255);
    EXPECT_EQ(at(m, 20, 20), 0);
    // an SVG stroke at its own width, then widened by 2 x grow
    StrokedPath line;
    line.path.moveTo(-15, 0);
    line.path.lineTo(15, 0);
    line.width = 2;
    TitleElement stroked;
    stroked.strokes = {line};
    const CoverageMask thin = rasterizeElement(stroked, frameOf(40, 40));
    stroked.grow = 3;
    stroked.kind = ElementKind::Rim;
    const CoverageMask wide = rasterizeElement(stroked, frameOf(40, 40));
    EXPECT_NEAR(coverage(thin), 60, 2);
    EXPECT_NEAR(coverage(wide), 30 * 8, 3);
    EXPECT_EQ(at(wide, 20, 16), 255);
    EXPECT_EQ(at(thin, 20, 16), 0);
}

TEST(MaskRasterTest, ElementBoundsEnclosesTheMask) {
    TitleElement e = rimOf({square(3, -2, 8)}, 4, StrokeJoin::Miter);
    e.offset = {2, 1};
    const TitleRect b = elementBounds(e);
    // a frame exactly on the bounds: nothing reaches its border
    MaskFrame f;
    f.scale = 2;
    f.width = (int)std::ceil(b.width * 2) + 2;
    f.height = (int)std::ceil(b.height * 2) + 2;
    f.origin = {b.x - 0.5, b.y + b.height + 0.5};
    const CoverageMask m = rasterizeElement(e, f);
    for (int x = 0; x < m.width; x++) {
        EXPECT_EQ(at(m, x, 0), 0);
        EXPECT_EQ(at(m, x, m.height - 1), 0);
    }
    for (int y = 0; y < m.height; y++) {
        EXPECT_EQ(at(m, 0, y), 0);
        EXPECT_EQ(at(m, m.width - 1, y), 0);
    }
    EXPECT_GT(coverage(m), 0);
}

TEST(MaskRasterTest, PicturesOfImagesAndArtwork) {
    // a 2 x 2 checker drawn 8 x 8: nearest keeps hard texels, linear blends them
    TitleImageData checker;
    checker.width = 2;
    checker.height = 2;
    checker.rgba = {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 255};
    TitleElement img;
    img.source.image = std::make_shared<TitleImageData>(checker);
    img.source.rect = {-4, -4, 8, 8};
    img.source.nearest = true;
    const CoverageMask hard = rasterizeElement(img, frameOf(8, 8));
    EXPECT_EQ(at(hard, 1, 1), 255);
    EXPECT_EQ(at(hard, 3, 3), 255);
    EXPECT_EQ(at(hard, 4, 3), 0);
    EXPECT_EQ(at(hard, 6, 6), 255);
    img.source.nearest = false;
    const CoverageMask soft = rasterizeElement(img, frameOf(8, 8));
    EXPECT_GT(at(soft, 4, 3), 0);
    EXPECT_LT(at(soft, 4, 3), 255);
    const TitleImageData colour = rasterizeSource(img.source, frameOf(8, 8));
    EXPECT_EQ(colour.rgba[(1 * 8 + 1) * 4 + 0], 255) << "straight colour of the red texel";
    EXPECT_EQ(colour.rgba[(1 * 8 + 1) * 4 + 1], 0);
    // reduced: area averaged, so a fine checker becomes even grey coverage
    TitleImageData fine;
    fine.width = 64;
    fine.height = 64;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            const uint8_t a = ((x + y) & 1) ? 255 : 0;
            fine.rgba.insert(fine.rgba.end(), {0, 0, 0, a});
        }
    }
    TitleElement reduced;
    reduced.source.image = std::make_shared<TitleImageData>(fine);
    reduced.source.rect = {-4, -4, 8, 8};
    const CoverageMask grey = rasterizeElement(reduced, frameOf(8, 8));
    for (uint8_t v : grey.pixels) EXPECT_NEAR(v, 128, 4);
    // artwork: LunaSVG at the frame's exact scale, the SVG's own colours
    TitleElement art;
    art.source.svg = std::make_shared<const std::string>(
        "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'><rect width='10' height='10' fill='#00ff00'/></svg>");
    art.source.rect = {-10, -5, 20, 10};
    const TitleImageData pic = rasterizeSource(art.source, frameOf(40, 20, 2.0));
    EXPECT_EQ(pic.rgba[(10 * 40 + 5) * 4 + 1], 255);
    EXPECT_EQ(pic.rgba[(10 * 40 + 5) * 4 + 3], 255);
    EXPECT_EQ(pic.rgba[(10 * 40 + 30) * 4 + 3], 0) << "the right half is empty";
    const CoverageMask artMask = rasterizeElement(art, frameOf(40, 20, 2.0));
    EXPECT_EQ(at(artMask, 5, 10), 255);
    EXPECT_EQ(at(artMask, 30, 10), 0);
}

TEST(MaskRasterTest, Deterministic) {
    Path glyph;
    glyph.moveTo(0, 0);
    glyph.cubicTo(30, 40, -20, 60, 10, 80);
    glyph.quadTo(40, 40, 30, 0);
    glyph.close();
    const TitleElement e = rimOf({glyph}, 6.3, StrokeJoin::Miter, true);
    const MaskFrame f = frameOf(150, 200, 1.7);
    const CoverageMask a = rasterizeElement(e, f);
    const CoverageMask b = rasterizeElement(e, f);
    EXPECT_EQ(a.pixels, b.pixels);
    EXPECT_GT(coverage(a), 100);
}
