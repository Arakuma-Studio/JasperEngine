// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The background layer math of html-layer (phase-2 §3.11.3, CSS Backgrounds 3 §3.4-3.9) and
// the nine-slice of html-border-image (§6): tile rectangles and texture coordinates for size
// (auto, lengths, cover, contain), position (keywords, percentages, edge offsets), repeat
// (repeat, no-repeat, space, round), origin and clip boxes, compared with hand-computed values.

#include "htmlui/compat/runtime/HtmlBorderImageDecorator.h"
#include "htmlui/compat/runtime/HtmlLayerDecorator.h"

#include <gtest/gtest.h>

#include <array>

using namespace jasperengine::htmlui::compat;

namespace {

    LayerInput base(float iw, float ih){
        LayerInput input;
        input.positioningArea = LayerRect{0, 0, 100, 100};
        input.clipArea = LayerRect{0, 0, 100, 100};
        input.intrinsicWidth = iw;
        input.intrinsicHeight = ih;
        input.repeatX = LayerRepeat::NoRepeat;
        input.repeatY = LayerRepeat::NoRepeat;
        return input;
    }

    ::testing::AssertionResult quadIs(const LayerQuad& quad, LayerRect rect, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1){
        const auto near = [](float a, float b){ return std::fabs(a - b) < 0.01f; };
        if (quad.rect == rect && near(quad.u0, u0) && near(quad.v0, v0) && near(quad.u1, u1) && near(quad.v1, v1))
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << "quad {" << quad.rect.x << ", " << quad.rect.y << ", " << quad.rect.width << ", "
                                             << quad.rect.height << "} uv (" << quad.u0 << ", " << quad.v0 << ")-(" << quad.u1 << ", "
                                             << quad.v1 << "), expected {" << rect.x << ", " << rect.y << ", " << rect.width << ", "
                                             << rect.height << "} uv (" << u0 << ", " << v0 << ")-(" << u1 << ", " << v1 << ")";
    }

    LayerPosition percent(float value){
        return LayerPosition{value, 0.0f};
    }

}

TEST(CompatLayerMath, NoRepeatAtTheOrigin){
    const LayerGeometry g = computeLayerGeometry(base(20, 10));
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_TRUE(quadIs(g.quads[0], {0, 0, 20, 10}));
    EXPECT_FLOAT_EQ(g.tileWidth, 20.0f);
    EXPECT_FLOAT_EQ(g.tileHeight, 10.0f);
}

TEST(CompatLayerMath, PositionKeywordsPercentagesAndEdgeOffsets){
    LayerInput in = base(20, 10);
    in.positionX = percent(50);    // center: 0.5 x (100 - 20)
    in.positionY = percent(50);
    LayerGeometry g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_TRUE(quadIs(g.quads[0], {40, 45, 20, 10}));

    // right 10px bottom 20px = calc(100% - 10px) calc(100% - 20px)
    in.positionX = LayerPosition{100, -10};
    in.positionY = LayerPosition{100, -20};
    g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_TRUE(quadIs(g.quads[0], {70, 70, 20, 10}));

    // 25% 5px: lengths are offsets from the left/top
    in.positionX = percent(25);
    in.positionY = LayerPosition{0, 5};
    g = computeLayerGeometry(in);
    EXPECT_TRUE(quadIs(g.quads[0], {20, 5, 20, 10}));
}

TEST(CompatLayerMath, RepeatFillsTheClipFromThePosition){
    LayerInput in = base(20, 10);
    in.repeatX = in.repeatY = LayerRepeat::Repeat;
    LayerGeometry g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 50u);   // 5 x 10
    EXPECT_TRUE(quadIs(g.quads.front(), {0, 0, 20, 10}));
    EXPECT_TRUE(quadIs(g.quads.back(), {80, 90, 20, 10}));

    // centred: x from 40 (whole tiles at 0..80), y from 45: the first row is cut at 0
    in.positionX = percent(50);
    in.positionY = percent(50);
    g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 55u);   // 5 x 11
    EXPECT_TRUE(quadIs(g.quads[0], {0, 0, 20, 5}, 0, 0.5f, 1, 1));
    EXPECT_TRUE(quadIs(g.quads[5], {0, 5, 20, 10}));
    EXPECT_TRUE(quadIs(g.quads.back(), {80, 95, 20, 5}, 0, 0, 1, 0.5f));

    // repeat-x only: one row at the position
    in.repeatY = LayerRepeat::NoRepeat;
    g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 5u);
    EXPECT_TRUE(quadIs(g.quads[0], {0, 45, 20, 10}));
}

TEST(CompatLayerMath, CoverAndContain){
    LayerInput in = base(50, 25);
    in.sizeMode = LayerSizeMode::Cover;     // scale 4: 200 x 100
    in.positionX = percent(50);
    in.positionY = percent(50);
    LayerGeometry g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_FLOAT_EQ(g.tileWidth, 200.0f);
    EXPECT_FLOAT_EQ(g.tileHeight, 100.0f);
    EXPECT_TRUE(quadIs(g.quads[0], {0, 0, 100, 100}, 0.25f, 0, 0.75f, 1));

    in.sizeMode = LayerSizeMode::Contain;   // scale 2: 100 x 50
    g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_TRUE(quadIs(g.quads[0], {0, 25, 100, 50}));

    // no intrinsic size: the positioning area
    LayerInput none = base(0, 0);
    none.sizeMode = LayerSizeMode::Cover;
    g = computeLayerGeometry(none);
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_TRUE(quadIs(g.quads[0], {0, 0, 100, 100}));
}

TEST(CompatLayerMath, LengthSizesKeepTheRatioForAuto){
    LayerInput in = base(50, 25);
    in.sizeX = LayerSizeValue{false, 40};
    LayerGeometry g = computeLayerGeometry(in);
    EXPECT_FLOAT_EQ(g.tileWidth, 40.0f);
    EXPECT_FLOAT_EQ(g.tileHeight, 20.0f);

    in.sizeX = LayerSizeValue{};
    in.sizeY = LayerSizeValue{false, 50};
    g = computeLayerGeometry(in);
    EXPECT_FLOAT_EQ(g.tileWidth, 100.0f);
    EXPECT_FLOAT_EQ(g.tileHeight, 50.0f);

    in.sizeX = LayerSizeValue{false, 30};
    in.sizeY = LayerSizeValue{false, 70};
    g = computeLayerGeometry(in);
    EXPECT_FLOAT_EQ(g.tileWidth, 30.0f);
    EXPECT_FLOAT_EQ(g.tileHeight, 70.0f);

    // auto auto without an intrinsic size: the area
    g = computeLayerGeometry(base(0, 0));
    EXPECT_FLOAT_EQ(g.tileWidth, 100.0f);
    EXPECT_FLOAT_EQ(g.tileHeight, 100.0f);

    // a zero size draws nothing
    in.sizeX = LayerSizeValue{false, 0};
    EXPECT_TRUE(computeLayerGeometry(in).quads.empty());
}

TEST(CompatLayerMath, RoundScalesToAWholeCount){
    LayerInput in = base(30, 30);
    in.repeatX = LayerRepeat::Round;   // 100 / 30 = 3.33 -> 3 tiles of 33.33
    in.repeatY = LayerRepeat::NoRepeat;
    LayerGeometry g = computeLayerGeometry(in);
    EXPECT_NEAR(g.tileWidth, 33.333f, 0.01f);
    // the other dimension is auto: it keeps the ratio
    EXPECT_NEAR(g.tileHeight, 33.333f, 0.01f);
    ASSERT_EQ(g.quads.size(), 3u);
    EXPECT_TRUE(quadIs(g.quads[2], {66.667f, 0, 33.333f, 33.333f}));

    // both round: each axis on its own (40 -> 2.5 -> 3 tiles across: 33.33; 2 down: 50)
    LayerInput both = base(40, 40);
    both.sizeX = LayerSizeValue{false, 40};
    both.sizeY = LayerSizeValue{false, 45};
    both.repeatX = both.repeatY = LayerRepeat::Round;
    g = computeLayerGeometry(both);
    EXPECT_NEAR(g.tileWidth, 33.333f, 0.01f);
    EXPECT_NEAR(g.tileHeight, 50.0f, 0.01f);
    EXPECT_EQ(g.quads.size(), 6u);

    // explicit height: not rescaled
    LayerInput fixed = base(30, 30);
    fixed.sizeY = LayerSizeValue{false, 30};
    fixed.sizeX = LayerSizeValue{false, 30};
    fixed.repeatX = LayerRepeat::Round;
    g = computeLayerGeometry(fixed);
    EXPECT_NEAR(g.tileHeight, 30.0f, 0.01f);
}

TEST(CompatLayerMath, SpaceSpreadsWholeTiles){
    LayerInput in = base(30, 30);
    in.repeatX = in.repeatY = LayerRepeat::Space;   // 3 tiles, gaps of 5
    LayerGeometry g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 9u);
    EXPECT_TRUE(quadIs(g.quads[0], {0, 0, 30, 30}));
    EXPECT_TRUE(quadIs(g.quads[1], {35, 0, 30, 30}));
    EXPECT_TRUE(quadIs(g.quads[2], {70, 0, 30, 30}));
    EXPECT_TRUE(quadIs(g.quads[8], {70, 70, 30, 30}));

    // only one fits: placed by the position, like no-repeat
    LayerInput one = base(60, 60);
    one.repeatX = one.repeatY = LayerRepeat::Space;
    one.positionX = percent(100);
    g = computeLayerGeometry(one);
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_TRUE(quadIs(g.quads[0], {40, 0, 60, 60}));
}

TEST(CompatLayerMath, OriginAndClipBoxes){
    // origin padding-box (10, 10, 80, 80), clip border-box: tiles continue into the border
    LayerInput in = base(20, 20);
    in.positioningArea = LayerRect{10, 10, 80, 80};
    in.repeatX = in.repeatY = LayerRepeat::Repeat;
    LayerGeometry g = computeLayerGeometry(in);
    ASSERT_EQ(g.quads.size(), 36u);   // starts -10, 10, ..., 90 on each axis
    EXPECT_TRUE(quadIs(g.quads[0], {0, 0, 10, 10}, 0.5f, 0.5f, 1, 1));
    EXPECT_TRUE(quadIs(g.quads[7], {10, 10, 20, 20}));

    // clip content-box (20, 20, 60, 60): nothing outside it
    in.clipArea = LayerRect{20, 20, 60, 60};
    g = computeLayerGeometry(in);
    for (const LayerQuad& quad : g.quads){
        EXPECT_GE(quad.rect.x, 20.0f - 0.001f);
        EXPECT_LE(quad.rect.right(), 80.0f + 0.001f);
    }
    ASSERT_EQ(g.quads.size(), 16u);   // tiles at 10..30 cut to 20..30, 30, 50, 70..90 cut to 70..80
    EXPECT_TRUE(quadIs(g.quads[0], {20, 20, 10, 10}, 0.5f, 0.5f, 1, 1));

    // fixed attachment: the caller passes the viewport, here at (-30, -30)
    LayerInput fixed = base(20, 20);
    fixed.positioningArea = LayerRect{-30, -30, 200, 200};
    fixed.positionX = percent(50);   // 0.5 x (200 - 20) - 30 = 60
    fixed.positionY = percent(50);
    g = computeLayerGeometry(fixed);
    ASSERT_EQ(g.quads.size(), 1u);
    EXPECT_TRUE(quadIs(g.quads[0], {60, 60, 20, 20}));
    // at the viewport's top left (0% 0%): above the element, nothing drawn
    fixed.positionX = fixed.positionY = percent(0);
    EXPECT_TRUE(computeLayerGeometry(fixed).quads.empty());
}

TEST(CompatLayerMath, TinyTilesAndQuadLimit){
    LayerInput in = base(1, 1);
    in.repeatX = in.repeatY = LayerRepeat::Repeat;
    LayerGeometry g = computeLayerGeometry(in);
    EXPECT_TRUE(g.tileClamped);
    EXPECT_FLOAT_EQ(g.tileWidth, 2.0f);
    EXPECT_EQ(g.quads.size(), 2500u);

    // a single no-repeat 1px image is drawn at its size
    g = computeLayerGeometry(base(1, 1));
    EXPECT_FALSE(g.tileClamped);
    EXPECT_FLOAT_EQ(g.tileWidth, 1.0f);

    LayerInput many = base(10, 10);
    many.positioningArea = many.clipArea = LayerRect{0, 0, 1000, 1000};
    many.repeatX = many.repeatY = LayerRepeat::Repeat;
    many.maxQuads = 100;
    g = computeLayerGeometry(many);
    EXPECT_TRUE(g.countClamped);
    EXPECT_LE(g.quads.size(), 100u);
    EXPECT_FLOAT_EQ(g.tileWidth, 100.0f);
}

TEST(CompatLayerMath, LayerSpecArguments){
    LayerSpec spec;
    std::string error;
    ASSERT_TRUE(LayerSpec::parse({"\"/ui/a.png\"", "cover", "auto", "center", "calc(100% - 10px)", "repeat", "no-repeat",
                                  "content-box", "padding-box", "fixed"}, spec, &error)) << error;
    EXPECT_EQ(spec.path, "/ui/a.png");
    EXPECT_EQ(spec.sizeMode, LayerSizeMode::Cover);
    EXPECT_FLOAT_EQ(spec.positionX.value, 50.0f);
    EXPECT_FLOAT_EQ(spec.positionY.value, 100.0f);
    EXPECT_FLOAT_EQ(spec.positionOffsetY.value, -10.0f);
    EXPECT_EQ(spec.repeatX, LayerRepeat::Repeat);
    EXPECT_EQ(spec.repeatY, LayerRepeat::NoRepeat);
    EXPECT_EQ(spec.origin, Rml::BoxArea::Content);
    EXPECT_EQ(spec.clip, Rml::BoxArea::Padding);
    EXPECT_EQ(spec.attachment, LayerAttachment::Fixed);

    ASSERT_TRUE(LayerSpec::parse({"a.png", "50%", "2em", "right", "bottom", "space", "round", "border-box", "border-box", "local"}, spec, &error));
    EXPECT_FALSE(spec.autoX);
    EXPECT_TRUE(spec.sizeX.isPercent());
    EXPECT_EQ(spec.sizeY.unit, Rml::Unit::EM);
    EXPECT_FLOAT_EQ(spec.positionX.value, 100.0f);
    EXPECT_EQ(spec.repeatX, LayerRepeat::Space);
    EXPECT_EQ(spec.repeatY, LayerRepeat::Round);

    // calc with the length first, and 10px alone
    decorators::Length percentPart, offsetPart;
    ASSERT_TRUE(LayerSpec::parsePosition("calc(10px + 25%)", true, percentPart, offsetPart));
    EXPECT_FLOAT_EQ(percentPart.value, 25.0f);
    EXPECT_FLOAT_EQ(offsetPart.value, 10.0f);
    ASSERT_TRUE(LayerSpec::parsePosition("10px", false, percentPart, offsetPart));
    EXPECT_FLOAT_EQ(percentPart.value, 0.0f);
    EXPECT_FLOAT_EQ(offsetPart.value, 10.0f);
    EXPECT_FALSE(LayerSpec::parsePosition("top", true, percentPart, offsetPart));
    EXPECT_FALSE(LayerSpec::parsePosition("calc(10px + 5px)", true, percentPart, offsetPart));
    EXPECT_FALSE(LayerSpec::parsePosition("calc(10px * 2)", true, percentPart, offsetPart));

    EXPECT_FALSE(LayerSpec::parse({"", "auto", "auto", "0%", "0%", "repeat", "repeat", "padding-box", "border-box", "scroll"}, spec, &error));
    EXPECT_FALSE(LayerSpec::parse({"a.png", "-5px", "auto", "0%", "0%", "repeat", "repeat", "padding-box", "border-box", "scroll"}, spec, &error));
    EXPECT_FALSE(LayerSpec::parse({"a.png", "auto", "auto", "0%", "0%", "tile", "repeat", "padding-box", "border-box", "scroll"}, spec, &error));
    EXPECT_FALSE(LayerSpec::parse({"a.png", "auto", "auto", "0%", "0%", "repeat", "repeat", "margin-box", "border-box", "scroll"}, spec, &error));
    EXPECT_FALSE(LayerSpec::parse({"a.png", "auto", "auto", "0%", "0%", "repeat", "repeat", "padding-box", "border-box", "sticky"}, spec, &error));
    EXPECT_NE(error.find("sticky"), std::string::npos);
}

// ---------------------------------------------------------------- border-image nine-slice

namespace {

    BorderImageInput nineSlice(){
        // a 30 x 30 image sliced at 10 everywhere, on a 100 x 60 box with 10px borders
        BorderImageInput in;
        in.borderBox = LayerRect{0, 0, 100, 60};
        in.borderWidths = {10, 10, 10, 10};
        in.imageWidth = 30;
        in.imageHeight = 30;
        in.slice = {10, 10, 10, 10};
        in.width = {10, 10, 10, 10};
        return in;
    }

}

TEST(CompatLayerMath, BorderImageStretch){
    std::vector<LayerQuad> quads = computeBorderImageGeometry(nineSlice());
    ASSERT_EQ(quads.size(), 8u);   // corners and edges, no fill
    EXPECT_TRUE(quadIs(quads[0], {0, 0, 10, 10}, 0, 0, 1.0f / 3, 1.0f / 3));
    EXPECT_TRUE(quadIs(quads[1], {90, 0, 10, 10}, 2.0f / 3, 0, 1, 1.0f / 3));
    EXPECT_TRUE(quadIs(quads[2], {90, 50, 10, 10}, 2.0f / 3, 2.0f / 3, 1, 1));
    EXPECT_TRUE(quadIs(quads[3], {0, 50, 10, 10}, 0, 2.0f / 3, 1.0f / 3, 1));
    // top edge stretched over 80px
    EXPECT_TRUE(quadIs(quads[4], {10, 0, 80, 10}, 1.0f / 3, 0, 2.0f / 3, 1.0f / 3));
    // left edge over 40px
    EXPECT_TRUE(quadIs(quads[6], {0, 10, 10, 40}, 0, 1.0f / 3, 1.0f / 3, 2.0f / 3));

    BorderImageInput fill = nineSlice();
    fill.fill = true;
    quads = computeBorderImageGeometry(fill);
    ASSERT_EQ(quads.size(), 9u);
    EXPECT_TRUE(quadIs(quads[8], {10, 10, 80, 40}, 1.0f / 3, 1.0f / 3, 2.0f / 3, 2.0f / 3));
}

TEST(CompatLayerMath, BorderImageRepeatRoundSpace){
    // top edge 80px, tile 10px (slice 10 scaled by 10/10)
    BorderImageInput in = nineSlice();
    in.repeatX = BorderImageRepeat::Round;   // 8 tiles of 10
    std::vector<LayerQuad> quads = computeBorderImageGeometry(in);
    size_t top = 0;
    for (const LayerQuad& quad : quads)
        top += (quad.rect.y == 0.0f && quad.rect.x >= 10.0f && quad.rect.right() <= 90.0f) ? 1 : 0;
    EXPECT_EQ(top, 8u);

    // repeat: tiles centred, cut at both ends (75px: 7.5 tiles -> halves at the ends)
    in = nineSlice();
    in.borderBox.width = 95;
    in.repeatX = BorderImageRepeat::Repeat;
    quads = computeBorderImageGeometry(in);
    std::vector<LayerQuad> topQuads;
    for (const LayerQuad& quad : quads){
        if (quad.rect.y == 0.0f && quad.rect.x >= 10.0f && quad.rect.right() <= 85.0f + 0.001f)
            topQuads.push_back(quad);
    }
    ASSERT_EQ(topQuads.size(), 9u);
    EXPECT_TRUE(quadIs(topQuads[0], {10, 0, 2.5f, 10}, 1.0f / 3 + (7.5f / 10) / 3, 0, 2.0f / 3, 1.0f / 3));

    // space: 7 whole tiles in 75px, gaps of 0.625
    in.repeatX = BorderImageRepeat::Space;
    quads = computeBorderImageGeometry(in);
    topQuads.clear();
    for (const LayerQuad& quad : quads){
        if (quad.rect.y == 0.0f && quad.rect.x >= 10.0f && quad.rect.right() <= 85.0f + 0.001f)
            topQuads.push_back(quad);
    }
    ASSERT_EQ(topQuads.size(), 7u);
    EXPECT_NEAR(topQuads[0].rect.x, 10.625f, 0.01f);
    EXPECT_NEAR(topQuads[1].rect.x, 21.25f, 0.01f);
}

TEST(CompatLayerMath, BorderImageWidthsOutsetsAndOverlap){
    BorderImageInput in = nineSlice();
    in.outset = {5, 5, 5, 5};
    std::vector<LayerQuad> quads = computeBorderImageGeometry(in);
    ASSERT_FALSE(quads.empty());
    EXPECT_TRUE(quadIs(quads[0], {-5, -5, 10, 10}, 0, 0, 1.0f / 3, 1.0f / 3));

    // widths larger than the box scale down together: 40 + 40 > 60 tall -> 0.75
    in = nineSlice();
    in.width = {40, 40, 40, 40};
    quads = computeBorderImageGeometry(in);
    EXPECT_TRUE(quadIs(quads[0], {0, 0, 30, 30}, 0, 0, 1.0f / 3, 1.0f / 3));

    // no image size: nothing
    in.imageWidth = 0;
    EXPECT_TRUE(computeBorderImageGeometry(in).empty());
}

TEST(CompatLayerMath, BorderImageSpecArguments){
    BorderImageSpec spec;
    std::string error;
    ASSERT_TRUE(BorderImageSpec::parse({"\"/ui/frame.png\"", "10", "20%", "10", "20%", "fill", "auto", "2", "10px", "50%",
                                        "0", "1", "4px", "0", "round", "space"}, spec, &error)) << error;
    EXPECT_EQ(spec.path, "/ui/frame.png");
    EXPECT_TRUE(spec.slice[0].isNumber());
    EXPECT_TRUE(spec.slice[1].isPercent());
    EXPECT_TRUE(spec.fill);
    EXPECT_TRUE(spec.width[0].automatic);
    EXPECT_TRUE(spec.width[1].length.isNumber());
    EXPECT_EQ(spec.width[2].length.unit, Rml::Unit::PX);
    EXPECT_TRUE(spec.width[3].length.isPercent());
    EXPECT_TRUE(spec.outset[1].isNumber());
    EXPECT_EQ(spec.repeatX, BorderImageRepeat::Round);
    EXPECT_EQ(spec.repeatY, BorderImageRepeat::Space);

    EXPECT_FALSE(BorderImageSpec::parse({"a.png", "10"}, spec, &error));
    EXPECT_FALSE(BorderImageSpec::parse({"a.png", "10px", "10", "10", "10", "fill", "1", "1", "1", "1", "0", "0", "0", "0", "stretch", "stretch"},
                                        spec, &error));
    EXPECT_FALSE(BorderImageSpec::parse({"a.png", "10", "10", "10", "10", "maybe", "1", "1", "1", "1", "0", "0", "0", "0", "stretch", "stretch"},
                                        spec, &error));
    EXPECT_FALSE(BorderImageSpec::parse({"a.png", "10", "10", "10", "10", "fill", "1", "1", "1", "1", "10%", "0", "0", "0", "stretch", "stretch"},
                                        spec, &error));
    EXPECT_FALSE(BorderImageSpec::parse({"a.png", "10", "10", "10", "10", "fill", "1", "1", "1", "1", "0", "0", "0", "0", "tile", "stretch"},
                                        spec, &error));
}
