// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upload pacing (spec S56 R8, AC3), the planning half: SokolCmdQueue::select_commands spreads
// uploads larger than the budget across frames, keeps each frame within it, runs an item larger
// than the budget alone, keeps what depends on a waiting upload behind it and runs the rest.
// UploadBudgetGpuTest runs the real queue and its readiness token.

#include "sokol/SokolCmdQueue.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

namespace {

    using UploadType = SokolRenderCommand::TYPE;
    constexpr uint64_t kUploadMB = 1024ull * 1024ull;

    uint64_t uploadNextSeq = 1;

    SokolRenderCommand uploadCommand(UploadType::ENUM type){
        SokolRenderCommand c(type);
        c.seq = uploadNextSeq++;
        return c;
    }

    SokolRenderCommand uploadMakeImage(uint32_t id, uint64_t bytes){
        SokolRenderCommand c = uploadCommand(UploadType::MAKE_IMAGE);
        c.make_image.desc = sg_image_desc{};
        c.make_image.desc.data.mip_levels[0].ptr = &uploadNextSeq;
        c.make_image.desc.data.mip_levels[0].size = bytes;
        c.make_image.image = {id};
        return c;
    }

    SokolRenderCommand uploadUpdateImage(uint32_t id, uint64_t bytes){
        SokolRenderCommand c = uploadCommand(UploadType::UPDATE_IMAGE);
        c.update_image.data = sg_image_data{};
        c.update_image.data.mip_levels[0].ptr = &uploadNextSeq;
        c.update_image.data.mip_levels[0].size = bytes;
        c.update_image.image = {id};
        return c;
    }

    SokolRenderCommand uploadMakeView(uint32_t id, uint32_t image){
        SokolRenderCommand c = uploadCommand(UploadType::MAKE_VIEW);
        c.make_view.desc = sg_view_desc{};
        c.make_view.desc.texture.image = {image};
        c.make_view.view = {id};
        return c;
    }

    SokolRenderCommand uploadMakeSampler(uint32_t id){
        SokolRenderCommand c = uploadCommand(UploadType::MAKE_SAMPLER);
        c.make_sampler.desc = sg_sampler_desc{};
        c.make_sampler.sampler = {id};
        return c;
    }

    SokolRenderCommand uploadDestroyImage(uint32_t id){
        SokolRenderCommand c = uploadCommand(UploadType::DESTROY_IMAGE);
        c.destroy_image.image = {id};
        return c;
    }

    SokolRenderCommand uploadCustom(){
        SokolRenderCommand c = uploadCommand(UploadType::CUSTOM);
        c.custom.custom_cb = nullptr;
        c.custom.custom_data = nullptr;
        return c;
    }

    // the frames select_commands takes to run all of `backlog`, each frame's bytes into `perFrame`
    int uploadFramesToDrain(std::vector<SokolRenderCommand> backlog, uint64_t budget, std::vector<uint64_t>& perFrame,
                      std::vector<uint64_t>* order = nullptr){
        int frames = 0;
        while (!backlog.empty() && frames < 1000){
            std::vector<SokolRenderCommand> run;
            perFrame.push_back(SokolCmdQueue::select_commands(backlog, budget, run));
            if (order)
                for (const SokolRenderCommand& c : run) order->push_back(c.seq);
            frames++;
        }
        return frames;
    }

}

TEST(UploadBudget, BytesOfACommand){
    EXPECT_EQ(SokolCmdQueue::upload_bytes(uploadMakeImage(1, 3 * kUploadMB)), 3 * kUploadMB);
    EXPECT_EQ(SokolCmdQueue::upload_bytes(uploadUpdateImage(1, 5)), 5u);
    EXPECT_EQ(SokolCmdQueue::upload_bytes(uploadMakeSampler(1)), 0u);
    SokolRenderCommand empty = uploadCommand(UploadType::MAKE_IMAGE);
    empty.make_image.desc = sg_image_desc{};
    EXPECT_EQ(SokolCmdQueue::upload_bytes(empty), 0u) << "a render target has no data";
}

// N textures larger than the budget: one a frame, each frame within the budget but the first
TEST(UploadBudget, LargeTexturesSpreadAcrossFrames){
    std::vector<SokolRenderCommand> backlog;
    for (uint32_t i = 0; i < 5; i++)
        backlog.push_back(uploadMakeImage(100 + i, 3 * kUploadMB));
    std::vector<uint64_t> perFrame;
    std::vector<uint64_t> order;
    EXPECT_EQ(uploadFramesToDrain(backlog, 4 * kUploadMB, perFrame, &order), 5);
    for (uint64_t bytes : perFrame)
        EXPECT_EQ(bytes, 3 * kUploadMB);
    for (size_t i = 1; i < order.size(); i++)
        EXPECT_LT(order[i - 1], order[i]) << "uploads keep their order";

    perFrame.clear();
    EXPECT_EQ(uploadFramesToDrain(backlog, 8 * kUploadMB, perFrame), 3) << "two of 3 MB fit in 8 MB";
    for (uint64_t bytes : perFrame)
        EXPECT_LE(bytes, 8 * kUploadMB);
}

TEST(UploadBudget, OversizeItemGoesAlone){
    std::vector<SokolRenderCommand> backlog = {uploadMakeImage(1, 1 * kUploadMB), uploadMakeImage(2, 20 * kUploadMB), uploadMakeImage(3, 1 * kUploadMB)};
    std::vector<SokolRenderCommand> run;
    EXPECT_EQ(SokolCmdQueue::select_commands(backlog, 8 * kUploadMB, run), 1 * kUploadMB);
    ASSERT_EQ(run.size(), 1u);
    EXPECT_EQ(run[0].make_image.image.id, 1u);
    // the next frame: the 20 MB item first, alone
    run.clear();
    EXPECT_EQ(SokolCmdQueue::select_commands(backlog, 8 * kUploadMB, run), 20 * kUploadMB);
    ASSERT_EQ(run.size(), 1u);
    EXPECT_EQ(run[0].make_image.image.id, 2u);
    run.clear();
    EXPECT_EQ(SokolCmdQueue::select_commands(backlog, 8 * kUploadMB, run), 1 * kUploadMB);
    EXPECT_TRUE(backlog.empty());
}

// what refers to a waiting image waits with it; what does not runs now; a CUSTOM (an object
// marked loaded) never overtakes a waiting command, nor does anything after it
TEST(UploadBudget, DependentsWaitTheRestRuns){
    std::vector<SokolRenderCommand> backlog = {
        uploadMakeImage(1, 6 * kUploadMB),   // runs
        uploadMakeImage(2, 6 * kUploadMB),   // waits: over the budget
        uploadMakeSampler(10),        // runs: no dependency
        uploadMakeView(20, 2),        // waits: its image waits
        uploadMakeView(21, 1),        // runs
        uploadDestroyImage(2),        // waits
        uploadCustom(),               // waits: it would overtake image 2
        uploadMakeSampler(11),        // waits: after the barrier
    };
    std::vector<SokolRenderCommand> run;
    EXPECT_EQ(SokolCmdQueue::select_commands(backlog, 8 * kUploadMB, run), 6 * kUploadMB);
    ASSERT_EQ(run.size(), 3u);
    EXPECT_EQ(run[0].type, UploadType::MAKE_IMAGE);
    EXPECT_EQ(run[1].type, UploadType::MAKE_SAMPLER);
    EXPECT_EQ(run[2].make_view.view.id, 21u);
    ASSERT_EQ(backlog.size(), 5u);
    EXPECT_EQ(backlog[0].make_image.image.id, 2u);
    EXPECT_EQ(backlog[1].make_view.view.id, 20u);
    EXPECT_EQ(backlog[2].type, UploadType::DESTROY_IMAGE);
    EXPECT_EQ(backlog[3].type, UploadType::CUSTOM);
    EXPECT_EQ(backlog[4].make_sampler.sampler.id, 11u);
    // the next frame runs all of it, in order
    run.clear();
    SokolCmdQueue::select_commands(backlog, 8 * kUploadMB, run);
    EXPECT_TRUE(backlog.empty());
    ASSERT_EQ(run.size(), 5u);
    for (size_t i = 1; i < run.size(); i++)
        EXPECT_LT(run[i - 1].seq, run[i].seq);
}

// sokol updates a resource once a frame: a second update of the same image waits a frame
TEST(UploadBudget, SecondUpdateOfAHandleWaits){
    std::vector<SokolRenderCommand> backlog = {uploadUpdateImage(5, 100), uploadUpdateImage(5, 100), uploadUpdateImage(6, 100)};
    std::vector<SokolRenderCommand> run;
    SokolCmdQueue::select_commands(backlog, 8 * kUploadMB, run);
    ASSERT_EQ(run.size(), 2u);
    EXPECT_EQ(run[0].update_image.image.id, 5u);
    EXPECT_EQ(run[1].update_image.image.id, 6u);
    ASSERT_EQ(backlog.size(), 1u);
    EXPECT_EQ(backlog[0].update_image.image.id, 5u);
}

TEST(UploadBudget, EmptyBacklogCostsNothing){
    std::vector<SokolRenderCommand> backlog;
    std::vector<SokolRenderCommand> run;
    EXPECT_EQ(SokolCmdQueue::select_commands(backlog, 8 * kUploadMB, run), 0u);
    EXPECT_TRUE(run.empty());
}
