// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Buoyancy in Water Volumes (docs/specs/S22-gameplay.md R30-R32, R37, AC12): Archimedes' columns
// in the fixed step settle a box as deep as its density is of the water's, ride the waves the
// shader draws (the material's, on the scene clock), and cost nothing in a scene without water.

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/BuoyancyComponent.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/WaterVolumeComponent.h"
#include "object/physics/Rigidbody.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/WaterSystem.h"
#include "util/WaterWaves.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    class BuoyancyTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            // the shader clock advances exactly a frame's delta: reproducible waves
            Engine::setFixedDeltatime(0.0);
            Engine::latchShaderTime();
            Engine::setFixedDeltatime(DT);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        void TearDown() override{
            scene.reset();
            Engine::setFixedDeltatime(0.0);
            Engine::latchShaderTime();
            Engine::setUpdateTime(updateTime);
        }

        // a 40 x 40 m volume 20 m deep whose surface is at y = 0
        Entity water(bool waves = false, float waveHeight = 0.3f, float waveLength = 20.0f, float waveSpeed = 4.0f){
            const Entity entity = test::makeTransformEntity(*scene);
            WaterVolumeComponent volume;
            volume.size = Vector3(40.0f, 20.0f, 40.0f);
            volume.waves = WaterVolumeWaves::FROM_MATERIAL;
            scene->addComponent<WaterVolumeComponent>(entity, volume);
            MeshComponent mesh;
            mesh.numSubmeshes = 1;
            mesh.submeshes[0].material.shader = "library:water";
            if (waves){
                mesh.submeshes[0].material.shaderFeatures = {"WAVES"};
                ShaderUniforms::set(mesh.submeshes[0].material.shaderParams, "waveDirection", Vector4(1.0f, 0.0f, 0.0f, 0.0f));
                ShaderUniforms::set(mesh.submeshes[0].material.shaderParams, "waveHeight", Vector4(waveHeight, 0.0f, 0.0f, 0.0f));
                ShaderUniforms::set(mesh.submeshes[0].material.shaderParams, "waveLength", Vector4(waveLength, 0.0f, 0.0f, 0.0f));
                ShaderUniforms::set(mesh.submeshes[0].material.shaderParams, "waveSpeed", Vector4(waveSpeed, 0.0f, 0.0f, 0.0f));
            }
            scene->addComponent<MeshComponent>(entity, mesh);
            return entity;
        }

        // a 1 m cube of a density, its bottom at `bottom`
        Entity box(float density, float bottom, float x = 0.0f){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(x, bottom + 0.5f, 0.0f);
            transform.needUpdate = true;
            RigidbodyComponent rigidbody;
            rigidbody.type = BodyType::DYNAMIC;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider3DType::BOX;
            rigidbody.colliders[0].width = 1.0f;
            rigidbody.colliders[0].height = 1.0f;
            rigidbody.colliders[0].depth = 1.0f;
            rigidbody.colliders[0].density = density;
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            scene->addComponent<BuoyancyComponent>(entity, BuoyancyComponent{});
            return entity;
        }

        // the Transforms written since the last frame resolved, as the frame's render pass does
        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        // a frame of one fixed step: the scene clock advanced by it (as Engine::runFrame does),
        // then the step
        void step(){
            scene->advanceSceneTime(DT);
            resolveTransforms();
            Input::beginFixedStep();
            scene->fixedUpdate(DT);
            Input::endFixedStep();
            resolveTransforms();
        }

        void steps(int count){
            for (int i = 0; i < count; i++)
                step();
        }

        float bottom(Entity entity){
            return scene->getComponent<Transform>(entity).worldPosition.y - 0.5f;
        }

        float verticalSpeed(Entity entity){
            return Rigidbody(scene.get(), entity).getLinearVelocity().y;
        }
    };

}

TEST_F(BuoyancyTest, Equilibrium){
    water();
    const Entity half = box(500.0f, -0.4f, -5.0f);
    const Entity quarter = box(250.0f, -0.15f, 0.0f);
    const Entity heavy = box(1200.0f, -0.5f, 5.0f);
    steps(300);
    // still sinking at 5 s, a metre under the surface
    EXPECT_LT(bottom(heavy) + 1.0f, -1.0f);
    EXPECT_LT(verticalSpeed(heavy), 0.0f);
    steps(300);
    // floating as deep as its density is of the water's: 0.5 and 0.25 m of its 1 m
    EXPECT_NEAR(-bottom(half), 0.50f, 0.02f);
    EXPECT_LT(std::fabs(verticalSpeed(half)), 0.01f);
    EXPECT_NEAR(-bottom(quarter), 0.25f, 0.02f);
    EXPECT_LT(std::fabs(verticalSpeed(quarter)), 0.01f);
}

TEST_F(BuoyancyTest, RidesWaves){
    water(true);
    const Entity floater = box(500.0f, -0.5f);
    steps(120);
    std::vector<float> body, surface;
    for (int i = 0; i < 600; i++){
        step();
        const Vector3 p = scene->getComponent<Transform>(floater).worldPosition;
        body.push_back(p.y - 0.5f);
        surface.push_back(physics->sampleWater(Vector3(p.x, 10.0f, p.z)).surfaceHeight);
    }
    const auto mean = [](const std::vector<float>& v){ return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
    const double meanBody = mean(body);
    const double meanSurface = mean(surface);
    // the flat equilibrium (half under) plus the mean wave height under it
    EXPECT_NEAR(meanBody, -0.5 + meanSurface, 0.02);
    double covariance = 0.0, varianceBody = 0.0, varianceSurface = 0.0;
    for (size_t i = 0; i < body.size(); i++){
        covariance += (body[i] - meanBody) * (surface[i] - meanSurface);
        varianceBody += (body[i] - meanBody) * (body[i] - meanBody);
        varianceSurface += (surface[i] - meanSurface) * (surface[i] - meanSurface);
    }
    const double correlation = covariance / std::sqrt(varianceBody * varianceSurface);
    EXPECT_GT(correlation, 0.8);
}

TEST_F(BuoyancyTest, NoWorkWhenAbsent){
    // bodies, no water: the buoyancy pass returns at once
    for (int i = 0; i < 5; i++){
        const Entity entity = test::makeTransformEntity(*scene);
        RigidbodyComponent rigidbody;
        rigidbody.type = BodyType::DYNAMIC;
        rigidbody.colliders.resize(1);
        rigidbody.colliders[0].type = Collider3DType::BOX;
        scene->addComponent<RigidbodyComponent>(entity, rigidbody);
    }
    const uint64_t before = WaterSystem::getWaveEvaluations();
    steps(60);
    EXPECT_EQ(WaterSystem::getWaveEvaluations(), before);

    // a Buoyancy without any water volume: still nothing
    box(500.0f, 0.0f);
    steps(60);
    EXPECT_EQ(WaterSystem::getWaveEvaluations(), before);
}

TEST_F(BuoyancyTest, SampleMatchesTheWaveTwin){
    water(true, 0.4f, 12.0f, 2.0f);
    step();
    const float time = (float)scene->getSceneTime();
    const WaterSample sample = physics->sampleWater(Vector3(3.0f, -1.0f, 2.0f));
    const WaterWaveSample wave = waterWaves(Vector2(3.0f, 2.0f), time, Vector2(1.0f, 0.0f), 0.4f, 12.0f, 2.0f);
    EXPECT_TRUE(sample.inWater);
    EXPECT_FLOAT_EQ(sample.surfaceHeight, wave.height);
    EXPECT_FLOAT_EQ(sample.flowVelocity.y, wave.heightRate);
    EXPECT_NEAR(physics->sampleWaterHeight(3.0f, 2.0f), wave.height, 1e-6f);
    // outside the volume: no water
    EXPECT_TRUE(std::isnan(physics->sampleWaterHeight(100.0f, 0.0f)));
    EXPECT_FALSE(physics->sampleWater(Vector3(100.0f, -1.0f, 0.0f)).inWater);
}
