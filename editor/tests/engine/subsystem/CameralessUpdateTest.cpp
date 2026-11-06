// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A scene updated before it has a camera (an inline child scene shown beside a running game
// before anything loaded it) waits: the geometry that flips against the camera is built once
// the scene has one. The update used to throw from getComponent<CameraComponent>(NULL_ENTITY),
// which took the editor down (docs/superpowers/specs/2026-11-04-scene-integrity-design.md 4.7).

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PolygonComponent.h"
#include "component/TilemapComponent.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "object/Camera.h"
#include "object/MeshPolygon.h"
#include "object/Tilemap.h"
#include "object/ui/Polygon.h"

using namespace jasperengine;

TEST(CameralessUpdate, TilemapWaitsForACamera) {
    Scene scene;
    ASSERT_EQ(scene.getCamera(), NULL_ENTITY);
    Tilemap tilemap(&scene);
    tilemap.addRect(0, "grass", Rect(0.0f, 0.0f, 0.5f, 0.5f));
    tilemap.addTile(0, Vector2(0.0f, 0.0f), 16.0f, 16.0f);
    ASSERT_TRUE(tilemap.getComponent<TilemapComponent>().automaticFlipY);

    EXPECT_FALSE(tilemap.createTilemap());
    EXPECT_TRUE(tilemap.getComponent<TilemapComponent>().needUpdateTilemap);

    Camera camera(&scene);
    scene.setCamera(camera.getEntity());
    EXPECT_TRUE(tilemap.createTilemap());
    EXPECT_FALSE(tilemap.getComponent<TilemapComponent>().needUpdateTilemap);
}

TEST(CameralessUpdate, MeshPolygonWaitsForACamera) {
    Scene scene;
    MeshPolygon polygon(&scene);
    polygon.addVertex(0.0f, 0.0f);
    polygon.addVertex(10.0f, 0.0f);
    polygon.addVertex(0.0f, 10.0f);

    EXPECT_FALSE(polygon.createPolygon());
    EXPECT_TRUE(polygon.getComponent<MeshPolygonComponent>().needUpdatePolygon);

    Camera camera(&scene);
    scene.setCamera(camera.getEntity());
    EXPECT_TRUE(polygon.createPolygon());
    EXPECT_FALSE(polygon.getComponent<MeshPolygonComponent>().needUpdatePolygon);
}

TEST(CameralessUpdate, UIPolygonWaitsForACamera) {
    Scene scene;
    Polygon polygon(&scene);
    polygon.addVertex(0.0f, 0.0f);
    polygon.addVertex(10.0f, 0.0f);
    polygon.addVertex(0.0f, 10.0f);

    EXPECT_FALSE(polygon.createPolygon());
    EXPECT_TRUE(polygon.getComponent<PolygonComponent>().needUpdatePolygon);

    Camera camera(&scene);
    scene.setCamera(camera.getEntity());
    EXPECT_TRUE(polygon.createPolygon());
    EXPECT_FALSE(polygon.getComponent<PolygonComponent>().needUpdatePolygon);
}
