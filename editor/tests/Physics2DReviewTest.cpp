// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Regression tests of the physics-2d review: old prefab overrides of colliders, a joint broken
// with DISABLE and enabled again, live read-outs after a type change, Unity 2D's "Mean", C++
// message overrides, the editor's polygon pieces (the body's own), runtime material changes
// without contact events, Lua argument errors, angle defaults in the project's unit, polygon
// edits in place, colliders removed during a Box2D step and joints whose body goes away.

#include <gtest/gtest.h>

#include "Physics2DTestWorld.h"

#include "Catalog.h"
#include "component/ScriptComponent.h"
#include "physics/PhysicsMaterial.h"
#include "script/LuaBinding.h"
#include "script/ScriptBase.h"
#include "script/ScriptMessages.h"
#include "util/Prefab.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::physics2dtest;

namespace {

    std::vector<Vector2> boxPoints(float w, float h, Vector2 c = Vector2(0, 0)) {
        return {c + Vector2(-w / 2, -h / 2), c + Vector2(w / 2, -h / 2), c + Vector2(w / 2, h / 2), c + Vector2(-w / 2, h / 2)};
    }

    Rigidbody2DComponent bodyWith(size_t colliders) {
        Rigidbody2DComponent body;
        body.type = BodyType::DYNAMIC;
        for (size_t i = 0; i < colliders; i++) {
            Collider2D collider;
            collider.type = Collider2DType::POLYGON;
            collider.points = boxPoints(20, 20, Vector2(30.0f * i, 0));
            collider.friction = 0.1f * (float)(i + 1);
            body.colliders.push_back(collider);
        }
        return body;
    }

    // what ProjectPrefabImport's copyProperties does with an instance's override names: each
    // name found on both sides is copied from the instance's values onto the prefab's
    void copyOverrides(Scene& from, Entity source, Scene& to, Entity target, const PrefabObjectOverrides& overrides) {
        const auto it = overrides.properties.find(ComponentType::Rigidbody2DComponent);
        ASSERT_NE(it, overrides.properties.end());
        for (const std::string& name : it->second) {
            ASSERT_TRUE(Catalog::findProperty(&from, source, ComponentType::Rigidbody2DComponent, name).ref) << name;
            ASSERT_TRUE(Catalog::findProperty(&to, target, ComponentType::Rigidbody2DComponent, name).ref)
                << name << ": no longer matches a property (would be dropped)";
            Catalog::copyPropertyValue(&from, source, &to, target, ComponentType::Rigidbody2DComponent, name);
        }
    }

}

// ---- 1. prefab overrides recorded by editors before the caps went ----------------------------

TEST(Physics2DReview, OldColliderOverrideNamesAreMigratedAndCopied) {
    PrefabObjectOverrides migrated;
    migrated.addProperty(ComponentType::Rigidbody2DComponent, "numColliders");
    migrated.addProperty(ComponentType::Rigidbody2DComponent, "colliders[1].numVertices");
    migrated.addProperty(ComponentType::Rigidbody2DComponent, "colliders[0].vertices[2]");
    migrated.addProperty(ComponentType::Rigidbody2DComponent, "colliders[0].friction");
    migrated.addProperty(ComponentType::SpriteComponent, "numColliders");   // other components: as recorded
    const std::set<std::string> names = migrated.properties[ComponentType::Rigidbody2DComponent];
    EXPECT_EQ(names, (std::set<std::string>{"colliders", "colliders[1].points", "colliders[0].points[2]", "colliders[0].friction"}));
    EXPECT_TRUE(migrated.properties[ComponentType::SpriteComponent].count("numColliders"));

    Scene prefab;
    Scene instance;
    const Entity prefabRoot = prefab.createEntity();
    const Entity instanceRoot = instance.createEntity();

    // an instance that added a collider (main recorded numColliders and the new collider's fields)
    {
        prefab.addComponent<Rigidbody2DComponent>(prefabRoot, bodyWith(2));
        instance.addComponent<Rigidbody2DComponent>(instanceRoot, bodyWith(3));
        PrefabObjectOverrides overrides;
        overrides.addProperty(ComponentType::Rigidbody2DComponent, "numColliders");
        overrides.addProperty(ComponentType::Rigidbody2DComponent, "colliders[2].friction");
        copyOverrides(instance, instanceRoot, prefab, prefabRoot, overrides);
        EXPECT_EQ(prefab.getComponent<Rigidbody2DComponent>(prefabRoot).colliders.size(), 3u) << "the added collider is kept";
        EXPECT_FLOAT_EQ(prefab.getComponent<Rigidbody2DComponent>(prefabRoot).colliders[2].friction, 0.3f);
    }
    // an instance that removed one
    {
        prefab.getComponent<Rigidbody2DComponent>(prefabRoot) = bodyWith(2);
        instance.getComponent<Rigidbody2DComponent>(instanceRoot) = bodyWith(1);
        PrefabObjectOverrides overrides;
        overrides.addProperty(ComponentType::Rigidbody2DComponent, "numColliders");
        copyOverrides(instance, instanceRoot, prefab, prefabRoot, overrides);
        EXPECT_EQ(prefab.getComponent<Rigidbody2DComponent>(prefabRoot).colliders.size(), 1u) << "the removed collider stays removed";
    }
    // point edits (old vertices[j], current points[j]) are copied, also by Apply to Prefab
    {
        prefab.getComponent<Rigidbody2DComponent>(prefabRoot) = bodyWith(2);
        Rigidbody2DComponent edited = bodyWith(2);
        edited.colliders[0].points[2] = Vector2(55, 66);
        edited.colliders[1].points[0] = Vector2(-7, -8);
        edited.colliders[1].points.push_back(Vector2(40, 30));
        instance.getComponent<Rigidbody2DComponent>(instanceRoot) = edited;
        PrefabObjectOverrides overrides;
        overrides.addProperty(ComponentType::Rigidbody2DComponent, "colliders[0].vertices[2]");
        overrides.addProperty(ComponentType::Rigidbody2DComponent, "colliders[1].numVertices");
        copyOverrides(instance, instanceRoot, prefab, prefabRoot, overrides);
        const Rigidbody2DComponent& applied = prefab.getComponent<Rigidbody2DComponent>(prefabRoot);
        EXPECT_EQ(applied.colliders[0].points[2], Vector2(55, 66));
        EXPECT_EQ(applied.colliders[1].points.size(), 5u);
        EXPECT_EQ(applied.colliders[1].points[0], Vector2(-7, -8));

        // a single point by its current name
        instance.getComponent<Rigidbody2DComponent>(instanceRoot).colliders[0].points[1] = Vector2(1.5f, 2.5f);
        prefab.getComponent<Rigidbody2DComponent>(prefabRoot).needUpdateColliders = false;
        Catalog::copyPropertyValue(&instance, instanceRoot, &prefab, prefabRoot, ComponentType::Rigidbody2DComponent, "colliders[0].points[1]");
        EXPECT_EQ(prefab.getComponent<Rigidbody2DComponent>(prefabRoot).colliders[0].points[1], Vector2(1.5f, 2.5f));
        // a name only the lookup resolves (not enumerated) still applies its update flags: the
        // shapes are rebuilt (they were read from the end() iterator of the enumerated names)
        EXPECT_TRUE(prefab.getComponent<Rigidbody2DComponent>(prefabRoot).needUpdateColliders);
    }
}

// ---- 3. DISABLE, then enabled again before the next step ------------------------------------

namespace {

    struct Hanging {
        World2D world;
        Entity anchor, weight, joint;
        explicit Hanging(JointBreakAction action) {
            anchor = world.body(Vector3(0, 100, 0), BodyType::STATIC);
            weight = world.body(Vector3(0, 60, 0), BodyType::DYNAMIC);
            world.rb(weight).createCircleCollider(Vector2(0, 0), 8);
            world.component(weight).useAutoMass = false;
            world.component(weight).mass = 10.0f;
            Joint2DComponent revolute = makeJoint2D(Joint2DType::REVOLUTE);
            revolute.bodyA = weight;
            revolute.bodyB = anchor;
            revolute.anchorA = Vector2(0, 40);
            revolute.breakForce = 50.0f;
            revolute.breakAction = action;
            joint = world.joint(revolute);
        }
    };

    struct Counter : public ScriptBase {
        int* calls;
        Counter(Scene* scene, Entity entity, int* calls) : ScriptBase(scene, entity), calls(calls) {}
    private:
        // a private override (the generated factory must not name it)
        void onJointBreak2D(Joint2D&) override { (*calls)++; }
    };

    // a C++ script that does not override any message
    struct Silent : public ScriptBase {
        using ScriptBase::ScriptBase;
    };

    template<typename T>
    void addScript(World2D& world, Entity entity, T* script) {
        ScriptComponent scripts;
        if (ScriptComponent* existing = world.scene.findComponent<ScriptComponent>(entity)) scripts = *existing;
        ScriptEntry entry;
        entry.type = ScriptType::CPP;
        entry.enabled = true;
        entry.className = "Script";
        entry.instance = script;
        entry.cppScript = ScriptMessages::cppScript<T>(script);
        entry.cppMessageMask = ScriptMessages::cppMask<T>();
        scripts.scripts.push_back(entry);
        if (world.scene.findComponent<ScriptComponent>(entity)) world.scene.getComponent<ScriptComponent>(entity) = scripts;
        else world.scene.addComponent<ScriptComponent>(entity, scripts);
    }

}

TEST(Physics2DReview, DisabledJointEnabledAgainBreaksAgain) {
    for (int way = 0; way < 2; way++) {
        Hanging h(JointBreakAction::DISABLE);
        int calls = 0;
        Counter counter(&h.world.scene, h.joint, &calls);
        addScript(h.world, h.joint, &counter);
        h.world.load();
        h.world.step();
        ASSERT_EQ(calls, 1);
        ASSERT_TRUE(h.world.jointComponent(h.joint).broken);
        // enabled again before the next step: by Lua / C++ (Joint2D) or by the Inspector (the field)
        if (way == 0) h.world.j(h.joint).setEnabled(true);
        else h.world.jointComponent(h.joint).enabled = true;
        h.world.step(3);
        EXPECT_EQ(calls, 2) << (way == 0 ? "Joint2D::setEnabled" : "the enabled field") << ": the load is still over 50 N, it breaks again";
        EXPECT_FALSE(h.world.jointComponent(h.joint).enabled);
        h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    }
}

// ---- 4. live read-outs wait for the rebuild of a type change --------------------------------

TEST(Physics2DReview, ReadOutsAfterATypeChangeWaitForTheRebuild) {
    World2D world;
    const Entity pivot = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    const Entity wheel = world.body(Vector3(0, -20, 0), BodyType::DYNAMIC);
    world.rb(wheel).createCircleCollider(Vector2(0, 0), 10);
    Joint2DComponent joint = makeJoint2D(Joint2DType::REVOLUTE);
    joint.bodyA = pivot;
    joint.bodyB = wheel;
    const Entity e = world.joint(joint);
    world.load();
    world.step();
    ASSERT_TRUE(world.j(e).isLive());
    // the Inspector in Play changes the type: the Box2D joint is still the revolute one
    world.jointComponent(e).type = Joint2DType::WHEEL;
    world.jointComponent(e).needUpdateJoint = true;
    EXPECT_FALSE(world.j(e).isLive());
    EXPECT_EQ(world.j(e).getJointTranslation(), 0.0f) << "no wheel getter on a revolute joint";
    EXPECT_EQ(world.j(e).getMotorTorque(), 0.0f);
    // a type that does not match the Box2D joint, even without the flag
    world.jointComponent(e).needUpdateJoint = false;
    EXPECT_FALSE(world.j(e).isLive());
    world.jointComponent(e).needUpdateJoint = true;
    world.step();
    EXPECT_TRUE(world.j(e).isLive());
    EXPECT_EQ(b2Joint_GetType(world.jointComponent(e).joint), b2_wheelJoint);
}

// ---- 6. Unity 2D's "Mean" is the geometric mean ----------------------------------------------

TEST(Physics2DReview, UnityMeanIsTheGeometricMean) {
    PhysicsMaterialCombine mode = PhysicsMaterialCombine::NONE;
    ASSERT_TRUE(physicsmaterial::combineFromName("Mean", mode));
    EXPECT_EQ(mode, PhysicsMaterialCombine::GEOMETRIC_MEAN);
    ASSERT_TRUE(physicsmaterial::combineFromName("average", mode));
    EXPECT_EQ(mode, PhysicsMaterialCombine::AVERAGE);
    const PhysicsMaterialParseResult unity = physicsmaterial::parse("{\"friction\": 1, \"frictionCombine\": \"Mean\"}");
    ASSERT_TRUE(unity.ok);
    EXPECT_EQ(unity.data.frictionCombine, PhysicsMaterialCombine::GEOMETRIC_MEAN);

    lua_State* L = LuaBinding::getLuaState();
    if (!L) {
        World2D world;   // makes the Lua state
        L = LuaBinding::getLuaState();
    }
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L, "meanResult = PhysicsMaterial.new({frictionCombine = 'Mean'}).frictionCombine == PhysicsMaterialCombine.GEOMETRIC_MEAN"), LUA_OK)
        << lua_tostring(L, -1);
    lua_getglobal(L, "meanResult");
    EXPECT_TRUE(lua_toboolean(L, -1)) << "Lua names go through the same table";
    lua_pop(L, 1);
}

// ---- 7. C++ message overrides, whatever their access ---------------------------------------

TEST(Physics2DReview, CppOverridesAreFoundWithoutNamingTheMember) {
    Hanging h(JointBreakAction::CALLBACK_ONLY);
    int calls = 0;
    Counter counter(&h.world.scene, h.joint, &calls);
    Silent silent(&h.world.scene, h.weight);
    addScript(h.world, h.joint, &counter);
    addScript(h.world, h.weight, &silent);
    const ScriptMessageMask bit = scriptMessageBit(ScriptMessage::JointBreak2D);
    EXPECT_TRUE(ScriptMessages::receivers(&h.world.scene, h.joint) & bit);
    h.world.load();
    h.world.step();
    EXPECT_EQ(calls, 1) << "the private override is called";
    EXPECT_TRUE(ScriptMessages::receivers(&h.world.scene, h.joint) & bit);
    EXPECT_FALSE(ScriptMessages::receivers(&h.world.scene, h.weight) & bit) << "seen not to override it";
    EXPECT_EQ(ScriptMessages::send(&h.world.scene, h.weight, ScriptMessage::JointBreak2D, nullptr,
                                   [&](ScriptBase& script) { Joint2D joint(&h.world.scene, h.joint); script.onJointBreak2D(joint); }), 0);
    h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    h.world.scene.getComponent<ScriptComponent>(h.weight).scripts.clear();
}

// ---- 8. the editor's pieces are the body's ---------------------------------------------------

TEST(Physics2DReview, EditorPiecesAreTheBodysOwnAndCached) {
    World2D world;
    const Entity e = world.entity(Vector3(0, 0, 0));
    world.scene.getComponent<Transform>(e).scale = Vector3(2, 3, 1);
    Rigidbody2DComponent body;
    body.type = BodyType::STATIC;
    Collider2D comb;
    comb.type = Collider2DType::POLYGON;
    for (int i = 0; i < 20; i++) {
        comb.points.push_back(Vector2(i * 10.0f, 0));
        comb.points.push_back(Vector2(i * 10.0f + 5, 0));
        comb.points.push_back(Vector2(i * 10.0f + 5, -20));
        comb.points.push_back(Vector2(i * 10.0f + 10, -20));
    }
    comb.points.push_back(Vector2(200, 0));
    comb.points.push_back(Vector2(200, 10));
    comb.points.push_back(Vector2(0, 10));
    body.colliders.push_back(comb);
    world.scene.addComponent<Rigidbody2DComponent>(e, body);
    world.load();

    Collider2D& collider = world.component(e).colliders[0];
    const size_t shapes = collider.shapes.size();
    ASSERT_GT(shapes, 1u);
    const uint64_t key = collider.decompositionKey;
    PhysicsSystem* physics = world.physics();
    const std::vector<std::vector<Vector2>> local = physics->getCollider2DPiecesLocal(e, collider);
    EXPECT_EQ(collider.decompositionKey, key) << "from the cache, not decomposed again";
    EXPECT_EQ(local.size(), shapes) << "one piece per Box2D shape";
    const float ptm = physics->getPointsToMeterScale2D();
    for (size_t i = 0; i < local.size(); i++) {
        for (size_t j = 0; j < local[i].size(); j++) {
            EXPECT_NEAR(local[i][j].x * 2.0f / ptm, collider.pieces[i][j].x, 1e-4f);
            EXPECT_NEAR(local[i][j].y * 3.0f / ptm, collider.pieces[i][j].y, 1e-4f);
        }
    }
    // a copy (as MCP reads an unsaved edit) gets the same count, in edit mode too
    Collider2D copy = comb;
    physics->updateCollider2DPieces(e, copy);
    EXPECT_EQ(copy.pieces.size(), shapes);
}

// ---- 9. a runtime material change keeps the resting contacts --------------------------------

TEST(Physics2DReview, MaterialChangeKeepsContactsAndApplies) {
    World2D world;
    const Entity ground = world.body(Vector3(0, -20, 0), BodyType::STATIC);
    world.rb(ground).createCenteredBoxCollider(4000, 40);
    const Entity box = world.body(Vector3(0, 10, 0), BodyType::DYNAMIC);
    world.rb(box).createCenteredBoxCollider(20, 20);
    int begins = 0, ends = 0;
    world.physics()->beginContact2D.add("review", std::function<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>(
        [&](Rigidbody2D, unsigned long, Rigidbody2D, unsigned long) { begins++; }));
    world.physics()->endContact2D.add("review", std::function<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>(
        [&](Rigidbody2D, unsigned long, Rigidbody2D, unsigned long) { ends++; }));
    world.load();
    world.step(60);
    ASSERT_EQ(begins, 1);
    ASSERT_EQ(ends, 0);

    // ice under a sliding box: no contact end / begin, and the box keeps its speed
    world.rb(box).setLinearVelocity(Vector2(200, 0));
    world.rb(box).setColliderMaterial(0, PhysicsMaterial::create(0.0f, 0.0f, PhysicsMaterialCombine::MINIMUM, PhysicsMaterialCombine::MAXIMUM));
    world.step(30);
    EXPECT_EQ(begins, 1) << "no new begin";
    EXPECT_EQ(ends, 0) << "no end";
    EXPECT_NEAR(world.rb(box).getLinearVelocity().x, 200.0f, 1.0f) << "the resting contact uses the new friction";

    // back to rough: it stops
    world.rb(box).setColliderMaterial(0, PhysicsMaterial::create(1.0f, 0.0f, PhysicsMaterialCombine::MAXIMUM, PhysicsMaterialCombine::MAXIMUM));
    world.step(60);
    EXPECT_LT(std::fabs(world.rb(box).getLinearVelocity().x), 1.0f);
    EXPECT_EQ(begins + ends, 1);
    world.physics()->beginContact2D.remove("review");
    world.physics()->endContact2D.remove("review");
}

// ---- 10. Lua argument errors are Lua errors ------------------------------------------------

TEST(Physics2DReview, LuaArgumentErrorsAreCatchableErrors) {
    World2D world;
    const Entity pivot = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    Joint2DComponent joint = makeJoint2D(Joint2DType::REVOLUTE);
    joint.bodyA = pivot;
    const Entity e = world.joint(joint);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    luabridge::setGlobal(L, Joint2D(&world.scene, e), "reviewJoint");
    luabridge::setGlobal(L, Rigidbody2D(&world.scene, pivot), "reviewBody");
    ASSERT_EQ(luaL_dostring(L,
        "local ok1, e1 = pcall(function() reviewJoint.bodyA = 42 end)\n"
        "local ok2, e2 = pcall(function() reviewBody.material = 42 end)\n"
        "local ok3, e3 = pcall(function() reviewBody:setColliderMaterial(0, 42) end)\n"
        "reviewErrors = {ok1, tostring(e1), ok2, tostring(e2), ok3, tostring(e3)}"), LUA_OK) << lua_tostring(L, -1);
    luabridge::LuaRef errors = luabridge::getGlobal(L, "reviewErrors");
    EXPECT_FALSE(errors[1].cast<bool>().value());
    EXPECT_NE(errors[2].cast<std::string>().value().find("Joint2D.bodyA"), std::string::npos) << errors[2].cast<std::string>().value();
    EXPECT_FALSE(errors[3].cast<bool>().value());
    EXPECT_NE(errors[4].cast<std::string>().value().find("Rigidbody2D.material"), std::string::npos);
    EXPECT_FALSE(errors[5].cast<bool>().value());
    luaL_dostring(L, "reviewJoint = nil reviewBody = nil reviewErrors = nil");
}

// ---- 10. angles in the project's unit --------------------------------------------------------

TEST(Physics2DReview, JointAngleDefaultsFollowTheAngleUnit) {
    const bool degrees = Engine::isUseDegrees();
    Engine::setUseDegrees(false);
    const Joint2DComponent radians = makeJoint2D(Joint2DType::REVOLUTE);
    EXPECT_NEAR(radians.lowerAngle, -(float)M_PI / 2, 1e-5f);
    EXPECT_NEAR(radians.upperAngle, (float)M_PI / 2, 1e-5f);
    Engine::setUseDegrees(true);
    const Joint2DComponent inDegrees = makeJoint2D(Joint2DType::REVOLUTE);
    EXPECT_FLOAT_EQ(inDegrees.lowerAngle, -90.0f);
    Engine::setUseDegrees(degrees);
}

// ---- spec 6.1.4: a single-piece polygon edit keeps its shape ---------------------------------

TEST(Physics2DReview, SinglePiecePolygonEditKeepsItsShape) {
    World2D world;
    const Entity e = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    world.rb(e).createPolygonCollider(boxPoints(20, 20));
    world.rb(e).createCircleCollider(Vector2(40, 0), 5);
    world.load();
    world.step();
    const b2ShapeId before = world.component(e).colliders[0].shapes[0];
    const float massBefore = world.rb(e).getMass();
    world.rb(e).setColliderPoints(0, boxPoints(40, 20));
    ASSERT_EQ(world.component(e).colliders[0].shapes.size(), 1u);
    EXPECT_TRUE(B2_ID_EQUALS(world.component(e).colliders[0].shapes[0], before)) << "b2Shape_SetPolygon, same shape";
    EXPECT_GT(world.rb(e).getMass(), massBefore * 1.5f) << "the mass follows the new shape";
    b2Polygon polygon = b2Shape_GetPolygon(before);
    float maxX = 0.0f;
    for (int i = 0; i < polygon.count; i++) maxX = std::max(maxX, polygon.vertices[i].x);
    EXPECT_NEAR(maxX, 20.0f / world.physics()->getPointsToMeterScale2D(), 1e-4f);
    // a concave outline is several pieces: made again
    world.rb(e).setColliderPoints(0, {Vector2(0, 0), Vector2(40, 0), Vector2(40, 10), Vector2(10, 10), Vector2(10, 40), Vector2(0, 40)});
    world.step();
    EXPECT_EQ(world.component(e).colliders[0].shapes.size(), 2u);
}

// ---- coverage: a collider removed during a Box2D step, a joint whose body goes away ----------

TEST(Physics2DReview, ColliderRemovedDuringAStepReloadsTheBodyAndItsJoints) {
    World2D world;
    const Entity ground = world.body(Vector3(0, -20, 0), BodyType::STATIC);
    world.rb(ground).createCenteredBoxCollider(4000, 40);
    const Entity crate = world.body(Vector3(0, 12, 0), BodyType::DYNAMIC);
    world.rb(crate).createCenteredBoxCollider(20, 20);
    world.rb(crate).createCircleCollider(Vector2(30, 0), 5);
    world.rb(crate).setColliderPreSolveEvents(0, true);
    const Entity hook = world.body(Vector3(0, 100, 0), BodyType::STATIC);
    Joint2DComponent rope = makeJoint2D(Joint2DType::DISTANCE);
    rope.bodyA = crate;
    rope.bodyB = hook;
    const Entity joint = world.joint(rope);
    world.load();
    const float length = world.jointComponent(joint).length;

    bool removed = false;
    bool removedWhileStepping = false;
    world.physics()->preSolve2D.add("review", std::function<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>(
        [&](Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D) {
            if (!removed) {
                removed = true;
                removedWhileStepping = world.physics()->isSteppingWorld2D();
                world.rb(crate).removeCollider(1);
            }
            return true;
        }));
    for (int i = 0; i < 60 && !removed; i++) world.step();
    world.physics()->preSolve2D.remove("review");
    ASSERT_TRUE(removed);
    EXPECT_TRUE(removedWhileStepping);
    EXPECT_EQ(world.component(crate).colliders.size(), 1u);
    EXPECT_TRUE(world.component(crate).needReloadBody) << "deferred: Box2D is locked";

    world.step();
    ASSERT_TRUE(b2Body_IsValid(world.component(crate).body));
    EXPECT_EQ(world.component(crate).colliders[0].shapes.size(), 1u);
    for (const b2ShapeId& shape : world.component(crate).colliders[0].shapes) {
        EXPECT_EQ((size_t)b2Shape_GetUserData(shape), 0u);
    }
    // the joint Box2D destroyed with the old body is made again, with its length of before
    EXPECT_TRUE(b2Joint_IsValid(world.jointComponent(joint).joint));
    EXPECT_NEAR(world.jointComponent(joint).length, length, 1e-3f);
}

TEST(Physics2DReview, AJointWhoseBodyGoesAwayLeavesTheWorld) {
    Hanging h(JointBreakAction::DESTROY);
    h.world.jointComponent(h.joint).breakForce = std::numeric_limits<float>::infinity();
    h.world.load();
    h.world.step(5);
    ASSERT_TRUE(b2Joint_IsValid(h.world.jointComponent(h.joint).joint));
    h.world.scene.removeComponent<Rigidbody2DComponent>(h.anchor);
    h.world.step(5);
    EXPECT_FALSE(b2Joint_IsValid(h.world.jointComponent(h.joint).joint));
    EXPECT_TRUE(h.world.jointComponent(h.joint).loadFailed) << "not made, reported once";
    EXPECT_LT(h.world.rb(h.weight).getPosition().y, 59.0f) << "the weight falls";
}

// ---- S96 WP-C: movePosition / moveRotation and the interpolated teleport in 2D ------------------

namespace {

    Entity moveBox2D(World2D& world, const Vector3& position, BodyType type) {
        const Entity e = world.body(position, type);
        Collider2D collider;
        collider.type = Collider2DType::POLYGON;
        collider.points = boxPoints(64, 64);
        world.component(e).colliders.push_back(collider);
        return e;
    }

}

TEST(Physics2DReview, MovePosition2D) {
    World2D world(Vector2(0, 0));
    world.scene.setPhysicsInterpolation(true);
    const Entity mover = moveBox2D(world, Vector3(0, 0, 0), BodyType::KINEMATIC);
    // touching (within Box2D's speculative distance): the move pushes it in that step
    const Entity crate = moveBox2D(world, Vector3(64.5f, 0, 0), BodyType::DYNAMIC);
    const Entity ball = moveBox2D(world, Vector3(0, 300, 0), BodyType::DYNAMIC);
    world.load();
    world.step();

    // kinematic: there at the end of the step by velocity, pushing; stopped the step after (a
    // turn under Box2D's 45 degrees a step)
    const float quarter = Angle::radToDefault((float)M_PI / 6.0f);
    world.rb(mover).movePosition(Vector2(64, 0));
    world.rb(mover).moveRotation(quarter);
    world.rb(ball).setLinearVelocity(Vector2(0, 128));
    world.step();
    EXPECT_NEAR(world.rb(mover).getPosition().x, 64.0f, 0.01f);
    EXPECT_NEAR(world.rb(mover).getAngle(), quarter, Angle::radToDefault(1e-3f));
    world.physics()->interpolate(0.5);
    world.resolve();
    EXPECT_NEAR(world.scene.getComponent<Transform>(mover).position.x, 32.0f, 0.01f) << "drawn half way";
    world.step(2);
    EXPECT_GT(world.rb(crate).getLinearVelocity().x, 10.0f) << "pushed";
    EXPECT_NEAR(world.rb(mover).getLinearVelocity().x, 0.0f, 1e-4f);
    EXPECT_NEAR(world.rb(mover).getAngularVelocity(), 0.0f, 1e-4f);

    // dynamic: placed before the step, its velocity kept, drawn half way
    const float y = world.rb(ball).getPosition().y;
    world.rb(ball).movePosition(Vector2(320, y));
    world.step();
    EXPECT_NEAR(world.rb(ball).getPosition().x, 320.0f, 0.01f);
    EXPECT_NEAR(world.rb(ball).getPosition().y, y + 128.0f * STEP, 0.01f);
    EXPECT_NEAR(world.rb(ball).getLinearVelocity().y, 128.0f, 0.01f);
    world.physics()->interpolate(0.5);
    world.resolve();
    EXPECT_NEAR(world.scene.getComponent<Transform>(ball).position.x, 160.0f, 0.01f);

    // setPosition slides only when asked
    world.step();
    const float from = world.rb(ball).getPosition().x;
    world.rb(ball).setPosition(Vector2(from + 200.0f, world.rb(ball).getPosition().y), true);
    world.physics()->interpolate(0.5);
    world.resolve();
    EXPECT_GT(world.scene.getComponent<Transform>(ball).position.x, from + 90.0f);
    EXPECT_LT(world.scene.getComponent<Transform>(ball).position.x, from + 110.0f);
    world.rb(ball).setPosition(Vector2(from + 400.0f, world.rb(ball).getPosition().y));
    world.physics()->interpolate(0.5);
    world.resolve();
    EXPECT_NEAR(world.scene.getComponent<Transform>(ball).position.x, from + 400.0f, 0.01f);
}
