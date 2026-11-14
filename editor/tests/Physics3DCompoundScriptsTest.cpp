// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Compound 3D bodies (S96 REQ-18, AC-18): a script on an attached child and on its ancestor both
// hear the child's collider's contact, whose collider is the child's and whose body the ancestor's.

#include <gtest/gtest.h>

#include "ScriptTestHarness.h"

#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"

#include <string>

using namespace jasperengine;

// ---- callbacks: a script on the child and on the ancestor both hear the child's contact ---------

namespace {

    const char* COMPOUND_LOGGER_LUA =
        "local M = {}\n"
        "local function n(go) if go then return go.name else return 'nil' end end\n"
        "function M:onCollisionEnter(c)\n"
        "  log[#log + 1] = 'CE:' .. self.gameObject.name .. ':' .. n(c.gameObject) .. ':' .. n(c.thisCollider.gameObject) .. ':' .. n(c.rigidbody and c.rigidbody:getAttachedObject())\n"
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
