// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Speed Lines' CPU layout (docs/specs/S83-title-effects-generators.md §6.7.3, R23): AC11 (the focus
// hole holds for the ellipse, the rectangle and a silhouette; toggling `side` keeps the other
// lines; no large angular gaps) and AC5's px_scale agreement in canvas px.

#include <gtest/gtest.h>

#include "titleart/analysis/SpeedLineLayout.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr double PI = 3.14159265358979323846;

    // a 1920 x 1080 canvas around the layer's origin
    std::vector<TitleVec2> canvasFrame() {
        return {{-960.0, -540.0}, {960.0, -540.0}, {960.0, 540.0}, {-960.0, 540.0}};
    }

    SpeedLineInput baseInput() {
        SpeedLineInput in;
        in.frame = canvasFrame();
        in.k = 1.0;
        return in;
    }

    // every line's centre line sampled each 2 px
    template <class F>
    void forEachPoint(const SpeedLineLayout& lay, F visit) {
        for (const SpeedLine& line : lay.lines) {
            const int n = std::max(2, (int)std::ceil(line.length / 2.0));
            for (int i = 0; i <= n; i++) visit(line, speedLinePoint(line, lay.curvature, line.length * i / n));
        }
    }

    struct Box {
        double x0, y0, x1, y1;
    };

    // two glyph-like boxes with a gap between them, canvas px
    const std::vector<Box> GLYPHS = {{-300.0, -60.0, -80.0, 90.0}, {-40.0, -90.0, 280.0, 60.0}};

    // the boxes rasterised into frameGridSpec's lattice for a 1920 x 1080 canvas (6 px cells,
    // 320 x 192 from (-960, -576)), each cell holding its exact coverage
    Grid glyphGrid(double pxScale) {
        Grid g;
        g.cellComp = SPEED_LINE_CELL;
        g.cell = SPEED_LINE_CELL * pxScale;
        g.w = 320;
        g.h = 192;
        g.x0 = -960.0;
        g.y0 = -576.0;
        g.alpha.assign((size_t)g.w * g.h, 0.0f);
        const double c = g.cellComp;
        for (int j = 0; j < g.h; j++) {
            for (int i = 0; i < g.w; i++) {
                const double cx0 = g.x0 + i * c, cy0 = g.y0 + j * c;
                double area = 0.0;
                for (const Box& b : GLYPHS) {
                    const double w = std::max(0.0, std::min(cx0 + c, b.x1) - std::max(cx0, b.x0));
                    const double h = std::max(0.0, std::min(cy0 + c, b.y1) - std::max(cy0, b.y0));
                    area += w * h;
                }
                g.alpha[(size_t)j * g.w + i] = (float)std::min(1.0, area / (c * c));
            }
        }
        return g;
    }

    // how far the ray from (cx, cy) along angle `a` reaches before it last leaves the boxes (0
    // when it misses them all)
    double farExit(double cx, double cy, double a) {
        const double dx = std::cos(a), dy = std::sin(a);
        double far = 0.0;
        for (const Box& b : GLYPHS) {
            double t0 = -1e30, t1 = 1e30;
            const double axes[2][4] = {{cx, dx, b.x0, b.x1}, {cy, dy, b.y0, b.y1}};
            bool miss = false;
            for (const auto& axis : axes) {
                if (std::abs(axis[1]) < 1e-12) {
                    if (axis[0] < axis[2] || axis[0] > axis[3]) miss = true;
                    continue;
                }
                const double ta = (axis[2] - axis[0]) / axis[1], tb = (axis[3] - axis[0]) / axis[1];
                t0 = std::max(t0, std::min(ta, tb));
                t1 = std::min(t1, std::max(ta, tb));
            }
            if (!miss && t1 >= std::max(t0, 0.0)) far = std::max(far, t1);
        }
        return far;
    }

    bool sameLine(const SpeedLine& a, const SpeedLine& b) {
        return a.slot == b.slot && a.side == b.side && a.x == b.x && a.y == b.y && a.heading == b.heading && a.length == b.length &&
               a.halfWidth == b.halfWidth && std::equal(a.color, a.color + 4, b.color);
    }

}

TEST(SpeedLineLayoutTest, LinesStayOutsideTheEllipse) {
    for (const SpeedLineType type : {SpeedLineType::Focus, SpeedLineType::Flash, SpeedLineType::Beta}) {
        for (const double jitter : {0.0, 60.0}) {
            SpeedLineInput in = baseInput();
            in.type = type;
            in.center = {100.0, -50.0};
            in.jitter = jitter;
            in.jagCount = 12;
            const SpeedLineLayout lay = layoutSpeedLines(in);
            ASSERT_EQ(lay.lines.size(), 160u) << "type " << (int)type << ", jitter " << jitter;
            // the boundary between two of the 1,024 samples is their chord: well under half a px inside
            const double rx = in.radius.x - 0.5, ry = in.radius.y - 0.5;
            int inside = 0;
            forEachPoint(lay, [&](const SpeedLine&, const TitleVec2& p) {
                const double u = (p.x - in.center.x) / rx, v = (p.y - in.center.y) / ry;
                if (u * u + v * v < 1.0) inside++;
            });
            EXPECT_EQ(inside, 0) << "type " << (int)type << ", jitter " << jitter;
        }
    }
}

TEST(SpeedLineLayoutTest, LinesStayOutsideTheRectangle) {
    for (const double jitter : {0.0, 60.0}) {
        SpeedLineInput in = baseInput();
        in.shape = SpeedLineShape::Rect;
        in.center = {-80.0, 40.0};
        in.jitter = jitter;
        in.count = 420;
        const SpeedLineLayout lay = layoutSpeedLines(in);
        ASSERT_EQ(lay.lines.size(), 420u);
        // between two samples straddling a corner the table's chord cuts the corner by under 2 px
        const double tol = 2.0;
        int inside = 0;
        forEachPoint(lay, [&](const SpeedLine&, const TitleVec2& p) {
            if (std::abs(p.x - in.center.x) < in.radius.x - tol && std::abs(p.y - in.center.y) < in.radius.y - tol) inside++;
        });
        EXPECT_EQ(inside, 0) << "jitter " << jitter;
    }
}

TEST(SpeedLineLayoutTest, LinesStayOutsideThePaddedSilhouette) {
    const Grid grid = glyphGrid(1.0);
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    ASSERT_TRUE(gridInkBox(grid, x0, y0, x1, y1));
    EXPECT_NEAR(x0, -300.0, 1e-3);
    EXPECT_NEAR(x1, 280.0, 1e-3);
    EXPECT_NEAR(y0, -90.0, 1e-3);
    EXPECT_NEAR(y1, 90.0, 1e-3);

    for (const SpeedLineType type : {SpeedLineType::Focus, SpeedLineType::Flash}) {
        for (const double jitter : {0.0, 60.0}) {
            SpeedLineInput in = baseInput();
            in.type = type;
            in.shape = SpeedLineShape::Layer;
            in.silhouette = &grid;
            in.center = {0.5 * (x0 + x1), 0.5 * (y0 + y1)};
            in.padding = 40.0;
            in.jitter = jitter;
            in.count = 300;
            const SpeedLineLayout lay = layoutSpeedLines(in);
            ASSERT_EQ(lay.lines.size(), 300u);
            // Pachislop's padding runs along each ray from the centre; the silhouette is known to
            // the grid's resolution (a cell of partial coverage and half a march step), so a
            // point may sit up to two cells closer than the exact padded boundary
            const double tol = 2.0 * SPEED_LINE_CELL;
            int close = 0, inGlyph = 0;
            forEachPoint(lay, [&](const SpeedLine&, const TitleVec2& p) {
                const double a = std::atan2(p.y - in.center.y, p.x - in.center.x);
                const double r = std::hypot(p.x - in.center.x, p.y - in.center.y);
                if (r < farExit(in.center.x, in.center.y, a) + in.padding - tol) close++;
                for (const Box& b : GLYPHS) {
                    if (p.x > b.x0 && p.x < b.x1 && p.y > b.y0 && p.y < b.y1) inGlyph++;
                }
            });
            EXPECT_EQ(close, 0) << "type " << (int)type << ", jitter " << jitter;
            EXPECT_EQ(inGlyph, 0) << "type " << (int)type << ", jitter " << jitter;
        }
    }
}

TEST(SpeedLineLayoutTest, TogglingSideKeepsTheOtherLines) {
    SpeedLineInput in = baseInput();
    in.type = SpeedLineType::Flow;
    in.angle = 0.3;
    const SpeedLineLayout both = layoutSpeedLines(in);
    for (const SpeedLineSide side : {SpeedLineSide::Front, SpeedLineSide::Back}) {
        in.side = side;
        const SpeedLineLayout one = layoutSpeedLines(in);
        const int kept = side == SpeedLineSide::Front ? 1 : -1;
        int matched = 0;
        for (const SpeedLine& line : one.lines) {
            EXPECT_EQ(line.side, kept);
            // the same slot and side drawn with both sides on (slots past the focus become
            // sweeps across the frame there, side 0)
            auto it = std::find_if(both.lines.begin(), both.lines.end(),
                                   [&](const SpeedLine& b) { return b.slot == line.slot && b.side == line.side; });
            if (it == both.lines.end()) continue;
            EXPECT_TRUE(sameLine(line, *it)) << "slot " << line.slot << " side " << line.side;
            matched++;
        }
        EXPECT_GE(matched, 40) << "the focus covers about half the slots";
    }

    // a radial type has no sides: the toggle changes nothing
    SpeedLineInput radial = baseInput();
    const SpeedLineLayout a = layoutSpeedLines(radial);
    radial.side = SpeedLineSide::Back;
    const SpeedLineLayout b = layoutSpeedLines(radial);
    ASSERT_EQ(a.lines.size(), b.lines.size());
    for (size_t i = 0; i < a.lines.size(); i++) EXPECT_TRUE(sameLine(a.lines[i], b.lines[i])) << i;
}

TEST(SpeedLineLayoutTest, NoLargeAngularGaps) {
    SpeedLineInput in = baseInput();
    in.jitter = 60.0;
    for (const double count : {16.0, 160.0, 600.0}) {
        in.count = count;
        const SpeedLineLayout lay = layoutSpeedLines(in);
        ASSERT_EQ(lay.lines.size(), (size_t)count);
        std::vector<double> angles;
        for (const SpeedLine& line : lay.lines) angles.push_back(std::atan2(line.y - in.center.y, line.x - in.center.x));
        std::sort(angles.begin(), angles.end());
        double largest = angles.front() + 2.0 * PI - angles.back();
        for (size_t i = 1; i < angles.size(); i++) largest = std::max(largest, angles[i] - angles[i - 1]);
        const double mean = 2.0 * PI / (double)angles.size();
        EXPECT_LE(largest, 2.0 * mean) << "count " << count;
    }
}

TEST(SpeedLineLayoutTest, PxScalesAgreeInCanvasPx) {
    const Grid grid1 = glyphGrid(1.0), grid2 = glyphGrid(2.0);
    for (const SpeedLineShape shape : {SpeedLineShape::Ellipse, SpeedLineShape::Layer}) {
        for (const SpeedLineType type : {SpeedLineType::Focus, SpeedLineType::Flow, SpeedLineType::Flash}) {
            SpeedLineInput in = baseInput();
            in.type = type;
            in.shape = shape;
            in.center = {-10.0, 0.0};
            in.silhouette = &grid1;
            const SpeedLineLayout one = layoutSpeedLines(in);
            in.k = 2.0;
            in.silhouette = &grid2;
            const SpeedLineLayout two = layoutSpeedLines(in);
            ASSERT_EQ(one.lines.size(), two.lines.size()) << "shape " << (int)shape << ", type " << (int)type;
            ASSERT_FALSE(one.lines.empty());
            for (size_t i = 0; i < one.lines.size(); i++) {
                const SpeedLine& a = one.lines[i];
                const SpeedLine& b = two.lines[i];
                EXPECT_EQ(a.slot, b.slot);
                EXPECT_EQ(a.side, b.side);
                EXPECT_NEAR(a.heading, b.heading, 1e-9);
                EXPECT_EQ(a.halfWidth, b.halfWidth);
                // the only difference: lines run 2 target px past the frame edge for
                // anti-aliasing, 1 canvas px less at px_scale 2 (a sweep across the frame starts
                // that much before it and ends that much after)
                EXPECT_LE(std::hypot(a.x - b.x, a.y - b.y), 1.0 + 1e-6) << "line " << i;
                EXPECT_NEAR(a.length, b.length, 2.0 + 1e-6) << "line " << i;
            }
        }
    }
}
