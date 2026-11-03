// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SplineSystem (docs/specs/S18-splines.md R13, R21, R22, R30, D3): what a spline builds is engine
// owned and regenerated from the curve, only the chunks a change touches rebuild, a removed or
// disabled consumer frees what it made, generated GameObjects map to their owner, Spline Animate
// follows at constant world speed and completes once, rivers answer the water queries, and the
// road fixture's vertices hash to the same value on every desktop compiler (R16, AC15).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SplineAnimateComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/Transform.h"
#include "object/spline/Spline.h"
#include "spline/SplineEvaluator.h"
#include "subsystem/SplineSystem.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "component/Rigidbody2DComponent.h"
#include "subsystem/PhysicsSystem.h"
#include "../ecs/EcsTestUtil.h"
#endif

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    // the road fixture's vertex hash, pinned on GCC (RoadFixtureVerticesAreDeterministic)
    constexpr uint64_t ROAD_FIXTURE_VERTEX_HASH = 0xb33a1dea13fbc139ull;

    std::vector<spline::SplineKnot> straightKnots(int count, float step, float radius){
        std::vector<spline::SplineKnot> knots;
        for (int i = 0; i < count; i++){
            spline::SplineKnot knot;
            knot.position = Vector3(step * static_cast<float>(i), 0.0f, 3.0f * std::sin(static_cast<float>(i) * 0.7f));
            knot.radius = radius;
            knots.push_back(knot);
        }
        return knots;
    }

    class SplineSystemTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        std::shared_ptr<SplineSystem> system;

        void SetUp() override{
            scene = std::make_unique<Scene>();
            system = scene->getSystem<SplineSystem>();
        }
        void TearDown() override{
            system.reset();
            scene.reset();
        }

        Entity road(int knots, float step, SplineExtrudeShape shape = SplineExtrudeShape::Crown){
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            SplineComponent spline;
            spline.spline.setKnots(straightKnots(knots, step, 4.0f));
            scene->addComponent<SplineComponent>(entity, spline);
            MeshComponent mesh;
            mesh.numSubmeshes = 1;
            scene->addComponent<MeshComponent>(entity, mesh);
            SplineExtrudeComponent extrude;
            extrude.shape = shape;
            scene->addComponent<SplineExtrudeComponent>(entity, extrude);
            return entity;
        }

        uint64_t vertexHash(Entity owner){
            uint64_t hash = 1469598103934665603ull;
            for (Entity chunk : system->getGeneratedEntities(owner)){
                const MeshComponent& mesh = scene->getComponent<MeshComponent>(chunk);
                const Transform& transform = scene->getComponent<Transform>(chunk);
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(mesh.buffer.getData());
                for (size_t i = 0; i < mesh.buffer.getSize(); i++){
                    hash = (hash ^ bytes[i]) * 1099511628211ull;
                }
                const float origin[3] = {transform.position.x, transform.position.y, transform.position.z};
                for (float v : origin){
                    uint32_t bits;
                    std::memcpy(&bits, &v, sizeof(bits));
                    hash = (hash ^ bits) * 1099511628211ull;
                }
            }
            return hash;
        }
    };

}

TEST_F(SplineSystemTest, ExtrudeBuildsEngineOwnedChunks){
    const Entity owner = road(5, 10.0f);
    system->rebuildNow(owner);
    const std::vector<Entity> chunks = system->getGeneratedEntities(owner);
    ASSERT_FALSE(chunks.empty());
    const SplineBuildStats stats = system->getExtrudeStats(owner);
    EXPECT_GT(stats.vertices, 0u);
    EXPECT_EQ(stats.chunks, chunks.size());
    for (Entity chunk : chunks){
        EXPECT_EQ(system->getGeneratedOwner(chunk), owner);
        EXPECT_TRUE(system->isGenerated(chunk));
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(chunk);
        EXPECT_GT(mesh.buffer.getCount(), 0u);
        // 16-bit indices: a chunk never passes the uint16 range (R27)
        EXPECT_LE(mesh.buffer.getCount(), SplineSystem::CHUNK_MAX_VERTICES);
    }
    EXPECT_FALSE(system->isGenerated(owner));
    EXPECT_FALSE(system->getGeneratedBounds(owner).isNull());
}

TEST_F(SplineSystemTest, OnlyTheChunksAChangeTouchesRebuild){
    // ~400 units: several 64-unit chunks; moving the last knot leaves the first chunks as they were
    const Entity owner = road(41, 10.0f);
    system->rebuildNow(owner);
    const SplineBuildStats first = system->getExtrudeStats(owner);
    ASSERT_GE(first.chunks, 4u);
    EXPECT_EQ(first.chunksRebuilt, first.chunks);
    const std::vector<Entity> before = system->getGeneratedEntities(owner);

    SplineComponent& spline = scene->getComponent<SplineComponent>(owner);
    spline::SplineKnot last = spline.spline.knot(spline.spline.knotCount() - 1);
    last.position.z += 4.0f;
    spline.spline.setKnot(spline.spline.knotCount() - 1, last);
    system->rebuildNow(owner);
    const SplineBuildStats second = system->getExtrudeStats(owner);
    EXPECT_GT(second.generation, first.generation);
    EXPECT_GE(second.chunksRebuilt, 1u);
    EXPECT_LT(second.chunksRebuilt, second.chunks);
    // the untouched chunks are the same GameObjects
    const std::vector<Entity> after = system->getGeneratedEntities(owner);
    EXPECT_EQ(before.front(), after.front());
}

TEST_F(SplineSystemTest, RemovingOrDisablingFreesTheChunks){
    const Entity owner = road(5, 10.0f);
    system->rebuildNow(owner);
    const std::vector<Entity> chunks = system->getGeneratedEntities(owner);
    ASSERT_FALSE(chunks.empty());

    scene->getComponent<SplineExtrudeComponent>(owner).enabled = false;
    system->update(0.016);
    EXPECT_TRUE(system->getGeneratedEntities(owner).empty());
    for (Entity chunk : chunks){
        EXPECT_FALSE(scene->isEntityCreated(chunk));
    }

    scene->getComponent<SplineExtrudeComponent>(owner).enabled = true;
    system->rebuildNow(owner);
    ASSERT_FALSE(system->getGeneratedEntities(owner).empty());
    scene->removeComponent<SplineComponent>(owner);
    system->update(0.016);
    EXPECT_TRUE(system->getGeneratedEntities(owner).empty());
}

TEST_F(SplineSystemTest, FillBuildsAClosedPolygonAndRefusesACrossedOne){
    const Entity owner = road(4, 10.0f, SplineExtrudeShape::Fill);
    SplineComponent& spline = scene->getComponent<SplineComponent>(owner);
    std::vector<spline::SplineKnot> square;
    for (Vector3 p : {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(10, 0, -10), Vector3(0, 0, -10)}){
        spline::SplineKnot knot;
        knot.position = p;
        square.push_back(knot);
    }
    spline.spline.setType(spline::SplineType::Linear);
    spline.spline.setKnots(square);
    spline.spline.setClosed(true);
    system->rebuildNow(owner);
    EXPECT_GT(system->getExtrudeStats(owner).triangles, 0u);
    EXPECT_TRUE(system->getExtrudeWarnings(owner).empty());

    // a bow tie crosses itself: refused with a warning, nothing built
    std::swap(square[1], square[2]);
    spline.spline.setKnots(square);
    system->rebuildNow(owner);
    EXPECT_EQ(system->getExtrudeStats(owner).triangles, 0u);
    bool warned = false;
    for (const SplineWarning& warning : system->getExtrudeWarnings(owner)){
        warned = warned || warning.kind == SplineWarningKind::Triangulation;
    }
    EXPECT_TRUE(warned);
}

TEST_F(SplineSystemTest, TwoDimensionalSplinesBuildInTheXYPlane){
    const Entity owner = road(4, 5.0f, SplineExtrudeShape::Flat);
    SplineComponent& spline = scene->getComponent<SplineComponent>(owner);
    std::vector<spline::SplineKnot> knots;
    for (int i = 0; i < 4; i++){
        spline::SplineKnot knot;
        knot.position = Vector3(5.0f * static_cast<float>(i), 2.0f * std::sin(static_cast<float>(i)), 0.0f);
        knot.radius = 0.5f;
        knots.push_back(knot);
    }
    spline.spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
    spline.spline.setKnots(knots);
    system->rebuildNow(owner);
    for (Entity chunk : system->getGeneratedEntities(owner)){
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(chunk);
        const Transform& transform = scene->getComponent<Transform>(chunk);
        const AABB& bounds = mesh.verticesAABB;
        // an edge lies in z = 0
        EXPECT_NEAR(bounds.getMinimum().z + transform.position.z, 0.0f, 1.0e-4f);
        EXPECT_NEAR(bounds.getMaximum().z + transform.position.z, 0.0f, 1.0e-4f);
    }
}

TEST_F(SplineSystemTest, SplineAnimateOnceReachesTheEndAtConstantSpeed){
    const Entity path = scene->createEntity();
    scene->addComponent<Transform>(path, {});
    SplineComponent spline;
    spline.spline.setKnots(straightKnots(6, 8.0f, 1.0f));
    scene->addComponent<SplineComponent>(path, spline);

    const Entity cart = scene->createEntity();
    scene->addComponent<Transform>(cart, {});
    SplineAnimateComponent animate;
    animate.spline = path;
    animate.duration = 2.0f;
    animate.loop = SplineLoop::Once;
    animate.alignment = SplineAlignment::SplineTangent;
    scene->addComponent<SplineAnimateComponent>(cart, animate);
    int completions = 0;
    scene->getComponent<SplineAnimateComponent>(cart).onComplete = [&completions](){ completions++; };

    const float length = Spline(scene.get(), path).getLength();
    ASSERT_GT(length, 0.0f);
    system->update(0.0);   // awakes: placed at the start
    float previous = 0.0f;
    for (int frame = 0; frame < 60; frame++){
        system->update(1.0 / 60.0);
        const float distance = scene->getComponent<SplineAnimateComponent>(cart).distance;
        // constant world speed: the same distance every frame
        EXPECT_NEAR(distance - previous, length / 120.0f, length * 1.0e-3f);
        previous = distance;
    }
    EXPECT_NEAR(previous, length * 0.5f, length * 0.01f);
    for (int frame = 0; frame < 90; frame++){
        system->update(1.0 / 60.0);
    }
    const SplineAnimateComponent& done = scene->getComponent<SplineAnimateComponent>(cart);
    EXPECT_NEAR(done.distance, length, length * 0.01f);
    EXPECT_TRUE(done.completed);
    EXPECT_FALSE(done.playing);
    EXPECT_EQ(completions, 1);
    // at the end of the curve, facing along it
    const spline::SplineFrame end = Spline(scene.get(), path).evaluateAtDistance(length);
    EXPECT_LT((scene->getComponent<Transform>(cart).position - end.position).length(), 1.0e-3f);

    // paused (the editor out of Play): nothing moves
    scene->getComponent<SplineAnimateComponent>(cart).distance = 0.0f;
    system->restart(cart);
    system->setPaused(true);
    const Vector3 held = scene->getComponent<Transform>(cart).position;
    system->update(0.5);
    EXPECT_EQ(scene->getComponent<Transform>(cart).position, held);
    system->setPaused(false);
}

TEST_F(SplineSystemTest, AFlowingChannelAnswersTheWaterQuery){
    const Entity river = road(5, 10.0f, SplineExtrudeShape::Flat);
    SplineExtrudeComponent& extrude = scene->getComponent<SplineExtrudeComponent>(river);
    extrude.flow = true;
    extrude.flowSpeed = 1.0f;
    system->rebuildNow(river);
    ASSERT_TRUE(system->hasWaterSurfaces());
    const Vector3 centre = Spline(scene.get(), river).evaluate(0.5f).position;
    const SplineWaterSample on = system->sampleWaterSurface(centre.x, centre.z);
    EXPECT_TRUE(on.hit);
    EXPECT_NEAR(on.height, centre.y, 1.0e-3f);
    EXPECT_EQ(on.owner, river);
    EXPECT_GT(on.flow.length(), 0.0f);
    const SplineWaterSample off = system->sampleWaterSurface(centre.x, centre.z + 50.0f);
    EXPECT_FALSE(off.hit);
}

TEST_F(SplineSystemTest, AnEmptySceneCostsNothing){
    // no spline components: the update returns before any work (R21; the timing is the bench's)
    for (int i = 0; i < 100; i++) system->update(1.0 / 60.0);
    EXPECT_FALSE(system->hasWaterSurfaces());
    EXPECT_TRUE(system->getGeneratedEntities(1).empty());
}

// R16, AC15: the road fixture's vertex buffers hash the same on GCC, Clang and MSVC (no
// contraction in the spline sources, no unordered output). The value is pinned on GCC (Linux CI).
TEST_F(SplineSystemTest, RoadFixtureVerticesAreDeterministic){
    const Entity owner = road(9, 12.5f);
    system->rebuildNow(owner);
    const uint64_t first = vertexHash(owner);
    // the same inputs again: bit-identical
    scene->getComponent<SplineComponent>(owner).spline.markDirty();
    system->rebuildNow(owner);
    EXPECT_EQ(vertexHash(owner), first);
    EXPECT_EQ(first, ROAD_FIXTURE_VERTEX_HASH) << "the road fixture's vertices changed: 0x" << std::hex << first;
}

#ifdef JASPERENGINE_PHYSICS_2D
// R13, AC14: a 2D edge with Chain2D collision is a static Box2D chain a ball comes to rest on
TEST_F(SplineSystemTest, AnEdgeWithChain2DCarriesABall){
    constexpr float DT = 1.0f / 60.0f;
    test::disableSubSystems(*scene);
    scene->getScheduler().setEnabled("PhysicsSystem", true);
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    physics->setPointsToMeterScale2D(64.0f);
    physics->setGravity2D(0.0f, -9.81f);

    const Entity edge = road(3, 200.0f, SplineExtrudeShape::Flat);
    SplineComponent& spline = scene->getComponent<SplineComponent>(edge);
    spline.spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
    std::vector<spline::SplineKnot> knots;
    for (float x : {-200.0f, 0.0f, 200.0f}){
        spline::SplineKnot knot;
        knot.position = Vector3(x, x == 0.0f ? -110.0f : -100.0f, 0.0f);
        knot.radius = 4.0f;
        knots.push_back(knot);
    }
    spline.spline.setKnots(knots);
    scene->getComponent<SplineExtrudeComponent>(edge).collision = SplineCollision::Chain2D;

    const Entity ball = scene->createEntity();
    scene->addComponent<Transform>(ball, {});
    Transform& transform = scene->getComponent<Transform>(ball);
    transform.position = Vector3(0.0f, 50.0f, 0.0f);
    transform.needUpdate = true;
    Rigidbody2DComponent body;
    body.type = BodyType::DYNAMIC;
    body.colliders.resize(1);
    body.colliders[0].type = Collider2DType::CIRCLE;
    body.colliders[0].radius = 10.0f;
    scene->addComponent<Rigidbody2DComponent>(ball, body);

    for (int frame = 0; frame < 240; frame++){
        system->update(DT);
        scene->fixedUpdate(DT);
    }
    std::vector<Entity> chainBodies;
    for (Entity generated : system->getGeneratedEntities(edge)){
        if (scene->findComponent<Rigidbody2DComponent>(generated)){
            chainBodies.push_back(generated);
        }
    }
    ASSERT_FALSE(chainBodies.empty()) << "no chain body";
    const Rigidbody2DComponent& chain = scene->getComponent<Rigidbody2DComponent>(chainBodies[0]);
    EXPECT_EQ(chain.type, BodyType::STATIC);
    ASSERT_GE(chain.colliders.size(), 1u);
    EXPECT_EQ(chain.colliders[0].type, Collider2DType::CHAIN);
    // resting on the curve under it (its lowest point -110 at x = 0): centre one radius above
    const float y = scene->getComponent<Transform>(ball).worldPosition.y;
    const spline::SplineHit under = spline::closestPoint(spline.spline, Vector3(0.0f, y, 0.0f));
    EXPECT_NEAR(y, under.position.y + 10.0f, 1.5f) << "the ball fell to " << y;
}
#endif
