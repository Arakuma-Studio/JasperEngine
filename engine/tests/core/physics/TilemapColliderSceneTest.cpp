// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A Tilemap Collider 2D's generated colliders in a Scene (docs/specs/S50-grid-tilemap.md R41-R43,
// R58, AC7): tilemapcolliders writes them into the Rigidbody2D's separate list, the PhysicsSystem
// makes their shapes only where they changed, contacts and raycasts report a key that resolves to
// the generated collider.

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/Rigidbody2DComponent.h"
#include "component/TilemapCollider2DComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "object/physics/Collision2D.h"
#include "object/physics/Rigidbody2D.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TilemapColliders.h"
#include "tilemap/TilesetPool.h"
#include "../ecs/EcsTestUtil.h"
#include "../support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;
    constexpr float M = 64.0f;
    constexpr int CHUNK_CELLS = 8;

    enum TestTile : int32_t { FULL = 0, ONE_WAY = 1, SENSOR = 2 };

    std::shared_ptr<TilesetRuntime> makeTilesetRuntime() {
        auto data = std::make_shared<TilesetData>();
        data->tileWidth = 16;
        data->tileHeight = 16;
        TilesetSource source;
        source.id = 0;
        data->sources.push_back(source);
        auto add = [&](TileCollisionKind kind) {
            TileDef& tile = data->addTile(0, (int)data->tiles.size(), 0);
            tile.collision.mode = TileCollisionMode::Full;
            tile.collision.kind = kind;
        };
        add(TileCollisionKind::Solid);
        add(TileCollisionKind::OneWay);
        add(TileCollisionKind::Sensor);
        auto runtime = std::make_shared<TilesetRuntime>();
        runtime->data = data;
        return runtime;
    }

    class TilemapColliderScene : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;
        std::shared_ptr<TilesetRuntime> tileset = makeTilesetRuntime();
        TilemapLayerComponent layer;
        TilemapCollider2DComponent collider;
        TileGridSpec grid;
        Entity map = NULL_ENTITY;

        void SetUp() override {
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setPointsToMeterScale2D(M);
            physics->setGravity2D(0.0f, -10.0f);

            map = test::makeTransformEntity(*scene);
            Rigidbody2DComponent body;
            body.type = BodyType::STATIC;
            scene->addComponent<Rigidbody2DComponent>(map, body);
            layer.cells.edit().setChunkSize(CHUNK_CELLS);
        }

        void TearDown() override {
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        TileCells& cells() { return layer.cells.edit(); }
        TileColliderState& state() { return *collider.state; }
        Rigidbody2DComponent& body() { return scene->getComponent<Rigidbody2DComponent>(map); }

        void resolveTransforms() {
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate) {
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                }
            }
        }

        void step() {
            resolveTransforms();
            Input::beginFixedStep();
            scene->fixedUpdate(DT);
            Input::endFixedStep();
            resolveTransforms();
        }

        void updateColliders() {
            tilemapcolliders::update(scene.get(), map, layer, collider, grid, tileset.get());
        }

        Entity dynamicBox(float xPx, float yPx, float sizePx) {
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(xPx, yPx, 0.0f);
            transform.needUpdate = true;
            Rigidbody2DComponent rigidbody;
            rigidbody.type = BodyType::DYNAMIC;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider2DType::POLYGON;
            rigidbody.colliders[0].pointA = Vector2(-sizePx * 0.5f, -sizePx * 0.5f);
            rigidbody.colliders[0].pointB = Vector2(sizePx * 0.5f, sizePx * 0.5f);
            scene->addComponent<Rigidbody2DComponent>(entity, rigidbody);
            return entity;
        }

        void floorAcrossChunks() {
            for (int x = 0; x < CHUNK_CELLS * 3; x++) {
                cells().setTile(CellCoord{x, 0}, FULL);
                cells().setTile(CellCoord{x, 1}, FULL);
            }
        }
    };

}

TEST_F(TilemapColliderScene, GeneratesChunkedCollidersAndShapes) {
    floorAcrossChunks();
    updateColliders();

    ASSERT_TRUE(body().generated);
    EXPECT_EQ(body().generated->chunks.size(), 3u);
    EXPECT_EQ(collider.generatedChains, 2u + 2u + 2u);
    EXPECT_EQ(collider.generatedPolygons, 0u);

    const uint64_t chainsBefore = physics->getChainsCreated2D();
    step();
    EXPECT_EQ(physics->getChainsCreated2D() - chainsBefore, 6u);
    EXPECT_TRUE(body().colliders.empty());
    for (const auto& [coord, chunk] : body().generated->chunks) {
        for (const Collider2D& generated : chunk.colliders) {
            EXPECT_TRUE(b2Chain_IsValid(generated.chain));
            EXPECT_TRUE(isGeneratedCollider2DKey(generated.generatedKey));
        }
    }
}

TEST_F(TilemapColliderScene, EditRebuildsOnlyTouchedAndBorderChunks) {
    for (int y = 0; y < 3 * CHUNK_CELLS; y++) {
        for (int x = 0; x < 3 * CHUNK_CELLS; x++) {
            cells().setTile(CellCoord{x, y}, FULL);
        }
    }
    updateColliders();
    step();
    ASSERT_EQ(state().chunksReplaced, 9u);
    const uint64_t builtBefore = state().chunksBuilt;
    const uint64_t replacedBefore = state().chunksReplaced;
    const uint64_t chainsBefore = physics->getChainsCreated2D();

    // an inner cell of the middle chunk: a new hole, its own loop: only that chunk's colliders change
    cells().setTile(CellCoord{11, 11}, NO_TILE);
    const ChunkCoord middle{1, 1};
    tilemapcolliders::markDirty(collider, std::span<const ChunkCoord>(&middle, 1));
    updateColliders();
    // the middle chunk and its 8 neighbours see the cell
    EXPECT_EQ(state().chunksBuilt - builtBefore, 9u);
    EXPECT_EQ(state().chunksReplaced - replacedBefore, 1u);
    step();
    EXPECT_GT(physics->getChainsCreated2D() - chainsBefore, 0u);

    // a border cell: the neighbour's chain changes too
    const uint64_t replacedBeforeBorder = state().chunksReplaced;
    cells().setTile(CellCoord{7, 0}, NO_TILE);
    const ChunkCoord corner{0, 0};
    tilemapcolliders::markDirty(collider, std::span<const ChunkCoord>(&corner, 1));
    updateColliders();
    EXPECT_GE(state().chunksReplaced - replacedBeforeBorder, 2u);
}

TEST_F(TilemapColliderScene, ContactsResolveGeneratedColliders) {
    floorAcrossChunks();
    updateColliders();
    const Entity box = dynamicBox(40.0f, 60.0f, 20.0f);
    for (int i = 0; i < 90; i++) {
        step();
    }
    const std::vector<ContactSummary> contacts = physics->getContactSummary2D(box);
    ASSERT_FALSE(contacts.empty());
    const ContactSummary& contact = contacts[0];
    EXPECT_EQ(contact.other, map);
    EXPECT_TRUE(isGeneratedCollider2DKey(contact.otherCollider));

    const ColliderRef2D ref(scene.get(), map, contact.otherCollider);
    ASSERT_TRUE(ref.isValid());
    EXPECT_EQ(ref.getType(), (int)Collider2DType::CHAIN);
    EXPECT_FALSE(ref.isTrigger());
    EXPECT_FALSE(ColliderRef2D(scene.get(), map, GENERATED_COLLIDER2D_BIT | 123456).isValid());
}

TEST_F(TilemapColliderScene, RaycastReportsTheGeneratedCollider) {
    floorAcrossChunks();
    updateColliders();
    step();
    const Ray ray(Vector3(100.0f, 200.0f, 0.0f), Vector3(0.0f, -400.0f, 0.0f));
    const RayReturn hit = ray.intersects(scene.get(), RayFilter::BODY_2D);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y, 32.0f, 0.1f);
    EXPECT_EQ(hit.body, map);
    EXPECT_TRUE(isGeneratedCollider2DKey((int)hit.colliderIndex));
    EXPECT_NE(findCollider2D(body(), (int)hit.colliderIndex), nullptr);
}

TEST_F(TilemapColliderScene, RaycastAgainstAGeneratedColliderByIndex) {
    floorAcrossChunks();
    updateColliders();
    step();
    const Rigidbody2D rigidbody(scene.get(), map);
    const Ray ray(Vector3(100.0f, 200.0f, 0.0f), Vector3(0.0f, -400.0f, 0.0f));
    const RayReturn hit = ray.intersects(scene.get(), RayFilter::BODY_2D);
    ASSERT_TRUE(hit.hit);
    const RayReturn againstCollider = ray.intersects(rigidbody, hit.colliderIndex);
    EXPECT_TRUE(againstCollider.hit);
    EXPECT_NEAR(againstCollider.point.y, 32.0f, 0.1f);
}

TEST_F(TilemapColliderScene, SensorsFireThroughTheEngine) {
    for (int x = 2; x < 6; x++) {
        cells().setTile(CellCoord{x, 3}, SENSOR);
    }
    updateColliders();
    EXPECT_EQ(collider.generatedPolygons, 4u);
    const Entity box = dynamicBox(64.0f, 120.0f, 10.0f);
    bool sawTrigger = false;
    for (int i = 0; i < 120 && !sawTrigger; i++) {
        step();
        for (const ContactSummary& contact : physics->getContactSummary2D(box)) {
            sawTrigger |= contact.trigger && contact.other == map;
        }
    }
    EXPECT_TRUE(sawTrigger);
}

TEST_F(TilemapColliderScene, ReleaseDropsGeneratedColliders) {
    floorAcrossChunks();
    updateColliders();
    step();
    ASSERT_TRUE(body().generated);
    tilemapcolliders::release(scene.get(), map, collider);
    EXPECT_EQ(collider.generatedChains, 0u);
    EXPECT_TRUE(body().generated->chunks.empty());
    step();
    int shapes = b2Body_GetShapeCount(body().body);
    EXPECT_EQ(shapes, 0);
}

TEST_F(TilemapColliderScene, BodyReloadRemakesGeneratedShapes) {
    floorAcrossChunks();
    updateColliders();
    step();
    const uint64_t chainsBefore = physics->getChainsCreated2D();
    body().needReloadBody = true;
    step();
    EXPECT_EQ(physics->getChainsCreated2D() - chainsBefore, 6u);
    for (const auto& [coord, chunk] : body().generated->chunks) {
        for (const Collider2D& generated : chunk.colliders) {
            EXPECT_TRUE(b2Chain_IsValid(generated.chain));
        }
    }
}

// R58: one chunk's outlines, and a 256x256 map's first build, through testsupport::perfSlack()
TEST_F(TilemapColliderScene, BuildBudgets) {
    using Clock = std::chrono::steady_clock;
    TileCells cells(32);
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) {
            if ((x * 7 + y * 3) % 11 != 0) {
                cells.setTile(CellCoord{x, y}, FULL);
            }
        }
    }
    TileColliderSettings settings;
    settings.cells = &cells;
    settings.tileset = tileset->data.get();
    settings.grid = grid;
    const TileColliderBuilder builder(settings);

    auto start = Clock::now();
    for (int i = 0; i < 16; i++) {
        (void)builder.build(ChunkCoord{i % 8, 0});
    }
    const double chunkMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 16.0;

    start = Clock::now();
    int colliders = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            colliders += (int)builder.build(ChunkCoord{x, y}).chainCount();
        }
    }
    const double firstBuildMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("[bench] tilemap colliders: chunk outlines %.3f ms, 256x256 first build %.2f ms single-threaded (%d chains)\n", chunkMs, firstBuildMs, colliders);
    EXPECT_GT(colliders, 0);

    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        // R58's budgets are for mobile (one chunk 0.5 ms) and parallel desktop (first build 30 ms)
        EXPECT_LT(chunkMs, 0.5 * slack);
        EXPECT_LT(firstBuildMs, 120.0 * slack);
    }
}
