// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// DrawBatcher: the runs of Dynamic Batching (sprite-batching spec 6.9). Objects of equal keys
// that come one after the other share a run; in REORDER mode an object joins an earlier run only
// past objects its bounds do not touch and of its own sort bucket; GROUP joins any run of its key;
// runs draw in the order they opened; why an object did not join the run before it is counted.

#include "render/DrawBatcher.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

namespace {
    struct BatcherDrawn{
        Entity head;
        uint32_t items;
        uint32_t vertices;
        bool allQuads;
        BatchBreak openedBy;
    };

    struct BatcherRecorder{
        std::vector<BatcherDrawn> runs;
        DrawBatcher* batcher = nullptr;
        int requests = 0;
    };
    BatcherRecorder* batcherRecorder = nullptr;

    void batcherDrawRun(void* owner, BatchRun& run){
        BatcherRecorder* recorder = static_cast<BatcherRecorder*>(owner);
        recorder->runs.push_back({run.head.entity, run.items, run.vertexCount, run.allQuads, run.openedBy});
    }

    void batcherRequestFlush(){
        batcherRecorder->requests++;
        batcherRecorder->batcher->flush();
    }

    class DrawBatcherTest : public ::testing::Test {
    protected:
        DrawBatcher batcher;
        BatcherRecorder recorder;

        void SetUp() override{
            recorder.batcher = &batcher;
            batcherRecorder = &recorder;
            batcher.setCallbacks(&batcherDrawRun, &batcherRequestFlush, &recorder);
        }

        void TearDown() override{
            batcherRecorder = nullptr;
        }

        static BatchKey key(uint32_t pipeline){
            BatchKey k;
            k.pipeline = pipeline;
            return k;
        }

        // a quad at x..x+1, y..y+1
        void add(Entity entity, const BatchKey& k, float x, float y, uint64_t sortKey = 0, uint32_t vertices = 4){
            BatchItem item;
            item.entity = entity;
            item.sortKey = sortKey;
            item.minX = x;
            item.minY = y;
            item.maxX = x + 1.0f;
            item.maxY = y + 1.0f;
            item.vertexCount = vertices;
            item.indexCount = 6;
            BatchHead head;
            head.entity = entity;
            BatchRun& run = batcher.place(k, head, item, 16);
            const uint32_t first = run.vertexCount;
            run.appendVertices(vertices);
            static const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
            run.appendIndices(quad, 6, first);
        }
    };
}

TEST_F(DrawBatcherTest, StrictJoinsOnlyTheRunBefore){
    batcher.beginPass(BatchMode::STRICT, 1);
    add(1, key(10), 0, 0);
    add(2, key(10), 0, 0);
    add(3, key(20), 5, 5);
    add(4, key(10), 10, 10);
    batcher.flush();

    ASSERT_EQ(recorder.runs.size(), 3u);
    EXPECT_EQ(recorder.runs[0].head, 1u);
    EXPECT_EQ(recorder.runs[0].items, 2u);
    EXPECT_EQ(recorder.runs[0].vertices, 8u);
    EXPECT_TRUE(recorder.runs[0].allQuads);
    EXPECT_EQ(recorder.runs[1].head, 3u);
    EXPECT_EQ(recorder.runs[1].openedBy, BatchBreak::PIPELINE);
    EXPECT_EQ(recorder.runs[2].head, 4u);
    EXPECT_EQ(batcher.getStats().batches, 1u);
    EXPECT_EQ(batcher.getStats().batchedObjects, 2u);
    EXPECT_EQ(batcher.getStats().singleDraws, 2u);
    EXPECT_EQ(batcher.getStats().savedByBatching(), 1u);
    EXPECT_EQ(batcher.getStats().breaks[(size_t)BatchBreak::PIPELINE], 2u);
    ASSERT_EQ(batcher.getBreakLog().size(), 2u);
    EXPECT_EQ(batcher.getBreakLog()[0].entity, 3u);
    EXPECT_EQ(batcher.getBreakLog()[0].previous, 2u);
}

TEST_F(DrawBatcherTest, ReorderJoinsPastDisjointObjectsOnly){
    batcher.beginPass(BatchMode::REORDER, 8);
    // A B A B, none touching: two runs, the second A and B jump back
    add(1, key(10), 0, 0);
    add(2, key(20), 2, 0);
    add(3, key(10), 4, 0);
    add(4, key(20), 6, 0);
    batcher.flush();
    ASSERT_EQ(recorder.runs.size(), 2u);
    EXPECT_EQ(recorder.runs[0].items, 2u);
    EXPECT_EQ(recorder.runs[1].items, 2u);
    EXPECT_EQ(batcher.getStats().reordered, 1u);

    // A B A with the second A over B: it cannot move before B
    recorder.runs.clear();
    batcher.beginPass(BatchMode::REORDER, 8);
    add(1, key(10), 0, 0);
    add(2, key(20), 2, 0);
    add(3, key(10), 2.5f, 0.5f);
    batcher.flush();
    ASSERT_EQ(recorder.runs.size(), 3u);
    EXPECT_EQ(recorder.runs[2].head, 3u);
    EXPECT_EQ(recorder.runs[2].openedBy, BatchBreak::PIPELINE);
}

TEST_F(DrawBatcherTest, ReorderStaysInsideItsSortBucket){
    batcher.beginPass(BatchMode::REORDER, 8);
    add(1, key(10), 0, 0, 1);
    add(2, key(20), 2, 0, 2);
    add(3, key(10), 4, 0, 2);
    batcher.flush();
    // the second A would jump over B of another bucket
    ASSERT_EQ(recorder.runs.size(), 3u);
}

// S52 R7: a run holds one sort bucket (a Sorting Layer, an Order in Layer, a Sorting Group): an
// object of the same key in the next bucket opens a run, counted as sort_bucket
TEST_F(DrawBatcherTest, ARunHoldsOneSortBucket){
    batcher.beginPass(BatchMode::STRICT, 1);
    add(1, key(10), 0, 0, 7);
    add(2, key(10), 2, 0, 7);
    add(3, key(10), 4, 0, 9);
    batcher.flush();
    ASSERT_EQ(recorder.runs.size(), 2u);
    EXPECT_EQ(recorder.runs[0].items, 2u);
    EXPECT_EQ(recorder.runs[1].items, 1u);
    EXPECT_EQ(batcher.getStats().breaks[(size_t)BatchBreak::SORT_BUCKET], 1u);
    EXPECT_STREQ(getBatchBreakName(BatchBreak::SORT_BUCKET), "sort_bucket");
}

// S52 §6.4: the new reasons keep stable names, two key parts and two reasons to draw alone
TEST_F(DrawBatcherTest, PixelArtReasonsHaveStableNames){
    EXPECT_STREQ(getBatchBreakName(BatchBreak::PIXEL_SNAP), "pixel_snap");
    EXPECT_STREQ(getBatchBreakName(BatchBreak::CUSTOM_PARAMS), "custom_params");
    EXPECT_STREQ(getBatchBreakName(BatchBreak::INELIGIBLE_PIXEL_SNAP_PIVOT), "ineligible_pixel_snap_pivot");
    EXPECT_STREQ(getBatchBreakName(BatchBreak::INELIGIBLE_FRAME_RECT), "ineligible_frame_rect");
    EXPECT_FALSE(isIneligibleBreak(BatchBreak::PIXEL_SNAP));
    EXPECT_FALSE(isIneligibleBreak(BatchBreak::CUSTOM_PARAMS));
    EXPECT_TRUE(isIneligibleBreak(BatchBreak::INELIGIBLE_PIXEL_SNAP_PIVOT));
    EXPECT_TRUE(isIneligibleBreak(BatchBreak::INELIGIBLE_FRAME_RECT));
    EXPECT_TRUE(isIneligibleBreak(BatchBreak::DISABLED));
    // a key section of the new reasons names the difference
    BatchKey a = key(10);
    BatchKey b = key(10);
    const uint8_t one = 1, two = 2;
    a.addSection(BatchBreak::PIXEL_SNAP, &one, 1);
    b.addSection(BatchBreak::PIXEL_SNAP, &two, 1);
    EXPECT_EQ(a.compare(b), BatchBreak::PIXEL_SNAP);
}

TEST_F(DrawBatcherTest, GroupJoinsAnyRunOfItsKey){
    batcher.beginPass(BatchMode::GROUP, 16);
    add(1, key(10), 0, 0);
    add(2, key(20), 0, 0);
    add(3, key(10), 0, 0);
    add(4, key(20), 0, 0);
    batcher.flush();
    ASSERT_EQ(recorder.runs.size(), 2u);
    EXPECT_EQ(recorder.runs[0].items, 2u);
    EXPECT_EQ(recorder.runs[1].items, 2u);
}

TEST_F(DrawBatcherTest, AFullWindowFlushesThroughTheRenderer){
    batcher.beginPass(BatchMode::REORDER, 2);
    add(1, key(10), 0, 0);
    add(2, key(20), 0, 0);
    EXPECT_EQ(recorder.requests, 0);
    add(3, key(30), 0, 0);
    EXPECT_EQ(recorder.requests, 1);
    EXPECT_EQ(recorder.runs.size(), 2u);
    batcher.flush();
    EXPECT_EQ(recorder.runs.size(), 3u);
}

TEST_F(DrawBatcherTest, RunsStopAt65536Vertices){
    batcher.beginPass(BatchMode::STRICT, 1);
    add(1, key(10), 0, 0, 0, 40000);
    add(2, key(10), 0, 0, 0, 40000);
    batcher.flush();
    ASSERT_EQ(recorder.runs.size(), 2u);
    EXPECT_EQ(recorder.runs[1].openedBy, BatchBreak::RUN_VERTEX_LIMIT);
    EXPECT_FALSE(recorder.runs[0].allQuads) << "40,000 vertices with quad indices of 4 are not quads";
}

TEST_F(DrawBatcherTest, KeySectionsNameTheDifference){
    BatchKey a = key(10);
    BatchKey b = key(10);
    const float white[4] = {1, 1, 1, 1};
    const float red[4] = {1, 0, 0, 1};
    const float cutoff = 0.5f;
    ASSERT_TRUE(a.addSection(BatchBreak::COLOR_FACTOR, white, sizeof(white)));
    ASSERT_TRUE(a.addSection(BatchBreak::ALPHA_CUTOFF, &cutoff, sizeof(cutoff)));
    ASSERT_TRUE(b.addSection(BatchBreak::COLOR_FACTOR, white, sizeof(white)));
    ASSERT_TRUE(b.addSection(BatchBreak::ALPHA_CUTOFF, &cutoff, sizeof(cutoff)));
    EXPECT_EQ(a.compare(b), BatchBreak::NONE);

    BatchKey c = key(10);
    ASSERT_TRUE(c.addSection(BatchBreak::COLOR_FACTOR, red, sizeof(red)));
    ASSERT_TRUE(c.addSection(BatchBreak::ALPHA_CUTOFF, &cutoff, sizeof(cutoff)));
    EXPECT_EQ(a.compare(c), BatchBreak::COLOR_FACTOR);

    BatchKey d = b;
    d.cell[0] = 1;
    EXPECT_EQ(a.compare(d), BatchBreak::ORIGIN_CELL);
    BatchKey e = b;
    e.mirrored = true;
    EXPECT_EQ(a.compare(e), BatchBreak::MIRRORED);
    BatchKey f = b;
    f.scissor.set = true;
    f.scissor.width = 10;
    EXPECT_EQ(a.compare(f), BatchBreak::SCISSOR);
    EXPECT_STREQ(getBatchBreakName(BatchBreak::COLOR_FACTOR), "color_factor");
    EXPECT_STREQ(getBatchBreakName(BatchBreak::INELIGIBLE_CUSTOM_SHADER), "ineligible_custom_shader");
}

TEST_F(DrawBatcherTest, AForeignDrawIsTheNextBreak){
    batcher.beginPass(BatchMode::STRICT, 1);
    add(1, key(10), 0, 0);
    batcher.flush(); // something else drew
    add(2, key(10), 0, 0);
    batcher.flush();
    ASSERT_EQ(recorder.runs.size(), 2u);
    EXPECT_EQ(recorder.runs[1].openedBy, BatchBreak::FOREIGN_DRAW);
    EXPECT_EQ(batcher.getStats().breaks[(size_t)BatchBreak::FOREIGN_DRAW], 1u);
}
