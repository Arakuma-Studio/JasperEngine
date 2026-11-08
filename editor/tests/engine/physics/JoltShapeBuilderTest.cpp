// SPDX-License-Identifier: MIT
//
// The scene's own (play-mode) 3D physics over the shared Jolt shape builders
// (engine/core/physics/JoltShapeBuilder): every collider type and source settles where it
// should, so moving the builders out of PhysicsSystem changed nothing.

#include <gtest/gtest.h>

#include "PhysicsTestScene.h"
#include "component/TerrainComponent.h"
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
