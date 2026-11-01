// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Native UI on the GPU (ui-scaling-text spec 14): Text with a 3 px outline and with a soft shadow
// (signed-distance glyphs, goldens), points under a Panel clipped by it and sized in UI units, and
// a UI scene under a Canvas Scaler (Expand) filling a 4:3 view with its stretch-anchored image.

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "object/Points.h"
#include "object/ui/Image.h"
#include "object/ui/Panel.h"
#include "object/ui/Text.h"

using namespace jasperengine;
using namespace jasperengine::test;

TEST_F(FullPathGoldenTest, TextOutline){
    SceneFixture fixture;

    Text& plain = fixture.add<Text>();
    plain.setText("Plain");
    plain.setFontSize(40);
    plain.setColor(1.0f, 1.0f, 1.0f, 1.0f);
    plain.setPosition(40, 60);

    Text& outlined = fixture.add<Text>();
    outlined.setText("Outline");
    outlined.setFontSize(40);
    outlined.setColor(1.0f, 1.0f, 1.0f, 1.0f);
    outlined.setOutlineWidth(3.0f);
    outlined.setOutlineColor(Vector4(1.0f, 0.0f, 0.0f, 1.0f));
    outlined.setPosition(40, 160);

    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen screen;
    ASSERT_TRUE(capture(screen));

    // the plain text has no red; the outlined one a red band around white glyphs
    EXPECT_EQ(screen.countColor({30, 40, 400, 120}, 255, 0, 0, 40), 0u);
    EXPECT_GT(screen.countColor({30, 140, 400, 220}, 255, 0, 0, 40), 200u);
    EXPECT_GT(screen.countColor({30, 140, 400, 220}, 255, 255, 255, 30), 200u);

    EXPECT_SCREEN_GOLDEN(screen, "ui_text_outline");
}

TEST_F(FullPathGoldenTest, TextShadow){
    SceneFixture fixture;

    Text& shadowed = fixture.add<Text>();
    shadowed.setText("Shadow");
    shadowed.setFontSize(48);
    shadowed.setColor(1.0f, 1.0f, 1.0f, 1.0f);
    shadowed.setShadowEnabled(true);
    shadowed.setShadowOffset(Vector2(2.0f, 2.0f));
    shadowed.setShadowSoftness(4.0f);
    shadowed.setShadowColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    shadowed.setPosition(40, 100);

    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen screen;
    ASSERT_TRUE(capture(screen));

    // darker than the background around the glyphs (the soft shadow), white glyphs on top
    EXPECT_GT(screen.countColor({30, 80, 420, 180}, 255, 255, 255, 30), 200u);
    EXPECT_GT(screen.countColor({30, 80, 420, 180}, 8, 8, 8, 14), 50u);

    EXPECT_SCREEN_GOLDEN(screen, "ui_text_shadow");
}

TEST_F(FullPathGoldenTest, UIPointsClippedByAPanel){
    SceneFixture fixture;

    Panel& panel = fixture.add<Panel>();
    panel.setPosition(100, 100);
    panel.setSize(200, 120);
    panel.setColor(0.0f, 0.0f, 0.5f, 1.0f);

    // squares of 20 UI units: one inside the panel, one across its right edge, one outside
    Points& points = fixture.add<Points>();
    panel.addChild(&points);
    points.setPosition(0, 0);
    PointData point;
    point.color = Vector4(0.0f, 1.0f, 0.0f, 1.0f);
    point.size = 20.0f;
    point.position = Vector3(60, 60, 0);
    points.addPoint(point);
    point.position = Vector3(200, 60, 0);
    points.addPoint(point);
    point.position = Vector3(400, 60, 0);
    points.addPoint(point);

    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen screen;
    ASSERT_TRUE(capture(screen));

    // inside: a 20 x 20 green square (UI units = pixels at this canvas)
    EXPECT_NEAR((double)screen.countColor({150, 150, 170, 170}, 0, 255, 0, 40), 400.0, 60.0);
    // across the edge: only its half inside the panel
    EXPECT_GT(screen.countColor({288, 150, 300, 170}, 0, 255, 0, 40), 150u);
    EXPECT_EQ(screen.countColor({301, 150, 312, 170}, 0, 255, 0, 40), 0u);
    // outside: clipped away
    EXPECT_EQ(screen.countColor({480, 140, 520, 180}, 0, 255, 0, 40), 0u);
}

TEST_F(FullPathGoldenTest, CanvasScalerExpandFillsTheView){
    // a 4:3 screen, the game canvas 640x480; the UI designed at 1920x1080 with Expand: 1920x1440
    GpuEnvironment::setView(640, 480, 640, 480, Scaling::FITWIDTH);
    SceneFixture fixture;
    CanvasScalerSettings settings;
    settings.mode = UIScaleMode::ScaleWithScreenSize;
    settings.referenceResolution = Vector2(1920, 1080);
    settings.screenMatchMode = ScreenMatchMode::Expand;
    fixture.getScene().setCanvasScaler(settings);
    fixture.getScene().setCanvasScalerUseProjectDefault(false);

    Image& backdrop = fixture.add<Image>();
    backdrop.setAnchorPreset(AnchorPreset::FULL_LAYOUT);
    backdrop.setColor(1.0f, 0.0f, 0.0f, 1.0f);

    Image& centre = fixture.add<Image>();
    centre.setAnchorPreset(AnchorPreset::CENTER);
    centre.setSize(300, 300);
    centre.setColor(0.0f, 1.0f, 0.0f, 1.0f);

    ASSERT_TRUE(fixture.waitUntilLoaded());
    EXPECT_NEAR(fixture.getScene().getCanvasScale().uiWidth, 1920.0f, 1e-2f);
    EXPECT_NEAR(fixture.getScene().getCanvasScale().uiHeight, 1440.0f, 1e-2f);

    Screen screen;
    ASSERT_TRUE(capture(screen));
    // the backdrop's corners, the 300-unit square at 1/3 of a pixel per unit: 100 x 100 px
    EXPECT_TRUE(screen.is(2, 2, 255, 0, 0, 30));
    EXPECT_TRUE(screen.is(637, 477, 255, 0, 0, 30));
    EXPECT_TRUE(screen.is(320, 240, 0, 255, 0, 30));
    EXPECT_NEAR((double)screen.countColor({250, 170, 390, 310}, 0, 255, 0, 30), 10000.0, 900.0);
}
