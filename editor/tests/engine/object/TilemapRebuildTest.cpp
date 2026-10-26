// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// object/Tilemap: every call that changes what the tiles draw asks MeshSystem to rebuild the
// geometry. removeRect, clearRects, clearTiles, addRect and setReserveTiles left
// needUpdateTilemap unset, so the map kept drawing the tiles and rects it had before.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/TilemapComponent.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "object/Tilemap.h"

using namespace jasperengine;

namespace {

    struct TilemapFixture {
        Scene scene;
        Tilemap tilemap{&scene};

        TilemapFixture() {
            // no scene camera to flip against
            component().automaticFlipY = false;
        }

        TilemapComponent& component() {
            return tilemap.getComponent<TilemapComponent>();
        }

        // what MeshSystem draws once the scene updates
        unsigned int rebuild() {
            EXPECT_TRUE(tilemap.createTilemap());
            return component().renderedTiles;
        }
    };

}

TEST(TilemapRebuild, AddTileRebuilds) {
    TilemapFixture fixture;
    fixture.tilemap.addRect(0, "grass", Rect(0.0f, 0.0f, 0.5f, 0.5f));
    fixture.tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    EXPECT_EQ(fixture.rebuild(), 1u);

    fixture.tilemap.addTile(0, Vector2(16.0f, 0.0f), 16.0f, 16.0f);
    EXPECT_EQ(fixture.rebuild(), 2u);
}

TEST(TilemapRebuild, RemoveRectRebuilds) {
    TilemapFixture fixture;
    fixture.tilemap.addRect(0, "grass", Rect(0.0f, 0.0f, 0.5f, 0.5f));
    fixture.tilemap.addRect(1, "rock", Rect(0.5f, 0.0f, 0.5f, 0.5f));
    fixture.tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    fixture.tilemap.addTile(1, Vector2(16.0f, 0.0f), 16.0f, 16.0f);
    EXPECT_EQ(fixture.rebuild(), 2u);

    // the grass tile loses its rect, the rock one moves to id 0
    fixture.tilemap.removeRect("grass");
    EXPECT_EQ(fixture.rebuild(), 1u);
    EXPECT_EQ(fixture.tilemap.getTile(1).rectId, 0);
}

TEST(TilemapRebuild, ClearRectsRebuilds) {
    TilemapFixture fixture;
    fixture.tilemap.addRect(0, "grass", Rect(0.0f, 0.0f, 0.5f, 0.5f));
    fixture.tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    EXPECT_EQ(fixture.rebuild(), 1u);

    fixture.tilemap.clearRects();
    EXPECT_EQ(fixture.rebuild(), 0u);
}

TEST(TilemapRebuild, ClearTilesRebuilds) {
    TilemapFixture fixture;
    fixture.tilemap.addRect(0, "grass", Rect(0.0f, 0.0f, 0.5f, 0.5f));
    fixture.tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    fixture.tilemap.addTile(0, Vector2(16.0f, 0.0f), 16.0f, 16.0f);
    EXPECT_EQ(fixture.rebuild(), 2u);

    fixture.tilemap.clearTiles();
    EXPECT_EQ(fixture.rebuild(), 0u);

    fixture.tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    fixture.tilemap.addTile(0, Vector2(0.0f, 16.0f), 16.0f, 16.0f);
    fixture.rebuild();
    fixture.tilemap.clearAll();
    EXPECT_EQ(fixture.rebuild(), 0u);
}

TEST(TilemapRebuild, AddRectRebuilds) {
    TilemapFixture fixture;
    fixture.tilemap.addRect(0, "grass", Rect(0.0f, 0.0f, 0.5f, 0.5f));
    fixture.tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    // its rect comes later
    fixture.tilemap.addTile(1, Vector2(16.0f, 0.0f), 16.0f, 16.0f);
    EXPECT_EQ(fixture.rebuild(), 1u);

    fixture.tilemap.addRect(1, "rock", Rect(0.5f, 0.0f, 0.5f, 0.5f));
    EXPECT_EQ(fixture.rebuild(), 2u);
}

TEST(TilemapRebuild, SetReserveTilesRebuilds) {
    TilemapFixture fixture;
    fixture.tilemap.addRect(0, "grass", Rect(0.0f, 0.0f, 0.5f, 0.5f));
    fixture.tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    // an empty tile keeps a slot only while reserved
    fixture.tilemap.addTile(0, Vector2(16.0f, 0.0f), 0.0f, 0.0f);
    fixture.tilemap.setReserveTiles(10);
    EXPECT_EQ(fixture.rebuild(), 2u);

    fixture.tilemap.setReserveTiles(0);
    EXPECT_EQ(fixture.rebuild(), 1u);
    EXPECT_EQ(fixture.tilemap.getReserveTiles(), 0u);
}
