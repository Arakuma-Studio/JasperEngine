// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Unity-style collision and trigger callbacks (physics-contacts spec 9, T1-T23): Lua scripts
// on in-memory scenes stepped by the engine's frame at 1/60 s. Scripts append
// "<method>:<self>:<other>:<this index>:<other index>@<step>" to the Lua `log` table.

#include "ScriptTestHarness.h"

#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "object/physics/Collision.h"
#include "object/physics/Collision2D.h"
#include "object/physics/Rigidbody.h"
#include "object/physics/Rigidbody2D.h"
#include "registry/PhysicsLayers.h"
#include "script/ScriptBase.h"
#include "script/ScriptMessages.h"
#include "subsystem/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    // ---- C++ scripts (T22), filled in like the generated factory does ----

    int cppEnters = 0;

    class CppContactScript : public ScriptBase {
    public:
        CppContactScript(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
        void onCollisionEnter(const Collision& collision) override {
            cppEnters++;
            lastOther = collision.otherCollider.entity;
        }
        Entity lastOther = NULL_ENTITY;
    };

    class CppPlainScript : public ScriptBase {
    public:
        CppPlainScript(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
    };

    class CppPrivateTrigger : public ScriptBase {
    public:
        CppPrivateTrigger(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
    private:
        void onTriggerEnter2D(const ColliderRef2D&) override {}
    };

    static_assert(ScriptMessages::cppMask<CppContactScript>() == scriptMessageBit(ScriptMessage::CollisionEnter),
                  "an onCollisionEnter override is a CollisionEnter receiver, nothing else");
    static_assert(ScriptMessages::cppMask<CppPlainScript>() == 0, "no override, no bit");
    static_assert(ScriptMessages::cppMask<CppPrivateTrigger>() & scriptMessageBit(ScriptMessage::TriggerEnter2D),
                  "a private override still counts");

    const char* LOGGER_LUA =
        "local M = {}\n"
        "local function n(go) if go then return go.name else return 'nil' end end\n"
        "local function rec(self, what, other, thisIndex, otherIndex)\n"
        "  log[#log + 1] = what .. ':' .. self.gameObject.name .. ':' .. other .. ':' .. tostring(thisIndex) .. ':' .. tostring(otherIndex) .. '@' .. tostring(step)\n"
        "end\n"
        "M.rec = rec\n"
        "function M:onCollisionEnter(c) rec(self, 'CE', n(c.gameObject), c.thisCollider.index, c.collider.index) end\n"
        "function M:onCollisionStay(c) rec(self, 'CS', n(c.gameObject), c.thisCollider.index, c.collider.index) end\n"
        "function M:onCollisionExit(c) rec(self, 'CX', n(c.gameObject), c.thisCollider.index, c.collider.index) end\n"
        "function M:onTriggerEnter(o) rec(self, 'TE', n(o.gameObject), -1, o.index) end\n"
        "function M:onTriggerStay(o) rec(self, 'TS', n(o.gameObject), -1, o.index) end\n"
        "function M:onTriggerExit(o) rec(self, 'TX', n(o.gameObject), -1, o.index) end\n"
        "function M:onCollisionEnter2D(c) rec(self, 'CE2', n(c.gameObject), c.thisCollider.index, c.collider.index) end\n"
        "function M:onCollisionStay2D(c) rec(self, 'CS2', n(c.gameObject), c.thisCollider.index, c.collider.index) end\n"
        "function M:onCollisionExit2D(c) rec(self, 'CX2', n(c.gameObject), c.thisCollider.index, c.collider.index) end\n"
        "function M:onTriggerEnter2D(o) rec(self, 'TE2', n(o.gameObject), -1, o.index) end\n"
        "function M:onTriggerStay2D(o) rec(self, 'TS2', n(o.gameObject), -1, o.index) end\n"
        "function M:onTriggerExit2D(o) rec(self, 'TX2', n(o.gameObject), -1, o.index) end\n";

    struct Event {
        std::string method, self, other;
        int thisIndex = 0, otherIndex = 0, step = 0;
    };

    class PhysicsContactsTest : public ScriptTest {
    protected:
        Scene* scene = nullptr;
        int stepNo = 0;
        size_t offThreadBefore = 0;
        std::vector<ScriptBase*> cppScripts;

        void SetUp() override {
            ScriptTest::SetUp();
            scene = newScene();
            scene->setPhysicsInterpolation(false);
            setScene("testScene", scene);
            lua("step = 0");
            writeScript("Contacts", std::string(LOGGER_LUA) + "return M\n");
            offThreadBefore = LuaBinding::getOffThreadUseCount();
            cppEnters = 0;
        }

        void TearDown() override {
            EXPECT_EQ(LuaBinding::getOffThreadUseCount(), offThreadBefore) << "Lua used off its thread (spec 7.1)";
            Engine::setTimeScale(1.0f);
            Physics::resetLayerCollisionMatrix();
            Engine::removeAllScenes();
            for (ScriptBase* script : cppScripts) {
                ScriptRuntime::forgetInstance(script);
            }
            if (scene) {
                auto scripts = scene->getComponentArray<ScriptComponent>();
                for (size_t i = 0; i < scripts->size(); i++) {
                    for (ScriptEntry& entry : scripts->getComponentFromIndex(i).scripts) {
                        if (entry.type == ScriptType::CPP) {
                            entry.cppScript = nullptr;
                            entry.instance = nullptr;
                        }
                    }
                }
            }
            for (ScriptBase* script : cppScripts) delete script;
            cppScripts.clear();
            lua("testScene = nil");
            ScriptTest::TearDown();
        }

        PhysicsSystem* physics() { return scene->getSystem<PhysicsSystem>().get(); }

        static Collider3D sphere(float radius) {
            Collider3D collider;
            collider.type = Collider3DType::SPHERE;
            collider.radius = radius;
            return collider;
        }

        static Collider3D box(float width, float height, float depth, Vector3 position = Vector3(0, 0, 0)) {
            Collider3D collider;
            collider.type = Collider3DType::BOX;
            collider.width = width;
            collider.height = height;
            collider.depth = depth;
            collider.position = position;
            return collider;
        }

        Entity body3D(const std::string& name, const Vector3& position, BodyType type, const std::vector<Collider3D>& colliders,
                      bool sensor = false, const char* script = nullptr) {
            const Entity e = entity(scene, name);
            scene->getComponent<Transform>(e).position = position;
            RigidbodyComponent body;
            body.type = type;
            body.sensor = sensor;
            for (const Collider3D& collider : colliders) {
                body.colliders[body.numColliders++] = collider;
            }
            scene->addComponent<RigidbodyComponent>(e, body);
            if (script) addScript(scene, e, script);
            return e;
        }

        Entity floor3D(const char* script = nullptr) {
            return body3D("Floor", Vector3(0, -0.5f, 0), BodyType::STATIC, {box(40, 1, 40)}, false, script);
        }

        Entity body2D(const std::string& name, const Vector3& position, BodyType type, const char* script = nullptr) {
            const Entity e = entity(scene, name);
            scene->getComponent<Transform>(e).position = position;
            Rigidbody2DComponent body;
            body.type = type;
            scene->addComponent<Rigidbody2DComponent>(e, body);
            if (script) addScript(scene, e, script);
            return e;
        }

        Collider2D& box2D(Entity e, float width, float height, Vector2 center = Vector2(0, 0)) {
            Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(e);
            Collider2D collider;
            collider.type = Collider2DType::POLYGON;
            const float hw = width * 0.5f, hh = height * 0.5f;
            collider.points = {Vector2(center.x - hw, center.y - hh), Vector2(center.x + hw, center.y - hh),
                               Vector2(center.x + hw, center.y + hh), Vector2(center.x - hw, center.y + hh)};
            body.colliders.push_back(collider);
            body.needUpdateColliders = true;
            return body.colliders.back();
        }

        Collider2D& circle2D(Entity e, float radius) {
            Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(e);
            Collider2D collider;
            collider.type = Collider2DType::CIRCLE;
            collider.radius = radius;
            body.colliders.push_back(collider);
            body.needUpdateColliders = true;
            return body.colliders.back();
        }

        template<class T>
        T* addCpp(Entity e) {
            ScriptEntry entry;
            entry.type = ScriptType::CPP;
            entry.className = "Cpp";
            entry.enabled = true;
            T* script = new T(scene, e);
            entry.instance = script;
            entry.cppScript = ScriptMessages::asScriptBase(script);
            entry.cppMessageMask = ScriptMessages::cppMask<T>();
            if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(e)) {
                scripts->scripts.push_back(entry);
            } else {
                ScriptComponent component;
                component.scripts.push_back(entry);
                scene->addComponent<ScriptComponent>(e, component);
            }
            cppScripts.push_back(script);
            return script;
        }

        // the scripts' instances and lifecycle, then the scene in the Engine (its bodies made)
        void start() {
            LuaBinding::initializeLuaScripts(scene);
            Engine::setScene(scene);
        }

        void run(int frames) {
            for (int i = 0; i < frames; i++) {
                stepNo++;
                lua("step = " + std::to_string(stepNo));
                frame();
            }
        }

        std::vector<Event> events() {
            std::vector<Event> result;
            std::istringstream in(log());
            std::string word;
            while (in >> word) {
                Event event;
                const size_t at = word.rfind('@');
                std::vector<std::string> parts;
                std::string part;
                std::istringstream fields(word.substr(0, at));
                while (std::getline(fields, part, ':')) parts.push_back(part);
                if (parts.size() != 5) continue;
                event.method = parts[0];
                event.self = parts[1];
                event.other = parts[2];
                event.thisIndex = std::atoi(parts[3].c_str());
                event.otherIndex = std::atoi(parts[4].c_str());
                event.step = std::atoi(word.substr(at + 1).c_str());
                result.push_back(event);
            }
            return result;
        }

        std::vector<Event> of(const std::string& self, const std::string& method = "") {
            std::vector<Event> result;
            for (const Event& event : events()) {
                if (event.self == self && (method.empty() || event.method == method)) result.push_back(event);
            }
            return result;
        }

        int count(const std::string& self, const std::string& method) { return (int)of(self, method).size(); }

        // "CE CX CE ..." of one object (Stay left out)
        std::string sequence(const std::string& self, bool withStay = false) {
            std::string text;
            for (const Event& event : of(self)) {
                if (!withStay && (event.method == "CS" || event.method == "TS" || event.method == "CS2" || event.method == "TS2")) continue;
                text += (text.empty() ? "" : " ") + event.method;
            }
            return text;
        }

        double number(const std::string& expression) {
            return std::atof(lua("return tostring(" + expression + ")").c_str());
        }

        std::string go(Entity e) { return "GameObject(testScene, " + std::to_string(e) + ")"; }
    };

}

// T1: a ball dropped on a static floor: one Enter, a Stay every step until it sleeps, no Exit;
// normals point away from the other collider, the relative velocity is the impact speed
TEST_F(PhysicsContactsTest, BallOnFloorEnterStayUntilSleepNoExit) {
    writeScript("Ball", std::string(LOGGER_LUA) +
        "local base = M.onCollisionEnter\n"
        "function M:onCollisionEnter(c) base(self, c); local p = c:getContact(0)\n"
        "  ballEnter = {nx = p.normal.x, ny = p.normal.y, nz = p.normal.z, rv = c.relativeVelocity.y, sep = p.separation, n = c.contactCount} end\n"
        "return M\n");
    writeScript("FloorScript", std::string(LOGGER_LUA) +
        "local base = M.onCollisionEnter\n"
        "function M:onCollisionEnter(c) base(self, c); floorNormalY = c:getContact(0).normal.y end\n"
        "return M\n");
    floor3D("FloorScript");
    body3D("Ball", Vector3(0, 2, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Ball");
    start();
    run(420);

    EXPECT_EQ(count("Ball", "CE"), 1) << sequence("Ball");
    EXPECT_EQ(count("Floor", "CE"), 1);
    EXPECT_EQ(count("Ball", "CX"), 0);
    EXPECT_EQ(count("Floor", "CX"), 0);
    const std::vector<Event> stays = of("Ball", "CS");
    ASSERT_FALSE(stays.empty());
    const int enterStep = of("Ball", "CE")[0].step;
    // one Stay per step from the step after the Enter until it slept, none after
    EXPECT_EQ((int)stays.size(), stays.back().step - enterStep);
    EXPECT_LT(stays.back().step, 420 - 30) << "the ball sleeps: no Stay";
    EXPECT_NEAR(number("ballEnter.ny"), 1.0, 0.01);
    EXPECT_NEAR(number("ballEnter.nx"), 0.0, 0.01);
    EXPECT_NEAR(number("floorNormalY"), -1.0, 0.01);
    const double impact = std::sqrt(2.0 * 9.81 * 1.5);
    EXPECT_GT(number("ballEnter.rv"), 0.0);
    EXPECT_NEAR(number("ballEnter.rv"), impact, impact * 0.1);
    EXPECT_LE(number("ballEnter.sep"), 0.02);
    EXPECT_GE(number("ballEnter.n"), 1.0);
}

// T2: a bouncing ball: Enter, Exit, Enter... as Jolt bounces it
TEST_F(PhysicsContactsTest, BouncingBallEntersAndExits) {
    floor3D();
    const Entity ball = body3D("Ball", Vector3(0, 3, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    scene->getComponent<RigidbodyComponent>(ball).restitution = 0.8f;
    start();
    run(300);
    const std::string seq = sequence("Ball");
    EXPECT_GE(count("Ball", "CE"), 3) << seq;
    // strictly alternating, starting with an Enter
    std::istringstream in(seq);
    std::string word, expected = "CE";
    while (in >> word) {
        EXPECT_EQ(word, expected) << seq;
        expected = expected == "CE" ? "CX" : "CE";
    }
}

// T3: a ball falling through a static trigger: both get Enter, Stay, Exit; no collisions
TEST_F(PhysicsContactsTest, BallThroughStaticTrigger) {
    writeScript("Ball", std::string(LOGGER_LUA) +
        "local base = M.onTriggerEnter\n"
        "function M:onTriggerEnter(o) base(self, o); otherIsTrigger = o.isTrigger end\n"
        "return M\n");
    body3D("Trigger", Vector3(0, 0, 0), BodyType::STATIC, {box(2, 2, 2)}, true, "Contacts");
    body3D("Ball", Vector3(0, 3, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Ball");
    start();
    run(150);
    EXPECT_EQ(sequence("Ball"), "TE TX");
    EXPECT_EQ(sequence("Trigger"), "TE TX");
    EXPECT_GE(count("Ball", "TS"), 1);
    EXPECT_GE(count("Trigger", "TS"), 1);
    EXPECT_EQ(lua("return tostring(otherIsTrigger)"), "true");
    // Enter, Stays, Exit in that order
    const std::vector<Event> ball = of("Ball");
    EXPECT_EQ(ball.front().method, "TE");
    EXPECT_EQ(ball.back().method, "TX");
}

// T4: a kinematic trigger moved through a static box detects it (D15)
TEST_F(PhysicsContactsTest, KinematicTriggerDetectsStaticBodies) {
    body3D("Wall", Vector3(0, 0, 0), BodyType::STATIC, {box(1, 1, 1)}, false, "Contacts");
    const Entity sensor = body3D("Sensor", Vector3(-3, 0, 0), BodyType::KINEMATIC, {sphere(0.5f)}, true, "Contacts");
    start();
    run(1);
    Rigidbody(scene, sensor).setLinearVelocity(Vector3(3, 0, 0));
    run(150);
    EXPECT_EQ(sequence("Sensor"), "TE TX");
    EXPECT_EQ(sequence("Wall"), "TE TX");
}

// T5: a ball asleep inside a trigger: no Stay, no Exit; woken and pushed out: Exit then
TEST_F(PhysicsContactsTest, SleepingInsideTriggerKeepsThePair) {
    writeScript("TriggerOnly",
        "local M = {}\n"
        "function M:onTriggerEnter(o) log[#log + 1] = 'TE:' .. self.gameObject.name .. ':' .. o.gameObject.name .. ':-1:0@' .. step end\n"
        "function M:onTriggerStay(o) log[#log + 1] = 'TS:' .. self.gameObject.name .. ':' .. o.gameObject.name .. ':-1:0@' .. step end\n"
        "function M:onTriggerExit(o) log[#log + 1] = 'TX:' .. self.gameObject.name .. ':' .. o.gameObject.name .. ':-1:0@' .. step end\n"
        "return M\n");
    floor3D();
    body3D("Zone", Vector3(0, 1, 0), BodyType::STATIC, {box(6, 4, 6)}, true, "TriggerOnly");
    const Entity ball = body3D("Ball", Vector3(0, 0.6f, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "TriggerOnly");
    start();
    run(300);
    EXPECT_EQ(sequence("Ball"), "TE");
    const std::vector<Event> stays = of("Ball", "TS");
    ASSERT_FALSE(stays.empty());
    EXPECT_LT(stays.back().step, 300 - 30) << "asleep: no Stay";
    const ContactTracker& tracker = physics()->getContactTracker3D();
    ASSERT_EQ(tracker.getPairs().size(), 1u);
    EXPECT_TRUE(tracker.getPairs().begin()->second.asleep);

    Rigidbody(scene, ball).activate();
    Rigidbody(scene, ball).setLinearVelocity(Vector3(25, 0, 0));
    run(60);
    EXPECT_EQ(sequence("Ball"), "TE TX");
    EXPECT_EQ(sequence("Zone"), "TE TX");
}

// T6: two dynamic trigger spheres overlapping: sensor vs sensor works in 3D
TEST_F(PhysicsContactsTest, DynamicSensorsDetectEachOther) {
    const Entity a = body3D("A", Vector3(0, 0, 0), BodyType::DYNAMIC, {sphere(0.5f)}, true, "Contacts");
    const Entity b = body3D("B", Vector3(0.5f, 0, 0), BodyType::DYNAMIC, {sphere(0.5f)}, true, "Contacts");
    scene->getComponent<RigidbodyComponent>(a).gravityFactor = 0.0f;
    scene->getComponent<RigidbodyComponent>(b).gravityFactor = 0.0f;
    start();
    run(5);
    EXPECT_EQ(sequence("A"), "TE");
    EXPECT_EQ(sequence("B"), "TE");
}

// T7: a ball in a trigger destroyed (deferred, then immediate from the trigger's own Enter):
// the trigger gets one Exit with the ball still named, the ball no Exit
TEST_F(PhysicsContactsTest, DestroyedInsideTriggerGivesOneExit) {
    for (int immediate = 0; immediate < 2; immediate++) {
        clearLog();
        writeScript("Destroyer", std::string(LOGGER_LUA) +
            "local base = M.onTriggerEnter\n"
            "function M:onTriggerEnter(o) base(self, o)\n"
            "  if " + std::string(immediate ? "true" : "false") + " then GameObject.destroyImmediate(o.gameObject) else GameObject.destroy(o.gameObject) end\n"
            "end\n"
            "return M\n");
        LuaBinding::clearLoadedProjectModules();
        if (immediate) {
            Engine::removeAllScenes();
            scene = newScene();
            scene->setPhysicsInterpolation(false);
            setScene("testScene", scene);
        }
        body3D("Trigger", Vector3(0, 0, 0), BodyType::STATIC, {box(4, 4, 4)}, true, "Destroyer");
        const Entity ball = body3D("Ball", Vector3(0, 0, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
        scene->getComponent<RigidbodyComponent>(ball).gravityFactor = 0.0f;
        start();
        run(5);
        EXPECT_FALSE(scene->isEntityCreated(ball)) << (immediate ? "destroyImmediate" : "destroy");
        EXPECT_EQ(sequence("Trigger"), "TE TX") << (immediate ? "destroyImmediate" : "destroy");
        const std::vector<Event> exits = of("Trigger", "TX");
        ASSERT_EQ(exits.size(), 1u);
        EXPECT_EQ(exits[0].other, "Ball") << "the departing GameObject still exists during the call";
        EXPECT_EQ(count("Ball", "TX"), 0);
        if (immediate) EXPECT_EQ(count("Ball", "TE"), 0) << "destroyed before its own Enter";
    }
}

// T8: a ball deactivated on the floor: the floor's Exit at the next step; active again: Enter
TEST_F(PhysicsContactsTest, DeactivateAndActivateEndAndStartThePair) {
    floor3D("Contacts");
    const Entity ball = body3D("Ball", Vector3(0, 0.55f, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    start();
    run(60);
    EXPECT_EQ(sequence("Floor"), "CE");
    scene->setEntityActive(ball, false);
    const int before = stepNo;
    run(1);
    EXPECT_EQ(sequence("Floor"), "CE CX");
    EXPECT_EQ(of("Floor", "CX")[0].step, before + 1);
    EXPECT_EQ(count("Ball", "CX"), 0) << "an inactive GameObject gets nothing";
    scene->setEntityActive(ball, true);
    run(10);
    EXPECT_EQ(sequence("Floor"), "CE CX CE");
    EXPECT_EQ(sequence("Ball"), "CE CE");
}

// T9: a collider resize and a scale change rebuild the body: no Exit / Enter
TEST_F(PhysicsContactsTest, RebuildsAreInvisible) {
    floor3D("Contacts");
    const Entity ball = body3D("Ball", Vector3(0, 0.55f, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    start();
    run(60);
    RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(ball);
    body.colliders[0].radius = 0.55f;
    body.needUpdateColliders = true;
    run(30);
    scene->getComponent<Transform>(ball).scale = Vector3(1.1f, 1.1f, 1.1f);
    scene->getComponent<Transform>(ball).needUpdate = true;
    run(30);
    EXPECT_EQ(sequence("Ball"), "CE");
    EXPECT_EQ(sequence("Floor"), "CE");
}

// T10: the layer matrix and Physics.ignoreCollision keep a pair from starting
TEST_F(PhysicsContactsTest, LayersAndIgnoreCollisionFilterPairs) {
    const Entity floorEntity = floor3D("Contacts");
    const Entity layered = body3D("Layered", Vector3(-3, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    scene->setEntityLayer(layered, 1);
    Physics::ignoreLayerCollision(0, 1, true);
    const Entity ball = body3D("Ball", Vector3(3, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    start();
    lua("Physics.ignoreCollision(" + go(ball) + ", " + go(floorEntity) + ")");
    EXPECT_EQ(lua("return tostring(Physics.getIgnoreCollision(" + go(floorEntity) + ", " + go(ball) + "))"), "true");
    run(60);
    EXPECT_EQ(sequence("Layered"), "");
    EXPECT_EQ(sequence("Ball"), "");
    EXPECT_EQ(sequence("Floor"), "");
    EXPECT_LT(scene->getComponent<Transform>(ball).worldPosition.y, -1.0f) << "fell through the floor";

    // collide again
    lua("Physics.ignoreCollision(" + go(ball) + ", " + go(floorEntity) + ", false)");
    Rigidbody(scene, ball).setPosition(Vector3(3, 1, 0));
    Rigidbody(scene, ball).setLinearVelocity(Vector3(0, 0, 0));
    run(60);
    EXPECT_EQ(sequence("Ball"), "CE");
    EXPECT_EQ(lua("return tostring(Physics.getIgnoreCollision(" + go(floorEntity) + ", " + go(ball) + "))"), "false");
    // a body of the wrong dimension is a Lua error
    EXPECT_NE(luaError("Physics2D.ignoreCollision(" + go(ball) + ", " + go(floorEntity) + ")").find("no Rigidbody 2D"), std::string::npos);
}

// T11: bodies without listeners or global subscribers record nothing
TEST_F(PhysicsContactsTest, NoListenersRecordNothing) {
    floor3D();
    for (int i = 0; i < 200; i++) {
        body3D("Box" + std::to_string(i), Vector3((float)(i % 20) - 10.0f, 0.6f + (float)(i / 20) * 1.1f, 0), BodyType::DYNAMIC, {box(1, 1, 1)});
    }
    start();
    for (int i = 0; i < 60; i++) {
        run(1);
        ASSERT_EQ(physics()->getContactTracker3D().recordedCount(), 0u) << "frame " << i;
    }
}

namespace {
    // T12 / T14: 300 bodies, 10 of them listening, a Lua global handler and a C++ modifier
    struct ManyBodies {
        std::mutex mutex;
        std::map<std::string, std::vector<float>> modify;
    };
}

static void buildManyBodies(PhysicsContactsTest* test, Scene* scene, std::function<Entity(const std::string&, const Vector3&, bool)> make) {
    (void)test;
    (void)scene;
    for (int i = 0; i < 300; i++) {
        const Vector3 position((float)(i % 15) * 1.2f - 9.0f, 0.6f + (float)(i / 15) * 1.2f, (float)((i * 7) % 5) * 0.3f);
        make("S" + std::to_string(i), position, i % 30 == 0);
    }
}

// T12: threaded job system, Lua handler and callbacks on the main thread only; the handler's
// snapshot equals what a C++ onContactModify3D subscriber saw live
TEST_F(PhysicsContactsTest, ThreadedStepRunsLuaOnTheMainThreadOnly) {
    floor3D();
    buildManyBodies(this, scene, [&](const std::string& name, const Vector3& position, bool listens) {
        return body3D(name, position, BodyType::DYNAMIC, {sphere(0.5f)}, false, listens ? "Contacts" : nullptr);
    });
    ManyBodies seen;
    physics()->onContactModify3D.add("test", std::function<void(Rigidbody, Rigidbody, Contact3D&)>([&seen](Rigidbody a, Rigidbody b, Contact3D& c) {
        const std::string key = std::to_string(a.getEntity()) + "-" + std::to_string(b.getEntity());
        std::lock_guard<std::mutex> lock(seen.mutex);
        if (!seen.modify.count(key)) {
            const Vector3 n = c.getWorldSpaceNormal();
            seen.modify[key] = {n.x, n.y, n.z, c.getPenetrationDepth()};
        }
    }));
    lua("added3D = {}\n"
        "testScene:getPhysicsSystem().onContactAdded3D = function(a, b, c)\n"
        "  local key = a.entity .. '-' .. b.entity\n"
        "  if not added3D[key] then local n = c:getWorldSpaceNormal(); added3D[key] = {n.x, n.y, n.z, c:getPenetrationDepth()} end\n"
        "end\n");
    start();
    EXPECT_FALSE(physics()->isStepSynchronous3D());
    run(90);
    EXPECT_FALSE(physics()->isStepSynchronous3D()) << "a Lua onContactAdded3D does not need the main thread step";
    EXPECT_GT(events().size(), 10u);
    int compared = 0;
    for (const auto& [key, values] : seen.modify) {
        const std::string entry = "added3D['" + key + "']";
        if (lua("return tostring(" + entry + " ~= nil)") != "true") continue;
        for (int i = 0; i < 4; i++) {
            EXPECT_NEAR(number(entry + "[" + std::to_string(i + 1) + "]"), values[(size_t)i], 1e-5) << key << " value " << i;
        }
        compared++;
    }
    EXPECT_GT(compared, 50);
    EXPECT_EQ(lua("local n = 0 for _ in pairs(added3D) do n = n + 1 end return n"), std::to_string(seen.modify.size()))
        << "every pair the job threads saw reached Lua";
    physics()->onContactModify3D.remove("test");
    lua("added3D = nil");
}

// T14: the same scene run twice gives the same callback log
TEST_F(PhysicsContactsTest, CallbackOrderIsDeterministic) {
    std::string logs[2];
    for (int run_ = 0; run_ < 2; run_++) {
        if (run_ == 1) {
            Engine::removeAllScenes();
            scene = newScene();
            scene->setPhysicsInterpolation(false);
            setScene("testScene", scene);
            stepNo = 0;
        }
        clearLog();
        floor3D("Contacts");
        buildManyBodies(this, scene, [&](const std::string& name, const Vector3& position, bool listens) {
            return body3D(name, position, BodyType::DYNAMIC, {sphere(0.5f)}, false, listens ? "Contacts" : nullptr);
        });
        start();
        run(90);
        logs[run_] = log();
    }
    EXPECT_FALSE(logs[0].empty());
    EXPECT_EQ(logs[0], logs[1]);
}

// T13: a Lua shouldCollide3D runs in a main-thread step (synchronous mode, logged once)
TEST_F(PhysicsContactsTest, LuaShouldCollideMakesTheStepSynchronous) {
    floor3D();
    body3D("Ghost", Vector3(-2, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    body3D("Ball", Vector3(2, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    lua("testScene:getPhysicsSystem().shouldCollide3D = function(a, b) return a.name ~= 'Ghost' and b.name ~= 'Ghost' end");
    start();
    run(90);
    EXPECT_TRUE(physics()->isStepSynchronous3D());
    EXPECT_EQ(sequence("Ghost"), "") << "rejected: fell through";
    EXPECT_EQ(sequence("Ball"), "CE");
    int infos = 0;
    for (const std::string& line : system.logLines) {
        if (line.find("steps on the main thread") != std::string::npos) infos++;
    }
    EXPECT_EQ(infos, 1);
}

// T13: a Contact3D setter in an after-the-step handler warns once and does nothing
TEST_F(PhysicsContactsTest, SnapshotSettersWarnOnce) {
    floor3D();
    body3D("Ball", Vector3(0, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false);
    lua("snapshotCalls = 0\n"
        "testScene:getPhysicsSystem().onContactAdded3D = function(a, b, c)\n"
        "  snapshotCalls = snapshotCalls + 1; snapshotWas = c.snapshot; c:setCombinedFriction(0); c:setCombinedFriction(0)\n"
        "end\n");
    start();
    run(60);
    EXPECT_GE(number("snapshotCalls"), 1.0);
    EXPECT_EQ(lua("return tostring(snapshotWas)"), "true");
    int warnings = 0;
    for (const std::string& line : system.logLines) {
        if (line.find("Contact3D setters have no effect") != std::string::npos) warnings++;
    }
    EXPECT_EQ(warnings, 1);
    // synchronous mode: live and settable, no warning
    physics()->setSynchronousContactEvents3D(true);
    lua("snapshotWas = nil");
    Rigidbody(scene, scene->getComponentArray<RigidbodyComponent>()->getEntity(1)).setPosition(Vector3(0, 2, 0));
    run(60);
    EXPECT_EQ(lua("return tostring(snapshotWas)"), "false");
}

// T15: a 2D box on a static box: Enter once, Stay while awake, none asleep; Unity's naming
TEST_F(PhysicsContactsTest, Box2DOnFloorEnterStayUntilAsleep) {
    writeScript("Box2DScript", std::string(LOGGER_LUA) +
        "local enter = M.onCollisionEnter2D\n"
        "function M:onCollisionEnter2D(c) enter(self, c); boxNormalY = c:getContact(0).normal.y end\n"
        "local stay = M.onCollisionStay2D\n"
        "function M:onCollisionStay2D(c) stay(self, c)\n"
        "  sameThis = (c.otherCollider.index == c.thisCollider.index) and c.otherRigidbody.entity == self.entity\n"
        "  otherIsFloor = c.collider.gameObject.name == 'Floor' and c.rigidbody.entity ~= self.entity\n"
        "  stayImpulse = c:getContact(0).normalImpulse\n"
        "end\n"
        "return M\n");
    const Entity ground = body2D("Floor", Vector3(0, 0, 0), BodyType::STATIC);
    box2D(ground, 1000, 64);
    const Entity crate = body2D("Box", Vector3(0, 200, 0), BodyType::DYNAMIC, "Box2DScript");
    box2D(crate, 64, 64);
    start();
    run(420);
    EXPECT_EQ(sequence("Box"), "CE2");
    const std::vector<Event> stays = of("Box", "CS2");
    ASSERT_FALSE(stays.empty());
    EXPECT_EQ((int)stays.size(), stays.back().step - of("Box", "CE2")[0].step);
    EXPECT_LT(stays.back().step, 420 - 30) << "asleep: no Stay";
    EXPECT_EQ(lua("return tostring(sameThis)"), "true");
    EXPECT_EQ(lua("return tostring(otherIsFloor)"), "true");
    EXPECT_GT(number("stayImpulse"), 0.0);
    EXPECT_GT(number("boxNormalY"), 0.9);
}

// T16: a coin (static sensor) destroyed in its own onTriggerEnter2D: the car gets one Exit
TEST_F(PhysicsContactsTest, CoinDestroyedInTriggerEnter2D) {
    writeScript("Coin", std::string(LOGGER_LUA) +
        "local base = M.onTriggerEnter2D\n"
        "function M:onTriggerEnter2D(o) base(self, o); GameObject.destroy(self.gameObject) end\n"
        "return M\n");
    const Entity coin = body2D("Coin", Vector3(0, 100, 0), BodyType::STATIC, "Coin");
    circle2D(coin, 20).sensor = true;
    const Entity car = body2D("Car", Vector3(0, 300, 0), BodyType::DYNAMIC, "Contacts");
    box2D(car, 64, 32);
    start();
    run(120);
    EXPECT_FALSE(scene->isEntityCreated(coin));
    EXPECT_EQ(sequence("Car"), "TE2 TX2");
    EXPECT_EQ(of("Car", "TX2")[0].other, "Coin");
}

// T17: listeners work without contactEvents; the global events keep to the authored flag
TEST_F(PhysicsContactsTest, GlobalEventsKeepTheAuthoredFlags) {
    for (int authored = 0; authored < 2; authored++) {
        if (authored) {
            Engine::removeAllScenes();
            scene = newScene();
            scene->setPhysicsInterpolation(false);
            setScene("testScene", scene);
            stepNo = 0;
        }
        clearLog();
        const Entity ground = body2D("Floor", Vector3(0, 0, 0), BodyType::STATIC);
        box2D(ground, 1000, 64).contactEvents = authored != 0;
        const Entity crate = body2D("Box", Vector3(0, 120, 0), BodyType::DYNAMIC, "Contacts");
        box2D(crate, 64, 64).contactEvents = authored != 0;
        scene->getComponent<Rigidbody2DComponent>(crate).sleepingMode = RigidbodySleepMode2D::NEVER_SLEEP;
        lua("globalBegins = 0; globalStays = 0\n"
            "local ps = testScene:getPhysicsSystem()\n"
            "ps.beginContact2D = function() globalBegins = globalBegins + 1 end\n"
            "ps.stayContact2D = function() globalStays = globalStays + 1 end\n");
        start();
        run(120);
        EXPECT_EQ(sequence("Box"), "CE2") << "authored " << authored;
        EXPECT_EQ(number("globalBegins"), authored ? 1.0 : 0.0);
        if (authored) {
            EXPECT_EQ((int)number("globalStays"), count("Box", "CS2")) << "once per step, as the per-object Stay";
        } else {
            EXPECT_EQ(number("globalStays"), 0.0);
        }
    }
}

// T18: a compound body resting on two colliders: one Enter per collider
TEST_F(PhysicsContactsTest, CompoundBodyEntersPerCollider) {
    floor3D();
    body3D("Bench", Vector3(0, 0.6f, 0), BodyType::DYNAMIC, {box(1, 1, 1, Vector3(-0.8f, 0, 0)), box(1, 1, 1, Vector3(0.8f, 0, 0))}, false, "Contacts");
    start();
    run(60);
    const std::vector<Event> enters = of("Bench", "CE");
    ASSERT_EQ(enters.size(), 2u) << sequence("Bench");
    EXPECT_EQ(enters[0].thisIndex + enters[1].thisIndex, 1) << "colliders 0 and 1";
    EXPECT_NE(enters[0].thisIndex, enters[1].thisIndex);
}

// T19: a collider's receiver gets that collider's callbacks; the body's scripts get all
TEST_F(PhysicsContactsTest, ColliderReceiverGetsItsCollidersCallbacks) {
    const Entity ground = body2D("Floor", Vector3(0, 0, 0), BodyType::STATIC);
    box2D(ground, 1000, 64);
    const Entity detector = entity(scene, "Detector");
    addScript(scene, detector, "Contacts");
    const Entity car = body2D("Car", Vector3(0, 150, 0), BodyType::DYNAMIC, "Contacts");
    box2D(car, 60, 30, Vector2(-50, 0));
    box2D(car, 60, 30, Vector2(50, 0)).receiver = detector;
    start();
    run(90);
    const std::vector<Event> detected = of("Detector", "CE2");
    ASSERT_EQ(detected.size(), 1u) << sequence("Detector");
    EXPECT_EQ(detected[0].thisIndex, 1);
    EXPECT_EQ(detected[0].other, "Floor");
    EXPECT_EQ(count("Car", "CE2"), 2);
}

// T20: every script defining the method gets it, enabled or not
TEST_F(PhysicsContactsTest, DisabledScriptsReceiveToo) {
    writeScript("Second", std::string(LOGGER_LUA) +
        "function M:init() self.enabled = false end\n"
        "local base = M.onCollisionEnter\n"
        "function M:onCollisionEnter(c) secondGot = true; secondEnabled = self.enabled end\n"
        "return M\n");
    writeScript("Silent", "local M = {}\nreturn M\n");
    floor3D();
    const Entity ball = body3D("Ball", Vector3(0, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    addScript(scene, ball, "Second");
    addScript(scene, ball, "Silent");
    start();
    run(60);
    EXPECT_EQ(sequence("Ball"), "CE");
    EXPECT_EQ(lua("return tostring(secondGot)"), "true");
    EXPECT_EQ(lua("return tostring(secondEnabled)"), "false");
}

// T21: a callback that teleports the other body, adds a collider, destroys and loads: no crash
TEST_F(PhysicsContactsTest, CallbacksMayChangeTheWorld) {
    writeScript("Chaos", std::string(LOGGER_LUA) +
        "local base = M.onCollisionEnter\n"
        "function M:onCollisionEnter(c) base(self, c)\n"
        "  local rb = c.rigidbody; if rb then rb.position = Vector3(0, 5, 0) end\n"
        "  local mine = Rigidbody(self.scene, self.entity); mine:createSphereCollider(0.25)\n"
        "  local victim = GameObject.find('Victim'); if victim then GameObject.destroyImmediate(victim) end\n"
        "  pcall(function() SceneManager.loadScene('NoSuchScene') end)\n"
        "end\n"
        "return M\n");
    floor3D("Chaos");
    body3D("Ball", Vector3(0, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    body3D("Victim", Vector3(3, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    start();
    run(120);
    EXPECT_GE(count("Floor", "CE"), 1);
    EXPECT_EQ(count("Victim", "CX"), 0) << "destroyed: it gets nothing more";
    const auto bodies = scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies->size(); i++) {
        EXPECT_NE(scene->getEntityName(bodies->getEntity(i)), "Victim");
    }
}

// T22: a C++ ScriptBase override gets the bit and the call; a plain one neither
TEST_F(PhysicsContactsTest, CppOverridesReceiveContacts) {
    floor3D();
    const Entity ball = body3D("Ball", Vector3(0, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)});
    const Entity other = body3D("Other", Vector3(3, 1, 0), BodyType::DYNAMIC, {sphere(0.5f)});
    CppContactScript* script = addCpp<CppContactScript>(ball);
    addCpp<CppPlainScript>(other);
    start();
    EXPECT_TRUE(ScriptMessages::receivers(scene, ball) & scriptMessageBit(ScriptMessage::CollisionEnter));
    EXPECT_FALSE(ScriptMessages::receivers(scene, other) & PhysicsCallbacks::ALL_3D);
    run(60);
    EXPECT_EQ(cppEnters, 1);
    EXPECT_EQ(scene->getEntityName(script->lastOther), "Floor");
}

// T23: time scale 0: no step, no callbacks; 4 fixed steps in a frame: 4 Stay rounds
TEST_F(PhysicsContactsTest, CallbacksFollowTheFixedSteps) {
    floor3D();
    const Entity ball = body3D("Ball", Vector3(0, 0.55f, 0), BodyType::DYNAMIC, {sphere(0.5f)}, false, "Contacts");
    start();
    Rigidbody(scene, ball).setAllowSleeping(false);
    run(30);
    clearLog();
    Engine::setTimeScale(0.0f);
    run(10);
    EXPECT_EQ(log(), "");
    Engine::setTimeScale(1.0f);
    Engine::setFixedDeltatime(4.0 / 60.0);
    run(5);
    Engine::setFixedDeltatime(1.0 / 60.0);
    EXPECT_NEAR(count("Ball", "CS"), 20, 1) << "4 rounds a frame";
}

// spec 8: 500 boxes on a floor, 10 listening: the contact work after the step stays under 5%
// of the step; with no listener it is nothing
TEST_F(PhysicsContactsTest, ContactProcessingCostIsSmall) {
    for (int listeners = 10; listeners >= 0; listeners -= 10) {
        if (listeners == 0) {
            Engine::removeAllScenes();
            scene = newScene();
            scene->setPhysicsInterpolation(false);
            setScene("testScene", scene);
        }
        clearLog();
        floor3D();
        for (int i = 0; i < 500; i++) {
            const Vector3 position((float)(i % 25) * 1.05f - 13.0f, 0.5f, (float)(i / 25) * 1.05f - 10.0f);
            body3D("B" + std::to_string(i), position, BodyType::DYNAMIC, {box(1, 1, 1)}, false, (i % 50 == 0 && listeners) ? "Contacts" : nullptr);
        }
        start();
        run(5);
        physics()->resetTimings3D();
        run(60);
        const double step = physics()->getStepSeconds3D();
        const double contacts = physics()->getContactSeconds3D();
        ASSERT_GT(step, 0.0);
        if (listeners) {
            EXPECT_LT(contacts, step * 0.05) << "step " << step << " s, contacts " << contacts << " s";
            EXPECT_GT(count("B0", "CS"), 0);
        } else {
            EXPECT_LT(contacts, step * 0.01) << "step " << step << " s, contacts " << contacts << " s";
            EXPECT_EQ(physics()->getContactTracker3D().recordedCount(), 0u);
        }
    }
}
