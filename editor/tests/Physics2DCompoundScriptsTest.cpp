// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Compound 2D bodies (S96 REQ-18, AC-18): a script on an attached child and on its ancestor both
// hear the child's collider's contact, whose collider is the child's and whose body the ancestor's.

#include <gtest/gtest.h>

#include "ScriptTestHarness.h"

#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"

#include <string>

using namespace jasperengine;

// ---- callbacks ---------------------------------------------------------------------------------

namespace {

    const char* COMPOUND2D_LOGGER_LUA =
        "local M = {}\n"
        "local function n(go) if go then return go.name else return 'nil' end end\n"
        "function M:onCollisionEnter2D(c)\n"
        "  log[#log + 1] = 'CE2:' .. self.gameObject.name .. ':' .. n(c.gameObject) .. ':' .. n(c.thisCollider.gameObject) .. ':' .. n(c.rigidbody and c.rigidbody:getAttachedObject())\n"
        "end\n"
        "return M\n";

    class Physics2DCompoundScripts : public scripttest::ScriptTest {
    protected:
        Scene* scene = nullptr;

        void SetUp() override {
            ScriptTest::SetUp();
            scene = newScene();
            scene->setPhysicsInterpolation(false);
            setScene("compound2DScene", scene);
            writeScript("Compound2DLogger", COMPOUND2D_LOGGER_LUA);
        }

        void TearDown() override {
            Engine::removeAllScenes();
            lua("compound2DScene = nil");
            ScriptTest::TearDown();
        }

        Entity boxBody(const std::string& name, const Vector3& position, BodyType type, float width, float height, Entity parent = NULL_ENTITY) {
            const Entity e = entity(scene, name, parent);
            scene->getComponent<Transform>(e).position = position;
            Rigidbody2DComponent body;
            body.type = type;
            Collider2D collider;
            collider.type = Collider2DType::POLYGON;
            const float w = width * 0.5f, h = height * 0.5f;
            collider.points = {Vector2(-w, -h), Vector2(w, -h), Vector2(w, h), Vector2(-w, h)};
            body.colliders.push_back(collider);
            scene->addComponent<Rigidbody2DComponent>(e, body);
            addScript(scene, e, "Compound2DLogger");
            return e;
        }

        bool hasEntry(const std::string& entry) {
            return (" " + log() + " ").find(" " + entry + " ") != std::string::npos;
        }
    };

}

TEST_F(Physics2DCompoundScripts, ContactOnChildReachesChildAndAncestor) {
    boxBody("Floor", Vector3(0, -32, 0), BodyType::STATIC, 2000, 64);
    const Entity car = boxBody("Car", Vector3(0, 200, 0), BodyType::DYNAMIC, 64, 64);
    const Entity wheel = boxBody("Wheel", Vector3(128, -64, 0), BodyType::STATIC, 64, 64, car);
    scene->getComponent<Rigidbody2DComponent>(wheel).attachToParentBody = true;
    LuaBinding::initializeLuaScripts(scene);
    Engine::setScene(scene);
    for (int i = 0; i < 120 && log().find("CE2:Wheel") == std::string::npos; i++) {
        frame();
    }
    EXPECT_TRUE(hasEntry("CE2:Wheel:Floor:Wheel:Floor")) << log();
    EXPECT_TRUE(hasEntry("CE2:Car:Floor:Wheel:Floor")) << log();
    EXPECT_TRUE(hasEntry("CE2:Floor:Wheel:Floor:Car")) << log();
}
