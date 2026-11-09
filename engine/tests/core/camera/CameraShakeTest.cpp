// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Camera shake (spec 14, tests 1-10 and section 13): the noise, the MilkShake timeline and
// formulas, scales and shakeAt, trauma, the view offset (the Transform untouched), 2D, scaled and
// unscaled time, lifetime, transform mode, the .shake format and pool, the Lua API, the budget.

#include <gtest/gtest.h>

#include "CameraTestSupport.h"

#include "Engine.h"
#include "Scene.h"
#include "camera/CameraShake.h"
#include "camera/ShakeInstance.h"
#include "camera/ShakePreset.h"
#include "camera/ShakePresets.h"
#include "camera/ShakerRuntime.h"
#include "component/CameraComponent.h"
#include "component/ImageComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "math/Noise.h"
#include "object/CameraShaker.h"
#include "component/CameraFollowComponent.h"
#include "pool/ShakePresetPool.h"
#include "script/LuaBinding.h"
#include "subsystem/CameraControlSystem.h"
#include "subsystem/RenderSystem.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::camera;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    ShakePreset preset(ShakeType type, float strength, float roughness, float fadeIn, float fadeOut,
                       Vector3 position = Vector3(1, 1, 1), Vector3 rotation = Vector3(1, 1, 1)){
        ShakePreset p;
        p.type = type;
        p.strength = strength;
        p.roughness = roughness;
        p.fadeIn = fadeIn;
        p.fadeOut = fadeOut;
        p.positionInfluence = position;
        p.rotationInfluence = rotation;
        return p;
    }

    float N(float x, float y){
        return (perlinNoise(x, y) - 0.5f) * 2.0f;
    }

    void expectMatrixNear(const Matrix4& a, const Matrix4& b, float tolerance){
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                EXPECT_NEAR(a[c][r], b[c][r], tolerance) << "column " << c << " row " << r;
            }
        }
    }

    class Shake : public ::testing::Test {
    protected:
        void SetUp() override {
            cameratest::takeWarnings();
            Engine::setScene(&scene);
        }
        void TearDown() override {
            Engine::removeScene(&scene);
        }

        Entity camera(Vector3 position = Vector3(0, 0, 10), CameraType type = CameraType::CAMERA_PERSPECTIVE){
            Entity entity = scene.createEntity();
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            CameraComponent cameraComponent;
            cameraComponent.type = type;
            cameraComponent.useTarget = false;
            scene.addComponent<CameraComponent>(entity, cameraComponent);
            scene.addComponent<CameraShakerComponent>(entity, {});
            return entity;
        }

        CameraControlSystem& system(){ return *scene.getSystem<CameraControlSystem>(); }
        CameraShakerComponent& shaker(Entity entity){ return scene.getComponent<CameraShakerComponent>(entity); }
        CameraComponent& cameraOf(Entity entity){ return scene.getComponent<CameraComponent>(entity); }

        void step(float dt = DT, int count = 1, float unscaled = -1.0f){
            for (int i = 0; i < count; i++){
                system().step(dt, unscaled < 0.0f ? dt : unscaled);
            }
        }

        // what RenderSystem's camera loop does with the shaken camera
        void render(Entity entity){
            RenderSystem& render = *scene.getSystem<RenderSystem>();
            Transform& transform = scene.getComponent<Transform>(entity);
            render.updateTransform(transform, entity);
            render.updateCamera(cameraOf(entity), transform);
        }

        Scene scene;
    };

}

// ---- 1. noise

TEST(ShakeNoise, RangeLatticeAndContinuity){
    float lo = 1.0f, hi = 0.0f;
    for (int i = 0; i < 100000; i++){
        const float x = (float)(i % 317) * 0.731f - 100.0f;
        const float y = (float)(i / 317) * 0.377f - 50.0f;
        const float v = perlinNoise(x, y);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        ASSERT_GE(v, 0.0f);
        ASSERT_LE(v, 1.0f);
    }
    // it spans most of the range, as Unity's Mathf.PerlinNoise
    EXPECT_LT(lo, 0.2f);
    EXPECT_GT(hi, 0.8f);
    for (int x = -20; x <= 20; x++){
        for (int y = -20; y <= 20; y++){
            EXPECT_FLOAT_EQ(perlinNoise((float)x, (float)y), 0.5f);
        }
    }
    float maxStep = 0.0f;
    for (int i = 0; i < 20000; i++){
        const float x = -50.0f + i * 0.01f;
        const float y = 3.3f + i * 0.0037f;
        maxStep = std::max(maxStep, std::fabs(perlinNoise(x + 0.01f, y) - perlinNoise(x, y)));
        maxStep = std::max(maxStep, std::fabs(perlinNoise(x, y + 0.01f) - perlinNoise(x, y)));
    }
    EXPECT_LT(maxStep, 0.05f);
}

// ---- 2. the fade timeline

TEST(ShakeTimeline, OneShotFadesInThenOutAndEnds){
    ShakeInstance instance(preset(ShakeType::OneShot, 1, 1, 0.05f, 1.0f), 3);
    int fullStep = -1, endStep = -1;
    float previousFade = 0.0f;
    bool decreased = false;
    for (int i = 1; i <= 200 && endStep < 0; i++){
        instance.step(DT);
        if (fullStep < 0 && instance.getFade() >= 1.0f) fullStep = i;
        if (fullStep > 0 && i > fullStep && instance.getFade() < previousFade) decreased = true;
        previousFade = instance.getFade();
        if (instance.isFinished()) endStep = i;
    }
    EXPECT_NEAR(fullStep * DT, 0.05f, DT + 1e-4f);
    EXPECT_TRUE(decreased);
    EXPECT_NEAR(endStep * DT, 1.05f, DT + 1e-4f);
}

TEST(ShakeTimeline, SustainedHoldsUntilStop){
    ShakeInstance instance(preset(ShakeType::Sustained, 1, 1, 0.1f, 2.0f), 3);
    for (int i = 0; i < 300; i++) instance.step(DT);
    EXPECT_EQ(instance.getState(), ShakeState::Sustained);
    EXPECT_FLOAT_EQ(instance.getFade(), 1.0f);
    instance.stop(0.5f);
    int steps = 0;
    while (!instance.isFinished() && steps < 1000){
        instance.step(DT);
        steps++;
    }
    EXPECT_NEAR(steps * DT, 0.5f, DT + 1e-4f);
}

TEST(ShakeTimeline, ZeroFadeInIsInstantAndZeroStepsChangeNothing){
    ShakeInstance instance(preset(ShakeType::Sustained, 1, 1, 0.0f, 0.0f), 3);
    instance.step(0.0f);
    EXPECT_EQ(instance.getState(), ShakeState::FadingIn);
    EXPECT_FLOAT_EQ(instance.getFade(), 0.0f);
    instance.step(DT);
    EXPECT_FLOAT_EQ(instance.getFade(), 1.0f);
    EXPECT_EQ(instance.getState(), ShakeState::Sustained);
    // a stop taken during a zero step is kept (MilkShake turned the shake Sustained again)
    instance.stop(1.0f);
    instance.step(0.0f);
    EXPECT_EQ(instance.getState(), ShakeState::FadingOut);
}

TEST(ShakeTimeline, UpdateReturnsTheOffsetBeforeTheStep){
    // MilkShake's UpdateShake: the first frame of a shake is still (fade 0)
    ShakeInstance instance(preset(ShakeType::Sustained, 1, 10, 0.0f, 0.0f), 42);
    ShakeOffset first = instance.update(DT);
    EXPECT_EQ(first.position, Vector3(0, 0, 0));
    EXPECT_EQ(first.rotation, Vector3(0, 0, 0));
    ShakeOffset expected = instance.getOffset();
    ShakeOffset second = instance.update(DT);
    EXPECT_EQ(second.position, expected.position);
    EXPECT_EQ(second.rotation, expected.rotation);
}

// ---- 3. MilkShake's formulas

TEST(ShakeFormula, MatchesMilkShake){
    const ShakePreset p = preset(ShakeType::Sustained, 0.7f, 30.0f, 0.0f, 0.0f, Vector3(1, 2, 3), Vector3(4, 5, 6));
    const int b = 100;
    const float t = 100.5f;
    const float strength = 0.7f;
    ShakeOffset offset = evaluateShake(p, b, t, strength);
    const float s1 = b / 2.0f, s2 = b / 3.0f, s3 = b / 4.0f;
    EXPECT_NEAR(offset.position.x, (perlinNoise(t + s1, (float)b) - 0.5f) * 2.0f * strength * 1.0f, 1e-6f);
    EXPECT_NEAR(offset.position.y, N((float)b, t) * strength * 2.0f, 1e-6f);
    EXPECT_NEAR(offset.position.z, N(s3 + t, b + t) * strength * 3.0f, 1e-6f);
    EXPECT_NEAR(offset.rotation.x, N(t - b, s3) * strength * 4.0f, 1e-6f);
    EXPECT_NEAR(offset.rotation.y, N((float)b, t + s2) * strength * 5.0f, 1e-6f);
    EXPECT_NEAR(offset.rotation.z, N(b + t, s1 + t) * strength * 6.0f, 1e-6f);

    // an instance of seed 100 at fade 1: its clock starts at b and moves by dt * roughness * fade
    ShakeInstance instance(p, b);
    EXPECT_FLOAT_EQ(instance.getNoiseTime(), 100.0f);
    instance.step(DT);                      // fade 0 -> 1: the clock moved with fade 0
    EXPECT_FLOAT_EQ(instance.getNoiseTime(), 100.0f);
    instance.step(DT);                      // 1/60 * 30 at fade 1
    EXPECT_NEAR(instance.getNoiseTime(), 100.5f, 1e-4f);
    ShakeOffset live = instance.getOffset();
    ShakeOffset reference = evaluateShake(p, b, instance.getNoiseTime(), 0.7f);
    EXPECT_NEAR(live.position.x, reference.position.x, 1e-6f);
    EXPECT_NEAR(live.rotation.z, reference.rotation.z, 1e-6f);
}

TEST(ShakeFormula, EulerOrderIsZThenXThenY){
    const Quaternion q = shakeEulerToQuaternion(Vector3(30, 40, 50));
    // Z first: rotate (1, 0, 0) by Z, then X, then Y
    auto axis = [](float degrees, const Vector3& a){
        const float h = degrees * 3.14159265f / 360.0f;
        return Quaternion(std::cos(h), a.x * std::sin(h), a.y * std::sin(h), a.z * std::sin(h));
    };
    const Vector3 v(1, 0.5f, -0.25f);
    const Vector3 expected = axis(40, Vector3(0, 1, 0)) * (axis(30, Vector3(1, 0, 0)) * (axis(50, Vector3(0, 0, 1)) * v));
    const Vector3 got = q * v;
    EXPECT_NEAR(got.x, expected.x, 1e-5f);
    EXPECT_NEAR(got.y, expected.y, 1e-5f);
    EXPECT_NEAR(got.z, expected.z, 1e-5f);
}

// ---- 4. scales and shakeAt

TEST(ShakeScales, StrengthAndRoughnessScales){
    const ShakePreset p = preset(ShakeType::Sustained, 1, 10, 0, 0);
    ShakeInstance a(p, 9), b(p, 9);
    b.strengthScale = 0.5f;
    b.roughnessScale = 2.0f;
    a.step(DT);
    b.step(DT);
    a.step(DT);
    b.step(DT);
    EXPECT_NEAR(b.getNoiseTime() - 9.0f, 2.0f * (a.getNoiseTime() - 9.0f), 1e-4f);
    ShakeInstance c(p, 9);
    c.strengthScale = 0.5f;
    c.step(DT);
    c.step(DT);
    EXPECT_NEAR(c.getOffset().position.x, 0.5f * a.getOffset().position.x, 1e-6f);
    EXPECT_NEAR(c.getOffset().rotation.y, 0.5f * a.getOffset().rotation.y, 1e-6f);
}

TEST_F(Shake, ShakeAtFallsOffWithDistance){
    Entity cam = camera(Vector3(0, 0, 0));
    EXPECT_NEAR(system().distanceScale(cam, shaker(cam), Vector3(5, 0, 0), 10.0f), 0.5f, 1e-6f);
    ShakeInstanceHandle half = CameraShake::shakeAt(Vector3(5, 0, 0), 10.0f, ShakeSource("explosion"));
    ASSERT_TRUE(half.isValid());
    EXPECT_NEAR(half.getStrengthScale(), 0.5f, 1e-6f);
    EXPECT_NEAR(half.getRoughnessScale(), 0.5f, 1e-6f);
    ShakeInstanceHandle none = CameraShake::shakeAt(Vector3(10, 0, 0), 10.0f, ShakeSource("explosion"));
    EXPECT_FALSE(none.isValid());
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 1u);
}

// ---- 5. trauma

TEST(ShakeTrauma, ClampsDecaysAndUsesThePower){
    TraumaState trauma;
    trauma.add(0.5f);
    trauma.add(0.5f);
    trauma.add(0.5f);
    EXPECT_FLOAT_EQ(trauma.trauma, 1.0f);
    for (int i = 0; i < 30; i++) trauma.step(DT, 1.0f, 15.0f);
    EXPECT_NEAR(trauma.trauma, 0.5f, 1e-4f);
    trauma.trauma = 0.5f;
    const ShakeOffset atPower2 = trauma.getOffset(2.0f, Vector3(1, 1, 1), Vector3(1, 1, 1));
    const ShakeOffset atPower1 = trauma.getOffset(1.0f, Vector3(1, 1, 1), Vector3(1, 1, 1));
    // the same noise, amplitude 0.25 vs 0.5
    EXPECT_NEAR(atPower2.position.x, 0.5f * atPower1.position.x, 1e-6f);
    EXPECT_NEAR(atPower2.rotation.z, 0.5f * atPower1.rotation.z, 1e-6f);
    const float tau = trauma.clock, sT = trauma.seed;
    EXPECT_NEAR(atPower2.position.x, N(tau + 11, sT) * 0.25f, 1e-6f);
    EXPECT_NEAR(atPower2.rotation.y, N(sT + 7, tau + 53) * 0.25f, 1e-6f);
}

TEST_F(Shake, TraumaShakesTheCamera){
    Entity cam = camera();
    CameraShake::addTrauma(0.8f);
    bool shaken = false;
    for (int i = 0; i < 4; i++){
        step();
        shaken = shaken || cameraOf(cam).hasViewOffset;
    }
    EXPECT_TRUE(shaken);
    step(DT, 120);
    EXPECT_FLOAT_EQ(shaker(cam).runtime->trauma.trauma, 0.0f);
    step();
    EXPECT_FALSE(cameraOf(cam).hasViewOffset);
}

// ---- 6. the view offset leaves the Transform alone

TEST_F(Shake, ViewOffsetMovesTheViewNotTheTransform){
    for (bool useTarget : {false, true}){
        Entity cam = camera(Vector3(0, 0, 10));
        Entity reference = camera(Vector3(1, 0, 10));
        cameraOf(cam).useTarget = useTarget;
        cameraOf(reference).useTarget = useTarget;
        cameraOf(cam).target = Vector3(0, 0, 0);
        cameraOf(reference).target = Vector3(1, 0, 0);
        // the offset is forced: one unit along the camera's x
        cameraOf(cam).hasViewOffset = true;
        cameraOf(cam).viewOffsetPosition = Vector3(1, 0, 0);
        cameraOf(cam).viewOffsetRotation = Quaternion();
        render(cam);
        render(reference);
        expectMatrixNear(cameraOf(cam).viewMatrix, cameraOf(reference).viewMatrix, 1e-5f);
        EXPECT_EQ(scene.getComponent<Transform>(cam).position, Vector3(0, 0, 10));
        // a script moves the camera: the shake rides on the new pose the same frame
        scene.getComponent<Transform>(cam).position = Vector3(5, 0, 10);
        cameraOf(cam).target = Vector3(5, 0, 0);
        scene.getComponent<Transform>(reference).position = Vector3(6, 0, 10);
        cameraOf(reference).target = Vector3(6, 0, 0);
        render(cam);
        render(reference);
        expectMatrixNear(cameraOf(cam).viewMatrix, cameraOf(reference).viewMatrix, 1e-5f);
        scene.destroyEntity(cam);
        scene.destroyEntity(reference);
    }
}

TEST_F(Shake, ShakerSetsTheViewOffsetAndNeverTheTransform){
    Entity cam = camera(Vector3(0, 0, 10));
    Transform& transform = scene.getComponent<Transform>(cam);
    const Quaternion rotation = transform.rotation;
    ShakeOptions options;
    options.hasSeed = true;
    options.seed = 77;
    CameraShaker(&scene, cam).shake(ShakeSource("explosion"), options);
    step(DT, 5);
    EXPECT_TRUE(cameraOf(cam).hasViewOffset);
    EXPECT_EQ(cameraOf(cam).viewOffsetPosition, shaker(cam).runtime->offsetPosition);
    EXPECT_NE(shaker(cam).runtime->offsetPosition, Vector3(0, 0, 0));
    EXPECT_EQ(transform.position, Vector3(0, 0, 10));
    EXPECT_EQ(transform.rotation, rotation);
    render(cam);
    // the eye moved by the offset, the camera's own direction did not
    const Matrix4 inverse = cameraOf(cam).viewMatrix.inverse();
    const Vector3 eye = inverse * Vector3(0, 0, 0);
    EXPECT_GT((eye - Vector3(0, 0, 10)).length(), 1e-5f);
    // explosion: position influence z = 0, so the eye stays in the z = 10 plane of the camera
    EXPECT_NEAR(eye.z, 10.0f, 1e-4f);
}

TEST_F(Shake, ZeroStrengthLeavesNoOffset){
    Entity cam = camera();
    ShakePreset p = preset(ShakeType::Sustained, 0.0f, 10.0f, 0.0f, 0.0f);
    CameraShaker(&scene, cam).shake(ShakeSource(p));
    step(DT, 10);
    EXPECT_FALSE(cameraOf(cam).hasViewOffset);
    ShakeOptions zero;
    zero.strength = 0.0f;
    CameraShaker(&scene, cam).shake(ShakeSource("explosion"), zero);
    step(DT, 10);
    EXPECT_FALSE(cameraOf(cam).hasViewOffset);
}

// ---- 7. 2D

TEST_F(Shake, OrthographicCameraShakesXYAndRollInTenthsOfTheViewHeight){
    Entity cam = camera(Vector3(0, 0, 0), CameraType::CAMERA_ORTHO);
    cameraOf(cam).bottomClip = 0.0f;
    cameraOf(cam).topClip = 20.0f;
    cameraOf(cam).leftClip = 0.0f;
    cameraOf(cam).rightClip = 30.0f;
    CameraShaker(&scene, cam).shake(ShakeSource(preset(ShakeType::Sustained, 1.0f, 20.0f, 0.0f, 0.0f)));
    float maxX = 0.0f;
    for (int i = 0; i < 600; i++){
        step();
        const ShakerRuntime& runtime = *shaker(cam).runtime;
        maxX = std::max(maxX, std::fabs(runtime.offsetPosition.x));
        ASSERT_EQ(runtime.offsetPosition.z, 0.0f);
        ASSERT_EQ(runtime.offsetRotation.x, 0.0f);
        ASSERT_EQ(runtime.offsetRotation.y, 0.0f);
    }
    EXPECT_LE(maxX, 2.0f);
    EXPECT_GT(maxX, 0.2f);
    // it rolls about the view's centre
    EXPECT_EQ(cameraOf(cam).viewOffsetPivot, Vector3(15, 10, 0));
}

// ---- 8. scaled and unscaled time

TEST_F(Shake, TimeScaleZeroFreezesScaledShakesOnly){
    Entity cam = camera();
    CameraShaker shakerObject(&scene, cam);
    ShakeOptions scaledOptions;
    scaledOptions.hasSeed = true;
    scaledOptions.seed = 5;
    ShakeInstanceHandle scaled = shakerObject.shake(ShakeSource("drunk"), scaledOptions);
    step(DT, 3);
    // time scale 0: the scene delta is 0, the unscaled one goes on. The first frozen frame shows
    // the state the last step reached (MilkShake reports the state before each step), then it holds
    step(0.0f, 1, DT);
    const Vector3 frozen = shaker(cam).runtime->offsetRotation;
    step(0.0f, 6, DT);
    EXPECT_EQ(shaker(cam).runtime->offsetRotation, frozen);
    scaled.stop(0.0f);
    step(DT, 2);
    EXPECT_FALSE(scaled.isValid());

    ShakeOptions unscaledOptions;
    unscaledOptions.unscaled = 1;
    unscaledOptions.hasSeed = true;
    unscaledOptions.seed = 5;
    shakerObject.shake(ShakeSource("drunk"), unscaledOptions);
    step(DT, 3);
    const Vector3 before = shaker(cam).runtime->offsetRotation;
    step(0.0f, 6, DT);
    EXPECT_NE(shaker(cam).runtime->offsetRotation, before);
}

// ---- 9. lifetime

TEST_F(Shake, DestroyedShakerInvalidatesItsHandles){
    Entity cam = camera();
    ShakeInstanceHandle handle = CameraShake::shake(ShakeSource("earthquake"));
    ASSERT_TRUE(handle.isValid());
    EXPECT_EQ(handle.getCount(), 1);
    scene.destroyEntity(cam);
    EXPECT_FALSE(handle.isValid());
    EXPECT_TRUE(handle.isFinished());
    EXPECT_EQ(handle.getState(), "stopped");
    handle.stop();          // a no-op
    handle.setStrengthScale(3.0f);
    EXPECT_FLOAT_EQ(handle.getStrengthScale(), 0.0f);
}

TEST_F(Shake, DisablingClearsTheOffsetAndEnablingReplaysTheStartPreset){
    Entity cam = camera();
    shaker(cam).startPreset = "electrocute";
    bool shaken = false;
    for (int i = 0; i < 5; i++){
        step();
        shaken = shaken || cameraOf(cam).hasViewOffset;
    }
    ASSERT_TRUE(shaker(cam).runtime);
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 1u);
    EXPECT_TRUE(shaken);
    shaker(cam).enabled = false;
    step();
    EXPECT_FALSE(cameraOf(cam).hasViewOffset);
    // the instance holds while disabled
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 1u);
    shaker(cam).enabled = true;
    step();
    // the start shake held while disabled: it plays on, none is added (no pile-up)
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 1u);
    // a deactivated GameObject is the same
    scene.setEntityActive(cam, false);
    step();
    EXPECT_FALSE(cameraOf(cam).hasViewOffset);
    scene.setEntityActive(cam, true);
    shaken = false;
    for (int i = 0; i < 3; i++){
        step();
        shaken = shaken || cameraOf(cam).hasViewOffset;
    }
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 1u);
    EXPECT_TRUE(shaken);
    for (int i = 0; i < 10; i++){
        shaker(cam).enabled = false;
        step();
        shaker(cam).enabled = true;
        step();
    }
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 1u) << "ten disable / enable cycles add nothing";
    // once the start shake is over, the next enable plays it again (Unity's OnEnable)
    CameraShaker(&scene, cam).stopAll(0.0f);
    step(DT, 2);
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 0u);
    shaker(cam).enabled = false;
    step();
    shaker(cam).enabled = true;
    step();
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 1u);
    EXPECT_EQ(shaker(cam).runtime->instances[0].presetName, "electrocute");
}

TEST_F(Shake, GlobalCallsSkipNonGlobalAndDisabledShakers){
    Entity a = camera();
    Entity b = camera();
    Entity c = camera();
    shaker(b).global = false;
    shaker(c).enabled = false;
    ShakeInstanceHandle handle = CameraShake::shake(ShakeSource("explosion"));
    EXPECT_EQ(handle.getCount(), 1);
    EXPECT_EQ(shaker(a).runtime->instances.size(), 1u);
    EXPECT_FALSE(shaker(b).runtime && !shaker(b).runtime->instances.empty());
    handle.pause();
    EXPECT_TRUE(handle.isPaused());
    EXPECT_EQ(handle.getState(), "paused");
    CameraShake::stopAll(0.0f);
    step(DT, 2);
    EXPECT_FALSE(handle.isValid());
}

TEST_F(Shake, UnknownPresetsWarnOnceAndShakeNothing){
    camera();
    cameratest::takeWarnings();
    EXPECT_FALSE(CameraShake::shake(ShakeSource("no_such_preset")).isValid());
    EXPECT_FALSE(CameraShake::shake(ShakeSource("no_such_preset")).isValid());
    EXPECT_FALSE(CameraShake::shake(ShakeSource("missing/file.shake")).isValid());
    std::vector<std::string> warnings = cameratest::takeWarnings();
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_NE(warnings[0].find("no_such_preset"), std::string::npos);
    EXPECT_NE(warnings[1].find("missing/file.shake"), std::string::npos);
}

TEST_F(Shake, TheInstanceCapDropsTheOldestOneShot){
    Entity cam = camera();
    CameraShaker shakerObject(&scene, cam);
    shakerObject.shake(ShakeSource("drunk"));
    for (int i = 0; i < 40; i++) shakerObject.shake(ShakeSource("explosion"));
    EXPECT_EQ(shakerObject.getInstanceCount(), (int)MAX_SHAKE_INSTANCES);
    // the sustained one stays
    EXPECT_EQ(shaker(cam).runtime->instances.front().presetName, "drunk");
}

// ---- 10. transform mode

TEST_F(Shake, TransformModeShakesAnAnchoredImageAdditively){
    Entity image = scene.createEntity();
    Transform transform;
    transform.position = Vector3(100, 50, 0);
    scene.addComponent<Transform>(image, transform);
    scene.addComponent<UIComponent>(image, {});
    scene.addComponent<ImageComponent>(image, {});
    RectTransformComponent layout;
    layout.usingAnchors = true;
    layout.positionOffset = Vector2(10, 20);
    scene.addComponent<RectTransformComponent>(image, layout);
    scene.addComponent<CameraShakerComponent>(image, {});

    CameraShaker shakerObject(&scene, image);
    // a fixed seed and a clock off the lattice's half points (Perlin is exactly 0.5 on some of them)
    ShakeOptions options;
    options.hasSeed = true;
    options.seed = 37;
    ShakeInstanceHandle handle = shakerObject.shake(ShakeSource(preset(ShakeType::Sustained, 1.0f, 13.0f, 0.0f, 0.2f,
                                                                       Vector3(1, 1, 0), Vector3(0, 0, 40))), options);
    bool rolled = false;
    for (int i = 0; i < 6; i++){
        step();
        rolled = rolled || scene.getComponent<Transform>(image).rotation != Quaternion();
    }
    RectTransformComponent& live = scene.getComponent<RectTransformComponent>(image);
    EXPECT_NE(live.positionOffset, Vector2(10, 20));
    // the layout places it from the offset: the Transform position is the layout's to write
    EXPECT_EQ(scene.getComponent<Transform>(image).position, Vector3(100, 50, 0));
    EXPECT_TRUE(rolled) << "the roll reaches the Transform's rotation";

    // a script writes the offset during the shake: it is the new base
    live.positionOffset = Vector2(300, 400);
    step();
    const ShakerRuntime& runtime = *shaker(image).runtime;
    EXPECT_EQ(runtime.baseAnchorOffset, Vector2(300, 400));
    EXPECT_NE(live.positionOffset, Vector2(300, 400));

    handle.stop(0.0f);
    step(DT, 3);
    EXPECT_EQ(live.positionOffset, Vector2(300, 400));
    EXPECT_EQ(scene.getComponent<Transform>(image).rotation, Quaternion());
}

TEST_F(Shake, TransformModeRestoresAPlainTransformExactly){
    Entity prop = scene.createEntity();
    Transform transform;
    transform.position = Vector3(1.25f, -3.5f, 7.0f);
    transform.rotation = Quaternion(0.9f, 0.1f, 0.3f, 0.2f).normalized();
    scene.addComponent<Transform>(prop, transform);
    scene.addComponent<CameraShakerComponent>(prop, {});
    const Vector3 position = transform.position;
    const Quaternion rotation = scene.getComponent<Transform>(prop).rotation;
    ShakeInstanceHandle handle = CameraShaker(&scene, prop).shake(ShakeSource("bump"));
    bool moved = false;
    for (int i = 0; i < 6; i++){
        step();
        moved = moved || scene.getComponent<Transform>(prop).position != position;
    }
    EXPECT_TRUE(moved);
    while (handle.isValid()) step();
    step();
    EXPECT_EQ(scene.getComponent<Transform>(prop).position, position);
    EXPECT_EQ(scene.getComponent<Transform>(prop).rotation, rotation);
}

TEST_F(Shake, RemovingTheShakerClearsItsOffset){
    Entity cam = camera();
    CameraShaker(&scene, cam).shake(ShakeSource("drunk"));
    step(DT, 3);
    EXPECT_TRUE(cameraOf(cam).hasViewOffset);
    scene.removeComponent<CameraShakerComponent>(cam);
    EXPECT_FALSE(cameraOf(cam).hasViewOffset);
}

// ---- the edit-mode preview

TEST_F(Shake, EditModePreviewUsesItsOwnRuntime){
    Entity cam = camera();
    system().setPaused(true);
    ASSERT_TRUE(system().previewShake(cam, *findBuiltinShakePreset("drunk"), "drunk"));
    for (int i = 0; i < 5; i++) system().update(DT);
    EXPECT_FALSE(shaker(cam).runtime && !shaker(cam).runtime->instances.empty());
    EXPECT_TRUE(system().isPreviewing());
    system().stopPreview();
    EXPECT_FALSE(cameraOf(cam).hasViewOffset);
    EXPECT_FALSE(system().isPreviewing());
    // play clears it too
    ASSERT_TRUE(system().previewAddTrauma(cam, 1.0f));
    system().setPaused(false);
    EXPECT_FALSE(system().isPreviewing());
}

// ---- .shake files and the pool

TEST(ShakeFile, RoundTripsCanonicallyAndKeepsUnknownKeys){
    const std::string text = "{\"format\": \"jasperengine.shake\", \"version\": 1, \"type\": \"sustained\", \"strength\": 0.25,"
                             " \"roughness\": 3, \"fade_in\": 1, \"fade_out\": 1, \"position_influence\": [0.1, 1, 0.1],"
                             " \"rotation_influence\": [20, 1, 1], \"note\": {\"by\": \"designer\"}}";
    ShakeParseResult parsed = parseShakePreset(text);
    ASSERT_TRUE(parsed.ok()) << parsed.errorText();
    EXPECT_EQ(parsed.file->preset, *findBuiltinShakePreset("offroad"));
    const std::string canonical = writeShakePreset(*parsed.file);
    EXPECT_NE(canonical.find("\"note\": {\"by\":\"designer\"}"), std::string::npos);
    ShakeParseResult again = parseShakePreset(canonical);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(writeShakePreset(*again.file), canonical);
}

TEST(ShakeFile, LenientValuesAndHardErrors){
    ShakeParseResult repaired = parseShakePreset("{\"format\": \"jasperengine.shake\", \"version\": 1, \"strength\": -2,"
                                                 " \"type\": \"wobbly\", \"position_influence\": [1, 2]}");
    ASSERT_TRUE(repaired.ok());
    EXPECT_FLOAT_EQ(repaired.file->preset.strength, 0.0f);
    EXPECT_EQ(repaired.file->preset.type, ShakeType::OneShot);
    EXPECT_EQ(repaired.file->preset.positionInfluence, Vector3(1, 1, 1));
    EXPECT_EQ(repaired.warnings.size(), 3u);

    ShakeParseResult syntax = parseShakePreset("{\n  \"format\": \"jasperengine.shake\",\n  \"version\": 1,\n  \"strength\" 2\n}");
    EXPECT_FALSE(syntax.ok());
    EXPECT_EQ(syntax.line, 4);
    EXPECT_FALSE(parseShakePreset("{\"format\": \"jasperengine.anim\", \"version\": 1}").ok());
    EXPECT_FALSE(parseShakePreset("{\"format\": \"jasperengine.shake\", \"version\": 2}").ok());
    EXPECT_FALSE(parseShakePreset("[1, 2]").ok());
}

TEST(ShakeFile, BuiltinsMatchTheGamesMilkShakeAssets){
    // MilkShake's 13, EZ Camera Shake's 3 and S48's pixel-art pixel_hit, pixel_heavy, pixel_rumble
    EXPECT_EQ(builtinShakePresets().size(), 19u);
    const ShakePreset* punch = findBuiltinShakePreset("punch");
    ASSERT_TRUE(punch);
    EXPECT_EQ(*punch, preset(ShakeType::OneShot, 0.5f, 2.5f, 0.05f, 1.0f, Vector3(4, 0, 0), Vector3(20, 90, 20)));
    const ShakePreset* entity = findBuiltinShakePreset("entity_shake");
    ASSERT_TRUE(entity);
    EXPECT_EQ(*entity, preset(ShakeType::OneShot, 0.2f, 15.0f, 7.0f, 2.0f, Vector3(0, 0, 0), Vector3(20, 0, 20)));
    EXPECT_EQ(findBuiltinShakePreset("explosion.shake"), nullptr);
}

TEST_F(Shake, ShakeFilesPlayAndReloadWhenEdited){
    cameratest::AssetRoot assets;   // asset paths under the test's temporary root
    Entity cam = camera();
    ShakePresetPool::clear();
    ShakePreset p = preset(ShakeType::Sustained, 0.3f, 4.0f, 0.0f, 0.0f);
    cameratest::writeAsset("shakes/boom.shake", writeShakePreset(p));
    ShakeInstanceHandle handle = CameraShake::shake(ShakeSource("shakes/boom.shake"));
    ASSERT_TRUE(handle.isValid());
    EXPECT_FLOAT_EQ(shaker(cam).runtime->instances.back().instance.getPreset().strength, 0.3f);
    // edited on disk (another size): the next shake reads it again
    p.strength = 0.75f;
    p.roughness = 12.5f;
    cameratest::writeAsset("shakes/boom.shake", writeShakePreset(p));
    CameraShake::shake(ShakeSource("shakes/boom.shake"));
    EXPECT_FLOAT_EQ(shaker(cam).runtime->instances.back().instance.getPreset().strength, 0.75f);
    EXPECT_FLOAT_EQ(shaker(cam).runtime->instances.back().instance.getPreset().roughness, 12.5f);
}

// ---- Lua

TEST_F(Shake, LuaApi){
    Entity cam = camera();
    scene.setEntityName(cam, "LuaCam");
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L,
        "local names = CameraShake.getPresetNames()\n"
        "shakeResult = {}\n"
        "shakeResult.names = #names\n"
        "local inst = CameraShake.shake('explosion', {strength = 0.5, seed = 3})\n"
        "shakeResult.valid = inst:isValid()\n"
        "shakeResult.scale = inst.strengthScale\n"
        "shakeResult.state = inst.state\n"
        "inst.roughnessScale = 2\n"
        "shakeResult.rough = inst.roughnessScale\n"
        "local t = CameraShake.shake({type = 'sustained', strength = 1, roughness = 5, rotationInfluence = Vector3(0, 0, 9)})\n"
        "shakeResult.tableValid = t:isValid()\n"
        "t:stop(0)\n"
        "CameraShake.addTrauma(0.25)\n"
        "shakeResult.badPreset = not pcall(CameraShake.shake, 42)\n"
        "shakeResult.badOption = not pcall(CameraShake.shake, 'explosion', {seed = 'x'})\n"), LUA_OK) << lua_tostring(L, -1);
    auto field = [&](const char* name){
        lua_getglobal(L, "shakeResult");
        lua_getfield(L, -1, name);
        lua_remove(L, -2);
    };
    // (16 before S48's pixel_hit, pixel_heavy and pixel_rumble)
    field("names"); EXPECT_EQ(lua_tointeger(L, -1), 19); lua_pop(L, 1);
    field("valid"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    field("scale"); EXPECT_FLOAT_EQ((float)lua_tonumber(L, -1), 0.5f); lua_pop(L, 1);
    field("state"); EXPECT_STREQ(lua_tostring(L, -1), "fading_in"); lua_pop(L, 1);
    field("rough"); EXPECT_FLOAT_EQ((float)lua_tonumber(L, -1), 2.0f); lua_pop(L, 1);
    field("tableValid"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    field("badPreset"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    field("badOption"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    EXPECT_EQ(shaker(cam).runtime->instances.size(), 2u);
    EXPECT_FLOAT_EQ(shaker(cam).runtime->trauma.trauma, 0.25f);
    EXPECT_EQ(shaker(cam).runtime->instances.back().instance.getPreset().rotationInfluence, Vector3(0, 0, 9));

    // the per-shaker object and its properties
    lua_pushinteger(L, (lua_Integer)cam);
    lua_setglobal(L, "shakeEntity");
    const Entity followed = scene.createEntity();
    scene.addComponent<Transform>(followed, {});
    lua_pushinteger(L, (lua_Integer)followed);
    lua_setglobal(L, "shakeTarget");
    ASSERT_EQ(luaL_dostring(L,
        "local go = GameObject(Engine.scene, shakeEntity)\n"
        "local s = go:getCameraShaker()\n"
        "shakeResult.has = go:hasCameraShaker() and s ~= nil\n"
        "s.mode = '2d'\n"
        "s.traumaPower = 9\n"
        "shakeResult.mode = s.mode\n"
        "shakeResult.power = s.traumaPower\n"
        "local h = s:shakeAt(Vector3(0, 0, 10), 4, 'punch')\n"
        "shakeResult.near = h:isValid()\n"
        "shakeResult.count = s:getInstanceCount()\n"
        "shakeResult.follow = go:getCameraFollow() == nil\n"
        "local f = CameraFollow(Engine.scene, shakeEntity)\n"
        "f.target = GameObject(Engine.scene, shakeTarget)\n"
        "shakeResult.targetOk = f.target ~= nil and f.target.entity == shakeTarget\n"
        "f.offsetSpace = 'target_yaw'\n"
        "shakeResult.space = f.offsetSpace\n"
        "f.target = nil\n"
        "shakeResult.targetNil = f.target == nil\n"), LUA_OK) << lua_tostring(L, -1);
    field("has"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    field("mode"); EXPECT_STREQ(lua_tostring(L, -1), "2d"); lua_pop(L, 1);
    field("power"); EXPECT_FLOAT_EQ((float)lua_tonumber(L, -1), 4.0f); lua_pop(L, 1);
    field("near"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    field("count"); EXPECT_EQ(lua_tointeger(L, -1), 3); lua_pop(L, 1);
    field("follow"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    field("targetOk"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    field("space"); EXPECT_STREQ(lua_tostring(L, -1), "target_yaw"); lua_pop(L, 1);
    field("targetNil"); EXPECT_TRUE(lua_toboolean(L, -1)); lua_pop(L, 1);
    EXPECT_EQ(scene.getComponent<CameraFollowComponent>(cam).target, NULL_ENTITY);
}

// ---- review fixes

TEST_F(Shake, TransformModeLeavesRelativeScriptMovesExact){
    // a script moving the GameObject relative to its current pose (position = position + v * dt)
    // sees the unshaken pose each frame (preUpdate writes the base back): no shake soaks in
    Entity prop = scene.createEntity();
    Transform transform;
    transform.position = Vector3(2, 1, 0);
    scene.addComponent<Transform>(prop, transform);
    scene.addComponent<CameraShakerComponent>(prop, {});
    ShakeOptions options;
    options.hasSeed = true;
    options.seed = 11;
    ShakeInstanceHandle handle = CameraShaker(&scene, prop).shake(ShakeSource(preset(ShakeType::OneShot, 1.0f, 13.0f, 0.05f, 0.3f,
                                                                                    Vector3(1, 1, 1), Vector3(5, 5, 5))), options);
    const Vector3 velocity(0.5f, 0.0f, -0.25f);
    int frames = 0;
    bool moved = false;
    while ((handle.isValid() || shaker(prop).runtime->transformActive) && frames < 300){
        system().preUpdate();
        Transform& t = scene.getComponent<Transform>(prop);
        t.position = t.position + velocity * DT;   // the script
        step();
        frames++;
        moved = moved || scene.getComponent<Transform>(prop).position != Vector3(2, 1, 0) + velocity * (DT * frames);
    }
    EXPECT_TRUE(moved) << "the shake showed while the script moved it";
    const Vector3 expected = Vector3(2, 1, 0) + velocity * (DT * frames);
    const Vector3 got = scene.getComponent<Transform>(prop).position;
    EXPECT_NEAR(got.x, expected.x, 1e-4f);
    EXPECT_NEAR(got.y, expected.y, 1e-4f);
    EXPECT_NEAR(got.z, expected.z, 1e-4f);
    EXPECT_EQ(scene.getComponent<Transform>(prop).rotation, Quaternion());
}

TEST_F(Shake, TwoDTransformModeUsesTheSceneCameraHeight){
    Entity cam = camera(Vector3(0, 0, 0), CameraType::CAMERA_ORTHO);
    // setCamera sizes an auto-resized camera to the canvas: a fixed 20-unit view after it
    scene.setCamera(cam);
    cameraOf(cam).autoResize = false;
    cameraOf(cam).bottomClip = 0.0f;
    cameraOf(cam).topClip = 20.0f;
    Entity sprite = scene.createEntity();
    scene.addComponent<Transform>(sprite, {});
    CameraShakerComponent component;
    component.mode = CameraShakeMode::Mode2D;
    scene.addComponent<CameraShakerComponent>(sprite, component);
    CameraShaker(&scene, sprite).shake(ShakeSource(preset(ShakeType::Sustained, 1.0f, 13.0f, 0.0f, 0.0f)));
    float maxX = 0.0f;
    for (int i = 0; i < 300; i++){
        step();
        maxX = std::max(maxX, std::fabs(shaker(sprite).runtime->offsetPosition.x));
    }
    // tenths of the camera's 20 units, not of the canvas
    EXPECT_LE(maxX, 2.0f);
    EXPECT_GT(maxX, 0.2f);
}

TEST_F(Shake, TheAudioListenerKeepsTheSteadyUp){
    Entity cam = camera(Vector3(0, 0, 10));
    cameraOf(cam).hasViewOffset = true;
    cameraOf(cam).viewOffsetRotation = shakeEulerToQuaternion(Vector3(0, 0, 30));
    render(cam);
    const Vector3 steady = cameraOf(cam).steadyWorldUp;
    EXPECT_NEAR(steady.x, 0.0f, 1e-5f);
    EXPECT_NEAR(steady.y, 1.0f, 1e-5f);
    // the drawn view rolled
    EXPECT_GT(std::fabs(cameraOf(cam).worldUp.x), 0.1f);
}

TEST_F(Shake, RemovingOrAddingTheCameraSwitchesModeCleanly){
    Entity cam = camera(Vector3(1, 2, 3));
    CameraShaker(&scene, cam).shake(ShakeSource(preset(ShakeType::Sustained, 1.0f, 13.0f, 0.0f, 0.0f)));
    step(DT, 4);
    ASSERT_TRUE(cameraOf(cam).hasViewOffset);
    scene.removeComponent<CameraComponent>(cam);
    EXPECT_FALSE(shaker(cam).runtime->applied);
    EXPECT_EQ(shaker(cam).runtime->offsetPosition, Vector3(0, 0, 0));
    // transform mode from its own pose
    step(DT, 4);
    EXPECT_TRUE(shaker(cam).runtime->transformActive);
    EXPECT_NE(scene.getComponent<Transform>(cam).position, Vector3(1, 2, 3));
    // a camera again: the Transform gets its base back at once, the shake goes to the view
    CameraComponent component;
    component.useTarget = false;
    scene.addComponent<CameraComponent>(cam, component);
    EXPECT_EQ(scene.getComponent<Transform>(cam).position, Vector3(1, 2, 3));
    EXPECT_FALSE(shaker(cam).runtime->transformActive);
    step(DT, 3);
    EXPECT_TRUE(cameraOf(cam).hasViewOffset);
    EXPECT_EQ(scene.getComponent<Transform>(cam).position, Vector3(1, 2, 3));
}

TEST_F(Shake, StaticCallsInAPausedSceneAreDroppedWithTheirOwnWarning){
    camera();
    system().setPaused(true);
    cameratest::takeWarnings();
    EXPECT_FALSE(CameraShake::shake(ShakeSource("explosion")).isValid());
    std::vector<std::string> warnings = cameratest::takeWarnings();
    for (const std::string& warning : warnings){
        EXPECT_EQ(warning.find("no global Camera Shaker"), std::string::npos) << "a pause is not a missing shaker: " << warning;
    }
    system().setPaused(false);
    EXPECT_TRUE(CameraShake::shake(ShakeSource("explosion")).isValid());
}

// ---- 13. the budget

TEST_F(Shake, FourShakersWithEightInstancesStayWithinBudget){
    std::vector<Entity> cams;
    for (int i = 0; i < 4; i++){
        Entity cam = camera();
        cams.push_back(cam);
        CameraShaker object(&scene, cam);
        for (int k = 0; k < 8; k++) object.shake(ShakeSource("drunk"));
        object.addTrauma(1.0f);
        shaker(cam).traumaDecay = 0.0f;
    }
    step(DT, 10);
    std::vector<double> frames;
    for (int batch = 0; batch < 21; batch++){
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; i++) step();
        auto end = std::chrono::steady_clock::now();
        frames.push_back(std::chrono::duration<double, std::milli>(end - start).count() / 100.0);
    }
    std::sort(frames.begin(), frames.end());
    const double median = frames[frames.size() / 2];
    RecordProperty("median_ms_per_frame", std::to_string(median));
#ifdef NDEBUG
    EXPECT_LE(median, 0.02) << "4 shakers x 8 instances: " << median << " ms per frame";
#endif
    // idle shakers cost nothing (no runtime touched beyond the early-out)
    for (Entity cam : cams) EXPECT_EQ(shaker(cam).runtime->instances.size(), 8u);
}
