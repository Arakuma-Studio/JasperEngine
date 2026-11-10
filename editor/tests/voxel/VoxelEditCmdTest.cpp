// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R29 (AC28): undo and redo swap block references, byte-exact, and what the steps keep counts

#include "VoxelTestScene.h"

#include "command/type/VoxelEditCmd.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::voxeltest;

namespace {
    class VoxelEditCmdTest : public VoxelSceneTest {};
}

TEST_F(VoxelEditCmdTest, UndoRedoExact) {
    addVolume();
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();
    sceneProject->isModified = false;
    const uint64_t start = hash();
    const int64_t usedAtStart = VoxelAssets::sharedBudget()->used();
    const int steps = 100;
    for (int i = 0; i < steps; i++) {
        ASSERT_TRUE(history->addCommandNoMerge(new VoxelEditCmd(project.get(), sceneProject->id, owner, "Dig Sphere", edit(i))));
    }
    const uint64_t edited = hash();
    EXPECT_NE(edited, start);
    EXPECT_EQ(history->entries().size(), static_cast<size_t>(steps));
    EXPECT_TRUE(sceneProject->isModified);
    EXPECT_TRUE(asset->dirty);
    ASSERT_NE(history->top(), nullptr);
    EXPECT_EQ(history->top()->name(), "Dig Sphere (Cave)");
    EXPECT_GT(history->top()->info.bytes, sizeof(VoxelEditCmd));
    const int64_t usedEdited = VoxelAssets::sharedBudget()->used();
    EXPECT_GT(usedEdited, usedAtStart);

    for (int i = 0; i < steps; i++) ASSERT_TRUE(history->undo());
    EXPECT_EQ(hash(), start);
    EXPECT_FALSE(sceneProject->isModified);
    for (int i = 0; i < steps; i++) ASSERT_TRUE(history->redo());
    EXPECT_EQ(hash(), edited);
    EXPECT_TRUE(sceneProject->isModified);

    // the steps' memory is the budget's until they go
    const int64_t usedRedone = VoxelAssets::sharedBudget()->used();
    EXPECT_EQ(usedRedone, usedEdited);
    history->clear();
    EXPECT_LT(VoxelAssets::sharedBudget()->used(), usedRedone);
}

// S24's cap frees a step's blocks from the budget like its bytes from the history
TEST_F(VoxelEditCmdTest, FollowsTheHistorysDepthCap) {
    addVolume();
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();
    const size_t cap = CommandHistory::maxEntries;
    CommandHistory::maxEntries = 3;
    const int64_t usedAtStart = VoxelAssets::sharedBudget()->used();
    const int64_t storeAtStart = asset->store->memory().total();
    for (int i = 0; i < 12; i++) {
        ASSERT_TRUE(history->addCommandNoMerge(new VoxelEditCmd(project.get(), sceneProject->id, owner, "Dig Sphere", edit(i))));
    }
    int64_t kept = 0;
    EXPECT_EQ(history->entries().size(), 3u);
    for (const UndoEntry* entry : history->entries()) kept += static_cast<int64_t>(entry->info.bytes);
    CommandHistory::maxEntries = cap;
    // nothing of the nine older steps stays charged: what remains over the start is the store's own
    // growth and at most the three steps kept
    const int64_t storeGrowth = asset->store->memory().total() - storeAtStart;
    EXPECT_LE(VoxelAssets::sharedBudget()->used() - usedAtStart, storeGrowth + kept);
    history->clear();
}

// A stroke's ticks are one step, back to each block's oldest state
TEST_F(VoxelEditCmdTest, ContinuousStrokeMergesToOneStep) {
    addVolume();
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();
    const uint64_t start = hash();
    for (int tick = 0; tick < 5; tick++) {
        // the same block again and again
        voxel::EditOp op = digAt(0).front();
        op.shape.radius = 2.0 + tick;
        voxel::EditOptions options;
        options.captureDelta = true;
        const voxel::EditResult result = system->edit(owner, std::span<const voxel::EditOp>(&op, 1), options);
        ASSERT_TRUE(result.ok);
        ASSERT_TRUE(history->addCommand(new VoxelEditCmd(project.get(), sceneProject->id, owner, "Dig Sphere", result.delta, true)));
    }
    const uint64_t edited = hash();
    EXPECT_EQ(history->entries().size(), 1u);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(hash(), start);
    ASSERT_TRUE(history->redo());
    EXPECT_EQ(hash(), edited);
    history->clear();
}

// A delta that changed nothing is no step
TEST_F(VoxelEditCmdTest, NoDeltaNoStep) {
    addVolume();
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    history->clear();
    sceneProject->isModified = false;
    ASSERT_TRUE(history->addCommandNoMerge(new VoxelEditCmd(project.get(), sceneProject->id, owner, "Dig Sphere", {})));
    EXPECT_TRUE(history->entries().empty());
    EXPECT_FALSE(sceneProject->isModified);
}
