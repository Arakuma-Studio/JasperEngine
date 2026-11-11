// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/designer/TitleLetterGizmo.h"

#include <cmath>

using namespace jasperengine::editor::titledesigner;

namespace {

    // two selected characters (code points 1 and 2) of a 200 px text at zoom 1, the document
    // centre at screen (500, 500); the second is already turned 10 degrees and scaled 1.5
    TitleLetterGizmo twoLetters(const Affine2& layerToDoc = Affine2()) {
        LetterGlyph a;
        a.index = 1;
        a.centre = {0.0, 0.0};
        a.width = 80.0;
        a.height = 100.0;
        LetterGlyph b;
        b.index = 2;
        b.centre = {200.0, 0.0};
        b.width = 80.0;
        b.height = 100.0;
        b.rotation = 10.0;
        b.scale = 1.5;
        b.offset = {4.0, -2.0};
        CanvasXform xform;
        xform.zoom = 1.0f;
        xform.origin = ImVec2(500.0f, 500.0f);
        TitleLetterGizmo gizmo;
        gizmo.setGlyphs({a, b}, layerToDoc, xform, 200.0);
        return gizmo;
    }

    float distance(ImVec2 a, ImVec2 b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    }

}

// R38: each selected character has its handles; the knob and the square win over the cell
TEST(TitleLetterGizmoTest, HandlesOfEachCharacter) {
    TitleLetterGizmo gizmo = twoLetters();
    EXPECT_EQ(gizmo.hitTest(gizmo.centreOf(0)).part, TitleLetterGizmo::Part::Move);
    EXPECT_EQ(gizmo.hitTest(gizmo.centreOf(1)).glyph, 1);
    EXPECT_EQ(gizmo.hitTest(gizmo.knobOf(1)).part, TitleLetterGizmo::Part::Rotate);
    EXPECT_EQ(gizmo.hitTest(gizmo.scaleHandleOf(0)).part, TitleLetterGizmo::Part::Scale);
    EXPECT_EQ(gizmo.hitTest(ImVec2(500.0f + 100.0f, 500.0f)).part, TitleLetterGizmo::Part::None) << "between the cells";
    // the knob sits above the cell, the square at its lower right (screen y down)
    EXPECT_LT(gizmo.knobOf(0).y, gizmo.centreOf(0).y - 50.0f);
    EXPECT_NEAR(gizmo.scaleHandleOf(0).x, 540.0f, 1e-3f);
    EXPECT_NEAR(gizmo.scaleHandleOf(0).y, 550.0f, 1e-3f);
}

// AC18: a quarter turn of one character's knob turns every selected character by 90 degrees
// clockwise, from its own value, as one "Rotate Letters" set
TEST(TitleLetterGizmoTest, RotateTurnsEverySelectedCharacter) {
    TitleLetterGizmo gizmo = twoLetters();
    const ImVec2 c = gizmo.centreOf(0), k = gizmo.knobOf(0);
    gizmo.begin(gizmo.hitTest(k), k);
    ASSERT_TRUE(gizmo.dragging());
    const Json v = gizmo.values(ImVec2(c.x + distance(c, k), c.y), false);
    EXPECT_NEAR(v["chars.1.rotation"].get<double>(), 90.0, 1e-3);
    EXPECT_NEAR(v["chars.2.rotation"].get<double>(), 100.0, 1e-3);
    EXPECT_EQ(gizmo.stepName(), "Rotate Letters");
    // Shift snaps to 15 degree steps
    const Json snapped = gizmo.values(ImVec2(c.x + 100.0f, c.y - 10.0f), true);
    EXPECT_EQ(std::fmod(snapped["chars.1.rotation"].get<double>(), 15.0), 0.0);
}

// a mirrored layer turns its characters the other way in its own space, so they follow the pointer
TEST(TitleLetterGizmoTest, MirroredLayerTurnsTheOtherWay) {
    TitleLetterGizmo gizmo = twoLetters(Affine2::scale(-1.0, 1.0));
    const ImVec2 c = gizmo.centreOf(0), k = gizmo.knobOf(0);
    gizmo.begin(gizmo.hitTest(k), k);
    const Json v = gizmo.values(ImVec2(c.x + distance(c, k), c.y), false);
    EXPECT_NEAR(v["chars.1.rotation"].get<double>(), -90.0, 1e-3);
}

// moving writes the offset in % of the text size (y up); scaling multiplies each own scale
TEST(TitleLetterGizmoTest, MoveAndScaleInCharacterUnits) {
    TitleLetterGizmo gizmo = twoLetters();
    const ImVec2 c = gizmo.centreOf(0);
    gizmo.begin(gizmo.hitTest(c), c);
    EXPECT_EQ(gizmo.stepName(), "Move Letters");
    const Json moved = gizmo.values(ImVec2(c.x + 10.0f, c.y - 20.0f), false);
    EXPECT_NEAR(moved["chars.1.offset"][0].get<double>(), 5.0, 1e-6);
    EXPECT_NEAR(moved["chars.1.offset"][1].get<double>(), 10.0, 1e-6);
    EXPECT_NEAR(moved["chars.2.offset"][0].get<double>(), 9.0, 1e-6);
    EXPECT_NEAR(moved["chars.2.offset"][1].get<double>(), 8.0, 1e-6);
    gizmo.end();

    const ImVec2 s = gizmo.scaleHandleOf(0);
    gizmo.begin(gizmo.hitTest(s), s);
    EXPECT_EQ(gizmo.stepName(), "Scale Letters");
    const Json scaled = gizmo.values(ImVec2(c.x + (s.x - c.x) * 2.0f, c.y + (s.y - c.y) * 2.0f), false);
    EXPECT_NEAR(scaled["chars.1.scale"].get<double>(), 2.0, 1e-3);
    EXPECT_NEAR(scaled["chars.2.scale"].get<double>(), 3.0, 1e-3);
}
