// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The first goldens of the 3D renderer: a scene drawn into GpuEnvironment::target() on the
// headless device (Linux, Xvfb; glcore, and Vulkan on lavapipe) and compared with
// engine/tests/core/golden/. JASPER_TEST_UPDATE_GOLDEN=1 rewrites them (on glcore); Vulkan must
// match the same images. Rendering specs (S10-S13) add their goldens next to these.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // a scene with a perspective camera looking at the origin from above and in front, set as
    // the engine's scene while it lives
    class RenderGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 3.0f, 5.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }
    };

}

TEST_F(RenderGpu, UnlitColouredPlane){
    JASPER_REQUIRE_GPU();
    Primitive plane(&scene);
    plane.createPlane(4.0f, 4.0f);
    plane.setColor(0.9f, 0.35f, 0.1f);
    plane.setReceiveLights(false);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_GOLDEN("render_unlit_plane");
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {
    void litCube(Primitive& cube, Light& sun){
        cube.createBox(1.5f, 1.5f, 1.5f);
        cube.setColor(0.2f, 0.6f, 0.9f);
        cube.setRotation(0.0f, 30.0f, 0.0f);
        sun.setType(LightType::DIRECTIONAL);
        sun.setDirection(-0.4f, -1.0f, -0.6f);
        sun.setIntensity(1.5f);
        sun.setShadows(false);
    }
}

// Lit Quality Standard is the shader of before S11: the golden written before it still matches
// (a Scene() is High since S11, docs/specs/S11-materials-lighting.md R3)
TEST_F(RenderGpu, LitCube){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::STANDARD);
    Primitive cube(&scene);
    Light sun(&scene);
    litCube(cube, sun);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_GOLDEN("render_lit_cube");
    EXPECT_NO_ENGINE_ERRORS();
}

// High: specular anti-aliasing and the direct light's multiscatter compensation
TEST_F(RenderGpu, LitCubeHigh){
    JASPER_REQUIRE_GPU();
    ASSERT_EQ(scene.getLitQuality(), LitQuality::HIGH);
    Primitive cube(&scene);
    Light sun(&scene);
    litCube(cube, sun);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_GOLDEN("render_lit_cube_high");
    EXPECT_NO_ENGINE_ERRORS();
}
