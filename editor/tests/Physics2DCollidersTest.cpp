// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 2D colliders and bodies without caps (physics-2d spec T8, T9, T11-T16): any number of
// colliders and points, the per-collider rebuild, concave polygons, short chains, the Unity
// Rigidbody2D fields, materials with their combine modes, serialization and the contactEvents
// default.

#include <gtest/gtest.h>

#include "Physics2DTestWorld.h"

#include "Factory.h"
#include "Stream.h"
#include "component/PhysicsCommon.h"
#include "physics/PhysicsMaterial.h"
#include "pool/PhysicsMaterialPool.h"
#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::physics2dtest;

namespace {

    std::vector<Vector2> wavyChain(int points, float width){
        std::vector<Vector2> out;
        for (int i = 0; i < points; i++){
            const float x = -width * 0.5f + width * i / (points - 1);
            out.push_back(Vector2(x, 20.0f * std::sin(i * 0.3f)));
        }
        return out;
    }

}

// ---- T8: no caps -------------------------------------------------------------------------------

TEST(Physics2DColliders, ManyCollidersAndLongChainsBuild) {
    World2D world;
    const Entity e = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    Rigidbody2D body = world.rb(e);
    for (int i = 0; i < 40; i++){
        body.createCircleCollider(Vector2(i * 30.0f, 0), 10);
    }
    body.createChainCollider(wavyChain(300, 3000.0f), false);
    world.load();
    EXPECT_EQ(body.getNumColliders(), 41u);
    // an open chain of n points has n - 3 colliding segments (Box2D's ghost vertices)
    const int segments = b2Chain_GetSegmentCount(world.component(e).colliders[40].chain);
    EXPECT_GE(segments, 296);
    EXPECT_EQ(b2Body_GetShapeCount(world.component(e).body), 40 + segments);
    EXPECT_EQ(body.getColliderPieceCount(40), (size_t)segments);
    EXPECT_EQ(body.getColliderPoints(40).size(), 300u);
    EXPECT_EQ(world.physics()->getChainsCreated2D(), 1u);
}

// ---- T9: one collider edit rebuilds that collider only --------------------------------------------

TEST(Physics2DColliders, EditingOneColliderRebuildsOnlyIt) {
    World2D world(Vector2(0, 0));
    const Entity anchor = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    const Entity e = world.body(Vector3(100, 0, 0), BodyType::DYNAMIC);
    Rigidbody2D body = world.rb(e);
    for (int i = 0; i < 5; i++){
        body.createCenteredBoxCollider(10, 10, Vector2(i * 20.0f, 0), 0);
    }
    Joint2DComponent joint = makeJoint2D(Joint2DType::REVOLUTE);
    joint.bodyA = anchor;
    joint.bodyB = e;
    const Entity jointEntity = world.joint(joint);
    world.load();
    body.setLinearVelocity(Vector2(0, 50));
    world.step();

    const b2BodyId bodyBefore = world.component(e).body;
    const b2JointId jointBefore = world.jointComponent(jointEntity).joint;
    const uint64_t shapesBefore = world.physics()->getShapesCreated2D();
    const uint64_t chainsBefore = world.physics()->getChainsCreated2D();
    const Vector2 velocityBefore = body.getLinearVelocity();
    const std::vector<b2ShapeId> otherShapes = body.getBox2DShapes(3);

    body.setColliderPoints(2, {Vector2(30, -8), Vector2(50, -8), Vector2(50, 8), Vector2(30, 8)});
    world.step();

    EXPECT_EQ(world.physics()->getShapesCreated2D() - shapesBefore, 1u) << "collider 2 only";
    EXPECT_EQ(world.physics()->getChainsCreated2D(), chainsBefore);
    EXPECT_TRUE(B2_ID_EQUALS(world.component(e).body, bodyBefore));
    EXPECT_TRUE(B2_ID_EQUALS(world.jointComponent(jointEntity).joint, jointBefore));
    EXPECT_TRUE(b2Joint_IsValid(jointBefore));
    EXPECT_TRUE(B2_ID_EQUALS(body.getBox2DShapes(3)[0], otherShapes[0])) << "the others are the same shapes";
    EXPECT_GT(body.getLinearVelocity().length(), velocityBefore.length() * 0.5f) << "kept moving";

    // removal: later colliders report their new index
    ASSERT_TRUE(body.removeCollider(1));
    EXPECT_EQ(body.getNumColliders(), 4u);
    for (size_t i = 0; i < 4; i++){
        for (const b2ShapeId& shape : body.getBox2DShapes(i)){
            EXPECT_EQ(reinterpret_cast<size_t>(b2Shape_GetUserData(shape)), i);
        }
    }
    EXPECT_EQ(b2Body_GetShapeCount(world.component(e).body), 4);
    EXPECT_THROW(body.removeCollider(9), std::runtime_error);
}

// ---- T11, T12: concave polygons and short chains --------------------------------------------------

TEST(Physics2DColliders, BallRestsInsideAConcavePolygon) {
    World2D world;
    const Entity cup = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    // a U: outer 200 x 120, walls 20 thick
    world.rb(cup).createPolygonCollider({Vector2(-100, 0), Vector2(100, 0), Vector2(100, 120), Vector2(80, 120),
                                         Vector2(80, 20), Vector2(-80, 20), Vector2(-80, 120), Vector2(-100, 120)});
    const Entity ball = world.body(Vector3(30, 200, 0), BodyType::DYNAMIC);
    world.rb(ball).createCircleCollider(Vector2(0, 0), 15);
    world.load();
    EXPECT_GT(world.rb(cup).getColliderPieceCount(0), 1u) << "split in convex pieces";
    world.step(240);
    const Vector2 position = world.rb(ball).getPosition();
    EXPECT_GT(position.x, -80.0f);
    EXPECT_LT(position.x, 80.0f);
    EXPECT_NEAR(position.y, 35.0f, 1.5f) << "resting on the inner bottom";
}

TEST(Physics2DColliders, ShortChainsAreSegments) {
    World2D world;
    const Entity e = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    Rigidbody2D body = world.rb(e);
    body.createChainCollider({Vector2(-50, 0), Vector2(50, 0)}, false);
    body.createChainCollider({Vector2(-50, 0), Vector2(0, 10), Vector2(50, 0)}, false);
    body.createChainCollider({Vector2(-50, 0), Vector2(0, 10), Vector2(50, 0)}, true);
    world.load();
    EXPECT_EQ(body.getColliderPieceCount(0), 1u);
    EXPECT_EQ(body.getColliderPieceCount(1), 2u);
    EXPECT_EQ(body.getColliderPieceCount(2), 3u) << "a loop of 3 closes";
    EXPECT_FALSE(b2Chain_IsValid(world.component(e).colliders[0].chain));
    // a ball lands on the 2-point edge (two-sided, Unity EdgeCollider2D)
    const Entity ball = world.body(Vector3(0, 100, 0), BodyType::DYNAMIC);
    world.rb(ball).createCircleCollider(Vector2(0, 0), 5);
    body.removeCollider(2);
    body.removeCollider(1);
    world.step(120);
    EXPECT_NEAR(world.rb(ball).getPosition().y, 5.0f, 1.0f);
}

// ---- T13: the Unity Rigidbody2D fields --------------------------------------------------------

TEST(Physics2DColliders, BodyFields) {
    World2D world;
    const Entity floating = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    world.rb(floating).createCircleCollider(Vector2(0, 0), 10);
    world.component(floating).gravityScale = 0.0f;

    const Entity damped = world.body(Vector3(500, 0, 0), BodyType::DYNAMIC);
    world.rb(damped).createCircleCollider(Vector2(0, 0), 10);
    world.component(damped).gravityScale = 0.0f;
    world.component(damped).linearDamping = 1.0f;

    const Entity heavy = world.body(Vector3(-500, 0, 0), BodyType::DYNAMIC);
    world.rb(heavy).createCircleCollider(Vector2(0, 0), 10);
    world.component(heavy).useAutoMass = false;
    world.component(heavy).mass = 1.0f;
    world.component(heavy).gravityScale = 0.0f;

    world.load();
    world.rb(damped).setLinearVelocity(Vector2(100, 0));
    // 0.69 s: e^-0.69 = 0.5
    world.step(41);
    EXPECT_NEAR(world.rb(floating).getPosition().y, 0.0f, 1e-4f);
    EXPECT_NEAR(world.rb(damped).getLinearVelocity().x, 50.0f, 2.5f);
    EXPECT_FLOAT_EQ(world.rb(heavy).getMass(), 1.0f);
    // the explicit mass is applied again after a collider rebuild
    world.rb(heavy).setColliderRadius(0, 40);
    world.step();
    EXPECT_FLOAT_EQ(world.rb(heavy).getMass(), 1.0f);
    // Lua-style live setters write the fields
    world.rb(heavy).setMass(3.0f);
    EXPECT_FLOAT_EQ(world.rb(heavy).getMass(), 3.0f);
    world.rb(heavy).setFixedRotation(true);
    EXPECT_FLOAT_EQ(world.rb(heavy).getMass(), 3.0f) << "fixing rotation keeps the explicit mass";
    EXPECT_TRUE(world.component(heavy).fixedRotation);
    world.rb(heavy).setBullet(true);
    EXPECT_EQ(world.component(heavy).collisionDetection, CollisionDetectionMode2D::CONTINUOUS);
    world.rb(heavy).setEnableSleep(false);
    EXPECT_EQ(world.component(heavy).sleepingMode, RigidbodySleepMode2D::NEVER_SLEEP);
}

TEST(Physics2DColliders, ContinuousBodiesDoNotTunnel) {
    float x[2];
    for (int mode = 0; mode < 2; mode++){
        World2D world(Vector2(0, 0));
        const Entity wall = world.body(Vector3(200, 0, 0), BodyType::DYNAMIC);
        world.rb(wall).createCenteredBoxCollider(4, 400);
        world.component(wall).useAutoMass = false;
        world.component(wall).mass = 1000.0f;
        const Entity bullet = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
        world.rb(bullet).createCircleCollider(Vector2(0, 0), 2);
        world.component(bullet).collisionDetection = mode == 0 ? CollisionDetectionMode2D::DISCRETE : CollisionDetectionMode2D::CONTINUOUS;
        world.load();
        world.rb(bullet).setLinearVelocity(Vector2(2000, 0));
        world.step(30);
        x[mode] = world.rb(bullet).getPosition().x;
    }
    // (a discrete one may tunnel through the thin moving wall: x[0])
    (void)x[0];
    EXPECT_LT(x[1], 200.0f) << "a continuous body stops at it";
}

TEST(Physics2DColliders, StartAsleepAndExplicitCenterOfMass) {
    World2D world;
    const Entity sleeper = world.body(Vector3(0, 100, 0), BodyType::DYNAMIC);
    world.rb(sleeper).createCircleCollider(Vector2(0, 0), 10);
    world.component(sleeper).sleepingMode = RigidbodySleepMode2D::START_ASLEEP;
    const Entity offset = world.body(Vector3(200, 100, 0), BodyType::DYNAMIC);
    world.rb(offset).createCenteredBoxCollider(40, 40);
    world.component(offset).autoCenterOfMass = false;
    world.component(offset).centerOfMass = Vector2(10, 0);
    world.load();
    world.step(10);
    EXPECT_NEAR(world.rb(sleeper).getPosition().y, 100.0f, 1e-4f) << "asleep until touched";
    EXPECT_NEAR(b2Body_GetLocalCenterOfMass(world.component(offset).body).x * 64.0f, 10.0f, 1e-3f);
    EXPECT_NEAR(world.rb(offset).getCenterOfMass().x, 10.0f, 1e-3f);
}

// ---- T14: materials --------------------------------------------------------------------------------

TEST(Physics2DColliders, CombineSurfacePriority) {
    using C = PhysicsMaterialCombine;
    EXPECT_FLOAT_EQ(combineSurface(0.0f, C::MINIMUM, 1.0f, C::NONE, C::GEOMETRIC_MEAN), 0.0f);
    EXPECT_FLOAT_EQ(combineSurface(0.2f, C::AVERAGE, 0.6f, C::MAXIMUM, C::GEOMETRIC_MEAN), 0.6f);
    EXPECT_FLOAT_EQ(combineSurface(2.0f, C::MULTIPLY, 0.5f, C::MINIMUM, C::GEOMETRIC_MEAN), 1.0f);
    EXPECT_FLOAT_EQ(combineSurface(0.4f, C::NONE, 0.9f, C::NONE, C::GEOMETRIC_MEAN), std::sqrt(0.4f * 0.9f));
    EXPECT_FLOAT_EQ(combineSurface(0.4f, C::NONE, 0.9f, C::NONE, C::MAXIMUM), 0.9f);
    // every pair: the higher mode of the two wins
    const C modes[] = {C::AVERAGE, C::GEOMETRIC_MEAN, C::MINIMUM, C::MULTIPLY, C::MAXIMUM};
    for (C a : modes){
        for (C b : modes){
            const C winner = (int)a > (int)b ? a : b;
            EXPECT_FLOAT_EQ(combineSurface(0.3f, a, 0.8f, b, C::GEOMETRIC_MEAN), combineSurface(0.3f, winner, 0.8f, winner, C::GEOMETRIC_MEAN));
        }
    }
}

namespace {

    // a box sliding at 200 points/s on a long static floor, its x after 1 s
    float slide(const PhysicsMaterial& boxMaterial, const PhysicsMaterial& floorMaterial, PhysicsMaterial* changeTo = nullptr){
        World2D world;
        const Entity floor = world.body(Vector3(0, -20, 0), BodyType::STATIC);
        world.rb(floor).createCenteredBoxCollider(6000, 40);
        if (floorMaterial.valid()) world.rb(floor).setColliderMaterial(0, floorMaterial);
        const Entity box = world.body(Vector3(0, 10, 0), BodyType::DYNAMIC);
        world.rb(box).createCenteredBoxCollider(20, 20);
        if (boxMaterial.valid()) world.rb(box).setColliderMaterial(0, boxMaterial);
        world.load();
        world.step(10);
        world.rb(box).setLinearVelocity(Vector2(200, 0));
        world.step(changeTo ? 20 : 60);
        if (changeTo){
            // an edit of the material in use reaches the touching pair at the next step
            changeTo->setFriction(1.0f);
            world.step(40);
        }
        return world.rb(box).getLinearVelocity().x;
    }

}

TEST(Physics2DColliders, MaterialsCombineAsTheirModesSay) {
    const PhysicsMaterial ice = PhysicsMaterial::create(0.0f, 0.0f, PhysicsMaterialCombine::MINIMUM);
    const PhysicsMaterial rubber = PhysicsMaterial::create(1.0f, 0.0f);
    EXPECT_GT(slide(ice, rubber), 190.0f) << "minimum of 0 and 1: no friction";
    EXPECT_LT(slide(PhysicsMaterial(), PhysicsMaterial()), 1.0f) << "no material: geometric mean of 0.6, it stops";
    EXPECT_LT(slide(rubber, PhysicsMaterial()), 1.0f);

    // a material made slippery then grippy while sliding
    PhysicsMaterial changing = PhysicsMaterial::create(0.0f, 0.0f, PhysicsMaterialCombine::MINIMUM);
    EXPECT_LT(slide(changing, rubber, &changing), 1.0f) << "the resting contact took the new friction";

    // bounciness 1 with Maximum bounces back up
    World2D world;
    const Entity floor = world.body(Vector3(0, -20, 0), BodyType::STATIC);
    world.rb(floor).createCenteredBoxCollider(2000, 40);
    const Entity ball = world.body(Vector3(0, 200, 0), BodyType::DYNAMIC);
    world.rb(ball).createCircleCollider(Vector2(0, 0), 10);
    world.rb(ball).setColliderMaterial(0, PhysicsMaterial::create(0.4f, 1.0f, PhysicsMaterialCombine::AVERAGE, PhysicsMaterialCombine::MAXIMUM));
    world.load();
    float lowest = 1e9f, highestAfter = -1e9f;
    bool bounced = false;
    for (int i = 0; i < 180; i++){
        world.step();
        const float y = world.rb(ball).getPosition().y;
        if (!bounced){
            lowest = std::min(lowest, y);
            if (world.rb(ball).getLinearVelocity().y > 0.0f && y < 50.0f) bounced = true;
        }else{
            highestAfter = std::max(highestAfter, y);
        }
    }
    EXPECT_TRUE(bounced);
    EXPECT_GT(highestAfter, 0.8f * 200.0f);
}

TEST(Physics2DColliders, MaterialAssetsParseWriteAndPool) {
    // canonical write, lenient parse, Unity's names
    PhysicsMaterialData data = PhysicsMaterialData::preset2D();
    data.guid = "0123456789abcdef0123456789abcdef";
    const std::string text = physicsmaterial::write(data);
    const PhysicsMaterialParseResult parsed = physicsmaterial::parse(text);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(physicsmaterial::write(parsed.data), text);
    EXPECT_FLOAT_EQ(parsed.data.friction, 0.4f);
    EXPECT_EQ(parsed.data.frictionCombine, PhysicsMaterialCombine::GEOMETRIC_MEAN);
    EXPECT_EQ(parsed.data.bounceCombine, PhysicsMaterialCombine::MAXIMUM);

    const PhysicsMaterialParseResult unity = physicsmaterial::parse(
        "{\"dynamicFriction\": 2, \"bounce\": 1.5, \"frictionCombine\": \"Minimum\", \"custom\": [1, 2]}");
    ASSERT_TRUE(unity.ok);
    EXPECT_FLOAT_EQ(unity.data.friction, 2.0f);
    EXPECT_FLOAT_EQ(unity.data.bounciness, 1.0f) << "clamped";
    EXPECT_FALSE(unity.warnings.empty());
    EXPECT_EQ(unity.data.frictionCombine, PhysicsMaterialCombine::MINIMUM);
    EXPECT_NE(physicsmaterial::write(unity.data).find("\"custom\": [1,2]"), std::string::npos) << "unknown keys kept";
    EXPECT_FALSE(physicsmaterial::parse("{\"format\": \"other\"}").ok);
    EXPECT_FALSE(physicsmaterial::parse("not json").ok);

    // the pool shares one runtime copy per file; invalidate reloads it
    // files are read through System::instance()
    ASSERT_TRUE(physicstest::installTestSystem());
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "jasper_physics2d_tire.physicsmaterial";
    {
        PhysicsMaterialData tire;
        tire.friction = 2.0f;
        std::ofstream(file) << physicsmaterial::write(tire);
    }
    const PhysicsMaterial first = PhysicsMaterial::load(file.string());
    const PhysicsMaterial second = PhysicsMaterial::load(file.string());
    ASSERT_TRUE(first.valid());
    EXPECT_TRUE(first == second);
    EXPECT_FLOAT_EQ(first.getFriction(), 2.0f);
    EXPECT_EQ(first.getPath(), file.string());
    {
        PhysicsMaterialData tire;
        tire.friction = 3.0f;
        std::ofstream(file) << physicsmaterial::write(tire);
    }
    PhysicsMaterialPool::invalidate(file.string());
    EXPECT_FLOAT_EQ(PhysicsMaterial::load(file.string()).getFriction(), 3.0f);
    std::filesystem::remove(file);
    PhysicsMaterialPool::invalidate(file.string());
    EXPECT_FALSE(PhysicsMaterial::load(file.string()).valid());
}

// ---- T15, T16: serialization and the contactEvents default ----------------------------------------

TEST(Physics2DColliders, EveryBodyFieldSurvivesEncodeAndDecode) {
    Rigidbody2DComponent body;
    body.type = BodyType::DYNAMIC;
    body.gravityScale = 0.5f;
    body.linearDamping = 0.2f;
    body.angularDamping = 0.05f;
    body.fixedRotation = true;
    body.collisionDetection = CollisionDetectionMode2D::CONTINUOUS;
    body.sleepingMode = RigidbodySleepMode2D::START_ASLEEP;
    body.useAutoMass = false;
    body.mass = 3.0f;
    body.autoCenterOfMass = false;
    body.centerOfMass = Vector2(1, 2);
    body.autoInertia = false;
    body.inertia = 0.25f;
    body.material = "physics/body.physicsmaterial";
    Collider2D collider;
    collider.type = Collider2DType::CHAIN;
    collider.points = wavyChain(70, 700.0f);
    collider.material = "physics/tire.physicsmaterial";
    collider.receiver = 12;
    collider.contactEvents = false;
    body.colliders.push_back(collider);

    const YAML::Node node = Stream::encodeRigidbody2DComponent(body);
    EXPECT_EQ(node["numColliders"].as<unsigned int>(), 1u) << "still written for older editors";
    const Rigidbody2DComponent decoded = Stream::decodeRigidbody2DComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(YAML::Dump(Stream::encodeRigidbody2DComponent(decoded)), YAML::Dump(node));
    EXPECT_EQ(decoded.colliders.size(), 1u);
    EXPECT_EQ(decoded.colliders[0].points.size(), 70u);
    EXPECT_EQ(decoded.colliders[0].receiver, 12u);
    EXPECT_EQ(decoded.sleepingMode, RigidbodySleepMode2D::START_ASLEEP);
    EXPECT_EQ(decoded.collisionDetection, CollisionDetectionMode2D::CONTINUOUS);
    EXPECT_FALSE(decoded.useAutoMass);
    EXPECT_EQ(decoded.material, "physics/body.physicsmaterial");

    // the exported-build code: every non-default field, the points in chunks of 64
    Scene scene;
    Entity e = scene.createEntity();
    scene.addComponent<Rigidbody2DComponent>(e, body);
    const std::string code = Factory::createRigidbody2DComponent(4, &scene, e, "scene", "entity", false, nullptr);
    for (const char* expected : {"rigidbody2d.gravityScale = 0.500000f;", "rigidbody2d.fixedRotation = true;",
                                 "rigidbody2d.collisionDetection = CollisionDetectionMode2D::CONTINUOUS;",
                                 "rigidbody2d.sleepingMode = RigidbodySleepMode2D::START_ASLEEP;", "rigidbody2d.useAutoMass = false;",
                                 "rigidbody2d.mass = 3.000000f;", "rigidbody2d.centerOfMass = Vector2(1.000000f, 2.000000f);",
                                 "rigidbody2d.inertia = 0.250000f;", "rigidbody2d.colliders.resize(1);",
                                 "rigidbody2d.colliders[0].points = {", "rigidbody2d.colliders[0].points.insert(",
                                 "rigidbody2d.colliders[0].contactEvents = false;", "rigidbody2d.colliders[0].receiver = 12;"}){
        EXPECT_NE(code.find(expected), std::string::npos) << expected;
    }
    EXPECT_EQ(code.find("numColliders"), std::string::npos);
    EXPECT_EQ(code.find("MAX_COLLIDER"), std::string::npos);
}

TEST(Physics2DColliders, OldFilesReadAndContactEventsDefault) {
    // a file of before: vertices, numColliders, no contactEvents key, maxValues ignored
    const Rigidbody2DComponent old = Stream::decodeRigidbody2DComponent(YAML::Load(
        "type: dynamic\nnumColliders: 1\ncolliders:\n"
        "  - type: polygon\n    vertices: [[0, 0], [10, 0], [10, 10], [0, 10]]\n"
        "  - type: circle\n    radius: 5\n"));
    ASSERT_EQ(old.colliders.size(), 1u) << "numColliders counts";
    EXPECT_EQ(old.colliders[0].points.size(), 4u);
    EXPECT_FALSE(old.colliders[0].contactEvents) << "missing key: as before";
    EXPECT_TRUE(old.useAutoMass);
    EXPECT_FLOAT_EQ(old.gravityScale, 1.0f);
    EXPECT_EQ(old.sleepingMode, RigidbodySleepMode2D::START_AWAKE);

    // colliders made from now on report contacts (Unity)
    World2D world;
    const Entity e = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    world.rb(e).createCircleCollider(Vector2(0, 0), 5);
    world.rb(e).createPolygonCollider({Vector2(0, 0), Vector2(10, 0), Vector2(5, 8)});
    EXPECT_TRUE(world.rb(e).isColliderContactEvents(0));
    EXPECT_TRUE(world.rb(e).isColliderContactEvents(1));
    EXPECT_TRUE(Collider2D().contactEvents);
}
