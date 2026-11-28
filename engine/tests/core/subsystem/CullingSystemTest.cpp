// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Culling Groups and Simulation Culling (docs/specs/culling-group-and-simulation-distance.md
// AC1-AC4, AC6, AC11, AC13, AC14): a scene whose scheduled systems are off, its CullingSystem
// driven directly (evaluateNow, update) and its physics stepped as the engine's fixed phase does.
// The scripted cases (counted callbacks, the reduced band's delta, messages) are in
// editor/tests/culling/SimulationCullingScriptTest.cpp, which runs whole engine frames.

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/SimulationCullingComponent.h"
#include "component/Transform.h"
#include "object/CullingGroup.h"
#include "object/SimulationCulling.h"
#include "object/physics/Rigidbody.h"
#include "object/physics/Rigidbody2D.h"
#include "script/ScriptBase.h"
#include "subsystem/CullingSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TransformPropagation.h"
#include "../ecs/EcsTestUtil.h"
#include "../support/AllocationCounter.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace CullingSystemTest_cpp {

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    // a frustum with a 90 degree opening looking along +Z (forward) or -Z, `far` metres deep (0: none)
    void setFrustum(CameraComponent& camera, const Vector3& eye, bool forward, float farDistance){
        const float s = forward ? 1.0f : -1.0f;
        const float k = 0.70710678f;
        const Vector3 normals[6] = {
            Vector3(0, 0, s),            // near
            Vector3(0, 0, -s),           // far
            Vector3(k, 0, s * k),        // left
            Vector3(-k, 0, s * k),       // right
            Vector3(0, -k, s * k),       // top
            Vector3(0, k, s * k),        // bottom
        };
        for (int p = 0; p < 6; p++){
            camera.frustumPlanes[p].normal = normals[p];
            camera.frustumPlanes[p].d = -normals[p].dotProduct(eye);
        }
        camera.frustumPlanes[0].d -= 0.1f;
        camera.frustumPlanes[1].d += farDistance;
        camera.farClip = farDistance;
    }

    // an orthographic box: |x - eye.x| <= half, |y - eye.y| <= half, eye.z <= z <= eye.z + depth
    void setOrtho(CameraComponent& camera, const Vector3& eye, float half, float depth){
        const Vector3 normals[6] = {Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(0, 1, 0)};
        const float extra[6] = {0.0f, depth, half, half, half, half};
        for (int p = 0; p < 6; p++){
            camera.frustumPlanes[p].normal = normals[p];
            camera.frustumPlanes[p].d = -normals[p].dotProduct(eye) + extra[p];
        }
        camera.farClip = depth;
    }

    class CullingTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        CullingSystem* culling = nullptr;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            SimulationCulling::setDistanceScale(1.0f);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            culling = scene->getSystem<CullingSystem>().get();
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        void TearDown() override{
            scene.reset();
            SimulationCulling::setDistanceScale(1.0f);
            Engine::setUpdateTime(updateTime);
        }

        Entity object(const Vector3& position, Entity parent = NULL_ENTITY){
            const Entity entity = test::makeTransformEntity(*scene, parent);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
            return entity;
        }

        Entity camera(const Vector3& eye, bool forward = true, float farDistance = 1000.0f, bool main = true){
            const Entity entity = object(eye);
            CameraComponent component;
            component.type = CameraType::CAMERA_PERSPECTIVE;
            setFrustum(component, eye, forward, farDistance);
            scene->addComponent<CameraComponent>(entity, component);
            if (main) scene->setCamera(entity);
            return entity;
        }

        void moveTo(Entity entity, const Vector3& position){
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
        }

        // one frame: the world transforms (Render's part), then the culling update
        void frame(){
            resolveTransforms();
            culling->update(DT);
        }

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        void step(){
            resolveTransforms();
            Input::beginFixedStep();
            scene->fixedUpdate(DT);
            Input::endFixedStep();
            resolveTransforms();
        }

        Entity body(const Vector3& position, BodyType type = BodyType::DYNAMIC, float size = 1.0f){
            const Entity entity = object(position);
            RigidbodyComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider3DType::BOX;
            rigidbody.colliders[0].width = size;
            rigidbody.colliders[0].height = size;
            rigidbody.colliders[0].depth = size;
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        SimulationCullingComponent& cull(Entity entity, float distance){
            SimulationCullingComponent component;
            component.simulationDistance = distance;
            component.boundsRadius = 0.5f;
            component.hysteresis = 0.0f;
            component.simulateWhenVisible = false;
            scene->addComponent<SimulationCullingComponent>(entity, component);
            return scene->getComponent<SimulationCullingComponent>(entity);
        }

        float y(Entity entity){
            return scene->getComponent<Transform>(entity).worldPosition.y;
        }
    };

    class CullingGroupTest : public CullingTest {};
    class SimulationCullingTest : public CullingTest {};

    struct Recorder {
        std::vector<CullingGroupEvent> events;
        void add(CullingGroup& group){
            group.onStateChanged().add("recorder", [this](CullingGroupEvent event){ events.push_back(event); });
        }
    };

}

TEST_F(CullingGroupTest, BandsVisibilityAndEvents){
    // AC1
    camera(Vector3(0, 0, 0), true);
    CullingGroup group(scene.get());
    ASSERT_TRUE(group.isValid());
    const float distances[] = {10.0f, 20.0f};
    group.setBoundingDistances(distances);
    group.setDistanceReferencePoint(Vector3(0, 0, 0));
    const BoundingSphere spheres[] = {
        BoundingSphere(Vector3(0, 0, 5), 0.0f), BoundingSphere(Vector3(0, 0, 15), 0.0f), BoundingSphere(Vector3(0, 0, 25), 0.0f),
        BoundingSphere(Vector3(0, 0, -5), 0.0f), BoundingSphere(Vector3(0, 0, 8), 0.0f),
    };
    group.setBoundingSpheres(spheres);
    Recorder recorder;
    recorder.add(group);
    EXPECT_EQ(group.getDistance(0), -1);

    frame();
    ASSERT_EQ(recorder.events.size(), 5u);
    const int bands[] = {0, 1, 2, 0, 0};
    const bool visible[] = {true, true, true, false, true};
    for (uint32_t i = 0; i < 5; i++){
        EXPECT_EQ(recorder.events[i].index, i);
        EXPECT_EQ(recorder.events[i].previousDistance, -1);
        EXPECT_EQ(recorder.events[i].currentDistance, bands[i]);
        EXPECT_EQ(recorder.events[i].isVisible, visible[i]);
        EXPECT_FALSE(recorder.events[i].wasVisible);
        EXPECT_EQ(group.getDistance(i), bands[i]);
    }

    // sphere 4 crosses 10 m and leaves the frustum (behind the camera)
    recorder.events.clear();
    group.setSphere(4, BoundingSphere(Vector3(0, 0, -12), 0.0f));
    frame();
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].index, 4u);
    EXPECT_FALSE(recorder.events[0].isVisible);
    EXPECT_TRUE(recorder.events[0].wasVisible);
    EXPECT_TRUE(recorder.events[0].hasBecomeInvisible());
    EXPECT_EQ(recorder.events[0].currentDistance, 1);
    EXPECT_EQ(recorder.events[0].previousDistance, 0);

    // nothing changed: nothing fires
    recorder.events.clear();
    frame();
    EXPECT_TRUE(recorder.events.empty());

    // a hysteresis of 1 m keeps a sphere at 10.5 m in band 0
    group.setBandHysteresis(1.0f);
    group.setSphere(0, BoundingSphere(Vector3(0, 0, 10.5f), 0.0f));
    frame();
    EXPECT_EQ(group.getDistance(0), 0);
    EXPECT_TRUE(recorder.events.empty());
    group.setSphere(0, BoundingSphere(Vector3(0, 0, 11.5f), 0.0f));
    frame();
    EXPECT_EQ(group.getDistance(0), 1);
    ASSERT_EQ(recorder.events.size(), 1u);
    group.setBandHysteresis(0.0f);

    // disabled: no events, states freeze; enabled again: only what changed meanwhile
    recorder.events.clear();
    group.setEnabled(false);
    group.setSphere(1, BoundingSphere(Vector3(0, 0, 3), 0.0f));
    group.setSphere(2, BoundingSphere(Vector3(0, 0, 30), 0.0f));
    frame();
    EXPECT_TRUE(recorder.events.empty());
    EXPECT_EQ(group.getDistance(1), 1);
    group.setEnabled(true);
    frame();
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].index, 1u);
    EXPECT_EQ(recorder.events[0].currentDistance, 0);
    EXPECT_EQ(recorder.events[0].previousDistance, 1);
}

TEST_F(CullingGroupTest, AnyCameraOrTarget){
    // AC2: two cameras looking apart
    const Entity front = camera(Vector3(0, 0, 0), true, 50.0f);
    const Entity back = camera(Vector3(0, 0, 0), false, 50.0f, false);
    CullingGroup group(scene.get());
    const BoundingSphere spheres[] = {BoundingSphere(Vector3(0, 0, 10), 1.0f), BoundingSphere(Vector3(0, 0, -10), 1.0f),
                                      BoundingSphere(Vector3(0, 0, 100), 1.0f)};
    group.setBoundingSpheres(spheres);
    frame();
    EXPECT_TRUE(group.isVisible(0));
    EXPECT_TRUE(group.isVisible(1));
    EXPECT_FALSE(group.isVisible(2)) << "past both far planes";

    group.setTargetCamera(back);
    frame();
    EXPECT_FALSE(group.isVisible(0));
    EXPECT_TRUE(group.isVisible(1));

    // a disabled target: every sphere invisible
    scene->getComponent<CameraComponent>(back).enabled = false;
    frame();
    EXPECT_FALSE(group.isVisible(0));
    EXPECT_FALSE(group.isVisible(1));
    scene->getComponent<CameraComponent>(back).enabled = true;

    // far clip 0 skips the far plane
    group.setTargetCamera(front);
    CameraComponent& frontCamera = scene->getComponent<CameraComponent>(front);
    setFrustum(frontCamera, Vector3(0, 0, 0), true, 50.0f);
    frontCamera.farClip = 0.0f;
    frame();
    EXPECT_TRUE(group.isVisible(2));

    // an orthographic camera's planes
    setOrtho(frontCamera, Vector3(0, 0, 0), 5.0f, 50.0f);
    group.setSphere(0, BoundingSphere(Vector3(4, 0, 10), 0.5f));
    group.setSphere(1, BoundingSphere(Vector3(8, 0, 10), 0.5f));
    group.setSphere(2, BoundingSphere(Vector3(0, 0, 60), 0.5f));
    frame();
    EXPECT_TRUE(group.isVisible(0));
    EXPECT_FALSE(group.isVisible(1));
    EXPECT_FALSE(group.isVisible(2));
}

TEST_F(CullingGroupTest, FollowEraseAndQuery){
    // AC3
    camera(Vector3(0, 0, 0), true);
    CullingGroup group(scene.get());
    const float distances[] = {10.0f};
    group.setBoundingDistances(distances);
    group.setDistanceReferencePoint(Vector3(0, 0, 0));
    group.setBoundingSphereCount(3);
    const Entity a = object(Vector3(0, 0, 5));
    const Entity b = object(Vector3(0, 0, 6));
    resolveTransforms();
    group.setSphereTarget(0, a, 1.0f);
    group.setSphereTarget(1, b, 1.0f, Vector3(0, 1, 0));
    group.setSphere(2, BoundingSphere(Vector3(0, 0, 30), 1.0f));
    frame();
    EXPECT_EQ(group.getDistance(0), 0);
    moveTo(a, Vector3(0, 0, 40));
    frame();
    EXPECT_EQ(group.getDistance(0), 1);
    EXPECT_NEAR(group.getSphere(0).position.z, 40.0f, 1e-4f);
    EXPECT_NEAR(group.getSphere(1).position.y, 1.0f, 1e-4f);

    // a destroyed GameObject's sphere keeps its last position, with one warning
    {
        test::LogCapture capture;
        scene->destroyEntity(b);
        frame();
        frame();
        EXPECT_EQ(capture.count(D_LOG_WARN, "keeps its last position"), 1u);
    }
    EXPECT_NEAR(group.getSphere(1).position.z, 6.0f, 1e-4f);

    // eraseSwapBack moves the last sphere and its state
    EXPECT_EQ(group.getDistance(2), 1);
    group.eraseSwapBack(0);
    EXPECT_EQ(group.getBoundingSphereCount(), 2u);
    EXPECT_NEAR(group.getSphere(0).position.z, 30.0f, 1e-4f);
    EXPECT_EQ(group.getDistance(0), 1);

    // the three forms with `first` against a brute force over 1,000 random spheres
    std::mt19937 random(7);
    std::uniform_real_distribution<float> coordinate(-60.0f, 60.0f);
    std::vector<BoundingSphere> many(1000);
    for (BoundingSphere& sphere : many) sphere = BoundingSphere(Vector3(coordinate(random), coordinate(random), coordinate(random)), 1.0f);
    group.setBoundingSpheres(many);
    const float bands[] = {20.0f, 40.0f};
    group.setBoundingDistances(bands);
    frame();
    std::vector<uint32_t> out(1000);
    for (uint32_t first : {0u, 3u, 50u}){
        for (int band = 0; band <= 2; band++){
            for (bool visible : {false, true}){
                std::vector<uint32_t> expectVisible, expectBand, expectBoth;
                for (uint32_t i = 0; i < 1000; i++){
                    if (group.isVisible(i) == visible) expectVisible.push_back(i);
                    if (group.getDistance(i) == band) expectBand.push_back(i);
                    if (group.isVisible(i) == visible && group.getDistance(i) == band) expectBoth.push_back(i);
                }
                auto check = [&](const std::vector<uint32_t>& expected, uint32_t n){
                    const size_t want = expected.size() > first ? expected.size() - first : 0;
                    ASSERT_EQ(n, want);
                    for (uint32_t k = 0; k < n; k++) { EXPECT_EQ(out[k], expected[first + k]); }
                };
                check(expectVisible, group.queryIndices(visible, out, first));
                check(expectBand, group.queryIndices(band, out, first));
                check(expectBoth, group.queryIndices(visible, band, out, first));
            }
        }
    }
}

TEST_F(CullingGroupTest, ParallelEqualsSerial){
    // AC4: 20,000 spheres with jobs and inline: identical states and event sequences
    camera(Vector3(0, 0, 0), true, 300.0f);
    std::mt19937 random(11);
    std::uniform_real_distribution<float> coordinate(-200.0f, 200.0f);
    std::vector<BoundingSphere> spheres(20000);
    for (BoundingSphere& sphere : spheres) sphere = BoundingSphere(Vector3(coordinate(random), coordinate(random), coordinate(random)), 2.0f);
    const float distances[] = {25.0f, 50.0f, 100.0f, 150.0f};
    std::vector<CullingGroupEvent> sequences[2];
    std::vector<int> states[2];
    for (int run = 0; run < 2; run++){
        culling->setParallelEvaluation(run == 0);
        CullingGroup group(scene.get());
        group.setBoundingDistances(distances);
        group.setBoundingSpheres(spheres);
        group.onStateChanged().add("seq", [&sequences, run](CullingGroupEvent event){ sequences[run].push_back(event); });
        frame();
        // a move of the reference point changes thousands
        group.setDistanceReferencePoint(Vector3(30, 0, 30));
        frame();
        for (uint32_t i = 0; i < spheres.size(); i++) states[run].push_back(group.getDistance(i) * 2 + (group.isVisible(i) ? 1 : 0));
        group.dispose();
    }
    culling->setParallelEvaluation(true);
    ASSERT_GT(sequences[0].size(), 20000u);
    ASSERT_EQ(sequences[0].size(), sequences[1].size());
    EXPECT_EQ(std::memcmp(sequences[0].data(), sequences[1].data(), sequences[0].size() * sizeof(CullingGroupEvent)), 0);
    EXPECT_EQ(states[0], states[1]);
}

TEST_F(CullingGroupTest, ManyFollowsReadOnWorkers){
    // R22: above PARALLEL_THRESHOLD the follows are read on the workers; a destroyed GameObject
    // still warns once and leaves its sphere where it was, and the others keep following
    camera(Vector3(0, 0, 0), true);
    for (int run = 0; run < 2; run++){
        culling->setParallelEvaluation(run == 0);
        CullingGroup group(scene.get());
        const float distances[] = {10.0f};
        group.setBoundingDistances(distances);
        group.setDistanceReferencePoint(Vector3(0, 0, 0));
        const uint32_t count = CullingSystem::PARALLEL_THRESHOLD + 904;
        group.setBoundingSphereCount(count);
        std::vector<Entity> targets;
        for (uint32_t i = 0; i < count; i++) targets.push_back(object(Vector3(static_cast<float>(i % 50), 0, 5)));
        resolveTransforms();
        for (uint32_t i = 0; i < count; i++) group.setSphereTarget(i, targets[i], 0.5f);
        frame();
        moveTo(targets[4000], Vector3(0, 0, 40));
        frame();
        EXPECT_NEAR(group.getSphere(4000).position.z, 40.0f, 1e-4f) << run;
        EXPECT_EQ(group.getDistance(4000), 1) << run;
        {
            test::LogCapture capture;
            scene->destroyEntity(targets[17]);
            moveTo(targets[4000], Vector3(0, 0, 2));
            frame();
            frame();
            EXPECT_EQ(capture.count(D_LOG_WARN, "keeps its last position"), 1u) << run;
        }
        EXPECT_NEAR(group.getSphere(17).position.x, 17.0f, 1e-4f) << run;
        EXPECT_NEAR(group.getSphere(4000).position.z, 2.0f, 1e-4f) << run;
        EXPECT_EQ(group.getDistance(4000), 0) << run;
        moveTo(targets[count - 1], Vector3(0, 0, 60));
        frame();
        EXPECT_NEAR(group.getSphere(count - 1).position.z, 60.0f, 1e-4f) << run << ": the follow moved into the dropped one's slot still follows";
        group.dispose();
        for (Entity target : targets) if (target != targets[17]) scene->destroyEntity(target);
    }
    culling->setParallelEvaluation(true);
}

TEST_F(CullingGroupTest, NoAllocationsAndZeroCost){
    // AC6: a scene with neither groups nor components touches nothing and allocates nothing
    {
        testsupport::AllocationCountScope allocations;
        for (int i = 0; i < 100; i++) culling->update(DT);
        EXPECT_EQ(allocations.count(), 0u);
    }
    EXPECT_EQ(culling->getEvaluatedSpheres(), 0u);
    EXPECT_FALSE(SimulationCulling::anyActive());

    camera(Vector3(0, 0, 0), true);
    CullingGroup group(scene.get());
    const float distances[] = {10.0f, 20.0f, 30.0f, 40.0f};
    group.setBoundingDistances(distances);
    std::vector<BoundingSphere> spheres(10000);
    for (size_t i = 0; i < spheres.size(); i++) spheres[i] = BoundingSphere(Vector3(0, 0, static_cast<float>(i % 50)), 0.5f);
    group.setBoundingSpheres(spheres);
    uint64_t events = 0;
    group.onStateChanged().add("count", [&events](CullingGroupEvent){ events++; });
    resolveTransforms();
    culling->update(DT);
    // every evaluation changes them: the reference point swings between two places. Evaluated inline
    // (as on single-threaded web): nothing in the process allocates. (On the workers S06's parallelFor
    // publishing its helpers allocates on some runs: see the spec's Deviations.)
    {
        culling->setParallelEvaluation(false);
        testsupport::AllocationCountScope allocations;
        for (int i = 0; i < 1000; i++){
            group.setDistanceReferencePoint(Vector3(0, 0, (i % 2) ? 25.0f : 0.0f));
            culling->update(DT);
        }
        EXPECT_EQ(allocations.count(), 0u);
        culling->setParallelEvaluation(true);
    }
    EXPECT_GT(events, 10000u);

    // R24: 20 bytes a sphere, a followed one 16 more (here 20: its index, entity and offset)
    group.dispose();
    CullingGroup measured(scene.get());
    measured.setBoundingSphereCount(10000);
    std::vector<Entity> targets;
    for (int i = 0; i < 2000; i++) targets.push_back(object(Vector3(0, 0, 0)));
    for (uint32_t i = 0; i < 2000; i++) measured.setSphereTarget(i, targets[i], 1.0f);
    const double bytes = static_cast<double>(culling->getMemoryBytes());
    EXPECT_NEAR(bytes, 232000.0, 232000.0 * 0.05);
}

// a script in a persistent scene can keep a group of a scene that unloads: the handle reads the
// scene's lifetime, never the freed scene
TEST_F(CullingGroupTest, HandleOfAGoneSceneIsInvalid){
    auto other = std::make_unique<Scene>();
    test::disableSubSystems(*other);
    CullingGroup group = CullingGroup::create(other.get());
    ASSERT_TRUE(group.isValid());
    group.setBoundingSphereCount(4);
    other.reset();
    ASSERT_EQ(group.getScene(), nullptr) << "the handle still names the freed scene";
    EXPECT_FALSE(group.isValid());
    EXPECT_EQ(group.getBoundingSphereCount(), 0u);
    group.setBoundingSphereCount(8);
    group.dispose();
}

// ------------------------------------------------------------------ Simulation Culling

TEST_F(SimulationCullingTest, DistanceFreezesBodies){
    // AC7's bodies (the counted callbacks are in the editor's script test): 100 m away the body does
    // not fall; moved to 10 m it resumes with its pre-freeze velocity
    camera(Vector3(0, 0, 0), true);
    const Entity falling = body(Vector3(0, 50, 100));
    SimulationCullingComponent& component = cull(falling, 64.0f);
    culling->evaluateNow();
    step();
    // give it a velocity before it freezes
    Rigidbody handle(scene.get(), falling);
    handle.setLinearVelocity(Vector3(0, -3, 0));
    frame();
    ASSERT_EQ(component.state, SimulationState::Culled);
    EXPECT_EQ(component.reason, SimulationCullReason::Distance);
    const float start = y(falling);
    for (int i = 0; i < 300; i++) step();
    EXPECT_NEAR(y(falling), start, 1e-3f);
    EXPECT_NEAR(handle.getLinearVelocity().y, -3.0f, 1e-4f);

    moveTo(falling, Vector3(0, 50, 10));
    // the Transform moved by hand: the body follows it at the next sync
    step();
    frame();
    EXPECT_EQ(component.state, SimulationState::Full);
    EXPECT_NEAR(handle.getLinearVelocity().y, -3.0f, 0.2f);
    const float resumed = y(falling);
    for (int i = 0; i < 30; i++) step();
    EXPECT_LT(y(falling), resumed - 1.0f);
}

TEST_F(SimulationCullingTest, FocusesAndTierScale){
    // AC11
    camera(Vector3(0, 0, 0), true);
    const Entity near = object(Vector3(30, 0, 0));
    const Entity far = object(Vector3(200, 0, 0));
    SimulationCullingComponent& nearCulling = cull(near, 50.0f);
    SimulationCullingComponent& farCulling = cull(far, 50.0f);
    culling->evaluateNow();
    EXPECT_EQ(nearCulling.state, SimulationState::Full);
    EXPECT_EQ(farCulling.state, SimulationState::Culled);

    // two focuses: the distance is to the nearest
    const int id = SimulationCulling::addFocus(scene.get(), Vector3(190, 0, 0));
    ASSERT_GT(id, 0);
    frame();
    EXPECT_EQ(farCulling.state, SimulationState::Full);
    EXPECT_NEAR(farCulling.focusDistance, 9.5f, 1e-3f);

    // a focus GameObject is never culled
    const Entity player = object(Vector3(5000, 0, 0));
    SimulationCullingComponent& playerCulling = cull(player, 10.0f);
    const int playerFocus = SimulationCulling::addFocus(scene.get(), player);
    frame();
    EXPECT_EQ(playerCulling.state, SimulationState::Full);

    // no script focus left and the camera not one: everything is culled by distance
    SimulationCulling::removeFocus(scene.get(), id);
    SimulationCulling::removeFocus(scene.get(), playerFocus);
    SimulationCulling::setCameraIsFocus(scene.get(), false);
    const int faraway = SimulationCulling::addFocus(scene.get(), Vector3(-10000, 0, 0));
    for (int i = 0; i < 3; i++) frame();
    EXPECT_EQ(nearCulling.state, SimulationState::Culled);
    EXPECT_EQ(nearCulling.reason, SimulationCullReason::Distance);
    SimulationCulling::removeFocus(scene.get(), faraway);
    SimulationCulling::setCameraIsFocus(scene.get(), true);
    frame();
    EXPECT_EQ(nearCulling.state, SimulationState::Full);

    // a scale of 0.5 halves the effective distances: 30 m is past 50 * 0.5
    SimulationCulling::setDistanceScale(0.5f);
    frame();
    EXPECT_EQ(nearCulling.state, SimulationState::Culled);
    SimulationCulling::setDistanceScale(1.0f);
    frame();
    EXPECT_EQ(nearCulling.state, SimulationState::Full);
}

TEST_F(SimulationCullingTest, FirstEvaluationBeforeFirstStep){
    // AC13 (a): 500 bodies over ground a C++ check says is not loaded, the check set before the start
    camera(Vector3(0, 0, 0), true);
    SimulationCulling::setGroundCheck(scene.get(), [](const Vector3&, float){ return false; });
    std::vector<Entity> bodies;
    for (int i = 0; i < 500; i++){
        const Entity entity = body(Vector3(static_cast<float>(i % 25) * 2.0f, 10.0f, 5.0f + static_cast<float>(i / 25) * 2.0f));
        cull(entity, 1000.0f);
        bodies.push_back(entity);
    }
    physics->load();
    culling->preUpdate();
    for (Entity entity : bodies){
        const SimulationCullingComponent& c = scene->getComponent<SimulationCullingComponent>(entity);
        EXPECT_EQ(c.state, SimulationState::Culled);
        EXPECT_EQ(c.reason, SimulationCullReason::GroundNotLoaded);
    }
    step();
    for (Entity entity : bodies) { EXPECT_NEAR(y(entity), 10.0f, 1e-4f); }
    EXPECT_EQ(culling->getStats().groundNotLoaded, 500u);
}

TEST_F(SimulationCullingTest, TransitionsArePaced){
    // AC13 (b): 500 Full components whose focus jumps 1 km away: 64 a frame, the farthest first
    const Entity eye = camera(Vector3(0, 0, 0), true);
    std::vector<Entity> objects;
    for (int i = 0; i < 500; i++){
        const Entity entity = object(Vector3(static_cast<float>(i) * 0.1f, 0, 0));
        cull(entity, 64.0f);
        objects.push_back(entity);
    }
    culling->evaluateNow();
    for (Entity entity : objects) { ASSERT_EQ(scene->getComponent<SimulationCullingComponent>(entity).state, SimulationState::Full); }
    moveTo(eye, Vector3(-1000, 0, 0));
    frame();
    EXPECT_EQ(culling->getStats().culled, 64u);
    EXPECT_EQ(culling->getStats().transitionsDeferred, 500u - 64u);
    // the farthest from the focus went first: the highest x
    for (int i = 0; i < 500; i++){
        const bool culled = scene->getComponent<SimulationCullingComponent>(objects[i]).state == SimulationState::Culled;
        EXPECT_EQ(culled, i >= 500 - 64) << i;
    }
    for (int frameIndex = 1; frameIndex < 8; frameIndex++) frame();
    EXPECT_EQ(culling->getStats().culled, 500u);
    EXPECT_EQ(culling->getStats().transitionsDeferred, 0u);
}

TEST_F(SimulationCullingTest, FrozenBodyReportsRealState){
    // AC14 (3D): the getters read what the body had, setters write what it gets at the thaw
    camera(Vector3(0, 0, 0), true);
    const Entity frozen = body(Vector3(0, 0, 100));
    SimulationCullingComponent& component = cull(frozen, 64.0f);
    component.freezeWhenGroundNotLoaded = false;
    physics->setGravity3D(0.0f, 0.0f, 0.0f);
    physics->load();
    Rigidbody handle(scene.get(), frozen);
    handle.setLinearVelocity(Vector3(1, 0, 0));
    culling->evaluateNow();
    ASSERT_TRUE(scene->getComponent<RigidbodyComponent>(frozen).simulationFrozen);
    EXPECT_FALSE(handle.isKinematic());
    EXPECT_EQ(handle.getType(), BodyType::DYNAMIC);
    EXPECT_NEAR(handle.getLinearVelocity().x, 1.0f, 1e-5f);
    handle.setLinearVelocity(Vector3(0, 0, -2));
    const float zBefore = scene->getComponent<Transform>(frozen).worldPosition.z;
    for (int i = 0; i < 30; i++) step();
    EXPECT_NEAR(scene->getComponent<Transform>(frozen).worldPosition.z, zBefore, 1e-4f);

    // another body hits it as it would hit a kinematic one: it stops, the frozen one stays
    const Entity hitter = body(Vector3(0, 0, 103));
    physics->load();
    Rigidbody(scene.get(), hitter).setLinearVelocity(Vector3(0, 0, -5));
    for (int i = 0; i < 60; i++) step();
    EXPECT_NEAR(scene->getComponent<Transform>(frozen).worldPosition.z, zBefore, 1e-3f);
    EXPECT_GT(scene->getComponent<Transform>(hitter).worldPosition.z, zBefore + 0.9f);

    // thawed: the velocity a script set meanwhile
    moveTo(scene->getCamera(), Vector3(0, 0, 90));
    frame();
    EXPECT_FALSE(scene->getComponent<RigidbodyComponent>(frozen).simulationFrozen);
    EXPECT_NEAR(handle.getLinearVelocity().z, -2.0f, 1e-4f);
}

#ifdef JASPERENGINE_PHYSICS_2D
TEST_F(SimulationCullingTest, FrozenBody2D){
    // AC14 (2D)
    camera(Vector3(0, 0, 0), true);
    const Entity entity = object(Vector3(0, 0, 100));
    Rigidbody2DComponent rigidbody;
    rigidbody.type = BodyType::DYNAMIC;
    rigidbody.colliders.resize(1);
    rigidbody.colliders[0].type = Collider2DType::CIRCLE;
    rigidbody.colliders[0].radius = 0.5f;
    scene->addComponent<Rigidbody2DComponent>(entity, rigidbody);
    SimulationCullingComponent& component = cull(entity, 64.0f);
    component.freezeWhenGroundNotLoaded = false;
    physics->load();
    Rigidbody2D handle(scene.get(), entity);
    handle.setLinearVelocity(Vector2(3, 0));
    const Vector2 before = handle.getLinearVelocity();
    culling->evaluateNow();
    ASSERT_TRUE(scene->getComponent<Rigidbody2DComponent>(entity).simulationFrozen);
    EXPECT_EQ(handle.getType(), BodyType::DYNAMIC);
    EXPECT_NEAR(handle.getLinearVelocity().x, before.x, 1e-4f);
    handle.setLinearVelocity(Vector2(0, 5));
    const float x = scene->getComponent<Transform>(entity).worldPosition.x;
    for (int i = 0; i < 30; i++) step();
    EXPECT_NEAR(scene->getComponent<Transform>(entity).worldPosition.x, x, 1e-3f);
    moveTo(scene->getCamera(), Vector3(0, 0, 90));
    frame();
    EXPECT_FALSE(scene->getComponent<Rigidbody2DComponent>(entity).simulationFrozen);
    EXPECT_NEAR(handle.getLinearVelocity().y, 5.0f, 1e-3f);
}
#endif

namespace {
    // a C++ scene script counting its update-family callbacks and an input event
    struct CountingScript : public ScriptBase {
        int updates = 0, postUpdates = 0, fixedUpdates = 0, keys = 0;
        CountingScript(Scene* scene, Entity entity): ScriptBase(scene, entity){
            REGISTER_ENGINE_EVENT(onUpdate);
            REGISTER_ENGINE_EVENT(onPostUpdate);
            REGISTER_ENGINE_EVENT(onFixedUpdate);
            REGISTER_ENGINE_EVENT(onKeyDown);
        }
        ~CountingScript() override{
            UNREGISTER_ENGINE_EVENT(onUpdate);
            UNREGISTER_ENGINE_EVENT(onPostUpdate);
            UNREGISTER_ENGINE_EVENT(onFixedUpdate);
            UNREGISTER_ENGINE_EVENT(onKeyDown);
        }
        void onUpdate(){ updates++; }
        void onPostUpdate(){ postUpdates++; }
        void onFixedUpdate(){ fixedUpdates++; }
        void onKeyDown(int, bool, int){ keys++; }
    };
}

TEST_F(SimulationCullingTest, SkipsCppUpdateCallbacks){
    // the C++ side of AC7's scripts: culled, its update callbacks are skipped, an input event is not
    camera(Vector3(0, 0, 0), true);
    const Entity far = object(Vector3(0, 0, 100));
    cull(far, 64.0f);
    CountingScript script(scene.get(), far);
    culling->evaluateNow();
    ASSERT_TRUE(SimulationCulling::anyActive());
    ASSERT_TRUE(SimulationCulling::isCulled(scene.get(), far));
    const bool enabled = Engine::onUpdate.isEnabled();
    Engine::pauseGameEvents(false);
    for (int i = 0; i < 10; i++){
        Engine::onFixedUpdate.call();
        Engine::onUpdate.call();
        Engine::onPostUpdate.call();
    }
    Engine::onKeyDown.call(65, false, 0);
    EXPECT_EQ(script.updates, 0);
    EXPECT_EQ(script.postUpdates, 0);
    EXPECT_EQ(script.fixedUpdates, 0);
    EXPECT_EQ(script.keys, 1);
    culling->preUpdate();
    EXPECT_EQ(culling->getStats().scriptUpdatesSkipped, 30u);

    moveTo(far, Vector3(0, 0, 10));
    frame();
    Engine::onUpdate.call();
    Engine::onFixedUpdate.call();
    Engine::onPostUpdate.call();
    EXPECT_EQ(script.updates, 1);
    EXPECT_EQ(script.fixedUpdates, 1);
    EXPECT_EQ(script.postUpdates, 1);
    Engine::pauseGameEvents(!enabled);
}

// the ground check runs inside the evaluation: a script's check that destroys a culled object or
// replaces itself must not pull the component or the running check from under it
TEST_F(SimulationCullingTest, GroundCheckMayReplaceItselfAndDestroyObjects){
    camera(Vector3(0, 0, 0), true);
    const Entity first = object(Vector3(0, 0, 5));
    const Entity second = object(Vector3(0, 0, 6));
    const Entity third = object(Vector3(0, 0, 7));
    cull(first, 100.0f);
    cull(second, 100.0f);
    cull(third, 100.0f);
    // destroyed while the check that holds it runs: the check's own storage went
    struct Sentinel {
        bool* running;
        bool* destroyedWhileRunning;
        ~Sentinel(){ if (*running) *destroyedWhileRunning = true; }
    };
    bool running = false;
    bool destroyedWhileRunning = false;
    bool victimAliveInCheck = false;
    int calls = 0;
    Scene* s = scene.get();
    auto sentinel = std::make_shared<Sentinel>(Sentinel{&running, &destroyedWhileRunning});
    SimulationCulling::setGroundCheck(s, [sentinel, s, second, &running, &calls, &victimAliveInCheck](const Vector3&, float){
        running = true;
        calls++;
        s->destroyEntity(second);
        victimAliveInCheck = s->isEntityCreated(second);
        SimulationCulling::setGroundCheck(s, [&calls](const Vector3&, float){ calls++; return false; });
        running = false;
        return false;
    });
    sentinel.reset();
    culling->evaluateNow();
    EXPECT_FALSE(destroyedWhileRunning) << "the check was destroyed while it ran";
    EXPECT_TRUE(victimAliveInCheck) << "a destroy inside the check waits for the evaluation's end";
    EXPECT_FALSE(scene->isEntityCreated(second)) << "and happens then";
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(scene->getComponent<SimulationCullingComponent>(first).reason, SimulationCullReason::GroundNotLoaded);
    EXPECT_EQ(scene->getComponent<SimulationCullingComponent>(third).reason, SimulationCullReason::GroundNotLoaded);
    SimulationCulling::setGroundCheck(s, nullptr);
}

// a scale a script set in one Play does not carry into the next (the editor's Stop resets the
// application state)
TEST_F(SimulationCullingTest, DistanceScaleEndsWithThePlay){
    SimulationCulling::setDistanceScale(0.25f);
    Engine::resetApplicationState();
    EXPECT_FLOAT_EQ(SimulationCulling::getDistanceScale(), 1.0f);
}

}  // namespace CullingSystemTest_cpp
