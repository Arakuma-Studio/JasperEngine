// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Dynamic Batching (docs/superpowers/specs/2026-11-04-sprite-batching-design.md): consecutive
// Sprites, small meshes and UI elements that share pipeline, textures and their unbaked values
// draw as one draw call, and the image is the one they make drawn one by one (each scene is drawn
// with Engine::setDynamicBatching on and off and compared). Also: the texture breaks a run, 2D
// cameras let a draw join an earlier run past draws it does not touch, lit sprites batch when
// their lights are the same, the depth pre-pass agrees with the batched colour pass, settled
// sprites follow direct writes, the MVP a batched sprite went without is made before it draws
// alone, and sprites of one shape share their GPU buffers (no sprite ceiling). glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Light2D.h"
#include "object/Primitive.h"
#include "object/Sprite.h"
#include "object/ui/Image.h"
#include "pool/MeshBufferPool.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int BATCH_VIEW_SIZE = 256;

    // pixels of the test textures, kept for the process (TextureData does not own them)
    std::vector<std::unique_ptr<std::vector<uint8_t>>>& batchTexturePixels(){
        static std::vector<std::unique_ptr<std::vector<uint8_t>>> pixels;
        return pixels;
    }

    // an 8x8 batchChecker of two colours; opaque or with a transparent quarter
    TextureData batchChecker(uint8_t r, uint8_t g, uint8_t b, bool cutout = false){
        auto pixels = std::make_unique<std::vector<uint8_t>>(8 * 8 * 4);
        for (int y = 0; y < 8; y++){
            for (int x = 0; x < 8; x++){
                uint8_t* p = pixels->data() + (y * 8 + x) * 4;
                const bool dark = ((x / 2) + (y / 2)) % 2 == 0;
                p[0] = dark ? r / 2 : r;
                p[1] = dark ? g / 2 : g;
                p[2] = dark ? b / 2 : b;
                p[3] = (cutout && x < 4 && y < 4) ? 0 : 255;
            }
        }
        TextureData data(8, 8, (unsigned int)pixels->size(), ColorFormat::RGBA, 4, pixels->data());
        batchTexturePixels().push_back(std::move(pixels));
        return data;
    }

    std::vector<uint8_t> batchReadTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    struct BatchDifference{
        int maxDiff = 0;
        size_t over1 = 0; // pixels whose largest channel difference is above 1
        size_t lit = 0;   // pixels that are not the background in the reference
    };

    BatchDifference batchCompare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        BatchDifference d;
        if (a.size() != b.size() || a.empty()){
            d.maxDiff = 255;
            d.over1 = a.size() / 4;
            return d;
        }
        for (size_t i = 0; i < a.size() / 4; i++){
            int largest = 0;
            for (int c = 0; c < 4; c++)
                largest = std::max(largest, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            d.maxDiff = std::max(d.maxDiff, largest);
            if (largest > 1)
                d.over1++;
            if (a[i * 4] != a[0] || a[i * 4 + 1] != a[1] || a[i * 4 + 2] != a[2])
                d.lit++;
        }
        return d;
    }

    class SpriteBatchGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Sprite>> sprites;

        void SetUp() override{
            GpuEnvironment::setView(BATCH_VIEW_SIZE, BATCH_VIEW_SIZE, BATCH_VIEW_SIZE, BATCH_VIEW_SIZE, Scaling::STRETCH);
            scene.setBackgroundColor(0.1f, 0.12f, 0.15f, 1.0f);
            Engine::setDynamicBatching(true);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            sprites.clear();
            Engine::removeScene(&scene);
            Engine::setDynamicBatching(true);
        }

        // a 2D camera: one world unit a pixel, origin at the bottom left, no depth test
        void camera2D(){
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(0.0f, (float)BATCH_VIEW_SIZE, 0.0f, (float)BATCH_VIEW_SIZE, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setDepthTest(false);
            camera.setTransparentSort(false);
            camera.activate();
        }

        void camera3D(){
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 4.0f, 12.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
        }

        Sprite& sprite(const std::string& texture, const TextureData& data, float x, float y, unsigned int w = 16, unsigned int h = 16){
            sprites.push_back(std::make_unique<Sprite>(&scene));
            Sprite& s = *sprites.back();
            s.setTexture(texture, data);
            s.setSize(w, h);
            s.setPosition(x, y, 0.0f);
            return s;
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        // the frame drawn with batching on or off, its draw calls and batch counters
        std::vector<uint8_t> frame(bool batching, uint32_t* draws = nullptr){
            Engine::setDynamicBatching(batching);
            GpuEnvironment::drawFrames(3);
            if (draws)
                *draws = Engine::getFrameStats().drawCalls;
            return batchReadTarget();
        }

        const BatchStats& stats(){
            static BatchStats none;
            const RenderSystem::BatchFrame* batchFrame = scene.getSystem<RenderSystem>()->getBatchFrame(SystemRender::FRAME_STATS_FRAME);
            return batchFrame ? batchFrame->stats : none;
        }

        // batching on and off draw the same image (max channel difference 1, at most `tolerated`
        // pixels more: edges of rotated quads), the on frame with `expectedDraws` draws or fewer
        void expectSameImage(size_t tolerated, uint32_t expectedOnDraws, uint32_t* offDrawsOut = nullptr){
            uint32_t onDraws = 0, offDraws = 0;
            const std::vector<uint8_t> on = frame(true, &onDraws);
            const std::vector<uint8_t> off = frame(false, &offDraws);
            const BatchDifference d = batchCompare(off, on);
            EXPECT_GT(d.lit, 200u) << "the scene should draw something";
            EXPECT_LE(d.over1, tolerated) << "max channel difference " << d.maxDiff;
            EXPECT_LE(onDraws, expectedOnDraws);
            EXPECT_LT(onDraws, offDraws);
            if (offDrawsOut)
                *offDrawsOut = offDraws;
        }
    };

}

TEST_F(SpriteBatchGpu, ConsecutiveSpritesDrawOnce){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData coin = batchChecker(250, 200, 40);
    for (int i = 0; i < 100; i++)
        sprite("sprite-batch-coin", coin, (float)(i % 10) * 24.0f + 8.0f, (float)(i / 10) * 24.0f + 8.0f);
    ASSERT_TRUE(render());

    uint32_t onDraws = 0, offDraws = 0;
    const std::vector<uint8_t> on = frame(true, &onDraws);
    EXPECT_EQ(onDraws, 1u);
    EXPECT_EQ(stats().batches, 1u);
    EXPECT_EQ(stats().batchedObjects, 100u);
    EXPECT_EQ(stats().savedByBatching(), 99u);
    const std::vector<uint8_t> off = frame(false, &offDraws);
    EXPECT_EQ(offDraws, 100u);
    const BatchDifference d = batchCompare(off, on);
    EXPECT_EQ(d.over1, 0u) << "max channel difference " << d.maxDiff;
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, MatchesUnbatchedUnlit2D){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData sheet = batchChecker(60, 200, 240, true);
    for (int i = 0; i < 40; i++){
        Sprite& s = sprite("sprite-batch-sheet", sheet, (float)(i % 8) * 30.0f + 16.0f, (float)(i / 8) * 46.0f + 20.0f, 20, 24);
        // tints, texture rects (frames, a mirrored one), rotations, a mirroring scale
        s.setColor(1.0f, 0.4f + 0.015f * (float)i, 0.8f, (i % 3 == 0) ? 0.6f : 1.0f);
        if (i % 4 == 1)
            s.setTextureRect(0.5f, 0.0f, 0.5f, 0.5f);
        if (i % 4 == 2)
            s.setTextureRect(1.0f, 0.5f, -0.5f, 0.5f);
        if (i % 5 == 3)
            s.setRotation(0.0f, 0.0f, 33.0f);
        if (i % 7 == 4)
            s.setScale(Vector3(-1.0f, 1.0f, 1.0f));
    }
    ASSERT_TRUE(render());
    // rotated quads may move an edge pixel by float rounding
    expectSameImage(16, 3);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, MatchesUnbatchedLight2D){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData sheet = batchChecker(220, 220, 220);
    for (int i = 0; i < 30; i++){
        Sprite& s = sprite("sprite-batch-lit2d", sheet, (float)(i % 6) * 40.0f + 12.0f, (float)(i / 6) * 48.0f + 12.0f, 32, 32);
        if (i % 4 == 1)
            s.setRotation(0.0f, 0.0f, 90.0f);
    }
    Light2D light(&scene);
    light.setColor(1.0f, 0.8f, 0.5f);
    light.setIntensity(2.0f);
    light.setRange(160.0f);
    light.setPosition(128.0f, 128.0f, 0.0f);
    ASSERT_TRUE(render());
    expectSameImage(8, 2);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, LitSpritesBatchWhenTheirLightsAreTheSame){
    JASPER_REQUIRE_GPU();
    camera3D();
    const TextureData card = batchChecker(200, 180, 160);
    for (int i = 0; i < 12; i++){
        Sprite& s = sprite("sprite-batch-lit", card, -5.5f + (float)i, 0.0f, 1, 1);
        s.setRotation(0.0f, (float)(i * 15), 0.0f);
    }
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.3f, -1.0f, -0.5f);
    sun.setIntensity(1.5f);
    sun.setShadows(false);
    ASSERT_TRUE(render());
    expectSameImage(8, 2);
    EXPECT_EQ(stats().breaks[(size_t)BatchBreak::OBJECT_LIGHTS], 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, TextureChangeBreaksTheRun){
    JASPER_REQUIRE_GPU();
    camera2D();
    // in hierarchy order A A B A, overlapping: the last A cannot move before B
    const TextureData a = batchChecker(240, 60, 60);
    const TextureData b = batchChecker(60, 240, 60);
    sprite("sprite-batch-a", a, 40.0f, 40.0f, 40, 40);
    sprite("sprite-batch-a", a, 60.0f, 60.0f, 40, 40);
    sprite("sprite-batch-b", b, 80.0f, 80.0f, 40, 40);
    sprite("sprite-batch-a", a, 100.0f, 100.0f, 40, 40);
    ASSERT_TRUE(render());
    uint32_t draws = 0;
    frame(true, &draws);
    EXPECT_EQ(draws, 3u);
    EXPECT_EQ(stats().breaks[(size_t)BatchBreak::TEXTURE], 2u);
    const RenderSystem::BatchFrame* batchFrame = scene.getSystem<RenderSystem>()->getBatchFrame(SystemRender::FRAME_STATS_FRAME);
    ASSERT_NE(batchFrame, nullptr);
    ASSERT_GE(batchFrame->breaks.size(), 1u);
    EXPECT_EQ(batchFrame->breaks[0].reason, BatchBreak::TEXTURE);
    EXPECT_EQ(batchFrame->breaks[0].entity, sprites[2]->getEntity());
    EXPECT_EQ(batchFrame->breaks[0].previous, sprites[1]->getEntity());
    expectSameImage(0, 3);
}

TEST_F(SpriteBatchGpu, A2DCameraReordersPastDisjointSprites){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData a = batchChecker(240, 60, 60);
    const TextureData b = batchChecker(60, 60, 240);
    // A B A B ... none touching: one run per texture
    for (int i = 0; i < 16; i++)
        sprite(i % 2 ? "sprite-batch-rb" : "sprite-batch-ra", i % 2 ? b : a, (float)(i % 8) * 30.0f + 4.0f, (float)(i / 8) * 40.0f + 30.0f);
    ASSERT_TRUE(render());
    uint32_t draws = 0;
    frame(true, &draws);
    EXPECT_EQ(draws, 2u);
    // the first of each texture opens its run, a blue one joins the newest run, a red one jumps back
    EXPECT_EQ(stats().reordered, 7u);
    expectSameImage(0, 2);
}

TEST_F(SpriteBatchGpu, DepthPrePassAgreesWithTheBatchedColourPass){
    JASPER_REQUIRE_GPU();
    camera3D();
    scene.setDepthPrepassEnabled(true);
    // alpha-masked cards (the opaque queue, so the pre-pass draws them) at equal and different
    // depths, crossing each other
    const TextureData card = batchChecker(210, 120, 90, true);
    for (int i = 0; i < 24; i++){
        Sprite& s = sprite("sprite-batch-prepass", card, -3.0f + (float)(i % 6), -1.0f + (float)(i / 6) * 0.7f, 2, 2);
        s.getComponent<MeshComponent>().submeshes[0].material.alphaMode = MaterialAlphaMode::MASK;
        s.setPosition(-3.0f + (float)(i % 6), -1.0f + (float)(i / 6) * 0.7f, (i % 3) * -0.5f);
        s.setRotation(0.0f, (float)((i % 5) * 20 - 40), 0.0f);
    }
    ASSERT_TRUE(render());
    expectSameImage(16, 8);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, ShadowCastersDrawInGroups){
    JASPER_REQUIRE_GPU();
    camera3D();
    const TextureData card = batchChecker(220, 220, 200);
    for (int i = 0; i < 10; i++){
        Sprite& s = sprite("sprite-batch-caster", card, -4.5f + (float)i, 0.5f, 1, 2);
        s.setRotation(0.0f, (float)(i * 9), 0.0f);
    }
    Primitive ground(&scene);
    ground.createPlane(20.0f, 20.0f);
    ground.setColor(0.6f, 0.6f, 0.6f);
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -1.0f, -0.3f);
    sun.setIntensity(2.0f);
    sun.setShadows(true);
    ASSERT_TRUE(render());
    uint32_t offDraws = 0;
    expectSameImage(24, 200, &offDraws);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, FarFromTheOriginKeepsItsPrecision){
    JASPER_REQUIRE_GPU();
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(0.0f, (float)BATCH_VIEW_SIZE, 0.0f, (float)BATCH_VIEW_SIZE, -100.0f, 100.0f);
    camera.setPosition(100000.0f, 50000.0f, 10.0f);
    camera.setTarget(100000.0f, 50000.0f, 0.0f);
    camera.setDepthTest(false);
    camera.activate();
    const TextureData coin = batchChecker(250, 220, 30);
    for (int i = 0; i < 20; i++)
        sprite("sprite-batch-far", coin, 100000.0f + (float)(i % 5) * 40.0f + 10.0f, 50000.0f + (float)(i / 5) * 50.0f + 10.0f, 24, 24);
    ASSERT_TRUE(render());
    expectSameImage(0, 1);
}

TEST_F(SpriteBatchGpu, SettledSpritesShowDirectWrites){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData coin = batchChecker(200, 200, 200);
    for (int i = 0; i < 10; i++)
        sprite("sprite-batch-direct", coin, (float)i * 24.0f + 4.0f, 100.0f, 20, 20);
    ASSERT_TRUE(render());
    frame(true);
    EXPECT_GT(stats().settled, 0u) << "unchanged sprites reuse their baked vertices";

    // written straight into the components, no setter, no flag
    MeshComponent& mesh = sprites[3]->getComponent<MeshComponent>();
    mesh.submeshes[0].material.baseColorFactor = Vector4(1.0f, 0.1f, 0.1f, 1.0f);
    mesh.submeshes[0].textureRect = Rect(0.0f, 0.0f, 0.5f, 0.5f);
    Transform& moved = sprites[6]->getComponent<Transform>();
    moved.position = Vector3(150.0f, 180.0f, 0.0f);
    moved.needUpdate = true;
    expectSameImage(0, 1);
}

TEST_F(SpriteBatchGpu, TheMVPABatchedSpriteWentWithoutIsMadeBeforeItDrawsAlone){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData coin = batchChecker(240, 180, 60);
    for (int i = 0; i < 10; i++)
        sprite("sprite-batch-mvp", coin, (float)i * 24.0f + 4.0f, 100.0f, 20, 20);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> reference = frame(false);

    // scroll with batching on (their MVPs wait), come back, turn it off: each draws alone with
    // its own MVP, which must be made for the camera it is drawn with
    Engine::setDynamicBatching(true);
    for (int step = 1; step <= 5; step++){
        camera.setPosition((float)(step * 7), 0.0f, 10.0f);
        camera.setTarget((float)(step * 7), 0.0f, 0.0f);
        GpuEnvironment::drawFrames(1);
    }
    camera.setPosition(0.0f, 0.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    GpuEnvironment::drawFrames(2);
    const std::vector<uint8_t> after = frame(false);
    const BatchDifference d = batchCompare(reference, after);
    EXPECT_EQ(d.over1, 0u) << "max channel difference " << d.maxDiff;
}

TEST_F(SpriteBatchGpu, SpritesOfOneShapeShareTheirBuffers){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData coin = batchChecker(250, 200, 40);
    ASSERT_TRUE(render());
    const int buffersBefore = SystemRender::getResourcesInUse(GpuResourceKind::BUFFER);
    const size_t sharedBefore = MeshBufferPool::getBufferCount();
    // 1,500 sprites of 3 shapes: two buffers each before, a pair per shape now
    for (int i = 0; i < 1500; i++){
        const unsigned int size = 8 + (unsigned int)(i % 3) * 4;
        sprite("sprite-batch-share", coin, (float)(i % 50) * 5.0f, (float)(i / 50) * 8.0f, size, size);
    }
    ASSERT_TRUE(render());
    frame(true);
    const int buffersAfter = SystemRender::getResourcesInUse(GpuResourceKind::BUFFER);
    EXPECT_LE(MeshBufferPool::getBufferCount() - sharedBefore, 6u);
    EXPECT_LE(buffersAfter - buffersBefore, 16) << "sprites take no GPU buffers of their own (stream chunks and the quad indices aside)";
    for (const std::unique_ptr<Sprite>& s : sprites)
        EXPECT_FALSE(s->getComponent<MeshComponent>().loadFailed);
    EXPECT_EQ(stats().runsSkipped, 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, GeometryThatKeepsChangingGetsABufferOfItsOwn){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData coin = batchChecker(250, 200, 40);
    Sprite& growing = sprite("sprite-batch-grow", coin, 40.0f, 40.0f, 10, 10);
    sprite("sprite-batch-grow", coin, 120.0f, 40.0f, 10, 10);
    ASSERT_TRUE(render());
    for (int i = 0; i < 6; i++){
        growing.setSize(10 + (unsigned int)i, 10 + (unsigned int)i);
        GpuEnvironment::drawFrames(1);
    }
    MeshComponent& mesh = growing.getComponent<MeshComponent>();
    EXPECT_EQ(mesh.buffer.getUsage(), BufferUsage::DYNAMIC);
    EXPECT_FALSE(mesh.buffer.getRender()->isShared());
    const size_t shared = MeshBufferPool::getBufferCount();
    for (int i = 0; i < 4; i++){
        growing.setSize(20 - (unsigned int)i, 20 - (unsigned int)i);
        GpuEnvironment::drawFrames(1);
    }
    EXPECT_EQ(MeshBufferPool::getBufferCount(), shared) << "no immutable buffer per frame";
    expectSameImage(0, 2);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SpriteBatchGpu, UIImagesBatch){
    JASPER_REQUIRE_GPU();
    const TextureData panel = batchChecker(90, 160, 230);
    std::vector<std::unique_ptr<Image>> images;
    for (int i = 0; i < 20; i++){
        images.push_back(std::make_unique<Image>(&scene));
        Image& image = *images.back();
        image.setTexture("sprite-batch-ui", panel);
        image.setSize(40, 30);
        image.setPosition((float)(i % 5) * 48.0f + 8.0f, (float)(i / 5) * 40.0f + 8.0f, 0.0f);
        image.setColor(1.0f, 1.0f, 1.0f - 0.03f * (float)i, 1.0f);
    }
    ASSERT_TRUE(render());
    expectSameImage(0, 1);
    images.clear();
}

TEST_F(SpriteBatchGpu, OffCostsNothing){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData coin = batchChecker(250, 200, 40);
    for (int i = 0; i < 20; i++)
        sprite("sprite-batch-off", coin, (float)(i % 10) * 24.0f + 8.0f, (float)(i / 10) * 24.0f + 8.0f);
    ASSERT_TRUE(render());
    uint32_t draws = 0;
    frame(false, &draws);
    EXPECT_EQ(draws, 20u);
    EXPECT_FALSE(stats().enabled);
    EXPECT_EQ(stats().batches + stats().singleDraws + stats().streamChunks, 0u);
    for (const std::unique_ptr<Sprite>& s : sprites)
        EXPECT_FALSE(scene.getSystem<RenderSystem>()->hasBatchRecord(s->getEntity()));
}

TEST_F(SpriteBatchGpu, ClippedUIChildrenBatchUnderTheirParentsScissor){
    JASPER_REQUIRE_GPU();
    // a panel clipping 20 children, some of them across its edges: between two children the pass
    // resets the scissor; that costs nothing while it is lazy, so the children are one draw
    const TextureData panel = batchChecker(200, 120, 220);
    std::vector<std::unique_ptr<Image>> images;
    images.push_back(std::make_unique<Image>(&scene));
    Image& parent = *images.back();
    parent.setTexture("sprite-batch-clip", panel);
    parent.setSize(160, 120);
    parent.setPosition(48.0f, 64.0f, 0.0f);
    for (int i = 0; i < 20; i++){
        images.push_back(std::make_unique<Image>(&scene));
        Image& child = *images.back();
        child.setTexture("sprite-batch-clip", panel);
        child.setSize(36, 30);
        child.setPosition((float)(i % 5) * 40.0f - 20.0f, (float)(i / 5) * 34.0f - 16.0f, 0.0f);
        parent.addChild(&child);
    }
    ASSERT_TRUE(render());
    expectSameImage(0, 2);
    images.clear();
}
