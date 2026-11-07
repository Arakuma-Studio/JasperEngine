// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene's navigation findings (docs/specs/S75-navigation-diagnostics.md R13-R15; AC10): a
// scene that raises every code once with its details, the same list twice, and stale counts from
// the caller's cache.

#include "NavTestUtil.h"

#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshObstacleComponent.h"

#include <algorithm>
#include <map>
#include <string>

using namespace jasperengine;
using namespace jasperengine::navtest;
using nav::FindingSeverity;
using nav::NavFinding;

namespace {

    double findingDetail(const NavFinding& finding, const std::string& key) {
        for (const auto& entry : finding.details) {
            if (entry.first == key)
                return entry.second;
        }
        ADD_FAILURE() << finding.code << " has no detail " << key;
        return 0.0;
    }

    class NavFindingsTest : public NavSceneTest {
    protected:
        Entity ground = NULL_ENTITY;
        Entity unbaked = NULL_ENTITY;
        Entity unreadable = NULL_ENTITY;
        Entity lost = NULL_ENTITY;
        Entity typeless = NULL_ENTITY;
        Entity dangling = NULL_ENTITY;
        Entity carver = NULL_ENTITY;

        // a scene raising each code once
        void everyCode() {
            nav::NavigationSettings settings = nav::NavigationSettings::defaults();
            nav::AgentTypeSettings giant;
            giant.id = 1;
            giant.name = "Giant";
            giant.radius = 1.5f;
            settings.agentTypes.push_back(giant);
            nav::setSettings(settings);

            // the main floor and a 4 x 4 m floor far off (a small island)
            plane(*scene, Vector3(), 40.0f, 40.0f);
            plane(*scene, Vector3(100.0f, 0.0f, 0.0f), 4.0f, 4.0f);
            ground = transformEntity(*scene, Vector3(), NULL_ENTITY, "Ground");
            addSurface(*scene, ground, 0.25f, 32);
            // a link whose end lands on nothing
            dangling = transformEntity(*scene, Vector3(), NULL_ENTITY, "Dangling Link");
            NavMeshLinkComponent link;
            link.startPoint = Vector3(0.0f, 0.0f, 15.0f);
            link.endPoint = Vector3(0.0f, 0.0f, 24.0f);
            scene->addComponent<NavMeshLinkComponent>(dangling, link);
            bake(ground);

            // no layer to collect: never baked, and its bake failed
            unbaked = transformEntity(*scene, Vector3(), NULL_ENTITY, "Empty Surface");
            addSurface(*scene, unbaked).includeLayers = 0;
            std::string error;
            EXPECT_FALSE(nav->bake(unbaked, NavBakeMode::FULL, error));

            unreadable = transformEntity(*scene, Vector3(), NULL_ENTITY, "Broken Surface");
            addSurface(*scene, unreadable).navMeshData = AssetRef("navigation/missing.navmesh");

            // a box placed after the bake: the main surface is stale
            box(*scene, Vector3(5.0f, 0.5f, 5.0f), Vector3(1.0f, 1.0f, 1.0f));
            // an agent far off every floor, one of an unknown type (no surface of it either)
            lost = agent(*scene, Vector3(0.0f, 0.0f, 80.0f));
            typeless = agent(*scene, Vector3(2.0f, 0.0f, 2.0f));
            scene->getComponent<NavMeshAgentComponent>(typeless).agentType = 7;
            // 17 distinct area masks on the main floor (all with Walkable)
            for (int i = 0; i < 17; i++) {
                const Entity walker = agent(*scene, Vector3(-15.0f + (float)i, 0.0f, -10.0f));
                // every area but one of 1-15, every area, Walkable only
                const int mask = i < 15 ? (int)(nav::ALL_AREAS & ~(1u << (i + 1))) : (i == 15 ? nav::ALL_AREAS : 1);
                scene->getComponent<NavMeshAgentComponent>(walker).areaMask = mask;
            }
            // carving on a surface without Runtime Carving
            carver = transformEntity(*scene, Vector3(-5.0f, 0.5f, 5.0f), NULL_ENTITY, "Crate");
            NavMeshObstacleComponent obstacle;
            obstacle.carve = true;
            scene->addComponent<NavMeshObstacleComponent>(carver, obstacle);
            resolveTransforms(*scene);
        }
    };

}

TEST_F(NavFindingsTest, EveryCode){
    everyCode();
    const std::vector<NavFinding> findings = nav->getFindings();
    std::map<std::string, std::vector<const NavFinding*>> byCode;
    for (const NavFinding& finding : findings)
        byCode[finding.code].push_back(&finding);
    const char* codes[] = {"not_baked", "navmesh_unreadable", "stale_bake", "bake_failed", "agent_off_mesh", "agent_no_surface",
                           "agent_type_missing", "agent_type_unused", "link_unconnected", "small_island", "crowd_filter_overflow",
                           "obstacle_carve_ignored"};
    for (const char* code : codes) {
        ASSERT_EQ(byCode[code].size(), 1u) << code;
        EXPECT_FALSE(byCode[code].front()->message.empty()) << code;
    }
    EXPECT_EQ(byCode.size(), sizeof(codes) / sizeof(codes[0]));

    EXPECT_EQ(byCode["not_baked"].front()->entity, unbaked);
    // S19's text (test_checks.py)
    EXPECT_EQ(byCode["not_baked"].front()->message,
              "NavMesh Surface 'Empty Surface' is not baked: its agents find no navmesh in the build (Bake, or bake_navigation)");
    EXPECT_EQ(byCode["navmesh_unreadable"].front()->entity, unreadable);
    EXPECT_EQ(byCode["navmesh_unreadable"].front()->severity, FindingSeverity::Error);
    EXPECT_EQ(byCode["stale_bake"].front()->entity, ground);
    EXPECT_GT(findingDetail(*byCode["stale_bake"].front(), "dirty_tiles"), 0.0);
    EXPECT_EQ(byCode["bake_failed"].front()->entity, unbaked);
    EXPECT_EQ(findingDetail(*byCode["bake_failed"].front(), "error_code"), (double)nav::NavBakeError::NothingCollected);
    EXPECT_EQ(byCode["agent_off_mesh"].front()->entity, lost);
    EXPECT_EQ(findingDetail(*byCode["agent_off_mesh"].front(), "snap_distance"), -1.0);
    EXPECT_EQ(byCode["agent_no_surface"].front()->entity, typeless);
    EXPECT_EQ(byCode["agent_type_missing"].front()->entity, typeless);
    EXPECT_EQ(byCode["agent_type_unused"].front()->entity, NULL_ENTITY);
    EXPECT_EQ(byCode["agent_type_unused"].front()->severity, FindingSeverity::Info);
    EXPECT_EQ(findingDetail(*byCode["agent_type_unused"].front(), "agent_type"), 1.0);
    EXPECT_EQ(byCode["link_unconnected"].front()->entity, dangling);
    EXPECT_GE(findingDetail(*byCode["link_unconnected"].front(), "island_start"), 0.0);
    EXPECT_EQ(findingDetail(*byCode["link_unconnected"].front(), "island_end"), -1.0);
    EXPECT_EQ(findingDetail(*byCode["link_unconnected"].front(), "connections"), 1.0);
    EXPECT_EQ(byCode["small_island"].front()->entity, ground);
    // 4 x 4 m eroded by r = 0.5 (whole 0.25 m cells and the boundary): between 2.5^2 and 3^2
    EXPECT_GE(findingDetail(*byCode["small_island"].front(), "area_m2"), 6.0);
    EXPECT_LE(findingDetail(*byCode["small_island"].front(), "area_m2"), 9.0);
    EXPECT_EQ(byCode["crowd_filter_overflow"].front()->entity, ground);
    EXPECT_EQ(findingDetail(*byCode["crowd_filter_overflow"].front(), "masks"), 17.0);
    EXPECT_EQ(byCode["obstacle_carve_ignored"].front()->entity, carver);

    // errors first, then by entity, then by code
    for (size_t i = 1; i < findings.size(); i++) {
        const NavFinding& a = findings[i - 1];
        const NavFinding& b = findings[i];
        ASSERT_TRUE(a.severity > b.severity || (a.severity == b.severity && (a.entity < b.entity || (a.entity == b.entity && a.code <= b.code))))
            << a.code << " before " << b.code;
    }
}

TEST_F(NavFindingsTest, Deterministic){
    everyCode();
    const std::vector<NavFinding> first = nav->getFindings();
    const std::vector<NavFinding> second = nav->getFindings();
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

TEST_F(NavFindingsTest, CachedStaleCounts){
    everyCode();
    std::vector<Entity> asked;
    nav::NavFindingOptions options;
    options.dirtyTilesOf = [&](Entity surface) {
        asked.push_back(surface);
        return 42;
    };
    const std::vector<NavFinding> findings = nav->getFindings(options);
    // only the baked surface is asked, and its count is the cache's
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked.front(), ground);
    const auto stale = std::find_if(findings.begin(), findings.end(), [](const NavFinding& f) { return f.code == "stale_bake"; });
    ASSERT_NE(stale, findings.end());
    EXPECT_EQ(findingDetail(*stale, "dirty_tiles"), 42.0);
    // stale off: no count, no stale_bake
    asked.clear();
    options.stale = false;
    const std::vector<NavFinding> fresh = nav->getFindings(options);
    EXPECT_TRUE(asked.empty());
    EXPECT_TRUE(std::none_of(fresh.begin(), fresh.end(), [](const NavFinding& f) { return f.code == "stale_bake"; }));
}
