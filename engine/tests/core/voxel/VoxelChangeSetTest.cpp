// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R10, R16, R17 (AC10, AC15, AC16): change sets never crack, whatever order jobs finish in

#include "VoxelTestSupport.h"
#include "voxel/VoxelChangeSet.h"
#include "voxel/VoxelClipbox.h"
#include "voxel/VoxelFile.h"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <map>

using namespace voxeltest;

namespace {

    // The screen and the jobs of one volume, with jobs finishing in a seeded order: all a real
    // VoxelSystem adds is chunks and buffers
    class FakeVolume {
    public:
        struct Pending {
            MeshId id;
            std::shared_ptr<const ReadView> view;
        };

        FakeVolume(Store& store, uint32_t seed, ChangeSetOptions options = {}): store(store), scheduler(options), random(seed) {}

        void setFocus(double x, double y, double z) {
            clipbox.update(x, y, z);
        }

        void wantLod() {
            std::vector<VisibleBlock> visible;
            clipbox.visibleBlocks(visible);
            scheduler.setDesiredLod(visible);
        }

        void collectRequests() {
            std::vector<MeshBuildRequest> requests;
            scheduler.takeRequests(requests);
            for (const MeshBuildRequest& request : requests) {
                // The job reads the store as it is at submit
                Pending job;
                job.id = request.id;
                job.view = store.readView(request.id.key, request.id.sideMask);
                pending.push_back(std::move(job));
                requested[request.id.key]++;
            }
        }

        // Finishes up to `count` jobs in a shuffled order. Returns activations seen.
        int step(int count) {
            collectRequests();
            std::shuffle(pending.begin(), pending.end(), random);
            for (int i = 0; i < count && !pending.empty(); i++) {
                Pending job = std::move(pending.back());
                pending.pop_back();
                finish(job);
            }
            std::vector<std::pair<MeshId, std::shared_ptr<const MeshOut>>> uploads;
            scheduler.takeUploads(uploads);
            for (const auto& upload : uploads) scheduler.meshUploaded(upload.first);
            int activations = 0;
            ChangeSetScheduler::Activation activation;
            while (scheduler.pollActivation(activation)) {
                activations++;
                applyActivation(activation);
            }
            if (scheduler.wantsLodRefresh() && scheduler.inFlight() == 0) {
                scheduler.clearLodRefresh();
                wantLod();
            }
            return activations;
        }

        void drain() {
            for (int i = 0; i < 100000; i++) {
                step(1000);
                collectRequests();
                if (pending.empty() && scheduler.idle()) return;
            }
            FAIL() << "the scheduler never went idle";
        }

        void applyActivation(const ChangeSetScheduler::Activation& activation) {
            for (const MeshId& id : activation.hide) screen.erase(id);
            for (const auto& shown : activation.show) screen[shown.first] = shown.second;
            activationCount++;
            if (auditEveryActivation) {
                const AuditResult audit = auditScreen();
                EXPECT_TRUE(audit.watertight) << "activation " << activationCount << ": " << audit.unbalancedEdges << " of " << audit.edgesChecked;
                if (!audit.watertight) audited = false;
            }
        }

        AuditResult auditScreen() const {
            std::vector<const MeshOut*> meshes;
            for (const auto& item : screen) meshes.push_back(item.second.get());
            return auditWatertight(meshes);
        }

        Store& store;
        Clipbox clipbox;
        ChangeSetScheduler scheduler;
        std::mt19937 random;
        std::vector<Pending> pending;
        std::map<MeshId, std::shared_ptr<const MeshOut>> screen;
        std::map<BlockKey, int> requested;
        int activationCount = 0;
        bool auditEveryActivation = true;
        bool audited = true;

    private:
        void finish(Pending& job) {
            auto mesh = std::make_shared<MeshOut>();
            MeshRequest request;
            request.key = job.id.key;
            request.sideMask = job.id.sideMask;
            meshBlock(*job.view, request, *mesh);
            store.publish(job.view->takeResolved());
            scheduler.meshBuilt(job.id, std::move(mesh));
        }
    };

}

TEST(VoxelChangeSet, CancelledJobsKeepOldVisible) {
    Store store(desc(128, 128, 128, 3));
    store.setBase(sphereBase(64.3, 63.7, 65.1, 41.3));
    FakeVolume volume(store, 1);
    volume.clipbox.configure(store.desc().dimensions, store.levels(), 4);
    volume.setFocus(64.0, 64.0, 64.0);
    volume.wantLod();
    volume.drain();
    const auto before = volume.screen;
    ASSERT_FALSE(before.empty());

    volume.setFocus(5.0, 64.0, 5.0);
    volume.wantLod();
    volume.collectRequests();
    ASSERT_FALSE(volume.pending.empty());
    // Every job gets dropped: nothing changes on screen while the set is short of meshes
    while (!volume.pending.empty()) {
        volume.scheduler.meshFailed(volume.pending.back().id);
        volume.pending.pop_back();
    }
    ChangeSetScheduler::Activation activation;
    EXPECT_FALSE(volume.scheduler.pollActivation(activation));
    EXPECT_EQ(volume.screen.size(), before.size());
    // The failed ones are asked for again, so the set still lands
    volume.drain();
    EXPECT_TRUE(volume.audited);
    EXPECT_NE(volume.screen.size(), 0u);
}

TEST(VoxelChangeSet, ShuffledCompletionNeverCracks) {
    Store store(desc(256, 128, 256, 3));
    store.setBase(wavyBase(60.0));
    std::mt19937 movement(77);
    for (uint32_t seed = 0; seed < 5; seed++) {
        FakeVolume volume(store, seed);
        volume.clipbox.configure(store.desc().dimensions, store.levels(), 4);
        volume.setFocus(100.0, 60.0, 100.0);
        volume.wantLod();
        volume.drain();
        for (int step = 0; step < 12; step++) {
            volume.setFocus(movement() % 256, 40 + movement() % 40, movement() % 256);
            volume.wantLod();
            // A few at a time: activations happen with most of the others still running
            for (int i = 0; i < 40; i++) volume.step(3);
            if (step % 4 == 0) {
                const EditOp op = digOp(sphereShape(movement() % 256, 55 + movement() % 10, movement() % 256, 4.0));
                const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
                if (result.ok && !result.changedVoxels.empty()) volume.scheduler.onEdit(result.changedVoxels, store.levels());
            }
        }
        volume.drain();
        EXPECT_TRUE(volume.audited) << seed;
    }
}

TEST(VoxelChangeSet, ActivatingBlockByBlockCracks) {
    // Red control of the design: show meshes per block as they finish instead of per set, and the audit fails
    Store store(desc(128, 128, 128, 3));
    store.setBase(sphereBase(64.3, 63.7, 65.1, 41.3));
    Clipbox clipbox;
    clipbox.configure(store.desc().dimensions, store.levels(), 4);
    clipbox.update(64.0, 64.0, 64.0);
    std::vector<VisibleBlock> first, second;
    clipbox.visibleBlocks(first);
    clipbox.update(10.0, 64.0, 10.0);
    clipbox.visibleBlocks(second);
    std::map<BlockKey, std::shared_ptr<const MeshOut>> screen;
    for (const VisibleBlock& block : first) screen[block.key] = meshOf(store, block.key, block.sideMask);
    std::mt19937 random(5);
    std::shuffle(second.begin(), second.end(), random);
    bool cracked = false;
    for (const VisibleBlock& block : second) {
        screen[block.key] = meshOf(store, block.key, block.sideMask);
        std::vector<const MeshOut*> meshes;
        for (const auto& item : screen) meshes.push_back(item.second.get());
        cracked = cracked || !auditWatertight(meshes).watertight;
    }
    EXPECT_TRUE(cracked);
}

TEST(VoxelChangeSet, EditPreemptsLod) {
    Store store(desc(256, 128, 256, 3));
    store.setBase(wavyBase(60.0));
    FakeVolume volume(store, 2);
    volume.clipbox.configure(store.desc().dimensions, store.levels(), 4);
    volume.setFocus(128.0, 60.0, 128.0);
    volume.wantLod();
    volume.drain();

    // A pending LOD set, half built
    volume.setFocus(20.0, 60.0, 20.0);
    volume.wantLod();
    volume.step(2);
    EXPECT_EQ(volume.scheduler.inFlight(), 1);
    const uint64_t cachedBefore = volume.scheduler.cachedMeshes();

    const EditOp op = digOp(sphereShape(128.0, 60.0, 128.0, 5.0));
    const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
    ASSERT_TRUE(result.ok);
    volume.scheduler.onEdit(result.changedVoxels, store.levels());
    std::vector<MeshId> cancelled;
    volume.scheduler.takeCancelled(cancelled);
    EXPECT_FALSE(cancelled.empty());
    // Only the edit set is in flight, and finished LOD meshes stayed in the cache
    EXPECT_EQ(volume.scheduler.inFlight(), 1);
    EXPECT_GE(volume.scheduler.cachedMeshes(), cachedBefore);
    // Its jobs are dropped by the queue in the real system
    for (size_t i = volume.pending.size(); i > 0; i--) {
        for (const MeshId& id : cancelled) {
            if (volume.pending[i - 1].id == id) {
                volume.pending.erase(volume.pending.begin() + static_cast<std::ptrdiff_t>(i - 1));
                break;
            }
        }
    }
    volume.drain();
    EXPECT_TRUE(volume.audited);
    // The screen holds what a fresh build at the focus would show
    std::vector<VisibleBlock> wanted;
    volume.clipbox.visibleBlocks(wanted);
    ASSERT_EQ(volume.scheduler.visible().size(), wanted.size());
    for (const VisibleBlock& block : wanted) {
        const auto found = volume.scheduler.visible().find(block.key);
        ASSERT_NE(found, volume.scheduler.visible().end());
        EXPECT_EQ(found->second.sideMask, block.sideMask);
    }
}

TEST(VoxelChangeSet, AtMostTwoInFlight) {
    Store store(desc(128, 128, 128, 2));
    store.setBase(wavyBase(60.0));
    FakeVolume volume(store, 3);
    volume.clipbox.configure(store.desc().dimensions, store.levels(), 4);
    volume.setFocus(64.0, 60.0, 64.0);
    volume.wantLod();
    volume.drain();
    for (int i = 0; i < 6; i++) {
        const EditOp op = digOp(sphereShape(20.0 + i * 15.0, 60.0, 64.0, 4.0));
        const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
        ASSERT_TRUE(result.ok);
        volume.scheduler.onEdit(result.changedVoxels, store.levels());
        // A set has one data version, so a job's view is taken with its request, before the next edit
        volume.collectRequests();
        EXPECT_LE(volume.scheduler.inFlight(), 2);
    }
    volume.drain();
    EXPECT_TRUE(volume.audited);
    EXPECT_EQ(volume.scheduler.inFlight(), 0);
}

TEST(TransvoxelMesher, CoarseNeverRemeshesForNeighbour) {
    // A scripted LOD move: a coarse block's mesh is asked for only when its own mask changes, and a
    // finer neighbour appearing never changes a coarse block's mask
    Store store(desc(256, 128, 256, 3));
    store.setBase(wavyBase(60.0));
    FakeVolume volume(store, 4);
    volume.clipbox.configure(store.desc().dimensions, store.levels(), 4);
    volume.setFocus(128.0, 60.0, 128.0);
    volume.wantLod();
    volume.drain();
    std::vector<VisibleBlock> before;
    volume.clipbox.visibleBlocks(before);
    std::map<BlockKey, uint8_t> maskBefore;
    for (const VisibleBlock& block : before) maskBefore[block.key] = block.sideMask;
    volume.requested.clear();
    volume.setFocus(128.0 + 40.0, 60.0, 128.0);
    volume.wantLod();
    volume.drain();
    std::vector<VisibleBlock> after;
    volume.clipbox.visibleBlocks(after);
    int stayed = 0;
    for (const VisibleBlock& block : after) {
        const auto found = maskBefore.find(block.key);
        if (found == maskBefore.end() || found->second != block.sideMask) continue;
        stayed++;
        EXPECT_EQ(volume.requested.count(block.key), 0u);
    }
    EXPECT_GT(stayed, 0);
    // The coarsest level has nothing coarser: its masks are always empty
    for (const VisibleBlock& block : after) {
        if (block.key.level == store.levels() - 1) {
            EXPECT_EQ(block.sideMask, 0);
        }
    }
}

namespace {

    EditOp randomOp(std::mt19937& random, const Int3& dim) {
        const auto rnd = [&](int n) { return static_cast<int>(random() % static_cast<uint32_t>(n)); };
        const double cx = rnd(dim.x), cy = 30 + rnd(40), cz = rnd(dim.z);
        Shape shape;
        switch (rnd(3)) {
            case 0:
                shape = sphereShape(cx, cy, cz, 1.0 + rnd(7));
                break;
            case 1: {
                shape.kind = ShapeKind::Box;
                shape.center = Vec3d{cx, cy, cz};
                shape.halfExtents = Vec3d{1.0 + rnd(6), 1.0 + rnd(6), 1.0 + rnd(6)};
                const double half = std::sqrt(0.5);
                if (rnd(2)) shape.rotation = Quatd{half, 0.0, half, 0.0};
                break;
            }
            default:
                shape.kind = ShapeKind::Capsule;
                shape.from = Vec3d{cx, cy, cz};
                shape.to = Vec3d{cx + rnd(30) - 15, cy + rnd(16) - 8, cz + rnd(30) - 15};
                shape.radius = 1.0 + rnd(4);
                break;
        }
        EditOp op;
        op.kind = static_cast<EditKind>(rnd(4));
        op.shape = shape;
        op.material = op.kind == EditKind::Paint ? rnd(6) : (rnd(2) ? rnd(6) : -1);
        op.blend = rnd(3) == 0 ? 1.5 : 0.0;
        op.iterations = 1 + rnd(2);
        op.amount = 0.5;
        return op;
    }

    void soak(int edits, ChangeSetOptions options, bool audit, bool& allAudited, bool& rebuildsMatch) {
        Store store(desc(192, 128, 192, 3));
        store.setBase(wavyBase(55.0));
        std::mt19937 random(2024);
        FakeVolume volume(store, 99, options);
        volume.auditEveryActivation = audit;
        volume.clipbox.configure(store.desc().dimensions, store.levels(), 4);
        volume.setFocus(96.0, 55.0, 96.0);
        volume.wantLod();
        volume.drain();
        for (int i = 0; i < edits; i++) {
            const EditOp op = randomOp(random, store.desc().dimensions);
            const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
            if (result.ok && !result.changedVoxels.empty()) volume.scheduler.onEdit(result.changedVoxels, store.levels());
            if (i % 37 == 0) {
                volume.setFocus(random() % 192, 40 + random() % 30, random() % 192);
                volume.wantLod();
            }
            volume.step(1 + static_cast<int>(random() % 12));
            if ((i + 1) % 250 == 0) {
                volume.drain();
                for (const auto& item : volume.screen) {
                    const auto rebuilt = meshOf(store, item.first.key, item.first.sideMask);
                    if (sortedTriangles(*rebuilt) != sortedTriangles(*item.second)) rebuildsMatch = false;
                }
                // The pyramid holds, and a file round trip keeps the hash
                for (int k = 0; k < 2000; k++) {
                    const int level = 1 + static_cast<int>(random() % 2);
                    const int x = static_cast<int>(random() % 96), y = static_cast<int>(random() % 64), z = static_cast<int>(random() % 96);
                    const int lx = x >> (level - 1), ly = y >> (level - 1), lz = z >> (level - 1);
                    if (store.sdfAt(level, lx, ly, lz) != store.sdfAt(0, lx << level, ly << level, lz << level)) rebuildsMatch = false;
                }
                const std::vector<uint8_t> file = saveVolumeFile(store, SaveOptions{});
                Store loaded(store.desc());
                loaded.setBase(store.base());
                if (!loadVolumeFile(file, loaded).ok || contentHash(loaded) != contentHash(store)) rebuildsMatch = false;
            }
        }
        volume.drain();
        allAudited = volume.audited;
    }

}

TEST(VoxelSoakSlow, IncrementalEqualsRebuild) {
    bool audited = true, rebuilds = true;
    soak(5000, ChangeSetOptions{}, true, audited, rebuilds);
    EXPECT_TRUE(audited);
    EXPECT_TRUE(rebuilds);
}

TEST(VoxelSoakSlow, RedControlMissingPadding) {
    // AC16's permanent negative case: with no padding a block misses changes in its neighbours'
    // samples, and the seams or the rebuild comparison must show it
    ChangeSetOptions options;
    options.dirtyPadding = 0;
    bool audited = true, rebuilds = true;
    ::testing::TestPartResultArray failures;
    {
        ::testing::ScopedFakeTestPartResultReporter reporter(::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ALL_THREADS, &failures);
        soak(300, options, true, audited, rebuilds);
    }
    EXPECT_TRUE(!audited || !rebuilds);
}
