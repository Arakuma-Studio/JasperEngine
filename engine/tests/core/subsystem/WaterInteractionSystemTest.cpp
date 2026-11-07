// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Water Excluders and Water Wakes in a scene (subsystem/WaterInteractionSystem.h,
// docs/superpowers/specs/2026-11-04-water-boats-design.md): which excluders keep water out, the
// hulls that follow their meshes, the body an excluder belongs to, the wakes moving hulls leave,
// and nothing done in a scene without either component.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterWakeComponent.h"
#include "object/Primitive.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/WaterInteractionSystem.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <memory>

using namespace jasperengine;

namespace water_interaction_system_test {

    constexpr float STEP = 1.0f / 30.0f;

    class WaterInteractionTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        WaterInteractionSystem* system = nullptr;

        void SetUp() override{
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            system = scene->getSystem<WaterInteractionSystem>().get();
        }

        void TearDown() override{
            Engine::removeScene(scene.get());
            scene.reset();
        }

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        // a frame: the transforms resolved, then the system's update
        void frame(float dt = STEP){
            resolveTransforms();
            system->update(dt);
        }

        Entity place(const Vector3& position, Entity parent = NULL_ENTITY){
            const Entity entity = test::makeTransformEntity(*scene, parent);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
            return entity;
        }

        void move(Entity entity, const Vector3& position){
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
        }

        Entity box(const Vector3& position, const Vector3& size){
            const Entity entity = place(position);
            WaterExcluderComponent component;
            component.shape = WaterExcluderShape::BOX;
            component.size = size;
            scene->addComponent<WaterExcluderComponent>(entity, component);
            return entity;
        }

        // a library:water lake at y = 0 over x, z in [x0, x1] x [-40, 40]
        Entity lake(float x0 = -40.0f, float x1 = 40.0f, float level = 0.0f){
            const Entity entity = place(Vector3());
            MeshComponent mesh;
            mesh.numSubmeshes = 1;
            mesh.submeshes[0].material.shader = "library:water";
            mesh.aabb = AABB(x0, level, -40.0f, x1, level, 40.0f);
            scene->addComponent<MeshComponent>(entity, mesh);
            return entity;
        }

        // a hull with a Water Wake, 2 m wide and 6 m long along Z, its waterline at y = 0
        Entity hull(const Vector3& position){
            const Entity entity = place(position);
            WaterWakeComponent wake;
            wake.width = 2.0f;
            wake.length = 6.0f;
            scene->addComponent<WaterWakeComponent>(entity, wake);
            return entity;
        }

        float foamAt(float x, float z, Entity water){
            float foam = 0.0f;
            float crest = 0.0f;
            system->sampleWake(x, z, water, foam, crest);
            return foam + crest;
        }
    };

}

using namespace water_interaction_system_test;

TEST_F(WaterInteractionTest, InactiveHiddenOrDisabledKeepNothingOut){
    const Entity dry = box(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f));
    frame();
    const Vector3 point(0.2f, 0.3f, -0.4f);
    EXPECT_EQ(system->findExcluder(point), dry);
    EXPECT_EQ(system->findExcluder(Vector3(3.0f, 0.0f, 0.0f)), NULL_ENTITY);
    EXPECT_EQ(system->getExcluders().size(), 1u);

    // disabled
    scene->getComponent<WaterExcluderComponent>(dry).enabled = false;
    frame();
    EXPECT_EQ(system->findExcluder(point), NULL_ENTITY);
    EXPECT_TRUE(system->getExcluders().empty());
    scene->getComponent<WaterExcluderComponent>(dry).enabled = true;

    // hidden (Doriax a0b400b4e: a hidden excluder cuts no hole)
    scene->getComponent<Transform>(dry).visible = false;
    frame();
    EXPECT_EQ(system->findExcluder(point), NULL_ENTITY);
    scene->getComponent<Transform>(dry).visible = true;

    // inactive
    scene->setEntityActive(dry, false);
    frame();
    EXPECT_EQ(system->findExcluder(point), NULL_ENTITY);
    scene->setEntityActive(dry, true);
    frame();
    EXPECT_EQ(system->findExcluder(point), dry);
}

TEST_F(WaterInteractionTest, HullFollowsTheMesh){
    Primitive cone(scene.get());
    cone.createCylinder(1.0f, 0.0f, 2.0f, 16, 1);
    scene->addComponent<WaterExcluderComponent>(cone.getEntity(), WaterExcluderComponent{});
    frame();
    const WaterInteractionSystem::HullInfo info = system->getHullInfo(cone.getEntity());
#ifdef JASPERENGINE_PHYSICS_3D
    ASSERT_EQ(info.status, WaterHullStatus::READY);
    EXPECT_GT(info.faces, 8u);
    ASSERT_EQ(system->getHullLayers().size(), 1u);
    ASSERT_NE(system->getHullMap(cone.getEntity()), nullptr);
#endif
    const uint64_t builds = system->getHullBuildCount();
    const uint64_t version = system->getHullMapsVersion();

    // moving the GameObject keeps its hull
    cone.setPosition(5.0f, 0.0f, 0.0f);
    frame();
    frame();
    EXPECT_EQ(system->getHullBuildCount(), builds);
    EXPECT_EQ(system->getHullMapsVersion(), version);

    // new geometry: built again, once
    scene->getSystem<MeshSystem>()->createBox(scene->getComponent<MeshComponent>(cone.getEntity()), 3.0f, 1.0f, 3.0f);
    frame();
    frame();
    EXPECT_EQ(system->getHullBuildCount(), builds + 1);
    EXPECT_NE(system->getHullMapsVersion(), version);

    // the excluder goes: its hull and map with it
    scene->removeComponent<WaterExcluderComponent>(cone.getEntity());
    frame();
    EXPECT_TRUE(system->getHullLayers().empty());
    EXPECT_EQ(system->getHullMap(cone.getEntity()), nullptr);
}

TEST_F(WaterInteractionTest, OwnerIsNearestRigidbody){
    const Entity boat = place(Vector3());
    scene->addComponent<RigidbodyComponent>(boat, RigidbodyComponent{});
    const Entity hold = box(Vector3(), Vector3(1.0f, 1.0f, 1.0f));
    scene->addEntityChild(boat, hold, false);
    EXPECT_EQ(WaterInteractionSystem::getExcluderOwner(scene.get(), hold), boat);

    // cargo with its own Rigidbody under the boat, with an excluder under the cargo: the cargo's
    const Entity cargo = place(Vector3(), boat);
    scene->addComponent<RigidbodyComponent>(cargo, RigidbodyComponent{});
    const Entity crate = box(Vector3(), Vector3(1.0f, 1.0f, 1.0f));
    scene->addEntityChild(cargo, crate, false);
    EXPECT_EQ(WaterInteractionSystem::getExcluderOwner(scene.get(), crate), cargo);

    // a static excluder belongs to nobody
    EXPECT_EQ(WaterInteractionSystem::getExcluderOwner(scene.get(), box(Vector3(9, 0, 0), Vector3(1, 1, 1))), NULL_ENTITY);

    // its owner floats through it, the others do not
    frame();
    EXPECT_EQ(system->findExcluder(Vector3(0.1f, 0.1f, 0.1f), boat), crate);
    EXPECT_NE(system->findExcluder(Vector3(0.1f, 0.1f, 0.1f), NULL_ENTITY), NULL_ENTITY);
}

TEST_F(WaterInteractionTest, NoWorkWhenAbsent){
    lake();
    Engine::setScene(scene.get());
    for (int i = 0; i < 30; i++)
        frame();
    EXPECT_EQ(system->getHullBuildCount(), 0u);
    EXPECT_EQ(system->getWakeStepCount(), 0u);
    EXPECT_TRUE(system->getExcluders().empty());
    EXPECT_FALSE(system->isWakeActive());
    EXPECT_FALSE(system->getWakeMap().isAllocated());
    EXPECT_EQ(system->findExcluder(Vector3()), NULL_ENTITY);
}

TEST_F(WaterInteractionTest, WakeBehindAMovingHull){
    const Entity water = lake();
    const Entity boat = hull(Vector3(0.0f, 0.0f, -10.0f));
    // a hull above the water makes none
    const Entity flying = hull(Vector3(10.0f, 5.0f, -10.0f));
    Engine::setScene(scene.get());
    for (int i = 0; i < 60; i++){
        move(boat, Vector3(0.0f, 0.0f, -10.0f + 5.0f * STEP * (float)i));
        move(flying, Vector3(10.0f, 5.0f, -10.0f + 5.0f * STEP * (float)i));
        frame();
    }
    ASSERT_TRUE(system->isWakeActive());
    EXPECT_TRUE(system->getEmitterInfo(boat).emitting);
    EXPECT_NEAR(system->getEmitterInfo(boat).speed, 5.0f, 0.2f);
    EXPECT_FALSE(system->getEmitterInfo(flying).emitting);
    EXPECT_NE(system->getBodyTag(water), 0);

    // the boat is at z = 0: foam behind its stern (z = -2.9), none far ahead or under the flying hull
    EXPECT_GT(foamAt(0.0f, -3.5f, water), 0.1f);
    EXPECT_EQ(foamAt(0.0f, 8.0f, water), 0.0f);
    EXPECT_EQ(foamAt(10.0f, -5.0f, water), 0.0f);
}

TEST_F(WaterInteractionTest, WakeMapReachesAheadOfTheCamera){
    const Entity water = lake();
    // a chase camera 24 m behind the start, looking along +Z and down (its worldDirection is the
    // view's backward axis, as RenderSystem sets it)
    const Entity camera = place(Vector3(0.0f, 9.0f, -24.0f));
    CameraComponent view;
    view.worldDirection = Vector3(0.0f, 0.47f, -0.88f);
    scene->addComponent<CameraComponent>(camera, view);
    scene->setCamera(camera);
    const Entity boat = hull(Vector3(0.0f, 0.0f, -12.0f));
    Engine::setScene(scene.get());
    for (int i = 0; i < 72; i++){
        move(boat, Vector3(0.0f, 0.0f, -12.0f + 5.0f * STEP * (float)i));
        frame();
    }
    ASSERT_TRUE(system->isWakeActive());
    const WaterInteractionSystem::WakeMapInfo map = system->getWakeMapInfo();
    // the map's centre is ahead of the camera, so the wake the camera sees is on it
    EXPECT_GT(map.corner.y + 0.5f * map.area, -24.0f);
    EXPECT_GT(foamAt(0.0f, -3.5f, water), 0.1f);
}

TEST_F(WaterInteractionTest, WakesStayOnTheirWater){
    // two ponds 1 m apart at one level: a hull along the first one's edge leaves no wake on the second
    const Entity first = lake(-40.0f, 0.0f);
    const Entity second = lake(1.0f, 40.0f);
    const Entity boat = hull(Vector3(-2.0f, 0.0f, -20.0f));
    scene->getComponent<WaterWakeComponent>(boat).width = 1.0f;
    scene->getComponent<WaterWakeComponent>(boat).length = 3.0f;
    Engine::setScene(scene.get());
    for (int i = 0; i < 90; i++){
        move(boat, Vector3(-2.0f, 0.0f, -20.0f + 6.0f * STEP * (float)i));
        frame();
    }
    ASSERT_TRUE(system->isWakeActive());
    EXPECT_NE(system->getBodyTag(first), system->getBodyTag(second));
    float on = 0.0f;
    float across = 0.0f;
    for (float z = -20.0f; z < -5.0f; z += 0.5f){
        for (float x = -6.0f; x < 6.0f; x += 0.25f){
            if (x < 0.0f)
                on += foamAt(x, z, first);
            else if (x > 1.0f)
                across += foamAt(x, z, second);
        }
    }
    EXPECT_GT(on, 1.0f);
    EXPECT_EQ(across, 0.0f);
}

TEST_F(WaterInteractionTest, HiddenHullLeavesNoTrail){
    const Entity water = lake();
    const Entity boat = hull(Vector3(-20.0f, 0.0f, 0.0f));
    Engine::setScene(scene.get());
    for (int i = 0; i < 10; i++){
        move(boat, Vector3(-20.0f + 0.1f * (float)i, 0.0f, 0.0f));
        frame();
    }
    // hidden, moved 15 m in small steps, shown again: no wake along the way
    scene->getComponent<Transform>(boat).visible = false;
    for (int i = 0; i < 30; i++){
        move(boat, Vector3(-19.0f + 0.5f * (float)i, 0.0f, 0.0f));
        frame();
    }
    EXPECT_FALSE(system->getEmitterInfo(boat).emitting);
    scene->getComponent<Transform>(boat).visible = true;
    frame();
    float along = 0.0f;
    for (float x = -15.0f; x < 0.0f; x += 0.25f)
        along += foamAt(x, 0.0f, water);
    EXPECT_EQ(along, 0.0f);
}

TEST_F(WaterInteractionTest, TeleportResets){
    const Entity water = lake();
    const Entity boat = hull(Vector3(-20.0f, 0.0f, 0.0f));
    Engine::setScene(scene.get());
    for (int i = 0; i < 10; i++){
        move(boat, Vector3(-20.0f + 0.1f * (float)i, 0.0f, 0.0f));
        frame();
    }
    // a 30 m jump in one frame draws nothing between the places
    move(boat, Vector3(10.0f, 0.0f, 0.0f));
    frame();
    EXPECT_EQ(system->getEmitterInfo(boat).speed, 0.0f);
    float along = 0.0f;
    for (float x = -15.0f; x < 6.0f; x += 0.25f)
        along += foamAt(x, 0.0f, water);
    EXPECT_EQ(along, 0.0f);
}

TEST_F(WaterInteractionTest, PauseFreezesAndStopClears){
    lake();
    const Entity boat = hull(Vector3(-10.0f, 0.0f, 0.0f));
    Engine::setScene(scene.get());
    for (int i = 0; i < 20; i++){
        move(boat, Vector3(-10.0f + 0.15f * (float)i, 0.0f, 0.0f));
        frame();
    }
    ASSERT_TRUE(system->isWakeActive());
    const uint64_t steps = system->getWakeStepCount();

    // paused (a zero delta, the system held): no step, nothing to upload
    system->markWakeUploaded();
    frame(0.0f);
    system->setPaused(true);
    frame();
    system->setPaused(false);
    EXPECT_EQ(system->getWakeStepCount(), steps);
    EXPECT_FALSE(system->isWakeDirty());

    // the scene stops running: every wake goes
    Engine::removeScene(scene.get());
    frame();
    EXPECT_FALSE(system->isWakeActive());
    EXPECT_FALSE(system->getWakeMap().isAllocated());
    EXPECT_FALSE(system->getEmitterInfo(boat).emitting);
}
