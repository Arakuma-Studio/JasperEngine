// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC1 (R1): the integer layout of a base image on a screen. The 40 rows were computed from the
// rules of R1 by an independent implementation (screens 300x100 to 3840x2160, references 320x180,
// 384x216 and 256x224, the three modes, forced scales 0/1/3, smoothing, PPU 1 and 16).

#include "pixelart/PixelPerfectLayout.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    struct LayoutRow {
        int screenWidth, screenHeight, referenceWidth, referenceHeight;
        PixelUpscaleMode mode;
        int forcedScale;
        bool smoothing;
        int ppu;
        // expected
        int scale, baseWidth, baseHeight, targetWidth, targetHeight;
        int viewport[4];
        float canvasWidth, canvasHeight;
        bool cropped;
    };

    const LayoutRow kLayoutRows[] = {
        {320, 180, 320, 180, PixelUpscaleMode::INTEGER, 0, false, 1,   1, 320, 180, 320, 180, {0, 0, 320, 180}, 320.0f, 180.0f, false},
        {320, 180, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 0, true, 1,   1, 384, 216, 386, 218, {-32, -18, 384, 216}, 384.0f, 216.0f, true},
        {320, 180, 256, 224, PixelUpscaleMode::STRETCH_FILL, 0, false, 1,   1, 256, 224, 256, 224, {32, -22, 256, 224}, 256.0f, 224.0f, true},
        {640, 360, 320, 180, PixelUpscaleMode::INTEGER, 1, true, 16,   1, 320, 180, 322, 182, {160, 90, 320, 180}, 20.0f, 11.25f, false},
        {640, 360, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 1, false, 1,   1, 640, 360, 640, 360, {0, 0, 640, 360}, 640.0f, 360.0f, false},
        {640, 360, 256, 224, PixelUpscaleMode::STRETCH_FILL, 0, true, 1,   1, 256, 224, 258, 226, {114, 0, 411, 360}, 256.0f, 224.0f, false},
        {1280, 720, 320, 180, PixelUpscaleMode::INTEGER, 3, false, 1,   3, 320, 180, 320, 180, {160, 90, 960, 540}, 320.0f, 180.0f, false},
        {1280, 720, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 3, true, 16,   3, 426, 240, 428, 242, {1, 0, 1278, 720}, 26.625f, 15.0f, false},
        {1280, 720, 256, 224, PixelUpscaleMode::STRETCH_FILL, 3, false, 1,   3, 256, 224, 256, 224, {228, 0, 823, 720}, 256.0f, 224.0f, false},
        {1920, 1080, 320, 180, PixelUpscaleMode::INTEGER, 0, true, 1,   6, 320, 180, 322, 182, {0, 0, 1920, 1080}, 320.0f, 180.0f, false},
        {1920, 1080, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 0, false, 1,   5, 384, 216, 384, 216, {0, 0, 1920, 1080}, 384.0f, 216.0f, false},
        {1920, 1080, 256, 224, PixelUpscaleMode::STRETCH_FILL, 0, true, 16,   4, 256, 224, 258, 226, {343, 0, 1234, 1080}, 16.0f, 14.0f, false},
        {2560, 1440, 320, 180, PixelUpscaleMode::INTEGER, 1, false, 1,   1, 320, 180, 320, 180, {1120, 630, 320, 180}, 320.0f, 180.0f, false},
        {2560, 1440, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 1, true, 1,   1, 2560, 1440, 2562, 1442, {0, 0, 2560, 1440}, 2560.0f, 1440.0f, false},
        {2560, 1440, 256, 224, PixelUpscaleMode::STRETCH_FILL, 1, false, 1,   1, 256, 224, 256, 224, {457, 0, 1646, 1440}, 256.0f, 224.0f, false},
        {3840, 2160, 320, 180, PixelUpscaleMode::INTEGER, 0, true, 16,   12, 320, 180, 322, 182, {0, 0, 3840, 2160}, 20.0f, 11.25f, false},
        {3840, 2160, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 3, false, 1,   3, 1280, 720, 1280, 720, {0, 0, 3840, 2160}, 1280.0f, 720.0f, false},
        {3840, 2160, 256, 224, PixelUpscaleMode::STRETCH_FILL, 3, true, 1,   3, 256, 224, 258, 226, {685, 0, 2469, 2160}, 256.0f, 224.0f, false},
        {1366, 768, 320, 180, PixelUpscaleMode::INTEGER, 0, false, 1,   4, 320, 180, 320, 180, {43, 24, 1280, 720}, 320.0f, 180.0f, false},
        {1366, 768, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 0, true, 16,   3, 455, 256, 457, 258, {0, 0, 1365, 768}, 28.4375f, 16.0f, false},
        {1366, 768, 256, 224, PixelUpscaleMode::STRETCH_FILL, 0, false, 1,   3, 256, 224, 256, 224, {244, 0, 878, 768}, 256.0f, 224.0f, false},
        {2400, 1080, 320, 180, PixelUpscaleMode::INTEGER, 1, true, 1,   1, 320, 180, 322, 182, {1040, 450, 320, 180}, 320.0f, 180.0f, false},
        {2400, 1080, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 1, false, 1,   1, 2400, 1080, 2400, 1080, {0, 0, 2400, 1080}, 2400.0f, 1080.0f, false},
        {2400, 1080, 256, 224, PixelUpscaleMode::STRETCH_FILL, 1, true, 16,   1, 256, 224, 258, 226, {583, 0, 1234, 1080}, 16.0f, 14.0f, false},
        {1280, 800, 320, 180, PixelUpscaleMode::INTEGER, 3, false, 1,   3, 320, 180, 320, 180, {160, 130, 960, 540}, 320.0f, 180.0f, false},
        {1280, 800, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 0, true, 1,   3, 426, 266, 428, 268, {1, 1, 1278, 798}, 426.0f, 266.0f, false},
        {1280, 800, 256, 224, PixelUpscaleMode::STRETCH_FILL, 3, false, 1,   3, 256, 224, 256, 224, {183, 0, 914, 800}, 256.0f, 224.0f, false},
        {800, 600, 320, 180, PixelUpscaleMode::INTEGER, 0, true, 16,   2, 320, 180, 322, 182, {80, 120, 640, 360}, 20.0f, 11.25f, false},
        {800, 600, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 0, false, 1,   2, 400, 300, 400, 300, {0, 0, 800, 600}, 400.0f, 300.0f, false},
        {800, 600, 256, 224, PixelUpscaleMode::STRETCH_FILL, 0, true, 1,   2, 256, 224, 258, 226, {57, 0, 686, 600}, 256.0f, 224.0f, false},
        {300, 100, 320, 180, PixelUpscaleMode::INTEGER, 0, false, 1,   1, 320, 180, 320, 180, {-10, -40, 320, 180}, 320.0f, 180.0f, true},
        {300, 100, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 1, true, 16,   1, 384, 216, 386, 218, {-42, -58, 384, 216}, 24.0f, 13.5f, true},
        {300, 100, 256, 224, PixelUpscaleMode::STRETCH_FILL, 1, false, 1,   1, 256, 224, 256, 224, {22, -62, 256, 224}, 256.0f, 224.0f, true},
        {1366, 768, 320, 180, PixelUpscaleMode::INTEGER, 0, false, 1,   4, 320, 180, 320, 180, {43, 24, 1280, 720}, 320.0f, 180.0f, false},
        {1366, 768, 320, 180, PixelUpscaleMode::INTEGER_EXPAND, 0, false, 1,   4, 341, 192, 341, 192, {1, 0, 1364, 768}, 341.0f, 192.0f, false},
        {1366, 768, 320, 180, PixelUpscaleMode::STRETCH_FILL, 0, false, 1,   4, 320, 180, 320, 180, {0, 0, 1365, 768}, 320.0f, 180.0f, false},
        {1920, 1080, 320, 180, PixelUpscaleMode::INTEGER, 0, true, 16,   6, 320, 180, 322, 182, {0, 0, 1920, 1080}, 20.0f, 11.25f, false},
        {300, 100, 320, 180, PixelUpscaleMode::INTEGER_EXPAND, 0, false, 1,   1, 320, 180, 320, 180, {-10, -40, 320, 180}, 320.0f, 180.0f, true},
        {2400, 1080, 384, 216, PixelUpscaleMode::INTEGER_EXPAND, 3, true, 1,   3, 800, 360, 802, 362, {0, 0, 2400, 1080}, 800.0f, 360.0f, false},
        {1280, 800, 256, 224, PixelUpscaleMode::STRETCH_FILL, 0, true, 16,   3, 256, 224, 258, 226, {183, 0, 914, 800}, 16.0f, 14.0f, false},
    };

}

TEST(PixelPerfectLayoutTest, Table){
    static_assert(sizeof(kLayoutRows) / sizeof(kLayoutRows[0]) == 40);
    for (const LayoutRow& row : kLayoutRows){
        SCOPED_TRACE(::testing::Message() << row.screenWidth << "x" << row.screenHeight << " ref " << row.referenceWidth << "x"
                     << row.referenceHeight << " mode " << (int)row.mode << " forced " << row.forcedScale << " smoothing "
                     << row.smoothing << " ppu " << row.ppu);
        const PixelPerfectLayout layout = computePixelPerfectLayout(row.screenWidth, row.screenHeight, row.referenceWidth,
            row.referenceHeight, row.mode, row.forcedScale, row.smoothing, row.ppu);
        EXPECT_EQ(layout.scale, row.scale);
        EXPECT_EQ(layout.baseWidth, row.baseWidth);
        EXPECT_EQ(layout.baseHeight, row.baseHeight);
        EXPECT_EQ(layout.targetWidth, row.targetWidth);
        EXPECT_EQ(layout.targetHeight, row.targetHeight);
        EXPECT_EQ(layout.viewport.getX(), (float)row.viewport[0]);
        EXPECT_EQ(layout.viewport.getY(), (float)row.viewport[1]);
        EXPECT_EQ(layout.viewport.getWidth(), (float)row.viewport[2]);
        EXPECT_EQ(layout.viewport.getHeight(), (float)row.viewport[3]);
        EXPECT_FLOAT_EQ(layout.canvasWidth, row.canvasWidth);
        EXPECT_FLOAT_EQ(layout.canvasHeight, row.canvasHeight);
        EXPECT_EQ(layout.cropped, row.cropped);
    }
}

// every base pixel is k x k screen pixels in the integer modes: the viewport is a whole multiple
// of the base, and the bars of Integer Expand are under k pixels
TEST(PixelPerfectLayoutTest, IntegerViewportsAreWholeMultiples){
    for (int width = 300; width <= 2600; width += 37){
        for (int height = 100; height <= 1500; height += 41){
            for (PixelUpscaleMode mode : {PixelUpscaleMode::INTEGER, PixelUpscaleMode::INTEGER_EXPAND}){
                const PixelPerfectLayout layout = computePixelPerfectLayout(width, height, 320, 180, mode, 0, false, 1);
                ASSERT_EQ(layout.viewport.getWidth(), (float)(layout.scale * layout.baseWidth));
                ASSERT_EQ(layout.viewport.getHeight(), (float)(layout.scale * layout.baseHeight));
                if (mode == PixelUpscaleMode::INTEGER_EXPAND && !layout.cropped){
                    ASSERT_LT(width - (int)layout.viewport.getWidth(), layout.scale);
                    ASSERT_LT(height - (int)layout.viewport.getHeight(), layout.scale);
                }
            }
        }
    }
}

TEST(PixelPerfectLayoutTest, EmptyInputsGiveAnEmptyLayout){
    const PixelPerfectLayout none = computePixelPerfectLayout(0, 0, 320, 180, PixelUpscaleMode::INTEGER, 0, false, 1);
    EXPECT_EQ(none.baseWidth, 0);
    EXPECT_EQ(none.viewport.getWidth(), 0.0f);
    const PixelPerfectLayout noReference = computePixelPerfectLayout(1280, 720, 0, 0, PixelUpscaleMode::INTEGER, 0, false, 1);
    EXPECT_EQ(noReference.baseWidth, 0);
    EXPECT_EQ(fitScale(1280, 720, 320, 180), 4);
    EXPECT_EQ(fitScale(100, 100, 320, 180), 1);
}
