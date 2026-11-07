// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/designer/TitleSnap.h"

#include <cmath>

using namespace jasperengine::editor::titledesigner;

namespace {

    // a context without a canvas (its edges and centre would be targets too)
    SnapContext guidesOnly(float zoom) {
        SnapContext c;
        c.zoom = zoom;
        c.guidesX = {100.0};
        c.guidesY = {-80.0};
        return c;
    }

}

// The radius is 8 SCREEN px: 8 canvas px at zoom 1, 2 at zoom 4.
// Red control: a radius in canvas px (threshold() returning settings.radiusPx) snaps the 9/4 px case at zoom 4.
TEST(TitleSnapTest, SnapsWithinEightScreenPxAtTwoZooms) {
    const TitleRect box{200.0, 200.0, 50.0, 20.0};    // left 200: moved to 93 / 91 by the deltas below
    for (float zoom : {1.0f, 4.0f}) {
        const SnapContext c = guidesOnly(zoom);
        const double inside = 7.0 / zoom, outside = 9.0 / zoom;
        const SnapResult in = snapBox(c, box, {100.0 - inside - 200.0, 0.0});
        EXPECT_TRUE(in.snappedX) << "zoom " << zoom;
        EXPECT_NEAR(box.x + in.delta.x, 100.0, 1e-9) << "zoom " << zoom;
        ASSERT_EQ(in.lines.size(), 1u);
        EXPECT_TRUE(in.lines[0].vertical);
        EXPECT_DOUBLE_EQ(in.lines[0].at, 100.0);
        const SnapResult out = snapBox(c, box, {100.0 - outside - 200.0, 0.0});
        EXPECT_FALSE(out.snappedX) << "zoom " << zoom;
        EXPECT_NEAR(box.x + out.delta.x, 100.0 - outside, 1e-9);
        EXPECT_TRUE(out.lines.empty());
    }
}

// Edges and centres of the canvas and of other layers, and guides, on both axes.
TEST(TitleSnapTest, EdgesCentresAndGuides) {
    SnapContext c;
    c.canvas = canvasRect(1000, 600);           // x -500..500, y -300..300
    c.others = {{300.0, -50.0, 100.0, 100.0}};  // x 300..400
    c.guidesY = {150.0};
    const TitleRect box{0.0, 0.0, 60.0, 40.0};
    // the right edge to the canvas's right edge
    SnapResult r = snapBox(c, box, {435.0, 0.0});
    EXPECT_NEAR(box.x + box.width + r.delta.x, 500.0, 1e-9);
    // the centre to the canvas centre (x 0) and the bottom edge to the canvas centre (y 0)
    r = snapBox(c, box, {-33.0, 3.0});
    EXPECT_NEAR(box.x + box.width * 0.5 + r.delta.x, 0.0, 1e-9);
    EXPECT_NEAR(box.y + r.delta.y, 0.0, 1e-9);
    // the right edge to the other layer's left edge, with the line spanning both
    r = snapBox(c, box, {236.0, -20.0});
    EXPECT_NEAR(box.x + box.width + r.delta.x, 300.0, 1e-9);
    bool line = false;
    for (const SnapLine& l : r.lines) {
        if (l.vertical && std::abs(l.at - 300.0) < 1e-9) {
            line = true;
            EXPECT_LE(l.from, -50.0);
            EXPECT_GE(l.to, 50.0);
        }
    }
    EXPECT_TRUE(line);
    // the top edge to a horizontal guide
    r = snapBox(c, box, {-200.0, 105.0});
    EXPECT_NEAR(box.y + box.height + r.delta.y, 150.0, 1e-9);
    // a point to the other layer's centre
    const SnapResult p = snapPoint(c, {345.0, 4.0}, {0.0, 0.0});
    EXPECT_NEAR(345.0 + p.delta.x, 350.0, 1e-9);
    EXPECT_NEAR(4.0 + p.delta.y, 0.0, 1e-9);
}

// The distances to the neighbours, labelled "24 px".
TEST(TitleSnapTest, GapsToNeighbours) {
    SnapContext c;
    c.others = {{124.0, 0.0, 50.0, 50.0}, {-300.0, 0.0, 50.0, 50.0}, {124.0, 500.0, 50.0, 50.0}};
    const SnapResult r = snapBox(c, {0.0, 0.0, 100.0, 50.0}, {0.0, 0.0});
    ASSERT_EQ(r.gaps.size(), 2u);
    double right = 0.0, left = 0.0;
    for (const SnapGap& g : r.gaps) {
        if (g.from.x >= 100.0) right = g.length;
        else left = g.length;
    }
    EXPECT_DOUBLE_EQ(right, 24.0);
    EXPECT_DOUBLE_EQ(left, 250.0);
    EXPECT_EQ(formatGap(24.0), "24 px");
    EXPECT_EQ(formatGap(12.5), "12.5 px");
}

// The pixel grid only when shown, and never over a real target; nothing when Snap is off.
TEST(TitleSnapTest, PixelGridAndDisabled) {
    SnapContext c;
    c.zoom = 1.0f;
    SnapResult r = snapPoint(c, {10.3, 20.6}, {0.0, 0.0});
    EXPECT_FALSE(r.snappedX);
    c.settings.pixelGrid = true;
    r = snapPoint(c, {10.3, 20.6}, {0.0, 0.0});
    EXPECT_NEAR(10.3 + r.delta.x, 10.0, 1e-9);
    EXPECT_NEAR(20.6 + r.delta.y, 21.0, 1e-9);
    EXPECT_TRUE(r.lines.empty());
    c.guidesX = {10.5};
    r = snapPoint(c, {10.3, 0.0}, {0.0, 0.0});
    EXPECT_NEAR(10.3 + r.delta.x, 10.5, 1e-9);
    c.settings.enabled = false;
    r = snapPoint(c, {10.3, 0.0}, {0.0, 0.0});
    EXPECT_DOUBLE_EQ(r.delta.x, 0.0);
}

// Ctrl's increments: 10 px, 15°, 0.1 scale, and the edited ones of the Snap menu.
// Red control: snapToIncrement truncating instead of rounding gives -20 for -27.
TEST(TitleSnapTest, CtrlIncrements) {
    SnapSettings s;
    const TitleVec2 d = incrementMove({13.0, -27.0}, s);
    EXPECT_DOUBLE_EQ(d.x, 10.0);
    EXPECT_DOUBLE_EQ(d.y, -30.0);
    EXPECT_DOUBLE_EQ(incrementAngle(37.0, s), 30.0);
    EXPECT_DOUBLE_EQ(incrementAngle(-8.0, s), -15.0);
    EXPECT_NEAR(incrementScale(1.26, s), 1.3, 1e-12);
    EXPECT_NEAR(incrementScale(0.02, s), 0.1, 1e-12);
    s.moveIncrement = 5.0;
    s.rotateIncrement = 45.0;
    s.scaleIncrement = 0.25;
    EXPECT_DOUBLE_EQ(incrementMove({13.0, 0.0}, s).x, 15.0);
    EXPECT_DOUBLE_EQ(incrementAngle(30.0, s), 45.0);
    EXPECT_DOUBLE_EQ(incrementScale(1.4, s), 1.5);
}
