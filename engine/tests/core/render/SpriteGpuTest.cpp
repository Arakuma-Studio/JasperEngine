// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S94 F29 (AC29): a Sprite's Flip X draws its picture mirrored about its pivot. Side by side, an
// asymmetric sprite and its flipped twin (pivot at their bottom-left: the twin draws on the other
// side of its origin): the twin's pixels are the first's mirrored, and the frame is the golden
// sprite_flip_x.png. Flip X off draws the same number of draws as before (nothing added).
// glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "render/RenderStats.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int FLIP_VIEW = 128;

    // 8 x 8, an L in the left columns and a dot at the top right: a mirror shows
    const std::vector<uint8_t>& flipPixels(){
        static const std::vector<uint8_t> pixels = [](){
            std::vector<uint8_t> rgba(8 * 8 * 4);
            for (int y = 0; y < 8; y++){
                for (int x = 0; x < 8; x++){
                    uint8_t* p = &rgba[(y * 8 + x) * 4];
                    const bool ell = x < 2 || y >= 6;
                    const bool dot = x >= 6 && y < 2;
                    p[0] = ell ? 250 : (dot ? 30 : 20);
                    p[1] = ell ? 200 : (dot ? 220 : 20);
                    p[2] = dot ? 250 : 40;
                    p[3] = 255;
                }
            }
            return rgba;
        }();
        return pixels;
    }

    class SpriteGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Sprite>> sprites;

        void SetUp() override{
            GpuEnvironment::setView(FLIP_VIEW, FLIP_VIEW, FLIP_VIEW, FLIP_VIEW, Scaling::STRETCH);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            Engine::setScene(&scene);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(0.0f, (float)FLIP_VIEW, 0.0f, (float)FLIP_VIEW, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setDepthTest(false);
            camera.setTransparentSort(false);
            camera.activate();
        }

        void TearDown() override{
            sprites.clear();
            Engine::removeScene(&scene);
            GpuEnvironment::resetView();
        }

        Sprite& sprite(float x, float y, bool flipX){
            sprites.push_back(std::make_unique<Sprite>(&scene));
            Sprite& s = *sprites.back();
            s.setTexture("sprite-gpu|flip", TextureData(8, 8, 8 * 8 * 4, ColorFormat::RGBA, 4, (void*)flipPixels().data()));
            Material material = s.getMaterial();
            material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
            material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
            s.setMaterial(material);
            s.setSize(32, 32);
            s.setPivotPreset(PivotPreset::BOTTOM_LEFT);
            s.setPosition(x, y, 0.0f);
            s.setFlipX(flipX);
            return s;
        }

        std::vector<uint8_t> image(uint32_t* draws = nullptr){
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
            GpuEnvironment::drawFrames(3);
            if (draws)
                *draws = Engine::getFrameStats().drawCalls;
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }
    };

}

TEST_F(SpriteGpu, FlipX){
    JASPER_REQUIRE_GPU();
    // the first spans x 16..48; the twin's origin at 112 draws it at 80..112, mirrored
    sprite(16.0f, 48.0f, false);
    sprite(112.0f, 48.0f, true);
    const std::vector<uint8_t> rgba = image();
    ASSERT_EQ(rgba.size(), (size_t)FLIP_VIEW * FLIP_VIEW * 4);
    size_t compared = 0, differing = 0, lit = 0;
    for (int y = 0; y < FLIP_VIEW; y++){
        for (int x = 0; x < 32; x++){
            const uint8_t* a = &rgba[((size_t)y * FLIP_VIEW + 16 + x) * 4];
            const uint8_t* b = &rgba[((size_t)y * FLIP_VIEW + 111 - x) * 4];
            compared++;
            lit += (a[0] | a[1] | a[2]) ? 1 : 0;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2])
                differing++;
        }
    }
    EXPECT_GT(lit, 900u);
    EXPECT_EQ(differing, 0u) << "of " << compared;
    // the two pictures differ (the art is asymmetric): a mirror, not a copy
    size_t same = 0;
    for (int y = 0; y < FLIP_VIEW; y++){
        for (int x = 0; x < 32; x++){
            const uint8_t* a = &rgba[((size_t)y * FLIP_VIEW + 16 + x) * 4];
            const uint8_t* b = &rgba[((size_t)y * FLIP_VIEW + 80 + x) * 4];
            same += (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ? 1 : 0;
        }
    }
    EXPECT_LT(same, compared);
    CompareOptions exact;
    exact.pixelThreshold = 0;
    exact.maxBadFraction = 0.0;
    exact.maxMeanDiff = 0.0;
    EXPECT_GOLDEN_OPTIONS("sprite_flip_x", exact);
    EXPECT_NO_ENGINE_ERRORS();
}

// F-PF1: Flip X adds no draw (it is the same quad, mirrored)
TEST_F(SpriteGpu, FlipXAddsNothing){
    JASPER_REQUIRE_GPU();
    Sprite& s = sprite(16.0f, 48.0f, false);
    uint32_t off = 0, on = 0;
    image(&off);
    s.setFlipX(true);
    image(&on);
    EXPECT_EQ(on, off);
    EXPECT_NO_ENGINE_ERRORS();
}
