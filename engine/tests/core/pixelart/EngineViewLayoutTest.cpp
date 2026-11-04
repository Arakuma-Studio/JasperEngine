// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC1 (R2): the project scaling modes Integer and Integer (Expand) are the Pixel Perfect
// Camera's layout at PPU 1, with the preferred canvas as the reference.

#include "Engine.h"
#include "pixelart/PixelPerfectLayout.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(EngineViewLayoutTest, IntegerModes){
    const int screens[][2] = {{320, 180}, {1280, 720}, {1366, 768}, {1920, 1080}, {2400, 1080}, {1280, 800}, {800, 600}, {300, 100}};
    const int references[][2] = {{320, 180}, {384, 216}, {256, 224}};
    for (const auto& screen : screens){
        for (const auto& reference : references){
            for (Scaling scaling : {Scaling::INTEGER, Scaling::INTEGER_EXPAND}){
                SCOPED_TRACE(::testing::Message() << screen[0] << "x" << screen[1] << " ref " << reference[0] << "x" << reference[1]
                             << " expand " << (scaling == Scaling::INTEGER_EXPAND));
                int canvasWidth = 0, canvasHeight = 0;
                Rect viewRect;
                Engine::computeViewLayout(screen[0], screen[1], reference[0], reference[1], scaling, canvasWidth, canvasHeight, viewRect);
                const pixelart::PixelPerfectLayout layout = pixelart::computePixelPerfectLayout(screen[0], screen[1], reference[0],
                    reference[1], scaling == Scaling::INTEGER ? PixelUpscaleMode::INTEGER : PixelUpscaleMode::INTEGER_EXPAND, 0, false, 1);
                EXPECT_EQ(canvasWidth, layout.baseWidth);
                EXPECT_EQ(canvasHeight, layout.baseHeight);
                EXPECT_EQ((float)canvasWidth, layout.canvasWidth);
                EXPECT_EQ(viewRect.getX(), layout.viewport.getX());
                EXPECT_EQ(viewRect.getY(), layout.viewport.getY());
                EXPECT_EQ(viewRect.getWidth(), layout.viewport.getWidth());
                EXPECT_EQ(viewRect.getHeight(), layout.viewport.getHeight());
            }
        }
    }
}

// the other modes are untouched by the integer ones (FITWIDTH at 1366 x 768 keeps its fractional scale)
TEST(EngineViewLayoutTest, OtherModesKeepTheirLayout){
    int canvasWidth = 0, canvasHeight = 0;
    Rect viewRect;
    Engine::computeViewLayout(1366, 768, 640, 360, Scaling::LETTERBOX, canvasWidth, canvasHeight, viewRect);
    EXPECT_EQ(canvasWidth, 640);
    EXPECT_EQ(canvasHeight, 360);
    EXPECT_EQ(viewRect.getWidth(), 1366.0f);
}
