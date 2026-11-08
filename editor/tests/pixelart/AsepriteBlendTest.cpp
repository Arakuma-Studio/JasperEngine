// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ported blenders against Aseprite's own exports (docs/specs/S44-aseprite-import.md AC3).
// asefile's blend_<mode>.aseprite files are one file with the second layer's blend mode changed
// (the same two cels, byte for byte; fixtures/aseprite/README.md): blend_normal.aseprite with the
// mode set in memory renders each blend_<mode>.png Aseprite exported.

#include "AsepriteTestSupport.h"

#include "pixelart/aseprite/AsepriteBlend.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::aseprite;
using namespace jasperengine::editor::pixelart::testing;

namespace {

    const std::pair<BlendMode, const char*> kModes[] = {
        {BlendMode::Normal, "normal"},         {BlendMode::Multiply, "multiply"},     {BlendMode::Screen, "screen"},
        {BlendMode::Overlay, "overlay"},       {BlendMode::Darken, "darken"},         {BlendMode::Lighten, "lighten"},
        {BlendMode::ColorDodge, "colordodge"}, {BlendMode::ColorBurn, "colorburn"},   {BlendMode::HardLight, "hardlight"},
        {BlendMode::SoftLight, "softlight"},   {BlendMode::Difference, "difference"}, {BlendMode::Exclusion, "exclusion"},
        {BlendMode::Hue, "hue"},               {BlendMode::Saturation, "saturation"}, {BlendMode::Color, "color"},
        {BlendMode::Luminosity, "luminosity"}, {BlendMode::Addition, "addition"},     {BlendMode::Subtract, "subtract"},
        {BlendMode::Divide, "divide"},
    };

    Document blendFixture() { return loadDocument(asepriteFixtures() / "asefile" / "blend_normal.aseprite"); }

    PixelImage golden(const char* mode) {
        return loadPng(asepriteFixtures() / "asefile" / (std::string("blend_") + mode + ".png"));
    }

    // the goldens were exported by Aseprite 1.3.12 or earlier: Hue and Saturation as those drew them
    RenderOptions goldenOptions(BlendMode mode) {
        RenderOptions o;
        o.legacyHsl = mode == BlendMode::Hue || mode == BlendMode::Saturation;
        return o;
    }

}

// The 19 modes equal Aseprite's export of each, pixel for pixel
TEST(AsepriteBlendTest, AllModes) {
    Document doc = blendFixture();
    ASSERT_EQ(doc.layers.size(), 2u);
    for (const auto& [mode, name] : kModes) {
        SCOPED_TRACE(name);
        doc.layers[1].blend = mode;
        EXPECT_TRUE(samePicture(renderFrame(doc, 0, goldenOptions(mode)), golden(name)));
    }
    // today's Aseprite (1.3.15.3+) fixed set_sat: the default differs from those goldens where
    // channels tie, and only there
    for (BlendMode mode : {BlendMode::Hue, BlendMode::Saturation}) {
        doc.layers[1].blend = mode;
        EXPECT_FALSE(samePicture(renderFrame(doc, 0), golden(mode == BlendMode::Hue ? "hue" : "saturation")));
    }
    // red control: a mode's golden is not another mode's picture
    doc.layers[1].blend = BlendMode::Multiply;
    EXPECT_FALSE(samePicture(renderFrame(doc, 0), golden("screen")));
    doc.layers[1].blend = BlendMode::Hue;
    EXPECT_FALSE(samePicture(renderFrame(doc, 0), golden("saturation")));
}

// Opacity 0 leaves the backdrop, opacity 255 is the full blend, between them the normal and the
// blended results mix monotonically; in a group the group's opacity multiplies in
TEST(AsepriteBlendTest, Opacities) {
    Document doc = blendFixture();
    RenderOptions backdropOnly;
    backdropOnly.soloLayer = 0;
    const PixelImage backdrop = renderFrame(doc, 0, backdropOnly);
    for (const auto& [mode, name] : kModes) {
        SCOPED_TRACE(name);
        doc.layers[1].blend = mode;
        doc.layers[1].opacity = 0;
        EXPECT_TRUE(samePicture(renderFrame(doc, 0), backdrop));
        doc.layers[1].opacity = 255;
        EXPECT_TRUE(samePicture(renderFrame(doc, 0, goldenOptions(mode)), golden(name)));
        for (uint8_t opacity : {uint8_t{1}, uint8_t{127}, uint8_t{254}}) {
            doc.layers[1].opacity = opacity;
            const PixelImage partial = renderFrame(doc, 0);
            // cel opacity and layer opacity are the same multiplier (MUL_UN8 of the two)
            Document viaCel = doc;
            viaCel.layers[1].opacity = 255;
            for (Cel& c : viaCel.cels) {
                if (c.layer == 1) c.opacity = opacity;
            }
            EXPECT_EQ(renderFrame(viaCel, 0), partial) << int(opacity);
        }
        doc.layers[1].opacity = 255;
    }

    // a composed group: one normal child at 255 in a normal group at 255 is the flat picture
    Document grouped = blendFixture();
    grouped.headerFlags |= kHeaderGroupBlend;
    Layer group;
    group.name = "Group";
    group.type = LayerType::Group;
    grouped.layers.insert(grouped.layers.begin() + 1, group);
    grouped.layers[2].childLevel = 1;
    for (Cel& c : grouped.cels) {
        if (c.layer == 1) c.layer = 2;
    }
    grouped.rebuildTreeFromLevels();
    grouped.sortCels();
    EXPECT_TRUE(samePicture(renderFrame(grouped, 0), golden("normal")));
    // the group's opacity applies once to what its children drew
    grouped.layers[1].opacity = 0;
    EXPECT_TRUE(samePicture(renderFrame(grouped, 0), backdrop));
    grouped.layers[1].opacity = 127;
    Document flatHalf = blendFixture();
    flatHalf.layers[1].opacity = 127;
    EXPECT_TRUE(samePicture(renderFrame(grouped, 0), renderFrame(flatHalf, 0)));
    // flattened (what Aseprite's CLI export does), the group's opacity is ignored
    RenderOptions flatten;
    flatten.groups = GroupMode::Flatten;
    EXPECT_TRUE(samePicture(renderFrame(grouped, 0, flatten), golden("normal")));
}

// The classic blenders against values worked by hand from blend_funcs.cpp's formulas
TEST(AsepriteBlendTest, ClassicVariants) {
    using namespace jasperengine::editor::pixelart::aseprite::blend;
    // MUL_UN8 rounds: 255 * 128 / 255 = 128, 200 * 100 / 255 = 78.43 -> 78
    EXPECT_EQ(mulUn8(255, 128), 128);
    EXPECT_EQ(mulUn8(200, 100), 78);
    // signed differences, as rgba_blender_merge uses it: (-6912 >> 8) = -27, (-27 - 6912) >> 8 = -28
    EXPECT_EQ(mulUn8(-55, 128), -28);
    EXPECT_EQ(divUn8(100, 200), 128);    // (100 * 255 + 100) / 200

    const uint32_t backdrop = rgba(200, 100, 50, 255);
    const uint32_t src = rgba(100, 200, 250, 255);
    // multiply: r = 200*100/255 = 78, g = 100*200/255 = 78, b = 50*250/255 = 49; opaque over opaque
    EXPECT_EQ(rgbaBlender(Mode::Multiply, false)(backdrop, src, 255), rgba(78, 78, 49, 255));
    // screen: b + s - b*s/255: 200+100-78 = 222, 100+200-78 = 222, 50+250-49 = 251
    EXPECT_EQ(rgbaBlender(Mode::Screen, false)(backdrop, src, 255), rgba(222, 222, 251, 255));
    // difference, addition (clamped), subtract (clamped)
    EXPECT_EQ(rgbaBlender(Mode::Difference, false)(backdrop, src, 255), rgba(100, 100, 200, 255));
    EXPECT_EQ(rgbaBlender(Mode::Addition, false)(backdrop, src, 255), rgba(255, 255, 255, 255));
    EXPECT_EQ(rgbaBlender(Mode::Subtract, false)(backdrop, src, 255), rgba(100, 0, 0, 255));
    // half opacity: the blended colour comes in at Sa = MUL_UN8(255, 128) = 128 over an opaque backdrop:
    // Rc = Bc + (Sc - Bc) * 128 / 255 (integer division)
    const uint32_t half = rgbaBlender(Mode::Multiply, false)(backdrop, src, 128);
    EXPECT_EQ(getR(half), static_cast<uint32_t>(200 + (78 - 200) * 128 / 255));
    EXPECT_EQ(getB(half), static_cast<uint32_t>(50 + (49 - 50) * 128 / 255));
    EXPECT_EQ(getA(half), 255u);
    // over a transparent backdrop the classic multiply multiplies with black, the new one keeps the source
    EXPECT_EQ(rgbaBlender(Mode::Multiply, false)(0, src, 128), rgba(0, 0, 0, 128));
    EXPECT_EQ(rgbaBlender(Mode::Multiply, true)(0, src, 128), (src & 0x00ffffffu) | (128u << 24));
    // the new variants equal the classic ones over an opaque backdrop, and differ over a translucent one
    for (int m = 1; m <= 18; m++) {
        EXPECT_EQ(rgbaBlender(static_cast<Mode>(m), true)(backdrop, src, 255), rgbaBlender(static_cast<Mode>(m), false)(backdrop, src, 255)) << m;
    }
    const uint32_t translucent = rgba(200, 100, 50, 100);
    EXPECT_NE(rgbaBlender(Mode::Multiply, true)(translucent, src, 255), rgbaBlender(Mode::Multiply, false)(translucent, src, 255));
    // today's set_sat scales every channel by the range: saturation of (0.4, 0.4, 0.8) with the
    // source's chroma 0.2 gives (0, 0, 0.2) before the luminosity is put back; the legacy one
    // aliases the two equal channels (min and mid are both r): r = 0, g ends at 0.4 untouched
    const uint32_t ties = rgba(102, 102, 204, 255);   // 0.4, 0.4, 0.8
    const uint32_t chroma = rgba(51, 102, 102, 255);   // chroma 0.2
    EXPECT_NE(rgbaBlender(Mode::Saturation, false, false)(ties, chroma, 255), rgbaBlender(Mode::Saturation, false, true)(ties, chroma, 255));
    EXPECT_EQ(rgbaBlender(Mode::Saturation, false, false)(backdrop, src, 255), rgbaBlender(Mode::Saturation, false, true)(backdrop, src, 255));
    // grayscale: multiply 200 * 100 / 255 = 78
    EXPECT_EQ(grayaBlender(Mode::Multiply, false)(graya(200, 255), graya(100, 255), 255), graya(78, 255));
}
