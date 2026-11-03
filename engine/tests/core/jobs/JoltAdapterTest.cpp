// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Jolt on the engine's job system (S06 AC19): the same scene stepped with JoltJobAdapter and
// with Jolt's own JobSystemThreadPool ends with the same body positions.

#ifdef JASPERENGINE_PHYSICS_3D

#include "jobs/JobSystem.h"
#include "util/JoltJobAdapter.h"

#include "JobsTestUtil.h"

#include "Jolt/Jolt.h"
#include "Jolt/Core/Factory.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/RegisterTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace{

    // two layers: the floor and the moving boxes
    constexpr JPH::ObjectLayer kStatic = 0;
    constexpr JPH::ObjectLayer kMoving = 1;

    class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
    public:
        JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return JPH::BroadPhaseLayer((JPH::BroadPhaseLayer::Type)layer); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return (JPH::BroadPhaseLayer::Type)layer == 0 ? "static" : "moving"; }
#endif
    };

    class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
            return layer == kMoving || (JPH::BroadPhaseLayer::Type)broadPhase == kMoving;
        }
    };

    class ObjectPairs final : public JPH::ObjectLayerPairFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == kMoving || b == kMoving; }
    };

    void ensureJolt(){
        if (JPH::Factory::sInstance)
            return;
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    // 500 boxes dropped in a 10 x 10 x 5 stack onto a floor, 300 steps of 1/60 s; returns
    // every box's final position, and in `moved` the farthest any box went
    std::vector<JPH::RVec3> simulate(JPH::JobSystem& jobs, double& moved){
        BroadPhaseLayers broadPhaseLayers;
        ObjectVsBroadPhase objectVsBroadPhase;
        ObjectPairs objectPairs;
        JPH::PhysicsSystem physics;
        physics.Init(1024, 0, 4096, 4096, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        JPH::BodyInterface& bodies = physics.GetBodyInterface();

        JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(50.0f, 1.0f, 50.0f)), JPH::RVec3(0, -1, 0),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStatic);
        bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate);

        std::vector<JPH::BodyID> boxes;
        JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
        for (int k = 0; k < 500; k++){
            const int x = k % 10, z = (k / 10) % 10, y = k / 100;
            // slightly offset layers so the stack topples and the boxes keep touching
            JPH::BodyCreationSettings settings(box, JPH::RVec3(x * 1.05f + y * 0.3f, 0.6f + y * 1.2f, z * 1.05f - y * 0.2f),
                                               JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.1f * (float)y),
                                               JPH::EMotionType::Dynamic, kMoving);
            boxes.push_back(bodies.CreateAndAddBody(settings, JPH::EActivation::Activate));
        }
        physics.OptimizeBroadPhase();

        std::vector<JPH::RVec3> start;
        for (const JPH::BodyID& id : boxes)
            start.push_back(bodies.GetPosition(id));
        JPH::TempAllocatorImpl temp(16 * 1024 * 1024);
        for (int step = 0; step < 300; step++)
            physics.Update(1.0f / 60.0f, 1, &temp, &jobs);

        std::vector<JPH::RVec3> positions;
        moved = 0.0;
        for (size_t i = 0; i < boxes.size(); i++){
            positions.push_back(bodies.GetPosition(boxes[i]));
            moved = std::max(moved, (double)(positions.back() - start[i]).Length());
        }
        for (const JPH::BodyID& id : boxes)
            bodies.RemoveBody(id);
        bodies.DestroyBodies(boxes.data(), (int)boxes.size());
        return positions;
    }

    double largestDifference(const std::vector<JPH::RVec3>& a, const std::vector<JPH::RVec3>& b){
        double largest = 0.0;
        for (size_t i = 0; i < a.size(); i++){
            const JPH::RVec3 delta = a[i] - b[i];
            largest = std::max({largest, std::abs((double)delta.GetX()), std::abs((double)delta.GetY()), std::abs((double)delta.GetZ())});
        }
        return largest;
    }
}

TEST(JobsJolt, AdapterMatchesJoltsOwnPool){
    ensureJolt();
    jasperengine::JoltJobAdapter adapter(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
    double moved = 0.0, theirMoved = 0.0, againMoved = 0.0;
    const std::vector<JPH::RVec3> ours = simulate(adapter, moved);
    std::vector<JPH::RVec3> theirs;
    {
        JPH::JobSystemThreadPool pool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 7);
        theirs = simulate(pool, theirMoved);
    }
    ASSERT_EQ(ours.size(), 500u);
    ASSERT_EQ(theirs.size(), ours.size());

    // the stack toppled: boxes travelled, the comparison is not of a scene at rest
    EXPECT_GT(moved, 1.0);

    const double difference = largestDifference(ours, theirs);
    RecordProperty("largest_difference_m", std::to_string(difference));
    std::printf("[ JoltAdapter ] largest position difference after 300 steps: %.3g m\n", difference);
    EXPECT_LE(difference, 1e-4);

    // and our pool repeats itself
    EXPECT_LE(largestDifference(ours, simulate(adapter, againMoved)), 1e-4);
}

// The adapter queues each Jolt job in an envelope that releases the job's reference after
// running it, and the last release frees the job through the adapter (JobSystem::FreeJob). A
// barrier waiter may run the job first, so the envelope can still be queued when the step is
// over and the adapter goes: the adapter waits for its envelopes (helping them) when it is
// destroyed, and none of them touches it afterwards.
TEST(JobsJolt, DestroyingTheAdapterWaitsForItsQueuedJobs){
    using namespace jasperengine;
    ensureJolt();
    jobs::JobSystem& system = jobs::system();
    const int workers = system.getWorkerCount();
    ASSERT_GT(workers, 0);

    // every worker busy: the envelope stays queued while the barrier's waiter runs the job
    std::atomic<int> started{0};
    jobs::Event release;
    std::vector<jobs::Task<void>> blockers;
    for (int i = 0; i < workers; i++){
        blockers.push_back(system.submit([&]{
            started++;
            release.wait();
        }, {jobs::Lane::High, jobs::GateClass::Default, "blocker"}));
    }
    ASSERT_TRUE(testsupport::spinUntil([&]{ return started.load() == workers; }, 10000.0));

    std::atomic<int> ran{0};
    auto adapter = std::make_unique<JoltJobAdapter>(64, 8);
    JPH::JobSystem::Barrier* barrier = adapter->CreateBarrier();
    {
        JPH::JobHandle handle = adapter->CreateJob("test", JPH::Color::sRed, [&]{ ran++; });
        barrier->AddJob(handle);
    }
    adapter->WaitForJobs(barrier);
    adapter->DestroyBarrier(barrier);
    EXPECT_EQ(ran.load(), 1);
    adapter.reset();

    release.set();
    for (auto& blocker : blockers)
        blocker.wait();
    // the pool takes its High lane in order: once a job queued now has run, so has the
    // envelope (were it still there), and a worker freeing a job of the destroyed adapter
    // would have crashed by the time the next one finished too
    for (int i = 0; i < 2 * workers; i++){
        jobs::Task<void> after = system.submit([]{}, {jobs::Lane::High, jobs::GateClass::Default, "after"});
        ASSERT_TRUE(testsupport::spinUntil([&]{ return after.isReady(); }, 10000.0));
    }
    EXPECT_EQ(ran.load(), 1);
}

#endif // JASPERENGINE_PHYSICS_3D
