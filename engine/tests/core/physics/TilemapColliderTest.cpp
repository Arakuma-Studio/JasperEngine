// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The generated tilemap colliders in real Box2D (docs/specs/S50-grid-tilemap.md AC7): a box slides
// over a floor of 64 chunks without catching at their borders, one-way platforms and sensors
// behave. Only the builder and Box2D: TilemapColliderSceneTest.cpp covers the PhysicsSystem side.

#include <gtest/gtest.h>

#include "box2d/box2d.h"
#include "tilemap/TileColliderBuilder.h"

#include <cmath>
#include <vector>

namespace TilemapColliderTest_cpp {

using namespace jasperengine;

namespace {

    constexpr float TilemapColliderM = 64.0f;       // pixels per metre, the engine's default
    constexpr float TILEMAP_COLLIDER_DT = 1.0f / 60.0f;
    constexpr int CHUNK_CELLS = 8;
    constexpr int CHUNKS = 64;
    constexpr int TILEMAP_COLLIDER_CELL = 16;

    enum TilemapColliderTestTile : int32_t { COLLIDER_TILE_FULL = 0, COLLIDER_TILE_ONE_WAY = 1, COLLIDER_TILE_SENSOR = 2 };

    TilesetData tilemapColliderMakeTileset() {
        TilesetData data;
        data.tileWidth = TILEMAP_COLLIDER_CELL;
        data.tileHeight = TILEMAP_COLLIDER_CELL;
        TilesetSource source;
        source.id = 0;
        data.sources.push_back(source);
        auto add = [&](TileCollisionKind kind) {
            TileDef& tile = data.addTile(0, (int)data.tiles.size(), 0);
            tile.collision.mode = TileCollisionMode::Full;
            tile.collision.kind = kind;
        };
        add(TileCollisionKind::Solid);
        add(TileCollisionKind::OneWay);
        add(TileCollisionKind::Sensor);
        return data;
    }

    struct TilemapColliderWorld {
        b2WorldId id;
        b2BodyId ground;

        TilemapColliderWorld() {
            b2WorldDef def = b2DefaultWorldDef();
            def.gravity = {0.0f, -10.0f};
            id = b2CreateWorld(&def);
            b2BodyDef body = b2DefaultBodyDef();
            ground = b2CreateBody(id, &body);
        }

        ~TilemapColliderWorld() {
            b2DestroyWorld(id);
        }

        void addChain(const std::vector<TilePoint>& points, bool loop) {
            std::vector<b2Vec2> vertices(points.size());
            for (size_t i = 0; i < points.size(); i++) {
                vertices[i] = {(float)points[i].x / TilemapColliderM, (float)points[i].y / TilemapColliderM};
            }
            b2ChainDef def = b2DefaultChainDef();
            def.points = vertices.data();
            def.count = (int)vertices.size();
            def.isLoop = loop;
            def.friction = 0.0f;
            b2CreateChain(ground, &def);
        }

        void addPolygon(const std::vector<TilePoint>& points, bool sensor) {
            std::vector<b2Vec2> vertices(points.size());
            for (size_t i = 0; i < points.size(); i++) {
                vertices[i] = {(float)points[i].x / TilemapColliderM, (float)points[i].y / TilemapColliderM};
            }
            b2Hull hull = b2ComputeHull(vertices.data(), (int)vertices.size());
            ASSERT_TRUE(b2ValidateHull(&hull));
            b2ShapeDef def = b2DefaultShapeDef();
            def.friction = 0.0f;
            def.isSensor = sensor;
            def.enableSensorEvents = sensor;
            const b2Polygon polygon = b2MakePolygon(&hull, 0.0f);
            b2CreatePolygonShape(ground, &def, &polygon);
        }

        void addChunk(const TileColliderChunk& chunk) {
            for (const TileColliderChain& chain : chunk.loops) addChain(chain.points, true);
            for (const TileColliderChain& chain : chunk.chains) addChain(chain.points, false);
            for (const TileColliderChain& chain : chunk.oneWay) addChain(chain.points, false);
            for (const TileColliderPolygon& polygon : chunk.polygons) addPolygon(polygon.points, false);
            for (const TileColliderPolygon& polygon : chunk.sensors) addPolygon(polygon.points, true);
        }

        b2BodyId addBox(float xPx, float yPx, float sizePx) {
            b2BodyDef def = b2DefaultBodyDef();
            def.type = b2_dynamicBody;
            def.position = {xPx / TilemapColliderM, yPx / TilemapColliderM};
            def.enableSleep = false;
            const b2BodyId body = b2CreateBody(id, &def);
            b2ShapeDef shape = b2DefaultShapeDef();
            shape.friction = 0.0f;
            shape.enableSensorEvents = true;
            const float half = sizePx * 0.5f / TilemapColliderM;
            const b2Polygon polygon = b2MakeBox(half, half);
            b2CreatePolygonShape(body, &shape, &polygon);
            return body;
        }

        void step() {
            b2World_Step(id, TILEMAP_COLLIDER_DT, 4);
        }
    };

    // a floor two cells high across CHUNKS chunks, its top at y = 32 px
    TileCells makeFloor() {
        TileCells cells(CHUNK_CELLS);
        for (int x = 0; x < CHUNK_CELLS * CHUNKS; x++) {
            cells.setTile(CellCoord{x, 0}, COLLIDER_TILE_FULL);
            cells.setTile(CellCoord{x, 1}, COLLIDER_TILE_FULL);
        }
        return cells;
    }

    // the box slides across the floor at a speed; returns the worst speed loss, as a fraction
    double worstSpeedLoss(TilemapColliderWorld& world, double speedPxPerSecond, double* fell = nullptr) {
        const float floorEndPx = (float)(CHUNK_CELLS * CHUNKS * TILEMAP_COLLIDER_CELL);
        const float sizePx = 20.0f;
        const b2BodyId box = world.addBox(20.0f, 32.0f + sizePx * 0.5f, sizePx);
        // settle on the floor before the push
        for (int i = 0; i < 30; i++) {
            world.step();
        }
        const float speed = (float)speedPxPerSecond / TilemapColliderM;
        b2Body_SetLinearVelocity(box, {speed, 0.0f});
        double worst = 0.0;
        const int maxSteps = (int)(floorEndPx / TilemapColliderM / speed / TILEMAP_COLLIDER_DT) + 120;
        for (int i = 0; i < maxSteps; i++) {
            world.step();
            const b2Vec2 velocity = b2Body_GetLinearVelocity(box);
            worst = std::max(worst, std::fabs((double)velocity.x - speed) / speed);
            const b2Vec2 position = b2Body_GetPosition(box);
            if (fell) {
                *fell = std::min(*fell, (double)position.y * TilemapColliderM);
            }
            if (position.x * TilemapColliderM > floorEndPx - 3.0f * sizePx) {
                break;
            }
        }
        return worst;
    }

}

TEST(TilemapColliderSlide, BoxKeepsItsSpeedAcrossSixtyFourChunks) {
    const TilesetData tileset = tilemapColliderMakeTileset();
    const TileCells cells = makeFloor();
    TileColliderSettings settings;
    settings.cells = &cells;
    settings.tileset = &tileset;
    const TileColliderBuilder builder(settings);
    std::vector<TileColliderChunk> chunks;
    for (int x = -1; x <= CHUNKS; x++) {
        chunks.push_back(builder.build(ChunkCoord{x, 0}));
    }

    for (double pxPerSecond : {128.0, 640.0, 2560.0}) {
        TilemapColliderWorld world;
        for (const TileColliderChunk& chunk : chunks) {
            world.addChunk(chunk);
        }
        double lowest = 1e9;
        const double loss = worstSpeedLoss(world, pxPerSecond, &lowest);
        EXPECT_LT(loss, 0.01) << "speed " << pxPerSecond;
        EXPECT_GT(lowest, 32.0) << "speed " << pxPerSecond;
        std::printf("[slide] %.0f px/s: speed loss %.5f, lowest %.3f px\n", pxPerSecond, loss, lowest);
    }
}

// The control for the test above (AC7's red control): each chunk closes its own outline at its
// border instead of sharing three points with its neighbour, so the walls of the neighbouring
// loops meet under the surface and a slow box catches on their corners. Keeps the harness honest:
// the same measure has to see that catch.
TEST(TilemapColliderSlide, ChunksClosingTheirOwnOutlineCatchTheBox) {
    double worst = 0.0;
    for (double pxPerSecond : {128.0, 640.0, 2560.0}) {
        TilemapColliderWorld world;
        const double topPx = 32.0;
        for (int chunk = 0; chunk < CHUNKS; chunk++) {
            const double right = (double)((chunk + 1) * CHUNK_CELLS * TILEMAP_COLLIDER_CELL);
            const double left = (double)(chunk * CHUNK_CELLS * TILEMAP_COLLIDER_CELL);
            world.addChain({{right, 0.0}, {right, topPx}, {left, topPx}, {left, 0.0}}, true);
        }
        const double loss = worstSpeedLoss(world, pxPerSecond);
        std::printf("[slide-red] %.0f px/s, chunks closed at their borders: speed loss %.5f\n", pxPerSecond, loss);
        worst = std::max(worst, loss);
    }
    EXPECT_GT(worst, 0.5);
}

TEST(TilemapColliderOneWay, PassesFromBelowAndBlocksFromAbove) {
    const TilesetData tileset = tilemapColliderMakeTileset();
    TileCells cells(CHUNK_CELLS);
    for (int x = 0; x < 32; x++) {
        cells.setTile(CellCoord{x, 8}, COLLIDER_TILE_ONE_WAY);   // a platform at y = 144 px across the chunk border
    }
    TileColliderSettings settings;
    settings.cells = &cells;
    settings.tileset = &tileset;
    const TileColliderBuilder builder(settings);

    // from below: thrown up through the platform
    {
        TilemapColliderWorld world;
        for (int c = 0; c < 4; c++) {
            world.addChunk(builder.build(ChunkCoord{c, 1}));
        }
        const b2BodyId box = world.addBox(100.0f, 100.0f, 20.0f);
        b2Body_SetLinearVelocity(box, {0.0f, 10.0f});
        bool above = false;
        for (int i = 0; i < 60 && !above; i++) {
            world.step();
            above = b2Body_GetPosition(box).y * TilemapColliderM > 160.0f;
        }
        EXPECT_TRUE(above) << "the one-way platform stopped a box coming from below";
    }

    // from above: it lands on the platform, on either side of the chunk border
    for (float xPx : {100.0f, 120.0f, 136.0f}) {
        TilemapColliderWorld world;
        for (int c = 0; c < 4; c++) {
            world.addChunk(builder.build(ChunkCoord{c, 1}));
        }
        const b2BodyId box = world.addBox(xPx, 200.0f, 20.0f);
        for (int i = 0; i < 120; i++) {
            world.step();
        }
        EXPECT_NEAR(b2Body_GetPosition(box).y * TilemapColliderM, 144.0f + 10.0f, 1.0f) << "x " << xPx;
    }
}

TEST(TilemapColliderSensors, FireEventsOnABodyPassingThrough) {
    const TilesetData tileset = tilemapColliderMakeTileset();
    TileCells cells(CHUNK_CELLS);
    for (int x = 2; x < 6; x++) {
        cells.setTile(CellCoord{x, 1}, COLLIDER_TILE_SENSOR);
    }
    TileColliderSettings settings;
    settings.cells = &cells;
    settings.tileset = &tileset;
    const TileColliderChunk chunk = TileColliderBuilder(settings).build(ChunkCoord{0, 0});
    ASSERT_EQ(chunk.sensors.size(), 4u);
    EXPECT_TRUE(chunk.loops.empty());
    EXPECT_TRUE(chunk.chains.empty());

    TilemapColliderWorld world;
    world.addChunk(chunk);
    const b2BodyId box = world.addBox(32.0f, 100.0f, 10.0f);
    int begun = 0;
    for (int i = 0; i < 120; i++) {
        world.step();
        const b2SensorEvents events = b2World_GetSensorEvents(world.id);
        begun += events.beginCount;
    }
    EXPECT_GT(begun, 0);
    (void)box;
}

}  // namespace TilemapColliderTest_cpp
