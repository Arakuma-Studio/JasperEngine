// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PickCache (docs/specs/S26-scene-view.md R9, AC7): one tree per geometry shared by every entity
// drawing it, a new geometry gets a new tree and the old one goes 30 s after its last user, a
// query that does not wait reports a pending mesh by its bounds, one that waits builds it. The
// disk cache cases are the editor's (editor/tests/picking/BvhDiskCacheTest.cpp).

#include "picking/PickCache.h"
#include "picking/MeshRaycast.h"
#include "jobs/JobSystem.h"
#include "jobs/Primitives.h"
#include "object/Primitive.h"
#include "picking/PickingTestUtil.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // Holds every Background slot of the job system until released, so a queued build cannot
    // start: what a query sees then is a pending tree
    class BackgroundBlocker {
    public:
        BackgroundBlocker(){
            jobs::JobSystem& system = jobs::system();
            if (system.getWorkerCount() == 0){
                return; // single-threaded: nothing runs before a pump or a wait
            }
            const int slots = system.getGate().maxBackground();
            for (int i = 0; i < slots; i++){
                jobs::SubmitOptions options;
                options.lane = jobs::Lane::Background;
                options.name = "test-blocker";
                tasks.push_back(jobs::submit([this](){
                    started.release();
                    released.wait();
                }, options));
            }
            // every slot taken before the test queues its build
            for (int i = 0; i < slots; i++)
                started.acquire();
        }
        ~BackgroundBlocker(){
            released.set();
            for (jobs::TaskRef& task : tasks){
                task.wait();
            }
        }
    private:
        jobs::Event released;
        jobs::Semaphore started;
        std::vector<jobs::TaskRef> tasks;
    };

}

TEST(PickCache, SharedAcrossInstances){
    Scene scene;
    Primitive a(&scene);
    a.createSphere(1.0f, 64, 64);
    a.setPosition(-3, 0, 0);
    Primitive b(&scene);
    b.createSphere(1.0f, 64, 64);
    b.setPosition(3, 0, 0);
    settleScene(scene);

    const uint64_t buildsBefore = PickCache::get().stats().builds + PickCache::get().stats().diskLoads;
    MeshRaycast::prepare(&scene);
    SceneBvh& bvh = scene.getPickBvh();
    const PickCache::EntryPtr entryA = bvh.meshEntry(a.getEntity(), false);
    const PickCache::EntryPtr entryB = bvh.meshEntry(b.getEntity(), false);
    ASSERT_TRUE(entryA);
    EXPECT_EQ(entryA, entryB);
    EXPECT_GE(entryA->triangles, MeshBvh::BRUTE_FORCE_TRIANGLES);
    ASSERT_TRUE(PickCache::get().wait(entryA));

    EXPECT_EQ(MeshRaycast::raycast(&scene, Vector3(-3, 0, 5), Vector3(0, 0, -1), 10.0f).entity, a.getEntity());
    EXPECT_EQ(MeshRaycast::raycast(&scene, Vector3(3, 0, 5), Vector3(0, 0, -1), 10.0f).entity, b.getEntity());
    // one build for both
    EXPECT_EQ(PickCache::get().stats().builds + PickCache::get().stats().diskLoads, buildsBefore + 1);
}

TEST(PickCache, InvalidatedOnReload){
    Scene scene;
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 48, 48);
    settleScene(scene);

    MeshRaycastHit hit = MeshRaycast::raycast(&scene, Vector3(0, 0, 5), Vector3(0, 0, -1), 10.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 4.0f, 0.01f);
    const PickCache::EntryPtr before = scene.getPickBvh().meshEntry(sphere.getEntity(), false);
    ASSERT_TRUE(before);
    const uint64_t oldHash = before->hash;

    // new geometry (what a reload does): a new tree, the old one unused
    sphere.createSphere(2.0f, 48, 48);
    settleScene(scene);
    EXPECT_EQ(scene.getComponent<MeshComponent>(sphere.getEntity()).pickGeometryHash, 0u);
    hit = MeshRaycast::raycast(&scene, Vector3(0, 0, 5), Vector3(0, 0, -1), 10.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 3.0f, 0.02f);
    const PickCache::EntryPtr after = scene.getPickBvh().meshEntry(sphere.getEntity(), false);
    ASSERT_TRUE(after);
    EXPECT_NE(after->hash, oldHash);

    // the old tree stays for 30 s after its last user, then goes
    PickCache::get().collect(std::chrono::steady_clock::now() + std::chrono::seconds(10));
    PickCache::EntryPtr kept = PickCache::get().acquire(oldHash);
    EXPECT_TRUE(kept);
    PickCache::get().release(kept);
    kept.reset();
    PickCache::get().collect(std::chrono::steady_clock::now() + std::chrono::seconds(31));
    EXPECT_FALSE(PickCache::get().acquire(oldHash));
    // the one in use stays
    PickCache::EntryPtr used = PickCache::get().acquire(after->hash);
    EXPECT_TRUE(used);
    PickCache::get().release(used);
}

TEST(PickCache, PendingReturnsBoundsAndWaitBuildsInline){
    Scene scene;
    Primitive sphere(&scene);
    // a tessellation no other test uses: a geometry (a cache entry) of its own
    sphere.createSphere(1.0f, 96, 96);
    sphere.setScale(1.37f);
    settleScene(scene);

    BackgroundBlocker blocker;
    MeshRaycast::prepare(&scene);
    const PickCache::EntryPtr entry = scene.getPickBvh().meshEntry(sphere.getEntity(), false);
    ASSERT_TRUE(entry);
    ASSERT_EQ(PickCache::ready(entry), nullptr) << "the build should be queued behind the blocked Background lane";

    // not waiting: its bounds, flagged pending
    MeshRaycastOptions noWait;
    noWait.waitForBvh = false;
    MeshRaycastHit hit = MeshRaycast::raycast(&scene, Vector3(1.2f, 1.2f, 5), Vector3(0, 0, -1), 10.0f, LAYER_MASK_EVERYTHING, noWait);
    ASSERT_TRUE(hit.hit);
    EXPECT_TRUE(hit.pending);
    EXPECT_EQ(hit.kind, PickKind::BOUNDS);

    // waiting: built here (the Background lane is still blocked), triangle-exact: that
    // corner of the bounds is outside the sphere
    const uint64_t waits = PickCache::get().stats().inlineWaits;
    hit = MeshRaycast::raycast(&scene, Vector3(1.2f, 1.2f, 5), Vector3(0, 0, -1), 10.0f);
    EXPECT_FALSE(hit.hit);
    EXPECT_EQ(PickCache::get().stats().inlineWaits, waits + 1);
    ASSERT_NE(PickCache::ready(entry), nullptr);
    hit = MeshRaycast::raycast(&scene, Vector3(0, 0, 5), Vector3(0, 0, -1), 10.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_FALSE(hit.pending);
    EXPECT_EQ(hit.kind, PickKind::MESH);
}
