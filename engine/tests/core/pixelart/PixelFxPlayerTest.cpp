// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48 R24-R26 (docs/specs/S48-pixel-fx.md AC20-AC21): the PixelFX player. Every effect starts and
// ends where the spec says (the neutral 0, the one-way fade_out and dissolve at 1); a sprite's
// effect waits for its variant, runs on game time (frozen by a hit-stop unless the GameObject is
// Hit Stop Exempt, or unscaled), turns a missing feature on in the GameObject's own material and
// replaces a running effect of the same feature; a dissolve's embers come out only on opaque texels
// whose key the front crossed that frame, where the CPU mirror of the shader's key says
// (PixelFxKey). test_pixel_fx_lua.py checks the same through Lua in Play.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/HitStopExemptComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "pixelart/HitStop.h"
#include "pixelart/PixelFxKey.h"
#include "pixelart/PixelFxPlayer.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "texture/Texture.h"

#include <cmath>
#include <set>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    constexpr double kDt = 1.0 / 60.0;
    constexpr int kSide = 16;

    float valueOf(const std::vector<PixelFxValue>& values, const char* member, int component = 0){
        for (const PixelFxValue& v : values){
            if (std::string(v.member) == member)
                return component == 0 ? v.value.x : component == 1 ? v.value.y : component == 2 ? v.value.z : v.value.w;
        }
        ADD_FAILURE() << "no value " << member;
        return -1.0f;
    }

    // a 16 x 16 frame: the left half opaque, the right half transparent
    PixelFxFrameAlpha halfFrame(){
        PixelFxFrameAlpha frame;
        frame.width = kSide;
        frame.height = kSide;
        frame.opaque.assign((size_t)kSide * kSide, 0);
        for (int row = 0; row < kSide; row++)
            for (int x = 0; x < kSide / 2; x++) frame.opaque[(size_t)row * kSide + x] = 1;
        return frame;
    }

    class PixelFxPlayerScene : public ::testing::Test {
    protected:
        void SetUp() override {
            PixelFxPlayer::reset();
            HitStop::reset();
            pixels.assign((size_t)kSide * kSide * 4, 0);
            for (int y = 0; y < kSide; y++)
                for (int x = 0; x < kSide / 2; x++) pixels[((size_t)y * kSide + x) * 4 + 3] = 255;
        }
        void TearDown() override {
            PixelFxPlayer::setEmberEmitter(nullptr);
            PixelFxPlayer::reset();
            HitStop::reset();
        }

        // a drawn sprite (its variant loaded) with a library:sprite material
        Entity sprite(const std::vector<std::string>& features){
            const Entity e = scene.createEntity();
            scene.addComponent<Transform>(e, Transform());
            scene.addComponent<SpriteComponent>(e, SpriteComponent());
            scene.addComponent<MeshComponent>(e, MeshComponent());
            MeshComponent& mesh = scene.getComponent<MeshComponent>(e);
            mesh.numSubmeshes = 1;
            Material& material = mesh.submeshes[0].material;
            material.shader = "library:sprite";
            material.shaderFeatures = features;
            material.baseColorTexture = Texture("pixel_fx_player_test", TextureData(kSide, kSide, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
            mesh.aabb = AABB(Vector3(-0.08f, -0.08f, 0.0f), Vector3(0.08f, 0.08f, 0.0f));
            mesh.loaded = true;
            return e;
        }

        Vector4 param(Entity e, const char* member){
            return ShaderLibraryValues::getParam(scene.getComponent<MeshComponent>(e).submeshes[0].material.shaderParams, "library:sprite", member);
        }

        bool hasFeature(Entity e, const char* feature){
            const Material& material = scene.getComponent<MeshComponent>(e).submeshes[0].material;
            return ShaderLibraryValues::isFeatureEnabled(material.shaderFeatures, "library:sprite", feature);
        }

        // the renderer reloads what the player turned on: here the test does
        void loadVariant(Entity e){
            MeshComponent& mesh = scene.getComponent<MeshComponent>(e);
            mesh.needReload = false;
            mesh.needVariantReload = false;
            mesh.loaded = true;
        }

        std::vector<uint8_t> pixels;
        Scene scene;
    };

}

// AC20: each effect starts and ends at its values; the symmetric ones end at the neutral 0
TEST(PixelFxPlayer, SampleStartsAndEndsWhereTheSpecSays){
    PixelFxOptions options;
    struct Case { PixelFxEffect effect; const char* member; float start; float end; };
    const Case cases[] = {
        {PixelFxEffect::FLASH, "flashAmount", 1.0f, 0.0f},
        {PixelFxEffect::FADE_OUT, "ditherFadeAmount", 0.0f, 1.0f},
        {PixelFxEffect::FADE_IN, "ditherFadeAmount", 1.0f, 0.0f},
        {PixelFxEffect::BLINK, "ditherFadeAmount", 0.0f, 0.0f},
        {PixelFxEffect::DISSOLVE, "dissolveProgress", 0.0f, 1.0f},
        {PixelFxEffect::UNDISSOLVE, "dissolveProgress", 1.0f, 0.0f},
        {PixelFxEffect::PETRIFY, "rampAmount", 0.0f, 0.0f},
        {PixelFxEffect::FREEZE, "rampAmount", 0.0f, 0.0f},
        {PixelFxEffect::POISON, "rampAmount", 0.0f, 0.0f},
    };
    for (const Case& c : cases){
        EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(c.effect, 0.8f, options, 0.0), c.member), c.start) << pixelFxEffectName(c.effect);
        EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(c.effect, 0.8f, options, 0.8), c.member), c.end) << pixelFxEffectName(c.effect);
    }
    // the status looks: in, held at full strength (half the duration by default), out
    EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(PixelFxEffect::PETRIFY, 1.0f, options, 0.5), "rampAmount"), 1.0f);
    // blink: count dithered blinks, gone at each blink's middle
    options.count = 4;
    EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(PixelFxEffect::BLINK, 1.0f, options, 0.125), "ditherFadeAmount"), 1.0f);
    EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(PixelFxEffect::BLINK, 1.0f, options, 0.25), "ditherFadeAmount"), 0.0f);
    // the outline pulse is whole on or off (never a partial alpha) and ends off
    options.hasColor = true;
    options.color = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(PixelFxEffect::OUTLINE_PULSE, 1.0f, options, 0.05), "outlineColor", 3), 1.0f);
    EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(PixelFxEffect::OUTLINE_PULSE, 1.0f, options, 0.2), "outlineColor", 3), 0.0f);
    EXPECT_FLOAT_EQ(valueOf(PixelFxPlayer::sample(PixelFxEffect::OUTLINE_PULSE, 1.0f, options, 1.0), "outlineColor", 3), 0.0f);
}

TEST(PixelFxPlayer, EffectNamesRoundTrip){
    for (int i = 0; i < kPixelFxEffectCount; i++){
        PixelFxEffect effect;
        ASSERT_TRUE(pixelFxEffectFromName(pixelFxEffectName((PixelFxEffect)i), effect)) << i;
        EXPECT_EQ((int)effect, i);
    }
    PixelFxEffect effect;
    EXPECT_FALSE(pixelFxEffectFromName("melt", effect));
}

// AC21: the ember texels are exactly the opaque ones whose key the front crossed between the two
// progresses (the shader's own test, pfxDissolveTexel), and a whole run burns every opaque texel once
TEST(PixelFxPlayer, EmberTexelsAreWhereTheFrontBurns){
    const PixelFxFrameAlpha frame = halfFrame();
    PfxDissolve dissolve;
    dissolve.seed = 7;
    std::vector<int> burnt((size_t)kSide * kSide, 0);
    float previous = 0.0f;
    for (int step = 1; step <= 30; step++){
        const float progress = step == 30 ? 1.0f : (float)step / 30.0f;
        const std::vector<PixelFxTexel> texels = PixelFxPlayer::emberTexels(frame, dissolve, previous, progress);
        std::set<int> got;
        for (const PixelFxTexel& t : texels){
            got.insert(t.row * kSide + t.x);
            burnt[(size_t)t.row * kSide + t.x]++;
            EXPECT_TRUE(frame.opaque[(size_t)t.row * kSide + t.x]) << "a transparent texel " << t.x << ", " << t.row;
        }
        for (int row = 0; row < kSide; row++){
            for (int x = 0; x < kSide; x++){
                const int y = kSide - 1 - row;
                const bool crossed = pfxDissolveTexel(x, y, kSide, kSide, dissolve, previous) >= 0.0f &&
                                     pfxDissolveTexel(x, y, kSide, kSide, dissolve, progress) < 0.0f;
                EXPECT_EQ(got.count(row * kSide + x) == 1, crossed && frame.opaque[(size_t)row * kSide + x] != 0)
                    << "texel " << x << ", " << row << " from " << previous << " to " << progress;
            }
        }
        previous = progress;
    }
    for (int i = 0; i < kSide * kSide; i++) EXPECT_EQ(burnt[(size_t)i], frame.opaque[(size_t)i] ? 1 : 0) << i;
}

// AC21 on a sprite: the embers emitAt receives each frame sit on the texels the front crossed,
// at their centres on the sprite's quad
TEST_F(PixelFxPlayerScene, EmbersOnFront){
    const Entity e = sprite({"DISSOLVE"});
    struct Emitted { std::string effect; std::vector<Vector3> positions; };
    std::vector<Emitted> emitted;
    PixelFxPlayer::setEmberEmitter([&](Scene* s, const std::string& effect, const std::vector<Vector3>& positions){
        EXPECT_EQ(s, &scene);
        emitted.push_back({effect, positions});
    });
    PixelFxOptions options;
    options.embers = "assets/fx/ember.particles";
    options.emberRate = 1e6f;   // every crossing texel: the test checks positions, not the budget
    PixelFxHandle handle = PixelFxPlayer::play(&scene, e, PixelFxEffect::DISSOLVE, 0.5f, options);
    ASSERT_TRUE(handle.isValid());
    PfxDissolve dissolve;   // the manifest's defaults: the material sets none
    const PixelFxFrameAlpha frame = halfFrame();
    float previous = 0.0f;
    int frames = 0;
    size_t total = 0;
    while (!handle.isDone() && frames < 100){
        emitted.clear();
        PixelFxPlayer::update(kDt, kDt);
        frames++;
        const float progress = param(e, "dissolveProgress").x;
        const std::vector<PixelFxTexel> expected = PixelFxPlayer::emberTexels(frame, dissolve, previous, progress);
        std::vector<Vector3> want;
        for (const PixelFxTexel& t : expected)
            want.push_back(Vector3(-0.08f + ((float)t.x + 0.5f) / kSide * 0.16f, -0.08f + (1.0f - ((float)t.row + 0.5f) / kSide) * 0.16f, 0.0f));
        if (want.empty()){
            EXPECT_TRUE(emitted.empty()) << "frame " << frames;
        }else{
            ASSERT_EQ(emitted.size(), 1u) << "frame " << frames;
            EXPECT_EQ(emitted[0].effect, "assets/fx/ember.particles");
            ASSERT_EQ(emitted[0].positions.size(), want.size()) << "frame " << frames;
            for (size_t i = 0; i < want.size(); i++){
                EXPECT_NEAR(emitted[0].positions[i].x, want[i].x, 1e-5f) << "frame " << frames << " ember " << i;
                EXPECT_NEAR(emitted[0].positions[i].y, want[i].y, 1e-5f) << "frame " << frames << " ember " << i;
                // a left-half texel: the transparent half never burns
                EXPECT_LT(emitted[0].positions[i].x, 0.0f);
            }
            total += want.size();
        }
        previous = progress;
    }
    EXPECT_TRUE(handle.isDone());
    EXPECT_EQ(total, (size_t)(kSide * kSide / 2)) << "every opaque texel once";
    EXPECT_FLOAT_EQ(param(e, "dissolveProgress").x, 1.0f) << "dissolve ends gone";
}

// AC20: game time, frozen by a hit-stop unless Hit Stop Exempt, or unscaled; it ends at the
// neutral value with onFinished once
TEST_F(PixelFxPlayerScene, RunsOnGameTimeAndEndsAtNeutral){
    const Entity e = sprite({"FLASH"});
    int finished = 0;
    PixelFxOptions options;
    options.onFinished = [&](){ finished++; };
    PixelFxHandle handle = PixelFxPlayer::play(&scene, e, PixelFxEffect::FLASH, 0.1f, options);
    ASSERT_TRUE(handle.isValid());
    EXPECT_FLOAT_EQ(param(e, "flashAmount").x, 1.0f);
    // frozen: a hit-stop holds it
    HitStop::play(1.0f);
    HitStop::beginFrame(kDt, kDt);
    PixelFxPlayer::update(0.0, kDt);
    EXPECT_FLOAT_EQ(handle.getProgress(), 0.0f);
    // exempt: it runs on the unfrozen delta
    scene.addComponent<HitStopExemptComponent>(e, HitStopExemptComponent());
    HitStop::beginFrame(kDt, kDt);
    PixelFxPlayer::update(0.0, kDt);
    EXPECT_NEAR(handle.getProgress(), (float)(kDt / 0.1), 1e-4f);
    HitStop::reset();
    for (int i = 0; i < 10; i++) PixelFxPlayer::update(kDt, kDt);
    EXPECT_TRUE(handle.isDone());
    EXPECT_EQ(finished, 1);
    EXPECT_FLOAT_EQ(param(e, "flashAmount").x, 0.0f);
    EXPECT_EQ(PixelFxPlayer::getActiveCount(), 0u);

    // unscaled: the game delta does not move it
    options.unscaled = true;
    options.onFinished = nullptr;
    handle = PixelFxPlayer::play(&scene, e, PixelFxEffect::FLASH, 0.1f, options);
    PixelFxPlayer::update(0.0, 0.05);
    EXPECT_NEAR(handle.getProgress(), 0.5f, 1e-4f);
}

// R25: a missing feature is turned on in the GameObject's material; the effect waits for the variant
// (progress 0) and starts once it draws
TEST_F(PixelFxPlayerScene, MissingFeatureWaitsForItsVariant){
    const Entity e = sprite({});
    PixelFxHandle handle = PixelFxPlayer::play(&scene, e, PixelFxEffect::FADE_OUT, 0.2f);
    ASSERT_TRUE(handle.isValid());
    EXPECT_TRUE(hasFeature(e, "DITHER_FADE"));
    EXPECT_TRUE(scene.getComponent<MeshComponent>(e).needReload);
    for (int i = 0; i < 5; i++) PixelFxPlayer::update(kDt, kDt);
    EXPECT_FLOAT_EQ(handle.getProgress(), 0.0f) << "waits while the variant builds";
    loadVariant(e);
    PixelFxPlayer::update(kDt, kDt);   // ready: counts from zero
    PixelFxPlayer::update(kDt, kDt);
    EXPECT_NEAR(handle.getProgress(), (float)(kDt / 0.2), 1e-4f);
}

// one effect per feature: a new one replaces the running one (no onFinished); stop() ends at 0;
// set() writes a channel and stops that feature's effect
TEST_F(PixelFxPlayerScene, ReplaceStopAndSet){
    const Entity e = sprite({"DITHER_FADE"});
    int finished = 0;
    PixelFxOptions options;
    options.onFinished = [&](){ finished++; };
    PixelFxHandle first = PixelFxPlayer::play(&scene, e, PixelFxEffect::FADE_OUT, 1.0f, options);
    PixelFxPlayer::update(0.5, 0.5);
    PixelFxHandle second = PixelFxPlayer::play(&scene, e, PixelFxEffect::BLINK, 1.0f);
    EXPECT_TRUE(first.isDone());
    EXPECT_FALSE(second.isDone());
    second.stop();
    EXPECT_TRUE(second.isDone());
    EXPECT_FLOAT_EQ(param(e, "ditherFadeAmount").x, 0.0f);
    EXPECT_EQ(finished, 0);

    PixelFxHandle third = PixelFxPlayer::play(&scene, e, PixelFxEffect::FADE_OUT, 1.0f);
    std::string error;
    ASSERT_TRUE(PixelFxPlayer::set(&scene, e, "fade", Vector4(0.25f, 0.0f, 0.0f, 0.0f), &error)) << error;
    EXPECT_TRUE(third.isDone());
    EXPECT_FLOAT_EQ(param(e, "ditherFadeAmount").x, 0.25f);
    ASSERT_TRUE(PixelFxPlayer::set(&scene, e, "dissolveSeed", Vector4(3.0f, 0.0f, 0.0f, 0.0f), &error)) << error;
    EXPECT_TRUE(hasFeature(e, "DISSOLVE"));
    EXPECT_FALSE(PixelFxPlayer::set(&scene, e, "melt", Vector4(), &error));
    EXPECT_NE(error.find("channel"), std::string::npos) << error;

    // a GameObject without a library:sprite material is refused with the fix
    const Entity plain = scene.createEntity();
    scene.addComponent<Transform>(plain, Transform());
    EXPECT_FALSE(PixelFxPlayer::play(&scene, plain, PixelFxEffect::FLASH, 0.1f, PixelFxOptions(), &error).isValid());
    EXPECT_NE(error.find("library:sprite"), std::string::npos) << error;
}
