// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S80 R26 (AC22): element coverage masks by ThorVG, checked pixel by pixel: fills and their
// rules, rims as fill + stroke of 2 x grow with the rim's join (the mitre against round red
// control), fill_counters (outer contours and the flood), offsets and scale, SVG strokes,
// pictures of Image and artwork layers, and determinism.

#include "gtest/gtest.h"

#include "titleart/support/TitleTestAssets.h"

#include "titleart/geometry/TitleGeometry.h"

#include <cmath>
#include <numeric>

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
