// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_FIXTURES_H
#define NAV_FIXTURES_H

// The navigation fixtures (docs/specs/S19-navigation.md section 7), built in code so the unit
// tests, je-bench-nav and the navigation test project (web gate, Android) share them:
//   bench_nav_city  a 256 x 256 m terrain with 200 static buildings (cell 0.2 m, 400 tiles)
//   nav_2d_topdown  a 2D scene (plane XY, points) with static Box2D polygons and circles
//   nav_tilemap     a tilemap whose rects map to grid areas
//   nav_doors       two rooms, a doorway, a mud strip and a bridge link to a platform (S76)
// Scenes are plain Scenes with no view: MeshSystem builds the primitives' CPU buffers, which is all
// a bake reads.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "component/Rigidbody2DComponent.h"
#endif

#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace jasperengine::navtest {

    // the world poses of every Transform, as the render pass resolves them between frames
    inline void resolveTransforms(Scene& scene) {
        auto transforms = scene.getComponentArray<Transform>();
        // parents first: entities are created parent before child in these fixtures
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < transforms->size(); i++) {
                Transform& transform = transforms->getComponentFromIndex(i);
                transform.needUpdate = true;
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }
    }

    inline Entity transformEntity(Scene& scene, const Vector3& position, Entity parent = NULL_ENTITY, const std::string& name = "") {
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity);
        if (parent != NULL_ENTITY)
            scene.addEntityChild(parent, entity, false);
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.position = position;
        transform.needUpdate = true;
        if (!name.empty())
            scene.setEntityName(entity, name);
        return entity;
    }

    // a static box mesh (its centre at `center`)
    inline Entity box(Scene& scene, const Vector3& center, const Vector3& size, Entity parent = NULL_ENTITY) {
        const Entity entity = transformEntity(scene, center, parent);
        scene.addComponent<MeshComponent>(entity);
        scene.getSystem<MeshSystem>()->createBox(scene.getComponent<MeshComponent>(entity), size.x, size.y, size.z);
        return entity;
    }

    // a flat plane mesh facing up at `center`
    inline Entity plane(Scene& scene, const Vector3& center, float width, float depth, Entity parent = NULL_ENTITY) {
        const Entity entity = transformEntity(scene, center, parent);
        scene.addComponent<MeshComponent>(entity);
        scene.getSystem<MeshSystem>()->createPlane(scene.getComponent<MeshComponent>(entity), width, depth);
        return entity;
    }

    inline NavMeshSurfaceComponent& addSurface(Scene& scene, Entity entity, float cellSize = 0.2f, int tileSize = 64) {
        NavMeshSurfaceComponent surface;
        surface.overrideCellSize = true;
        surface.cellSize = cellSize;
        surface.overrideTileSize = true;
        surface.tileSize = tileSize;
        scene.addComponent<NavMeshSurfaceComponent>(entity, surface);
        return scene.getComponent<NavMeshSurfaceComponent>(entity);
    }

    inline Entity agent(Scene& scene, const Vector3& position, float speed = 3.5f, float radius = 0.5f) {
        const Entity entity = transformEntity(scene, position);
        NavMeshAgentComponent component;
        component.speed = speed;
        component.radius = radius;
        scene.addComponent<NavMeshAgentComponent>(entity, component);
        return entity;
    }

    // An 8-bit heightmap the terrain samples (kept alive by the caller)
    struct Heightmap {
        std::shared_ptr<std::vector<uint8_t>> pixels;
        int size = 0;
    };

    // gentle hills: 0..1 normalized heights on a size x size map
    inline Heightmap hills(int size, uint32_t seed) {
        Heightmap map;
        map.size = size;
        map.pixels = std::make_shared<std::vector<uint8_t>>((size_t)size * (size_t)size);
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> phase(0.0f, 6.2831853f);
        const float px = phase(random), pz = phase(random);
        for (int z = 0; z < size; z++) {
            for (int x = 0; x < size; x++) {
                const float u = (float)x / (float)(size - 1), v = (float)z / (float)(size - 1);
                const float h = 0.5f + 0.25f * std::sin(u * 6.2831853f * 1.5f + px) * std::cos(v * 6.2831853f * 1.25f + pz);
                (*map.pixels)[(size_t)z * (size_t)size + (size_t)x] = (uint8_t)std::lround(std::fmin(1.0f, std::fmax(0.0f, h)) * 255.0f);
            }
        }
        return map;
    }

    inline Entity terrain(Scene& scene, const Vector3& center, float size, float maxHeight, const Heightmap& heights) {
        const Entity entity = transformEntity(scene, center);
        TerrainComponent component;
        component.terrainSize = size;
        component.maxHeight = maxHeight;
        // a texture id of its own every time: the texture pool keeps ids across scenes, and an
        // earlier test's id would hand back its (freed) pixels
        static int serial = 0;
        component.heightMap.setData("navtest-heights-" + std::to_string(++serial),
                                    TextureData(heights.size, heights.size, (unsigned int)heights.pixels->size(), ColorFormat::RED, 1,
                                                heights.pixels->data()));
        scene.addComponent<TerrainComponent>(entity, component);
        return entity;
    }

    struct City {
        Entity surface = NULL_ENTITY;
        Entity terrain = NULL_ENTITY;
        std::vector<Entity> buildings;
        Heightmap heights;
        float size = 256.0f;
    };

    // bench_nav_city: terrain + `buildings` static boxes over size x size metres, cell 0.2 m,
    // tiles of 64 cells (12.8 m: 400 tiles at 256 m). `reduced` is the web gate's quarter.
    inline City city(Scene& scene, float size = 256.0f, int buildings = 200, uint32_t seed = 19) {
        City city;
        city.size = size;
        city.heights = hills(257, seed);
        city.terrain = terrain(scene, Vector3(0.0f, 0.0f, 0.0f), size, 6.0f, city.heights);
        std::mt19937 random(seed * 7919u + 1u);
        std::uniform_real_distribution<float> footprint(4.0f, 9.0f);
        std::uniform_real_distribution<float> tall(3.0f, 14.0f);
        const int perSide = (int)std::ceil(std::sqrt((float)buildings));
        const float spacing = size / (float)(perSide + 1);
        for (int i = 0; i < buildings; i++) {
            const int gx = i % perSide, gz = i / perSide;
            const float x = -size * 0.5f + spacing * (float)(gx + 1);
            const float z = -size * 0.5f + spacing * (float)(gz + 1);
            const Vector3 boxSize(footprint(random), tall(random), footprint(random));
            city.buildings.push_back(box(scene, Vector3(x, boxSize.y * 0.5f + 1.5f, z), boxSize));
        }
        city.surface = transformEntity(scene, Vector3(), NULL_ENTITY, "City Surface");
        addSurface(scene, city.surface, 0.2f, 64);
        resolveTransforms(scene);
        return city;
    }

    // nav_doors (S76): two rooms of a 40 x 12 m floor split by a wall at x = 0 with a 2.5 m doorway
    // (z in [-1.25, 1.25]), a strip of area 3 across the east room (x in [8, 10]), and a platform
    // (x in [23, 31]) beyond a 3 m gap, reached only by the bridge link from (19, 0, 0) to
    // (24, 0, 0): without the bridge the platform is a second island. Cell 0.2 m, tiles of 32
    // cells (6.4 m). The doorway's runtime volume is added by its tests.
    struct Doors {
        Entity surface = NULL_ENTITY;
        Entity floor = NULL_ENTITY;
        Entity platform = NULL_ENTITY;
        Entity mud = NULL_ENTITY;
        Entity bridge = NULL_ENTITY;
        // the doorway's centre and size (the wall's thickness, the gap's width, 3 m up)
        Vector3 doorCenter = Vector3(0.0f, 1.5f, 0.0f);
        Vector3 doorSize = Vector3(2.0f, 3.0f, 3.0f);
        // a point in each room and on the platform
        Vector3 west = Vector3(-10.0f, 0.0f, 0.0f);
        Vector3 east = Vector3(14.0f, 0.0f, 0.0f);
        Vector3 island = Vector3(28.0f, 0.0f, 0.0f);
    };

    inline Doors doors(Scene& scene) {
        Doors fixture;
        fixture.floor = plane(scene, Vector3(), 40.0f, 12.0f);
        fixture.platform = plane(scene, Vector3(27.0f, 0.0f, 0.0f), 8.0f, 8.0f);
        // the wall, either side of the doorway
        box(scene, Vector3(0.0f, 1.5f, -3.625f), Vector3(1.0f, 3.0f, 4.75f));
        box(scene, Vector3(0.0f, 1.5f, 3.625f), Vector3(1.0f, 3.0f, 4.75f));

        fixture.mud = transformEntity(scene, Vector3(9.0f, 0.0f, 0.0f), NULL_ENTITY, "Mud");
        NavMeshModifierComponent mud;
        mud.useVolume = true;
        mud.overrideArea = true;
        mud.area = 3;
        mud.volumeCenter = Vector3(0.0f, 0.0f, 0.0f);
        mud.volumeSize = Vector3(2.0f, 4.0f, 14.0f);
        scene.addComponent<NavMeshModifierComponent>(fixture.mud, mud);

        fixture.bridge = transformEntity(scene, Vector3(21.5f, 0.0f, 0.0f), NULL_ENTITY, "Bridge");
        NavMeshLinkComponent bridge;
        bridge.startPoint = Vector3(-2.5f, 0.0f, 0.0f);
        bridge.endPoint = Vector3(2.5f, 0.0f, 0.0f);
        bridge.bidirectional = true;
        scene.addComponent<NavMeshLinkComponent>(fixture.bridge, bridge);

        fixture.surface = transformEntity(scene, Vector3(), NULL_ENTITY, "Doors Surface");
        addSurface(scene, fixture.surface, 0.2f, 32);
        resolveTransforms(scene);
        return fixture;
    }

    // nav_doors' doorway as a runtime volume (S76 R11): a NavMesh Modifier with Use Volume and
    // Apply At Runtime over the gap, Not Walkable (a closed door) unless told otherwise
    inline Entity doorway(Scene& scene, const Doors& fixture, bool closed = true, int area = nav::AREA_NOT_WALKABLE) {
        const Entity door = transformEntity(scene, fixture.doorCenter, NULL_ENTITY, "Door");
        NavMeshModifierComponent modifier;
        modifier.enabled = closed;
        modifier.useVolume = true;
        modifier.applyAtRuntime = true;
        modifier.area = area;
        modifier.volumeCenter = Vector3(0.0f, 0.0f, 0.0f);
        modifier.volumeSize = fixture.doorSize;
        scene.addComponent<NavMeshModifierComponent>(door, modifier);
        resolveTransforms(scene);
        return door;
    }

    // nav_2d_topdown: a 2D surface (plane XY, scene points) over 1280 x 960 points with static
    // Box2D polygons and circles (needs JASPERENGINE_PHYSICS_2D)
    struct TopDown {
        Entity surface = NULL_ENTITY;
        std::vector<Entity> colliders;
        // the footprints, centre and radius (circles) or half extents (boxes), for the checks
        std::vector<Vector3> circles;
        std::vector<std::pair<Vector2, Vector2>> boxes;
    };

#ifdef JASPERENGINE_PHYSICS_2D
    inline TopDown topDown(Scene& scene) {
        TopDown fixture;
        fixture.surface = transformEntity(scene, Vector3(), NULL_ENTITY, "2D Surface");
        NavMeshSurfaceComponent& surface = addSurface(scene, fixture.surface, 0.0f, 64);
        surface.overrideCellSize = false;
        surface.plane = nav::NavPlane::XY;
        surface.collectObjects = NavCollectObjects::VOLUME;
        surface.volumeCenter = Vector3(640.0f, 480.0f, 0.0f);
        surface.volumeSize = Vector3(1280.0f, 960.0f, 10.0f);
        auto staticBody = [&](const Vector3& position) {
            const Entity entity = transformEntity(scene, position);
            Rigidbody2DComponent body;
            body.type = BodyType::STATIC;
            scene.addComponent<Rigidbody2DComponent>(entity, body);
            fixture.colliders.push_back(entity);
            return entity;
        };
        const Vector2 boxes[] = {{300.0f, 300.0f}, {700.0f, 250.0f}, {500.0f, 650.0f}, {950.0f, 600.0f}};
        const Vector2 halves[] = {{80.0f, 40.0f}, {40.0f, 120.0f}, {120.0f, 30.0f}, {60.0f, 60.0f}};
        for (int i = 0; i < 4; i++) {
            const Entity entity = staticBody(Vector3(boxes[i].x, boxes[i].y, 0.0f));
            Rigidbody2DComponent& body = scene.getComponent<Rigidbody2DComponent>(entity);
            body.colliders.resize(1);
            Collider2D& collider = body.colliders[0];
            collider.type = Collider2DType::POLYGON;
            collider.points.resize(4);
            collider.points[0] = Vector2(-halves[i].x, -halves[i].y);
            collider.points[1] = Vector2(halves[i].x, -halves[i].y);
            collider.points[2] = Vector2(halves[i].x, halves[i].y);
            collider.points[3] = Vector2(-halves[i].x, halves[i].y);
            fixture.boxes.emplace_back(boxes[i], halves[i]);
        }
        const Vector3 circles[] = {{200.0f, 700.0f, 70.0f}, {1050.0f, 250.0f, 90.0f}};
        for (const Vector3& circle : circles) {
            const Entity entity = staticBody(Vector3(circle.x, circle.y, 0.0f));
            Rigidbody2DComponent& body = scene.getComponent<Rigidbody2DComponent>(entity);
            body.colliders.resize(1);
            Collider2D& collider = body.colliders[0];
            collider.type = Collider2DType::CIRCLE;
            collider.pointA = Vector2(0.0f, 0.0f);
            collider.radius = circle.z;
            fixture.circles.push_back(circle);
        }
        resolveTransforms(scene);
        return fixture;
    }
#endif

    // nav_tilemap: a 16 x 12 tilemap of 32-point tiles ("floor", "water", "wall") and a 16 x 12
    // NavGrid on it (plane XY), floor walkable, water area 3, wall Not Walkable
    struct TilemapFixture {
        Entity tilemap = NULL_ENTITY;
        Entity grid = NULL_ENTITY;
    };

    inline char tilemapCell(int x, int y) {
        // a wall across the middle with a door, a pond in a corner, holes left untiled
        if (x == 8 && y != 6)
            return 'w';
        if (x <= 3 && y >= 8)
            return '~';
        if (x == 14 && y == 2)
            return ' ';
        return '.';
    }

    inline TilemapFixture tilemap(Scene& scene) {
        TilemapFixture fixture;
        fixture.tilemap = transformEntity(scene, Vector3(), NULL_ENTITY, "Tilemap");
        TilemapComponent tilemap;
        tilemap.width = 512;
        tilemap.height = 384;
        const char* names[] = {"floor", "water", "wall"};
        for (int i = 0; i < 3; i++) {
            tilemap.tilesRect[(unsigned int)i].name = names[i];
            tilemap.tilesRect[(unsigned int)i].rect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
        }
        tilemap.numTilesRect = 3;
        for (int y = 0; y < 12; y++) {
            for (int x = 0; x < 16; x++) {
                const char c = tilemapCell(x, y);
                if (c == ' ')
                    continue;
                TileData tile;
                tile.rectId = c == '.' ? 0 : (c == '~' ? 1 : 2);
                tile.name = names[tile.rectId];
                tile.position = Vector2((float)x * 32.0f, (float)y * 32.0f);
                tile.width = 32.0f;
                tile.height = 32.0f;
                tilemap.tiles[tilemap.numTiles++] = tile;
            }
        }
        scene.addComponent<TilemapComponent>(fixture.tilemap, tilemap);

        fixture.grid = transformEntity(scene, Vector3(), NULL_ENTITY, "Grid");
        NavGridComponent grid;
        grid.width = 16;
        grid.height = 12;
        grid.cellSize = 32.0f;
        grid.plane = nav::NavPlane::XY;
        grid.source = NavGridSource::TILEMAP;
        grid.sourceEntity = fixture.tilemap;
        grid.tileAreas = {{"floor", nav::AREA_WALKABLE}, {"water", 3}, {"wall", nav::AREA_NOT_WALKABLE}};
        grid.emptyCellArea = nav::AREA_NOT_WALKABLE;
        scene.addComponent<NavGridComponent>(fixture.grid, grid);
        resolveTransforms(scene);
        return fixture;
    }

}

#endif // NAV_FIXTURES_H
