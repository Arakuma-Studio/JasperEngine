// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The spline -> terrain bake (docs/specs/S18-splines.md R11, R12, R20, D8, AC9): a straight road
// flattens a 513^2 16-bit heightmap to its height within one quantization step inside its band
// and leaves every texel past band + falloff as it was; CutOnly never raises, FillOnly never
// lowers; Area fills a closed spline's inside; painting and clearing foliage; on a Project the
// bake is one undo step that restores the maps byte for byte, and Live Terrain re-bakes from the
// snapshot it took. The budgets (R20) are printed, and enforced on the benchmark server.

#include "Project.h"
#include "command/CommandHandle.h"
#include "component/SplineComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "spline/SplineStations.h"
#include "subsystem/SplineSystem.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"
#include "util/TerrainSplineBake.h"
#include "pool/TextureDataPool.h"
#include "terrain/FoliageTypes.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr int MAP = 513;
    constexpr float SIZE = 512.0f;          // one unit per texel spacing
    constexpr float MAX_HEIGHT = 40.0f;

    struct Map16{
        std::vector<unsigned char> pixels;
        TerrainBakeMap view(){
            TerrainBakeMap map;
            map.pixels = pixels.data();
            map.width = MAP;
            map.height = MAP;
            map.channels = 1;
            map.bytesPerChannel = 2;
            return map;
        }
        float at(int x, int y) const{
            const size_t i = (static_cast<size_t>(y) * MAP + static_cast<size_t>(x)) * 2;
            return static_cast<float>(pixels[i] | (pixels[i + 1] << 8)) / 65535.0f;
        }
    };

    Map16 flatMap(float normalized){
        Map16 map;
        map.pixels.resize(static_cast<size_t>(MAP) * MAP * 2);
        const unsigned q = static_cast<unsigned>(std::lround(normalized * 65535.0f));
        for (size_t i = 0; i < map.pixels.size(); i += 2){
            map.pixels[i] = static_cast<unsigned char>(q & 0xFF);
            map.pixels[i + 1] = static_cast<unsigned char>(q >> 8);
        }
        return map;
    }

    TerrainBakeFrame frame(){
        TerrainBakeFrame f;
        f.terrainSize = SIZE;
        f.maxHeight = MAX_HEIGHT;
        return f;
    }

    // the world position of texel (i, j) (texel centres at (i + 0.5) / size)
    Vector3 texelWorld(int i, int j){
        const float texel = SIZE / static_cast<float>(MAP);
        return Vector3((static_cast<float>(i) + 0.5f) * texel - SIZE * 0.5f, 0.0f, (static_cast<float>(j) + 0.5f) * texel - SIZE * 0.5f);
    }

    // a straight road along X at height h, half-width 4
    std::vector<spline::SplineStation> straightRoad(float h, float from, float to){
        spline::SplineData data;
        spline::SplineKnot a;
        a.position = Vector3(from, h, 0.0f);
        a.radius = 4.0f;
        spline::SplineKnot b = a;
        b.position = Vector3(to, h, 0.0f);
        data.setType(spline::SplineType::Linear);
        data.setKnots({a, b});
        std::vector<spline::SplineStation> stations;
        spline::SplineStationOptions options;
        spline::buildStations(data, options, stations);
        std::vector<spline::SplineStation> route;
        TerrainSplineBake::routeStations(stations, SIZE / MAP, route);
        return route;
    }

    SplineTerrainSettings roadSettings(){
        SplineTerrainSettings settings;
        settings.extraWidth = 0.0f;
        settings.falloff = 2.0f;
        return settings;
    }

}

TEST(TerrainSplineBakeTests, RouteIsResampledToHalfATexel){
    const std::vector<spline::SplineStation> route = straightRoad(10.0f, -100.0f, 100.0f);
    ASSERT_GT(route.size(), 2u);
    for (size_t i = 1; i < route.size(); i++){
        EXPECT_LE(route[i].distance - route[i - 1].distance, SIZE / MAP * 0.5f + 1.0e-4f);
    }
}

TEST(TerrainSplineBakeTests, FlattenHoldsTheBandAndLeavesTheRest){
    const float h = 12.0f;
    Map16 map = flatMap(0.25f);           // 10 units
    const Map16 before = map;
    TerrainBakeMap view = map.view();
    const TerrainMapRegion changed = TerrainSplineBake::bakeHeights(straightRoad(h, -150.0f, 150.0f), {}, roadSettings(), frame(), view);
    ASSERT_FALSE(changed.empty());
    const float step = 1.0f / 65535.0f;
    int inBand = 0;
    for (int j = 0; j < MAP; j++){
        for (int i = 0; i < MAP; i++){
            const Vector3 w = texelWorld(i, j);
            if (w.x < -150.0f || w.x > 150.0f){
                continue;
            }
            const float d = std::fabs(w.z);
            if (d <= 4.0f){
                inBand++;
                EXPECT_NEAR(map.at(i, j) * MAX_HEIGHT, h, step * MAX_HEIGHT * 1.01f) << i << ", " << j;
            }else if (d > 6.0f){
                EXPECT_EQ(map.at(i, j), before.at(i, j)) << i << ", " << j;
            }
        }
    }
    EXPECT_GT(inBand, 300 * 8);
    // past the road's ends nothing within the band changed either beyond its round cap
    EXPECT_EQ(map.at(0, MAP / 2), before.at(0, MAP / 2));
}

TEST(TerrainSplineBakeTests, CutOnlyNeverRaisesAndFillOnlyNeverLowers){
    // a road above the ground: CutOnly leaves it, FillOnly raises to it
    SplineTerrainSettings settings = roadSettings();
    settings.flattenMode = SplineFlattenMode::CutOnly;
    Map16 map = flatMap(0.25f);
    const Map16 before = map;
    TerrainBakeMap view = map.view();
    TerrainSplineBake::bakeHeights(straightRoad(20.0f, -100.0f, 100.0f), {}, settings, frame(), view);
    for (size_t i = 0; i < map.pixels.size(); i++){
        ASSERT_EQ(map.pixels[i], before.pixels[i]);
    }
    // a road under the ground: CutOnly lowers, never raises anywhere
    TerrainSplineBake::bakeHeights(straightRoad(4.0f, -100.0f, 100.0f), {}, settings, frame(), view);
    bool lowered = false;
    for (int j = 0; j < MAP; j++){
        for (int i = 0; i < MAP; i++){
            ASSERT_LE(map.at(i, j), before.at(i, j));
            lowered = lowered || map.at(i, j) < before.at(i, j);
        }
    }
    EXPECT_TRUE(lowered);

    settings.flattenMode = SplineFlattenMode::FillOnly;
    Map16 fill = flatMap(0.25f);
    TerrainBakeMap fillView = fill.view();
    TerrainSplineBake::bakeHeights(straightRoad(4.0f, -100.0f, 100.0f), {}, settings, frame(), fillView);
    for (size_t i = 0; i < fill.pixels.size(); i++){
        ASSERT_EQ(fill.pixels[i], before.pixels[i]);
    }
}

TEST(TerrainSplineBakeTests, AreaFillsTheInsideOfAClosedSpline){
    // a 40 x 40 square outline at height 6
    spline::SplineData data;
    data.setType(spline::SplineType::Linear);
    std::vector<spline::SplineKnot> knots;
    for (Vector3 p : {Vector3(-20, 6, -20), Vector3(20, 6, -20), Vector3(20, 6, 20), Vector3(-20, 6, 20)}){
        spline::SplineKnot k;
        k.position = p;
        k.radius = 0.5f;
        knots.push_back(k);
    }
    data.setKnots(knots);
    data.setClosed(true);
    std::vector<spline::SplineStation> stations;
    spline::SplineStationOptions options;
    spline::buildStations(data, options, stations);
    std::vector<spline::SplineStation> route;
    TerrainSplineBake::routeStations(stations, SIZE / MAP, route);
    const std::vector<Vector3> outline = {Vector3(-20, 6, -20), Vector3(20, 6, -20), Vector3(20, 6, 20), Vector3(-20, 6, 20)};
    SplineTerrainSettings settings = roadSettings();
    settings.region = SplineTerrainRegion::Area;
    Map16 map = flatMap(0.25f);
    TerrainBakeMap view = map.view();
    TerrainSplineBake::bakeHeights(route, outline, settings, frame(), view);
    // the centre, far from the outline, is flattened to the spline's height
    EXPECT_NEAR(map.at(MAP / 2, MAP / 2) * MAX_HEIGHT, 6.0f, 2.0f / 65535.0f * MAX_HEIGHT);
    // well outside, untouched
    EXPECT_NEAR(map.at(MAP / 2 + 40, MAP / 2) * MAX_HEIGHT, 10.0f, 2.0f / 65535.0f * MAX_HEIGHT);
}

TEST(TerrainSplineBakeTests, PaintAndClearFoliageFollowTheBand){
    std::vector<unsigned char> blend(static_cast<size_t>(MAP) * MAP * 4, 0);
    for (size_t i = 0; i < blend.size(); i += 4){
        blend[i] = 255;     // layer 0 everywhere
        blend[i + 3] = 255;
    }
    TerrainBakeMap blendView;
    blendView.pixels = blend.data();
    blendView.width = MAP;
    blendView.height = MAP;
    blendView.channels = 4;
    SplineTerrainSettings settings = roadSettings();
    settings.paintStrength = 1.0f;
    settings.paintFalloff = 1.0f;
    const TerrainMapRegion painted = TerrainSplineBake::bakeBlend(straightRoad(0.0f, -50.0f, 50.0f), {}, settings, frame(), 2, blendView);
    ASSERT_FALSE(painted.empty());
    const size_t centre = (static_cast<size_t>(MAP / 2) * MAP + MAP / 2) * 4;
    EXPECT_EQ(blend[centre + 2], 255);
    EXPECT_EQ(blend[centre + 0], 0);
    const size_t far = (static_cast<size_t>(MAP / 2 + 20) * MAP + MAP / 2) * 4;
    EXPECT_EQ(blend[far + 0], 255);

    std::vector<unsigned char> density(static_cast<size_t>(MAP) * MAP, 200);
    TerrainBakeMap densityView;
    densityView.pixels = density.data();
    densityView.width = MAP;
    densityView.height = MAP;
    densityView.channels = 1;
    TerrainSplineBake::bakeDensity(straightRoad(0.0f, -50.0f, 50.0f), {}, settings, frame(), densityView);
    EXPECT_EQ(density[static_cast<size_t>(MAP / 2) * MAP + MAP / 2], 0);
    EXPECT_EQ(density[static_cast<size_t>(MAP / 2 + 20) * MAP + MAP / 2], 200);
}

// R20: a 1 km road on a 1025^2 heightmap plus one blend map <= 150 ms; a 50 m drag's re-bake <= 16 ms
TEST(TerrainSplineBakeTests, BakeBudgets){
    constexpr int N = 1025;
    std::vector<unsigned char> heights(static_cast<size_t>(N) * N * 2, 0);
    std::vector<unsigned char> blend(static_cast<size_t>(N) * N * 4, 0);
    TerrainBakeMap heightView;
    heightView.pixels = heights.data();
    heightView.width = N;
    heightView.height = N;
    heightView.channels = 1;
    heightView.bytesPerChannel = 2;
    TerrainBakeMap blendView;
    blendView.pixels = blend.data();
    blendView.width = N;
    blendView.height = N;
    blendView.channels = 4;
    TerrainBakeFrame f;
    f.terrainSize = 1024.0f;
    f.maxHeight = 50.0f;
    SplineTerrainSettings settings;
    settings.falloff = 3.0f;
    settings.paint = true;

    // a 1 km meander across the map
    spline::SplineData data;
    std::vector<spline::SplineKnot> knots;
    for (int i = 0; i < 11; i++){
        spline::SplineKnot k;
        const float x = -500.0f + 100.0f * static_cast<float>(i);
        k.position = Vector3(x, 10.0f, 40.0f * std::sin(x * 0.01f));
        k.radius = 4.0f;
        knots.push_back(k);
    }
    data.setKnots(knots);
    std::vector<spline::SplineStation> stations;
    spline::SplineStationOptions options;
    auto bake = [&](){
        const auto start = std::chrono::steady_clock::now();
        spline::buildStations(data, options, stations);
        std::vector<spline::SplineStation> route;
        TerrainSplineBake::routeStations(stations, f.terrainSize / N, route);
        TerrainSplineBake::bakeHeights(route, {}, settings, f, heightView);
        TerrainSplineBake::bakeBlend(route, {}, settings, f, 0, blendView);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    std::vector<double> full;
    for (int r = 0; r < 5; r++){
        full.push_back(bake());
    }
    std::sort(full.begin(), full.end());
    std::printf("[bench] spline terrain bake 1 km on 1025^2 + 1 blend map: %.2f ms (budget 150 ms)\n", full[2]);

    // the live session's re-bake of a 50 m drag: the stretch of route the drag touched
    spline::SplineData drag;
    drag.setKnots({knots[4], knots[5]});
    std::vector<double> partial;
    for (int r = 0; r < 9; r++){
        const auto start = std::chrono::steady_clock::now();
        spline::buildStations(drag, options, stations);
        std::vector<spline::SplineStation> route;
        TerrainSplineBake::routeStations(stations, f.terrainSize / N, route);
        // 50 m of the 100 m segment
        route.resize(route.size() / 2);
        TerrainSplineBake::bakeHeights(route, {}, settings, f, heightView);
        partial.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(partial.begin(), partial.end());
    std::printf("[bench] spline live re-bake of a 50 m drag: %.2f ms (budget 16 ms)\n", partial[4]);
    const double slack = jasperengine::editor::test::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(full[2], 150.0 * slack);
        EXPECT_LE(partial[4], 16.0 * slack);
    }
}

namespace {

    // a Project (the prefab-runtime fixture) with a 129^2 16-bit terrain and a road over it
    class TerrainSplineBakeProjectTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"spline-bake"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        Entity terrain = NULL_ENTITY;
        Entity road = NULL_ENTITY;
        // the heightmap's pixels: the TextureData made from memory points at them
        std::vector<unsigned char> heightData;
        static constexpr int N = 129;

        void SetUp() override{
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()){
                if (!candidate.filepath.empty() && !candidate.scene){
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (SceneProject& candidate : project->getScenes()){
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            Scene* s = scene->scene;

            terrain = s->createEntity();
            s->addComponent<Transform>(terrain, {});
            TerrainComponent t;
            t.terrainSize = 128.0f;
            t.maxHeight = 20.0f;
            s->addComponent<TerrainComponent>(terrain, t);
            std::vector<unsigned char>& pixels = heightData;
            pixels.assign(static_cast<size_t>(N) * N * 2, 0);
            for (size_t i = 0; i < pixels.size(); i += 2){
                const unsigned q = 16384u + static_cast<unsigned>((i / 2) % 97);
                pixels[i] = static_cast<unsigned char>(q & 0xFF);
                pixels[i + 1] = static_cast<unsigned char>(q >> 8);
            }
            TerrainComponent& component = s->getComponent<TerrainComponent>(terrain);
            component.heightMap.setData("spline-bake-height", TextureData(N, N, pixels.size(), ColorFormat::RED16, 1, pixels.data()));
            component.heightMap.setReleaseDataAfterLoad(false);

            road = s->createEntity();
            s->addComponent<Transform>(road, {});
            SplineComponent spline;
            spline::SplineKnot a;
            a.position = Vector3(-40.0f, 3.0f, 0.0f);
            a.radius = 3.0f;
            spline::SplineKnot b = a;
            b.position = Vector3(40.0f, 3.0f, 10.0f);
            spline.spline.setKnots({a, b});
            spline.terrain.terrain = terrain;
            s->addComponent<SplineComponent>(road, spline);
        }

        void TearDown() override{
            if (project){
                for (SceneProject& candidate : project->getScenes()){
                    CommandHandle::remove(candidate.id);
                }
                project->releaseScenes();
            }
            project.reset();
            TextureDataPool::remove("spline-bake-height");
        }

        std::vector<unsigned char> heightPixels(){
            Texture& map = scene->scene->getComponent<TerrainComponent>(terrain).heightMap;
            EXPECT_TRUE(TerrainMapUtils::hasLoadedData(map));
            TextureData& data = map.getData();
            const unsigned char* p = static_cast<const unsigned char*>(data.getData());
            return std::vector<unsigned char>(p, p + data.getSize());
        }
    };

}

TEST_F(TerrainSplineBakeProjectTest, ApplyIsOneUndoStepThatRestoresTheMapsExactly){
    const std::vector<unsigned char> before = heightPixels();
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t position = history->position();
    TerrainBakeChanges changes;
    std::string error;
    ASSERT_TRUE(TerrainSplineBake::apply(project.get(), scene->id, road, changes, error)) << error;
    EXPECT_FALSE(changes.heights.empty());
    EXPECT_EQ(history->position(), position + 1) << "one undo step";
    ASSERT_NE(history->top(), nullptr);
    EXPECT_NE(history->top()->name().find("Apply Spline to Terrain"), std::string::npos) << history->top()->name();
    EXPECT_NE(heightPixels(), before);
    history->undo();
    EXPECT_EQ(heightPixels(), before);
    history->redo();
    EXPECT_NE(heightPixels(), before);
}

TEST_F(TerrainSplineBakeProjectTest, LiveSessionBakesFromItsSnapshotAndCommitsOnce){
    const std::vector<unsigned char> before = heightPixels();
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t position = history->position();
    TerrainSplineLiveSession session;
    std::string error;
    ASSERT_TRUE(session.begin(project.get(), scene->id, road, error)) << error;
    const std::vector<unsigned char> first = heightPixels();
    EXPECT_NE(first, before);

    // the road moves away: its old bed goes back to the snapshot
    SplineComponent& spline = scene->scene->getComponent<SplineComponent>(road);
    spline::SplineKnot a = spline.spline.knot(0);
    spline::SplineKnot b = spline.spline.knot(1);
    a.position.z += 30.0f;
    b.position.z += 30.0f;
    spline.spline.setKnots({a, b});
    EXPECT_TRUE(session.update(error)) << error;
    // baking the moved road alone into the original map gives the same pixels
    std::vector<unsigned char> expected = before;
    {
        const std::vector<unsigned char> live = heightPixels();
        session.end();
        EXPECT_EQ(history->position(), position + 1) << "one grouped patch";
        history->undo();
        EXPECT_EQ(heightPixels(), before);
        TerrainBakeChanges changes;
        ASSERT_TRUE(TerrainSplineBake::bakeInPlace(project.get(), scene->id, road, terrain, changes, error)) << error;
        expected = heightPixels();
        EXPECT_EQ(live, expected);
    }
}

// T5 AC23: a bake with Clear Trees removes what its footprint covers, as one undo step with the maps
TEST(TerrainSplineBakeTests, ClearTreesRemovesFootprint){
    std::vector<terraincore::TreeRecord> records;
    for (int i = 0; i < 40; i++){
        terraincore::TreeRecord record{};
        record.u = 0.5f + (static_cast<float>(i) - 20.0f) / 64.0f;
        record.v = 0.5f;
        record.scale = 1.0f;
        records.push_back(record);
    }
    terraincore::TreeStore store;
    store.apply({}, records, {}, nullptr);
    std::vector<uint32_t> removed = TerrainSplineBake::treesInFootprint(store, SIZE, {Vector2(0.0f, -100.0f), Vector2(0.0f, 100.0f)}, 10.0f);
    ASSERT_FALSE(removed.empty());
    for (uint32_t id : removed){
        const terraincore::TreeRecord* record = store.find(id);
        ASSERT_NE(record, nullptr);
        EXPECT_LE(std::fabs((record->u - 0.5f) * SIZE), 10.0f);
    }
    // outside it nothing goes
    EXPECT_LT(removed.size(), records.size());
    EXPECT_TRUE(TerrainSplineBake::treesInFootprint(store, SIZE, {Vector2(240.0f, -100.0f), Vector2(240.0f, 100.0f)}, 1.0f).empty());
}
