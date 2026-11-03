// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 3D physics events reach their subscribers on the main thread: Jolt reports contacts and
// activations from its job threads, where a Lua subscriber would run Lua off the Lua thread
// (LuaBinding's thread check aborts; tools/ci/upgrade_from_main found a game crash there). The
// contact and activation events are queued during the step and delivered after it, in a fixed
// order (2D's contact events come after the step too); shouldCollide3D, which answers inside the
// step, runs it on the calling thread. A setting changed on a delivered Contact3D applies to that
// contact from the next step.

#include "Engine.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "object/physics/Contact3D.h"
#include "object/physics/Rigidbody.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    class ContactEvents3DTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        Entity box(Vector3 position, Vector3 size, BodyType type){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
            RigidbodyComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider3DType::BOX;
            rigidbody.colliders[0].width = size.x;
            rigidbody.colliders[0].height = size.y;
            rigidbody.colliders[0].depth = size.z;
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        // a floor and a pile of boxes falling on it: contacts in many pairs at once, so Jolt
        // spreads its collision work over its job threads
        std::vector<Entity> pile(int columns = 6, int rows = 4){
            box(Vector3(0.0f, -0.5f, 0.0f), Vector3(40.0f, 1.0f, 40.0f), BodyType::STATIC);
            std::vector<Entity> boxes;
            for (int y = 0; y < rows; y++){
                for (int x = 0; x < columns; x++){
                    for (int z = 0; z < columns; z++){
                        boxes.push_back(box(Vector3(x * 1.05f - 3.0f, 0.6f + y * 1.05f, z * 1.05f - 3.0f), Vector3(1.0f, 1.0f, 1.0f),
                                            BodyType::DYNAMIC));
                    }
                }
            }
            return boxes;
        }

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        void steps(int count){
            for (int i = 0; i < count; i++){
                resolveTransforms();
                scene->fixedUpdate(DT);
                resolveTransforms();
            }
        }
    };

    struct Call {
        std::thread::id thread;
        bool inStep;
    };

}

TEST_F(ContactEvents3DTest, ContactAndActivationEventsArriveOnTheMainThreadAfterTheStep){
    pile();
    const std::thread::id mainThread = std::this_thread::get_id();
    std::vector<Call> calls;
    auto record = [&](){ calls.push_back({std::this_thread::get_id(), physics->isSteppingWorld3D()}); };
    physics->onContactAdded3D.add("test", std::function<void(Rigidbody, Rigidbody, Contact3D)>([&](Rigidbody, Rigidbody, Contact3D){ record(); }));
    physics->onContactPersisted3D.add("test", std::function<void(Rigidbody, Rigidbody, Contact3D)>([&](Rigidbody, Rigidbody, Contact3D){ record(); }));
    physics->onContactRemoved3D.add("test", std::function<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>(
        [&](Rigidbody, Rigidbody, unsigned long, unsigned long){ record(); }));
    physics->onBodyActivated3D.add("test", std::function<void(Rigidbody)>([&](Rigidbody){ record(); }));
    physics->onBodyDeactivated3D.add("test", std::function<void(Rigidbody)>([&](Rigidbody){ record(); }));

    steps(120);

    ASSERT_GT(calls.size(), 100u);
    size_t inStep = 0;
    size_t offThread = 0;
    for (const Call& call : calls){
        if (call.inStep) inStep++;
        if (call.thread != mainThread) offThread++;
    }
    EXPECT_EQ(inStep, 0u) << "events delivered inside the step (on Jolt's threads)";
    EXPECT_EQ(offThread, 0u) << "events delivered off the main thread";
}

TEST_F(ContactEvents3DTest, ShouldCollideRunsOnTheMainThread){
    pile(4, 2);
    const std::thread::id mainThread = std::this_thread::get_id();
    size_t calls = 0;
    size_t offThread = 0;
    physics->shouldCollide3D.add("test", std::function<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>(
        [&](Rigidbody, Rigidbody, Vector3, CollideShapeResult3D){
            calls++;
            if (std::this_thread::get_id() != mainThread) offThread++;
            return true;
        }));
    steps(60);
    ASSERT_GT(calls, 0u);
    EXPECT_EQ(offThread, 0u) << "shouldCollide3D answered on Jolt's threads";
}

TEST_F(ContactEvents3DTest, DeliveryOrderIsTheSameEveryRun){
    auto run = [this](){
        TearDown();
        SetUp();
        pile(5, 3);
        std::vector<std::tuple<int, Entity, Entity>> order;
        physics->onContactAdded3D.add("test", std::function<void(Rigidbody, Rigidbody, Contact3D)>([&](Rigidbody a, Rigidbody b, Contact3D){
            order.emplace_back(0, a.getEntity(), b.getEntity());
        }));
        physics->onContactPersisted3D.add("test", std::function<void(Rigidbody, Rigidbody, Contact3D)>([&](Rigidbody a, Rigidbody b, Contact3D){
            order.emplace_back(1, a.getEntity(), b.getEntity());
        }));
        steps(60);
        return order;
    };
    const auto first = run();
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(run(), first);
}

TEST_F(ContactEvents3DTest, SettingChangedOnADeliveredContactAppliesFromTheNextStep){
    const Entity floor = box(Vector3(0.0f, -0.5f, 0.0f), Vector3(40.0f, 1.0f, 40.0f), BodyType::STATIC);
    const Entity faller = box(Vector3(0.0f, 0.6f, 0.0f), Vector3(1.0f, 1.0f, 1.0f), BodyType::DYNAMIC);
    int touched = 0;
    physics->onContactAdded3D.add("test", std::function<void(Rigidbody, Rigidbody, Contact3D)>([&](Rigidbody a, Rigidbody b, Contact3D contact){
        if ((a.getEntity() == floor || b.getEntity() == floor) && (a.getEntity() == faller || b.getEntity() == faller)){
            touched++;
            // the floor lets this box through from now on
            contact.setIsSensor(true);
            EXPECT_TRUE(contact.isSensor());
        }
    }));
    steps(90);
    EXPECT_GE(touched, 1);
    EXPECT_LT(Rigidbody(scene.get(), faller).getPosition().y, -2.0f) << "the box went through the floor once the contact was a sensor";
}
