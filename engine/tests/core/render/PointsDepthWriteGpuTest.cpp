// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Points with Depth Write Auto write depth only when opaque (Doriax 0141520ca;
// docs/superpowers/specs/2026-11-04-render-correctness-design.md 6.3). A transparent Points
// quad wrote depth over its whole quad, its transparent texels too, so points and transparent
// objects drawn after it were cut out where its quads were. On and Off stay as set. Points that
// turn transparent after their load get pipelines without depth write. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/PointsComponent.h"
#include "object/Camera.h"
#include "object/Points.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "TempDir.h"

#include "stb_image_write.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::array<int, 3> pointsCenterPixel(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        const int w = (int)target.getWidth(), h = (int)target.getHeight();
        if (rgba.size() < (size_t)(w * h * 4))
            return {0, 0, 0};
        const size_t i = ((size_t)(h / 2) * w + w / 2) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }

    // A, a Points component in front (made first, so drawn first: the camera does not sort the
    // transparent queue), and B, a green transparent one behind it
    class PointsDepthWriteGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Points front{&scene};
        Points back{&scene};
        TempDir dir;

        void SetUp() override{
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 0.0f, 4.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setTransparentSort(false);
            camera.activate();
            Engine::setScene(&scene);

            front.addPoint(Vector3(0.0f, 0.0f, 1.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 1.0f);
            back.addPoint(Vector3(0.0f, 0.0f, 0.0f), Vector4(0.0f, 1.0f, 0.0f, 1.0f), 1.0f);
            back.setTransparent(true);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        std::array<int, 3> settledCenter(){
            Scene* current = &scene;
            // the image loads off the main thread: the points draw a white stand-in until then
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded() && TextureDataPool::getLoadingCount() == 0;
            }, 600)) << "the scene never finished loading";
            // the image's alpha reloads the points' pipelines on the next update
            GpuEnvironment::drawFrames(3);
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            })) << "the points never finished their reload";
            GpuEnvironment::drawFrames(2);
            return pointsCenterPixel();
        }

        // a 4x4 PNG, opaque white with its centre 2x2 of the given alpha
        void registerPointsImage(const std::string& name, uint8_t centreAlpha){
            std::vector<uint8_t> pixels(4 * 4 * 4, 255);
            for (int y = 1; y < 3; y++){
                for (int x = 1; x < 3; x++)
                    pixels[(y * 4 + x) * 4 + 3] = centreAlpha;
            }
            ASSERT_TRUE(stbi_write_png(dir.file(name + ".png").string().c_str(), 4, 4, 4, pixels.data(), 4 * 4));
        }

        // the image as the Inspector or a script gives it
        void useTexture(const std::string& name){
            front.setTexture(dir.file(name + ".png").string());
        }

        PointsComponent& frontComponent(){
            return scene.getComponent<PointsComponent>(front.getEntity());
        }
    };

}

TEST_F(PointsDepthWriteGpu, TransparentPointsDoNotHideWhatIsDrawnAfter){
    JASPER_REQUIRE_GPU();
    // transparent, its image's centre clear: there B shows
    registerPointsImage("rc-points-clear", 0);
    useTexture("rc-points-clear");
    front.setTransparent(true);
    const std::array<int, 3> center = settledCenter();
    EXPECT_GT(center[1], 128) << "the transparent quad in front wrote depth and cut out the points behind it";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(PointsDepthWriteGpu, DepthWriteOnKeepsWriting){
    JASPER_REQUIRE_GPU();
    registerPointsImage("rc-points-clear-on", 0);
    useTexture("rc-points-clear-on");
    front.setTransparent(true);
    frontComponent().depthWrite = MaterialDepthWrite::ENABLED;
    const std::array<int, 3> center = settledCenter();
    EXPECT_LT(center[1], 64) << "Depth Write On no longer writes depth";
    EXPECT_NO_ENGINE_ERRORS();
}

// loaded opaque (writing depth), then made transparent: its pipelines are made again without it
TEST_F(PointsDepthWriteGpu, TurningTransparentOnStopsTheDepthWrite){
    JASPER_REQUIRE_GPU();
    registerPointsImage("rc-points-later", 0);
    useTexture("rc-points-later");
    frontComponent().autoTransparency = false;
    const std::array<int, 3> opaque = settledCenter();
    ASSERT_FALSE(frontComponent().isTransparent());
    EXPECT_LT(opaque[1], 64) << "the opaque points do not write depth";

    front.setTransparent(true);
    const std::array<int, 3> center = settledCenter();
    EXPECT_GT(center[1], 128) << "the points kept the depth write they were loaded with";
    EXPECT_NO_ENGINE_ERRORS();
}
