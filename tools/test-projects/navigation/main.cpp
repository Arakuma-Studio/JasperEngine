// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S19's test project (docs/specs/S19-navigation.md AC20, AC27): a reduced bench_nav_city
// (a 64 x 64 m floor with 24 buildings, render meshes) and nav_2d_topdown (a plane XY surface
// over static Box2D polygons and circles) bake at runtime (NavMeshSurface:bakeAsync). In a
// single-threaded web build the tiles are time-sliced on the main thread: no slice may exceed
// the web bake budget (4 ms) + 2 ms. The navmesh files' hashes are logged: a threaded build gives
// the same bytes (tools/web/gates/navigation.json holds them for both). Then an agent walks the
// city and arrives (onDestinationReached), and a grid path is found. Last, S53's carving fixture
// (NavObstacleTest's walled gap, docs/specs/S53-navigation-fixes.md AC21): a carving box closes
// the gap within the tile budget of Carve Tiles Per Tick, and opens it again when it goes.
//
//   BUILD_TYPE=Release WEB_PROJECT=navigation tools/build.sh web ; GATE=navigation tools/build.sh web-gate
//   BUILD_TYPE=Release WEB_THREADS=1 WEB_PROJECT=navigation tools/build.sh web ; WEB_THREADS=1 GATE=navigation tools/build.sh web-gate
// (the slice budget is a release budget: a Debug wasm build's Recast stages are several times slower)
//   ANDROID_PROJECT=navigation tools/build.sh android (tools/android-emulator-test.sh --expect-file
//   tools/test-projects/navigation/android-expect.txt --golden tools/test-projects/navigation/golden:
//   the city under a sun, as the emulator shows it)

#include "JasperEngine.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);
// (the screen golden's: a light and colours, which the bake does not read)
Light sun(&scene);

namespace {

    Entity cityFloor = NULL_ENTITY;
    Entity topDownSurface = NULL_ENTITY;
    Entity walker = NULL_ENTITY;
    Entity grid = NULL_ENTITY;
    int frame = 0;
    bool baked = false;
    bool arrived = false;
    bool sent = false;
    int round = 0;
    bool reported = false;
    // the carving fixture: its surface, the box closing its gap, and the step it is at
    Entity gapFloor = NULL_ENTITY;
    Entity blocker = NULL_ENTITY;
    int carving = 0;
    int carvingFrame = 0;
    const Vector3 GAP_ORIGIN(-200.0f, 0.0f, 0.0f);

    uint64_t fnv(const std::vector<uint8_t>& bytes) {
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t b : bytes) {
            hash ^= b;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    Entity box(float x, float y, float z, float w, float h, float d) {
        Primitive* shape = new Primitive(&scene);
        shape->createBox(w, h, d);
        shape->setPosition(x, y, z);
        shape->setColor(0.85f, 0.45f, 0.2f);
        return shape->getEntity();
    }

    Entity empty(const Vector3& position) {
        GameObject* object = new GameObject(&scene);
        object->setPosition(position);
        return object->getEntity();
    }

    void buildCity() {
        Primitive* floor = new Primitive(&scene);
        floor->createPlane(64.0f, 64.0f);
        floor->setColor(0.55f, 0.55f, 0.5f);
        cityFloor = floor->getEntity();
        NavMeshSurfaceComponent surface;
        surface.useGeometry = NavUseGeometry::RENDER_MESHES;
        surface.overrideCellSize = true;
        surface.cellSize = 0.2f;
        surface.overrideTileSize = true;
        surface.tileSize = 64;
        scene.addComponent<NavMeshSurfaceComponent>(cityFloor, surface);
        std::mt19937 random(19);
        std::uniform_real_distribution<float> footprint(2.0f, 5.0f), tall(2.0f, 8.0f);
        for (int i = 0; i < 24; i++) {
            const float x = -26.0f + 10.5f * (float)(i % 6), z = -24.0f + 12.0f * (float)(i / 6);
            const float w = footprint(random), d = footprint(random), h = tall(random);
            box(x, h * 0.5f, z, w, h, d);
        }
    }

    void buildTopDown() {
        // far from the city, on the XY plane (2D points; agents in metres)
        topDownSurface = empty(Vector3(0.0f, 0.0f, 500.0f));
        NavMeshSurfaceComponent surface;
        surface.plane = nav::NavPlane::XY;
        surface.collectObjects = NavCollectObjects::VOLUME;
        surface.volumeCenter = Vector3(320.0f, 240.0f, 0.0f);
        surface.volumeSize = Vector3(640.0f, 480.0f, 10.0f);
        scene.addComponent<NavMeshSurfaceComponent>(topDownSurface, surface);
        const Vector2 boxes[] = {{150, 150}, {350, 125}, {250, 325}, {475, 300}};
        for (const Vector2& centre : boxes) {
            const Entity entity = empty(Vector3(centre.x, centre.y, 500.0f));
            Rigidbody2DComponent body;
            body.type = BodyType::STATIC;
            body.colliders[0].type = Collider2DType::POLYGON;
            body.colliders[0].numVertices = 4;
            body.colliders[0].vertices[0] = Vector2(-40, -20);
            body.colliders[0].vertices[1] = Vector2(40, -20);
            body.colliders[0].vertices[2] = Vector2(40, 20);
            body.colliders[0].vertices[3] = Vector2(-40, 20);
            body.numColliders = 1;
            scene.addComponent<Rigidbody2DComponent>(entity, body);
        }
        for (const Vector3& circle : {Vector3(100, 350, 35), Vector3(525, 125, 45)}) {
            const Entity entity = empty(Vector3(circle.x, circle.y, 500.0f));
            Rigidbody2DComponent body;
            body.type = BodyType::STATIC;
            body.colliders[0].type = Collider2DType::CIRCLE;
            body.colliders[0].radius = circle.z;
            body.numColliders = 1;
            scene.addComponent<Rigidbody2DComponent>(entity, body);
        }
    }

    // 30 x 10 m walled at x = 0 but for a 2 m gap, on a surface with Runtime Carving
    void buildGap() {
        Primitive* floor = new Primitive(&scene);
        floor->createPlane(30.0f, 10.0f);
        floor->setPosition(GAP_ORIGIN);
        floor->setColor(0.5f, 0.6f, 0.5f);
        gapFloor = floor->getEntity();
        box(GAP_ORIGIN.x, 1.0f, -3.25f, 1.0f, 2.0f, 3.5f);
        box(GAP_ORIGIN.x, 1.0f, 3.25f, 1.0f, 2.0f, 3.5f);
        NavMeshSurfaceComponent surface;
        surface.overrideCellSize = true;
        surface.cellSize = 0.2f;
        surface.overrideTileSize = true;
        surface.tileSize = 32;
        surface.runtimeCarving = true;
        scene.addComponent<NavMeshSurfaceComponent>(gapFloor, surface);
    }

    // whether a path crosses the gap
    bool gapOpen() {
        return scene.getSystem<NavigationSystem>()->calculatePathOn(gapFloor, GAP_ORIGIN + Vector3(-10.0f, 0.0f, 0.0f),
                                                                     GAP_ORIGIN + Vector3(10.0f, 0.0f, 0.0f)).status == nav::PathStatus::Complete;
    }

    // after the city: bake the gap, close it with a carving box, open it again
    void runCarving() {
        auto system = scene.getSystem<NavigationSystem>();
        switch (carving) {
            case 0:
                carving = 1;
                NavMeshSurface(&scene, gapFloor).bakeAsync();
                return;
            case 1:
                if (system->isBaking(gapFloor))
                    return;
                if (!gapOpen()) {
                    Log::error("[gate] nav carving: the baked gap is closed");
                    carving = 5;
                    return;
                }
                {
                    blocker = empty(GAP_ORIGIN + Vector3(0.0f, 1.0f, 0.0f));
                    NavMeshObstacleComponent obstacle;
                    obstacle.size = Vector3(1.5f, 2.0f, 3.0f);
                    obstacle.carve = true;
                    obstacle.carveOnlyStationary = false;
                    scene.addComponent<NavMeshObstacleComponent>(blocker, obstacle);
                }
                carving = 2;
                carvingFrame = frame;
                return;
            case 2:
                if (gapOpen()) {
                    if (frame - carvingFrame > 120) {
                        Log::error("[gate] nav carving: the box did not close the gap");
                        carving = 5;
                    }
                    return;
                }
                Log::print("[gate] nav carving cut");
                scene.destroyEntity(blocker);
                carving = 3;
                carvingFrame = frame;
                return;
            case 3:
                if (!gapOpen()) {
                    if (frame - carvingFrame > 120) {
                        Log::error("[gate] nav carving: the gap did not open again");
                        carving = 5;
                    }
                    return;
                }
                Log::print("[gate] nav carving restored");
                carving = 4;
                return;
            default:
                return;
        }
    }

    void buildAgentAndGrid() {
        walker = empty(Vector3(-30.0f, 0.0f, -30.0f));
        NavMeshAgentComponent agent;
        agent.speed = 6.0f;
        agent.acceleration = 20.0f;
        scene.addComponent<NavMeshAgentComponent>(walker, agent);
        grid = empty(Vector3(100.0f, 0.0f, 0.0f));
        NavGridComponent cells;
        cells.width = 24;
        cells.height = 16;
        scene.addComponent<NavGridComponent>(grid, cells);
    }

    void report() {
        auto system = scene.getSystem<NavigationSystem>();
        const uint64_t city = fnv(system->getNavMeshFile(cityFloor));
        const uint64_t topDown = fnv(system->getNavMeshFile(topDownSurface));
        Log::print("[gate] nav baked city %016llx topdown %016llx", (unsigned long long)city, (unsigned long long)topDown);
        std::string stages;
        for (const auto& stage : system->getBakeStageMaxMs())
            stages += " " + stage.first + " " + std::to_string(stage.second);
        Log::print("[nav] longest step per stage (ms):%s", stages.c_str());
        const double slice = system->getMaxBakeSliceMs();
        const double budget = nav::getSettings().webBakeBudgetMs + 2.0;
        if (slice <= budget)
            Log::print("[gate] nav slices within budget");
        else
            Log::error("[gate] nav slice %.2f ms over %.2f ms (slowest stage %.2f ms: %s)", slice, budget,
                       system->getSlowestBakeStepMs(), system->getSlowestBakeStep().c_str());
        NavGrid cells(&scene, grid);
        cells.setCellArea(nav::GridCell{12, 0}, nav::AREA_NOT_WALKABLE);
        const nav::GridPath path = cells.findPath(nav::GridCell{0, 0}, nav::GridCell{23, 15}, nav::GridQuery());
        Log::print("[gate] nav grid path %s %zu cells", nav::pathStatusName(path.status), path.cells.size());
    }

    void onUpdate() {
        frame++;
        auto system = scene.getSystem<NavigationSystem>();
        // a first bake warms the code up (a browser compiles wasm functions on their first
        // call, which no budget covers); the second one is timed and hashed
        if (frame == 2 || (round == 1 && !system->isAnyBaking())) {
            round++;
            system->resetBakeSliceStats();
            NavMeshSurface(&scene, cityFloor).bakeAsync();
            NavMeshSurface(&scene, topDownSurface).bakeAsync();
            return;
        }
        if (round == 2 && !baked && !system->isAnyBaking()) {
            baked = true;
            report();
        }
        // once the agent stands on the baked navmesh (the next fixed step places it), it walks
        if (baked && !sent) {
            NavMeshAgent agent(&scene, walker);
            if (agent.isOnNavMesh()) {
                sent = true;
                agent.getOnDestinationReached() = std::function<void(NavMeshAgent)>([](NavMeshAgent) { arrived = true; });
                if (!agent.setDestination(Vector3(30.0f, 0.0f, 30.0f)))
                    Log::error("[gate] nav setDestination failed");
            }
        }
        if (sent && !arrived && frame % 120 == 0) {
            NavMeshAgent agent(&scene, walker);
            const Vector3 p = agent.getEntity() != NULL_ENTITY ? scene.getComponent<Transform>(walker).worldPosition : Vector3();
            Log::print("[nav] agent at %.2f %.2f %.2f, %s, %.2f m left", p.x, p.y, p.z, nav::pathStatusName(agent.getPathStatus()),
                       agent.getRemainingDistance());
        }
        if (arrived && !reported) {
            reported = true;
            Log::print("[gate] nav agent arrived");
        }
        if (reported)
            runCarving();
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    sceneCamera.setPosition(0, 60, 60);
    sceneCamera.setTarget(0, 0, 0);
    scene.setCamera(&sceneCamera);
    scene.setBackgroundColor(0.45f, 0.62f, 0.85f);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.5f, -1.0f, -0.4f);
    sun.setIntensity(3.0f);
    buildCity();
    buildTopDown();
    buildGap();
    buildAgentAndGrid();
    Engine::setScene(&scene);
    Engine::onUpdate.add("navigation-gate", std::function<void()>(onUpdate));
}
