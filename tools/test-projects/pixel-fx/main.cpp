// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48's test project (docs/specs/S48-pixel-fx.md AC26): the runtime Pixel FX in a WebAssembly build without
// pthreads, on WebGL2 (GLES3 GLSL). Checked in a browser (WEB_PROJECT=pixel-fx tools/build.sh web,
// GATE=pixel-fx tools/build.sh web-gate) by the lines below and the centre pixel:
// - five library:sprite sprites, one per Pixel FX feature group: the PIXEL4 outline with Dissolve (Enemy),
//   Dither Fade at 0.5 (Fade), Ramp Recolour on the default ramp (Petrify), Drop Shadow (Shadow), and Hit flash
//   at 1 in magenta (Flash, at the centre), under a Glitch post-process at intensity 0;
// - "[gate] pixel_fx dissolve true", "[gate] pixel_fx hit_stop true", "[gate] pixel_fx glitch burst 0.8": a PixelFX
//   dissolve on Enemy, a Hit-Stop and a glitch burst, started together;
// - "[gate] pixel_fx dissolve finished": the dissolve ended by itself; Enemy is then held at 0.5 (PixelFX set);
// - "[gate] pixel_fx covered true", "[gate] pixel_fx revealed": an iris Transition play covers and reveals;
// - the frame then holds: the centre pixel is the flash's magenta, #ff00ff.

#include "JasperEngine.h"
#include "pixelart/Glitch.h"
#include "pixelart/HitStop.h"
#include "pixelart/PixelFxPlayer.h"
#include "pixelart/ScreenTransition.h"

#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    Scene scene;
    Camera sceneCamera(&scene);
    Sprite enemy(&scene);
    Sprite fade(&scene);
    Sprite petrify(&scene);
    Sprite shadow(&scene);
    Sprite flash(&scene);

    std::vector<uint8_t> figure;
    PixelFxHandle dissolve;
    TransitionHandle transition;
    bool finished = false;
    int frames = 0;
    int stage = 0;

    void makeSprite(Sprite& sprite, float x, float y, float size, std::vector<std::string> features,
                    std::vector<std::pair<std::string, Vector4>> params) {
        sprite.setTexture("pixel-fx|figure", TextureData(12, 12, (unsigned int)figure.size(), ColorFormat::RGBA, 4, figure.data()));
        sprite.setSize(size, size);
        sprite.setPivotPreset(PivotPreset::CENTER);
        sprite.setPosition(x, y, 0.0f);
        Material& material = sprite.getComponent<MeshComponent>().submeshes[0].material;
        material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
        material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
        material.shader = "library:sprite";
        material.shaderFeatures = std::move(features);
        material.shaderParams = std::move(params);
    }

    Vector4 number(float value) {
        return Vector4(value, 0.0f, 0.0f, 0.0f);
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(320, 180);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.10f, 0.11f, 0.14f);
    sceneCamera.setType(CameraType::CAMERA_ORTHO);
    sceneCamera.setOrtho(0.0f, 320.0f, 0.0f, 180.0f, -10.0f, 10.0f);
    scene.setCamera(&sceneCamera);

    // an 8 x 8 figure on a transparent 12 x 12 frame, so outlines and shadows have room
    figure.assign(12 * 12 * 4, 0);
    for (int y = 2; y < 10; y++){
        for (int x = 2; x < 10; x++){
            uint8_t* p = &figure[(size_t)(y * 12 + x) * 4];
            p[0] = (uint8_t)(200 - 10 * y);
            p[1] = (uint8_t)(80 + 10 * x);
            p[2] = 40;
            p[3] = 255;
        }
    }
    makeSprite(enemy, 40.0f, 90.0f, 36.0f, {"FLASH", "DISSOLVE", "OUTLINE", "OUTLINE_STYLE_PIXEL4"},
               {{"flashColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f)}, {"flashAmount", number(0.0f)}, {"dissolveProgress", number(0.0f)},
                {"outlineWidth", number(1.0f)}, {"outlineColorFrom", number(1.0f)}});
    makeSprite(fade, 100.0f, 90.0f, 36.0f, {"DITHER_FADE"}, {{"ditherFadeAmount", number(0.5f)}});
    makeSprite(petrify, 220.0f, 90.0f, 36.0f, {"RAMP_RECOLOR"}, {{"rampAmount", number(1.0f)}, {"rampBevel", number(1.0f)}});
    makeSprite(shadow, 280.0f, 90.0f, 36.0f, {"DROP_SHADOW"},
               {{"shadowOffset", Vector4(1.0f, -1.0f, 0.0f, 0.0f)}, {"shadowColor", Vector4(0.0f, 0.0f, 0.0f, 0.5f)}});
    // the centre pixel: the flash's colour over the whole figure (0 and 1 are the same in sRGB and linear)
    makeSprite(flash, 160.0f, 90.0f, 48.0f, {"FLASH"}, {{"flashColor", Vector4(1.0f, 0.0f, 1.0f, 1.0f)}, {"flashAmount", number(1.0f)}});

    PostProcessingSettings post;
    post.glitchEnabled = true;
    post.glitchIntensity = 0.0f;
    scene.setPostProcessing(post);

    Engine::onUpdate.add("pixelFxTest", [](){
        frames++;
        if (stage == 0 && frames == 10){
            stage = 1;
            PixelFxOptions options;
            options.onFinished = [](){ finished = true; };
            std::string error;
            dissolve = PixelFxPlayer::play(&scene, enemy.getEntity(), PixelFxEffect::DISSOLVE, 0.4f, options, &error);
            if (dissolve.isValid()) Log::print("[gate] pixel_fx dissolve true");
            else Log::error("[gate] pixel_fx dissolve did not start: %s", error.c_str());
            HitStop::play(0.05f);
            if (HitStop::isActive()) Log::print("[gate] pixel_fx hit_stop true");
            else Log::error("[gate] pixel_fx hit_stop is not active");
            Glitch::burst(0.2f, 0.8f);
            Log::print("[gate] pixel_fx glitch burst %.1f", Glitch::getBurstIntensity());
        }else if (stage == 1 && finished && !HitStop::isActive() && Glitch::getBurstRemaining() <= 0.0f){
            stage = 2;
            Log::print("[gate] pixel_fx dissolve finished");
            std::string error;
            if (!PixelFxPlayer::set(&scene, enemy.getEntity(), "dissolve", number(0.5f), &error))
                Log::error("[gate] pixel_fx set failed: %s", error.c_str());
            transition = ScreenTransition::play(TransitionKind::IRIS, 0.6f, TransitionOptions::fromDefaults(), [](){
                Log::print("[gate] pixel_fx covered %s", ScreenTransition::isCovered() ? "true" : "false");
            });
            if (!transition.isValid()) Log::error("[gate] pixel_fx transition did not start");
        }else if (stage == 2 && transition.isDone() && !ScreenTransition::isActive()){
            stage = 3;
            Log::print("[gate] pixel_fx revealed");
        }else if (stage < 3 && frames > 3600){
            Log::error("[gate] pixel_fx stuck at stage %d after 3600 frames", stage);
            stage = 3;
        }
    });
}
