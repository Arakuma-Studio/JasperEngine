// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Render diagnostics (spec S15): the GPU timer on the headless
// device (AC5: glcore on llvmpipe, Vulkan on lavapipe), the VRAM tracker's byte sums and queued
// owners (AC8) and the pass recorder's list.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "profiler/GpuMemory.h"
#include "profiler/Profile.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "sokol/SokolCmdQueue.h"
#include "sokol/SokolVramTracker.h"

#include "GpuEnvironment.h"

#include "sokol_gfx.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class ProfilerScene : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Primitive plane{&scene};

        void SetUp() override{
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 3.0f, 5.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            plane.createPlane(4.0f, 4.0f);
            plane.setColor(1.0f, 0.0f, 0.0f);
            plane.setReceiveLights(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        bool render(){
            Scene* current = &scene;
            // the first GPU test of a run also waits for the runtime shader builds
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            }, 5000);
        }
    };

    class GpuTimerGpu : public ProfilerScene {};
    class PassRecorderGpu : public ProfilerScene {};

    const gpumem::Allocation* findAllocation(const gpumem::Report& report, uint32_t id, bool image){
        for (const gpumem::Allocation& allocation : report.allocations){
            if (allocation.id == id && allocation.image == image)
                return &allocation;
        }
        return nullptr;
    }

}

TEST_F(GpuTimerGpu, EveryPassIsTimed){
    JASPER_REQUIRE_GPU();
    ASSERT_TRUE(render());
    const GpuTimingState idle = SystemRender::getGpuTimingState();
    ASSERT_TRUE(idle.available) << "GPU timing unavailable: " << idle.reason;
    EXPECT_FALSE(idle.enabled);   // off until a consumer asks: no queries

    SystemRender::acquireGpuTiming();
    // the times of a frame arrive a few frames later
    PassList list;
    for (int frame = 0; frame < 60; frame++){
        GpuEnvironment::drawFrames(1);
        list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, true);
        if (frame >= 4 && !list.gpuPending && list.gpuFrameValid)
            break;
    }
    const GpuTimingState timer = SystemRender::getGpuTimingState();
    SystemRender::releaseGpuTiming();
    ASSERT_FALSE(list.passes.empty());
    EXPECT_FALSE(list.gpuPending) << "resolved " << timer.resolvedCommits << ", dropped (overflow " << timer.droppedOverflow
                                  << ", disjoint " << timer.droppedDisjoint << "), " << list.passes.size() << " passes";
    ASSERT_TRUE(list.gpuFrameValid);
    double sum = 0.0;
    for (const PassRecord& pass : list.passes){
        if (!pass.executed)
            continue;
        EXPECT_TRUE(pass.gpuValid) << pass.label;
        EXPECT_GE(pass.gpuMs, 0.0) << pass.label;
        sum += pass.gpuMs;
    }
    EXPECT_GT(sum, 0.0);
    // pass times fit in the frame (the GL family's frame is their sum)
    EXPECT_LE(sum, list.gpuFrameMs * 1.05 + 1e-6);
    const GpuTimingState state = SystemRender::getGpuTimingState();
    if (state.frameIsSum){
        EXPECT_NEAR(sum, list.gpuFrameMs, 1e-6);
    }
    EXPECT_GT(state.resolvedCommits, 0u);

    // released: the next commits issue nothing
    GpuEnvironment::drawFrames(2);
    EXPECT_FALSE(SystemRender::getGpuTimingState().enabled);
}

TEST_F(PassRecorderGpu, ListsTheFramePassesInOrder){
    JASPER_REQUIRE_GPU();
    ASSERT_TRUE(render());
    const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
    ASSERT_FALSE(list.passes.empty());
    uint32_t draws = 0;
    for (size_t i = 0; i < list.passes.size(); i++){
        EXPECT_EQ(list.passes[i].index, (uint32_t)i);
        draws += list.passes[i].draws;
    }
    EXPECT_GT(draws, 0u);
    const RenderStats stats = SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME);
    EXPECT_TRUE(stats.valid);
    EXPECT_EQ(stats.passes, (uint32_t)list.passes.size());
    EXPECT_GT(stats.aliveImages, 0u);

    // a reader asked: the next frames' passes carry their attachments (an idle editor's do not)
    GpuEnvironment::drawFrames(2);
    const PassList detailed = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
    ASSERT_FALSE(detailed.passes.empty());
    for (const PassRecord& pass : detailed.passes){
        EXPECT_TRUE(pass.detailed) << pass.label;
        if (!pass.swapchain && !pass.compute){
            EXPECT_GT(pass.width, 0) << pass.label;
        }
    }
}

TEST(VramTrackerGpu, CreateDestroyBalances){
    JASPER_REQUIRE_GPU();
    const uint64_t before = gpumem::report(false).totalBytes;
    std::vector<sg_image> images;
    std::vector<uint64_t> expected;

    // RGBA8 256x256 with every mip: 4 * (256^2 + 128^2 + ... + 1)
    sg_image_desc mipped = {};
    mipped.width = 256;
    mipped.height = 256;
    mipped.num_mipmaps = 9;
    mipped.pixel_format = SG_PIXELFORMAT_RGBA8;
    mipped.usage.dynamic_update = true;
    mipped.label = "test-mipped";
    uint64_t mipBytes = 0;
    for (int size = 256; size >= 1; size /= 2)
        mipBytes += (uint64_t)size * size * 4;
    images.push_back(sg_make_image(&mipped));
    expected.push_back(mipBytes * SG_NUM_INFLIGHT_FRAMES);   // a copy per frame in flight

    // an MSAA 4 colour target 128x128: every sample
    sg_image_desc msaa = {};
    msaa.width = 128;
    msaa.height = 128;
    msaa.pixel_format = SG_PIXELFORMAT_RGBA8;
    msaa.sample_count = sg_query_pixelformat(SG_PIXELFORMAT_RGBA8).msaa ? 4 : 1;
    msaa.usage.color_attachment = true;
    msaa.label = "test-msaa";
    images.push_back(sg_make_image(&msaa));
    expected.push_back(128ull * 128 * 4 * (uint64_t)msaa.sample_count);

    // a depth target 64x64
    sg_image_desc depth = {};
    depth.width = 64;
    depth.height = 64;
    depth.pixel_format = SG_PIXELFORMAT_DEPTH;
    depth.usage.depth_stencil_attachment = true;
    depth.label = "test-depth";
    images.push_back(sg_make_image(&depth));
    expected.push_back(64ull * 64 * 4);

    // a cube map 32x32: six faces
    sg_image_desc cube = {};
    cube.type = SG_IMAGETYPE_CUBE;
    cube.width = 32;
    cube.height = 32;
    cube.pixel_format = SG_PIXELFORMAT_RGBA8;
    cube.usage.color_attachment = true;
    cube.label = "test-cube";
    images.push_back(sg_make_image(&cube));
    expected.push_back(32ull * 32 * 4 * 6);

    // BC1 where the device has it: 8 bytes a 4x4 block
    if (sg_query_pixelformat(SG_PIXELFORMAT_BC1_RGBA).sample){
        sg_image_desc bc1 = {};
        bc1.width = 64;
        bc1.height = 64;
        bc1.pixel_format = SG_PIXELFORMAT_BC1_RGBA;
        bc1.label = "test-bc1";
        // compressed images are immutable: their blocks come with them
        static const std::vector<uint8_t> blocks(16 * 16 * 8, 0);
        bc1.data.mip_levels[0] = {blocks.data(), blocks.size()};
        images.push_back(sg_make_image(&bc1));
        expected.push_back(16ull * 16 * 8);
    }

    std::vector<sg_buffer> buffers;
    for (int i = 1; i <= 5; i++){
        sg_buffer_desc desc = {};
        desc.size = (size_t)(1024 * i);
        desc.usage.vertex_buffer = true;
        desc.usage.stream_update = true;
        desc.label = "test-buffer";
        buffers.push_back(sg_make_buffer(&desc));
    }

    gpumem::Report report = gpumem::report(true);
    uint64_t sum = 0;
    for (size_t i = 0; i < images.size(); i++){
        ASSERT_EQ(sg_query_image_state(images[i]), SG_RESOURCESTATE_VALID) << i;
        const gpumem::Allocation* allocation = findAllocation(report, images[i].id, true);
        ASSERT_TRUE(allocation) << i;
        EXPECT_EQ(allocation->bytes, expected[i]) << allocation->label;
        sum += expected[i];
    }
    for (size_t i = 0; i < buffers.size(); i++){
        const gpumem::Allocation* allocation = findAllocation(report, buffers[i].id, false);
        ASSERT_TRUE(allocation);
        EXPECT_EQ(allocation->bytes, 1024ull * (i + 1) * SG_NUM_INFLIGHT_FRAMES);
        EXPECT_EQ(allocation->category, gpumem::Category::Uniforms);   // a stream buffer, untagged
        sum += allocation->bytes;
    }
    EXPECT_EQ(findAllocation(report, images[2].id, true)->kind, gpumem::Kind::DepthTarget);
    EXPECT_EQ(findAllocation(report, images[0].id, true)->category, gpumem::Category::Textures);
    EXPECT_EQ(report.totalBytes, before + sum);

    for (sg_image image : images)
        sg_destroy_image(image);
    for (sg_buffer buffer : buffers)
        sg_destroy_buffer(buffer);
    report = gpumem::report(true);
    EXPECT_EQ(report.totalBytes, before);
    for (sg_image image : images)
        EXPECT_FALSE(findAllocation(report, image.id, true));
}

TEST(VramTrackerGpu, QueuedCreationKeepsOwner){
    JASPER_REQUIRE_GPU();
    sg_image_desc desc = {};
    desc.width = 64;
    desc.height = 64;
    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    desc.usage.color_attachment = true;
    std::string label = "test-queued-bloom";
    desc.label = label.c_str();
    sg_image image;
    {
        JE_GPU_ALLOC_SCOPE(PostFX, "Bloom");
        image = SokolCmdQueue::add_command_make_image(desc);
    }
    // the caller's label string is gone by the time the command runs
    label.assign(label.size(), 'x');
    SystemRender::commitQueue();
    SystemRender::executeQueue();
    const gpumem::Report report = gpumem::report(true);
    const gpumem::Allocation* allocation = findAllocation(report, image.id, true);
    ASSERT_TRUE(allocation);
    EXPECT_EQ(allocation->owner, "Bloom");
    EXPECT_EQ(allocation->category, gpumem::Category::PostFX);
    EXPECT_TRUE(allocation->tagged);
    EXPECT_EQ(allocation->label, "test-queued-bloom");
    sg_destroy_image(image);
}
