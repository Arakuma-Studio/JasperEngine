// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R17: terrain outside the view casts into the shadow maps. A ridge behind the camera, the sun
// behind the ridge: the ground in view is in its shadow (AC24, AC25). Off draws what it drew.

#include "TerrainGpuFixture.h"

#include "LogCapture.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class TerrainShadowsGpu : public TerrainGpuFixture {
    protected:
        // a ridge along z = 45, 40 m tall; the camera at z = 20 looks down and away from it (-z), so the
        // ridge is outside the view, and a low sun behind the ridge shadows the ground in front of it
        void ridgeBehindTheCamera(){
            makeTerrain(40.0f, [](float, float z){
                const float d = (z - 45.0f) / 6.0f;
                return std::fmax(0.0f, 1.0f - d * d);
            });
            terrain->setColor(0.8f, 0.8f, 0.8f);
            sun.setDirection(0.0f, -0.6f, -1.0f);
            sun.setIntensity(3.0f);
            sun.setShadows(true);
            camera.setPosition(0.0f, 6.0f, 20.0f);
            camera.setTarget(0.0f, 0.0f, 10.0f);
        }

        int groundBrightness(const std::string& name = std::string()){
            EXPECT_TRUE(render());
            std::vector<uint8_t> frame;
            EXPECT_TRUE(readFrame(frame));
            if (!name.empty()){
                writePng(getOutputDirectory() + "/" + name + ".png", frame, FRAME_WIDTH, FRAME_HEIGHT);
            }
            const size_t pixel = (static_cast<size_t>(FRAME_HEIGHT * 3 / 4) * FRAME_WIDTH + FRAME_WIDTH / 2) * 4;
            return frame[pixel + 1];
        }

        int brightnessWithoutCaster(){
            sun.setShadows(false);
            const int lit = groundBrightness();
            sun.setShadows(true);
            return lit;
        }
    };

}

TEST_F(TerrainShadowsGpu, OffscreenRidgeCastsShadow){
    JASPER_REQUIRE_GPU();
    ridgeBehindTheCamera();
    const int lit = brightnessWithoutCaster();
    const int shadowed = groundBrightness("ridge_view");
    std::printf("[gpu] ridge: lit %d, with shadows %d\n", lit, shadowed);
    EXPECT_LT(shadowed, lit - 30) << "the ridge outside the view casts nothing";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(TerrainShadowsGpu, OffRestoresOldBehaviour){
    JASPER_REQUIRE_GPU();
    ridgeBehindTheCamera();
    component().offscreenShadows = TerrainOffscreenShadows::OFF;
    const int lit = brightnessWithoutCaster();
    const int shadowed = groundBrightness();
    EXPECT_NEAR(shadowed, lit, 8) << "Off draws the view's cut: nothing behind the camera casts";
}
