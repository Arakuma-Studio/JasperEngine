// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The gameplay queries of docs/specs/S76-navigation-gameplay-api.md (R1-R8; AC1-AC4, AC7-AC9):
// seeded random points (anywhere uniform by area, around a point, on an island), the area costs and
// corner areas of paths, the raycast's cost, and per-query area costs.

#include "NavTestUtil.h"

#include "LogCapture.h"
#include "component/NavMeshLinkComponent.h"
#include "navigation/NavRandom.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    // one cell of the 0.2 m bakes
    constexpr float CELL = 0.2f;
    // chi-square with one degree of freedom at p = 0.001
    constexpr double CHI2_1DOF_P001 = 10.828;

    class NavRandomTest : public NavSceneTest {
    protected:
        // the 2D area of the navmesh's triangles on each side of x = split (world)
        void areasAround(Entity surface, float split, double& left, double& right) {
            nav::NavDebugGeometry geometry;
            ASSERT_TRUE(nav->getDebugGeometry(surface, nullptr, 1u << 20, false, geometry));
            left = right = 0.0;
            for (size_t i = 0; i + 8 < geometry.triangles.size(); i += 9) {
                const float* t = &geometry.triangles[i];
                const double area = 0.5 * std::fabs((double)(t[3] - t[0]) * (t[8] - t[2]) - (double)(t[6] - t[0]) * (t[5] - t[2]));
                const float cx = (t[0] + t[3] + t[6]) / 3.0f;
                (cx < split ? left : right) += area;
            }
        }
    };

    class NavPathTest : public NavSceneTest {
    protected:
        // a 30 x 10 plane with a strip of area 3 ("Mud", cost 4) across it at x in [-1, 1]
        Entity mudStrip(float mudCost = 4.0f) {
            nav::NavigationSettings settings = nav::NavigationSettings::defaults();
            settings.areas[3].name = "Mud";
            settings.areas[3].cost = mudCost;
            nav::setSettings(settings);
            const Entity surface = planeSurface(30.0f, 10.0f);
            areaVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 12.0f), 3);
            bake(surface);
            return surface;
        }
    };

    class NavFilterTest : public NavSceneTest {};

    std::vector<float> flatten(const std::vector<nav::NavHit>& hits) {
        std::vector<float> out;
        for (const nav::NavHit& hit : hits)
            out.insert(out.end(), {hit.position.x, hit.position.y, hit.position.z, (float)hit.area});
        return out;
    }

    float sumLengths(const nav::NavPath& path) {
        float total = 0.0f;
        for (float length : path.areaLengths)
            total += length;
        return total;
    }

}

// AC1: the same seed, mesh and calls give the same points; a seeded call leaves the system's
// stream alone; setRandomSeed restarts it; an anywhere draw takes exactly 3 values
TEST_F(NavRandomTest, SeededStreamRepeats){
    const Entity surface = planeSurface();
    box(*scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(4.0f, 2.0f, 4.0f));
    bake(surface);
    EXPECT_EQ(nav->getRandomSeed(), NavigationSystem::DEFAULT_RANDOM_SEED);

    // a second system on the same fixture
    Scene other;
    test::disableSubSystems(other);
    other.getScheduler().setEnabled("NavigationSystem", true);
    NavigationSystem* otherNav = other.getSystem<NavigationSystem>().get();
    plane(other, Vector3(), 20.0f, 20.0f);
    box(other, Vector3(0.0f, 1.0f, 0.0f), Vector3(4.0f, 2.0f, 4.0f));
    const Entity otherSurface = transformEntity(other, Vector3(), NULL_ENTITY, "Surface");
    addSurface(other, otherSurface, 0.2f, 32);
    resolveTransforms(other);
    ASSERT_TRUE(otherNav->bakeNow(otherSurface, NavBakeMode::FULL).ok);

    auto draws = [](NavigationSystem* system, int count) {
        std::vector<nav::NavHit> hits;
        for (int i = 0; i < count; i++) {
            nav::NavHit hit;
            EXPECT_TRUE(system->randomPoint(hit));
            EXPECT_TRUE(hit.hit);
            hits.push_back(hit);
        }
        return flatten(hits);
    };
    const std::vector<float> first = draws(nav, 100);
    const std::vector<float> second = draws(otherNav, 100);
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)), 0);
    // the points are spread, not one point
    float spread = 0.0f;
    for (size_t i = 4; i < first.size(); i += 4)
        spread = std::max(spread, std::fabs(first[i] - first[0]));
    EXPECT_GT(spread, 5.0f);

    // a seeded call: its own stream, the same point for the same seed; the system stream's next
    // value is unchanged (both systems still agree)
    nav::RandomPointQuery seeded;
    seeded.seeded = true;
    seeded.seed = 42;
    nav::NavHit a, b;
    ASSERT_TRUE(nav->randomPoint(a, {}, seeded));
    ASSERT_TRUE(nav->randomPoint(b, {}, seeded));
    EXPECT_EQ(std::memcmp(&a.position, &b.position, sizeof(Vector3)), 0);
    const std::vector<float> afterSeeded = draws(nav, 10);
    const std::vector<float> otherNext = draws(otherNav, 10);
    EXPECT_EQ(std::memcmp(afterSeeded.data(), otherNext.data(), afterSeeded.size() * sizeof(float)), 0);

    // setRandomSeed restarts the stream
    nav->setRandomSeed(NavigationSystem::DEFAULT_RANDOM_SEED);
    const std::vector<float> restarted = draws(nav, 100);
    EXPECT_EQ(std::memcmp(first.data(), restarted.data(), first.size() * sizeof(float)), 0);
    nav->setRandomSeed(7);
    EXPECT_EQ(nav->getRandomSeed(), 7u);
    EXPECT_NE(std::memcmp(first.data(), draws(nav, 100).data(), first.size() * sizeof(float)), 0);

    // exactly three draws per anywhere point (R2)
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    ASSERT_NE(mesh, nullptr);
    nav::NavRandom stream(99), reference(99);
    nav::NavHit hit;
    ASSERT_TRUE(mesh->findRandomPoint(nav::NavFilter::fromSettings(nav::ALL_AREAS), stream, -1, hit));
    for (int i = 0; i < 3; i++)
        reference.next();
    EXPECT_EQ(stream.state, reference.state);
}

// AC2: a tile fully covered and a tile a quarter covered; 20k anywhere draws split by area
// (chi-square p > 0.001). Detour's findRandomPoint picks a tile first (about half the draws each)
TEST_F(NavRandomTest, UniformByArea){
    // tiles of 32 cells (6.4 m) from the geometry's minimum (0, 0): the first tile is all floor,
    // the second has a 3.2 x 3.2 floor in its corner
    plane(*scene, Vector3(3.2f, 0.0f, 3.15f), 6.4f, 6.3f);
    plane(*scene, Vector3(8.0f, 0.0f, 1.6f), 3.2f, 3.2f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);
    ASSERT_EQ(nav->getNavMesh(surface)->getTileCount(), 2);
    double leftArea = 0.0, rightArea = 0.0;
    areasAround(surface, 6.4f, leftArea, rightArea);
    ASSERT_GT(leftArea, 0.0);
    ASSERT_GT(rightArea, 0.0);
    // the floors' walkable shares are far from one half each (what a tile-first pick gives)
    EXPECT_GT(leftArea / (leftArea + rightArea), 0.75);

    constexpr int DRAWS = 20000;
    int left = 0, right = 0;
    for (int i = 0; i < DRAWS; i++) {
        nav::NavHit hit;
        ASSERT_TRUE(nav->randomPoint(hit));
        (hit.position.x < 6.4f ? left : right)++;
    }
    const double expectedLeft = DRAWS * leftArea / (leftArea + rightArea);
    const double expectedRight = DRAWS - expectedLeft;
    const double chi2 = (left - expectedLeft) * (left - expectedLeft) / expectedLeft +
                        (right - expectedRight) * (right - expectedRight) / expectedRight;
    EXPECT_LT(chi2, CHI2_1DOF_P001) << left << " / " << right << " draws, expected " << expectedLeft << " / " << expectedRight;
}

// AC3: around a point: every point within the radius, reachable from the centre, never on the
// other island, never in a masked area
TEST_F(NavRandomTest, AroundCircleConnectedWithinRadius){
    // two islands 2 m apart; the left one has a strip of area 4 the filter masks out
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 10.0f, 10.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 10.0f, 10.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    areaVolume(Vector3(-6.0f, 0.0f, 3.0f), Vector3(6.0f, 4.0f, 1.0f), 4);
    bake(surface);

    nav::QueryFilter filter;
    filter.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << 4));
    nav::RandomPointQuery query;
    query.around = true;
    query.center = Vector3(-3.0f, 0.0f, 0.0f);
    query.radius = 5.0f;
    int misses = 0;
    for (int i = 0; i < 1000; i++) {
        nav::NavHit hit;
        if (!nav->randomPoint(hit, filter, query)) {
            misses++;
            continue;
        }
        const Vector3 p = hit.position;
        EXPECT_LE(Vector3(p.x - query.center.x, 0.0f, p.z - query.center.z).length(), query.radius + 1e-3f) << p.toString();
        EXPECT_LT(p.x, 0.0f) << "the other island: " << p.toString();
        EXPECT_NE(hit.area, 4) << p.toString();
        EXPECT_TRUE(nav->isReachable(query.center, p, filter)) << p.toString();
    }
    // 8 tries per call: a miss is rare
    EXPECT_LT(misses, 10);

    // a circle far smaller than the polygon it is in (one polygon over a 40 m floor) still finds
    // its points (Detour alone puts most of them past the radius)
    Scene open;
    test::disableSubSystems(open);
    open.getScheduler().setEnabled("NavigationSystem", true);
    plane(open, Vector3(), 40.0f, 40.0f);
    const Entity openSurface = transformEntity(open, Vector3(), NULL_ENTITY, "Surface");
    addSurface(open, openSurface, 0.2f, 256);
    resolveTransforms(open);
    NavigationSystem* openNav = open.getSystem<NavigationSystem>().get();
    ASSERT_TRUE(openNav->bakeNow(openSurface, NavBakeMode::FULL).ok);
    nav::RandomPointQuery small;
    small.around = true;
    small.center = Vector3(-10.0f, 0.0f, -10.0f);
    small.radius = 1.0f;
    for (int i = 0; i < 200; i++) {
        nav::NavHit hit;
        ASSERT_TRUE(openNav->randomPoint(hit, {}, small)) << "draw " << i;
        EXPECT_LE(Vector3(hit.position.x + 10.0f, 0.0f, hit.position.z + 10.0f).length(), 1.0f + 1e-3f);
    }

    // a centre off the mesh finds nothing
    query.center = Vector3(0.0f, 0.0f, 30.0f);
    nav::NavHit none;
    EXPECT_FALSE(nav->randomPoint(none, filter, query));
}

// AC4: on an island: every point shares the island of the given point; nothing under it, no
// point; closing a door between two halves makes them two islands and the draws keep to one
TEST_F(NavRandomTest, SameIsland){
    // a corridor (x in [-11, -1]) with a strip of area 4 across it at x = -6 (the door), and a
    // second floor apart from it (x in [6, 10])
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 10.0f, 4.0f);
    plane(*scene, Vector3(8.0f, 0.0f, 0.0f), 4.0f, 4.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    areaVolume(Vector3(-6.0f, 0.0f, 0.0f), Vector3(1.0f, 4.0f, 6.0f), 4);
    bake(surface);
    ASSERT_EQ(nav->getIslands(surface).size(), 2u);

    nav::RandomPointQuery query;
    query.sameIsland = true;
    query.islandOf = Vector3(-9.0f, 0.0f, 0.0f);
    const int corridor = nav->locate(query.islandOf).island;
    ASSERT_GE(corridor, 0);
    bool crossesDoor = false;
    for (int i = 0; i < 1000; i++) {
        nav::NavHit hit;
        ASSERT_TRUE(nav->randomPoint(hit, {}, query));
        EXPECT_EQ(nav->locate(hit.position).island, corridor) << hit.position.toString();
        EXPECT_LT(hit.position.x, 0.0f) << hit.position.toString();
        crossesDoor = crossesDoor || hit.position.x > -5.5f;
    }
    EXPECT_TRUE(crossesDoor);
    // nothing under the point: no island, no point
    query.islandOf = Vector3(2.0f, 0.0f, 0.0f);
    nav::NavHit none;
    EXPECT_FALSE(nav->randomPoint(none, {}, query));

    // the door closes: its polygons get flags 0 (what a Not Walkable runtime volume does)
    nav::NavMesh* mesh = const_cast<nav::NavMesh*>(nav->getNavMesh(surface));
    ASSERT_NE(mesh, nullptr);
    const nav::NavFilter all = nav::NavFilter::fromSettings(nav::ALL_AREAS);
    // every polygon of the strip (x in [-6.5, -5.5], the walkable z in [-1.5, 1.5])
    for (float x = -6.4f; x <= -5.59f; x += 0.1f) {
        for (float z = -1.45f; z <= 1.46f; z += 0.1f) {
            const uint32_t ref = mesh->getPolyAt(Vector3(x, 0.0f, z), all, Vector3(0.04f, 1.0f, 0.04f));
            // the door's polygons only (a point at its edge may find the corridor's)
            if (ref && mesh->locate(Vector3(x, 0.0f, z), all, Vector3(0.04f, 1.0f, 0.04f), 0.0f).area == 4)
                mesh->setPolyFlags(ref, 0);
        }
    }
    ASSERT_EQ(nav->getIslands(surface).size(), 3u);
    query.islandOf = Vector3(-9.0f, 0.0f, 0.0f);
    // with a centre too: around and on the island at once
    nav::RandomPointQuery around = query;
    around.around = true;
    around.center = Vector3(-7.0f, 0.0f, 0.0f);
    around.radius = 5.0f;
    for (int i = 0; i < 1000; i++) {
        nav::NavHit hit;
        ASSERT_TRUE(nav->randomPoint(hit, {}, query));
        EXPECT_LT(hit.position.x, -6.5f + CELL) << hit.position.toString();
        nav::NavHit near;
        if (nav->randomPoint(near, {}, around)) {
            EXPECT_LT(near.position.x, -6.5f + CELL) << near.position.toString();
            EXPECT_LE(Vector3(near.position.x + 7.0f, 0.0f, near.position.z).length(), 5.0f + 1e-3f);
        }
    }
    query.islandOf = Vector3(-3.0f, 0.0f, 0.0f);
    for (int i = 0; i < 200; i++) {
        nav::NavHit hit;
        ASSERT_TRUE(nav->randomPoint(hit, {}, query));
        EXPECT_GT(hit.position.x, -5.5f - CELL) << hit.position.toString();
        EXPECT_LT(hit.position.x, 0.0f) << hit.position.toString();
    }
}

// AC7: a path across the mud strip prices each stretch by its area; areaCrossings adds the strip
// edges as corners; a link path flags its link corner and prices the link at its own cost
TEST_F(NavPathTest, CornerAreasAndCost){
    const Entity surface = mudStrip(4.0f);
    const Vector3 from(-10.0f, 0.0f, 0.0f), to(10.0f, 0.0f, 0.0f);
    const nav::NavPath path = nav->calculatePath(from, to);
    ASSERT_EQ(path.status, nav::PathStatus::Complete);
    ASSERT_EQ(path.cornerAreas.size(), path.corners.size());
    ASSERT_EQ(path.cornerFlags.size(), path.corners.size());
    EXPECT_TRUE(path.cornerFlags.front() & nav::CORNER_START);
    EXPECT_TRUE(path.cornerFlags.back() & nav::CORNER_END);
    EXPECT_EQ(path.cornerAreas.back(), nav::AREA_WALKABLE);
    EXPECT_NEAR(path.areaLengths[3], 2.0f, CELL);
    EXPECT_NEAR(path.areaLengths[nav::AREA_WALKABLE], 18.0f, 2.0f * CELL);
    EXPECT_NEAR(sumLengths(path), path.length, 1e-3f);
    EXPECT_NEAR(path.cost, path.areaLengths[nav::AREA_WALKABLE] * 1.0f + path.areaLengths[3] * 4.0f, 1e-3f);
    EXPECT_FALSE(path.polys.empty());
    EXPECT_EQ(path.meshRevision, nav->getNavMesh(surface)->getRevision());

    // the strip's two edges become corners, the stretch between them is the mud's
    nav::PathOptions crossings;
    crossings.areaCrossings = true;
    const nav::NavPath crossed = nav->calculatePath(from, to, {}, crossings);
    ASSERT_EQ(crossed.status, nav::PathStatus::Complete);
    bool enters = false, leaves = false;
    for (size_t i = 0; i + 1 < crossed.corners.size(); i++) {
        if (std::fabs(crossed.corners[i].x + 1.0f) < CELL && crossed.cornerAreas[i] == 3)
            enters = true;
        if (std::fabs(crossed.corners[i].x - 1.0f) < CELL && crossed.cornerAreas[i] == nav::AREA_WALKABLE)
            leaves = true;
    }
    EXPECT_TRUE(enters && leaves) << crossed.corners.size() << " corners";
    EXPECT_GT(crossed.corners.size(), path.corners.size());
    EXPECT_NEAR(crossed.cost, path.cost, 1e-3f);
    EXPECT_NEAR(crossed.length, path.length, 1e-3f);
}

TEST_F(NavPathTest, LinkCornerAndCost){
    // two islands joined by a link of cost 3 per metre (area Jump)
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
    const Entity linkEntity = transformEntity(*scene, Vector3(), NULL_ENTITY, "Link");
    NavMeshLinkComponent link;
    link.startPoint = Vector3(-2.5f, 0.0f, 0.0f);
    link.endPoint = Vector3(2.5f, 0.0f, 0.0f);
    link.costModifier = 3.0f;
    scene->addComponent<NavMeshLinkComponent>(linkEntity, link);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);

    const nav::NavPath path = nav->calculatePath(Vector3(-8.0f, 0.0f, 0.0f), Vector3(8.0f, 0.0f, 0.0f));
    ASSERT_EQ(path.status, nav::PathStatus::Complete);
    size_t linkCorner = path.corners.size();
    for (size_t i = 0; i < path.cornerFlags.size(); i++) {
        if (path.cornerFlags[i] & nav::CORNER_OFF_MESH_LINK)
            linkCorner = i;
    }
    ASSERT_LT(linkCorner + 1, path.corners.size()) << "a corner flags the link start";
    // the link's stretch reports its area, and costs its own 3 per metre
    EXPECT_EQ(path.cornerAreas[linkCorner], nav::AREA_JUMP);
    const float linkLength = path.corners[linkCorner].distance(path.corners[linkCorner + 1]);
    EXPECT_NEAR(path.areaLengths[nav::AREA_JUMP], linkLength, 1e-3f);
    // the link's ends snap to the mesh
    EXPECT_NEAR(linkLength, 5.0f, 3.0f * CELL);
    EXPECT_NEAR(path.cost, path.areaLengths[nav::AREA_WALKABLE] + 3.0f * linkLength, 1e-3f);
    EXPECT_NEAR(sumLengths(path), path.length, 1e-3f);
}

// AC8: the raycast's cost is the travelled part's
TEST_F(NavPathTest, RaycastCost){
    mudStrip(4.0f);
    nav::NavHit hit;
    // along the strip: 6 m of mud
    EXPECT_FALSE(nav->raycast(Vector3(0.0f, 0.0f, -3.0f), Vector3(0.0f, 0.0f, 3.0f), hit));
    // the cost walks the polygons' heights, the distance is in the plane (S19): equal to 1e-3 relative
    EXPECT_NEAR(hit.cost, hit.distance * 4.0f, 1e-3f * hit.distance * 4.0f);
    EXPECT_NEAR(hit.distance, 6.0f, CELL);
    // across it: 18 m of floor and 2 of mud
    EXPECT_FALSE(nav->raycast(Vector3(-9.0f, 0.0f, 0.0f), Vector3(9.0f, 0.0f, 0.0f), hit));
    EXPECT_NEAR(hit.cost, 16.0f + 2.0f * 4.0f, 3.0f * CELL);
    // blocked at the edge as S19's raycast (NavQueryTest.Raycast): hit, position and normal kept,
    // the cost of the part up to the wall
    EXPECT_TRUE(nav->raycast(Vector3(5.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f), hit));
    EXPECT_NEAR(hit.position.x, 14.5f, 2.0f * CELL);
    EXPECT_NEAR(hit.normal.x, -1.0f, 0.05f);
    EXPECT_NEAR(hit.cost, hit.distance, 1e-3f * hit.distance);
}

// AC9: a filter's own area cost makes the path detour; reset restores it; a cost below 1 is 1
TEST_F(NavFilterTest, PerQueryAreaCost){
    // a strip of area 3 (project cost 1) across the plane but for a gap at z in [3, 5]
    const Entity surface = planeSurface(30.0f, 10.0f);
    areaVolume(Vector3(0.0f, 0.0f, -1.0f), Vector3(2.0f, 4.0f, 8.0f), 3);
    bake(surface);
    const Vector3 from(-10.0f, 0.0f, 0.0f), to(10.0f, 0.0f, 0.0f);
    nav::QueryFilter filter;
    const nav::NavPath straight = nav->calculatePath(from, to, filter);
    ASSERT_EQ(straight.status, nav::PathStatus::Complete);
    EXPECT_GT(straight.areaLengths[3], 1.0f);
    auto maxZ = [](const nav::NavPath& path) {
        float z = -1e9f;
        for (const Vector3& corner : path.corners)
            z = std::max(z, corner.z);
        return z;
    };
    EXPECT_LT(maxZ(straight), 1.0f);

    EXPECT_FLOAT_EQ(filter.getAreaCost(3), 1.0f);
    filter.setAreaCost(3, 10.0f);
    EXPECT_FLOAT_EQ(filter.getAreaCost(3), 10.0f);
    const nav::NavPath detour = nav->calculatePath(from, to, filter);
    ASSERT_EQ(detour.status, nav::PathStatus::Complete);
    EXPECT_GT(maxZ(detour), 3.0f - CELL);
    EXPECT_NEAR(detour.areaLengths[3], 0.0f, 1e-3f);
    // the project's costs did not change
    EXPECT_FLOAT_EQ(nav::getSettings().areas[3].cost, 1.0f);

    filter.resetAreaCost(3);
    const nav::NavPath restored = nav->calculatePath(from, to, filter);
    ASSERT_EQ(restored.corners.size(), straight.corners.size());
    for (size_t i = 0; i < restored.corners.size(); i++)
        EXPECT_LT(restored.corners[i].distance(straight.corners[i]), 1e-4f);

    // below 1: used as 1, with a warning (once per process)
    test::LogCapture log;
    filter.setAreaCost(3, 0.5f);
    filter.setAreaCost(3, 0.25f);
    EXPECT_FLOAT_EQ(filter.getAreaCost(3), 1.0f);
    EXPECT_LE(log.count(D_LOG_WARN, "below 1"), 1u);
    const nav::NavPath clamped = nav->calculatePath(from, to, filter);
    EXPECT_NEAR(clamped.cost, straight.cost, 1e-3f);
    // the project's cost of an area the filter does not set follows the project
    nav::NavigationSettings settings = nav::NavigationSettings::defaults();
    settings.areas[4].cost = 7.0f;
    nav::setSettings(settings);
    EXPECT_FLOAT_EQ(filter.getAreaCost(4), 7.0f);
    EXPECT_FLOAT_EQ(filter.getAreaCost(16), 0.0f);
}
