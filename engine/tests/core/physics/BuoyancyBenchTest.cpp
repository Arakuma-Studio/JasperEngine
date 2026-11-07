// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Buoyancy's budget (docs/specs/S22-gameplay.md R37, AC17): 100 floating bodies of 4 columns each
// within 0.1 ms a step (desktop), the buoyancy pass alone (WaterSystem::applyBuoyancy, as
// PhysicsSystem::fixedUpdate runs it before the step). Like every *Bench suite the budget is
// enforced with JASPER_TEST_PERF_SLACK (the benchmark run on server E), failing at 1.5x it.

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/BuoyancyComponent.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/WaterVolumeComponent.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/WaterSystem.h"
#include "support/AllocationCounter.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>

using namespace jasperengine;

TEST(BuoyancyBench, HundredBodiesFourColumns){
    constexpr float DT = 1.0f / 60.0f;
    const float updateTime = Engine::getUpdateTime();
    Engine::setUpdateTime(DT);
    Engine::setFixedDeltatime(0.0);
    Engine::latchShaderTime();
    Engine::setFixedDeltatime(DT);
    {
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        test::disableSubSystems(*scene);
        scene->getScheduler().setEnabled("PhysicsSystem", true);
        PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
        physics->setGravity3D(0.0f, -9.81f, 0.0f);

        // a waving 60 x 60 m lake
        const Entity lake = test::makeTransformEntity(*scene);
        WaterVolumeComponent volume;
        volume.size = Vector3(60.0f, 10.0f, 60.0f);
        scene->addComponent<WaterVolumeComponent>(lake, volume);
        MeshComponent mesh;
        mesh.numSubmeshes = 1;
        mesh.submeshes[0].material.shader = "library:water";
        mesh.submeshes[0].material.shaderFeatures = {"WAVES"};
        ShaderUniforms::set(mesh.submeshes[0].material.shaderParams, "waveHeight", Vector4(0.3f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(mesh.submeshes[0].material.shaderParams, "waveLength", Vector4(12.0f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(mesh.submeshes[0].material.shaderParams, "waveSpeed", Vector4(2.0f, 0.0f, 0.0f, 0.0f));
        scene->addComponent<MeshComponent>(lake, mesh);

        // 100 crates on a 10 x 10 grid, half the water's density
        for (int i = 0; i < 100; i++){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(-22.5f + 5.0f * (float)(i % 10), 0.0f, -22.5f + 5.0f * (float)(i / 10));
            transform.needUpdate = true;
            RigidbodyComponent rigidbody;
            rigidbody.type = BodyType::DYNAMIC;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider3DType::BOX;
            rigidbody.colliders[0].density = 500.0f;
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            scene->addComponent<BuoyancyComponent>(entity, BuoyancyComponent{});
        }

        auto resolve = [&](){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t t = 0; t < transforms->size(); t++){
                Transform& transform = transforms->getComponentFromIndex(t);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(t));
            }
        };
        for (int i = 0; i < 30; i++){
            scene->advanceSceneTime(DT);
            resolve();
            scene->fixedUpdate(DT);
            resolve();
        }

        WaterSystem water(scene.get(), physics);
        constexpr int steps = 600;
        const uint64_t evaluations = WaterSystem::getWaveEvaluations();
        double total = 0.0;
        for (int i = 0; i < steps; i++){
            scene->advanceSceneTime(DT);
            const auto start = std::chrono::steady_clock::now();
            water.applyBuoyancy(DT);
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            // keep the bodies moving: the step itself is not timed
            resolve();
            scene->fixedUpdate(DT);
            resolve();
        }
        const double ms = total / steps;
        std::printf("[bench] Buoyancy, 100 bodies x 4 columns %.4f ms a step (budget 0.1 ms desktop)\n", ms);
        // every column of every body sampled the waves each step (twice: the pass and the step)
        EXPECT_GE(WaterSystem::getWaveEvaluations() - evaluations, (uint64_t)steps * 100 * 4);
        if (testsupport::perfSlack() > 0.0){
            EXPECT_LE(ms, 0.1 * 1.5 * testsupport::perfSlack());
        }
    }
    Engine::setFixedDeltatime(0.0);
    Engine::latchShaderTime();
    Engine::setUpdateTime(updateTime);
}
