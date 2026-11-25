// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The one answer to "is the scene's 2D ambient bright" (docs/specs/S78-sprite-normals-improvements.md
// R15): intensity x the largest linear colour channel, bright from 0.8.

#include <gtest/gtest.h>

#include "Scene.h"
#include "util/Ambient2D.h"

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(Ambient2D, BrightnessIsIntensityTimesTheLargestLinearChannel) {
    Scene scene;
    scene.setAmbientLight2D(0.8f, Vector3(1.0f, 1.0f, 1.0f));
    EXPECT_FLOAT_EQ(ambientLight2DBrightness(scene), 0.8f);
    EXPECT_TRUE(isAmbientLight2DBright(scene)) << "white at 0.8";

    scene.setAmbientLight2D(0.7f, Vector3(1.0f, 1.0f, 1.0f));
    EXPECT_FALSE(isAmbientLight2DBright(scene)) << "white at 0.7";

    // the night of Darken Ambient: its brightest linear channel is about 0.09
    scene.setAmbientLight2D(1.0f, Vector3(kDarkAmbient2DColor[0], kDarkAmbient2DColor[1], kDarkAmbient2DColor[2]));
    EXPECT_LT(ambientLight2DBrightness(scene), 0.1f);
    EXPECT_FALSE(isAmbientLight2DBright(scene)) << "dark blue at 1";

    // one bright channel is enough; sRGB 0.5 grey is linear 0.21, so it takes intensity 4
    scene.setAmbientLight2D(1.0f, Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_TRUE(isAmbientLight2DBright(scene)) << "pure blue at 1";
    scene.setAmbientLight2D(4.0f, Vector3(0.5f, 0.5f, 0.5f));
    EXPECT_TRUE(isAmbientLight2DBright(scene)) << "grey at 4";
    scene.setAmbientLight2D(3.0f, Vector3(0.5f, 0.5f, 0.5f));
    EXPECT_FALSE(isAmbientLight2DBright(scene)) << "grey at 3";
}
