// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The NavMeshAgent gameplay API of docs/specs/S55-navigation-gameplay-api.md (R19-R29; AC19-AC25):
// scripted paths, the corridor as a path and its staleness, root motion, the agent's queries, the
// stuck watchdog and why a path failed.

#include "NavTestUtil.h"

#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "object/navigation/NavMeshAgent.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    // one cell of the 0.2 m bakes
    constexpr float CELL = 0.2f;
    constexpr uint8_t MUD = 3;

    class NavAgentApiTest : public NavSceneTest {
    protected:
        // area 3 is "Mud" at `cost` per metre
        void mudArea(float cost) {
            nav::NavigationSettings settings = nav::NavigationSettings::defaults();
            settings.areas[MUD].name = "Mud";
            settings.areas[MUD].cost = cost;
            nav::setSettings(settings);
        }

        // steps until `done` or `limit` ticks; the ticks taken
        int stepUntil(const std::function<bool()>& done, int limit) {
            int ticks = 0;
            while (ticks < limit && !done()) {
                step();
                ticks++;
            }
            return ticks;
        }
    };

    // the 2D (XZ) distance from a point to a polyline
    float distanceToPolyline(const Vector3& point, const std::vector<Vector3>& corners) {
        float best = 1e9f;
        for (size_t i = 0; i + 1 < corners.size(); i++) {
            const Vector3 a(corners[i].x, 0.0f, corners[i].z), b(corners[i + 1].x, 0.0f, corners[i + 1].z), p(point.x, 0.0f, point.z);
            const Vector3 ab = b - a;
            const float length2 = ab.dotProduct(ab);
            const float t = length2 > 0.0f ? std::clamp((p - a).dotProduct(ab) / length2, 0.0f, 1.0f) : 0.0f;
            best = std::min(best, p.distance(a + ab * t));
        }
        return best;
    }

    Entity obstacle(Scene& scene, const Vector3& center, const Vector3& size, bool carve) {
        const Entity entity = transformEntity(scene, center);
        NavMeshObstacleComponent component;
        component.shape = NavObstacleShape::BOX;
        component.size = size;
        component.carve = carve;
        component.carveOnlyStationary = false;
        scene.addComponent<NavMeshObstacleComponent>(entity, component);
        return entity;
    }

}

// AC19: a path planned through the mud (cost 1 for the query) that the agent itself would walk
// around (its cost 10) is followed as given and arrives; a path that does not start at the agent
// is refused and leaves it without a path
TEST_F(NavAgentApiTest, CalculateAndSetPath){
    mudArea(10.0f);
    const Entity surface = planeSurface(30.0f, 12.0f);
    areaVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(4.0f, 4.0f, 4.0f), MUD);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-12.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.isOnNavMesh());
    const Vector3 target(12.0f, 0.0f, 0.0f);

    // its own plan goes around the mud, and asking for it does not give it a path
    const nav::NavPath own = handle.calculatePath(target);
    ASSERT_EQ(own.status, nav::PathStatus::Complete);
    EXPECT_EQ(own.surface, surface);
    float detour = 0.0f;
    for (const Vector3& corner : own.corners)
        detour = std::max(detour, std::fabs(corner.z));
    EXPECT_GT(detour, 1.5f);
    EXPECT_FALSE(handle.hasPath());

    nav::QueryFilter cheapMud;
    cheapMud.setAreaCost(MUD, 1.0f);
    const nav::NavPath straight = nav->calculatePathOn(surface, position(walker), target, cheapMud);
    ASSERT_EQ(straight.status, nav::PathStatus::Complete);
    ASSERT_FALSE(straight.polys.empty());
    ASSERT_GT(straight.areaLengths[MUD], 3.0f);

    // a path from elsewhere is refused
    const nav::NavPath elsewhere = nav->calculatePathOn(surface, Vector3(10.0f, 0.0f, 4.0f), target, cheapMud);
    ASSERT_EQ(elsewhere.status, nav::PathStatus::Complete);
    ASSERT_TRUE(handle.setDestination(target));
    steps(2);
    ASSERT_TRUE(handle.hasPath());
    EXPECT_FALSE(handle.setPath(elsewhere));
    EXPECT_FALSE(handle.hasPath());

    ASSERT_TRUE(handle.setPath(straight));
    EXPECT_FALSE(handle.isPathPending());
    EXPECT_TRUE(handle.hasPath());
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Complete);
    int reached = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    float worst = 0.0f;
    for (int tick = 0; tick < 60 * 15 && reached == 0; tick++) {
        step();
        worst = std::max(worst, distanceToPolyline(position(walker), straight.corners));
    }
    EXPECT_EQ(reached, 1);
    EXPECT_LE(worst, 0.1f);
    EXPECT_NEAR(position(walker).x, target.x, 0.05f);
}

// AC20: a path of more than 256 polygons (the corridor's capacity) is accepted, followed to its end
// (the crowd plans the rest near the corridor's end) and arrives once
TEST_F(NavAgentApiTest, SetPathLongerThan256Polys){
    // 700 m x 3 m on tiles of 16 cells (1.6 m): more than 400 polygons end to end, and few enough
    // for one search (2048 nodes)
    plane(*scene, Vector3(), 700.0f, 3.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.1f, 16);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-345.0f, 0.0f, 0.0f), 20.0f, 0.4f);
    scene->getComponent<NavMeshAgentComponent>(walker).acceleration = 40.0f;
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.isOnNavMesh());
    const Vector3 target(345.0f, 0.0f, 0.0f);
    const nav::NavPath path = handle.calculatePath(target);
    ASSERT_EQ(path.status, nav::PathStatus::Complete);
    ASSERT_GE(path.polys.size(), 400u);
    ASSERT_TRUE(handle.setPath(path));
    EXPECT_FALSE(handle.isPathPending());
    int reached = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    stepUntil([&]() { return reached > 0; }, 60 * 60);
    steps(30);
    EXPECT_EQ(reached, 1);
    EXPECT_NEAR(position(walker).x, target.x, 0.1f);
}

// AC21: agent.path is the corridor from where it stands (its corners after the first are
// getPathCorners'), steeringTarget the first corner ahead; an area on the corridor far ahead
// closed to it makes the path stale until the replan lands
TEST_F(NavAgentApiTest, CorridorPathAndStale){
    mudArea(1.0f);
    plane(*scene, Vector3(), 40.0f, 6.0f);
    box(*scene, Vector3(-4.0f, 1.0f, 1.5f), Vector3(1.0f, 2.0f, 3.0f));
    // mud across the corridor near its end
    areaVolume(Vector3(14.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 8.0f), MUD);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, CELL, 8);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-18.0f, 0.0f, 1.5f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(Vector3(18.0f, 0.0f, 1.5f)));
    stepUntil([&]() { return !handle.isPathPending(); }, 60);
    steps(3);
    ASSERT_TRUE(handle.hasPath());

    const nav::NavPath corridor = handle.getPath();
    const std::vector<Vector3> ahead = handle.getPathCorners();
    ASSERT_EQ(corridor.status, nav::PathStatus::Complete);
    ASSERT_FALSE(corridor.polys.empty());
    ASSERT_EQ(corridor.corners.size(), ahead.size() + 1);
    EXPECT_LT(corridor.corners[0].distance(handle.getNextPosition()), 1e-3f);
    for (size_t i = 0; i < ahead.size(); i++)
        EXPECT_LT(corridor.corners[i + 1].distance(ahead[i]), 1e-4f) << "corner " << i;
    // around the box: the first corner ahead is its corner, the one it steers to
    ASSERT_GE(ahead.size(), 2u);
    EXPECT_LT(handle.getSteeringTarget().distance(ahead[0]), 1e-4f);
    EXPECT_FALSE(handle.isPathStale());

    // the mud closed to it, far ahead: past dtCrowd's 10-polygon look-ahead (and no mesh change, so
    // no replan of the whole corridor), only the whole corridor's check sees it
    scene->getComponent<NavMeshAgentComponent>(walker).areaMask = nav::ALL_AREAS & ~(1 << MUD);
    step();
    EXPECT_TRUE(handle.isPathStale());
    // the crowd replans once the mud is within its look-ahead: no longer stale, short of the mud
    stepUntil([&]() { return handle.getPathStatus() == nav::PathStatus::Partial && !handle.isPathPending(); }, 60 * 20);
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Partial);
    EXPECT_FALSE(handle.isPathStale());
    steps(60 * 3);
    EXPECT_LT(position(walker).x, 13.0f);
}

// AC22: root motion. With updatePosition off, nextPosition moves the simulation along the surface
// (a line into the edge stops there) and the Transform never moves; velocity = v moves v dt for
// one tick and the path goes on; without a path the crowd steers it at v that tick
TEST_F(NavAgentApiTest, RootMotion){
    const Entity surface = planeSurface(20.0f, 20.0f);
    bake(surface);
    const Entity walker = agent(*scene, Vector3(-5.0f, 0.0f, 0.0f));
    NavMeshAgentComponent& component = scene->getComponent<NavMeshAgentComponent>(walker);
    component.updatePosition = false;
    step();
    NavMeshAgent handle(scene.get(), walker);
    const Vector3 transformStart = position(walker);
    Vector3 next = handle.getNextPosition();
    for (int tick = 0; tick < 120; tick++) {
        handle.setNextPosition(next + Vector3(0.2f, 0.0f, 0.0f));
        next = handle.getNextPosition();
        step();
        EXPECT_EQ(position(walker), transformStart) << "tick " << tick;
    }
    // the plane's edge, less the agent's radius
    EXPECT_NEAR(handle.getNextPosition().x, 9.5f, CELL + 0.05f);
    EXPECT_NEAR(handle.getNextPosition().z, 0.0f, 1e-3f);

    // a velocity for one tick on a path
    component.updatePosition = true;
    ASSERT_TRUE(handle.warp(Vector3(-5.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(handle.setDestination(Vector3(5.0f, 0.0f, 0.0f)));
    steps(30);
    ASSERT_TRUE(handle.hasPath());
    const Vector3 before = handle.getNextPosition();
    const Vector3 sideways(0.0f, 0.0f, 2.0f);
    handle.setVelocity(sideways);
    step();
    const Vector3 moved = handle.getNextPosition() - before;
    EXPECT_NEAR(moved.x, 0.0f, 1e-3f);
    EXPECT_NEAR(moved.z, sideways.z * DT, 1e-3f);
    EXPECT_LT(handle.getVelocity().distance(sideways), 1e-4f);
    // the path goes on: the crowd steers it from that velocity back to the destination
    steps(30);
    EXPECT_GT(handle.getVelocity().x, 1.0f);
    EXPECT_TRUE(handle.hasPath());

    // no path: the crowd steers it at the velocity for that tick (an acceleration that reaches it)
    handle.resetPath();
    component.acceleration = 1000.0f;
    component.obstacleAvoidance = nav::AvoidanceQuality::None;
    steps(10);
    const Vector3 still = handle.getNextPosition();
    const Vector3 push(1.5f, 0.0f, 0.0f);
    handle.setVelocity(push);
    step();
    EXPECT_NEAR(handle.getNextPosition().x - still.x, push.x * DT, 2e-3f);
    EXPECT_FALSE(handle.hasPath());
}

// AC23: samplePathPosition stops where the path enters an area outside the mask; raycast and
// findClosestEdge answer as the system's queries with the agent's filter; nextOffMeshLinkData
// names the link ahead
TEST_F(NavAgentApiTest, AgentQueries){
    mudArea(1.0f);
    plane(*scene, Vector3(-4.0f, 0.0f, 0.0f), 30.0f, 10.0f);
    // the mud across the plane at x in [-1, 1]
    areaVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 12.0f), MUD);
    // an island past a gap, joined by a one-way link
    plane(*scene, Vector3(20.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    const Entity bridge = transformEntity(*scene, Vector3(), NULL_ENTITY, "Bridge");
    NavMeshLinkComponent link;
    link.startPoint = Vector3(10.0f, 0.0f, 0.0f);
    link.endPoint = Vector3(16.5f, 0.0f, 0.0f);
    link.bidirectional = false;
    scene->addComponent<NavMeshLinkComponent>(bridge, link);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, CELL, 32);
    bake(surface);

    const Entity walker = agent(*scene, Vector3(-16.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    stepUntil([&]() { return !handle.isPathPending(); }, 60);
    ASSERT_TRUE(handle.hasPath());

    nav::NavHit hit;
    EXPECT_TRUE(handle.samplePathPosition((int)nav::AreaMask::Walkable, 20.0f, hit));
    EXPECT_NEAR(hit.position.x, -1.0f, CELL + 0.05f);
    EXPECT_EQ(hit.area, MUD);
    EXPECT_NEAR(hit.distance, hit.position.x - handle.getNextPosition().x, 0.05f);
    const int walkableAndMud = (int)nav::AreaMask::Walkable | (1 << MUD);
    EXPECT_FALSE(handle.samplePathPosition(walkableAndMud, 20.0f, hit));
    EXPECT_NEAR(hit.distance, 20.0f, 1e-3f);
    EXPECT_NEAR(hit.position.x, handle.getNextPosition().x + 20.0f, 0.05f);

    // the agent's queries are the system's with its filter
    const nav::QueryFilter filter = nav->getAgentQueryFilter(walker);
    const Vector3 from = handle.getNextPosition();
    nav::NavHit agentHit, systemHit;
    const bool agentBlocked = handle.raycast(Vector3(-10.0f, 0.0f, 9.0f), agentHit);
    const bool systemBlocked = nav->raycastOn(surface, from, Vector3(-10.0f, 0.0f, 9.0f), systemHit, filter);
    EXPECT_TRUE(agentBlocked);
    EXPECT_EQ(agentBlocked, systemBlocked);
    EXPECT_LT(agentHit.position.distance(systemHit.position), 1e-5f);
    EXPECT_FLOAT_EQ(agentHit.distance, systemHit.distance);
    ASSERT_TRUE(handle.findClosestEdge(agentHit));
    ASSERT_TRUE(nav->findClosestEdgeOn(surface, from, systemHit, filter));
    EXPECT_LT(agentHit.position.distance(systemHit.position), 1e-5f);
    EXPECT_FLOAT_EQ(agentHit.distance, systemHit.distance);

    // no link ahead yet; across the bridge it is the next one
    Vector3 start, end;
    Entity next = NULL_ENTITY;
    EXPECT_FALSE(handle.getNextOffMeshLink(start, end, next));
    ASSERT_TRUE(handle.setDestination(Vector3(21.0f, 0.0f, 0.0f)));
    stepUntil([&]() { return !handle.isPathPending(); }, 60);
    ASSERT_TRUE(handle.getNextOffMeshLink(start, end, next));
    EXPECT_EQ(next, bridge);
    EXPECT_NEAR(start.x, 10.0f, 0.3f);
    EXPECT_NEAR(end.x, 16.5f, 0.3f);
}

// AC24: an agent held by a non-carving obstacle in a corridor one agent wide fires onStuck once
// stuckTimeout passed without progress, asks again, and fires again a timeout later; one jittering
// in place is stuck too; without detection nothing fires and stuckTime stays 0
TEST_F(NavAgentApiTest, StuckFiresAndRepaths){
    plane(*scene, Vector3(), 20.0f, 1.4f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.1f, 32);
    bake(surface);
    obstacle(*scene, Vector3(0.0f, 0.5f, 0.0f), Vector3(0.8f, 1.0f, 0.8f), false);
    const Entity walker = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f), 3.5f, 0.3f);
    NavMeshAgentComponent& component = scene->getComponent<NavMeshAgentComponent>(walker);
    component.stuckDetection = true;
    step();
    NavMeshAgent handle(scene.get(), walker);
    std::vector<int> stuckTicks;
    int tick = 0;
    int requests = 0;
    handle.getOnStuck() = std::function<void(NavMeshAgent)>([&](NavMeshAgent a) {
        stuckTicks.push_back(tick);
        // it asked for its destination again (repathWhenStuck)
        requests += a.isPathPending() ? 1 : 0;
        EXPECT_EQ(a.getStuckTime(), 0.0f);
    });
    ASSERT_TRUE(handle.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    // the last tick its stuckTime restarted before each fire
    int lastRestart = 0;
    float previous = 0.0f;
    std::vector<int> restarts;
    for (tick = 1; tick <= 60 * 8 && stuckTicks.size() < 2; tick++) {
        const size_t fired = stuckTicks.size();
        step();
        if (stuckTicks.size() > fired)
            restarts.push_back(lastRestart);
        const float now = handle.getStuckTime();
        if (now == 0.0f || now < previous)
            lastRestart = tick;
        previous = now;
    }
    ASSERT_EQ(stuckTicks.size(), 2u);
    EXPECT_EQ(requests, 2);
    EXPECT_LT(position(walker).x, 0.0f);
    for (size_t i = 0; i < stuckTicks.size(); i++)
        EXPECT_NEAR(stuckTicks[i] - restarts[i], 120, 1) << "fire " << i;
    // the second a timeout (and the repath's tick) after the first
    EXPECT_NEAR(stuckTicks[1] - stuckTicks[0], 121, 1);

    // jitter: back and forth across its path makes no progress
    handle.resetPath();
    ASSERT_TRUE(handle.warp(Vector3(-8.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(handle.setDestination(Vector3(-2.0f, 0.0f, 0.0f)));
    stepUntil([&]() { return !handle.isPathPending(); }, 60);
    stuckTicks.clear();
    for (tick = 0; tick < 60 * 3 && stuckTicks.empty(); tick++) {
        handle.setVelocity(Vector3(tick % 2 ? 1.0f : -1.0f, 0.0f, 0.0f));
        step();
    }
    EXPECT_EQ(stuckTicks.size(), 1u);

    // off: never, and stuckTime stays 0
    component.stuckDetection = false;
    stuckTicks.clear();
    for (tick = 0; tick < 60 * 3; tick++) {
        handle.setVelocity(Vector3(tick % 2 ? 1.0f : -1.0f, 0.0f, 0.0f));
        step();
        EXPECT_EQ(handle.getStuckTime(), 0.0f);
    }
    EXPECT_TRUE(stuckTicks.empty());
}

// AC25: why a destination fails: no surface, a surface never baked, a destination on another island,
// a door closed or a bridge deactivated on the way without Auto Repath (P2's replan drop);
// lastPathFailure goes back to None after a path that succeeds
TEST_F(NavAgentApiTest, PathFailedReason){
    std::vector<nav::PathFailure> reasons;
    const auto listen = [&](Entity entity) {
        NavMeshAgent(scene.get(), entity).getOnPathFailed() =
            std::function<void(NavMeshAgent, nav::PathFailure)>([&](NavMeshAgent a, nav::PathFailure reason) {
                reasons.push_back(reason);
                EXPECT_EQ(a.getLastPathFailure(), reason);
            });
    };

    // no surface at all
    const Entity lost = agent(*scene, Vector3(0.0f, 0.0f, 0.0f));
    listen(lost);
    step();
    ASSERT_TRUE(NavMeshAgent(scene.get(), lost).setDestination(Vector3(5.0f, 0.0f, 0.0f)));
    steps(3);
    ASSERT_EQ(reasons.size(), 1u);
    EXPECT_EQ(reasons[0], nav::PathFailure::NoSurface);
    EXPECT_EQ(NavMeshAgent(scene.get(), lost).getLastPathFailure(), nav::PathFailure::NoSurface);
    EXPECT_FALSE(NavMeshAgent(scene.get(), lost).isPathPending());
    scene->destroyEntity(lost);

    // a surface that was never baked
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, CELL, 32);
    const Entity walker = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f));
    listen(walker);
    step();
    NavMeshAgent handle(scene.get(), walker);
    reasons.clear();
    ASSERT_TRUE(handle.setDestination(Vector3(-4.0f, 0.0f, 0.0f)));
    steps(3);
    ASSERT_EQ(reasons.size(), 1u);
    EXPECT_EQ(reasons[0], nav::PathFailure::NotBaked);

    // baked: the other island is out of reach
    bake(surface);
    reasons.clear();
    ASSERT_TRUE(handle.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    steps(60);
    ASSERT_EQ(reasons.size(), 1u);
    EXPECT_EQ(reasons[0], nav::PathFailure::Disconnected);
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Partial);

    // a destination it reaches clears the reason
    reasons.clear();
    ASSERT_TRUE(handle.setDestination(Vector3(-4.0f, 0.0f, 1.0f)));
    stepUntil([&]() { return !handle.isPathPending(); }, 60);
    EXPECT_TRUE(reasons.empty());
    EXPECT_EQ(handle.getLastPathFailure(), nav::PathFailure::None);
    // and resetPath does too
    ASSERT_TRUE(handle.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    steps(10);
    EXPECT_EQ(handle.getLastPathFailure(), nav::PathFailure::Disconnected);
    handle.resetPath();
    EXPECT_EQ(handle.getLastPathFailure(), nav::PathFailure::None);

    // a door closed on its way (a Not Walkable script volume) with Auto Repath off: the path is
    // dropped, blocked by an area
    scene = std::make_unique<Scene>();
    test::disableSubSystems(*scene);
    scene->getScheduler().setEnabled("NavigationSystem", true);
    nav = scene->getSystem<NavigationSystem>().get();
    plane(*scene, Vector3(), 20.0f, 8.0f);
    box(*scene, Vector3(0.0f, 1.0f, -2.75f), Vector3(1.0f, 2.0f, 2.5f));
    box(*scene, Vector3(0.0f, 1.0f, 2.75f), Vector3(1.0f, 2.0f, 2.5f));
    const Entity doorSurface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, doorSurface, CELL, 32);
    bake(doorSurface);
    const Entity runner = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f));
    scene->getComponent<NavMeshAgentComponent>(runner).autoRepath = false;
    reasons.clear();
    listen(runner);
    step();
    NavMeshAgent door(scene.get(), runner);
    ASSERT_TRUE(door.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    stepUntil([&]() { return !door.isPathPending(); }, 60);
    ASSERT_EQ(door.getPathStatus(), nav::PathStatus::Complete);
    steps(10);
    EXPECT_NE(nav->setAreaInVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.2f, 4.0f, 3.5f), Quaternion(), nav::AREA_NOT_WALKABLE), 0u);
    stepUntil([&]() { return !reasons.empty(); }, 60);
    ASSERT_EQ(reasons.size(), 1u);
    EXPECT_EQ(reasons[0], nav::PathFailure::BlockedByArea);
    EXPECT_FALSE(door.hasPath());
    EXPECT_EQ(door.getLastPathFailure(), nav::PathFailure::BlockedByArea);

    // a bridge (the only way across) deactivated under it, Auto Repath off: blocked by an area too
    scene = std::make_unique<Scene>();
    test::disableSubSystems(*scene);
    scene->getScheduler().setEnabled("NavigationSystem", true);
    nav = scene->getSystem<NavigationSystem>().get();
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    const Entity bridge = transformEntity(*scene, Vector3(), NULL_ENTITY, "Bridge");
    NavMeshLinkComponent link;
    link.startPoint = Vector3(-2.5f, 0.0f, 0.0f);
    link.endPoint = Vector3(2.5f, 0.0f, 0.0f);
    scene->addComponent<NavMeshLinkComponent>(bridge, link);
    const Entity bridgeSurface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, bridgeSurface, CELL, 32);
    bake(bridgeSurface);
    const Entity crosser = agent(*scene, Vector3(-8.0f, 0.0f, 0.0f));
    scene->getComponent<NavMeshAgentComponent>(crosser).autoRepath = false;
    reasons.clear();
    listen(crosser);
    step();
    NavMeshAgent across(scene.get(), crosser);
    ASSERT_TRUE(across.setDestination(Vector3(8.0f, 0.0f, 0.0f)));
    stepUntil([&]() { return !across.isPathPending(); }, 60);
    ASSERT_EQ(across.getPathStatus(), nav::PathStatus::Complete);
    scene->getComponent<NavMeshLinkComponent>(bridge).activated = false;
    stepUntil([&]() { return !reasons.empty(); }, 60);
    ASSERT_EQ(reasons.size(), 1u);
    EXPECT_EQ(reasons[0], nav::PathFailure::BlockedByArea);
    EXPECT_FALSE(across.hasPath());
}
