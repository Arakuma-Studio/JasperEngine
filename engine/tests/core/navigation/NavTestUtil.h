// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_TEST_UTIL_H
#define NAV_TEST_UTIL_H

// A scene with only its NavigationSystem running, stepped at 60 Hz as the engine's fixed phase
// does, with the project's navigation settings reset for each test.

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "navigation/NavTypes.h"
#include "subsystem/NavigationSystem.h"
#include "../ecs/EcsTestUtil.h"
#include "NavFixtures.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>

namespace jasperengine::navtest {

    constexpr float DT = 1.0f / 60.0f;

    // a box NavMeshObstacle (carving at once when `carve`)
    inline Entity obstacleBox(Scene& scene, const Vector3& center, const Vector3& size, bool carve) {
        const Entity entity = transformEntity(scene, center);
        NavMeshObstacleComponent obstacle;
        obstacle.shape = NavObstacleShape::BOX;
        obstacle.size = size;
        obstacle.carve = carve;
        obstacle.carveOnlyStationary = false;
        scene.addComponent<NavMeshObstacleComponent>(entity, obstacle);
        return entity;
    }

    class NavSceneTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        NavigationSystem* nav = nullptr;
        float updateTime = 0.0f;

        void SetUp() override {
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            nav::setSettings(nav::NavigationSettings::defaults());
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("NavigationSystem", true);
            nav = scene->getSystem<NavigationSystem>().get();
        }

        void TearDown() override {
            scene.reset();
            nav::setSettings(nav::NavigationSettings::defaults());
            Engine::setUpdateTime(updateTime);
        }

        // a full bake on this thread; the test fails when it does not succeed
        NavBakeResult bake(Entity surface, NavBakeMode mode = NavBakeMode::FULL) {
            resolveTransforms(*scene);
            NavBakeResult result = nav->bakeNow(surface, mode);
            EXPECT_TRUE(result.ok) << result.error;
            return result;
        }

        // one engine fixed step, in the fixed phase: outside it a CharacterController moves by
        // Engine::getDeltatime(), which is whatever frame an earlier test of the process last ran
        void step(const std::function<void()>& scripts = nullptr) {
            resolveTransforms(*scene);
            Input::beginFixedStep();
            if (scripts)
                scripts();
            scene->fixedUpdate(DT);
            Input::endFixedStep();
            resolveTransforms(*scene);
        }

        void steps(int count, const std::function<void()>& scripts = nullptr) {
            for (int i = 0; i < count; i++)
                step(scripts);
        }

        // A bake's tiles build on the job system, in wall-clock time, while a test steps as fast as
        // it can: wait for the bake to land (within a generous minute), not for a tick count
        bool bakeRunning(Entity surface, std::chrono::steady_clock::time_point since) {
            return nav->isBaking(surface) && std::chrono::steady_clock::now() - since < std::chrono::seconds(60);
        }

        Vector3 position(Entity entity) {
            return scene->getComponent<Transform>(entity).worldPosition;
        }

        // a box volume marking `area` (NavMeshModifier with Use Volume)
        Entity areaVolume(const Vector3& center, const Vector3& size, uint8_t area) {
            const Entity entity = transformEntity(*scene, center);
            NavMeshModifierComponent modifier;
            modifier.useVolume = true;
            modifier.overrideArea = true;
            modifier.area = area;
            modifier.volumeCenter = Vector3(0.0f, 0.0f, 0.0f);
            modifier.volumeSize = size;
            scene->addComponent<NavMeshModifierComponent>(entity, modifier);
            return entity;
        }

        // a surface over a w x d plane at the origin
        Entity planeSurface(float width = 20.0f, float depth = 20.0f, float cellSize = 0.2f) {
            plane(*scene, Vector3(), width, depth);
            const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
            addSurface(*scene, surface, cellSize, 32);
            return surface;
        }

        // a 600 x 3 m strip along x on tiles of 16 cells of 0.1 m: a path end to end crosses more
        // polygons than a crowd corridor holds (256, spec S74 AC1)
        Entity stripSurface() {
            plane(*scene, Vector3(), 600.0f, 3.0f);
            const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Strip");
            addSurface(*scene, surface, 0.1f, 16);
            return surface;
        }

        // two 8 x 6 m islands at x = -6 and x = 6, with nothing between them (a 4 m gap)
        Entity islandsSurface(bool carving = false) {
            plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
            plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 6.0f);
            const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
            addSurface(*scene, surface, 0.2f, 32).runtimeCarving = carving;
            return surface;
        }

        // a 30 x 10 m plane walled at x = 0 but for a 2 m gap at z in [-1, 1], on a carving surface
        Entity gapSurface() {
            plane(*scene, Vector3(), 30.0f, 10.0f);
            box(*scene, Vector3(0.0f, 1.0f, -3.25f), Vector3(1.0f, 2.0f, 3.5f));
            box(*scene, Vector3(0.0f, 1.0f, 3.25f), Vector3(1.0f, 2.0f, 3.5f));
            const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
            addSurface(*scene, surface, 0.2f, 32).runtimeCarving = true;
            return surface;
        }
    };

}

#endif // NAV_TEST_UTIL_H
