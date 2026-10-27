// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// placePopup (S17 §6.3, AC8): OpenEngine's TooltipPlacementTests cases (a 400 x 300 window, a
// source in its bottom-right corner) as pure placements, and every side's flip and clamp.

#include "htmlui/widgets/PopupPlacement.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui;

namespace {

    const PopupRect kWindow{0.0f, 0.0f, 400.0f, 300.0f};
    const PopupRect kCornerSource{340.0f, 260.0f, 50.0f, 30.0f};

    PlaceRequest request(PopupRect anchor, PopupSize popup, PopupSide side){
        PlaceRequest r;
        r.anchor = anchor;
        r.popup = popup;
        r.viewport = kWindow;
        r.preferred = side;
        return r;
    }

    void expectInside(const PlaceResult& result, const PopupRect& viewport, float pad){
        EXPECT_GE(result.rect.x, viewport.x + pad - 0.001f);
        EXPECT_GE(result.rect.y, viewport.y + pad - 0.001f);
        EXPECT_LE(result.rect.right(), viewport.right() - pad + 0.001f) << "right edge";
        EXPECT_LE(result.rect.bottom(), viewport.bottom() - pad + 0.001f) << "bottom edge";
    }

}

TEST(PopupPlacement, ARightTooltipAtTheWindowsRightEdgeFlipsLeft){
    const PlaceResult result = placePopup(request(kCornerSource, {40.0f, 20.0f}, PopupSide::Right));
    EXPECT_EQ(result.side, PopupSide::Left);
    EXPECT_TRUE(result.flipped);
    expectInside(result, kWindow, 4.0f);
    EXPECT_LE(result.rect.right(), kCornerSource.x) << "the bubble sits left of its source";
}

TEST(PopupPlacement, ABelowTooltipAtTheWindowsBottomEdgeFlipsAbove){
    const PlaceResult result = placePopup(request(kCornerSource, {40.0f, 20.0f}, PopupSide::Bottom));
    EXPECT_EQ(result.side, PopupSide::Top);
    EXPECT_TRUE(result.flipped);
    expectInside(result, kWindow, 4.0f);
    EXPECT_LE(result.rect.bottom(), kCornerSource.y) << "the bubble sits above its source";
}

TEST(PopupPlacement, ARightTooltipThatFitsOnNeitherSideStaysInsideTheWindow){
    const PlaceResult result = placePopup(request(kCornerSource, {380.0f, 20.0f}, PopupSide::Right));
    EXPECT_TRUE(result.clamped);
    expectInside(result, kWindow, 4.0f);
}

TEST(PopupPlacement, PreferredSideKeptWhenItFits){
    const PopupRect anchor{180.0f, 140.0f, 40.0f, 20.0f};
    for (PopupSide side : {PopupSide::Top, PopupSide::Bottom, PopupSide::Left, PopupSide::Right}){
        const PlaceResult result = placePopup(request(anchor, {60.0f, 30.0f}, side));
        EXPECT_EQ(result.side, side) << popupSideName(side);
        EXPECT_FALSE(result.flipped);
        EXPECT_FALSE(result.clamped);
    }
    const PlaceResult top = placePopup(request(anchor, {60.0f, 30.0f}, PopupSide::Top));
    EXPECT_FLOAT_EQ(top.rect.y, 140.0f - 6.0f - 30.0f);
    EXPECT_FLOAT_EQ(top.rect.x, 170.0f) << "centred on the anchor";
}

// four sides: each flips at its own edge
TEST(PopupPlacement, EverySideFlipsAtItsEdge){
    const PopupSize size{60.0f, 30.0f};
    const PlaceResult top = placePopup(request({180.0f, 5.0f, 40.0f, 20.0f}, size, PopupSide::Top));
    EXPECT_EQ(top.side, PopupSide::Bottom);
    EXPECT_GE(top.rect.y, 25.0f);
    const PlaceResult bottom = placePopup(request({180.0f, 275.0f, 40.0f, 20.0f}, size, PopupSide::Bottom));
    EXPECT_EQ(bottom.side, PopupSide::Top);
    EXPECT_LE(bottom.rect.bottom(), 275.0f);
    const PlaceResult left = placePopup(request({5.0f, 140.0f, 40.0f, 20.0f}, size, PopupSide::Left));
    EXPECT_EQ(left.side, PopupSide::Right);
    EXPECT_GE(left.rect.x, 45.0f);
    const PlaceResult right = placePopup(request({355.0f, 140.0f, 40.0f, 20.0f}, size, PopupSide::Right));
    EXPECT_EQ(right.side, PopupSide::Left);
    EXPECT_LE(right.rect.right(), 355.0f);
}

// no flip when the other side has even less room: the popup is clamped instead
TEST(PopupPlacement, NoFlipTowardsLessRoom){
    PlaceRequest r = request({180.0f, 20.0f, 40.0f, 260.0f}, {60.0f, 40.0f}, PopupSide::Top);
    const PlaceResult result = placePopup(r);
    EXPECT_EQ(result.side, PopupSide::Top);
    EXPECT_FALSE(result.flipped);
    EXPECT_TRUE(result.clamped);
    expectInside(result, kWindow, 4.0f);
}

// clamped along the edge: near the left edge a top popup slides right; the arrow still points at the anchor
TEST(PopupPlacement, ClampAlongTheEdgeKeepsTheArrowOnTheAnchor){
    PlaceRequest r = request({2.0f, 150.0f, 10.0f, 10.0f}, {100.0f, 30.0f}, PopupSide::Top);
    r.arrow = 6.0f;
    const PlaceResult result = placePopup(r);
    EXPECT_TRUE(result.clamped);
    EXPECT_FLOAT_EQ(result.rect.x, 4.0f);
    EXPECT_FLOAT_EQ(result.arrowOffset, 6.0f) << "the anchor's centre (7) is left of the arrow's minimum";
    r.anchor = {30.0f, 150.0f, 10.0f, 10.0f};
    EXPECT_FLOAT_EQ(placePopup(r).arrowOffset, 35.0f - 4.0f);
}

// title tooltips: a 1x1 anchor at the pointer, 16 px below it, above when there is no room
TEST(PopupPlacement, PointerAnchorFlipsAboveAtTheBottom){
    PlaceRequest r = request({100.0f, 100.0f, 1.0f, 1.0f}, {80.0f, 24.0f}, PopupSide::Bottom);
    r.gap = 16.0f;
    PlaceResult result = placePopup(r);
    EXPECT_FLOAT_EQ(result.rect.y, 117.0f);
    r.anchor = {100.0f, 280.0f, 1.0f, 1.0f};
    result = placePopup(r);
    EXPECT_EQ(result.side, PopupSide::Top);
    EXPECT_FLOAT_EQ(result.rect.bottom(), 280.0f - 16.0f);
}

TEST(PopupPlacement, AutomaticTakesTheSideWithRoom){
    PlaceRequest r = request({10.0f, 10.0f, 30.0f, 30.0f}, {100.0f, 40.0f}, PopupSide::Top);
    r.automatic = true;
    const PlaceResult result = placePopup(r);
    EXPECT_TRUE(result.side == PopupSide::Bottom || result.side == PopupSide::Right);
    expectInside(result, kWindow, 4.0f);
    EXPECT_FALSE(result.clamped);
}

TEST(PopupPlacement, SideNames){
    PopupSide side = PopupSide::Top;
    EXPECT_TRUE(parsePopupSide("right", side));
    EXPECT_EQ(side, PopupSide::Right);
    EXPECT_FALSE(parsePopupSide("auto", side));
    EXPECT_STREQ(popupSideName(PopupSide::Bottom), "bottom");
}
