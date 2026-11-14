// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Compound 3D bodies from child GameObjects (S96 REQ-18, REQ-19, AC-18, AC-19): a Rigidbody with
// Attach To Parent Body has no body of its own, its colliders are sub-shapes of the nearest
// ancestor's body, posed by its Transform, adding their mass; their contacts reach the child and
// the ancestor; a moved child composes the ancestor again in that step, keeping the body.

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"
#include "ScriptTestHarness.h"

#include "physics/AttachedColliders.h"
#include "registry/PhysicsLayers.h"

#include <cmath>
#include <sstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::physics3dtest;

namespace {

    Entity compoundChild3D(World3D& world, Entity parent, const Vector3& local, const Vector3& size,
                           const Quaternion& rotation = Quaternion::IDENTITY, const Vector3& scale = Vector3(1, 1, 1)) {
        const Entity child = world.box(local, size, BodyType::STATIC, parent, rotation);
        world.transform(child).scale = scale;
        world.transform(child).needUpdate = true;
        world.component(child).attachToParentBody = true;
        return child;
    }

}

TEST(Physics3DCompound, ChildrenJoinAncestorBody) {
    World3D world(Vector3(0, 0, 0));
    const Entity car = world.box(Vector3(0, 5, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const Entity rotated = compoundChild3D(world, car, Vector3(-2, 0, 0), Vector3(1, 1, 1), degreesAbout(45.0f, Vector3(0, 1, 0)));
    const Entity scaled = compoundChild3D(world, car, Vector3(2, 0, 0), Vector3(1, 1, 1), Quaternion::IDENTITY, Vector3(2, 1, 1));
    world.load();
    world.step();

    // one body: the children have none of their own (red: three bodies)
    EXPECT_FALSE(world.component(car).body.IsInvalid());
    EXPECT_TRUE(world.component(rotated).body.IsInvalid());
    EXPECT_TRUE(world.component(scaled).body.IsInvalid());
    EXPECT_EQ(world.physics()->getWorld3D()->GetNumBodies(), 1u);
    EXPECT_EQ(world.component(car).attachedColliders.size(), 2u);
    EXPECT_EQ(world.component(rotated).attachedTo, car);
    EXPECT_EQ(world.rb(rotated).getAttachedBody(), car);

    // 1 m^3 + 1 m^3 + 2 m^3 of 1000 kg/m^3
    EXPECT_NEAR(world.rb(car).getMass(), 4000.0f, 40.0f);

    // a ray at the scaled child hits the body, and its collider is the child's
    const RayReturn hit = Ray(Vector3(2.5f, 10, 0), Vector3(0, -20, 0)).intersects(&world.scene, RayFilter::BODY_3D);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, car);
    const ColliderRef collider(&world.scene, hit.body, (int)hit.colliderIndex);
    EXPECT_EQ(collider.entity, scaled);
    EXPECT_EQ(collider.index, 0);
    EXPECT_EQ(collider.getRigidbody().getEntity(), car);
    EXPECT_NEAR(hit.point.y, 5.5f, 1e-3f);

    // the rotated child: a 45 degree turned box reaches sqrt(0.5) from its centre along x
    const RayReturn side = Ray(Vector3(-5, 5, 0), Vector3(10, 0, 0)).intersects(&world.scene, RayFilter::BODY_3D);
    ASSERT_TRUE(side.hit);
    EXPECT_NEAR(side.point.x, -2.0f - std::sqrt(0.5f), 1e-3f);
    EXPECT_EQ(ColliderRef(&world.scene, side.body, (int)side.colliderIndex).entity, rotated);
}

TEST(Physics3DCompound, MovedChildRecomposesOnceKeepingBodyAndVelocity) {
    World3D world(Vector3(0, 0, 0));
    const Entity car = world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const Entity wing = compoundChild3D(world, car, Vector3(2, 0, 0), Vector3(1, 1, 1));
    world.load();
    world.step();
    const JPH::BodyID id = world.component(car).body;
    world.rb(car).setLinearVelocity(Vector3(1, 0, 0));
    world.step();
    EXPECT_EQ(world.component(car).attachedRecomposeSteps, 0) << "nothing moved: no compose";

    world.transform(wing).position = Vector3(3, 0, 0);
    world.transform(wing).needUpdate = true;
    world.step();
    EXPECT_EQ(world.component(car).body, id) << "the shape swapped, the body kept";
    EXPECT_EQ(world.component(car).attachedRecomposeSteps, 1) << "composed once in that step";
    EXPECT_NEAR(world.component(car).attachedColliders[0].collider.position.x, 3.0f, 1e-5f);
    EXPECT_NEAR(world.rb(car).getLinearVelocity().x, 1.0f, 1e-3f);

    world.step();
    EXPECT_EQ(world.component(car).attachedRecomposeSteps, 0);

    // a child that leaves (flag off) makes its own body again; the ancestor drops its collider
    world.rb(wing).setAttachToParentBody(false);
    world.step();
    EXPECT_TRUE(world.component(car).attachedColliders.empty());
    EXPECT_FALSE(world.component(wing).body.IsInvalid());
    EXPECT_EQ(world.rb(wing).getAttachedBody(), NULL_ENTITY);
}

TEST(Physics3DCompound, NoAncestorBodyMakesItsOwn) {
    World3D world(Vector3(0, 0, 0));
    const Entity holder = world.entity(Vector3(0, 0, 0));
    const Entity lone = compoundChild3D(world, holder, Vector3(1, 0, 0), Vector3(1, 1, 1));
    world.load();
    world.step();
    EXPECT_FALSE(world.component(lone).body.IsInvalid());
    EXPECT_EQ(world.rb(lone).getAttachedBody(), NULL_ENTITY);
}

TEST(Physics3DCompound, AnimatedChildWarnsOnce) {
    World3D world(Vector3(0, 0, 0));
    const Entity car = world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const Entity arm = compoundChild3D(world, car, Vector3(2, 0, 0), Vector3(1, 1, 1));
    world.load();
    world.step();
    LogCapture log;
    for (int i = 0; i < 40; i++) {
        world.transform(arm).position = Vector3(2.0f + 0.01f * (float)(i + 1), 0, 0);
        world.transform(arm).needUpdate = true;
        world.step();
    }
    EXPECT_EQ(log.count("moved on"), 1u);
    EXPECT_EQ(log.count("kinematic Rigidbody"), 1u);
}

// ---- callbacks: a script on the child and on the ancestor both hear the child's contact ---------

namespace {

    const char* COMPOUND_LOGGER_LUA =
        "local M = {}\n"
        "local function n(go) if go then return go.name else return 'nil' end end\n"
        "function M:onCollisionEnter(c)\n"
        "  log[#log + 1] = 'CE:' .. self.gameObject.name .. ':' .. n(c.gameObject) .. ':' .. n(c.thisCollider.gameObject) .. ':' .. n(c.rigidbody and c.rigidbody.gameObject)\n"
        "end\n"
        "return M\n";

    class Physics3DCompoundScripts : public scripttest::ScriptTest {
    protected:
        Scene* scene = nullptr;

        void SetUp() override {
            ScriptTest::SetUp();
            scene = newScene();
            scene->setPhysicsInterpolation(false);
            setScene("compoundScene", scene);
            writeScript("CompoundLogger", COMPOUND_LOGGER_LUA);
        }

        void TearDown() override {
            Engine::removeAllScenes();
            lua("compoundScene = nil");
            ScriptTest::TearDown();
        }

        Entity boxBody(const std::string& name, const Vector3& position, BodyType type, const Vector3& size, Entity parent = NULL_ENTITY) {
            const Entity e = entity(scene, name, parent);
            scene->getComponent<Transform>(e).position = position;
            RigidbodyComponent body;
            body.type = type;
            Collider3D collider;
            collider.type = Collider3DType::BOX;
            collider.width = size.x;
            collider.height = size.y;
            collider.depth = size.z;
            body.colliders.push_back(collider);
            scene->addComponent<RigidbodyComponent>(e, body);
            addScript(scene, e, "CompoundLogger");
            return e;
        }

        bool hasEntry(const std::string& entry) {
            return (" " + log() + " ").find(" " + entry + " ") != std::string::npos;
        }
    };

}

TEST_F(Physics3DCompoundScripts, ContactOnChildReachesChildAndAncestor) {
    boxBody("Floor", Vector3(0, -0.5f, 0), BodyType::STATIC, Vector3(40, 1, 40));
    const Entity car = boxBody("Car", Vector3(0, 2, 0), BodyType::DYNAMIC, Vector3(1, 1, 1));
    // lower than the car's own box: it lands first
    const Entity wheel = boxBody("Wheel", Vector3(2, -1, 0), BodyType::STATIC, Vector3(1, 1, 1), car);
    scene->getComponent<RigidbodyComponent>(wheel).attachToParentBody = true;
    LuaBinding::initializeLuaScripts(scene);
    Engine::setScene(scene);
    for (int i = 0; i < 90 && log().find("CE:Wheel") == std::string::npos; i++) {
        frame();
    }
    // the child's collider touched: its GameObject, the ancestor's and the floor's scripts are told,
    // the collider is the child's, the body the ancestor's
    EXPECT_TRUE(hasEntry("CE:Wheel:Floor:Wheel:Floor")) << log();
    EXPECT_TRUE(hasEntry("CE:Car:Floor:Wheel:Floor")) << log();
    EXPECT_TRUE(hasEntry("CE:Floor:Wheel:Floor:Car")) << log();
}
