// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Simulation Culling's ground rule over a Terrain World (docs/specs/culling-group-and-simulation-
// distance.md AC8, AC9): GameObjects over a present tile that is not in memory freeze, whether a
// body would fall through or a script moves them; at the world's edge (absent tiles) they run.
// The world has no focus, so its tiles stay Cold until the test adds one.

#include "TerrainWorldTestSupport.h"
#include "TerrainWorldSceneSupport.h"

#include "Engine.h"
#include "Input.h"
#include "component/CameraComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/SimulationCullingComponent.h"
#include "script/ScriptBase.h"
#include "subsystem/CullingSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"

#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace TerrainWorldResidencyTest_cpp {

using namespace terrainworldtest;
using jasperengine::CameraComponent;
using jasperengine::CullingSystem;
using jasperengine::Engine;
using jasperengine::PhysicsSystem;
using jasperengine::RigidbodyComponent;
using jasperengine::SimulationCullingComponent;
using jasperengine::SimulationCullReason;
using jasperengine::SimulationState;
using jasperengine::Vector3;

#if JASPERENGINE_TERRAIN_WORLD
namespace {

    constexpr float kResidencyDt = 1.0f / 60.0f;

    // moves its GameObject down every onUpdate, as a script that walks it would
    struct SinkingScript : public jasperengine::ScriptBase {
        SinkingScript(Scene* scene, Entity entity): ScriptBase(scene, entity){
            REGISTER_ENGINE_EVENT(onUpdate);
        }
        ~SinkingScript() override{
            UNREGISTER_ENGINE_EVENT(onUpdate);
        }
        void onUpdate(){
            Transform& transform = getScene()->getComponent<Transform>(getEntity());
            transform.position.y -= 0.2f;
            transform.needUpdate = true;
        }
    };

    struct ResidencyWorld : public WorldScene {
        CullingSystem* culling = nullptr;
        Entity camera = NULL_ENTITY;
        bool gameEvents = true;

        ResidencyWorld(): WorldScene(-2, 1){
            Engine::setUpdateTime(kResidencyDt);
            scene.getSystem<PhysicsSystem>()->setGravity3D(0.0f, -9.81f, 0.0f);
            culling = scene.getSystem<CullingSystem>().get();
            camera = scene.createEntity();
            Transform eye;
            eye.position = Vector3(-100.0f, 20.0f, -100.0f);
            scene.addComponent<Transform>(camera, eye);
            scene.addComponent<CameraComponent>(camera, CameraComponent());
            scene.setCamera(camera);
            gameEvents = Engine::onUpdate.isEnabled();
            Engine::pauseGameEvents(false);
        }

        ~ResidencyWorld(){
            Engine::pauseGameEvents(!gameEvents);
        }

        Entity object(const Vector3& position, bool dynamic){
            const Entity entity = scene.createEntity();
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            if (dynamic){
                RigidbodyComponent body;
                body.type = jasperengine::BodyType::DYNAMIC;
                body.colliders.resize(1);
                body.colliders[0].type = jasperengine::Collider3DType::BOX;
                body.colliders[0].width = body.colliders[0].height = body.colliders[0].depth = 1.0f;
                scene.addComponent<RigidbodyComponent>(entity, body);
            }
            return entity;
        }

        void resolveTransforms(){
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate) scene.getSystem<jasperengine::RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        // one engine frame: the start point, a fixed step, the scripts' update, the world's pump,
        // the transforms and the culling update
        void frame(){
            culling->preUpdate();
            resolveTransforms();
            jasperengine::Input::beginFixedStep();
            Engine::onFixedUpdate.call();
            scene.fixedUpdate(kResidencyDt);
            jasperengine::Input::endFixedStep();
            Engine::onUpdate.call();
            if (jasperengine::jobs::hasSystem() && jasperengine::jobs::system().isSingleThreaded()) jasperengine::jobs::system().pump(4.0);
            scene.getSystem<TerrainWorldSystem>()->pump(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
            resolveTransforms();
            culling->update(kResidencyDt);
            Engine::onPostUpdate.call();
        }

        float y(Entity entity){
            return scene.getComponent<Transform>(entity).worldPosition.y;
        }
    };

    // the tile of a local position, as the runtime counts them
    jasperengine::terraincore::TileCoord tileAt(const TerrainWorldRuntime& runtime, float x, float z){
        const float size = runtime.lattice().tileSize();
        return jasperengine::terraincore::TileCoord(static_cast<int>(std::floor(x / size)), static_cast<int>(std::floor(z / size)));
    }

}

TEST(TerrainWorldResidency, GroundNotLoadedFreezes){
    // AC8, the Craftsman defect as a test
    ResidencyWorld world;
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->isReady(); }));
    // 30 m from the camera, over the present tile (-2, -2), which nothing asked for: Cold
    const Vector3 over(-100.0f, 10.0f, -70.0f);
    ASSERT_EQ(runtime->tileState(tileAt(*runtime, over.x, over.z)), jasperengine::terraincore::TileState::Cold);
    const Entity body = world.object(over, true);
    const Entity walker = world.object(over + Vector3(2.0f, 0.0f, 0.0f), false);
    // past the world's edge (no tile there): never frozen by the ground rule
    const Entity edge = world.object(Vector3(-400.0f, 10.0f, -70.0f), false);
    SimulationCullingComponent culling;
    culling.simulationDistance = 10000.0f;
    for (Entity entity : {body, walker, edge}) world.scene.addComponent<SimulationCullingComponent>(entity, culling);
    SinkingScript walking(&world.scene, walker);
    SinkingScript edgeWalking(&world.scene, edge);
    world.scene.getSystem<PhysicsSystem>()->load();

    for (int i = 0; i < 600; i++) world.frame();
    for (Entity entity : {body, walker}){
        const SimulationCullingComponent& c = world.scene.getComponent<SimulationCullingComponent>(entity);
        EXPECT_EQ(c.state, SimulationState::Culled);
        EXPECT_EQ(c.reason, SimulationCullReason::GroundNotLoaded);
        EXPECT_NEAR(world.y(entity), 10.0f, 1e-3f);
    }
    EXPECT_EQ(world.scene.getComponent<SimulationCullingComponent>(edge).state, SimulationState::Full);
    EXPECT_LT(world.y(edge), -100.0f) << "at the world's edge it runs (and sinks)";

    // the tile loads (a T4 focus streams it): both resume within 2 frames of CpuReady
    runtime->addFocus(over.x, over.z);
    const auto tile = tileAt(*runtime, over.x, over.z);
    ASSERT_TRUE(world.pumpUntil([&] {
        const auto state = runtime->tileState(tile);
        return state == jasperengine::terraincore::TileState::CpuReady || state == jasperengine::terraincore::TileState::Uploading ||
               state == jasperengine::terraincore::TileState::Resident;
    }));
    world.frame();
    world.frame();
    for (Entity entity : {body, walker}) EXPECT_EQ(world.scene.getComponent<SimulationCullingComponent>(entity).state, SimulationState::Full);
    const float walkerY = world.y(walker);
    world.frame();
    EXPECT_LT(world.y(walker), walkerY) << "its script runs again";
}

TEST(TerrainWorldResidency, IsAreaLoadedNoAlloc){
    // AC9
    WorldScene world;
    TerrainWorldRuntime* runtime = world.runtime();
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->isReady(); }));
    // one tile in memory, the others Cold
    runtime->addFocus(10.0f, 10.0f);
    ASSERT_TRUE(world.pumpUntil([&] { return runtime->resident(jasperengine::terraincore::TileCoord(0, 0)) != nullptr; }));
    const float size = runtime->lattice().tileSize();
    auto loaded = [](jasperengine::terraincore::TileState state) {
        using jasperengine::terraincore::TileState;
        return state == TileState::CpuReady || state == TileState::Uploading || state == TileState::Resident;
    };
    // the brute force: every present tile whose square the circle touches
    auto expected = [&](float x, float z, float radius) {
        for (int tz = -10; tz <= 10; tz++) {
            for (int tx = -10; tx <= 10; tx++) {
                const jasperengine::terraincore::TileCoord tile(tx, tz);
                if (!runtime->occupancy().present(tile)) continue;
                const float minX = static_cast<float>(tx) * size, minZ = static_cast<float>(tz) * size;
                const float dx = std::max({minX - x, 0.0f, x - (minX + size)});
                const float dz = std::max({minZ - z, 0.0f, z - (minZ + size)});
                if (std::sqrt(dx * dx + dz * dz) > radius) continue;
                if (!loaded(runtime->tileState(tile))) return false;
            }
        }
        return true;
    };
    std::mt19937 random(5);
    std::uniform_real_distribution<float> coordinate(-200.0f, 150.0f);
    std::uniform_real_distribution<float> radii(0.0f, 40.0f);
    int agreeing = 0, loadedAreas = 0;
    std::vector<std::array<float, 3>> areas(1000);
    for (auto& area : areas) area = {coordinate(random), coordinate(random), radii(random)};
    bool answers[1000];
    {
        testsupport::AllocationCountScope allocations;
        for (size_t i = 0; i < areas.size(); i++) answers[i] = runtime->isAreaLoaded(areas[i][0], areas[i][1], areas[i][2]);
        EXPECT_EQ(allocations.count(), 0u);
    }
    for (size_t i = 0; i < areas.size(); i++) {
        if (answers[i] == expected(areas[i][0], areas[i][1], areas[i][2])) agreeing++;
        if (answers[i]) loadedAreas++;
    }
    EXPECT_EQ(agreeing, 1000);
    EXPECT_GT(loadedAreas, 0);
    // the focus's tile is in memory: loaded (whatever its fine collision, which isAreaLoaded never reads)
    EXPECT_TRUE(runtime->isAreaLoaded(32.0f, 32.0f, 1.0f));
}
#endif

}  // namespace TerrainWorldResidencyTest_cpp
