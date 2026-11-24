// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The 2D world settings (docs/specs/S96-physics-gaps.md REQ-30..REQ-34, AC-27): points per metre as
// a setting that rebuilds the bodies where they are, the rotation cap per step (a Box2D patch) and
// the bodies free of it, the solver tuning. A scene with only its PhysicsSystem running, stepped as
// the engine's fixed phase does.

#include "Engine.h"
#include "Scene.h"
#include "component/Joint2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "object/physics/Rigidbody2D.h"
#include "physics/WorldSettings.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"
#include "../ecs/EcsTestUtil.h"

#include "box2d/box2d.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    // points a metre (the default)
    constexpr float M = 64.0f;

    class Physics2DWorldSettingsTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;
        float step = 1.0f / 60.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            makeScene();
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        void makeScene(float dt = 1.0f / 60.0f){
            step = dt;
            Engine::setUpdateTime(dt);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setInterpolation(false);
            physics->setGravity2D(0.0f, -9.81f);
        }

        // a body at a position (points) with one collider
        Entity body(Vector2 position, BodyType type, const Collider2D& collider){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(position.x, position.y, 0.0f);
            transform.needUpdate = true;
            Rigidbody2DComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders.push_back(collider);
            scene->addComponent<Rigidbody2DComponent>(entity, rigidbody);
            return entity;
        }

        Entity box(Vector2 position, Vector2 size, BodyType type, float density = 1.0f){
            Collider2D collider;
            collider.type = Collider2DType::POLYGON;
            collider.pointA = Vector2(-size.x * 0.5f, -size.y * 0.5f);
            collider.pointB = Vector2(size.x * 0.5f, size.y * 0.5f);
            collider.density = density;
            return body(position, type, collider);
        }

        Entity ball(Vector2 position, float radius, BodyType type, float restitution = 0.0f){
            Collider2D collider;
            collider.type = Collider2DType::CIRCLE;
            collider.radius = radius;
            collider.restitution = restitution;
            return body(position, type, collider);
        }

        Rigidbody2DComponent& component(Entity entity){
            return scene->getComponent<Rigidbody2DComponent>(entity);
        }

        Rigidbody2D rb(Entity entity){
            return Rigidbody2D(scene.get(), entity);
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
                scene->fixedUpdate(step);
                resolveTransforms();
            }
        }

        Vector2 worldPosition(Entity entity){
            const Vector3 p = scene->getComponent<Transform>(entity).worldPosition;
            return Vector2(p.x, p.y);
        }

        // a pyramid of boxes on a ground: what the bit-identical runs step
        std::vector<Entity> stack(){
            std::vector<Entity> boxes;
            box(Vector2(0.0f, -0.5f * M), Vector2(40.0f * M, 1.0f * M), BodyType::STATIC);
            for (int row = 0; row < 6; row++){
                for (int column = 0; column < 6 - row; column++){
                    const float x = (column - (5 - row) * 0.5f) * 1.05f * M;
                    boxes.push_back(box(Vector2(x, (0.5f + row * 1.02f) * M), Vector2(M, M), BodyType::DYNAMIC));
                }
            }
            return boxes;
        }

        // every dynamic body's Box2D pose and velocity, as bytes
        std::vector<float> state(const std::vector<Entity>& entities){
            std::vector<float> values;
            for (Entity entity : entities){
                const b2BodyId id = component(entity).body;
                const b2Transform xf = b2Body_GetTransform(id);
                const b2Vec2 v = b2Body_GetLinearVelocity(id);
                values.insert(values.end(), {xf.p.x, xf.p.y, xf.q.c, xf.q.s, v.x, v.y, b2Body_GetAngularVelocity(id)});
            }
            return values;
        }
    };

    // a raw Box2D pyramid, the same in every run, stepped 600 times at 60 Hz; the poses at the end
    std::vector<float> rawBox2DStack(const b2WorldDef& def, bool applyLiveDefaults){
        const b2WorldId world = b2CreateWorld(&def);
        if (applyLiveDefaults){
            applyPhysicsWorldSettings(ScenePhysics2DSettings(), world);
        }
        b2BodyDef groundDef = b2DefaultBodyDef();
        const b2BodyId ground = b2CreateBody(world, &groundDef);
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        const b2Polygon groundBox = b2MakeOffsetBox(20.0f, 0.5f, {0.0f, -0.5f}, b2Rot_identity);
        b2CreatePolygonShape(ground, &shapeDef, &groundBox);
        std::vector<b2BodyId> bodies;
        const b2Polygon unit = b2MakeBox(0.5f, 0.5f);
        for (int row = 0; row < 8; row++){
            for (int column = 0; column < 8 - row; column++){
                b2BodyDef bodyDef = b2DefaultBodyDef();
                bodyDef.type = b2_dynamicBody;
                bodyDef.position = {(column - (7 - row) * 0.5f) * 1.05f, 0.5f + row * 1.02f};
                bodyDef.rotation = b2MakeRot(0.01f * (float)column);
                const b2BodyId id = b2CreateBody(world, &bodyDef);
                b2CreatePolygonShape(id, &shapeDef, &unit);
                bodies.push_back(id);
            }
        }
        for (int i = 0; i < 600; i++){
            b2World_Step(world, 1.0f / 60.0f, 4);
        }
        std::vector<float> values;
        for (b2BodyId id : bodies){
            const b2Transform xf = b2Body_GetTransform(id);
            values.insert(values.end(), {xf.p.x, xf.p.y, xf.q.c, xf.q.s});
        }
        b2DestroyWorld(world);
        return values;
    }

    bool sameBits(const std::vector<float>& a, const std::vector<float>& b){
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }

}

// With every setting at its default the solver computes exactly what it did before the settings
// existed (D11): a world made from Box2D's own defaults, one made through the settings and one given
// them live step the same bits; and an engine scene whose settings went away and back steps as one
// whose settings never changed. A guard: there is no red run.
TEST_F(Physics2DWorldSettingsTest, DefaultsAreBitIdentical){
    EXPECT_FLOAT_EQ(b2DefaultWorldDef().maximumRotation, 0.25f * 3.14159265359f);

    const std::vector<float> plain = rawBox2DStack(b2DefaultWorldDef(), false);
    b2WorldDef throughSettings = b2DefaultWorldDef();
    applyPhysicsWorldSettings(ScenePhysics2DSettings(), throughSettings);
    EXPECT_TRUE(sameBits(plain, rawBox2DStack(throughSettings, false)));
    EXPECT_TRUE(sameBits(plain, rawBox2DStack(b2DefaultWorldDef(), true)));

    const std::vector<Entity> untouched = stack();
    steps(600);
    const std::vector<float> reference = state(untouched);

    makeScene();
    const std::vector<Entity> roundTrip = stack();
    steps(1);
    ScenePhysics2DSettings changed;
    changed.maxRotation = Angle::degToDefault(360.0f);
    changed.contactHertz = 10.0f;
    changed.contactDampingRatio = 1.0f;
    changed.contactPushSpeed = 1.0f;
    changed.jointHertz = 20.0f;
    changed.jointDampingRatio = 1.0f;
    changed.bounceThreshold = 3.0f;
    changed.maxLinearSpeed = 50.0f;
    changed.warmStarting = false;
    changed.continuous = false;
    physics->setWorldSettings2D(changed);
    physics->setWorldSettings2D(ScenePhysics2DSettings());
    steps(599);
    EXPECT_TRUE(sameBits(reference, state(roundTrip)));
}

// REQ-30, REQ-31: a new scale with bodies made rebuilds them where they are, moving on at the same
// speed in points, with masses in the new metres (an explicit one kept); joints are made again
// before the next step. Red: the scale only stored, bodies stay in old metres and jump
TEST_F(Physics2DWorldSettingsTest, PointsPerMeterRebuilds){
    physics->setGravity2D(0.0f, 0.0f);
    const Entity fixedMass = box(Vector2(3.0f * M, 2.0f * M), Vector2(M, M), BodyType::DYNAMIC);
    component(fixedMass).useAutoMass = false;
    component(fixedMass).mass = 1.0f;
    const Entity dense = box(Vector2(-3.0f * M, 2.0f * M), Vector2(M, M), BodyType::DYNAMIC);
    const Entity anchor = box(Vector2(0.0f, 4.0f * M), Vector2(0.5f * M, 0.5f * M), BodyType::STATIC);
    const Entity hinge = test::makeTransformEntity(*scene);
    Joint2DComponent joint;
    joint.type = Joint2DType::REVOLUTE;
    joint.bodyA = dense;
    joint.bodyB = anchor;
    joint.anchorA = Vector2(0.0f, 0.5f * M);
    scene->addComponent<Joint2DComponent>(hinge, joint);
    steps(2);
    ASSERT_TRUE(b2Joint_IsValid(scene->getComponent<Joint2DComponent>(hinge).joint));

    rb(fixedMass).setLinearVelocity(Vector2(2.0f * M, -M));
    steps(1);
    EXPECT_NEAR(rb(dense).getMass(), 1.0f, 1e-4f) << "1 m^2 at density 1";
    const Vector2 before = rb(fixedMass).getPosition();
    const Vector2 velocity = rb(fixedMass).getLinearVelocity();
    const Vector2 densePosition = rb(dense).getPosition();

    scene->setPhysics2DPointsPerMeter(1.0f);
    EXPECT_FLOAT_EQ(physics->getPointsToMeterScale2D(), 1.0f);
    EXPECT_FLOAT_EQ(scene->getPhysics2DPointsPerMeter(), 1.0f);
    // where it was and as fast, in points
    EXPECT_NEAR(rb(fixedMass).getPosition().x, before.x, 1e-3f);
    EXPECT_NEAR(rb(fixedMass).getPosition().y, before.y, 1e-3f);
    EXPECT_NEAR(rb(fixedMass).getLinearVelocity().x, velocity.x, 1e-3f);
    EXPECT_NEAR(rb(fixedMass).getLinearVelocity().y, velocity.y, 1e-3f);
    EXPECT_NEAR(rb(dense).getPosition().x, densePosition.x, 1e-3f);
    // the 1 kg box weighs 1 kg; the other is now 64 x 64 m at density 1
    EXPECT_NEAR(rb(fixedMass).getMass(), 1.0f, 1e-5f);
    EXPECT_NEAR(rb(dense).getMass(), M * M, 1e-1f);

    steps(1);
    EXPECT_NEAR(worldPosition(fixedMass).x, before.x + velocity.x * step, 1e-2f);
    EXPECT_NEAR(worldPosition(fixedMass).y, before.y + velocity.y * step, 1e-2f);
    EXPECT_TRUE(b2Joint_IsValid(scene->getComponent<Joint2DComponent>(hinge).joint)) << "the joint is made again";

    // and back to the default units: the same again
    const Vector2 at = rb(fixedMass).getPosition();
    physics->setPointsToMeterScale2D(M);
    EXPECT_FLOAT_EQ(scene->getPhysics2DPointsPerMeter(), M);
    EXPECT_NEAR(rb(fixedMass).getPosition().x, at.x, 1e-3f);
    EXPECT_NEAR(rb(dense).getMass(), 1.0f, 1e-4f);
}

// REQ-32, REQ-33: Box2D turns a body at most 0.25 pi a step (39.3 rad/s at 50 Hz); the scene's
// Max Rotation Per Step raises it, a body with allowFastRotation is free of it, live. Red: the
// constant (no setting, no setter) caps all three
TEST_F(Physics2DWorldSettingsTest, MaxRotation){
    const float capAt50Hz = 0.25f * 3.14159265359f * 50.0f;
    auto spin = [&](float maxRotationDegrees, bool fastBody, bool fastLive){
        makeScene(1.0f / 50.0f);
        physics->setGravity2D(0.0f, 0.0f);
        if (maxRotationDegrees > 0.0f){
            scene->setPhysics2DMaxRotation(Angle::degToDefault(maxRotationDegrees));
        }
        const Entity wheel = ball(Vector2(0.0f, 0.0f), 0.5f * M, BodyType::DYNAMIC);
        component(wheel).allowFastRotation = fastBody;
        steps(1);
        if (fastLive){
            rb(wheel).setAllowFastRotation(true);
        }
        for (int i = 0; i < 100; i++){
            rb(wheel).applyTorque(200.0f, true);
            steps(1);
        }
        return b2Body_GetAngularVelocity(component(wheel).body);
    };

    const float capped = spin(0.0f, false, false);
    EXPECT_NEAR(capped, capAt50Hz, 0.05f) << "Box2D's 0.25 pi a step";
    EXPECT_GT(spin(360.0f, false, false), capAt50Hz + 1.0f) << "Max Rotation Per Step 360";
    EXPECT_GT(spin(0.0f, true, false), capAt50Hz + 1.0f) << "allowFastRotation from the body";
    EXPECT_GT(spin(0.0f, false, true), capAt50Hz + 1.0f) << "allowFastRotation set live";
    std::printf("[MaxRotation] default cap %.2f rad/s, 360 deg %.2f rad/s\n", capped, spin(360.0f, false, false));
}

// REQ-34: the tuning reaches the world when it is made and when it changes; a bounce threshold of
// 5 m/s stops a ball bouncing earlier than 1. Red: the world made from Box2D's defaults only
TEST_F(Physics2DWorldSettingsTest, WorldTuning){
    auto bounces = [&](float threshold){
        makeScene();
        scene->setPhysics2DBounceThreshold(threshold);
        box(Vector2(0.0f, -0.5f * M), Vector2(40.0f * M, 1.0f * M), BodyType::STATIC);
        const Entity dropped = ball(Vector2(0.0f, 5.0f * M), 0.25f * M, BodyType::DYNAMIC, 0.8f);
        int count = 0;
        float previous = 0.0f;
        for (int i = 0; i < 600; i++){
            steps(1);
            const float vy = b2Body_GetLinearVelocity(component(dropped).body).y;
            if (previous < -0.01f && vy > 0.01f){
                count++;
            }
            previous = vy;
        }
        EXPECT_FLOAT_EQ(b2World_GetRestitutionThreshold(physics->getWorld2D()), threshold);
        return count;
    };
    const int atOne = bounces(1.0f);
    const int atFive = bounces(5.0f);
    std::printf("[WorldTuning] bounces: threshold 1 m/s %d, 5 m/s %d\n", atOne, atFive);
    EXPECT_GT(atOne, atFive);
    EXPECT_GT(atFive, 0);

    // made with them, then changed live (applied before the next step)
    makeScene();
    ScenePhysics2DSettings settings;
    settings.maxLinearSpeed = 50.0f;
    settings.warmStarting = false;
    settings.sleep = false;
    settings.continuous = false;
    settings.maxRotation = Angle::degToDefault(90.0f);
    scene->setPhysics2DWorldSettings(settings);
    box(Vector2(0.0f, 0.0f), Vector2(M, M), BodyType::DYNAMIC);
    steps(1);
    const b2WorldId world = physics->getWorld2D();
    ASSERT_TRUE(b2World_IsValid(world));
    EXPECT_FLOAT_EQ(b2World_GetMaximumLinearSpeed(world), 50.0f);
    EXPECT_FALSE(b2World_IsWarmStartingEnabled(world));
    EXPECT_FALSE(b2World_IsSleepingEnabled(world));
    EXPECT_FALSE(b2World_IsContinuousEnabled(world));
    EXPECT_NEAR(b2World_GetMaximumRotation(world), 0.5f * 3.14159265f, 1e-5f);

    scene->setPhysics2DMaxLinearSpeed(80.0f);
    scene->setPhysics2DSleep(true);
    scene->setPhysics2DMaxRotation(ScenePhysics2DSettings::defaultMaxRotation());
    steps(1);
    EXPECT_FLOAT_EQ(b2World_GetMaximumLinearSpeed(world), 80.0f);
    EXPECT_TRUE(b2World_IsSleepingEnabled(world));
    EXPECT_EQ(b2World_GetMaximumRotation(world), b2DefaultWorldDef().maximumRotation) << "Box2D's own float at the default";

    // out of range values are clamped, a non-finite one is the default
    scene->setPhysics2DPointsPerMeter(-5.0f);
    EXPECT_GT(scene->getPhysics2DPointsPerMeter(), 0.0f);
    scene->setPhysics2DContactHertz(NAN);
    EXPECT_FLOAT_EQ(scene->getPhysics2DContactHertz(), 30.0f);
    scene->setPhysics3DCollisionSteps(9);
    EXPECT_EQ(scene->getPhysics3DCollisionSteps(), 4);
}
