// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// 2D sorting drawn (docs/specs/S52-sprite-instancing.md R3-R9, AC1, AC2): Sorting Layers and Order
// in Layer decide which of two overlapping sprites is on top in a 2D camera, a Sorting Group sorts
// its subtree as one item, the 3D transparent queue compares the layer first; with defaults the
// hierarchy decides and nothing is gathered. The batcher's runs never span a bucket (a layer):
// the break is counted as sort_bucket and the image is the one batching off draws. glcore and
// Vulkan (lavapipe).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SortingGroupComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "registry/SortingLayers.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int SORT_VIEW_SIZE = 128;

    std::vector<std::unique_ptr<std::vector<uint8_t>>>& sortTexturePixels(){
        static std::vector<std::unique_ptr<std::vector<uint8_t>>> pixels;
        return pixels;
    }

    // a 4x4 texture of one colour
    TextureData sortSolid(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255){
        auto pixels = std::make_unique<std::vector<uint8_t>>(4 * 4 * 4);
        for (size_t i = 0; i < 16; i++){
            (*pixels)[i * 4] = r;
            (*pixels)[i * 4 + 1] = g;
            (*pixels)[i * 4 + 2] = b;
            (*pixels)[i * 4 + 3] = a;
        }
        TextureData data(4, 4, (unsigned int)pixels->size(), ColorFormat::RGBA, 4, pixels->data());
        sortTexturePixels().push_back(std::move(pixels));
        return data;
    }

    class SpriteSortGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Sprite>> sprites;
        std::vector<std::unique_ptr<EntityHandle>> handles;

        void SetUp() override{
            GpuEnvironment::setView(SORT_VIEW_SIZE, SORT_VIEW_SIZE, SORT_VIEW_SIZE, SORT_VIEW_SIZE, Scaling::STRETCH);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            Engine::setDynamicBatching(true);
            Engine::setScene(&scene);
            SortingLayers::setLayers({{3, "Back"}, {0, "Default"}, {4, "Front"}});
        }

        void TearDown() override{
            sprites.clear();
            handles.clear();
            Engine::removeScene(&scene);
            Engine::setDynamicBatching(true);
            GpuEnvironment::resetView();
            SortingLayers::reset();
        }

        void camera2D(){
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(0.0f, (float)SORT_VIEW_SIZE, 0.0f, (float)SORT_VIEW_SIZE, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setDepthTest(false);
            camera.setTransparentSort(false);
            camera.activate();
        }

        Sprite& sprite(const std::string& texture, const TextureData& data, float x, float y, unsigned int size = 32){
            sprites.push_back(std::make_unique<Sprite>(&scene));
            Sprite& s = *sprites.back();
            s.setTexture(texture, data);
            s.setSize(size, size);
            s.setPosition(x, y, 0.0f);
            return s;
        }

        void layer(Sprite& s, int layerId, int order = 0){
            MeshComponent& mesh = s.getComponent<MeshComponent>();
            mesh.sortingLayer = layerId;
            mesh.sortingOrder = order;
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        std::vector<uint8_t> frame(bool batching, uint32_t* draws = nullptr){
            Engine::setDynamicBatching(batching);
            GpuEnvironment::drawFrames(3);
            if (draws)
                *draws = Engine::getFrameStats().drawCalls;
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        // the pixel at (x, y) of the view, y up (the target's rows are top first)
        static std::array<uint8_t, 4> pixel(const std::vector<uint8_t>& rgba, int x, int y){
            const size_t row = (size_t)(SORT_VIEW_SIZE - 1 - y);
            const size_t i = (row * SORT_VIEW_SIZE + (size_t)x) * 4;
            return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
        }

        const BatchStats& stats(){
            static BatchStats none;
            const RenderSystem::BatchFrame* batchFrame = scene.getSystem<RenderSystem>()->getBatchFrame(SystemRender::FRAME_STATS_FRAME);
            return batchFrame ? batchFrame->stats : none;
        }

        static size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
            if (a.size() != b.size())
                return a.size() / 4 + 1;
            size_t count = 0;
            for (size_t i = 0; i < a.size(); i += 4){
                if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
                    count++;
            }
            return count;
        }
    };

}

// AC1 (R9): with defaults the hierarchy decides (the later sprite covers the earlier) and the
// 2D loop gathers nothing, frame after frame
TEST_F(SpriteSortGpu, DefaultsDrawTheHierarchyAndGatherNothing){
    JASPER_REQUIRE_GPU();
    camera2D();
    sprite("sort-red", sortSolid(255, 0, 0), 40, 40);
    sprite("sort-green", sortSolid(0, 255, 0), 50, 50);
    ASSERT_TRUE(render());
    RenderSystem& system = *scene.getSystem<RenderSystem>();
    const uint32_t gathers = system.getSortGatherCount();
    const std::vector<uint8_t> image = frame(true);
    EXPECT_FALSE(system.isSortingActive());
    EXPECT_EQ(system.getSortGatherCount(), gathers);
    EXPECT_EQ(pixel(image, 60, 60)[1], 255u) << "the second in the hierarchy is on top";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC1 (R3): Sorting Layer first, then Order in Layer, whatever the hierarchy
TEST_F(SpriteSortGpu, LayersAndOrdersDecideWhatIsOnTop){
    JASPER_REQUIRE_GPU();
    camera2D();
    Sprite& red = sprite("sort-red", sortSolid(255, 0, 0), 40, 40);
    Sprite& green = sprite("sort-green", sortSolid(0, 255, 0), 50, 50);
    layer(red, 4);
    ASSERT_TRUE(render());
    std::vector<uint8_t> image = frame(true);
    EXPECT_EQ(pixel(image, 60, 60)[0], 255u) << "the Front layer is on top";
    EXPECT_TRUE(scene.getSystem<RenderSystem>()->isSortingActive());
    // one layer, the orders decide
    layer(red, 0, 5);
    layer(green, 0, 1);
    image = frame(true);
    EXPECT_EQ(pixel(image, 60, 60)[0], 255u) << "Order in Layer 5 over 1";
    layer(green, 0, 6);
    image = frame(true);
    EXPECT_EQ(pixel(image, 60, 60)[1], 255u) << "Order in Layer 6 over 5";
    // batching off draws the same
    EXPECT_EQ(differing(frame(false), image), 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC1 (R5): a Sorting Group sorts its subtree as one item at its layer
TEST_F(SpriteSortGpu, ASortingGroupSortsAsOneItem){
    JASPER_REQUIRE_GPU();
    camera2D();
    Sprite& groupRoot = sprite("sort-blue", sortSolid(0, 0, 255), 20, 20);
    Sprite& child = sprite("sort-red", sortSolid(255, 0, 0), 30, 30);
    child.setPosition(10, 10, 0); // local: 30, 30 in the world
    groupRoot.addChild(&child);
    Sprite& outside = sprite("sort-green", sortSolid(0, 255, 0), 35, 35);
    // the child alone would draw over everything (Front), the group keeps it under Default's green
    layer(child, 4);
    SortingGroupComponent group;
    group.sortingLayer = 3;
    scene.addComponent<SortingGroupComponent>(groupRoot.getEntity(), group);
    ASSERT_TRUE(render());
    std::vector<uint8_t> image = frame(true);
    EXPECT_EQ(pixel(image, 45, 45)[1], 255u) << "the group (Back) is under the green sprite (Default)";
    EXPECT_EQ(pixel(image, 32, 32)[0], 255u) << "inside the group the child (Front) is over its root";
    // disabled: the child sorts on its own again (Front over everything)
    scene.getComponent<SortingGroupComponent>(groupRoot.getEntity()).enabled = false;
    image = frame(true);
    EXPECT_EQ(pixel(image, 45, 45)[0], 255u);
    (void)outside;
    EXPECT_NO_ENGINE_ERRORS();
}

// AC2 (R7, D2): two layers of interleaved, non-overlapping sprites of two textures: the runs never
// span a layer (sort_bucket breaks counted), 2D reordering works inside each, and the image is
// batching off's
TEST_F(SpriteSortGpu, BucketsBound){
    JASPER_REQUIRE_GPU();
    camera2D();
    const TextureData a = sortSolid(250, 200, 40);
    const TextureData b = sortSolid(40, 120, 250);
    for (int i = 0; i < 24; i++){
        Sprite& s = sprite(i % 2 ? "sort-a" : "sort-b", i % 2 ? a : b, (float)(i % 6) * 20.0f + 2.0f, (float)(i / 6) * 20.0f + 2.0f, 12);
        layer(s, (i / 3) % 2 ? 4 : 3);
    }
    ASSERT_TRUE(render());
    uint32_t onDraws = 0, offDraws = 0;
    const std::vector<uint8_t> on = frame(true, &onDraws);
    const BatchStats batched = stats();
    const std::vector<uint8_t> off = frame(false, &offDraws);
    EXPECT_EQ(differing(on, off), 0u);
    // per layer one run of each texture: 4 draws, not 24
    EXPECT_LE(onDraws, 4u);
    EXPECT_EQ(offDraws, 24u);
    EXPECT_GE(batched.breaks[(size_t)BatchBreak::SORT_BUCKET], 1u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC1 (R6): the 3D transparent queue orders by layer before the distance: a far blended sprite of
// the Front layer draws after (over) a near one of Default
TEST_F(SpriteSortGpu, TheTransparentQueueComparesTheLayerFirst){
    JASPER_REQUIRE_GPU();
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 0.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    Sprite& nearSprite = sprite("sort-green-alpha", sortSolid(0, 255, 0, 128), -2.0f, -2.0f, 4);
    nearSprite.setPosition(-2.0f, -2.0f, 2.0f);
    Sprite& farSprite = sprite("sort-red-alpha", sortSolid(255, 0, 0, 128), -2.0f, -2.0f, 4);
    farSprite.setPosition(-2.5f, -2.5f, -2.0f);
    ASSERT_TRUE(render());
    std::vector<uint8_t> image = frame(true);
    const std::array<uint8_t, 4> byDistance = pixel(image, SORT_VIEW_SIZE / 2, SORT_VIEW_SIZE / 2);
    EXPECT_GT(byDistance[1], byDistance[0]) << "far first: the near green is blended last";
    layer(farSprite, 4);
    image = frame(true);
    EXPECT_TRUE(scene.getSystem<RenderSystem>()->isSortingActive());
    const std::array<uint8_t, 4> byLayer = pixel(image, SORT_VIEW_SIZE / 2, SORT_VIEW_SIZE / 2);
    EXPECT_GT(byLayer[0], byLayer[1]) << "the Front layer blends last, far or not";
    const std::array<uint8_t, 4> unbatched = pixel(frame(false), SORT_VIEW_SIZE / 2, SORT_VIEW_SIZE / 2);
    EXPECT_GT(unbatched[0], unbatched[1]) << "the same with batching off";
    EXPECT_NO_ENGINE_ERRORS();
}
