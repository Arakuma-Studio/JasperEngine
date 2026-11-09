// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC4 (R19, R20): outline and inline, the Shading.gd formulas (values computed with a port of
// the GDScript in Python: #3b5dc9 lighten (89,145,227) darken (34,44,176), #f0e000 lighten clamps at
// hue 60), the ramp step and the indexed snap / add.

#include "SpriteDrawTestSupport.h"

#include "pixelart/draw/DrawPrimitives.h"
#include "pixelart/draw/ShadeColor.h"

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::s47test::json;

TEST(ColorOps, NeighbourhoodShapes) {
    EXPECT_EQ(neighbourhood(1, NeighbourShape::Diamond).size(), 4u);
    EXPECT_EQ(neighbourhood(1, NeighbourShape::Square).size(), 8u);
    EXPECT_EQ(neighbourhood(2, NeighbourShape::Diamond).size(), 12u);
    // floor(sqrt(2.5^2 - i^2)): reaches 2,2,2,... at i=0, 2 at i=1, 1 at i=2
    EXPECT_EQ(neighbourhood(2, NeighbourShape::Circle).size(), 20u);
}

TEST(ColorOps, OutlineAndInlineMatchHandMadeMasks) {
    draw::SpriteDoc sprite = s47test::makeSprite(5, 5);
    S47_APPLY(sprite, json::array({{{"op", "rect"}, {"rect", {1, 1, 3, 3}}, {"color", 1}, {"fill", true}}, {{"op", "outline"}, {"color", 2}}}));
    std::vector<std::string> rows = s47test::canvasRows(sprite);
    EXPECT_EQ(rows[0], ".222.");
    EXPECT_EQ(rows[1], "21112");
    EXPECT_EQ(rows[2], "21112");
    EXPECT_EQ(rows[4], ".222.");

    draw::SpriteDoc square = s47test::makeSprite(5, 5);
    S47_APPLY(square, json::array({{{"op", "rect"}, {"rect", {1, 1, 3, 3}}, {"color", 1}, {"fill", true}}, {{"op", "outline"}, {"color", 2}, {"shape", "square"}}}));
    rows = s47test::canvasRows(square);
    EXPECT_EQ(rows[0], "22222") << "a square brush reaches the corners";

    draw::SpriteDoc inside = s47test::makeSprite(5, 5);
    S47_APPLY(inside, json::array({{{"op", "rect"}, {"rect", {0, 0, 5, 5}}, {"color", 1}, {"fill", true}},
                                   {{"op", "rect"}, {"rect", {2, 2, 1, 1}}, {"color", "transparent"}, {"fill", true}},
                                   {{"op", "inline"}, {"color", 3}}}));
    rows = s47test::canvasRows(inside);
    EXPECT_EQ(rows[0], "33333") << "the canvas edge counts as outside";
    EXPECT_EQ(rows[1], "31313") << "the hole's neighbours";
    EXPECT_EQ(rows[2], "33.33");
    EXPECT_EQ(rows[3], "31313");
}

TEST(ColorOps, OutlineWidthTwoDiamond) {
    draw::SpriteDoc sprite = s47test::makeSprite(5, 5);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{2, 2}}}, {"color", 1}}, {{"op", "outline"}, {"color", 2}, {"width", 2}}}));
    const std::vector<std::string> rows = s47test::canvasRows(sprite);
    EXPECT_EQ(rows[0], "..2..");
    EXPECT_EQ(rows[1], ".222.");
    EXPECT_EQ(rows[2], "22122");
}

TEST(ColorOps, HueShiftMatchesShadingFormulas) {
    ShadeParams params;
    params.hueShift = true;
    EXPECT_EQ(shadeColor({0x3b, 0x5d, 0xc9, 255}, params), (PixelRgba{89, 145, 227, 255}));
    params.lighten = false;
    EXPECT_EQ(shadeColor({0x3b, 0x5d, 0xc9, 255}, params), (PixelRgba{34, 44, 176, 255}));
}

TEST(ColorOps, HueLimitsAreHit) {
    ShadeParams params;
    params.hueShift = true;
    // hue 56 lightens only as far as 60 (yellow): the limit is hit, not passed
    GodotColor c{};
    const PixelRgba lighter = shadeColor({0xf0, 0xe0, 0x00, 255}, params);
    c.r = lighter[0] / 255.0;
    c.g = lighter[1] / 255.0;
    c.b = lighter[2] / 255.0;
    EXPECT_NEAR(c.h() * 360.0, 60.0, 1.0);
    // hue 240 (blue) darkens only as far as 240
    params.lighten = false;
    const PixelRgba darker = shadeColor({0x10, 0x10, 0xe0, 255}, params);
    c.r = darker[0] / 255.0;
    c.g = darker[1] / 255.0;
    c.b = darker[2] / 255.0;
    EXPECT_NEAR(c.h() * 360.0, 240.0, 1.0);
}

TEST(ColorOps, SimpleLightenAndDarken) {
    ShadeParams params;
    EXPECT_EQ(shadeColor({0x3b, 0x5d, 0xc9, 255}, params), (PixelRgba{79, 109, 206, 255}));
    params.lighten = false;
    EXPECT_EQ(shadeColor({0x3b, 0x5d, 0xc9, 255}, params), (PixelRgba{53, 84, 181, 255}));
}

TEST(ColorOps, RampMovesOneStepAndLeavesOtherColours) {
    draw::SpriteDoc sprite = s47test::makeSprite(3, 1);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"125"}}}}));
    S47_APPLY(sprite, json::array({{{"op", "shade"}, {"mode", "ramp"}, {"direction", "lighten"}, {"ramp", {1, 2, 3}}}}));
    EXPECT_EQ(s47test::canvasRows(sprite)[0], "235");
    S47_APPLY(sprite, json::array({{{"op", "shade"}, {"mode", "ramp"}, {"direction", "darken"}, {"ramp", {1, 2, 3}}}}));
    EXPECT_EQ(s47test::canvasRows(sprite)[0], "125");
}

TEST(ColorOps, TransparentPixelsNeverChange) {
    draw::SpriteDoc sprite = s47test::makeSprite(2, 1, aseprite::ColorMode::Rgba);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "#808080"}}, {{"op", "shade"}, {"mode", "simple"}, {"rect", {0, 0, 2, 1}}}}));
    const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
    EXPECT_NE(canvas.at(0, 0), (PixelRgba{128, 128, 128, 255}));
    EXPECT_EQ(canvas.at(1, 0)[3], 0);
}

TEST(ColorOps, IndexedShadeSnapsAddsAndFailsWhenFull) {
    draw::SpriteDoc sprite = s47test::makeSprite(1, 1);
    sprite.doc.palette = s47test::drawPalette(9);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 8}}}));
    // snap: still an index of the palette
    S47_APPLY(sprite, json::array({{{"op", "shade"}, {"mode", "simple"}, {"direction", "darken"}, {"amount", 20}, {"points", {{0, 0}}}}}));
    int index = draw::celCanvas(sprite.doc, 0, 0).indexAt(0, 0);
    EXPECT_LT(index, 9);

    draw::SpriteDoc adding = s47test::makeSprite(1, 1);
    adding.doc.palette = s47test::drawPalette(9);
    S47_APPLY(adding, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 8}}}));
    s47test::Run added = s47test::apply(adding, json::array({{{"op", "shade"}, {"mode", "simple"}, {"direction", "darken"}, {"amount", 20}, {"points", {{0, 0}}}, {"palette_mode", "add"}}}));
    ASSERT_TRUE(added.ok) << added.problem.message;
    EXPECT_TRUE(added.result.structureChanged);
    EXPECT_EQ(adding.doc.palette.size(), 10u);
    EXPECT_EQ(draw::celCanvas(adding.doc, 0, 0).indexAt(0, 0), 9);

    draw::SpriteDoc full = s47test::makeSprite(1, 1);
    full.doc.palette.resize(256);
    for (int i = 1; i < 256; i++) full.doc.palette[static_cast<size_t>(i)] = {{static_cast<uint8_t>(i), 10, 20, 255}, std::nullopt};
    S47_APPLY(full, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 100}}}));
    s47test::Run rejected = s47test::apply(full, json::array({{{"op", "shade"}, {"mode", "simple"}, {"amount", 40}, {"points", {{0, 0}}}, {"palette_mode", "add"}}}));
    ASSERT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.problem.kind, "palette_full");
}
