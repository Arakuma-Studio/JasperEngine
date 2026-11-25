// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Jolt's contact cache (engine/libs/joltphysics/PATCHES.md, 1): a body at rest reuses its body
// pair's cached manifolds, and OnContactPersisted then reports a depth Jolt estimates from the
// cached points. That estimate has to be the deepest point's: with only the first point's, a
// manifold whose first point is speculative read as not touching for as long as the body rested
// (PlacementWorld's ring on Windows never slept, Contact3D's depth was off).

#ifdef JASPERENGINE_PHYSICS_3D

#include "Jolt/Jolt.h"
#include "Jolt/Core/Factory.h"
#include "Jolt/Core/JobSystemSingleThreaded.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Collision/ContactListener.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/RegisterTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace{

    constexpr JPH::ObjectLayer kContactCacheStatic = 0;
    constexpr JPH::ObjectLayer kContactCacheMoving = 1;

    class ContactCacheBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
    public:
        JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return JPH::BroadPhaseLayer((JPH::BroadPhaseLayer::Type)layer); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return (JPH::BroadPhaseLayer::Type)layer == 0 ? "static" : "moving"; }
#endif
    };

    class ContactCacheObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
            return layer == kContactCacheMoving || (JPH::BroadPhaseLayer::Type)broadPhase == kContactCacheMoving;
        }
    };

    class ContactCacheObjectPairs final : public JPH::ObjectLayerPairFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == kContactCacheMoving || b == kContactCacheMoving; }
    };

    // Each persisted manifold: the depth Jolt reports against the deepest of its own points
    class ContactCacheDepths final : public JPH::ContactListener {
    public:
        bool recording = false;
        int persisted = 0;
        float worstShortfall = 0.0f;    // how far the reported depth fell below the deepest point
        float shallowest = FLT_MAX;     // the least depth reported

        void OnContactPersisted(const JPH::Body&, const JPH::Body&, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
            if (!recording) return;
            float deepest = -FLT_MAX;
            for (JPH::uint i = 0; i < manifold.mRelativeContactPointsOn1.size(); i++) {
                deepest = std::max(deepest, (manifold.mRelativeContactPointsOn1[i] - manifold.mRelativeContactPointsOn2[i]).Dot(manifold.mWorldSpaceNormal));
            }
            persisted++;
            worstShortfall = std::max(worstShortfall, deepest - manifold.mPenetrationDepth);
            shallowest = std::min(shallowest, manifold.mPenetrationDepth);
        }
    };

    void contactCacheEnsureJolt(){
        if (JPH::Factory::sInstance)
            return;
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

}

TEST(JoltContactCache, PersistedDepthIsTheDeepestCachedPoint){
    contactCacheEnsureJolt();
    // A 1 m slab that cannot turn, tilted so one edge rests on the floor and the opposite one is
    // 10 mm up (within the 20 mm speculative distance): one manifold with touching and
    // speculative points. Tilted four ways, so whatever order Jolt clips the points in, the
    // first point is a speculative one in some of them.
    const JPH::Vec3 axes[4] = {JPH::Vec3::sAxisX(), -JPH::Vec3::sAxisX(), JPH::Vec3::sAxisZ(), -JPH::Vec3::sAxisZ()};
    for (const JPH::Vec3& axis : axes) {
        SCOPED_TRACE(testing::Message() << "tilted about (" << axis.GetX() << ", " << axis.GetZ() << ")");
        ContactCacheBroadPhaseLayers broadPhaseLayers;
        ContactCacheObjectVsBroadPhase objectVsBroadPhase;
        ContactCacheObjectPairs objectPairs;
        ContactCacheDepths depths;
        JPH::PhysicsSystem physics;
        physics.Init(16, 0, 64, 64, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        physics.SetContactListener(&depths);
        JPH::BodyInterface& bodies = physics.GetBodyInterface();

        JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(5.0f, 0.5f, 5.0f)), JPH::RVec3(0, -0.5f, 0),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Static, kContactCacheStatic);
        const JPH::BodyID floorId = bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate);

        const float tilt = std::asin(0.01f);
        JPH::BodyCreationSettings slab(new JPH::BoxShape(JPH::Vec3(0.5f, 0.05f, 0.5f)), JPH::RVec3(0, 0.1f, 0),
                                       JPH::Quat::sRotation(axis, tilt), JPH::EMotionType::Dynamic, kContactCacheMoving);
        slab.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
        // awake at rest: every step goes through the body pair cache
        slab.mAllowSleeping = false;
        const JPH::BodyID slabId = bodies.CreateAndAddBody(slab, JPH::EActivation::Activate);

        JPH::JobSystemSingleThreaded jobs(JPH::cMaxPhysicsJobs);
        JPH::TempAllocatorImpl temp(1024 * 1024);
        for (int step = 0; step < 120; step++)
            physics.Update(1.0f / 60.0f, 1, &temp, &jobs);
        depths.recording = true;
        for (int step = 0; step < 60; step++)
            physics.Update(1.0f / 60.0f, 1, &temp, &jobs);

        std::printf("[ contact cache ] %d persisted, reported depth at most %.4f m under the deepest point, least %.4f m\n",
                    depths.persisted, depths.worstShortfall, depths.shallowest);
        EXPECT_GE(depths.persisted, 60);
        EXPECT_LT(depths.worstShortfall, 1e-4f);
        // resting on its low edge: touching, never reported 10 mm away
        EXPECT_GT(depths.shallowest, -0.002f);

        physics.SetContactListener(nullptr);
        bodies.RemoveBody(slabId);
        bodies.DestroyBody(slabId);
        bodies.RemoveBody(floorId);
        bodies.DestroyBody(floorId);
    }
}

#endif
