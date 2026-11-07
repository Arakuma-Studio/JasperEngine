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
#include <functional>
#include <string>
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
        body.colliders.resize(1);
        body.colliders[0].type = Collider3DType::SPHERE;
        body.colliders[0].radius = 0.4f;
        scene->addComponent<RigidbodyComponent>(entity, body);
    }));
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    walkers.push_back(run(NavAgentDriver::RIGIDBODY, [&](Entity entity) {
        Rigidbody2DComponent body;
        body.type = BodyType::KINEMATIC;
        body.colliders.resize(1);
        body.colliders[0].type = Collider2DType::CIRCLE;
        body.colliders[0].radius = 0.4f;
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
    ground.colliders.resize(1);
    ground.colliders[0].type = Collider3DType::BOX;
    ground.colliders[0].width = 40.0f;
    ground.colliders[0].height = 1.0f;
    ground.colliders[0].depth = 40.0f;
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

// ---------------------------------------------------------------- S53 (docs/specs/S53-navigation-fixes.md)

// 20 agents walking through a gap a carving box then closes: every path status comes from the
// crowd's corridor, no path search runs for them (getPathQueryCount stays 0 across arrivals and
// replans), and each matches calculatePath afterwards (AC9, R6)
TEST_F(NavAgentTest, PathStatusWithoutSearch){
    const Entity surface = gapSurface();
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    std::vector<Entity> agents;
    std::vector<Vector3> destinations;
    for (int i = 0; i < 20; i++) {
        const float z = -4.0f + 8.0f * (float)(i % 5) / 4.0f;
        agents.push_back(agent(*scene, Vector3(-13.0f + 1.5f * (float)(i / 5), 0.0f, z)));
        scene->getComponent<NavMeshAgentComponent>(agents.back()).stoppingDistance = 0.2f;
        // the first half stays on its side (a destination each, away from the gap), the rest
        // crosses the gap
        destinations.push_back(i < 10 ? Vector3(i < 5 ? -5.0f : -7.5f, 0.0f, -z) : Vector3(12.0f, 0.0f, -z));
    }
    step();
    const uint64_t queries = nav->getPathQueryCount();
    int reached = 0, failed = 0;
    for (size_t i = 0; i < agents.size(); i++) {
        NavMeshAgent handle(scene.get(), agents[i]);
        handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
        handle.getOnPathFailed() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { failed++; });
        ASSERT_TRUE(handle.setDestination(destinations[i]));
    }
    steps(90);
    // the gap closes: corridors through it become invalid and are planned again
    obstacleBox(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(1.5f, 2.0f, 3.0f), true);
    steps(60 * 12);
    EXPECT_EQ(nav->getPathQueryCount(), queries);
    EXPECT_GT(reached, 0);
    EXPECT_GT(failed, 0);
    for (size_t i = 0; i < agents.size(); i++) {
        NavMeshAgent handle(scene.get(), agents[i]);
        const nav::NavPath path = nav->calculatePath(position(agents[i]), destinations[i]);
        EXPECT_EQ(handle.getPathStatus(), path.status) << "agent " << i;
    }
}

// a destination on another island: the agent stops at the near edge, its path stays Partial,
// onPathFailed fires once and onDestinationReached never (AC10, R7)
TEST_F(NavAgentTest, PartialPathNoArrival){
    const Entity surface = islandsSurface();
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    int failed = 0, reached = 0;
    handle.getOnPathFailed() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { failed++; });
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    handle.setDestination(Vector3(8.0f, 0.0f, 0.0f));
    steps(900);
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(reached, 0);
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Partial);
    // at the near island's edge (x = -2 less the agent's radius), and still
    EXPECT_GT(position(walker).x, -3.0f);
    EXPECT_LT(position(walker).x, -2.0f);
    EXPECT_LT(handle.getVelocity().length(), 0.05f);
}

// Auto Repath off on the strip: dtCrowd's extensions of the truncated corridor are not repaths,
// the agent arrives and onPathFailed never fires (AC11, R8)
TEST_F(NavAgentTest, ExtensionKeepsPathWithoutAutoRepath){
    const Entity surface = stripSurface();
    bake(surface);
    const Vector3 destination(295.0f, 0.0f, 0.0f);
    const Entity runner = agent(*scene, Vector3(-295.0f, 0.0f, 0.0f), 20.0f);
    scene->getComponent<NavMeshAgentComponent>(runner).autoRepath = false;
    step();
    NavMeshAgent handle(scene.get(), runner);
    int reached = 0, failed = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    handle.getOnPathFailed() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { failed++; });
    ASSERT_TRUE(handle.setDestination(destination));
    for (int tick = 0; tick < 60 * 60 && reached == 0 && failed == 0; tick++)
        step();
    EXPECT_EQ(failed, 0);
    EXPECT_EQ(reached, 1);
    EXPECT_TRUE(handle.hasPath());
    EXPECT_LT(Vector3(position(runner).x - destination.x, 0.0f, position(runner).z - destination.z).length(), 0.1f);
}

namespace {

    // the link between the islands of islandsSurface, from x = -2.5 to 2.5 at z = 0
    Entity islandLink(Scene& scene, bool autoUpdate) {
        const Entity entity = transformEntity(scene, Vector3(), NULL_ENTITY, "Link");
        NavMeshLinkComponent link;
        link.startPoint = Vector3(-2.5f, 0.0f, 0.0f);
        link.endPoint = Vector3(2.5f, 0.0f, 0.0f);
        link.autoUpdatePosition = autoUpdate;
        scene.addComponent<NavMeshLinkComponent>(entity, link);
        return entity;
    }

    // the corner of the path nearest `point` (horizontally)
    float nearestCorner(const nav::NavPath& path, const Vector3& point) {
        float best = 1e9f;
        for (const Vector3& corner : path.corners)
            best = std::min(best, Vector3(corner.x - point.x, 0.0f, corner.z - point.z).length());
        return best;
    }

    void moveTo(Scene& scene, Entity entity, const Vector3& position) {
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.position = position;
        transform.needUpdate = true;
    }

}

// Auto Update Positions on a static surface: the link moves 2 m within its tile and the path's link
// corner follows within one tick (AC13, R10)
TEST_F(NavAgentTest, LinkAutoUpdateStatic){
    const Entity surface = islandsSurface();
    const Entity link = islandLink(*scene, true);
    bake(surface);
    const Vector3 from(-8.0f, 0.0f, 0.0f), to(8.0f, 0.0f, 0.0f);
    step();
    ASSERT_LT(nearestCorner(nav->calculatePath(from, to), Vector3(-2.5f, 0.0f, 0.0f)), 0.3f);
    // tiles of 6.4 m: the start (-2.5, 2) stays in tile (-1, 0)
    moveTo(*scene, link, Vector3(0.0f, 0.0f, 2.0f));
    step();
    const nav::NavPath path = nav->calculatePath(from, to);
    EXPECT_EQ(path.status, nav::PathStatus::Complete);
    EXPECT_LT(nearestCorner(path, Vector3(-2.5f, 0.0f, 2.0f)), 0.3f);
    EXPECT_GT(nearestCorner(path, Vector3(-2.5f, 0.0f, 0.0f)), 1.0f);
}

// Auto Update Positions on a carving surface: the start moves into the next tile, and the cut of a
// carving obstacle in the rebuilt tile stays (AC13, R10)
TEST_F(NavAgentTest, LinkAutoUpdateCarving){
    const Entity surface = islandsSurface(true);
    const Entity link = islandLink(*scene, true);
    bake(surface);
    nav->setCarveTimeCeiling(0.0);
    // a cut in tile (-1, -1), the tile the start moves into
    obstacleBox(*scene, Vector3(-5.0f, 0.5f, -1.5f), Vector3(1.0f, 1.0f, 1.0f), true);
    steps(3);
    nav::NavHit hit;
    ASSERT_FALSE(nav->samplePosition(Vector3(-5.0f, 0.0f, -1.5f), 0.15f, hit) && hit.position.distance(Vector3(-5.0f, 0.0f, -1.5f)) < 0.15f);
    const Vector3 from(-8.0f, 0.0f, 0.0f), to(8.0f, 0.0f, 0.0f);
    moveTo(*scene, link, Vector3(0.0f, 0.0f, -2.0f));
    steps(3);
    const nav::NavPath path = nav->calculatePath(from, to);
    EXPECT_EQ(path.status, nav::PathStatus::Complete);
    EXPECT_LT(nearestCorner(path, Vector3(-2.5f, 0.0f, -2.0f)), 0.3f);
    EXPECT_GT(nearestCorner(path, Vector3(-2.5f, 0.0f, 0.0f)), 1.0f);
    EXPECT_FALSE(nav->samplePosition(Vector3(-5.0f, 0.0f, -1.5f), 0.15f, hit) && hit.position.distance(Vector3(-5.0f, 0.0f, -1.5f)) < 0.15f);
    // and it is crossed there
    const Entity walker = agent(*scene, from);
    step();
    NavMeshAgent handle(scene.get(), walker);
    int reached = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    ASSERT_TRUE(handle.setDestination(to));
    for (int tick = 0; tick < 60 * 20 && reached == 0; tick++)
        step();
    EXPECT_EQ(reached, 1);
}

// Auto Update off: a moved link stays where it was baked. On a static surface a start leaving its
// baked tile keeps the last connections and warns once, as does a cost the bake did not have
// (AC13, R10)
TEST_F(NavAgentTest, LinkAutoUpdateLimits){
    const Entity surface = islandsSurface();
    const Entity fixed = islandLink(*scene, false);
    bake(surface);
    const Vector3 from(-8.0f, 0.0f, 0.0f), to(8.0f, 0.0f, 0.0f);
    moveTo(*scene, fixed, Vector3(0.0f, 0.0f, 2.0f));
    steps(3);
    EXPECT_LT(nearestCorner(nav->calculatePath(from, to), Vector3(-2.5f, 0.0f, 0.0f)), 0.3f);

    // auto-updating, its start pushed into tile (-1, -1)
    scene->getComponent<NavMeshLinkComponent>(fixed).autoUpdatePosition = true;
    test::LogCapture log;
    moveTo(*scene, fixed, Vector3(0.0f, 0.0f, -2.0f));
    steps(10);
    EXPECT_EQ(log.count(D_LOG_WARN, "left its baked tile on NavMeshSurface 'Surface': turn on Runtime Carving or rebake"), 1u);
    EXPECT_LT(nearestCorner(nav->calculatePath(from, to), Vector3(-2.5f, 0.0f, 0.0f)), 0.3f);

    // back within its tile, with a cost the bake did not see
    scene->getComponent<NavMeshLinkComponent>(fixed).costModifier = 7.0f;
    moveTo(*scene, fixed, Vector3(0.0f, 0.0f, 1.0f));
    steps(10);
    EXPECT_LT(nearestCorner(nav->calculatePath(from, to), Vector3(-2.5f, 0.0f, 1.0f)), 0.3f);
    EXPECT_EQ(log.count(D_LOG_WARN, "a cost NavMeshSurface 'Surface' was not baked with"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "left its baked tile"), 1u);
}
