// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor side of navigation (docs/specs/S19-navigation.md R31, R35, R39, R41): the six
// components re-encode identically (scene round trip), the project's navigation block keeps only
// what differs from the defaults and reads back sanitized, the generated main.cpp sets the
// settings, and the nav-grid brush's stamps, lines, fills and strokes. (The overlay's settings:
// NavigationOverlayTest.cpp.)
// S76 (docs/specs/S76-navigation-gameplay-api.md R40, R41, R47): the Link, Modifier and Agent fields
// of old scenes, their Inspector rows' undo, and the overlay's area override boxes.
// S74 (docs/specs/S74-navigation-fixes.md R17, R22, R27): Max Carving Obstacles and Carve Tiles Per
// Tick default when missing, and the overlay's reservation tint.

#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/UndoFakeCommand.h"
#include "command/type/PropertyCmd.h"
#include "component/NavMeshAgentComponent.h"
#include "navigation/NavGridBrush.h"
#include "navigation/NavigationOverlay.h"
#include "navigation/NavigationProjectSettings.h"
#include "object/Primitive.h"
#include "subsystem/NavigationSystem.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <filesystem>
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
           component.width = 2.0f; component.costModifier = 4.0f; component.bidirectional = false; component.area = 4;
           component.activated = false)
NAV_STABLE(NavMeshModifierComponent, component.overrideInclusion = NavModifierInclusion::EXCLUDE; component.overrideArea = true;
           component.area = 1; component.useVolume = true; component.volumeSize = Vector3(2, 2, 2); component.applyAtRuntime = true)
NAV_STABLE(NavGridComponent, component.layout = nav::GridLayout::Hex; component.width = 20; component.height = 9;
           component.cellSize = 0.5f; component.source = NavGridSource::TILEMAP; component.emptyCellArea = 0;
           component.tileAreas = {{"water", 3}, {"wall", 1}}; component.collidersBlock = true;
           component.gridData = AssetRef("Level/NavGrid-Grid.navgrid"))

// S74 R27: a surface saved before Max Carving Obstacles existed loads with 128 and writes it back
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

// S74 R22: in play the cells agents reserved are tinted halfway to the agent-path colour; the
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

// ---------------------------------------------------------------- S76 runtime areas and links

namespace {

    // tools/fixtures/prefab-runtime, loaded headless: its Main Scene takes the GameObjects
    class NavigationEditorTest : public ::testing::Test {
    protected:
        editor::test::UndoStatics statics;
        jasperengine::test::TestTempDir dir{"nav-rows"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;

        void SetUp() override {
            std::filesystem::copy(std::filesystem::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                                  std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
            project->setSelectedSceneId(sceneId);
        }
        void TearDown() override {
            if (project) editor::test::closeProject(*project);
            project.reset();
        }
        CommandHistory* history() { return CommandHandle::get(sceneId); }

        Entity addObject(const char* name) {
            Scene* s = scene->scene;
            const Entity entity = s->createEntity();
            s->setEntityName(entity, name);
            s->addComponent<Transform>(entity, {});
            scene->entities.push_back(entity);
            return entity;
        }
    };

}

// AC29 (R47): a scene saved before S76 has no Activated or Apply At Runtime: they load as their
// defaults and the next save writes them
TEST_F(NavigationEditorTest, OldSceneLoadsDefaults){
    const YAML::Node link = YAML::Load("enabled: true\nagentType: 0\nstartPoint: [0, 0, -2.5]\nendPoint: [0, 0, 2.5]\nwidth: 0\n"
                                       "costModifier: -1\nbidirectional: true\nautoUpdatePosition: false\narea: 2\n");
    const NavMeshLinkComponent decodedLink = Stream::decodeNavMeshLinkComponent(link);
    EXPECT_TRUE(decodedLink.activated);
    const YAML::Node linkAgain = Stream::encodeNavMeshLinkComponent(decodedLink);
    ASSERT_TRUE(linkAgain["activated"]);
    EXPECT_TRUE(linkAgain["activated"].as<bool>());

    const YAML::Node modifier = YAML::Load("enabled: true\noverrideInclusion: auto\noverrideArea: true\narea: 1\napplyToChildren: true\n"
                                           "useVolume: true\nvolumeCenter: [0, 1, 0]\nvolumeSize: [4, 3, 4]\n");
    const NavMeshModifierComponent decodedModifier = Stream::decodeNavMeshModifierComponent(modifier);
    EXPECT_FALSE(decodedModifier.applyAtRuntime);
    EXPECT_TRUE(decodedModifier.useVolume);
    const YAML::Node modifierAgain = Stream::encodeNavMeshModifierComponent(decodedModifier);
    ASSERT_TRUE(modifierAgain["applyAtRuntime"]);
    EXPECT_FALSE(modifierAgain["applyAtRuntime"].as<bool>());

    // set, they round trip
    NavMeshLinkComponent off;
    off.activated = false;
    EXPECT_FALSE(Stream::decodeNavMeshLinkComponent(YAML::Load(YAML::Dump(Stream::encodeNavMeshLinkComponent(off)))).activated);
    NavMeshModifierComponent runtime;
    runtime.applyAtRuntime = true;
    EXPECT_TRUE(Stream::decodeNavMeshModifierComponent(YAML::Load(YAML::Dump(Stream::encodeNavMeshModifierComponent(runtime)))).applyAtRuntime);

    // S76 P3: an S19 agent has the stuck watchdog's defaults, and saves them
    const YAML::Node agent = YAML::Load("enabled: true\nagentType: 0\nspeed: 3.5\nautoRepath: true\nareaMask: 65535\n");
    const NavMeshAgentComponent decodedAgent = Stream::decodeNavMeshAgentComponent(agent);
    EXPECT_FALSE(decodedAgent.stuckDetection);
    EXPECT_FLOAT_EQ(decodedAgent.stuckSpeedRatio, 0.1f);
    EXPECT_FLOAT_EQ(decodedAgent.stuckTimeout, 2.0f);
    EXPECT_TRUE(decodedAgent.repathWhenStuck);
    const YAML::Node agentAgain = Stream::encodeNavMeshAgentComponent(decodedAgent);
    for (const char* key : {"stuckDetection", "stuckSpeedRatio", "stuckTimeout", "repathWhenStuck"})
        EXPECT_TRUE(agentAgain[key]) << key;
    NavMeshAgentComponent watched;
    watched.stuckDetection = true;
    watched.stuckSpeedRatio = 0.25f;
    watched.stuckTimeout = 3.0f;
    watched.repathWhenStuck = false;
    const NavMeshAgentComponent watchedAgain = Stream::decodeNavMeshAgentComponent(YAML::Load(YAML::Dump(Stream::encodeNavMeshAgentComponent(watched))));
    EXPECT_TRUE(watchedAgain.stuckDetection);
    EXPECT_FLOAT_EQ(watchedAgain.stuckSpeedRatio, 0.25f);
    EXPECT_FLOAT_EQ(watchedAgain.stuckTimeout, 3.0f);
    EXPECT_FALSE(watchedAgain.repathWhenStuck);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC29 (R40): Activated, Apply At Runtime and Detect Stuck are Inspector rows (PropertyCmd, undoable)
TEST_F(NavigationEditorTest, GameplayRowsUndo){
    Scene* s = scene->scene;
    const Entity bridge = addObject("Bridge");
    s->addComponent<NavMeshLinkComponent>(bridge, {});
    const Entity door = addObject("Door");
    NavMeshModifierComponent volume;
    volume.useVolume = true;
    s->addComponent<NavMeshModifierComponent>(door, volume);

    ASSERT_TRUE(history()->addCommand(new PropertyCmd<bool>(project.get(), sceneId, bridge, ComponentType::NavMeshLinkComponent, "activated", false)));
    EXPECT_FALSE(s->getComponent<NavMeshLinkComponent>(bridge).activated);
    ASSERT_TRUE(history()->addCommand(new PropertyCmd<bool>(project.get(), sceneId, door, ComponentType::NavMeshModifierComponent, "applyAtRuntime", true)));
    EXPECT_TRUE(s->getComponent<NavMeshModifierComponent>(door).applyAtRuntime);

    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(s->getComponent<NavMeshModifierComponent>(door).applyAtRuntime);
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(s->getComponent<NavMeshLinkComponent>(bridge).activated);

    // S76 P3: Detect Stuck and Stuck After (s)
    const Entity walker = addObject("Walker");
    s->addComponent<NavMeshAgentComponent>(walker, {});
    ASSERT_TRUE(history()->addCommand(new PropertyCmd<bool>(project.get(), sceneId, walker, ComponentType::NavMeshAgentComponent, "stuckDetection", true)));
    ASSERT_TRUE(history()->addCommand(new PropertyCmd<float>(project.get(), sceneId, walker, ComponentType::NavMeshAgentComponent, "stuckTimeout", 4.0f)));
    EXPECT_TRUE(s->getComponent<NavMeshAgentComponent>(walker).stuckDetection);
    EXPECT_FLOAT_EQ(s->getComponent<NavMeshAgentComponent>(walker).stuckTimeout, 4.0f);
    ASSERT_TRUE(history()->undo());
    EXPECT_FLOAT_EQ(s->getComponent<NavMeshAgentComponent>(walker).stuckTimeout, 2.0f);
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(s->getComponent<NavMeshAgentComponent>(walker).stuckDetection);
}

// AC30 (R41): a runtime modifier volume is drawn whether selected or not, its 12 box lines in its
// area's colour: solid while applied, at 35 % alpha while disabled
TEST_F(NavigationEditorTest, OverlayDrawsAreaOverrides){
    nav::NavigationSettings settings = nav::NavigationSettings::defaults();
    settings.areas[4].color = Vector4(0.9f, 0.2f, 0.1f, 1.0f);
    nav::setSettings(settings);
    {
        Scene sceneValue;
        Scene* s = &sceneValue;
        Primitive floor(s);
        floor.createPlane(20.0f, 20.0f);
        s->addComponent<NavMeshSurfaceComponent>(floor.getEntity(), NavMeshSurfaceComponent());
        const Entity door = s->createEntity();
        s->addComponent<Transform>(door, {});
        NavMeshModifierComponent modifier;
        modifier.useVolume = true;
        modifier.applyAtRuntime = true;
        modifier.area = 4;
        modifier.volumeCenter = Vector3(0.0f, 0.0f, 0.0f);
        modifier.volumeSize = Vector3(2.0f, 2.0f, 2.0f);
        s->addComponent<NavMeshModifierComponent>(door, modifier);
        NavigationSystem* system = s->getSystem<NavigationSystem>().get();
        system->setKeepBakeData(true);
        ASSERT_TRUE(system->bakeNow(floor.getEntity(), NavBakeMode::FULL).ok);
        system->syncAreaOverrides();

        SceneProject sceneProject;
        sceneProject.id = 1;
        sceneProject.scene = s;
        NavOverlaySettings overlaySettings;
        // no navmesh triangles: no camera is needed
        overlaySettings.showNavMesh = false;
        NavigationOverlay overlay(s);
        overlay.update(sceneProject, nullptr, {}, overlaySettings);
        ASSERT_EQ(overlay.getAreaOverrideDraws().size(), 1u);
        NavigationOverlay::AreaOverrideDraw drawn = overlay.getAreaOverrideDraws()[0];
        EXPECT_EQ(drawn.modifier, door);
        EXPECT_EQ(drawn.lines, 12u);
        EXPECT_FLOAT_EQ(drawn.color.x, 0.9f);
        EXPECT_FLOAT_EQ(drawn.color.y, 0.2f);
        EXPECT_FLOAT_EQ(drawn.color.w, 1.0f);

        // disabled: still drawn, faint
        s->getComponent<NavMeshModifierComponent>(door).enabled = false;
        system->syncAreaOverrides();
        overlay.update(sceneProject, nullptr, {}, overlaySettings);
        ASSERT_EQ(overlay.getAreaOverrideDraws().size(), 1u);
        drawn = overlay.getAreaOverrideDraws()[0];
        EXPECT_EQ(drawn.lines, 12u);
        EXPECT_FLOAT_EQ(drawn.color.w, 0.35f);

        // Show Area Overrides off: none
        overlaySettings.showAreaOverrides = false;
        overlay.update(sceneProject, nullptr, {}, overlaySettings);
        EXPECT_TRUE(overlay.getAreaOverrideDraws().empty());
        overlay.hide();
        sceneProject.scene = nullptr;
    }
    nav::setSettings(nav::NavigationSettings::defaults());
}
