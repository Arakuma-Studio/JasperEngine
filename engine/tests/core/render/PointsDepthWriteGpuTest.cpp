// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Points with Depth Write Auto write depth only when opaque (Doriax 0141520ca;
// docs/superpowers/specs/2026-11-04-render-correctness-design.md 6.3). A transparent Points
// quad wrote depth over its whole quad, its transparent texels too, so points and transparent
// objects drawn after it were cut out where its quads were. On and Off stay as set. A texture
// gaining alpha moves the component to the transparent queue and gives it pipelines without
// depth write. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/PointsComponent.h"
#include "object/Camera.h"
#include "object/Points.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // a 4x4 image of one texel value
    void registerFlatImage(const std::string& id, std::array<uint8_t, 4> texel){
        TextureDataPool::setGenerator(id, [texel]() -> TextureData {
            const int size = 4 * 4 * 4;
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
            if (!pixels)
                return TextureData();
            for (int i = 0; i < 16; i++)
                std::copy(texel.begin(), texel.end(), pixels + i * 4);
            return TextureData(4, 4, (unsigned int)size, ColorFormat::RGBA, 4, pixels);
        });
    }

    std::array<int, 3> centerPixel(){
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
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            })) << "the scene never finished loading";
            GpuEnvironment::drawFrames(3);
            return centerPixel();
        }

        // the generated image `id` (TextureDataPool), as an Inspector or script swap gives it
        void useTexture(const std::string& id){
            frontComponent().texture.setId(id);
            frontComponent().needUpdateTexture = true;
        }

        PointsComponent& frontComponent(){
            return scene.getComponent<PointsComponent>(front.getEntity());
        }
    };

}

TEST_F(PointsDepthWriteGpu, TransparentPointsDoNotHideWhatIsDrawnAfter){
    JASPER_REQUIRE_GPU();
    // its texels are all transparent: it draws nothing, so B shows
    registerFlatImage("rc-points-clear", {255, 255, 255, 0});
    useTexture("rc-points-clear");
    const std::array<int, 3> center = settledCenter();
    ASSERT_TRUE(frontComponent().isTransparent()) << "Auto Transparency did not find the texture's alpha";
    EXPECT_GT(center[1], 128) << "the transparent quad in front wrote depth and cut out the points behind it";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(PointsDepthWriteGpu, DepthWriteOnKeepsWriting){
    JASPER_REQUIRE_GPU();
    registerFlatImage("rc-points-clear-on", {255, 255, 255, 0});
    useTexture("rc-points-clear-on");
    frontComponent().depthWrite = MaterialDepthWrite::ENABLED;
    const std::array<int, 3> center = settledCenter();
    EXPECT_LT(center[1], 64) << "Depth Write On no longer writes depth";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(PointsDepthWriteGpu, ATextureGainingAlphaMovesThePointsToTheTransparentQueue){
    JASPER_REQUIRE_GPU();
    registerFlatImage("rc-points-opaque", {255, 255, 255, 255});
    registerFlatImage("rc-points-clear-later", {255, 255, 255, 0});
    useTexture("rc-points-opaque");
    settledCenter();
    ASSERT_FALSE(frontComponent().isTransparent());

    useTexture("rc-points-clear-later");
    const std::array<int, 3> center = settledCenter();
    EXPECT_TRUE(frontComponent().isTransparent()) << "the new texture's alpha did not reach the queue";
    EXPECT_GT(center[1], 128) << "the points kept the depth write of their opaque texture";
    EXPECT_NO_ENGINE_ERRORS();
}
