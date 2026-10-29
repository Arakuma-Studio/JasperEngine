// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// NavMeshAgent movement (docs/specs/S19-navigation.md R11-R13, R7; AC8, AC12-AC14): arrival with
// auto braking, the drivers, off-mesh links and avoidance priority.

#include "NavTestUtil.h"

#include "LogCapture.h"
#include "component/NavMeshLinkComponent.h"
#include "object/navigation/NavMeshAgent.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "component/Rigidbody2DComponent.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#include "subsystem/PhysicsSystem.h"
#endif

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    class NavAgentTest : public NavSceneTest {
    protected:
        float horizontalSpeed(const Vector3& a, const Vector3& b) const {
            return Vector3(b.x - a.x, 0.0f, b.z - a.z).length() / DT;
        }
    };

}

TEST_F(NavAgentTest, Arrival){
    const Entity surface = planeSurface(40.0f, 10.0f);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-15.0f, 0.0f, 0.0f));
    NavMeshAgentComponent& component = scene->getComponent<NavMeshAgentComponent>(walker);
    component.stoppingDistance = 0.5f;
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.isOnNavMesh());
    int reached = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    const Vector3 target(12.0f, 0.0f, 0.0f);
    ASSERT_TRUE(handle.setDestination(target));
    EXPECT_TRUE(handle.isPathPending());

    Vector3 previous = position(walker);
    float previousSpeed = 0.0f;
    float maxSpeed = 0.0f;
    int ticks = 0;
    while (reached == 0 && ticks < 60 * 20) {
        step();
        ticks++;
        const Vector3 now = position(walker);
        const float speed = horizontalSpeed(previous, now);
        // speed and acceleration limits per tick (one tick of slack for the integration)
        EXPECT_LE(speed, component.speed + 1e-3f) << "tick " << ticks;
        EXPECT_LE(speed - previousSpeed, component.acceleration * DT * 1.05f + 1e-3f) << "tick " << ticks;
        // never past the stopping distance (+0.01 m)
        EXPECT_LE(now.x, target.x - component.stoppingDistance + 0.01f) << "tick " << ticks;
        maxSpeed = std::max(maxSpeed, speed);
        previous = now;
        previousSpeed = speed;
    }
    EXPECT_EQ(reached, 1);
    EXPECT_NEAR(maxSpeed, component.speed, 0.05f);
    EXPECT_FALSE(handle.isPathPending());
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Complete);
    // auto braking: the speed fell below 0.05 m/s at the stop
    EXPECT_LT(previousSpeed, 0.05f);
    EXPECT_LE(target.x - position(walker).x, component.stoppingDistance + 0.01f);
    EXPECT_LE(handle.getRemainingDistance(), component.stoppingDistance + 0.01f);
    // once per destination
    steps(60);
    EXPECT_EQ(reached, 1);
    EXPECT_LT(handle.getVelocity().length(), 0.05f);

    // a new destination fires again
    ASSERT_TRUE(handle.setDestination(Vector3(-12.0f, 0.0f, 2.0f)));
    for (int i = 0; i < 60 * 20 && reached < 2; i++)
        step();
    EXPECT_EQ(reached, 2);
}

TEST_F(NavAgentTest, OffMeshSpawnWarnsOnce){
    const Entity surface = planeSurface();
    bake(surface);
    test::LogCapture log;
    const Entity lost = agent(*scene, Vector3(0.0f, 0.0f, 80.0f));
    steps(30);
    NavMeshAgent handle(scene.get(), lost);
    EXPECT_FALSE(handle.isOnNavMesh());
    EXPECT_EQ(log.count(D_LOG_WARN, "is not on"), 1u);
    handle.setDestination(Vector3(0.0f, 0.0f, 0.0f));
    steps(30);
    // no movement
    EXPECT_EQ(position(lost), Vector3(0.0f, 0.0f, 80.0f));
    EXPECT_EQ(log.count(D_LOG_WARN, "is not on"), 1u);
}

TEST_F(NavAgentTest, Drivers){
    // the same desired velocity for every driver on the first ticks (+-1e-4)
    const Entity surface = planeSurface(40.0f, 40.0f);
    bake(surface);
    auto run = [&](NavAgentDriver driver, const std::function<void(Entity)>& prepare) {
        const Entity walker = agent(*scene, Vector3(-10.0f, 0.0f, (float)scene->getComponentArray<NavMeshAgentComponent>()->size() * 3.0f - 12.0f));
        scene->getComponent<NavMeshAgentComponent>(walker).driver = driver;
        scene->getComponent<NavMeshAgentComponent>(walker).obstacleAvoidance = nav::AvoidanceQuality::None;
        if (prepare)
            prepare(walker);
        return walker;
    };
    std::vector<Entity> walkers;
    walkers.push_back(run(NavAgentDriver::TRANSFORM, nullptr));
    walkers.push_back(run(NavAgentDriver::NONE, nullptr));
#ifdef JASPERENGINE_PHYSICS_3D
    scene->getScheduler().setEnabled("PhysicsSystem", true);
    scene->getSystem<PhysicsSystem>()->setGravity3D(0.0f, 0.0f, 0.0f);
    walkers.push_back(run(NavAgentDriver::RIGIDBODY, [&](Entity entity) {
        RigidbodyComponent body;
        body.type = BodyType::KINEMATIC;
        body.colliders[0].type = Collider3DType::SPHERE;
        body.colliders[0].radius = 0.4f;
        body.numColliders = 1;
        scene->addComponent<RigidbodyComponent>(entity, body);
    }));
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    walkers.push_back(run(NavAgentDriver::RIGIDBODY, [&](Entity entity) {
        Rigidbody2DComponent body;
        body.type = BodyType::KINEMATIC;
        body.colliders[0].type = Collider2DType::CIRCLE;
        body.colliders[0].radius = 0.4f;
        body.numColliders = 1;
        scene->addComponent<Rigidbody2DComponent>(entity, body);
    }));
#endif
    step();
    // every agent walks the same 20 m along +x, avoidance off
    for (Entity walker : walkers) {
        const Vector3 start = position(walker);
        ASSERT_TRUE(NavMeshAgent(scene.get(), walker).setDestination(start + Vector3(20.0f, 0.0f, 0.0f)));
    }
    for (int tick = 0; tick < 30; tick++) {
        step();
        const Vector3 reference = NavMeshAgent(scene.get(), walkers[0]).getDesiredVelocity();
        for (size_t i = 1; i < walkers.size(); i++) {
            const Vector3 desired = NavMeshAgent(scene.get(), walkers[i]).getDesiredVelocity();
            EXPECT_NEAR(desired.x, reference.x, 1e-4f) << "driver " << i << " tick " << tick;
            EXPECT_NEAR(desired.y, reference.y, 1e-4f) << "driver " << i << " tick " << tick;
            EXPECT_NEAR(desired.z, reference.z, 1e-4f) << "driver " << i << " tick " << tick;
        }
    }
    // None never writes the Transform; its nextPosition moves
    NavMeshAgent none(scene.get(), walkers[1]);
    EXPECT_NEAR(position(walkers[1]).x, -10.0f, 1e-4f);
    EXPECT_GT(none.getNextPosition().x, -9.0f);
}

#ifdef JASPERENGINE_PHYSICS_3D
TEST_F(NavAgentTest, CharacterControllerDriver){
    // the CharacterController driver (S22) passes the desired velocity to simpleMove
    scene->getScheduler().setEnabled("PhysicsSystem", true);
    const Entity floor = transformEntity(*scene, Vector3(0.0f, -0.5f, 0.0f));
    RigidbodyComponent ground;
    ground.type = BodyType::STATIC;
    ground.colliders[0].type = Collider3DType::BOX;
    ground.colliders[0].width = 40.0f;
    ground.colliders[0].height = 1.0f;
    ground.colliders[0].depth = 40.0f;
    ground.numColliders = 1;
    scene->addComponent<RigidbodyComponent>(floor, ground);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32).useGeometry = NavUseGeometry::PHYSICS_COLLIDERS;
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-10.0f, 1.0f, 0.0f));
    NavMeshAgentComponent& component = scene->getComponent<NavMeshAgentComponent>(walker);
    component.driver = NavAgentDriver::CHARACTER_CONTROLLER;
    component.baseOffset = 1.0f;
    CharacterControllerComponent controller;
    controller.radius = 0.4f;
    controller.height = 2.0f;
    scene->addComponent<CharacterControllerComponent>(walker, controller);
    steps(5);
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.isOnNavMesh());
    ASSERT_TRUE(handle.setDestination(Vector3(10.0f, 0.0f, 0.0f)));
    int reached = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    for (int tick = 0; tick < 60 * 15 && reached == 0; tick++)
        step();
    EXPECT_EQ(reached, 1);
    EXPECT_NEAR(position(walker).x, 10.0f, 0.3f);
}
#endif

TEST_F(NavAgentTest, WarpAndMove){
    const Entity surface = planeSurface();
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-5.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.warp(Vector3(4.0f, 0.5f, 4.0f)));
    EXPECT_NEAR(position(walker).x, 4.0f, 1e-3f);
    EXPECT_NEAR(position(walker).z, 4.0f, 1e-3f);
    EXPECT_NEAR(position(walker).y, 0.0f, 0.2f);
    // Move is constrained to the mesh: a move out to 20 stops at the edge
    handle.move(Vector3(20.0f, 0.0f, 0.0f));
    EXPECT_NEAR(position(walker).x, 9.5f, 0.25f);
    // a warp off the mesh fails
    EXPECT_FALSE(handle.warp(Vector3(0.0f, 0.0f, 90.0f)));
    EXPECT_FALSE(handle.isOnNavMesh());
}

TEST_F(NavAgentTest, StoppedAgentHolds){
    const Entity surface = planeSurface(40.0f, 10.0f);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-15.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    handle.setDestination(Vector3(15.0f, 0.0f, 0.0f));
    steps(60);
    handle.setStopped(true);
    steps(120);
    const Vector3 held = position(walker);
    steps(30);
    EXPECT_LT(position(walker).distance(held), 1e-3f);
    handle.setStopped(false);
    steps(30);
    EXPECT_GT(position(walker).distance(held), 0.5f);
}

// two islands joined by a link: the path goes through it, an agent traverses it once (AC8)
TEST_F(NavAgentTest, LinkGapCrossing){
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    const Entity linkEntity = transformEntity(*scene, Vector3(0.0f, 0.0f, 0.0f), NULL_ENTITY, "Link");
    NavMeshLinkComponent link;
    link.startPoint = Vector3(-2.5f, 0.0f, 0.0f);
    link.endPoint = Vector3(2.5f, 0.0f, 0.0f);
    link.bidirectional = false;
    scene->addComponent<NavMeshLinkComponent>(linkEntity, link);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);

    const nav::NavPath path = nav->calculatePath(Vector3(-8.0f, 0.0f, 0.0f), Vector3(8.0f, 0.0f, 0.0f));
    EXPECT_EQ(path.status, nav::PathStatus::Complete);
    // one way only: back across is Partial
    EXPECT_EQ(nav->calculatePath(Vector3(8.0f, 0.0f, 0.0f), Vector3(-8.0f, 0.0f, 0.0f)).status, nav::PathStatus::Partial);

    const Entity walker = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    std::vector<std::string> events;
    handle.getOnOffMeshLinkStart() = std::function<void(NavMeshAgent)>([&](NavMeshAgent a) {
        events.push_back("start");
        EXPECT_TRUE(a.isOnOffMeshLink());
    });
    handle.getOnOffMeshLinkEnd() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { events.push_back("end"); });
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { events.push_back("arrived"); });
    ASSERT_TRUE(handle.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    bool sawLink = false;
    for (int tick = 0; tick < 60 * 20 && (events.empty() || events.back() != "arrived"); tick++) {
        step();
        if (handle.isOnOffMeshLink()) {
            sawLink = true;
            Vector3 start, end;
            Entity linkSeen = NULL_ENTITY;
            ASSERT_TRUE(handle.getCurrentOffMeshLink(start, end, linkSeen));
            EXPECT_EQ(linkSeen, linkEntity);
        }
    }
    EXPECT_TRUE(sawLink);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], "start");
    EXPECT_EQ(events[1], "end");
    EXPECT_EQ(events[2], "arrived");
    EXPECT_NEAR(position(walker).x, 8.0f, 0.1f);
}

TEST_F(NavAgentTest, LinkWithoutAutoTraverseWaits){
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    const Entity linkEntity = transformEntity(*scene, Vector3(), NULL_ENTITY, "Link");
    NavMeshLinkComponent link;
    link.startPoint = Vector3(-2.5f, 0.0f, 0.0f);
    link.endPoint = Vector3(2.5f, 0.0f, 0.0f);
    scene->addComponent<NavMeshLinkComponent>(linkEntity, link);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f));
    scene->getComponent<NavMeshAgentComponent>(walker).autoTraverseOffMeshLink = false;
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    for (int tick = 0; tick < 600 && !handle.isOnOffMeshLink(); tick++)
        step();
    ASSERT_TRUE(handle.isOnOffMeshLink());
    const Vector3 waiting = position(walker);
    steps(60);
    // held at the link until the script completes it
    EXPECT_LT(position(walker).distance(waiting), 1e-3f);
    handle.completeOffMeshLink();
    EXPECT_FALSE(handle.isOnOffMeshLink());
    steps(2);
    EXPECT_GT(position(walker).x, 2.0f);
}

TEST_F(NavAgentTest, PathFailedFires){
    // a destination on another island: the path is Partial, onPathFailed fires once
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    int failed = 0;
    handle.getOnPathFailed() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { failed++; });
    handle.setDestination(Vector3(8.0f, 0.0f, 0.0f));
    steps(60);
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Partial);
}
