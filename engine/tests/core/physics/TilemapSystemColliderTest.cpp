// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A Tilemap Collider 2D driven by the TilemapSystem (docs/specs/S50-grid-tilemap.md R29, R38,
// R41, R58, AC7): a scene with a Grid, a Tilemap Layer, its collider and a static Rigidbody2D,
// the system telling the edited chunks from the cell store's revisions every frame and rebuilding
// their colliders at the next fixed step. No GPU: the chunks are never drawn, so none is built
// for rendering, and the colliders must still follow only the edits.

#include "Engine.h"
#include "Input.h"
#include "LogCapture.h"
#include "Scene.h"
#include "component/GridComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/TilemapCollider2DComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "object/physics/Rigidbody2D.h"
#include "object/tilemap/TilemapLayer.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TilemapColliders.h"
#include "subsystem/TilemapSystem.h"
#include "tilemap/TilesetPool.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>

using namespace jasperengine;

namespace {

    constexpr float SYSTEM_DT = 1.0f / 60.0f;
    constexpr int SYSTEM_CHUNK = 8;
    const char* const SYSTEM_TILESET = "tilemap-system-colliders.tileset";

    std::shared_ptr<TilesetData> systemTileset() {
        auto data = std::make_shared<TilesetData>();
        data->tileWidth = 16;
        data->tileHeight = 16;
        TilesetSource source;
        source.id = 0;
        data->sources.push_back(source);
        TileDef& tile = data->addTile(0, 0, 0);
        tile.collision.mode = TileCollisionMode::Full;
        tile.collision.kind = TileCollisionKind::Solid;
        return data;
    }

    class TilemapSystemColliders : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        std::unique_ptr<TilemapLayer> layer;
        PhysicsSystem* physics = nullptr;
        TilemapSystem* tilemaps = nullptr;
        float updateTime = 0.0f;
        int begins = 0;
        int ends = 0;

        void SetUp() override {
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(SYSTEM_DT);
            TilesetPool::set(SYSTEM_TILESET, systemTileset());
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            tilemaps = scene->getSystem<TilemapSystem>().get();
            physics->setPointsToMeterScale2D(64.0f);
            physics->setGravity2D(0.0f, -10.0f);

            layer = std::make_unique<TilemapLayer>(scene.get());
            const Entity entity = layer->getEntity();
            scene->addComponent<GridComponent>(entity, GridComponent());
            TilemapLayerComponent& component = layer->getComponent<TilemapLayerComponent>();
            component.tileset = AssetRef(SYSTEM_TILESET);
            component.chunkSize = SYSTEM_CHUNK;
            Rigidbody2DComponent body;
            body.type = BodyType::STATIC;
            scene->addComponent<Rigidbody2DComponent>(entity, body);
            scene->addComponent<TilemapCollider2DComponent>(entity, TilemapCollider2DComponent());

            physics->beginContact2D.add("tilemap-system", std::function<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>(
                [this](Rigidbody2D, unsigned long, Rigidbody2D, unsigned long) { begins++; }));
            physics->endContact2D.add("tilemap-system", std::function<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>(
                [this](Rigidbody2D, unsigned long, Rigidbody2D, unsigned long) { ends++; }));
        }

        void TearDown() override {
            physics->beginContact2D.remove("tilemap-system");
            physics->endContact2D.remove("tilemap-system");
            layer.reset();
            scene.reset();
            TilesetPool::invalidate(SYSTEM_TILESET);
            Engine::setUpdateTime(updateTime);
        }

        TilemapLayerComponent& component() { return layer->getComponent<TilemapLayerComponent>(); }
        TilemapCollider2DComponent& collider() { return layer->getComponent<TilemapCollider2DComponent>(); }
        TileColliderState& state() { return *collider().state; }
        Rigidbody2DComponent& body() { return layer->getComponent<Rigidbody2DComponent>(); }

        void resolveTransforms() {
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate) {
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                }
            }
        }

        // one frame: the system's update (what changed), then a fixed step (colliders, physics)
        void frame(int count = 1) {
            for (int i = 0; i < count; i++) {
                resolveTransforms();
                tilemaps->update(SYSTEM_DT);
                Input::beginFixedStep();
                scene->fixedUpdate(SYSTEM_DT);
                Input::endFixedStep();
            }
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

        void floor() {
            for (int x = 0; x < SYSTEM_CHUNK * 3; x++) {
                layer->setCell(x, 0, 0);
                layer->setCell(x, 1, 0);
            }
        }
    };

}

// R41, AC7 through the system: an edit rebuilds its chunk and the neighbours that see its cells,
// and the next edit again only those: the chunks out of view are not re-dirtied by every edit
// (their render build waits for a view, R58)
TEST_F(TilemapSystemColliders, EditsRebuildOnlyTheTouchedChunksAndTheirNeighbours) {
    for (int y = 0; y < 3 * SYSTEM_CHUNK; y++) {
        for (int x = 0; x < 3 * SYSTEM_CHUNK; x++)
            layer->setCell(x, y, 0);
    }
    frame(2);
    ASSERT_TRUE(collider().state);
    ASSERT_TRUE(body().generated);
    // the middle chunk of a solid 3x3 block has no outline of its own
    EXPECT_EQ(state().chunksReplaced, 8u);
    EXPECT_EQ(collider().generatedChains, 8u);

    for (int edit = 0; edit < 3; edit++) {
        const uint64_t built = state().chunksBuilt;
        const uint64_t replaced = state().chunksReplaced;
        // an inner cell of the middle chunk: a hole of its own, its loop in that chunk only
        layer->setCell(10 + edit, 11, NO_TILE);
        frame();
        EXPECT_EQ(state().chunksBuilt - built, 9u) << "edit " << edit << ": the chunk and its 8 neighbours";
        EXPECT_EQ(state().chunksReplaced - replaced, 1u) << "edit " << edit;
    }
    // a frame without edits rebuilds nothing
    const uint64_t built = state().chunksBuilt;
    frame(2);
    EXPECT_EQ(state().chunksBuilt, built);
}

// a body resting on the map keeps its contact through an edit elsewhere on a store a Play-mode
// copy (or an undo snapshot) still shares, and through a layer-wide change that leaves the shapes
// as they were (the tint): no chunk under it is replaced, so no end and begin events
TEST_F(TilemapSystemColliders, UnrelatedEditsKeepRestingContacts) {
    floor();
    frame();
    const Entity box = dynamicBox(40.0f, 60.0f, 20.0f);
    frame(90);
    ASSERT_EQ(begins, 1);
    ASSERT_EQ(ends, 0);
    ASSERT_FALSE(physics->getContactSummary2D(box).empty());

    // what an edit of chunk (2, 0) rebuilds on a store nobody else holds
    uint64_t built = state().chunksBuilt;
    layer->setCell(21, 6, 0);
    frame();
    const uint64_t oneChunkEdit = state().chunksBuilt - built;
    ASSERT_GT(oneChunkEdit, 0u);

    // the copy on write: the first write clones the store; the clone's chunks keep their
    // revisions, so the same edit rebuilds the same chunks, not every one
    const TileCellsRef snapshot = component().cells;
    const TileCells* before = &component().cells.read();
    layer->setCell(20, 6, 0);
    ASSERT_NE(&component().cells.read(), before);
    const uint64_t replaced = state().chunksReplaced;
    built = state().chunksBuilt;
    frame(30);
    EXPECT_EQ(state().chunksBuilt - built, oneChunkEdit) << "a copy's edit rebuilds what the same edit did before it";
    EXPECT_EQ(state().chunksReplaced - replaced, 1u) << "only the edited chunk's colliders";
    EXPECT_EQ(ends, 0) << "the box's contact did not end";
    EXPECT_EQ(begins, 1);

    // back to the snapshot (an undo): the edited chunk changes again, the floor under the box stays
    component().cells = snapshot;
    frame(30);
    EXPECT_EQ(ends, 0);
    EXPECT_EQ(begins, 1);

    component().tint = Vector4(0.5f, 0.5f, 0.5f, 1.0f);
    component().needUpdate = true;
    frame(30);
    EXPECT_EQ(ends, 0) << "a layer-wide change with the same shapes";
    EXPECT_EQ(begins, 1);
    EXPECT_FALSE(physics->getContactSummary2D(box).empty());
}

// R62: a disabled collider takes its colliders away once, not every frame
TEST_F(TilemapSystemColliders, DisabledColliderReleasesOnce) {
    floor();
    frame(2);
    ASSERT_GT(collider().generatedChains, 0u);
    collider().enabled = false;
    frame();
    EXPECT_EQ(collider().generatedChains, 0u);
    EXPECT_TRUE(body().generated->chunks.empty());
    EXPECT_EQ(b2Body_GetShapeCount(body().body), 0);
    body().needUpdateColliders = false;
    tilemaps->fixedUpdate(SYSTEM_DT);
    EXPECT_FALSE(body().needUpdateColliders) << "nothing left to take away";
    collider().enabled = true;
    frame(2);
    EXPECT_GT(collider().generatedChains, 0u);
}

// R38: Box2D filters are 16 bits: wider values clamp instead of wrapping onto other categories
TEST_F(TilemapSystemColliders, FilterBitsClampToSixteenBits) {
    const test::LogCapture log;
    const char* clampWarning = "past Box2D's 16 bits";
    collider().categoryBits = 0x12345u;
    collider().maskBits = 0xFFFFFFFFu;
    collider().groupIndex = 40000;
    floor();
    frame(2);
    // once for the change, not again on the frames after it
    EXPECT_EQ(log.count(D_LOG_WARN, clampWarning), 1u);
    ASSERT_TRUE(body().generated);
    ASSERT_FALSE(body().generated->chunks.empty());
    for (const auto& [coord, chunk] : body().generated->chunks) {
        for (const Collider2D& generated : chunk.colliders) {
            EXPECT_EQ(generated.categoryBits, 0xFFFFu);
            EXPECT_EQ(generated.maskBits, 0xFFFFu);
            EXPECT_EQ(generated.groupIndex, 32767);
        }
    }
    collider().groupIndex = -40000;
    collider().needUpdate = true;
    frame();
    EXPECT_EQ(body().generated->chunks.begin()->second.colliders[0].groupIndex, -32768);
    EXPECT_EQ(log.count(D_LOG_WARN, clampWarning), 2u);
    // values Box2D holds warn nothing
    collider().categoryBits = 2;
    collider().maskBits = 0xFFFFu;
    collider().groupIndex = -3;
    collider().needUpdate = true;
    frame();
    EXPECT_EQ(log.count(D_LOG_WARN, clampWarning), 2u);
}
