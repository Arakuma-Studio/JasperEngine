// SPDX-License-Identifier: MIT
//
// The scene's own (play-mode) 3D physics over the shared Jolt shape builders
// (engine/core/physics/JoltShapeBuilder): every collider type and source settles where it
// should, so moving the builders out of PhysicsSystem changed nothing.

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#include "terrain/TerrainHoles.h"

#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "PhysicsTestScene.h"
#include "math/Ray.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"

#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::physicstest;

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    void runScene(TestScene& t, int steps) {
        t.resolve();
        PhysicsSystem* physics = t.scene.getSystem<PhysicsSystem>().get();
        physics->load();
        for (int i = 0; i < steps; i++) {
            physics->fixedUpdate(1.0 / 60.0);
            t.resolve();
        }
    }

    void print(const char* name, TestScene& t, Entity e) {
        const Transform& tr = t.transform(e);
        std::printf("[scene-physics] %s p=(%.9g, %.9g, %.9g) q=(%.9g, %.9g, %.9g, %.9g)\n", name,
                    tr.position.x, tr.position.y, tr.position.z, tr.rotation.w, tr.rotation.x, tr.rotation.y, tr.rotation.z);
    }

}

// Primitive colliders: a 1 m box on a static box floor rests on its face
TEST(ScenePhysics, BoxOnBoxFloor) {
    TestScene t;
    const Entity floor = t.box(Vector3(10, 1, 10), Vector3(0, -0.5f, 0));
    Collider3D& floorCollider = t.collider(floor, Collider3DType::BOX);
    floorCollider.width = 10; floorCollider.height = 1; floorCollider.depth = 10;

    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0.2f, 2.0f, -0.1f));
    Collider3D& crateCollider = t.collider(crate, Collider3DType::BOX, BodyType::DYNAMIC);
    crateCollider.width = 1; crateCollider.height = 1; crateCollider.depth = 1;

    runScene(t, 240);
    print("box-on-box", t, crate);
    EXPECT_NEAR(t.transform(crate).position.y, 0.5f, 0.03f);
    EXPECT_NEAR(t.transform(crate).position.x, 0.2f, 1e-3f);
}

// A convex hull with Source=None (falls back to the entity's own mesh) on a triangle-mesh floor
// read from a scaled child entity's mesh
TEST(ScenePhysics, HullOnChildMeshFloor) {
    TestScene t;
    const Entity ground = t.entity(Vector3(0, 0, 0));
    const Entity floorMesh = t.box(Vector3(1, 1, 1), Vector3(0, -0.5f, 0), ground);
    t.setScale(floorMesh, Vector3(10, 1, 10));
    Collider3D& floorCollider = t.collider(ground, Collider3DType::MESH);
    floorCollider.source = Collider3DSource::ENTITY_MESH;
    floorCollider.sourceEntity = floorMesh;

    const Entity crate = t.box(Vector3(1, 0.5f, 1), Vector3(0, 1.5f, 0));
    t.setRotation(crate, axisAngle(Vector3(0, 1, 0), 0.3f));
    t.collider(crate, Collider3DType::CONVEX_HULL, BodyType::DYNAMIC);

    runScene(t, 240);
    print("hull-on-mesh", t, crate);
    EXPECT_NEAR(t.transform(crate).position.y, 0.25f, 0.03f);
}

// Raw vertices (scaled by the entity) on a raw triangle mesh
TEST(ScenePhysics, RawHullOnRawMesh) {
    TestScene t;
    const Entity ground = t.entity(Vector3(0, 0, 0));
    Collider3D& mesh = t.collider(ground, Collider3DType::MESH);
    mesh.source = Collider3DSource::RAW_MESH;
    const Vector3 corners[4] = {Vector3(-5, 0, -5), Vector3(5, 0, -5), Vector3(5, 0, 5), Vector3(-5, 0, 5)};
    for (const Vector3& c : corners) mesh.vertices.push_back(c);
    const uint16_t tris[6] = {0, 2, 1, 0, 3, 2};
    for (uint16_t i : tris) mesh.indices.push_back(i);

    const Entity body = t.entity(Vector3(0, 1, 0));
    t.setScale(body, Vector3(2, 1, 2));
    Collider3D& hull = t.collider(body, Collider3DType::CONVEX_HULL, BodyType::DYNAMIC);
    hull.source = Collider3DSource::RAW_VERTICES;
    for (int i = 0; i < 8; i++) {
        hull.vertices.push_back(Vector3((i & 1) ? 0.25f : -0.25f, (i & 2) ? 0.25f : -0.25f, (i & 4) ? 0.25f : -0.25f));
    }

    runScene(t, 240);
    print("raw-hull-on-raw-mesh", t, body);
    EXPECT_NEAR(t.transform(body).position.y, 0.25f, 0.03f);
}

// Plates and discs thinner than Jolt's 5 cm convex radius build (they used to fail): a thin
// cylinder coin lands on a 6 cm box plate
TEST(ScenePhysics, ThinBoxAndCylinder) {
    TestScene t;
    const Entity plate = t.entity(Vector3(0, -0.03f, 0));
    Collider3D& plateCollider = t.collider(plate, Collider3DType::BOX);
    plateCollider.width = 4; plateCollider.height = 0.06f; plateCollider.depth = 4;

    const Entity coin = t.entity(Vector3(0.1f, 1.0f, 0));
    Collider3D& coinCollider = t.collider(coin, Collider3DType::CYLINDER, BodyType::DYNAMIC);
    coinCollider.radius = 0.3f; coinCollider.halfHeight = 0.02f;

    runScene(t, 240);
    print("coin-on-plate", t, coin);
    EXPECT_NEAR(t.transform(coin).position.y, 0.02f, 0.03f);
    EXPECT_GT(t.transform(coin).position.y, -0.01f);   // on the plate (Jolt's 2 cm slop), not through it
}

// A mirrored (negative scale) triangle-mesh floor still faces up: its winding is flipped back
TEST(ScenePhysics, MirroredMeshFloor) {
    TestScene t;
    const Entity floor = t.box(Vector3(10, 1, 10), Vector3(0, -0.5f, 0));
    t.setScale(floor, Vector3(-1, 1, 1));
    t.collider(floor, Collider3DType::MESH).source = Collider3DSource::ENTITY_MESH;
    const Entity raw = t.entity(Vector3(20, 0, 0));
    t.setScale(raw, Vector3(1, 1, -1));
    Collider3D& mesh = t.collider(raw, Collider3DType::MESH);
    mesh.source = Collider3DSource::RAW_MESH;
    const Vector3 corners[4] = {Vector3(-5, 0, -5), Vector3(5, 0, -5), Vector3(5, 0, 5), Vector3(-5, 0, 5)};
    for (const Vector3& c : corners) mesh.vertices.push_back(c);
    const uint16_t tris[6] = {0, 2, 1, 0, 3, 2};
    for (uint16_t i : tris) mesh.indices.push_back(i);

    std::vector<Entity> crates;
    for (const Vector3& at : {Vector3(0.5f, 2, 0.3f), Vector3(20.5f, 2, 0.3f)}) {
        crates.push_back(t.box(Vector3(1, 1, 1), at));
        Collider3D& c = t.collider(crates.back(), Collider3DType::BOX, BodyType::DYNAMIC);
        c.width = 1; c.height = 1; c.depth = 1;
    }
    runScene(t, 240);
    for (Entity crate : crates) EXPECT_NEAR(t.transform(crate).position.y, 0.5f, 0.03f) << crate;
}

// A box falls on a flat-topped terrain heightfield
TEST(ScenePhysics, BoxOnHeightfield) {
    TestScene t;
    const Entity terrain = t.terrain(64, 40.0f, 4.0f, Vector3(0, 0, 0), [](float, float) { return 0.5f; });
    t.collider(terrain, Collider3DType::HEIGHTFIELD);

    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(1.0f, 4.0f, 2.0f));
    Collider3D& c = t.collider(crate, Collider3DType::BOX, BodyType::DYNAMIC);
    c.width = 1; c.height = 1; c.depth = 1;

    runScene(t, 240);
    print("box-on-heightfield", t, crate);
    EXPECT_NEAR(t.transform(crate).position.y, 2.0f + 0.5f, 0.05f);
}

// The heightfield collider is the rendered surface on a moved, scaled terrain (an odd-sized
// 16-bit heightmap). At the collider's samples (one per texel, edge to edge) a ray cast down
// lands on MeshSystem::sampleTerrainSurface's height (the shader's texel-centre lookup) to
// Jolt's 8-bit-per-block height quantisation; anywhere else, within the resampling between the
// collider's grid and the texel-centre grid. It used to sit about a cell short, with no
// last-texel row and no entity scale.
TEST(ScenePhysics, HeightfieldMatchesTerrainSurface) {
    TestScene t;
    const float terrainSize = 40.0f;
    const float maxHeight = 6.0f;
    const Vector3 origin(3.0f, -1.0f, -2.0f);
    const Vector3 scale(1.5f, 0.75f, 1.25f);
    const Entity terrain = t.terrain(33, terrainSize, maxHeight, origin, [](float u, float v) {
        return 0.4f + 0.12f * std::sin(4.0f * u) + 0.1f * v * v;
    }, true);
    t.setScale(terrain, scale);
    t.collider(terrain, Collider3DType::HEIGHTFIELD);
    runScene(t, 0);

    TerrainComponent& component = t.scene.getComponent<TerrainComponent>(terrain);
    MeshSystem* meshSystem = t.scene.getSystem<MeshSystem>().get();
    // the collider's sample grid: one sample per texel of the 33-wide map, edge to edge
    const int samples = 33;
    const float spacing = terrainSize / float(samples - 1);
    float maxError = 0.0f;
    int hits = 0;
    for (int i = 0; i < 40; i++) {
        // 20 points on the collider's samples (edges included), then 20 anywhere
        const bool onSample = i < 20;
        const float localX = onSample ? -terrainSize * 0.5f + spacing * float((i * 7) % samples)
                                      : -terrainSize * 0.5f + terrainSize * std::fmod(0.05f + i * 0.618034f, 1.0f);
        const float localZ = onSample ? -terrainSize * 0.5f + spacing * float((i * 13 + (i == 19 ? 7 : 0)) % samples)
                                      : -terrainSize * 0.5f + terrainSize * ((i - 20) / 19.0f);
        float height = 0.0f;
        Vector3 normal;
        meshSystem->sampleTerrainSurface(component, localX, localZ, height, normal);
        const Vector3 world = origin + Vector3(localX * scale.x, height * scale.y, localZ * scale.z);

        const float inX = std::fmin(std::fmax(world.x, origin.x - terrainSize * 0.5f * scale.x + 1e-3f), origin.x + terrainSize * 0.5f * scale.x - 1e-3f);
        const float inZ = std::fmin(std::fmax(world.z, origin.z - terrainSize * 0.5f * scale.z + 1e-3f), origin.z + terrainSize * 0.5f * scale.z - 1e-3f);
        Ray ray(Vector3(inX, 20.0f, inZ), Vector3(0, -40.0f, 0));
        RayReturn hit = ray.intersects(&t.scene, RayFilter::BODY_3D);
        ASSERT_TRUE(hit.hit) << "point " << i;
        hits++;
        maxError = std::fmax(maxError, std::fabs(hit.point.y - world.y));
        EXPECT_NEAR(hit.point.y, world.y, onSample ? 2e-3f : 1e-2f) << "point " << i << " local (" << localX << ", " << localZ << ")";
    }
    EXPECT_EQ(hits, 40);

    // mirrored along x, the collider mirrors with the rendered surface
    t.setScale(terrain, Vector3(-scale.x, scale.y, scale.z));
    t.scene.getComponent<RigidbodyComponent>(terrain).needReloadBody = true;
    runScene(t, 1);
    for (int i = 0; i < 20; i++) {
        // on the collider's samples, across the terrain
        const float localX = -terrainSize * 0.5f + spacing * float(2 + i * 3 / 2);
        const float localZ = -terrainSize * 0.5f + spacing * 18.0f;
        float height = 0.0f;
        Vector3 normal;
        meshSystem->sampleTerrainSurface(component, localX, localZ, height, normal);
        const Vector3 world = origin + Vector3(-localX * scale.x, height * scale.y, localZ * scale.z);
        Ray ray(Vector3(world.x, 20.0f, world.z), Vector3(0, -40.0f, 0));
        RayReturn hit = ray.intersects(&t.scene, RayFilter::BODY_3D);
        ASSERT_TRUE(hit.hit) << "mirrored point " << i;
        EXPECT_NEAR(hit.point.y, world.y, 2e-3f) << "mirrored point " << i;
    }
    std::printf("[scene-physics] heightfield max error %.6f m\n", maxError);
}

#endif

// ---------------------------------------------------------------- T3 R22, R24

namespace {

    // A square map of side texels, its [x0, x1) x [y0, y1) cut, on a terrain whose lattice has one
    // sample per corner of those texels when samples is side + 1
    struct HoleFixture {
        std::vector<uint8_t> texels;
        int side = 0;

        void cut(TestScene& t, Entity terrain, int mapSide, int x0, int y0, int x1, int y1, const std::string& id) {
            side = mapSide;
            texels.assign(static_cast<size_t>(side) * side, terrain::SOLID_VALUE);
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    texels[static_cast<size_t>(y) * side + x] = terrain::HOLE_VALUE;
                }
            }
            t.scene.getComponent<TerrainComponent>(terrain).holeMap.setData(
                id, TextureData(side, side, static_cast<unsigned int>(texels.size()), ColorFormat::RED, 1, texels.data()));
        }
    };

    const JPH::HeightFieldShape& heightField(const JPH::ShapeRefC& shape) {
        return *static_cast<const JPH::HeightFieldShape*>(shape.GetPtr());
    }

}

TEST(JoltShapeBuilder, HoleLatticeRule) {
    // 8 squares a side over 8 texels, so a texel is a square. Jolt pads the sample count to its block
    // size, so what counts is that the samples asked for answer as the rule says.
    TestScene t;
    const Entity terrain = t.terrain(9, 8.0f, 2.0f, Vector3(0, 0, 0), [](float, float) { return 0.5f; });
    TerrainComponent& component = t.scene.getComponent<TerrainComponent>(terrain);
    MeshSystem* meshSystem = t.scene.getSystem<MeshSystem>().get();
    (void)meshSystem;
    constexpr unsigned int SAMPLES = 9;
    std::string error;
    HoleFixture fixture;

    struct Case {
        int x0, y0, x1, y1;
        std::vector<std::pair<int, int>> gaps;
        const char* name;
    };
    const std::vector<Case> cases = {
        {3, 3, 4, 4, {}, "1x1: no interior sample"},
        {3, 3, 5, 5, {{4, 4}}, "2x2: one sample"},
        {2, 2, 5, 5, {{3, 3}, {4, 3}, {3, 4}, {4, 4}}, "3x3: its four inner samples"},
        {0, 3, 2, 5, {{0, 4}, {1, 4}}, "at an edge a sample has two squares"},
        {0, 0, 1, 1, {{0, 0}}, "at a corner a sample has one square"},
    };
    int index = 0;
    for (const Case& c : cases) {
        fixture.cut(t, terrain, 8, c.x0, c.y0, c.x1, c.y1, "jolt-hole-lattice-" + std::to_string(index++));
        JPH::ShapeRefC shape = JoltShapeBuilder::buildHeightFieldShape(&t.scene, component, Vector3(1, 1, 1), SAMPLES, &error);
        ASSERT_TRUE(shape) << error;
        const JPH::HeightFieldShape& field = heightField(shape);
        for (unsigned int y = 0; y < SAMPLES; y++) {
            for (unsigned int x = 0; x < SAMPLES; x++) {
                const bool expected = std::find(c.gaps.begin(), c.gaps.end(), std::make_pair(static_cast<int>(x), static_cast<int>(y))) != c.gaps.end();
                EXPECT_EQ(field.IsNoCollision(x, y), expected) << c.name << " sample " << x << ", " << y;
            }
        }
    }

    // mirrored in x, the gap mirrors with the surface: sample i sits at -(-half + spacing * i)
    fixture.cut(t, terrain, 8, 0, 3, 2, 5, "jolt-hole-lattice-mirror");
    JPH::ShapeRefC mirrored = JoltShapeBuilder::buildHeightFieldShape(&t.scene, component, Vector3(-1, 1, 1), SAMPLES, &error);
    ASSERT_TRUE(mirrored) << error;
    EXPECT_TRUE(heightField(mirrored).IsNoCollision(8, 4));
    EXPECT_TRUE(heightField(mirrored).IsNoCollision(7, 4));
    EXPECT_FALSE(heightField(mirrored).IsNoCollision(0, 4));

    // no hole map: every sample stands
    component.holeMap = Texture();
    JPH::ShapeRefC plain = JoltShapeBuilder::buildHeightFieldShape(&t.scene, component, Vector3(1, 1, 1), SAMPLES, &error);
    ASSERT_TRUE(plain) << error;
    for (unsigned int y = 0; y < SAMPLES; y++) {
        for (unsigned int x = 0; x < SAMPLES; x++) {
            EXPECT_FALSE(heightField(plain).IsNoCollision(x, y));
        }
    }
}

// R24: navmesh walkable area inside drawn ground inside collider surface. 10,000 rays down: no drawn
// ground lacks a hit, and a point of a hole deeper than the rim has none.
TEST(JoltShapeBuilder, HolesNestInsideDrawn) {
    constexpr int MAP = 40;
    constexpr float SIZE = 64.0f;
    TestScene t;
    const Entity terrain = t.terrain(41, SIZE, 4.0f, Vector3(0, 0, 0), [](float u, float v) { return 0.3f + 0.1f * u * v; });
    t.collider(terrain, Collider3DType::HEIGHTFIELD);
    HoleFixture fixture;
    fixture.cut(t, terrain, MAP, 10, 8, 27, 30, "jolt-hole-nest");
    t.scene.getComponent<RigidbodyComponent>(terrain).needReloadBody = true;
    runScene(t, 0);

    const terrain::HoleMapView view{fixture.texels.data(), MAP, MAP};
    const float cell = SIZE / 40.0f;
    const float texel = SIZE / MAP;
    // a point deeper than this inside the hole has lost its collider. R22 speaks of one cell; a point
    // can only rely on a corner of its own triangle, so the bound for any point is two.
    const float rim = 2.0f * std::max(cell, texel);
    int drawnPoints = 0, deepPoints = 0, rimPoints = 0;
    std::mt19937 random(11);
    std::uniform_real_distribution<float> coordinate(-SIZE * 0.5f + 0.01f, SIZE * 0.5f - 0.01f);
    for (int i = 0; i < 10000; i++) {
        const float x = coordinate(random);
        const float z = coordinate(random);
        const float u = (x + SIZE * 0.5f) / SIZE;
        const float v = (z + SIZE * 0.5f) / SIZE;
        const bool hole = terrain::isHole(view, u, v);
        Ray ray(Vector3(x, 30.0f, z), Vector3(0, -60.0f, 0));
        const bool hit = ray.intersects(&t.scene, RayFilter::BODY_3D).hit;
        if (!hole) {
            drawnPoints++;
            EXPECT_TRUE(hit) << "drawn ground without a collider at " << x << ", " << z;
            continue;
        }
        bool deep = true;
        for (float dz = -rim; dz <= rim && deep; dz += texel * 0.5f) {
            for (float dx = -rim; dx <= rim; dx += texel * 0.5f) {
                if (!terrain::isHole(view, (x + dx + SIZE * 0.5f) / SIZE, (z + dz + SIZE * 0.5f) / SIZE)) {
                    deep = false;
                    break;
                }
            }
        }
        if (deep) {
            deepPoints++;
            EXPECT_FALSE(hit) << "a body would stand deep in the hole at " << x << ", " << z;
        } else {
            rimPoints++;
        }
    }
    EXPECT_GT(drawnPoints, 1000);
    EXPECT_GT(deepPoints, 100);
    EXPECT_GT(rimPoints, 100);
}

// A ball falls through a hole, and stands on the ground beside it
TEST(JoltShapeBuilder, BallFallsThroughAHole) {
    constexpr int MAP = 32;
    TestScene t;
    const Entity terrain = t.terrain(33, 32.0f, 2.0f, Vector3(0, 0, 0), [](float, float) { return 0.5f; });
    t.collider(terrain, Collider3DType::HEIGHTFIELD);
    HoleFixture fixture;
    fixture.cut(t, terrain, MAP, 12, 12, 20, 20, "jolt-hole-ball");
    t.scene.getComponent<RigidbodyComponent>(terrain).needReloadBody = true;
    runScene(t, 0);
    Ray overHole(Vector3(0.0f, 10.0f, 0.0f), Vector3(0, -20.0f, 0));
    EXPECT_FALSE(overHole.intersects(&t.scene, RayFilter::BODY_3D).hit) << "a ball would fall in";
    Ray beside(Vector3(10.0f, 10.0f, 0.0f), Vector3(0, -20.0f, 0));
    const RayReturn hit = beside.intersects(&t.scene, RayFilter::BODY_3D);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y, 1.0f, 2e-2f);
}
