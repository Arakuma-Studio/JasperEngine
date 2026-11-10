// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S18's water fill on a Terrain World (T4 R26, AC38): a river across a tile border floods the world's
// own sample lattice, read on the spline's job from tiles not in memory, exactly as it floods one
// Terrain map holding the same samples at the same places.

#include "TerrainWorldTestSupport.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "object/Terrain.h"
#include "subsystem/SplineSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "texture/TextureData.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <thread>

using namespace terrainworldtest;
using namespace jasperengine;

namespace {

    constexpr float RIVER_MAX_HEIGHT = 20.0f;

    // a valley winding along z = 6 sin(x / 15), falling downstream, at world sample (sx, sz) = (x, z) m
    uint16_t riverValley(int sx, int sz) {
        const double x = sx, z = sz;
        const double d = (z - 6.0 * std::sin(x / 15.0)) / 18.0;
        const double value = 0.28 + 0.42 * std::fmin(d * d, 1.0) + 0.002 * x;
        return static_cast<uint16_t>(std::lround(std::fmin(std::fmax(value, 0.0), 1.0) * 65535.0));
    }

    Entity riverSpline(Scene& scene, Entity ground) {
        std::vector<spline::SplineKnot> knots;
        for (int i = 0; i <= 8; i++) {
            const float x = -50.0f + 12.5f * static_cast<float>(i);
            spline::SplineKnot knot;
            knot.position = Vector3(x, RIVER_MAX_HEIGHT * (0.36f + 0.002f * x), 6.0f * std::sin(x / 15.0f));
            knot.radius = 6.0f;
            knots.push_back(knot);
        }
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        SplineComponent spline;
        spline.spline.setKnots(knots);
        spline.terrain.terrain = ground;
        scene.addComponent<SplineComponent>(entity, spline);
        MeshComponent mesh;
        mesh.numSubmeshes = 1;
        scene.addComponent<MeshComponent>(entity, mesh);
        SplineExtrudeComponent extrude;
        extrude.shape = SplineExtrudeShape::WaterBanks;
        extrude.maxHalfWidth = 14.0f;
        scene.addComponent<SplineExtrudeComponent>(entity, extrude);
        return entity;
    }

    struct RiverFlood {
        SplineBuildStats stats;
        AABB bounds;
        std::vector<SplineWaterSample> samples;
        bool noTerrain = false;
    };

    RiverFlood floodOf(Scene& scene, Entity owner) {
        SplineSystem& system = *scene.getSystem<SplineSystem>();
        system.rebuildNow(owner);
        RiverFlood out;
        out.stats = system.getExtrudeStats(owner);
        out.bounds = system.getGeneratedBounds(owner);
        for (const SplineWarning& warning : system.getExtrudeWarnings(owner)) out.noTerrain = out.noTerrain || warning.kind == SplineWarningKind::NoTerrain;
        for (int i = 0; i <= 40; i++) {
            const float x = -45.0f + 2.25f * static_cast<float>(i);
            for (float dz : {-4.0f, 0.0f, 3.0f}) out.samples.push_back(system.sampleWaterSurface(x, 6.0f * std::sin(x / 15.0f) + dz, owner));
        }
        return out;
    }

}

TEST(TerrainWorldSplines, WaterFillAcrossBorderMatchesOneMap) {
    // 2 x 2 tiles of 64 m round the origin: the river crosses the border at x = 0
    jasperengine::test::TempDir files;
    const WorldLattice lattice = smallLattice(64, 1.0f);
    ASSERT_TRUE(writePlayerWorld(files.path() / "River.terrainworld", lattice, RIVER_MAX_HEIGHT, -1, 0, riverValley));
    Scene worldScene;
    const Entity world = worldScene.createEntity();
    worldScene.addComponent<Transform>(world, {});
    worldScene.addComponent<MeshComponent>(world, {});
    worldScene.addComponent<TerrainComponent>(world, {});
    TerrainWorldComponent component;
    component.worldData.path = (files.path() / "River.terrainworld").generic_string();
    component.useCameraFocus = false;
    worldScene.addComponent<TerrainWorldComponent>(world, component);
    std::string error;
    TerrainWorldRuntime* runtime = worldScene.getSystem<TerrainWorldSystem>()->ensureRuntime(world, &error);
    ASSERT_NE(runtime, nullptr) << error;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!runtime->isReady() && std::chrono::steady_clock::now() < until) {
        worldScene.getSystem<TerrainWorldSystem>()->pump(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(runtime->isReady());
    // no focus: every tile is on disk, so a main-thread game query would answer from the overview
    for (int z = -1; z <= 0; z++) {
        for (int x = -1; x <= 0; x++) ASSERT_EQ(runtime->resident(TileCoord(x, z)), nullptr);
    }
    const RiverFlood onWorld = floodOf(worldScene, riverSpline(worldScene, world));

    // one Terrain of 129 texels over 129 m: texel i at (i + 0.5) - 64.5 = i - 64, the world's sample i - 64
    Scene mapScene;
    Terrain terrain(&mapScene);
    terrain.setSize(129.0f);
    terrain.setMaxHeight(RIVER_MAX_HEIGHT);
    std::vector<unsigned char> pixels(129u * 129u * 2u);
    for (int j = 0; j < 129; j++) {
        for (int i = 0; i < 129; i++) {
            const uint16_t value = riverValley(i - 64, j - 64);
            pixels[(static_cast<size_t>(j) * 129 + i) * 2] = static_cast<unsigned char>(value & 0xFF);
            pixels[(static_cast<size_t>(j) * 129 + i) * 2 + 1] = static_cast<unsigned char>(value >> 8);
        }
    }
    TerrainComponent& map = mapScene.getComponent<TerrainComponent>(terrain.getEntity());
    map.heightMap.setData("terrain-world-splines-height", TextureData(129, 129, static_cast<unsigned int>(pixels.size()), ColorFormat::RED16, 1, pixels.data()));
    map.heightMap.setReleaseDataAfterLoad(false);
    const RiverFlood onMap = floodOf(mapScene, riverSpline(mapScene, terrain.getEntity()));

    ASSERT_FALSE(onMap.noTerrain);
    ASSERT_GT(onMap.stats.vertices, 0u);
    EXPECT_FALSE(onWorld.noTerrain) << "WaterBanks found the World's ground";
    EXPECT_EQ(onWorld.stats.vertices, onMap.stats.vertices);
    EXPECT_EQ(onWorld.stats.triangles, onMap.stats.triangles);
    for (const auto& [a, b] : {std::make_pair(onWorld.bounds.getMinimum(), onMap.bounds.getMinimum()),
                               std::make_pair(onWorld.bounds.getMaximum(), onMap.bounds.getMaximum())}) {
        EXPECT_NEAR(a.x, b.x, 1e-3f);
        EXPECT_NEAR(a.y, b.y, 1e-3f);
        EXPECT_NEAR(a.z, b.z, 1e-3f);
    }
    ASSERT_EQ(onWorld.samples.size(), onMap.samples.size());
    int wet = 0;
    for (size_t i = 0; i < onMap.samples.size(); i++) {
        EXPECT_EQ(onWorld.samples[i].hit, onMap.samples[i].hit) << i;
        if (!onMap.samples[i].hit) continue;
        wet++;
        EXPECT_NEAR(onWorld.samples[i].height, onMap.samples[i].height, 1e-3f) << i;
    }
    EXPECT_GT(wet, 40);
}
