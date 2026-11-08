// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Which tiles stream, in what order, and what leaves memory (T4 R8, R13-R16, R34; AC14-AC16)

#include "TerrainWorldTestSupport.h"
#include "terrain/world/TileStreamer.h"

#include <gtest/gtest.h>

#include <map>

using namespace terrainworldtest;

namespace {

    // Loads that finish when the test says so
    class FakeIO final : public TileIO {
    public:
        struct Load {
            TileCoord tile;
            TilePriority priority;
            bool cancelled = false;
            int finish = 0;     // 0 pending, 1 done, -1 failed
        };

        explicit FakeIO(const WorldLattice& lattice): lattice(lattice) {}

        uint64_t begin(TileCoord tile, TilePriority priority) override {
            loads[++next] = Load{tile, priority};
            order.push_back(tile);
            return next;
        }

        TileLoadStatus poll(uint64_t handle, std::shared_ptr<TileData>& data, std::string& error) override {
            Load& load = loads[handle];
            if (autoFinish && load.finish == 0) load.finish = failing.count(load.tile.key()) ? -1 : 1;
            if (load.finish == 0) return TileLoadStatus::Pending;
            if (load.finish < 0) {
                error = "read failed";
                return TileLoadStatus::Failed;
            }
            auto tile = std::make_shared<TileData>();
            initTile(*tile, lattice, load.tile, 100);
            data = tile;
            return TileLoadStatus::Done;
        }

        void cancel(uint64_t handle) override { loads[handle].cancelled = true; }

        int inFlight() const {
            int count = 0;
            for (const auto& [handle, load] : loads) count += load.finish == 0 && !load.cancelled ? 1 : 0;
            return count;
        }

        WorldLattice lattice;
        std::map<uint64_t, Load> loads;
        std::vector<TileCoord> order;
        std::map<uint32_t, bool> failing;
        bool autoFinish = false;
        uint64_t next = 0;
    };

    struct Stream {
        WorldLattice lattice = smallLattice(64, 1.0f);
        WorldOccupancy occupancy;
        FakeIO io{lattice};
        TileStreamer streamer{io, occupancy};

        explicit Stream(int half = 10) {
            for (int z = -half; z < half; z++) {
                for (int x = -half; x < half; x++) occupancy.setTile(TileCoord(x, z), true);
            }
            streamer.setLattice(lattice);
        }

        void focus(float x, float z, float vx = 0.0f, float vz = 0.0f) {
            StreamFocus f;
            f.x = x;
            f.z = z;
            f.velocityX = vx;
            f.velocityZ = vz;
            streamer.setFocuses({f});
        }

        // runs frames until nothing is loading, uploading everything planned
        void settle(double& now, int frames = 200) {
            io.autoFinish = true;
            for (int i = 0; i < frames; i++) {
                now += 0.05;
                streamer.update(now);
                for (const TileCoord tile : streamer.planUploads()) streamer.uploaded(tile, streamer.pageBytes(*streamer.data(tile)));
            }
        }
    };

}

TEST(TileStreamer, PriorityByDistance) {
    Stream stream;
    stream.streamer.budgets().maxInFlight = 3;
    stream.focus(10.0f, 10.0f);
    stream.streamer.update(0.0);
    ASSERT_EQ(stream.io.order.size(), 3u) << "maxInFlight at once";
    EXPECT_EQ(stream.io.order[0], TileCoord(0, 0)) << "the focus's tile first";
    EXPECT_EQ(stream.io.loads[1].priority, TilePriority::Critical);
    EXPECT_EQ(stream.io.loads[2].priority, TilePriority::High);
    // the rest wait in the queue, nearest first
    EXPECT_GT(stream.streamer.stats().queued, 0);
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Loading);
    EXPECT_EQ(stream.streamer.state(TileCoord(9, 9)), TileState::Cold);
    EXPECT_EQ(stream.streamer.state(TileCoord(50, 50)), TileState::Absent);
}

TEST(TileStreamer, Reprioritises) {
    Stream stream;
    stream.streamer.budgets().maxInFlight = 1;
    stream.focus(10.0f, 10.0f);
    stream.streamer.update(0.0);
    ASSERT_EQ(stream.io.order.back(), TileCoord(0, 0));
    stream.io.loads[1].finish = 1;
    // the focus moves before the next re-sort tick
    stream.focus(-300.0f, 200.0f);
    stream.streamer.update(0.02);
    EXPECT_EQ(stream.io.order.back(), TileCoord(tileOfLocal(-300.0f, 64.0f), tileOfLocal(200.0f, 64.0f)))
        << "a new focus re-sorts at once, and its tile is first";
    // the velocity's predicted position is wanted at Normal
    stream.focus(10.0f, 10.0f, 200.0f, 0.0f);
    stream.streamer.update(0.2);
    EXPECT_TRUE(stream.streamer.wanted(TileCoord(tileOfLocal(10.0f + 300.0f, 64.0f), 0)));
}

TEST(TileStreamer, CancelsOutsideRadius) {
    Stream stream;
    stream.streamer.budgets().maxInFlight = 4;
    stream.focus(10.0f, 10.0f);
    stream.streamer.update(0.0);
    const uint64_t firstLoad = 1;
    ASSERT_FALSE(stream.io.loads[firstLoad].cancelled);
    // far away: the old loads are more than 2 S past every band and go
    stream.focus(10.0f + 64.0f * 8.0f, 10.0f);
    stream.streamer.update(0.2);
    EXPECT_TRUE(stream.io.loads[firstLoad].cancelled);
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Cold);
    EXPECT_GT(stream.streamer.stats().cancelled, 0);
    EXPECT_LE(stream.io.inFlight(), 4);
}

TEST(TileStreamer, CancelledTileReturns) {
    Stream stream;
    stream.focus(10.0f, 10.0f);
    stream.streamer.update(0.0);
    stream.focus(10.0f + 64.0f * 8.0f, 10.0f);
    stream.streamer.update(0.2);
    ASSERT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Cold);
    // back: the cancelled tile is demanded again and loads (the godot_voxel hole)
    stream.focus(10.0f, 10.0f);
    double now = 0.4;
    stream.settle(now);
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Resident);
}

TEST(TileStreamer, ReadFailureRetriesThenError) {
    Stream stream;
    stream.io.failing[TileCoord(0, 0).key()] = true;
    stream.focus(10.0f, 10.0f);
    double now = 0.0;
    stream.io.autoFinish = true;
    int beginsOfTile = 0;
    std::vector<TileStreamEvent> errors;
    for (int frame = 0; frame < 200; frame++) {
        now += 0.05;
        stream.streamer.update(now);
        for (const TileStreamEvent& event : stream.streamer.takeEvents()) {
            if (event.kind == TileStreamEvent::Kind::Error) errors.push_back(event);
        }
    }
    for (const TileCoord tile : stream.io.order) beginsOfTile += tile == TileCoord(0, 0) ? 1 : 0;
    EXPECT_EQ(beginsOfTile, 1 + TileStreamer::MAX_RETRIES) << "three retries after the first read";
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Error);
    ASSERT_EQ(errors.size(), 1u) << "logged once";
    EXPECT_EQ(errors[0].message, "read failed");
    // a changed file clears it
    stream.io.failing.clear();
    stream.streamer.invalidate(TileCoord(0, 0));
    stream.settle(now);
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Resident);
}

TEST(TileStreamer, LruEvicts) {
    Stream stream(20);
    stream.streamer.budgets().poolSize = 12;
    double now = 0.0;
    stream.focus(10.0f, 10.0f);
    stream.settle(now);
    const int firstWave = stream.streamer.stats().residentPages;
    EXPECT_GT(firstWave, 0);
    EXPECT_LE(firstWave, 12);
    // a walk: the oldest pages go first, and the pool is never passed
    for (int step = 1; step <= 6; step++) {
        stream.focus(10.0f + 64.0f * 2.0f * static_cast<float>(step), 10.0f);
        stream.settle(now, 40);
        EXPECT_LE(stream.streamer.stats().residentPages, 12);
    }
    EXPECT_GT(stream.streamer.stats().evictedPages, 0);
    EXPECT_NE(stream.streamer.state(TileCoord(0, 0)), TileState::Resident) << "the first tile went long ago";
}

TEST(TileStreamer, NeverEvictsFocusOrPinned) {
    Stream stream(20);
    stream.streamer.budgets().poolSize = 4;
    double now = 0.0;
    stream.focus(10.0f, 10.0f);
    stream.settle(now);
    ASSERT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Resident);
    stream.streamer.pin(TileCoord(0, 0), true);
    stream.focus(10.0f + 64.0f * 10.0f, 10.0f);
    stream.settle(now);
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Resident) << "a pinned page stays";
    EXPECT_EQ(stream.streamer.state(TileCoord(10, 0)), TileState::Resident) << "the focus's tile has a page";
}

TEST(TileStreamer, RespectsMemoryCaps) {
    Stream stream(20);
    WorldLattice lattice = stream.lattice;
    TileData sample;
    initTile(sample, lattice, TileCoord(0, 0), 0);
    stream.streamer.budgets().cpuCapBytes = sample.cpuBytes() * 30;
    stream.streamer.budgets().gpuCapBytes = stream.streamer.pageBytes(sample) * 6;
    double now = 0.0;
    for (int step = 0; step < 8; step++) {
        stream.focus(10.0f + 64.0f * 2.0f * static_cast<float>(step), 10.0f + 64.0f * static_cast<float>(step % 3));
        stream.settle(now, 30);
        EXPECT_LE(stream.streamer.stats().gpuBytes, stream.streamer.budgets().gpuCapBytes);
        EXPECT_LE(stream.streamer.stats().cpuBytes, stream.streamer.budgets().cpuCapBytes);
    }
    EXPECT_GT(stream.streamer.stats().evictedCpu, 0);
}

TEST(TileStreamer, PoolExhaustedFallsBack) {
    Stream stream;
    stream.streamer.budgets().poolSize = 2;
    double now = 0.0;
    stream.focus(10.0f, 10.0f);
    stream.settle(now);
    EXPECT_EQ(stream.streamer.stats().residentPages, 2);
    EXPECT_GT(stream.streamer.stats().poolExhausted, 0) << "wanted tiles past the pool stay CPU-only and draw the overview";
    int cpuOnly = 0;
    for (const TileCoord tile : stream.streamer.knownTiles()) cpuOnly += stream.streamer.state(tile) == TileState::CpuReady ? 1 : 0;
    EXPECT_GT(cpuOnly, 0);
}

TEST(TileStreamer, UploadBudget) {
    Stream stream;
    double now = 0.0;
    stream.io.autoFinish = true;
    stream.focus(10.0f, 10.0f);
    stream.streamer.update(now);
    for (int frame = 0; frame < 5; frame++) {
        now += 0.05;
        stream.streamer.update(now);
    }
    TileData sample;
    initTile(sample, stream.lattice, TileCoord(0, 0), 0);
    const uint64_t page = stream.streamer.pageBytes(sample);
    // a budget of 2.5 pages takes 2; a budget under one page still takes one (progress)
    stream.streamer.budgets().uploadBytesPerFrame = page * 5 / 2;
    std::vector<TileCoord> plan = stream.streamer.planUploads();
    EXPECT_EQ(plan.size(), 2u);
    EXPECT_LE(stream.streamer.stats().uploadBytesLastFrame, page * 5 / 2);
    for (const TileCoord tile : plan) stream.streamer.uploadRefused(tile);
    EXPECT_EQ(stream.streamer.stats().uploadRetries, 2);
    stream.streamer.budgets().uploadBytesPerFrame = page / 3;
    plan = stream.streamer.planUploads();
    EXPECT_EQ(plan.size(), 1u);
}

TEST(TileStreamer, HeadlessStopsAtCpuReady) {
    Stream stream;
    stream.streamer.budgets().gpu = false;
    double now = 0.0;
    stream.focus(10.0f, 10.0f);
    stream.settle(now);
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::CpuReady);
    EXPECT_TRUE(stream.streamer.planUploads().empty());
}

TEST(TileStreamer, FreezeStopsEverything) {
    Stream stream;
    double now = 0.0;
    stream.focus(10.0f, 10.0f);
    stream.settle(now);
    const size_t loads = stream.io.order.size();
    stream.streamer.setFrozen(true);
    stream.focus(10.0f + 64.0f * 6.0f, 10.0f);
    stream.settle(now);
    EXPECT_EQ(stream.io.order.size(), loads) << "no load starts";
    EXPECT_EQ(stream.streamer.state(TileCoord(0, 0)), TileState::Resident) << "nothing is evicted";
    EXPECT_EQ(stream.streamer.stats().evictedPages, 0);
}
