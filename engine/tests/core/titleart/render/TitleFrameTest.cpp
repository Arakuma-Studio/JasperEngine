// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleFrame.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    class TitleFrameTest : public ::testing::Test {};

    RenderFeatures desktop() {
        RenderFeatures f;
        f.renderRGBA16F = f.blendRGBA16F = f.filterRGBA16F = true;
        f.renderRGBA32F = true;
        f.renderR32F = true;
        f.renderR16F = f.filterR16F = true;
        return f;
    }

}

TEST_F(TitleFrameTest, DocumentSpaceMapsToMemoryRows) {
    const TitleFrame frame = TitleFrame::canvas(1920, 1080, 0.5);
    EXPECT_EQ(frame.region.width, 960);
    EXPECT_EQ(frame.region.height, 540);
    // the canvas centre, and +y up becomes a smaller row (row 0 is the top of the PNG)
    TitleVec2 centre = frame.toFrame({0, 0});
    EXPECT_DOUBLE_EQ(centre.x, 480);
    EXPECT_DOUBLE_EQ(centre.y, 270);
    TitleVec2 topLeft = frame.toFrame({-960, 540});
    EXPECT_DOUBLE_EQ(topLeft.x, 0);
    EXPECT_DOUBLE_EQ(topLeft.y, 0);
    TitleVec2 up = frame.toFrame({0, 100});
    EXPECT_LT(up.y, centre.y);
    TitleVec2 back = frame.toDocument(up);
    EXPECT_DOUBLE_EQ(back.x, 0);
    EXPECT_DOUBLE_EQ(back.y, 100);
    // a region of interest moves the origin
    TitleFrame roi = frame;
    roi.region = {64, 128, 256, 256};
    EXPECT_DOUBLE_EQ(roi.toFrame({0, 0}).x, 480 - 64);
    EXPECT_DOUBLE_EQ(roi.toFrame({0, 0}).y, 270 - 128);
    const TitleRectI box = frame.toFrameRect(TitleRect{-100, -50, 200, 100});
    EXPECT_EQ(box, (TitleRectI{430, 245, 100, 50}));
    EXPECT_EQ(frame.canvasToFrame({100, 100, 10, 10}), (TitleRectI{50, 50, 5, 5}));
}

TEST_F(TitleFrameTest, RegionsAlignTo64PixelsAndClipToTheFrame) {
    const TitleFrame frame = TitleFrame::canvas(1920, 1080, 1.0);
    EXPECT_EQ(alignRegionOfInterest({70, 10, 20, 20}, frame), (TitleRectI{64, 0, 64, 64}));
    EXPECT_EQ(alignRegionOfInterest({-30, -30, 40, 40}, frame), (TitleRectI{0, 0, 64, 64})) << "cut at the canvas";
    EXPECT_EQ(alignRegionOfInterest({1900, 1060, 100, 100}, frame), (TitleRectI{1856, 1024, 64, 56}));
    EXPECT_TRUE(alignRegionOfInterest({5000, 0, 10, 10}, frame).empty()) << "off the canvas: the node is skipped";
    EXPECT_TRUE(alignRegionOfInterest({}, frame).empty());
    EXPECT_EQ(unionRect({0, 0, 10, 10}, {20, 20, 5, 5}), (TitleRectI{0, 0, 25, 25}));
    EXPECT_TRUE(intersectRect({0, 0, 10, 10}, {20, 20, 5, 5}).empty());
}

TEST_F(TitleFrameTest, CapsTakeTheFloatPathWhereItRendersAndFilters) {
    const TitleArtCaps caps = TitleArtCaps::select(desktop(), 0);
    EXPECT_TRUE(caps.floatPath);
    EXPECT_TRUE(caps.floatBlend);
    EXPECT_STREQ(caps.pathName(), "rgba16f");
    EXPECT_EQ(caps.format(TitleBuffer::Accumulator).format, ColorFormat::RGBA16F);
    EXPECT_EQ(caps.format(TitleBuffer::Seeds).format, ColorFormat::RGBA32F);
    EXPECT_EQ(caps.format(TitleBuffer::Seeds).filter, TextureFilter::NEAREST);
    EXPECT_EQ(caps.format(TitleBuffer::Distance).format, ColorFormat::R16F);
    EXPECT_EQ(caps.format(TitleBuffer::Height).format, ColorFormat::R32F);
    EXPECT_EQ(caps.format(TitleBuffer::Satin).format, ColorFormat::R16F);
    EXPECT_EQ(caps.format(TitleBuffer::Coverage).format, ColorFormat::RED);
}

TEST_F(TitleFrameTest, TestFlagsForceTheFallbacks) {
    const TitleArtCaps rgba8 = TitleArtCaps::select(desktop(), TITLE_ART_TEST_NO_FLOAT_TARGETS);
    EXPECT_FALSE(rgba8.floatPath);
    EXPECT_STREQ(rgba8.pathName(), "rgba8");
    EXPECT_EQ(rgba8.format(TitleBuffer::Accumulator).format, ColorFormat::SRGB8A8);
    EXPECT_EQ(rgba8.format(TitleBuffer::Accumulator).encoding, TitleEncoding::SrgbPremultiplied);
    EXPECT_EQ(rgba8.format(TitleBuffer::Seeds).encoding, TitleEncoding::Fixed12_4);
    EXPECT_EQ(rgba8.format(TitleBuffer::Distance).encoding, TitleEncoding::Fixed16);
    EXPECT_EQ(rgba8.format(TitleBuffer::Height).encoding, TitleEncoding::Fixed16);
    EXPECT_EQ(rgba8.format(TitleBuffer::Satin).format, ColorFormat::RED);
    EXPECT_EQ(rgba8.format(TitleBuffer::ToneLut).format, ColorFormat::RGBA);

    const TitleArtCaps noR16 = TitleArtCaps::select(desktop(), TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F);
    EXPECT_TRUE(noR16.floatPath);
    EXPECT_EQ(noR16.format(TitleBuffer::Seeds).format, ColorFormat::RGBA);
    EXPECT_EQ(noR16.format(TitleBuffer::Distance).format, ColorFormat::RGBA);
    EXPECT_EQ(noR16.format(TitleBuffer::Height).format, ColorFormat::R32F);

    const TitleArtCaps noBlend = TitleArtCaps::select(desktop(), TITLE_ART_TEST_NO_FLOAT_BLEND);
    EXPECT_TRUE(noBlend.floatPath);
    EXPECT_FALSE(noBlend.floatBlend) << "Normal and Add take the shader path";

    RenderFeatures webgl;   // GLES3 without EXT_color_buffer_float
    const TitleArtCaps web = TitleArtCaps::select(webgl, 0);
    EXPECT_FALSE(web.floatPath);
    EXPECT_EQ(web.format(TitleBuffer::Seeds).format, ColorFormat::RGBA);
}
