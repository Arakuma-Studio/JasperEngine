// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R20-R24 (AC19-AC23): colliders near a focus, swapped with no gap, and bodies that cannot escape

#include "VoxelTestSupport.h"

#include "Engine.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "math/Ray.h"
#include "object/GameObject.h"
#include "object/physics/Rigidbody.h"
#include "physics/JoltShapeBuilder.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/VoxelSystem.h"
#include "../ecs/EcsTestUtil.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyInterface.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace voxeltest;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    Vec3d toLocal(const Vector3& v) { return Vec3d{v.x, v.y, v.z}; }

    class VoxelPhysics : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        std::shared_ptr<VoxelSystem> system;
        PhysicsSystem* physics = nullptr;
        std::shared_ptr<VoxelAsset> asset;
        Entity focus = NULL_ENTITY;
        float updateTime = 0.0f;

        void SetUp() override {
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            Engine::setFixedDeltatime(DT);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            scene->getScheduler().setEnabled("VoxelSystem", true);
            system = scene->getSystem<VoxelSystem>();
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        void TearDown() override {
            system.reset();
            scene.reset();
            asset.reset();
            Engine::setFixedDeltatime(0.0);
            Engine::setUpdateTime(updateTime);
        }

        // Flat ground at y = 32 in a volume whose origin is the world's
        Entity ground(const std::string& name, VoxelQuality quality = VoxelQuality::High, int dimX = 128, int dimZ = 128) {
            GeneratorDesc generator;
            generator.kind = "solid_ground";
            generator.height = 32.0;
            asset = VoxelAssets::create(name, desc(dimX, 64, dimZ, 2), generator, 1);
            focus = scene->createEntity();
            scene->addComponent<Transform>(focus, {});
            setWorldPosition(focus, Vector3(static_cast<float>(dimX) * 0.5f, 34.0f, static_cast<float>(dimZ) * 0.5f));
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.quality = quality;
            component.focus = focus;
            scene->addComponent<VoxelVolumeComponent>(entity, component);
            return entity;
        }

        void setWorldPosition(Entity entity, const Vector3& position) {
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.worldPosition = position;
            transform.modelMatrix = Matrix4::translateMatrix(position);
            transform.needUpdate = true;
        }

        Entity ball(const Vector3& position, float radius = 0.5f) {
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            setWorldPosition(entity, position);
            physics->createRigidbody(entity);
            Rigidbody body(scene.get(), entity);
            body.setType(BodyType::DYNAMIC);
            body.createSphereCollider(radius);
            return entity;
        }

        void step(int count = 1) {
            for (int i = 0; i < count; i++) {
                scene->fixedUpdate(DT);
                scene->update(DT);
            }
        }

        RayReturn castRay(const Vector3& from, const Vector3& to) {
            return Ray(from, to - from).intersects(scene.get(), RayFilter::BODY_3D);
        }

        float heightOf(Entity entity) {
            return scene->getComponent<Transform>(entity).worldPosition.y;
        }

        JPH::BodyInterface& bodies() { return physics->getBodyInterface3D(); }

        bool isAwake(Entity entity) {
            return bodies().IsActive(scene->getComponent<RigidbodyComponent>(entity).body);
        }

        // Runs until the ball rests, or the frames run out
        void settle(Entity entity, int frames = 300) {
            for (int i = 0; i < frames && isAwake(entity); i++) step();
        }
    };

}

TEST_F(VoxelPhysics, ColliderBeforeFirstStep) {
    ground("mem://first-step");
    const Entity body = ball(Vector3(64.0f, 32.6f, 64.0f));
    // A body on the surface at frame 1 does not fall through
    step(120);
    EXPECT_GT(heightOf(body), 32.0f);
    EXPECT_LT(heightOf(body), 33.0f);
}

TEST_F(VoxelPhysics, ShapeBuiltOffMain) {
    const Entity owner = ground("mem://off-main");
    step(10);
    system->finishPendingWork();
    const VoxelStats stats = system->getStats(owner);
    EXPECT_GT(stats.collisionBodies, 0);
    // The queue's shapes run on the job system; blocking starts are R20's and do not count
    if (jobs::system().getWorkerCount() > 0) {
        EXPECT_EQ(stats.shapesBuiltOnMain, 0);
    }
}

TEST_F(VoxelPhysics, BatchAddMatchesSingleAdd) {
    const Entity owner = ground("mem://batch");
    step(30);
    system->finishPendingWork();
    step(5);
    // What batches added answers a ray as the surface says
    for (int i = 0; i < 16; i++) {
        const float x = 50.0f + static_cast<float>(i) * 2.0f;
        const Vector3 origin(x, 40.0f, 64.0f);
        const Vector3 to(x, 20.0f, 64.0f);
        const RayReturn result = castRay(origin, to);
        ASSERT_TRUE(result.hit) << x;
        EXPECT_NEAR(result.point.y, 32.0f, 0.01f);
        EXPECT_EQ(system->getGeneratedOwner(result.body), owner);
    }
}

TEST_F(VoxelPhysics, BudgetPerTick) {
    const Entity owner = ground("mem://budget", VoxelQuality::Low);
    step(1);
    int previous = system->getStats(owner).collisionBodies;
    const int batch = voxelTierSettings(VoxelTier::Low).bodyBatch;
    // The blocking start aside, a tick adds at most one batch
    system->finishPendingWork();
    for (int i = 0; i < 20; i++) {
        setWorldPosition(focus, Vector3(20.0f + static_cast<float>(i) * 4.0f, 34.0f, 20.0f));
        step();
        const int now = system->getStats(owner).collisionBodies;
        EXPECT_LE(now - previous, batch);
        previous = now;
    }
}

TEST_F(VoxelPhysics, EmptiedBlockRemovesBody) {
    const Entity owner = ground("mem://emptied");
    step(10);
    system->finishPendingWork();
    step(5);
    const int before = system->getStats(owner).collisionBodies;
    ASSERT_GT(before, 0);
    // Takes out everything solid around the focus, floor to surface
    Shape box;
    box.kind = ShapeKind::Box;
    box.center = Vec3d{64.0, 16.0, 64.0};
    box.halfExtents = Vec3d{200.0, 32.0, 200.0};
    const EditOp op = digOp(box);
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{}).ok);
    system->finishPendingWork();
    step(5);
    system->finishPendingWork();
    step(5);
    EXPECT_LT(system->getStats(owner).collisionBodies, before);
}

TEST_F(VoxelPhysics, TwoEditsOverOneBlockKeepItsBody) {
    // R22: a second edit over a block whose swap has not landed yet. Both swaps apply its newest shape
    // once; the second must not take the body away as if the block had gone empty
    const Entity owner = ground("mem://two-edits");
    step(5);
    system->finishPendingWork();
    for (int i = 0; i < 10; i++) step();
    const RayReturn before = castRay(Vector3(58.0f, 50.0f, 58.0f), Vector3(58.0f, 10.0f, 58.0f));
    ASSERT_TRUE(before.hit);
    const EditOp first = digOp(sphereShape(62.0, 32.0, 62.0, 2.0));
    const EditOp second = digOp(sphereShape(61.0, 32.0, 61.0, 2.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&first, 1), EditOptions{}).ok);
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&second, 1), EditOptions{}).ok);
    for (int i = 0; i < 120; i++) {
        step();
        if (i % 10 == 0) system->finishPendingWork();
    }
    // the same block, outside both digs: the ground is still there
    const RayReturn after = castRay(Vector3(58.0f, 50.0f, 58.0f), Vector3(58.0f, 10.0f, 58.0f));
    ASSERT_TRUE(after.hit);
    EXPECT_NEAR(after.point.y, 32.0f, 0.05f);
}

TEST_F(VoxelPhysics, BubbleAroundDynamicBody) {
    ground("mem://bubble", VoxelQuality::High, 256, 256);
    setWorldPosition(focus, Vector3(16.0f, 34.0f, 16.0f));
    step(5);
    system->finishPendingWork();
    // Far from the focus, where the ground draws at a coarse level
    const Entity body = ball(Vector3(230.0f, 40.0f, 230.0f));
    for (int i = 0; i < 240; i++) {
        step();
        if (i % 4 == 0) system->finishPendingWork();
    }
    EXPECT_GT(heightOf(body), 32.0f);
    EXPECT_LT(heightOf(body), 33.0f);
}

TEST_F(VoxelPhysics, EnsureCollisionBlocks) {
    const Entity owner = ground("mem://ensure", VoxelQuality::High, 256, 256);
    setWorldPosition(focus, Vector3(16.0f, 34.0f, 16.0f));
    step(1);
    const AABB far(200.0f, 30.0f, 200.0f, 210.0f, 36.0f, 210.0f);
    EXPECT_FALSE(system->isCollisionReady(owner, far));
    system->ensureCollision(owner, far);
    EXPECT_TRUE(system->isCollisionReady(owner, far));
    const RayReturn result = castRay(Vector3(205.0f, 40.0f, 205.0f), Vector3(205.0f, 20.0f, 205.0f));
    EXPECT_TRUE(result.hit);
}

TEST_F(VoxelPhysics, RebuildContract) {
    const Entity owner = ground("mem://contract");
    int collisionUpdates = 0;
    scene->getComponent<VoxelVolumeComponent>(owner).onCollisionUpdated.add("test", [&collisionUpdates](AABB) { collisionUpdates++; });
    const Entity sitting = ball(Vector3(64.0f, 32.6f, 64.0f));
    const Entity faraway = ball(Vector3(64.0f + 48.0f, 32.6f, 64.0f));
    step(10);
    system->finishPendingWork();
    settle(sitting);
    settle(faraway);
    ASSERT_FALSE(isAwake(sitting));
    ASSERT_FALSE(isAwake(faraway));
    const float rest = heightOf(sitting);

    const EditOp op = digOp(sphereShape(64.0, 32.0, 64.0, 6.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{}).ok);
    // The old collider stays live until the new one is ready: no fall before the swap
    EXPECT_EQ(collisionUpdates, 0);
    for (int i = 0; i < 4 && collisionUpdates == 0; i++) {
        EXPECT_FALSE(isAwake(sitting));
        EXPECT_NEAR(heightOf(sitting), rest, 0.001f);
        scene->update(DT);
        if (i == 1) system->finishPendingWork();
        scene->fixedUpdate(DT);
        scene->update(DT);
    }
    for (int i = 0; i < 20 && collisionUpdates == 0; i++) {
        system->finishPendingWork();
        step();
    }
    EXPECT_GE(collisionUpdates, 1);
    // The swap wakes what it touched and nothing else; the body falls into the hole
    EXPECT_TRUE(isAwake(sitting));
    EXPECT_FALSE(isAwake(faraway));
    step(60);
    EXPECT_LT(heightOf(sitting), rest - 1.0f);
    EXPECT_NEAR(heightOf(faraway), rest, 0.01f);
}

TEST_F(VoxelPhysics, RaycastSeesNewDataAtOncePhysicsAfterTheSwap) {
    const Entity owner = ground("mem://rays");
    step(10);
    system->finishPendingWork();
    step(5);
    const EditOp op = digOp(sphereShape(64.0, 32.0, 64.0, 6.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{}).ok);
    const RaycastHit fresh = raycast(*system->getStore(owner), Vec3d{64.0, 50.0, 64.0}, Vec3d{0.0, -1.0, 0.0}, 40.0);
    ASSERT_TRUE(fresh.hit);
    EXPECT_NEAR(fresh.point.y, 26.0, 0.1);
    const RayReturn before = castRay(Vector3(64.0f, 50.0f, 64.0f), Vector3(64.0f, 10.0f, 64.0f));
    ASSERT_TRUE(before.hit);
    EXPECT_NEAR(before.point.y, 32.0f, 0.01f);
}

TEST_F(VoxelPhysics, BallRollsAcrossBlockSeam) {
    // Risks: separate bodies share no active-edge data, so the roll is held against the same ball on
    // one box with no seam: speed loss at most 5 %, height deviation 0.02 voxel.
    const auto roll = [this](Entity body, float& endSpeed, float& lowest, float& highest) {
        const JPH::BodyID id = scene->getComponent<RigidbodyComponent>(body).body;
        bodies().SetLinearVelocity(id, JPH::Vec3(8.0f, 0.0f, 0.0f));
        step(30);
        lowest = 1e9f;
        highest = -1e9f;
        for (int i = 0; i < 180; i++) {
            if (focus != NULL_ENTITY) setWorldPosition(focus, scene->getComponent<Transform>(body).worldPosition);
            step();
            if (i % 8 == 0) system->finishPendingWork();
            lowest = std::min(lowest, heightOf(body));
            highest = std::max(highest, heightOf(body));
        }
        endSpeed = bodies().GetLinearVelocity(id).GetX();
    };

    // The control: the same triangles as one mesh body, so the only difference is the seams
    float controlSpeed = 0.0f, controlLow = 0.0f, controlHigh = 0.0f;
    {
        VolumeDesc flat = desc(256, 64, 256);
        Store source(flat);
        source.setBase(makeSolidGroundBase(32.0, 1.0, 0));
        std::vector<Vector3> vertices;
        std::vector<uint32_t> indices;
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                for (int y = 1; y <= 2; y++) {
                    const auto mesh = meshOf(source, BlockKey{0, x, y, z}, 0, MeshMode::CollisionOnly);
                    const uint32_t base = static_cast<uint32_t>(vertices.size());
                    for (const MeshVertex& v : mesh->vertices) vertices.emplace_back(fixedToLocal(v.position[0], 1.0f), fixedToLocal(v.position[1], 1.0f), fixedToLocal(v.position[2] + 256 * 512, 1.0f));
                    for (uint16_t index : mesh->indices) indices.push_back(base + index);
                }
            }
        }
        std::string error;
        const JPH::ShapeRefC shape = JoltShapeBuilder::buildMeshShape(vertices, indices, &error, nullptr);
        ASSERT_TRUE(shape != nullptr) << error;
        const Entity floor = scene->createEntity();
        scene->addComponent<Transform>(floor, {});
        physics->createRigidbody(floor);
        JPH::BodyID id = physics->createStaticBodyForShape(floor, shape.GetPtr());
        ASSERT_FALSE(id.IsInvalid());
        physics->finalizeBodies(std::span<JPH::BodyID>(&id, 1), physics->prepareBodies(std::span<JPH::BodyID>(&id, 1)));
        const Entity ballOnFloor = ball(Vector3(40.0f, 32.5f, 584.0f));
        step(10);
        roll(ballOnFloor, controlSpeed, controlLow, controlHigh);
    }
    ground("mem://seam", VoxelQuality::High, 256, 256);
    setWorldPosition(focus, Vector3(64.0f, 34.0f, 72.0f));
    const Entity body = ball(Vector3(40.0f, 32.5f, 72.0f));
    step(10);
    system->finishPendingWork();
    float speed = 0.0f, low = 0.0f, high = 0.0f;
    roll(body, speed, low, high);
    std::printf("[bench] ball over block seams: end speed %.3f (no seam %.3f), height range %.4f (no seam %.4f)\n", speed, controlSpeed, high - low, controlHigh - controlLow);
    EXPECT_GE(speed, controlSpeed * 0.95f);
    EXPECT_LE(high - low, (controlHigh - controlLow) + 0.02f);
}

namespace {

    // How many bodies end below the ground or deep in solid
    struct TunnelResult {
        int lost = 0;
        int rescues = 0;
        int lostBelow = 0;     // under the volume's ground: fell through
        int lostInside = 0;    // deep in solid
        int lostSlow = 0;      // of those, thrown under 20 m/s
        int left = 0;          // bounced off the volume's footprint: the ground ends there, no evidence
    };

}

TEST_F(VoxelPhysics, RescueLiftsWhatAFillSwallows) {
    const Entity owner = ground("mem://swallow");
    const Entity body = ball(Vector3(64.0f, 33.0f, 64.0f));
    step(10);
    system->finishPendingWork();
    settle(body);
    int rescued = 0;
    scene->getComponent<VoxelVolumeComponent>(owner).onBodyRescued.add("test", [&rescued](GameObject) { rescued++; });
    // Solid well above the body
    const EditOp op = addOp(sphereShape(64.0, 34.0, 64.0, 8.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{}).ok);
    for (int i = 0; i < 30; i++) {
        system->finishPendingWork();
        step();
    }
    EXPECT_GE(rescued, 1);
    // First air straight up, plus half its height: on top of the mound
    EXPECT_GT(valueAt(*system->getStore(owner), toLocal(scene->getComponent<Transform>(body).worldPosition)), 0.0);
}

TEST_F(VoxelPhysics, RescueOffLeavesItInside) {
    const Entity owner = ground("mem://norescue");
    scene->getComponent<VoxelVolumeComponent>(owner).rescueBodies = false;
    const Entity body = ball(Vector3(64.0f, 33.0f, 64.0f));
    step(10);
    system->finishPendingWork();
    settle(body);
    const EditOp op = addOp(sphereShape(64.0, 34.0, 64.0, 8.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{}).ok);
    for (int i = 0; i < 30; i++) {
        system->finishPendingWork();
        step();
    }
    EXPECT_LT(valueAt(*system->getStore(owner), toLocal(scene->getComponent<Transform>(body).worldPosition)), 0.0);
}

// The Slow pair: 1,000 bodies from 5 to 200 m/s at a wall and at the ground (AC23). Red control: the
// same throw with rescueBodies off shows bodies escaping.
class VoxelPhysicsSlow : public VoxelPhysics {
protected:
    TunnelResult throwBodies(const std::string& name, bool rescue) {
        const Entity owner = ground(name, VoxelQuality::High, 128, 128);
        VoxelVolumeComponent& component = scene->getComponent<VoxelVolumeComponent>(owner);
        component.rescueBodies = rescue;
        // the system knows the volume after its first update
        step(1);
        // A wall one voxel thick across the middle
        Shape wall;
        wall.kind = ShapeKind::Box;
        wall.center = Vec3d{64.0, 40.0, 64.0};
        wall.halfExtents = Vec3d{0.5, 8.0, 40.0};
        const EditOp op = addOp(wall);
        if (!system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{}).ok) {
            ADD_FAILURE() << "the wall";
            return {};
        }
        step(10);
        system->finishPendingWork();
        std::vector<Entity> thrown;
        std::mt19937 random(31);
        for (int i = 0; i < 1000; i++) {
            const bool down = i % 2 == 0;
            const Vector3 position(down ? 40.0f + static_cast<float>(random() % 48) : 40.0f, down ? 44.0f + static_cast<float>(random() % 16) : 36.0f + static_cast<float>(random() % 8), 30.0f + static_cast<float>(random() % 68));
            const Entity body = ball(position, 0.3f);
            thrown.push_back(body);
        }
        step(1);
        std::mt19937 speeds(33);
        for (Entity body : thrown) {
            const float speed = 5.0f + static_cast<float>(speeds() % 195);
            const bool down = (body & 1) == 0;
            bodies().SetLinearVelocity(scene->getComponent<RigidbodyComponent>(body).body, down ? JPH::Vec3(0.0f, -speed, 0.0f) : JPH::Vec3(speed, 0.0f, 0.0f));
        }
        for (int i = 0; i < 300; i++) {
            step();
            if (i % 10 == 0) system->finishPendingWork();
        }
        TunnelResult result;
        result.rescues = system->getStats(owner).rescues;
        Store& store = *system->getStore(owner);
        std::mt19937 replay(33);
        for (Entity body : thrown) {
            const float speed = 5.0f + static_cast<float>(replay() % 195);
            const Vector3 p = scene->getComponent<Transform>(body).worldPosition;
            // a body that bounced off the 128 m ground fell off its edge, not through it
            if (p.x < 0.0f || p.z < 0.0f || p.x > 128.0f || p.z > 128.0f) {
                result.left++;
                continue;
            }
            const double value = valueAt(store, toLocal(p));
            if (p.y < 28.0f || value < -1.5) {
                result.lost++;
                if (p.y < 28.0f) result.lostBelow++; else result.lostInside++;
                if (speed < 20.0f) result.lostSlow++;
            }
        }
        std::printf("[bench] tunnelling (rescue %s): %d lost (%d below, %d inside, %d under 20 m/s), %d left the footprint, %d rescues\n",
                    rescue ? "on" : "off", result.lost, result.lostBelow, result.lostInside, result.lostSlow, result.left, result.rescues);
        return result;
    }
};

TEST_F(VoxelPhysicsSlow, Tunnelling) {
    const TunnelResult result = throwBodies("mem://tunnel", true);
    EXPECT_EQ(result.lost, 0);
}

TEST_F(VoxelPhysicsSlow, TunnellingRedControlNoRescue) {
    const TunnelResult result = throwBodies("mem://tunnelred", false);
    EXPECT_GT(result.lost, 0);
}
