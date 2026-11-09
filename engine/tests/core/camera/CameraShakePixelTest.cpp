// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48 R22 (docs/specs/S48-pixel-fx.md AC18): the pixel-art shake. pixelfx's kick formula
// (act.js), a shaker's Pixel Snap (whole art pixels, no roll) and Tick Rate, the presets' units
// and tick keys in the .shake format (old files unchanged), CameraShake.kick and the built-ins.

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
#include "component/CameraShakerComponent.h"
#include "component/Transform.h"
#include "subsystem/CameraControlSystem.h"

#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::camera;

namespace {

    constexpr float kDt = 1.0f / 60.0f;

    // pixelfx src/core/act.js, `get shake()`: shakeT counts down from shake0
    void actJsShake(float amp, float shakeT, float shake0, float& x, float& y){
        if (shakeT <= 0.0f){
            x = y = 0.0f;
            return;
        }
        const float k = amp * (shakeT / shake0);
        const float f = std::floor(shakeT * 60.0f);
        x = std::round(std::sin(f * 2.4f) * k);
        y = std::round(std::cos(f * 3.7f) * k * 0.7f);
    }

    // a scene with an orthographic camera shaker (the Engine's scene while it lives)
    struct OrthoShaker{
        Scene scene;
        Entity camera = NULL_ENTITY;

        OrthoShaker(){
            cameratest::takeWarnings();
            Engine::setScene(&scene);
            camera = scene.createEntity();
            Transform transform;
            transform.position = Vector3(0, 0, 10);
            scene.addComponent<Transform>(camera, transform);
            CameraComponent component;
            component.type = CameraType::CAMERA_ORTHO;
            component.useTarget = false;
            component.leftClip = 0.0f;
            component.rightClip = 320.0f;
            component.bottomClip = 0.0f;
            component.topClip = 180.0f;
            scene.addComponent<CameraComponent>(camera, component);
            scene.addComponent<CameraShakerComponent>(camera, {});
        }
        ~OrthoShaker(){
            Engine::removeScene(&scene);
        }

        CameraControlSystem& system(){ return *scene.getSystem<CameraControlSystem>(); }
        CameraShakerComponent& shaker(){ return scene.getComponent<CameraShakerComponent>(camera); }
        CameraComponent& cameraComponent(){ return scene.getComponent<CameraComponent>(camera); }
        float pixel(){ return system().pixelWorldSize(camera, &cameraComponent()); }
    };

    bool wholePixels(float value, float pixel){
        const float n = value / pixel;
        return std::fabs(n - std::round(n)) < 1e-3f;
    }

}

TEST(ShakeInstance, Kick){
    // CameraShake.kick(3, 0.25): 3 art pixels decaying over 0.25 s, stepped at 60 Hz
    const ShakePreset kick = makeKickPreset(3.0f, 0.25f);
    EXPECT_EQ(kick.type, ShakeType::Kick);
    EXPECT_EQ(kick.units, ShakeUnits::Pixels);
    ShakeInstance instance(kick, 7);
    for (int i = 0; i < 10; i++){
        const ShakeOffset offset = instance.update(kDt);
        float x = 0.0f, y = 0.0f;
        actJsShake(3.0f, 0.25f - i * kDt, 0.25f, x, y);
        EXPECT_FLOAT_EQ(offset.pixels.x, x) << "tick " << i;
        EXPECT_FLOAT_EQ(offset.pixels.y, y) << "tick " << i;
        EXPECT_EQ(offset.position, Vector3(0, 0, 0));
        EXPECT_EQ(offset.rotation, Vector3(0, 0, 0));
    }
    // the kick ends at its length
    for (int i = 10; i < 20; i++) instance.update(kDt);
    EXPECT_TRUE(instance.isFinished());
    EXPECT_EQ(instance.getOffset().pixels, Vector3(0, 0, 0));
}

TEST(ShakeInstance, TickHoldsTheOffsetBetweenTicks){
    // pixel_rumble steps at 30 Hz: at 60 fps the offset changes at most every other frame
    const ShakePreset* rumble = findBuiltinShakePreset("pixel_rumble");
    ASSERT_TRUE(rumble);
    EXPECT_EQ(rumble->tick, 30.0f);
    ShakeInstance instance(*rumble, 11);
    Vector3 previous = instance.update(kDt).pixels;
    int changes = 0;
    bool changedLastFrame = false;
    for (int i = 0; i < 120; i++){
        const Vector3 now = instance.update(kDt).pixels;
        const bool changed = now != previous;
        EXPECT_FALSE(changed && changedLastFrame) << "frame " << i;
        if (changed) changes++;
        changedLastFrame = changed;
        previous = now;
    }
    EXPECT_GT(changes, 20);
}

TEST(ShakeInstance, PixelBuiltins){
    const ShakePreset* hit = findBuiltinShakePreset("pixel_hit");
    const ShakePreset* heavy = findBuiltinShakePreset("pixel_heavy");
    ASSERT_TRUE(hit && heavy);
    EXPECT_EQ(hit->type, ShakeType::Kick);
    EXPECT_EQ(hit->units, ShakeUnits::Pixels);
    EXPECT_FLOAT_EQ(hit->strength, 2.0f);
    EXPECT_FLOAT_EQ(hit->fadeOut, 0.15f);
    EXPECT_FLOAT_EQ(heavy->strength, 4.0f);
    EXPECT_FLOAT_EQ(heavy->fadeOut, 0.35f);
}

TEST(ShakeFile, UnitsAndTickRoundTripAndOldFilesStayByteIdentical){
    // an old file (no units, no tick) writes back exactly as it was
    const std::string old = writeShakePreset(*findBuiltinShakePreset("punch"));
    EXPECT_EQ(old.find("units"), std::string::npos);
    EXPECT_EQ(old.find("tick"), std::string::npos);
    ShakeParseResult parsed = parseShakePreset(old);
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_EQ(parsed.file->preset.units, ShakeUnits::View);
    EXPECT_EQ(writeShakePreset(*parsed.file), old);

    // the new keys and the kick type
    ShakePreset kick = makeKickPreset(2.0f, 0.2f);
    kick.tick = 30.0f;
    const std::string text = writeShakePreset(kick);
    EXPECT_NE(text.find("\"type\": \"kick\""), std::string::npos);
    EXPECT_NE(text.find("\"units\": \"pixels\""), std::string::npos);
    EXPECT_NE(text.find("\"tick\": 30"), std::string::npos);
    parsed = parseShakePreset(text);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.file->preset, kick);
    EXPECT_EQ(writeShakePreset(*parsed.file), text);

    // a bad unit is repaired with a warning (the lenient parser)
    parsed = parseShakePreset("{\"format\": \"jasperengine.shake\", \"version\": 1, \"units\": \"miles\"}");
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.file->preset.units, ShakeUnits::View);
    ASSERT_FALSE(parsed.warnings.empty());
    EXPECT_NE(parsed.warnings[0].find("units"), std::string::npos);
}

TEST(CameraShaker, PixelSnap){
    OrthoShaker fixture;
    fixture.shaker().pixelSnap = true;
    const float pixel = fixture.pixel();
    ASSERT_GT(pixel, 0.0f);
    // a rolling, noisy shake (in view units) and a pixel kick, both snapped
    CameraShake::shake(ShakeSource("earthquake"));
    CameraShake::kick(3.0f, 0.5f);
    bool moved = false;
    for (int i = 0; i < 120; i++){
        fixture.system().step(kDt, kDt);
        const ShakerRuntime& runtime = *fixture.shaker().runtime;
        EXPECT_TRUE(wholePixels(runtime.offsetPosition.x, pixel)) << "frame " << i << ": " << runtime.offsetPosition.x;
        EXPECT_TRUE(wholePixels(runtime.offsetPosition.y, pixel)) << "frame " << i << ": " << runtime.offsetPosition.y;
        EXPECT_EQ(runtime.offsetRotation, Vector3(0, 0, 0)) << "frame " << i;
        if (runtime.offsetPosition.x != 0.0f || runtime.offsetPosition.y != 0.0f) moved = true;
    }
    EXPECT_TRUE(moved);
}

TEST(CameraShaker, PixelSnapOffKeepsTheRoll){
    // the red control of PixelSnap: without it the same shakes are off-grid and roll
    OrthoShaker fixture;
    const float pixel = fixture.pixel();
    CameraShake::shake(ShakeSource("earthquake"));
    bool offGrid = false, rolled = false;
    for (int i = 0; i < 120; i++){
        fixture.system().step(kDt, kDt);
        const ShakerRuntime& runtime = *fixture.shaker().runtime;
        if (!wholePixels(runtime.offsetPosition.x, pixel)) offGrid = true;
        if (runtime.offsetRotation.z != 0.0f) rolled = true;
    }
    EXPECT_TRUE(offGrid);
    EXPECT_TRUE(rolled);
}

TEST(CameraShaker, TickRate){
    // Tick Rate 30 at 60 fps: the applied offset changes at most every other frame
    {
    OrthoShaker fixture;
    fixture.shaker().tickRate = 30.0f;
    CameraShake::shake(ShakeSource("electrocute"));
    CameraShake::shake(ShakeSource("earthquake"));
    fixture.system().step(kDt, kDt);
    Vector3 previous = fixture.shaker().runtime->offsetPosition;
    bool changedLastFrame = false;
    int changes = 0;
    for (int i = 0; i < 120; i++){
        fixture.system().step(kDt, kDt);
        const Vector3 now = fixture.shaker().runtime->offsetPosition;
        const bool changed = now != previous;
        EXPECT_FALSE(changed && changedLastFrame) << "frame " << i;
        if (changed) changes++;
        changedLastFrame = changed;
        previous = now;
    }
    EXPECT_GT(changes, 20);
    }

    // the red control: Tick Rate 0 changes it every frame
    OrthoShaker every;
    CameraShake::shake(ShakeSource("earthquake"));
    every.system().step(kDt, kDt);
    Vector3 previous = every.shaker().runtime->offsetPosition;
    int consecutive = 0;
    for (int i = 0; i < 30; i++){
        every.system().step(kDt, kDt);
        const Vector3 now = every.shaker().runtime->offsetPosition;
        if (now != previous) consecutive++;
        previous = now;
    }
    EXPECT_GT(consecutive, 25);
}
