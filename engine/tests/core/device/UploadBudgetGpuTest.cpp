// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upload pacing on the real queue (spec S56 R8, AC3): queued textures larger than the budget are
// created over several frames, the readiness token completes once the last one ran, the
// cleanups of their data wait for them, and Engine::areScenesLoaded waits for the queue.

#include "render/SystemRender.h"
#include "sokol/SokolCmdQueue.h"

#include "GpuEnvironment.h"

#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

namespace {

    constexpr int kUploadGpuSide = 512;
    constexpr uint64_t kUploadGpuBytes = (uint64_t)kUploadGpuSide * kUploadGpuSide * 4;

    class UploadBudgetScope {
    public:
        explicit UploadBudgetScope(uint64_t bytes): previous(SystemRender::getUploadBudget()){ SystemRender::setUploadBudget(bytes); }
        ~UploadBudgetScope(){ SystemRender::setUploadBudget(previous); }
    private:
        uint64_t previous;
    };

}

TEST(UploadBudgetGpu, QueuedTexturesSpreadAndTheTokenCompletes){
    JASPER_REQUIRE_GPU();
    // what an earlier test left queued runs first
    SystemRender::commitQueue();
    SystemRender::executeQueue();
    while (SystemRender::getDeferredUploadCount() > 0)
        SystemRender::executeQueue();

    UploadBudgetScope budget(kUploadGpuBytes + kUploadGpuBytes / 2);
    std::vector<std::vector<uint8_t>> pixels(4, std::vector<uint8_t>(kUploadGpuBytes, 0x80));
    std::vector<sg_image> images;
    int cleanups = 0;
    for (auto& data : pixels){
        sg_image_desc desc = {};
        desc.width = kUploadGpuSide;
        desc.height = kUploadGpuSide;
        desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.data.mip_levels[0] = {data.data(), data.size()};
        images.push_back(SokolCmdQueue::add_command_make_image(desc));
    }
    // a cleanup queued after them (TexturePool frees the pixels so): runs once they all exist
    SokolCmdQueue::schedule_cleanup([](void* counter){ (*static_cast<int*>(counter))++; }, &cleanups);
    const uint64_t token = SystemRender::getUploadToken();
    EXPECT_FALSE(SystemRender::isUploadTokenComplete(token));

    SystemRender::commitQueue();
    SystemRender::executeQueue();
    EXPECT_EQ(sg_query_image_state(images[0]), SG_RESOURCESTATE_VALID);
    EXPECT_EQ(sg_query_image_state(images[1]), SG_RESOURCESTATE_ALLOC) << "over the budget: the next frame";
    EXPECT_EQ(SystemRender::getUploadBytesLastFrame(), kUploadGpuBytes);
    EXPECT_EQ(SystemRender::getDeferredUploadCount(), 3u);
    EXPECT_FALSE(SystemRender::isUploadTokenComplete(token));

    int frames = 1;
    while (SystemRender::getDeferredUploadCount() > 0 && frames < 10){
        EXPECT_EQ(cleanups, 0) << "the cleanup waits for the creations before it";
        SystemRender::executeQueue();
        EXPECT_LE(SystemRender::getUploadBytesLastFrame(), kUploadGpuBytes + kUploadGpuBytes / 2);
        frames++;
    }
    EXPECT_EQ(frames, 4) << "one texture a frame";
    for (sg_image image : images)
        EXPECT_EQ(sg_query_image_state(image), SG_RESOURCESTATE_VALID);
    EXPECT_TRUE(SystemRender::isUploadTokenComplete(token));
    EXPECT_EQ(cleanups, 1) << "the frame of the last creation ran it";

    for (sg_image image : images)
        sg_destroy_image(image);
}
