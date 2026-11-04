// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor side of navigation (docs/specs/S19-navigation.md R31, R35, R39, R41): the six
// components re-encode identically (scene round trip), the project's navigation block keeps only
// what differs from the defaults and reads back sanitized, the overlay settings round trip, the
// generated main.cpp sets the settings, and the nav-grid brush's stamps, lines, fills and strokes.
// S53 (docs/specs/S53-navigation-fixes.md R17, R22, R27): Max Carving Obstacles and Carve Tiles Per
// Tick default when missing, and the overlay's reservation tint.

#include "Stream.h"
#include "navigation/NavGridBrush.h"
#include "navigation/NavigationOverlay.h"
#include "navigation/NavigationProjectSettings.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;
using nav::GridCell;

namespace {

    template <typename T>
    void expectStable(const T& component, std::function<YAML::Node(const T&)> encode, std::function<T(const YAML::Node&)> decode) {
        const YAML::Node first = encode(component);
        ASSERT_TRUE(first.IsDefined());
        const T decoded = decode(YAML::Load(YAML::Dump(first)));
        EXPECT_EQ(YAML::Dump(first), YAML::Dump(encode(decoded)));
    }

}

#define NAV_STABLE(Type, ...) \
    TEST(NavigationStream, Type##ReEncodesIdentically){ \
        Type component{}; \
        __VA_ARGS__; \
        expectStable<Type>(component, [](const Type& c){ return Stream::encode##Type(c); }, \
                           [](const YAML::Node& node){ return Stream::decode##Type(node); }); \
        EXPECT_NO_ENGINE_ERRORS(); \
    }

NAV_STABLE(NavMeshSurfaceComponent, component.agentType = 2; component.collectObjects = NavCollectObjects::VOLUME;
           component.useGeometry = NavUseGeometry::BOTH; component.includeLayers = 0x5u; component.defaultArea = 3;
           component.volumeSize = Vector3(4, 5, 6); component.overrideCellSize = true; component.cellSize = 0.25f;
           component.overrideTileSize = true; component.tileSize = 48; component.plane = nav::NavPlane::XY;
           component.runtimeCarving = true; component.maxCarvingObstacles = 64;
           component.navMeshData = AssetRef("Level/NavMesh-Floor.navmesh"))
NAV_STABLE(NavMeshAgentComponent, component.speed = 5.0f; component.autoBraking = false; component.avoidancePriority = 10;
           component.obstacleAvoidance = nav::AvoidanceQuality::Low; component.areaMask = 0x1D;
           component.driver = NavAgentDriver::CHARACTER_CONTROLLER; component.baseOffset = 1.0f)
NAV_STABLE(NavMeshObstacleComponent, component.shape = NavObstacleShape::CAPSULE; component.radius = 2.0f; component.carve = true;
           component.carveOnlyStationary = false; component.moveThreshold = 0.3f)
NAV_STABLE(NavMeshLinkComponent, component.startPoint = Vector3(-1, 0, 2); component.endPoint = Vector3(3, 1, 2);
           component.width = 2.0f; component.costModifier = 4.0f; component.bidirectional = false; component.area = 4)
NAV_STABLE(NavMeshModifierComponent, component.overrideInclusion = NavModifierInclusion::EXCLUDE; component.overrideArea = true;
           component.area = 1; component.useVolume = true; component.volumeSize = Vector3(2, 2, 2))
NAV_STABLE(NavGridComponent, component.layout = nav::GridLayout::Hex; component.width = 20; component.height = 9;
           component.cellSize = 0.5f; component.source = NavGridSource::TILEMAP; component.emptyCellArea = 0;
           component.tileAreas = {{"water", 3}, {"wall", 1}}; component.collidersBlock = true;
           component.gridData = AssetRef("Level/NavGrid-Grid.navgrid"))

// S53 R27: a surface saved before Max Carving Obstacles existed loads with 128 and writes it back
TEST(NavigationStream, OldSurfaceLoadsDefaults){
    const YAML::Node s19 = YAML::Load("enabled: true\nagentType: 0\ncollectObjects: all\nuseGeometry: render_meshes\n"
                                      "includeLayers: 4294967295\ndefaultArea: 0\nruntimeCarving: true\ntileSize: 64\n");
    const NavMeshSurfaceComponent surface = Stream::decodeNavMeshSurfaceComponent(s19);
    EXPECT_TRUE(surface.runtimeCarving);
    EXPECT_EQ(surface.maxCarvingObstacles, 128);
    EXPECT_EQ(Stream::encodeNavMeshSurfaceComponent(surface)["maxCarvingObstacles"].as<int>(), 128);
    // out of range values are put back in 1-4096
    EXPECT_EQ(Stream::decodeNavMeshSurfaceComponent(YAML::Load("maxCarvingObstacles: 0")).maxCarvingObstacles, 1);
    EXPECT_EQ(Stream::decodeNavMeshSurfaceComponent(YAML::Load("maxCarvingObstacles: 9000")).maxCarvingObstacles, 4096);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(NavigationProjectSettings, DefaultsWriteNothing){
    EXPECT_EQ(encodeNavigationProjectSettings(NavigationProjectSettings()).size(), 0u);
    EXPECT_EQ(decodeNavigationProjectSettings(YAML::Node()), NavigationProjectSettings());
    // Carve Tiles Per Tick: 0 (Auto) by default, written only when set
    EXPECT_EQ(NavigationProjectSettings().settings.carveTilesPerTick, 0);
    EXPECT_FALSE(encodeNavigationProjectSettings(NavigationProjectSettings())["carveTilesPerTick"].IsDefined());
    NavigationProjectSettings carving;
    carving.settings.carveTilesPerTick = 3;
    const YAML::Node node = YAML::Load(YAML::Dump(encodeNavigationProjectSettings(carving)));
    EXPECT_EQ(node["carveTilesPerTick"].as<int>(), 3);
    EXPECT_EQ(decodeNavigationProjectSettings(node), carving);
    EXPECT_EQ(decodeNavigationProjectSettings(YAML::Load("carveTilesPerTick: 99")).settings.carveTilesPerTick, 64);
    EXPECT_EQ(decodeNavigationProjectSettings(YAML::Load("carveTilesPerTick: -2")).settings.carveTilesPerTick, 0);
}

TEST(NavigationProjectSettings, RoundTripAndSanitize){
    NavigationProjectSettings settings;
    settings.includeInBuilds = false;
    settings.settings.crowdMaxAgents = 64;
    settings.settings.webBakeBudgetMs = 2.5f;
    settings.settings.areas[3].name = "Mud";
    settings.settings.areas[3].cost = 5.0f;
    settings.settings.areas[4].color = Vector4(0.1f, 0.2f, 0.3f, 1.0f);
    nav::AgentTypeSettings giant;
    giant.id = 1;
    giant.name = "Giant";
    giant.radius = 1.5f;
    giant.height = 4.0f;
    settings.settings.agentTypes.push_back(giant);
    const YAML::Node node = YAML::Load(YAML::Dump(encodeNavigationProjectSettings(settings)));
    // only rows 3 and 4 of the area table were written
    ASSERT_TRUE(node["areas"].IsSequence());
    EXPECT_EQ(node["areas"].size(), 2u);
    EXPECT_EQ(decodeNavigationProjectSettings(node), settings);

    // costs under 1 and fixed rows renamed are put right on load
    YAML::Node bad = YAML::Load("areas: [{index: 0, name: Floor}, {index: 5, cost: 0.2}, {index: 99, cost: 3}]\ncrowdMaxAgents: -4");
    const NavigationProjectSettings read = decodeNavigationProjectSettings(bad);
    EXPECT_EQ(read.settings.areas[0].name, "Walkable");
    EXPECT_GE(read.settings.areas[5].cost, 1.0f);
    EXPECT_EQ(read.settings.crowdMaxAgents, 1);
}

TEST(NavigationProjectSettings, GeneratedCodeSetsThem){
    EXPECT_EQ(generateNavigationSettingsCode(nav::NavigationSettings(), "    "), "");
    nav::NavigationSettings settings;
    settings.crowdMaxAgents = 300;
    settings.areas[6].name = "Ice \"slippery\"";
    const std::string code = generateNavigationSettingsCode(settings, "    ");
    EXPECT_NE(code.find("navigation.crowdMaxAgents = 300;"), std::string::npos) << code;
    EXPECT_NE(code.find("navigation.areas[6] = {\"Ice \\\"slippery\\\"\""), std::string::npos) << code;
    EXPECT_NE(code.find("nav::setSettings(navigation);"), std::string::npos) << code;
    EXPECT_EQ(code.find("carveTilesPerTick"), std::string::npos) << "Auto is the default: " << code;
    settings.carveTilesPerTick = 2;
    const std::string carving = generateNavigationSettingsCode(settings, "    ");
    EXPECT_NE(carving.find("navigation.carveTilesPerTick = 2;"), std::string::npos) << carving;
    nav::NavigationSettings onlyCarving;
    onlyCarving.carveTilesPerTick = 1;
    EXPECT_NE(generateNavigationSettingsCode(onlyCarving, "").find("navigation.carveTilesPerTick = 1;"), std::string::npos);
}

TEST(NavigationOverlay, SettingsRoundTrip){
    EXPECT_EQ(encodeNavOverlaySettings(NavOverlaySettings()).size(), 0u);
    NavOverlaySettings settings;
    settings.showTileBounds = true;
    settings.showPaths = NavOverlaySettings::Paths::All;
    settings.showNavMesh = false;
    NavOverlaySettings read;
    decodeNavOverlaySettings(YAML::Load(YAML::Dump(encodeNavOverlaySettings(settings))), read);
    EXPECT_EQ(read, settings);
}

// S53 R22: in play the cells agents reserved are tinted halfway to the agent-path colour; the
// others keep their area's colour (overrides more opaque)
TEST(NavigationOverlay, ReservationTint){
    nav::NavGridMap map;
    map.reset(4, 3);
    map.setArea(GridCell{1, 0}, 3);
    map.setOverride(GridCell{2, 1}, true);
    std::array<Vector4, nav::AREA_COUNT> areaColors{};
    for (int a = 0; a < nav::AREA_COUNT; a++) areaColors[(size_t)a] = Vector4(0.0f, (float)a / 16.0f, 1.0f, 1.0f);
    std::vector<Vector4> plain;
    NavigationOverlay::gridCellColors(map, areaColors, {}, plain);
    ASSERT_EQ(plain.size(), 12u);
    EXPECT_EQ(plain[0], Vector4(0.0f, 0.0f, 1.0f, NavigationOverlay::CELL_ALPHA));
    EXPECT_EQ(plain[1], Vector4(0.0f, 3.0f / 16.0f, 1.0f, NavigationOverlay::CELL_ALPHA));
    EXPECT_EQ(plain[6], Vector4(0.0f, 0.0f, 1.0f, NavigationOverlay::OVERRIDE_CELL_ALPHA));

    // cells 1 and 6 reserved (6 twice: two agents), and one index outside the grid
    std::vector<Vector4> tinted;
    NavigationOverlay::gridCellColors(map, areaColors, {6, 1, 6, 40}, tinted);
    ASSERT_EQ(tinted.size(), 12u);
    const Vector4 path = NavigationOverlay::AGENT_PATH_COLOR;
    for (int i = 0; i < 12; i++) {
        if (i == 1 || i == 6) {
            const Vector4& base = plain[(size_t)i];
            const Vector4 expected((base.x + path.x) * 0.5f, (base.y + path.y) * 0.5f, (base.z + path.z) * 0.5f, base.w);
            EXPECT_EQ(tinted[(size_t)i], expected) << "cell " << i;
        } else {
            EXPECT_EQ(tinted[(size_t)i], plain[(size_t)i]) << "cell " << i;
        }
    }
}

TEST(NavGridBrush, StampsLinesAndFills){
    nav::NavGridMap map;
    map.reset(20, 20);
    // a 1-cell stamp is the cell; a 3-cell circle is the cell and its 4 neighbours; a 3-cell square 9
    EXPECT_EQ(NavGridBrush::stamp(map, GridCell{5, 5}, 1, NavGridBrush::Shape::Circle).size(), 1u);
    EXPECT_EQ(NavGridBrush::stamp(map, GridCell{5, 5}, 3, NavGridBrush::Shape::Circle).size(), 5u);
    EXPECT_EQ(NavGridBrush::stamp(map, GridCell{5, 5}, 3, NavGridBrush::Shape::Square).size(), 9u);
    // clipped at the edge
    EXPECT_EQ(NavGridBrush::stamp(map, GridCell{0, 0}, 3, NavGridBrush::Shape::Square).size(), 4u);
    // a line steps one cell at a time, ends included
    const std::vector<GridCell> line = NavGridBrush::line(map, GridCell{1, 1}, GridCell{7, 4});
    ASSERT_EQ(line.size(), 7u);
    EXPECT_EQ(line.front(), (GridCell{1, 1}));
    EXPECT_EQ(line.back(), (GridCell{7, 4}));
    // a wall splits the grid: a fill on one side stays there
    for (int y = 0; y < 20; y++) map.setArea(GridCell{10, y}, nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(NavGridBrush::flood(map, GridCell{3, 3}).size(), 200u);
    EXPECT_EQ(NavGridBrush::flood(map, GridCell{10, 3}).size(), 20u);
}

TEST(NavGridBrush, StrokeRecordsFirstOldAndLastNew){
    nav::NavGridMap map;
    map.reset(8, 8);
    NavGridBrush::Stroke stroke;
    EXPECT_EQ(stroke.apply(map, {0, 1, 2}, 3), 3);
    // painting the same cells again with another area: one entry each, the first old state kept
    EXPECT_EQ(stroke.apply(map, {1, 2, 3}, 1), 3);
    const std::vector<NavGridCellsCmd::Cell> cells = stroke.cells();
    ASSERT_EQ(cells.size(), 4u);
    for (const NavGridCellsCmd::Cell& cell : cells) {
        EXPECT_EQ(cell.oldArea, nav::AREA_WALKABLE);
        EXPECT_FALSE(cell.oldOverride);
        EXPECT_TRUE(cell.newOverride);
    }
    EXPECT_EQ(cells[0].newArea, 3);
    EXPECT_EQ(cells[3].newArea, nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(map.countOverrides(), 4);
    stroke.revert(map);
    EXPECT_EQ(map.countOverrides(), 0);
    for (int i = 0; i < map.getCellCount(); i++) EXPECT_EQ(map.getAreaAt(i), nav::AREA_WALKABLE);
}
