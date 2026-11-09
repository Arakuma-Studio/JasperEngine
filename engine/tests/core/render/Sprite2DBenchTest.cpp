// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S52's budgets (docs/specs/S52-sprite-instancing.md R26-R29, AC15), desktop: 2,000 sprites in 10
// Sorting Layers with Custom Axis gather and sort in 0.2 ms, defaults gather nothing (a counter);
// 10,000 moving Sprite Batch instances pack and upload in 0.5 ms (measured alone: the stats'
// packMs + uploadMs) as one draw of 96 bytes an instance (112 with FX), a static batch uploads
// nothing, C++ setPositions of 10,000 in 1 ms (Lua's: LuaSpriteBatchBench); 1,000 snapped library:sprite sprites with FLASH animating on 100 draw in 2 runs at
// most and the flash writes no GPU vertex buffer. Times are [bench] lines, enforced through
// perfSlack() (JASPER_TEST_PERF_SLACK); counts are always asserted. Mobile and web are measured by
// hand (docs/specs/evidence/s52/).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "object/SpriteBatch.h"
#include "registry/SortingLayers.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"
#include "support/AllocationCounter.h"

#include "GpuEnvironment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    double benchMedian(std::vector<double> values){
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[values.size() / 2];
    }

    double benchMs(std::chrono::steady_clock::time_point start){
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    std::vector<uint8_t>& benchPixels(){
        static std::vector<uint8_t> pixels(8 * 8 * 4, 200);
        return pixels;
    }

}

// R26: 2,000 sprites in 10 layers with Custom Axis: the gather and sort; defaults gather nothing
TEST(SpriteSortBench, GatherAndSort2000){
    Scene scene;
    std::vector<SortingLayerInfo> table;
    for (int l = 0; l < 10; l++)
        table.push_back({l == 0 ? 0 : l + 10, l == 0 ? "Default" : "Layer " + std::to_string(l)});
    SortingLayers::setLayers(table);
    RenderSystem& render = *scene.getSystem<RenderSystem>();
    for (int i = 0; i < 2000; i++){
        const Entity entity = scene.createUserEntity();
        scene.addComponent<Transform>(entity, {});
        MeshComponent mesh;
        scene.addComponent<MeshComponent>(entity, mesh);
        scene.addComponent<SpriteComponent>(entity, {});
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.worldPosition = Vector3((float)(i % 50), (float)((i * 37) % 400), 0.0f);
    }
    // defaults: nothing gathered
    const uint32_t before = render.getSortGatherCount();
    render.updateSortingActive();
    render.getSorted2DOrder();
    EXPECT_FALSE(render.isSortingActive());
    EXPECT_EQ(render.getSortGatherCount(), before);

    auto meshes = scene.getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        mesh.sortingLayer = table[i % 10].id;
        mesh.sortingOrder = (int)(i % 7);
    }
    SortingLayers::setTransparencySortMode(TransparencySortMode::CUSTOM_AXIS);
    std::vector<double> times;
    for (int run = 0; run < 30; run++){
        const auto start = std::chrono::steady_clock::now();
        render.updateSortingActive();
        const size_t drawn = render.getSorted2DOrder().size();
        times.push_back(benchMs(start));
        EXPECT_EQ(drawn, 2000u);
    }
    const double median = benchMedian(times);
    std::printf("[bench] 2D sort, 2,000 sprites in 10 layers, Custom Axis: %.3f ms median (budget 0.2)\n", median);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(median, 0.2 * testsupport::perfSlack());
    }
    SortingLayers::reset();
}

// R27: 10,000 moving instances of one page: one draw, the pack and upload, 96 bytes an instance;
// a static batch uploads nothing
TEST(SpriteBatchBenchGpu, TenThousandMoving){
    JASPER_REQUIRE_GPU();
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(0.0f, 512.0f, 0.0f, 512.0f, -100.0f, 100.0f);
    camera.setPosition(0.0f, 0.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.setDepthTest(false);
    camera.activate();
    Engine::setScene(&scene);
    Sprite sprite(&scene);
    sprite.setTexture("s52-bench|texture", TextureData(8, 8, (unsigned int)benchPixels().size(), ColorFormat::RGBA, 4, benchPixels().data()));
    sprite.setSize(4, 4);
    SpriteBatch batch(&scene, sprite.getEntity());
    std::vector<float> positions(20000);
    auto place = [&](float t){
        for (int i = 0; i < 10000; i++){
            positions[(size_t)i * 2] = std::fmod((float)(i % 100) * 5.0f + t, 500.0f);
            positions[(size_t)i * 2 + 1] = (float)(i / 100) * 5.0f;
        }
    };
    place(0.0f);
    ASSERT_EQ(batch.setPositions(positions), "");
    ASSERT_TRUE(GpuEnvironment::runFrames([&scene](){ return scene.getSystem<RenderSystem>()->isAllLoaded(); }));
    RenderSystem& render = *scene.getSystem<RenderSystem>();
    std::vector<double> setTimes, frameTimes, packUploadTimes;
    for (int f = 0; f < 20; f++){
        place((float)f);
        auto start = std::chrono::steady_clock::now();
        batch.setPositions(positions);
        setTimes.push_back(benchMs(start));
        start = std::chrono::steady_clock::now();
        GpuEnvironment::drawFrames(1);
        frameTimes.push_back(benchMs(start));
        packUploadTimes.push_back(render.getSpriteBatchStats().packMs + render.getSpriteBatchStats().uploadMs);
        EXPECT_EQ(render.getSpriteBatchStats().draws, 1u);
        EXPECT_EQ(render.getSpriteBatchStats().instances, 10000u);
        EXPECT_EQ(render.getSpriteBatchStats().uploadBytes, 10000u * 96u);
    }
    // static: nothing uploaded
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(render.getSpriteBatchStats().uploadBytes, 0u);
    const double setMedian = benchMedian(setTimes);
    const double packUploadMedian = benchMedian(packUploadTimes);
    std::printf("[bench] Sprite Batch, 10,000 moving: setPositions (C++) %.3f ms, pack and upload %.3f ms (budget 0.5), "
                "whole frame %.3f ms median\n", setMedian, packUploadMedian, benchMedian(frameTimes));
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(setMedian, 1.0 * testsupport::perfSlack());
        EXPECT_LE(packUploadMedian, 0.5 * testsupport::perfSlack());
    }
    Engine::removeScene(&scene);
}

// R28: 1,000 snapped library:sprite sprites of one sheet, FLASH animating on 100 of them: at most
// 2 colour-pass draws, no GPU vertex-buffer write caused by the flash
TEST(PixelArtBatchBenchGpu, ThousandSnappedFlashing){
    JASPER_REQUIRE_GPU();
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(0.0f, 512.0f, 0.0f, 512.0f, -100.0f, 100.0f);
    camera.setPosition(0.0f, 0.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.setDepthTest(false);
    camera.activate();
    Engine::setScene(&scene);
    Engine::setDynamicBatching(true);
    {
        PixelPerfectCamera ppc(&scene, camera.getEntity());
        ppc.setReferenceResolution(Vector2(256.0f, 256.0f));
    }
    std::vector<std::unique_ptr<Sprite>> sprites;
    for (int i = 0; i < 1000; i++){
        sprites.push_back(std::make_unique<Sprite>(&scene));
        Sprite& s = *sprites.back();
        s.setTexture("s52-bench|texture", TextureData(8, 8, (unsigned int)benchPixels().size(), ColorFormat::RGBA, 4, benchPixels().data()));
        s.setSize(8, 8);
        s.setPosition((float)(i % 40) * 12.0f + 0.3f, (float)(i / 40) * 12.0f + 0.6f, 0.0f);
        MeshComponent& mesh = s.getComponent<MeshComponent>();
        mesh.submeshes[0].material.shader = "library:sprite";
        mesh.submeshes[0].material.shaderFeatures = {"FLASH"};
        mesh.submeshes[0].material.shaderParams = {{"flashColor", Vector4(1, 1, 1, 1)}, {"flashAmount", Vector4(0, 0, 0, 0)}};
        mesh.needReload = true;
    }
    ASSERT_TRUE(GpuEnvironment::runFrames([&scene](){ return scene.getSystem<RenderSystem>()->isAllLoaded(); }));
    GpuEnvironment::drawFrames(2);
    std::vector<uint64_t> writes;
    for (const auto& s : sprites)
        writes.push_back(s->getComponent<MeshComponent>().buffer.getWriteCount());
    std::vector<double> times;
    for (int f = 0; f < 20; f++){
        for (int i = 0; i < 100; i++){
            MeshComponent& mesh = sprites[(size_t)i * 10]->getComponent<MeshComponent>();
            mesh.submeshes[0].material.shaderParams[1].second.x = (float)((f + i) % 10) / 10.0f;
            mesh.needUpdateShaderUniforms = true;
        }
        const auto start = std::chrono::steady_clock::now();
        GpuEnvironment::drawFrames(1);
        times.push_back(benchMs(start));
        const RenderSystem::BatchFrame* frame = scene.getSystem<RenderSystem>()->getBatchFrame(SystemRender::FRAME_STATS_FRAME);
        ASSERT_NE(frame, nullptr);
        EXPECT_LE(frame->stats.batches + frame->stats.singleDraws + frame->stats.aloneDraws, 2u);
    }
    for (size_t i = 0; i < sprites.size(); i++)
        EXPECT_EQ(sprites[i]->getComponent<MeshComponent>().buffer.getWriteCount(), writes[i]);
    std::printf("[bench] 1,000 snapped library:sprite sprites, FLASH animating on 100: %.3f ms a frame median, <= 2 draws\n",
                benchMedian(times));
    sprites.clear();
    Engine::removeScene(&scene);
}
