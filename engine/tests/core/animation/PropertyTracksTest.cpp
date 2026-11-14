// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Clip property tracks (anim-properties spec 15, tests 1-12): IO of every kind, the registry,
// discrete GameObject active with events after the values, sRGB colour fades and swizzles,
// blending with the rest, layers, Lua script fields, sprite frames, missing components binding
// later, self deactivation, edit-mode preview restore, Hermite tangents, the port of the game's
// GameOverPanel clips, and the performance budget.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/KeyframeSampling.h"
#include "animation/PropertyBindings.h"
#include "component/CanvasGroupComponent.h"
#include "component/Joint2DComponent.h"
#include "component/LatticeComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/TrailRendererComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/ScriptComponent.h"
#include "component/SpriteComponent.h"
#include "component/TextEffectComponent.h"
#include "component/UIComponent.h"
#include "object/AnimationPlayer.h"
#include "object/Light.h"
#include "pool/AnimationClipPool.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/UISystem.h"
#include "util/Color.h"

#include "lua.hpp"
#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <thread>

using namespace jasperengine;

namespace {

    ClipKey key(float time, std::vector<float> value, EaseType ease = EaseType::LINEAR){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        k.ease = ease;
        return k;
    }

    ClipTrackData property(const std::string& path, const std::string& component, const std::string& name, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Property;
        t.component = component;
        t.property = name;
        t.keys = std::move(keys);
        return t;
    }

    ClipEventData event(float time, const std::string& function){
        ClipEventData e;
        e.time = time;
        e.function = function;
        return e;
    }

    bool hasWarning(const std::vector<ClipWarning>& warnings, const std::string& part){
        for (const ClipWarning& warning : warnings){
            if (warning.message.find(part) != std::string::npos) return true;
        }
        return false;
    }

    std::string clipText(const std::string& tracks){
        return "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  \"frame_rate\": 30,\n  \"wrap\": \"once\",\n"
               "  \"tracks\": [\n" + tracks + "\n  ],\n  \"events\": []\n}\n";
    }

    class Properties : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            prefix = "properties/r" + std::to_string(run++) + "/";
            AnimationClipPool::clear();
            root = scene.createEntity();
            scene.setEntityName(root, "Panel");
            scene.addComponent<Transform>(root, {});
        }
        void TearDown() override { AnimationClipPool::clear(); }

        Entity child(Entity parent, const std::string& name){
            Entity entity = scene.createEntity();
            scene.setEntityName(entity, name);
            scene.addComponent<Transform>(entity, Transform());
            scene.addEntityChild(parent, entity, false);
            return entity;
        }

        Entity image(Entity parent, const std::string& name, Vector4 srgb){
            Entity entity = child(parent, name);
            UIComponent ui;
            ui.color = Color::sRGBToLinear(srgb);
            scene.addComponent<UIComponent>(entity, ui);
            scene.addComponent<RectTransformComponent>(entity, RectTransformComponent());
            return entity;
        }

        Entity light(Entity parent, const std::string& name, float intensity){
            Light object(&scene);
            object.setEntityOwned(false);
            const Entity entity = object.getEntity();
            scene.setEntityName(entity, name);
            scene.addEntityChild(parent, entity, false);
            scene.getComponent<LightComponent>(entity).intensity = intensity;
            return entity;
        }

        std::string clip(const std::string& name, const ClipData& data){
            animtest::writeAsset(prefix + name + ".anim", anim::writeClip(data));
            return prefix + name + ".anim";
        }

        AnimationPlayer player(std::vector<std::pair<std::string, ClipData>> clips, Entity on = NULL_ENTITY){
            AnimationPlayer object(&scene, on == NULL_ENTITY ? root : on);
            for (auto& entry : clips) object.addClip(clip(entry.first, entry.second), entry.first);
            object.setPlayAutomatically(false);
            return object;
        }

        void step(double dt, int count = 1){
            for (int i = 0; i < count; i++) scene.getSystem<ActionSystem>()->update(dt);
        }

        Vector4 color(Entity entity){ return scene.getComponent<UIComponent>(entity).color; }

        Scene scene;
        std::string prefix;
        Entity root = NULL_ENTITY;
    };

}

// ---- the registry ----

TEST(PropertyRegistry, ResolvesNamesSwizzlesAndSuggestions){
    anim::PropertyRef ref;
    ASSERT_TRUE(anim::parsePropertyRef("ui", "color", ref));
    EXPECT_EQ(ref.kind, PropertyKind::Color4);
    EXPECT_EQ(ref.display, PropertyDisplay::SRGB);
    EXPECT_EQ(ref.swizzle, -1);
    ASSERT_TRUE(anim::parsePropertyRef("ui", "color.a", ref));
    EXPECT_EQ(ref.swizzle, 3);
    EXPECT_EQ(ref.base, "color");
    ASSERT_TRUE(anim::parsePropertyRef("gameobject", "active", ref));
    EXPECT_TRUE(ref.discrete);
    EXPECT_TRUE(ref.deferred);
    ASSERT_TRUE(anim::parsePropertyRef("mesh", "submeshes[2].material.baseColorFactor", ref));
    EXPECT_EQ(ref.params.submesh, 2);
    ASSERT_TRUE(anim::parsePropertyRef("mesh", "submeshes[0].material.shaderParams.glow", ref));
    EXPECT_EQ(ref.params.member, "glow");
    EXPECT_EQ(ref.kind, PropertyKind::None) << "a shader member's kind comes from its manifest or keys";
    ASSERT_TRUE(anim::parsePropertyRef("recttransform", "positionOffset.y", ref));
    EXPECT_EQ(ref.swizzle, 1);
    ASSERT_TRUE(anim::parsePropertyRef("recttransform", "width", ref));
    EXPECT_EQ(ref.kind, PropertyKind::Int);
    EXPECT_TRUE(ref.interpolateInt);
    EXPECT_FALSE(ref.discrete);
    ASSERT_TRUE(anim::parsePropertyRef("canvasgroup", "alpha", ref));
    EXPECT_EQ(ref.kind, PropertyKind::Float);
    ASSERT_TRUE(anim::parsePropertyRef("canvasgroup", "interactable", ref));
    EXPECT_TRUE(ref.discrete);
    ASSERT_TRUE(anim::parsePropertyRef("script:Torch", "glow", ref));
    EXPECT_TRUE(ref.script);
    EXPECT_EQ(ref.params.scriptClass, "Torch");
    ASSERT_TRUE(anim::parsePropertyRef("script:Torch", "enabled", ref));
    EXPECT_TRUE(ref.deferred);

    EXPECT_FALSE(anim::parsePropertyRef("light", "fooBar", ref));
    EXPECT_NE(ref.error.find("unknown animatable property 'light.fooBar'"), std::string::npos) << ref.error;
    EXPECT_NE(ref.error.find("did you mean"), std::string::npos) << ref.error;
    EXPECT_FALSE(anim::parsePropertyRef("light", "intensity.x", ref)) << "a float has no swizzle";
    EXPECT_FALSE(anim::parsePropertyRef("animationplayer", "enabled", ref)) << "the player's own enabled is not animatable";
    EXPECT_FALSE(anim::parsePropertyRef("mesh", "castShadows", ref)) << "properties that reload GPU state are not in the registry";
    EXPECT_FALSE(anim::parsePropertyRef("script:Torch", "isActiveAndEnabled", ref));

    // P9: an sRGB key written as linear reads back the same
    float values[4] = {0.25f, 0.5f, 0.75f, 0.4f};
    anim::keyToStored(PropertyKind::Color4, PropertyDisplay::SRGB, false, values, 4);
    EXPECT_NEAR(values[1], Color::sRGBToLinear(Vector3(0.5f, 0.5f, 0.5f)).x, 1e-6f);
    EXPECT_FLOAT_EQ(values[3], 0.4f) << "alpha is not converted";
    anim::storedToKey(PropertyKind::Color4, PropertyDisplay::SRGB, values, 4);
    EXPECT_NEAR(values[0], 0.25f, 1e-5f);
    EXPECT_NEAR(values[2], 0.75f, 1e-5f);
    float cone[1] = {60.0f};
    anim::keyToStored(PropertyKind::Float, PropertyDisplay::ConeDegrees, false, cone, 1);
    EXPECT_NEAR(cone[0], std::cos(30.0 * 3.14159265358979 / 180.0), 1e-6f);
    anim::storedToKey(PropertyKind::Float, PropertyDisplay::ConeDegrees, cone, 1);
    EXPECT_NEAR(cone[0], 60.0f, 1e-3f);
}

// ---- 1. IO ----

TEST(PropertyClipIO, EveryKindParsesAndWritesCanonically){
    const std::string text = clipText(
        "    {\"path\": \"Panel/Stats/puntaje\", \"channel\": \"property\", \"component\": \"gameobject\", \"property\": \"active\", \"keys\": [[0, false], [0.5, true]]},\n"
        "    {\"path\": \"Fade\", \"channel\": \"property\", \"component\": \"ui\", \"property\": \"color.a\", \"keys\": [{\"time\": 0, \"value\": 0, \"in\": 0, \"out\": 0, \"tangent_mode\": \"auto\"}, {\"time\": 1.5, \"value\": 1, \"in\": 0, \"out\": 0, \"tangent_mode\": \"auto\"}]},\n"
        "    {\"path\": \"Panel/Vignette\", \"channel\": \"property\", \"component\": \"ui\", \"property\": \"color\", \"keys\": [[0, [1, 0, 0, 1]], [0.1, \"#330000FF\", \"step\"]]},\n"
        "    {\"path\": \"\", \"channel\": \"property\", \"component\": \"script:Torch\", \"property\": \"glow\", \"keys\": [[0, 0.2], [0.5, 1, \"sine_in_out\"]]},\n"
        "    {\"path\": \"Ship\", \"channel\": \"property\", \"component\": \"sprite\", \"property\": \"frame\", \"keys\": [[0, 0], [0.083, 1], [0.166, 2]]},\n"
        "    {\"path\": \"Box\", \"channel\": \"property\", \"component\": \"recttransform\", \"property\": \"positionOffset\", \"keys\": [[0, [0, 150]], [0.25, [0, -70]]]},\n"
        "    {\"path\": \"Box\", \"channel\": \"property\", \"component\": \"recttransform\", \"property\": \"width\", \"keys\": [[0, 100], [1, 200.4]]},\n"
        "    {\"path\": \"Lamp\", \"channel\": \"property\", \"component\": \"light\", \"property\": \"color\", \"keys\": [[0, \"#FF8000\"]]}");
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    const ClipData& data = *parsed.data;
    ASSERT_EQ(data.tracks.size(), 8u);
    // sorted by path, then (component, property)
    std::vector<std::string> order;
    for (const ClipTrackData& t : data.tracks) order.push_back(t.path + ":" + t.component + "." + t.property);
    EXPECT_EQ(order, (std::vector<std::string>{":script:Torch.glow", "Box:recttransform.positionOffset", "Box:recttransform.width",
                                               "Fade:ui.color.a", "Lamp:light.color", "Panel/Stats/puntaje:gameobject.active",
                                               "Panel/Vignette:ui.color", "Ship:sprite.frame"}));
    for (const ClipTrackData& t : data.tracks) EXPECT_TRUE(t.known) << t.component << "." << t.property;
    const ClipTrackData& glow = data.tracks[0];
    EXPECT_EQ(glow.kind, PropertyKind::Float);
    EXPECT_EQ(glow.keys[1].ease, EaseType::SINE_IN_OUT);
    const ClipTrackData& width = data.tracks[2];
    EXPECT_EQ(width.kind, PropertyKind::Int);
    EXPECT_FLOAT_EQ(width.keys[1].value[0], 200.0f) << "a fractional int rounds";
    EXPECT_TRUE(hasWarning(parsed.warnings, "not a whole number"));
    const ClipTrackData& alpha = data.tracks[3];
    EXPECT_EQ(alpha.width, 1u);
    EXPECT_EQ(alpha.swizzle, 3);
    EXPECT_EQ(alpha.keys[0].tangentMode, ClipTangentMode::Auto);
    const ClipTrackData& lamp = data.tracks[4];
    ASSERT_EQ(lamp.keys[0].value.size(), 3u);
    EXPECT_NEAR(lamp.keys[0].value[1], 128.0f / 255.0f, 1e-6f);
    const ClipTrackData& active = data.tracks[5];
    EXPECT_EQ(active.kind, PropertyKind::Bool);
    EXPECT_TRUE(active.discrete);
    EXPECT_FLOAT_EQ(active.keys[1].value[0], 1.0f);
    const ClipTrackData& vignette = data.tracks[6];
    EXPECT_NEAR(vignette.keys[1].value[0], 0.2f, 1e-6f);
    EXPECT_EQ(vignette.keys[1].ease, EaseType::STEP);
    EXPECT_EQ(data.tracks[7].kind, PropertyKind::Frame);
    EXPECT_NEAR(data.effectiveLength(), 1.5f, 1e-6f) << "property tracks count toward the length";

    const std::string written = anim::writeClip(data);
    EXPECT_NE(written.find("{\"path\": \"Panel/Stats/puntaje\", \"channel\": \"property\", \"component\": \"gameobject\", \"property\": \"active\", \"keys\": [\n"
                           "      [0, false],\n      [0.5, true]\n    ]}"), std::string::npos) << written;
    EXPECT_NE(written.find("{\"time\": 0, \"value\": 0, \"in\": 0, \"out\": 0, \"tangent_mode\": \"auto\"}"), std::string::npos) << written;
    EXPECT_NE(written.find("[0.1, [0.2, 0, 0, 1], \"step\"]"), std::string::npos) << "colours are written as arrays";
    EXPECT_NE(written.find("[1, 200]"), std::string::npos) << "ints without decimals";
    EXPECT_NE(written.find("[0.166, 2]"), std::string::npos);
    // canonical: the same bytes again, and a fixed point
    EXPECT_EQ(anim::writeClip(data), written);
    anim::ClipParseResult again = anim::parseClip(written);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(anim::writeClip(*again.data), written);
}

TEST(PropertyClipIO, HexColoursEasesOnBoolsAndUnknownTracks){
    anim::ClipParseResult parsed = anim::parseClip(clipText(
        "    {\"path\": \"A\", \"channel\": \"property\", \"component\": \"ui\", \"property\": \"color\", \"keys\": [[0, \"#FF000080\"]]},\n"
        "    {\"path\": \"A\", \"channel\": \"property\", \"component\": \"gameobject\", \"property\": \"active\", \"keys\": [[0, 0], [1, 1, \"quad_out\"], [2, false, \"cubic_in\"]]}"));
    ASSERT_TRUE(parsed.ok());
    const ClipData& data = *parsed.data;
    ASSERT_EQ(data.tracks.size(), 2u);
    const ClipTrackData& colour = data.tracks[1];
    ASSERT_EQ(colour.component, "ui");
    ASSERT_EQ(colour.keys[0].value.size(), 4u);
    EXPECT_FLOAT_EQ(colour.keys[0].value[0], 1.0f);
    EXPECT_FLOAT_EQ(colour.keys[0].value[1], 0.0f);
    EXPECT_NEAR(colour.keys[0].value[3], 0.502f, 1e-3f);
    const ClipTrackData& active = data.tracks[0];
    EXPECT_EQ(active.keys[1].ease, EaseType::LINEAR) << "an ease on a bool is ignored";
    size_t easeWarnings = 0;
    for (const ClipWarning& warning : parsed.warnings){
        if (warning.message.find("properties step") != std::string::npos) easeWarnings++;
    }
    EXPECT_EQ(easeWarnings, 1u) << "one warning per track";

    // a v1 file with tracks this engine cannot animate round-trips byte-identically
    const std::string unknown = clipText(
        "    {\"path\":\"Fx\",\"channel\":\"property\",\"component\":\"light\",\"property\":\"fooBar\",\"keys\":[[0,{\"x\":1}]],\"extra\":true},\n"
        "    {\"path\":\"Fx\",\"channel\":\"property\",\"component\":\"particlecollider\",\"property\":\"bounce\",\"keys\":[[0,1]]}");
    anim::ClipParseResult kept = anim::parseClip(unknown);
    ASSERT_TRUE(kept.ok());
    EXPECT_TRUE(hasWarning(kept.warnings, "unknown animatable property 'light.fooBar' (did you mean"));
    EXPECT_FALSE(kept.data->tracks[0].known);
    const std::string canonical = anim::writeClip(*kept.data);
    anim::ClipParseResult reread = anim::parseClip(canonical);
    ASSERT_TRUE(reread.ok());
    EXPECT_EQ(anim::writeClip(*reread.data), canonical);
    EXPECT_NE(canonical.find("\"extra\":true"), std::string::npos) << "kept verbatim";
    std::shared_ptr<const AnimationClip> built = AnimationClip::build(*kept.data);
    EXPECT_FALSE(built->getTracks()[0].isSampled());
    EXPECT_FALSE(built->hasPropertyTracks());
}

TEST(PropertyClipIO, TangentModesRoundTripOnEveryChannel){
    ClipData data;
    ClipTrackData position;
    position.path = "A";
    position.channel = ClipChannel::Position;
    ClipKey first = key(0, {0, 0, 0});
    first.inTangent = {0, 0, 0};
    first.outTangent = {1, 0, 0};
    first.tangentMode = ClipTangentMode::Free;
    ClipKey second = key(1, {1, 0, 0});
    second.inTangent = {1, 0, 0};
    second.outTangent = {1, 0, 0};
    second.tangentMode = ClipTangentMode::Broken;
    position.keys = {first, second};
    data.tracks.push_back(position);
    ClipKey linear = key(0.5f, {2, 0, 0});
    linear.tangentMode = ClipTangentMode::Linear;  // no tangents: not written
    data.tracks.push_back(position);
    data.tracks.back().channel = ClipChannel::Scale;
    data.tracks.back().keys.push_back(linear);
    const std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("\"tangent_mode\": \"free\""), std::string::npos);
    EXPECT_NE(text.find("\"tangent_mode\": \"broken\""), std::string::npos);
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->tracks[0].keys[0].tangentMode, ClipTangentMode::Free);
    EXPECT_EQ(parsed.data->tracks[0].keys[1].tangentMode, ClipTangentMode::Broken);
    EXPECT_EQ(anim::writeClip(*parsed.data), text);
}

// ---- 12. tangents (sampling) ----

TEST(PropertySampling, HermiteTangents){
    ClipData data;
    ClipKey a = key(0, {0});
    a.inTangent = {0};
    a.outTangent = {2};
    ClipKey b = key(1, {1});
    b.inTangent = {0};
    b.outTangent = {0};
    data.tracks.push_back(property("", "light", "intensity", {a, b}));
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    anim::TrackSample sample;
    ASSERT_TRUE(anim::sampleTrack(clip->getTracks()[0], 0.5f, sample));
    ASSERT_EQ(sample.weights.size(), 1u);
    EXPECT_NEAR(sample.weights[0], 0.75f, 1e-6f);

    // discrete: the key at or before the time (at a key time, that key)
    ClipData steps;
    steps.tracks.push_back(property("", "gameobject", "active", {key(0, {0}), key(0.5f, {1}), key(1, {0})}));
    std::shared_ptr<const AnimationClip> stepped = AnimationClip::build(steps);
    ASSERT_TRUE(anim::sampleTrack(stepped->getTracks()[0], 0.49f, sample));
    EXPECT_EQ(sample.weights[0], 0.0f);
    anim::sampleTrack(stepped->getTracks()[0], 0.5f, sample);
    EXPECT_EQ(sample.weights[0], 1.0f);
    anim::sampleTrack(stepped->getTracks()[0], 0.99f, sample);
    EXPECT_EQ(sample.weights[0], 1.0f);
}

// ---- 2. discrete active (the GameStatsShowing port) ----

TEST_F(Properties, DiscreteActiveStepsAndEventsSeeTheValues){
    Entity stats = child(root, "Stats");
    Entity a = child(stats, "puntaje");
    scene.setEntityActive(a, false);
    ClipData data;
    data.tracks.push_back(property("Stats/puntaje", "gameobject", "active", {key(0, {0}), key(0.5f, {1})}));
    data.events = {event(0.5f, "CountThisGameScore")};
    AnimationPlayer object = player({{"show", data}});
    bool sawActive = false;
    int calls = 0;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){
        calls++;
        if (e.functionName == "CountThisGameScore") sawActive = scene.isEntityActive(a);
    }));
    object.play("show");
    step(0.0);
    step(0.49);
    EXPECT_FALSE(scene.isEntityActive(a));
    step(0.01);
    EXPECT_TRUE(scene.isEntityActive(a)) << "the step rule: at the key time, the key's value";
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(sawActive) << "events are dispatched after the values are written";
    EXPECT_EQ(object.getAnimatedProperties("show"), (std::vector<std::string>{"Stats/puntaje:gameobject.active"}));
    object.getOnEvent().clear();
}

// ---- 3. colour fade (swizzle) and 4. sRGB interpolation ----

TEST_F(Properties, AlphaFadeTouchesOnlyAlpha){
    Entity fade = image(root, "Fade", Vector4(1.0f, 0.5f, 0.2f, 0.0f));
    ClipData data;
    data.tracks.push_back(property("Fade", "ui", "color.a", {key(0, {0}), key(1.5f, {1})}));
    AnimationPlayer object = player({{"fadeIn", data}});
    object.play("fadeIn");
    step(0.0);
    step(0.75);
    const Vector4 expected = Color::sRGBToLinear(Vector4(1.0f, 0.5f, 0.2f, 0.5f));
    EXPECT_NEAR(color(fade).x, expected.x, 1e-5f);
    EXPECT_NEAR(color(fade).y, expected.y, 1e-5f);
    EXPECT_NEAR(color(fade).z, expected.z, 1e-5f);
    EXPECT_NEAR(color(fade).w, 0.5f, 1e-5f);
}

TEST_F(Properties, ColoursInterpolateInSrgb){
    Entity panel = image(root, "Vignette", Vector4(0, 0, 0, 1));
    ClipData data;
    data.tracks.push_back(property("Vignette", "ui", "color", {key(0, {0, 0, 0, 1}), key(1, {1, 1, 1, 1})}));
    AnimationPlayer object = player({{"flash", data}});
    object.play("flash");
    step(0.0);
    step(0.5);
    // the engine's sRGB curve (Color::sRGBToLinear, gamma 2.2) of the key-space midpoint
    EXPECT_NEAR(color(panel).x, Color::sRGBToLinear(Vector3(0.5f, 0.5f, 0.5f)).x, 1e-4f);
    EXPECT_GT(std::fabs(color(panel).x - 0.5f), 0.2f) << "not interpolated in linear space";
}

// ---- 5. blend and rest ----

TEST_F(Properties, BlendsWithTheRest){
    Entity lamp = light(root, "Lamp", 1.0f);
    Entity box = child(root, "Box");
    scene.setEntityActive(box, true);
    ClipData data;
    data.wrap = ClipWrapMode::ClampForever;
    data.tracks.push_back(property("Lamp", "light", "intensity", {key(0, {4}), key(1, {4})}));
    data.tracks.push_back(property("Box", "gameobject", "active", {key(0, {0})}));
    AnimationPlayer object = player({{"glow", data}});
    object.blend("glow", 0.25f, 0.0f);
    step(0.0);
    step(0.1);
    EXPECT_NEAR(scene.getComponent<LightComponent>(lamp).intensity, 1.75f, 1e-6f);
    EXPECT_TRUE(scene.isEntityActive(box)) << "a discrete value at weight 0.25: the rest (0.75) wins";
    object.blend("glow", 0.6f, 0.0f);
    step(0.1);
    EXPECT_NEAR(scene.getComponent<LightComponent>(lamp).intensity, 1.0f * 0.4f + 4.0f * 0.6f, 1e-5f);
    EXPECT_FALSE(scene.isEntityActive(box)) << "at weight 0.6 the clip wins";
}

// P1: in a crossfade the discrete value flips at the midpoint (ties: the incoming state)
TEST_F(Properties, CrossfadeFlipsDiscreteValuesAtTheMidpoint){
    Entity box = child(root, "Box");
    ClipData on;
    on.wrap = ClipWrapMode::Loop;
    on.length = 2.0f;
    on.tracks.push_back(property("Box", "gameobject", "active", {key(0, {1})}));
    ClipData off = on;
    off.tracks[0].keys[0].value = {0};
    AnimationPlayer object = player({{"on", on}, {"off", off}});
    object.play("on");
    step(0.0);
    EXPECT_TRUE(scene.isEntityActive(box));
    object.crossFade("off", 1.0f);
    step(0.4);
    EXPECT_TRUE(scene.isEntityActive(box));
    step(0.1);
    EXPECT_FALSE(scene.isEntityActive(box)) << "0.5 / 0.5: the later (incoming) state wins";
}

// ---- 6. layers ----

TEST_F(Properties, LayersOverrideAndAdd){
    Entity lamp = light(root, "Lamp", 0.0f);
    ClipData base;
    base.wrap = ClipWrapMode::Loop;
    base.tracks.push_back(property("Lamp", "light", "intensity", {key(0, {1}), key(1, {1})}));
    ClipData over = base;
    over.tracks[0].keys = {key(0, {3}), key(1, {3})};
    AnimationPlayer object(&scene, root);
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    AnimationPlayerClip baseEntry;
    baseEntry.clip = clip("base", base);
    baseEntry.name = "base";
    AnimationPlayerClip overEntry;
    overEntry.clip = clip("over", over);
    overEntry.name = "over";
    overEntry.layer = 1;
    component.clips = {baseEntry, overEntry};
    AnimationPlayerLayer layer0;
    layer0.name = anim::BASE_LAYER_NAME;
    AnimationPlayerLayer layer1;
    layer1.name = "Flicker";
    layer1.weight = 0.5f;
    component.layers = {layer0, layer1};
    component.playAutomatically = false;
    component.needsLoad = true;
    object.play("base");
    object.play("over");
    step(0.0);
    step(0.1);
    EXPECT_NEAR(scene.getComponent<LightComponent>(lamp).intensity, 2.0f, 1e-5f) << "lerp(1, 3, 0.5)";

    // additive: v - v_ref added, from the clip's additive_reference (0: v_ref 3)
    ClipData add = over;
    add.tracks[0].keys = {key(0, {3}), key(1, {5})};
    AnimationClipPool::invalidate(clip("over", add));
    object.setLayerBlendMode("Flicker", "additive");
    object.setLayerWeight("Flicker", 1.0f);
    object.getState("over").setTime(0.5f);
    step(0.0);
    EXPECT_NEAR(scene.getComponent<LightComponent>(lamp).intensity, 1.0f + (4.0f - 3.0f), 1e-4f);
}

// ---- 7. script fields ----

TEST_F(Properties, LuaScriptFields){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L, "torchInstance = {glow = 0, count = 0, offset = Vector3(0, 0, 0)}"), LUA_OK);
    lua_getglobal(L, "torchInstance");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.enabled = true;
    entry.className = "Torch";
    entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    ScriptProperty glow;
    glow.name = "glow";
    glow.type = ScriptPropertyType::Float;
    glow.value = 0.0f;
    ScriptProperty count;
    count.name = "count";
    count.type = ScriptPropertyType::Int;
    count.value = 0;
    ScriptProperty offset;
    offset.name = "offset";
    offset.type = ScriptPropertyType::Vector3;
    offset.value = Vector3(0, 0, 0);
    entry.properties = {glow, count, offset};
    scripts.scripts.push_back(entry);
    scene.addComponent<ScriptComponent>(root, scripts);

    ClipData data;
    data.tracks.push_back(property("", "script:Torch", "glow", {key(0, {0}), key(1, {1})}));
    data.tracks.push_back(property("", "script:Torch", "count", {key(0, {0}), key(1, {4})}));
    data.tracks.push_back(property("", "script:Torch", "offset", {key(0, {0, 0, 0}), key(1, {2, 4, 6})}));
    AnimationPlayer object = player({{"flicker", data}});
    object.play("flicker");
    step(0.0);
    ASSERT_EQ(luaL_dostring(L, "torchFirst = torchInstance.offset"), LUA_OK);
    step(0.5);
    ASSERT_EQ(luaL_dostring(L, "torchResult = string.format('%.6f|%s|%s|%.3f', torchInstance.glow, math.type(torchInstance.count), "
                               "tostring(torchInstance.count), torchInstance.offset.y) .. '|' .. tostring(torchFirst.y) .. '|' .. "
                               "tostring(rawequal(torchFirst, torchInstance.offset))"), LUA_OK);
    lua_getglobal(L, "torchResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    // an Int field rounds and steps (the declared type wins over the keys'): 0 until its key at 1
    EXPECT_EQ(result, "0.500000|integer|0|2.000|0.0|false") << "a fresh Vector3 per change";
    const ScriptComponent& after = scene.getComponent<ScriptComponent>(root);
    EXPECT_NEAR(std::get<float>(after.scripts[0].properties[0].value), 0.5f, 1e-6f) << "ScriptProperty::value follows (Inspector)";
    step(0.5);
    EXPECT_EQ(std::get<int>(after.scripts[0].properties[1].value), 4);
    scene.getComponent<ScriptComponent>(root).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// ---- 8. sprite frame ----

TEST_F(Properties, SpriteFrames){
    Entity ship = child(root, "Ship");
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    scene.addComponent<MeshComponent>(ship, mesh);
    SpriteComponent sprite;
    for (int i = 0; i < 3; i++){
        SpriteFrameData frame;
        frame.name = "f" + std::to_string(i);
        frame.rect = Rect(0.25f * (float)i, 0.0f, 0.25f, 1.0f);
        sprite.framesRect[(size_t)i] = frame;
    }
    sprite.numFramesRect = 3;
    scene.addComponent<SpriteComponent>(ship, sprite);
    ClipData data;
    data.tracks.push_back(property("Ship", "sprite", "frame", {key(0, {0}), key(0.083f, {1}), key(0.166f, {2})}));
    AnimationPlayer object = player({{"falling", data}});
    object.play("falling");
    step(0.0);
    step(0.1);
    const Rect& rect = scene.getComponent<MeshComponent>(ship).submeshes[0].textureRect;
    EXPECT_NEAR(rect.getX(), 0.25f, 1e-6f);
    EXPECT_NEAR(rect.getWidth(), 0.25f, 1e-6f);
    float frame = -2.0f;
    anim::PropertyRef ref;
    anim::parsePropertyRef("sprite", "frame", ref);
    anim::PropertyBinding binding = anim::bindProperty(&scene, ship, ref, 1);
    ASSERT_EQ(binding.status, anim::PropertyBindStatus::Bound);
    ASSERT_TRUE(anim::readPropertyKeySpace(&scene, ship, binding, &frame));
    EXPECT_EQ(frame, 1.0f);
}

// ---- 9. missing component ----

TEST_F(Properties, MissingComponentsBindWhenAdded){
    Entity panel = child(root, "Panel");
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks.push_back(property("Panel", "canvasgroup", "alpha", {key(0, {0.25f}), key(1, {0.25f})}));
    AnimationPlayer object = player({{"fade", data}});
    object.play("fade");
    step(0.0);
    const std::vector<std::string> warnings = animtest::takeWarnings();
    bool reported = false;
    for (const std::string& warning : warnings) reported = reported || warning.find("no 'canvasgroup' component") != std::string::npos;
    EXPECT_TRUE(reported) << "the binding says why, once";
    EXPECT_EQ(object.getStateInfo("fade").boundTracks, 0u);
    step(0.1);
    EXPECT_TRUE(animtest::takeWarnings().empty()) << "not again while unchanged";
    scene.addComponent<CanvasGroupComponent>(panel, CanvasGroupComponent());
    step(0.1);
    step(0.1);
    step(0.1);
    EXPECT_NEAR(scene.getComponent<CanvasGroupComponent>(panel).alpha, 0.25f, 1e-6f) << "bound within one retry period (0.25 s)";
    EXPECT_EQ(object.getStateInfo("fade").boundTracks, 1u);
}

// ---- 10. self deactivation ----

TEST_F(Properties, ClipTurningOffItsRootStopsThePlayer){
    ClipData data;
    data.tracks.push_back(property("", "gameobject", "active", {key(0, {1}), key(0.5f, {0}), key(0.7f, {1})}));
    data.tracks.push_back(property("", "ui", "color.a", {key(0, {0}), key(1, {1})}));
    scene.addComponent<UIComponent>(root, UIComponent());
    AnimationPlayer object = player({{"blink", data}});
    object.play("blink");
    step(0.0);
    step(0.5);
    EXPECT_FALSE(scene.isEntityActive(root));
    step(0.25);
    EXPECT_NEAR(object.getStateInfo("blink").time, 0.5f, 1e-6f) << "inactive: time stops (R25)";
    EXPECT_NEAR(scene.getComponent<UIComponent>(root).color.w, 0.5f, 1e-5f) << "and nothing is written";
    scene.setEntityActive(root, true);
    step(0.25);
    EXPECT_NEAR(object.getStateInfo("blink").time, 0.75f, 1e-6f) << "resumes from its held time";
    EXPECT_TRUE(scene.isEntityActive(root)) << "keyed on again at 0.7";
    EXPECT_NEAR(scene.getComponent<UIComponent>(root).color.w, 0.75f, 1e-5f);
}

// ---- 11. preview restore ----

TEST_F(Properties, PreviewRestoresActiveAndColour){
    Entity box = image(root, "Box", Vector4(0.2f, 0.4f, 0.6f, 1.0f));
    const Vector4 before = color(box);
    ClipData data;
    data.tracks.push_back(property("Box", "gameobject", "active", {key(0, {0})}));
    data.tracks.push_back(property("Box", "ui", "color", {key(0, {1, 0, 0, 1}), key(1, {0, 1, 0, 0.5f})}));
    std::shared_ptr<const AnimationClip> clipObject = AnimationClip::build(data);
    ActionSystem* actions = scene.getSystem<ActionSystem>().get();
    anim::ClipPreviewSession session;
    ActionSystem::PlayerPreview preview;
    ActionSystem::PlayerPreviewState state;
    state.clip = clipObject;
    state.time = 0.5f;
    preview.states = {state};
    ActionSystem::PlayerPreviewResult result = actions->previewPlayer(root, preview, session);
    EXPECT_TRUE(result.ready);
    EXPECT_EQ(result.properties.size(), 2u);
    EXPECT_FALSE(scene.isEntityActive(box)) << "the edit-mode preview toggles the real flag";
    EXPECT_NE(color(box).x, before.x);
    actions->restorePlayerPreview(session);
    EXPECT_TRUE(scene.isEntityActive(box));
    EXPECT_FLOAT_EQ(color(box).x, before.x);
    EXPECT_FLOAT_EQ(color(box).y, before.y);
    EXPECT_FLOAT_EQ(color(box).z, before.z);
    EXPECT_FLOAT_EQ(color(box).w, before.w);
}

// the preview puts back the stored value exactly (not a key-space round trip: with gamma 2.2
// in float most linear values do not survive one), and a channel fade leaves the others' bits
TEST_F(Properties, PreviewRestoreAndUnkeyedComponentsAreExact){
    // linear values the sRGB round trip changes (found, not assumed)
    std::vector<float> lossy;
    for (int i = 1; i < 4000 && lossy.size() < 3; i++){
        float v[4] = {(float)i / 4001.0f, 0, 0, 1};
        const float original = v[0];
        anim::storedToKey(PropertyKind::Color4, PropertyDisplay::SRGB, v, 4);
        anim::keyToStored(PropertyKind::Color4, PropertyDisplay::SRGB, false, v, 4);
        if (v[0] != original) lossy.push_back(original);
    }
    ASSERT_EQ(lossy.size(), 3u) << "values that do not round-trip exist";
    Entity box = image(root, "Box", Vector4(1, 1, 1, 1));
    scene.getComponent<UIComponent>(box).color = Vector4(lossy[0], lossy[1], lossy[2], 0.25f);
    const Vector4 before = color(box);

    ClipData data;
    data.tracks.push_back(property("Box", "ui", "color", {key(0, {1, 0, 0, 1}), key(1, {0, 1, 0, 0.5f})}));
    std::shared_ptr<const AnimationClip> clipObject = AnimationClip::build(data);
    ActionSystem* actions = scene.getSystem<ActionSystem>().get();
    anim::ClipPreviewSession session;
    ActionSystem::PlayerPreview preview;
    ActionSystem::PlayerPreviewState state;
    state.clip = clipObject;
    state.time = 0.5f;
    preview.states = {state};
    actions->previewPlayer(root, preview, session);
    ASSERT_NE(color(box).x, before.x);
    actions->restorePlayerPreview(session);
    EXPECT_EQ(color(box).x, before.x) << "bit-exact";
    EXPECT_EQ(color(box).y, before.y);
    EXPECT_EQ(color(box).z, before.z);
    EXPECT_EQ(color(box).w, before.w);

    // an alpha fade in play: r, g, b never rewritten (not even 1 ULP)
    ClipData fade;
    fade.tracks.push_back(property("Box", "ui", "color.a", {key(0, {0}), key(1, {1})}));
    AnimationPlayer object = player({{"fade", fade}});
    object.play("fade");
    step(0.0);
    for (int i = 0; i < 5; i++){
        step(0.1);
        EXPECT_EQ(color(box).x, before.x);
        EXPECT_EQ(color(box).y, before.y);
        EXPECT_EQ(color(box).z, before.z);
    }
    EXPECT_NEAR(color(box).w, 0.5f, 1e-5f);
}

// RGB keys of a Color4 script field key an opaque alpha (spec 5.4)
TEST_F(Properties, RgbKeysOfAColor4ScriptFieldAreOpaque){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_EQ(luaL_dostring(L, "tintInstance = {tint = Vector4(0, 0, 0, 0)}"), LUA_OK);
    lua_getglobal(L, "tintInstance");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.enabled = true;
    entry.className = "Tint";
    entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    ScriptProperty tint;
    tint.name = "tint";
    tint.type = ScriptPropertyType::Color4;
    tint.value = Vector4(0, 0, 0, 0);
    entry.properties = {tint};
    scripts.scripts.push_back(entry);
    scene.addComponent<ScriptComponent>(root, scripts);
    // as a file has them: "#RRGGBB" keys (3 values)
    anim::ClipParseResult parsed = anim::parseClip(clipText(
        "    {\"path\": \"\", \"channel\": \"property\", \"component\": \"script:Tint\", \"property\": \"tint\", "
        "\"keys\": [[0, \"#FF0000\"], [1, \"#FF0000\"]]}"));
    ASSERT_TRUE(parsed.ok());
    AnimationPlayer object = player({{"tint", *parsed.data}});
    object.play("tint");
    step(0.0);
    step(0.5);
    const Vector4 value = std::get<Vector4>(scene.getComponent<ScriptComponent>(root).scripts[0].properties[0].value);
    EXPECT_NEAR(value.x, 1.0f, 1e-5f);
    EXPECT_NEAR(value.w, 1.0f, 1e-6f) << "alpha 1, not the field's 0";
    scene.getComponent<ScriptComponent>(root).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// two scripts of one class: a binding keeps the script it picked (keying enabled = false does not
// walk through the instances); a reload (another instance) binds again, its declared type too (P4)
TEST_F(Properties, ScriptBindingsKeepTheirInstanceAndRebindOnReload){
    lua_State* L = LuaBinding::getLuaState();
    // (torchC's glow starts as a float: only an Int binding writes it as an integer)
    ASSERT_EQ(luaL_dostring(L, "torchA = {glow = 0}; torchB = {glow = 0}; torchC = {glow = 0.25}"), LUA_OK);
    auto refOf = [&](const char* name){
        lua_getglobal(L, name);
        return luaL_ref(L, LUA_REGISTRYINDEX);
    };
    const int a = refOf("torchA"), b = refOf("torchB"), c = refOf("torchC");
    auto torch = [](int ref, ScriptPropertyType type){
        ScriptEntry entry;
        entry.type = ScriptType::LUA;
        entry.enabled = true;
        entry.className = "Torch";
        entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
        ScriptProperty glow;
        glow.name = "glow";
        glow.type = type;
        if (type == ScriptPropertyType::Int) glow.value = 0;
        else glow.value = 0.0f;
        entry.properties = {glow};
        return entry;
    };
    ScriptComponent scripts;
    scripts.scripts = {torch(a, ScriptPropertyType::Float), torch(b, ScriptPropertyType::Float)};
    scene.addComponent<ScriptComponent>(root, scripts);
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.length = 2.0f;
    data.tracks.push_back(property("", "script:Torch", "enabled", {key(0, {0})}));
    data.tracks.push_back(property("", "script:Torch", "glow", {key(0, {0}), key(1, {1})}));
    AnimationPlayer object = player({{"torch", data}});
    object.play("torch");
    for (int i = 0; i < 4; i++) step(i == 0 ? 0.0 : 0.05);
    const ScriptComponent& now = scene.getComponent<ScriptComponent>(root);
    EXPECT_FALSE(now.scripts[0].enabled) << "the picked script";
    EXPECT_TRUE(now.scripts[1].enabled) << "the other instance is never touched";
    // a reload: the first script's instance is another (enabled, as a reloaded script starts),
    // its glow now an Int; the stale binding would write 0.5 as a float into it
    scene.getComponent<ScriptComponent>(root).scripts[0] = torch(c, ScriptPropertyType::Int);
    object.getState("torch").setTime(0.5f);
    step(0.0);
    ASSERT_EQ(luaL_dostring(L, "torchResult = math.type(torchC.glow) .. '|' .. tostring(torchC.glow)"), LUA_OK);
    lua_getglobal(L, "torchResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "integer|0") << "bound again to the new instance with its declared Int (steps)";
    scene.getComponent<ScriptComponent>(root).scripts.clear();
    for (int ref : {a, b, c}) luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// enabled writes do what the components' Lua setters do; every Catalog checkbox has a row
TEST_F(Properties, EnabledWritesMirrorTheSetters){
    auto write = [&](Entity entity, const char* component, float value){
        anim::PropertyRef ref;
        ASSERT_TRUE(anim::parsePropertyRef(component, "enabled", ref)) << component;
        const anim::PropertyBinding binding = anim::bindProperty(&scene, entity, ref, 1);
        ASSERT_EQ(binding.status, anim::PropertyBindStatus::Bound) << component;
        ASSERT_TRUE(anim::writePropertyKeySpace(&scene, entity, binding, &value)) << component;
    };
    Entity trail = child(root, "Trail");
    TrailRendererComponent trailComponent;
    trailComponent.hasLastHeadPosition = true;
    trailComponent.idleTime = 1.0f;
    scene.addComponent<TrailRendererComponent>(trail, trailComponent);
    write(trail, "trailrenderer", 0.0f);
    EXPECT_FALSE(scene.getComponent<TrailRendererComponent>(trail).enabled);
    EXPECT_FALSE(scene.getComponent<TrailRendererComponent>(trail).hasLastHeadPosition) << "cleared, as TrailRenderer::setEnabled(false)";
    EXPECT_EQ(scene.getComponent<TrailRendererComponent>(trail).idleTime, 0.0f);

    Entity joint = child(root, "Joint");
    Joint2DComponent jointComponent;
    jointComponent.enabled = false;
    jointComponent.broken = true;
    jointComponent.breakArmed = false;
    jointComponent.needUpdateJoint = false;
    scene.addComponent<Joint2DComponent>(joint, jointComponent);
    write(joint, "joint2d", 1.0f);
    const Joint2DComponent& j = scene.getComponent<Joint2DComponent>(joint);
    EXPECT_TRUE(j.enabled);
    EXPECT_FALSE(j.broken) << "a broken joint enabled again is a fresh joint";
    EXPECT_TRUE(j.breakArmed);
    EXPECT_TRUE(j.needUpdateJoint);

    Entity cage = child(root, "Cage");
    scene.addComponent<LatticeComponent>(cage, LatticeComponent());
    const uint32_t revision = scene.getComponent<LatticeComponent>(cage).revision;
    write(cage, "lattice", 0.0f);
    EXPECT_EQ(scene.getComponent<LatticeComponent>(cage).revision, revision + 1) << "the deformation is rebuilt";

    Entity text = child(root, "Text");
    TextEffectComponent effect;
    effect.needApply = false;
    scene.addComponent<TextEffectComponent>(text, effect);
    write(text, "texteffect", 0.0f);
    EXPECT_FALSE(scene.getComponent<TextEffectComponent>(text).enabled);
    EXPECT_TRUE(scene.getComponent<TextEffectComponent>(text).needApply) << "TextEffect::setEnabled applies the text again";

    Entity controls = child(root, "Controls");
    scene.addComponent<OnScreenStickComponent>(controls, OnScreenStickComponent());
    scene.addComponent<OnScreenButtonComponent>(controls, OnScreenButtonComponent());
    scene.addComponent<TouchAreaComponent>(controls, TouchAreaComponent());
    write(controls, "onscreenstick", 0.0f);
    write(controls, "onscreenbutton", 0.0f);
    write(controls, "toucharea", 0.0f);
    EXPECT_FALSE(scene.getComponent<OnScreenStickComponent>(controls).enabled);
    EXPECT_FALSE(scene.getComponent<OnScreenButtonComponent>(controls).enabled);
    EXPECT_FALSE(scene.getComponent<TouchAreaComponent>(controls).enabled);
}

// the writes that run callbacks (active) go in hierarchy order once the update's blend ended, so
// an onEnable they run can sample a player
namespace {
    std::function<void()> probeSampleHook;
    int probeSample(lua_State*){
        if (probeSampleHook) probeSampleHook();
        return 0;
    }
}

TEST_F(Properties, DeferredActiveWritesRunInHierarchyOrderAndMaySample){
    lua_State* L = LuaBinding::getLuaState();
    lua_register(L, "probeSample", &probeSample);
    ASSERT_EQ(luaL_dostring(L, "probeOrder = {}"), LUA_OK);
    Entity a = child(root, "A");
    Entity a1 = child(a, "A1");
    Entity b = child(root, "B");
    std::vector<int> refs;
    for (const std::pair<Entity, std::string>& entry : std::vector<std::pair<Entity, std::string>>{{a, "A"}, {a1, "A1"}, {b, "B"}}){
        const std::string code = "probe" + entry.second + " = {name = '" + entry.second + "', onEnable = function(self) "
                                 "probeOrder[#probeOrder + 1] = self.name; if self.name == 'B' then probeSample() end end}";
        ASSERT_EQ(luaL_dostring(L, code.c_str()), LUA_OK);
        lua_getglobal(L, ("probe" + entry.second).c_str());
        refs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
        ScriptComponent scripts;
        ScriptEntry script;
        script.type = ScriptType::LUA;
        script.enabled = true;
        script.className = "Probe";
        script.instance = reinterpret_cast<void*>(static_cast<intptr_t>(refs.back()));
        scripts.scripts.push_back(script);
        scene.addComponent<ScriptComponent>(entry.first, scripts);
        scene.setEntityActive(entry.first, false);
    }
    Entity lamp = light(root, "Lamp", 1.0f);
    ClipData glow;
    glow.tracks.push_back(property("", "light", "intensity", {key(0, {7}), key(1, {7})}));
    AnimationPlayer lampPlayer = player({{"glow", glow}}, lamp);
    float sampled = -1.0f;
    probeSampleHook = [&](){
        lampPlayer.play("glow");
        scene.getSystem<ActionSystem>()->samplePlayer(lamp);
        sampled = scene.getComponent<LightComponent>(lamp).intensity;
    };
    ScriptRuntime::awakeScene(&scene);
    // keyed in the reverse of the hierarchy (B, A1, A)
    ClipData show;
    show.tracks.push_back(property("B", "gameobject", "active", {key(0, {1})}));
    show.tracks.push_back(property("A/A1", "gameobject", "active", {key(0, {1})}));
    show.tracks.push_back(property("A", "gameobject", "active", {key(0, {1})}));
    AnimationPlayer object = player({{"show", show}});
    object.play("show");
    step(0.0);
    ASSERT_EQ(luaL_dostring(L, "probeResult = table.concat(probeOrder, ',')"), LUA_OK);
    lua_getglobal(L, "probeResult");
    const std::string order = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(order, "A,A1,B") << "parents first, siblings in their order";
    EXPECT_FLOAT_EQ(sampled, 7.0f) << "sample() inside a clip's onEnable is not ignored";
    probeSampleHook = nullptr;
    ScriptRuntime::forgetScene(&scene);
    for (Entity entity : {a, a1, b}) scene.getComponent<ScriptComponent>(entity).scripts.clear();
    for (int ref : refs) luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// N1: a runtime rebind (a missing component's 0.25 s retry) keeps the script each track had
TEST_F(Properties, RebindsKeepTheScriptATrackHad){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_EQ(luaL_dostring(L, "pinA = {}; pinB = {}"), LUA_OK);
    lua_getglobal(L, "pinA");
    const int a = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getglobal(L, "pinB");
    const int b = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    for (int ref : {a, b}){
        ScriptEntry entry;
        entry.type = ScriptType::LUA;
        entry.enabled = true;
        entry.className = "Torch";
        entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
        scripts.scripts.push_back(entry);
    }
    scene.addComponent<ScriptComponent>(root, scripts);
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.length = 2.0f;
    data.tracks.push_back(property("", "script:Torch", "enabled", {key(0, {0})}));
    // never binds (no Canvas Group): the player rebinds every 0.25 s
    data.tracks.push_back(property("", "canvasgroup", "alpha", {key(0, {0.5f})}));
    AnimationPlayer object = player({{"pin", data}});
    object.play("pin");
    step(0.0);
    animtest::takeWarnings();
    for (int i = 0; i < 12; i++) step(0.1);
    const ScriptComponent& now = scene.getComponent<ScriptComponent>(root);
    EXPECT_FALSE(now.scripts[0].enabled);
    EXPECT_TRUE(now.scripts[1].enabled) << "the retries rebound the track to the same script, not the next enabled one";
    scene.getComponent<ScriptComponent>(root).scripts.clear();
    for (int ref : {a, b}) luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// N2 / N3: the bind stamp is the instance's generation (a released registry ref is reused by
// the next instance), checked every update, a frame that also changes the hierarchy included
TEST_F(Properties, AnInstanceMadeAgainOnTheSameRefBindsAgain){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_EQ(luaL_dostring(L, "sameRef = {glow = 0}"), LUA_OK);
    lua_getglobal(L, "sameRef");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    auto torch = [ref](ScriptPropertyType type, uint64_t generation){
        ScriptEntry entry;
        entry.type = ScriptType::LUA;
        entry.enabled = true;
        entry.className = "Torch";
        entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
        entry.instanceGeneration = generation;
        ScriptProperty glow;
        glow.name = "glow";
        glow.type = type;
        if (type == ScriptPropertyType::Int) glow.value = 0;
        else glow.value = 0.0f;
        entry.properties = {glow};
        return entry;
    };
    ScriptComponent scripts;
    scripts.scripts = {torch(ScriptPropertyType::Float, 1)};
    scene.addComponent<ScriptComponent>(root, scripts);
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.length = 2.0f;
    data.tracks.push_back(property("", "script:Torch", "glow", {key(0, {0}), key(1, {1})}));
    AnimationPlayer object = player({{"glow", data}});
    object.play("glow");
    step(0.0);
    step(0.1);
    // released and made again: the same ref, another generation, glow now declared Int; the
    // same frame adds a GameObject (the hierarchy changed)
    ASSERT_EQ(luaL_dostring(L, "sameRef.glow = 0.25"), LUA_OK);
    scene.getComponent<ScriptComponent>(root).scripts[0] = torch(ScriptPropertyType::Int, 2);
    child(root, "Spark");
    object.getState("glow").setTime(0.5f);
    step(0.0);
    ASSERT_EQ(luaL_dostring(L, "sameRefResult = math.type(sameRef.glow) .. '|' .. tostring(sameRef.glow)"), LUA_OK);
    lua_getglobal(L, "sameRefResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "integer|0") << "bound again with the declared Int (it steps)";
    scene.getComponent<ScriptComponent>(root).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

#ifdef JASPERENGINE_PHYSICS_2D
// the Rigidbody 2D's enabled row takes a loaded body out of the world at once (its setter)
TEST_F(Properties, Rigidbody2DEnabledAppliesAtOnce){
    Entity crate = child(root, "Crate");
    Rigidbody2DComponent body;
    body.type = BodyType::DYNAMIC;
    scene.addComponent<Rigidbody2DComponent>(crate, body);
    ASSERT_TRUE(scene.getSystem<PhysicsSystem>()->ensureRigidbody2DLoaded(crate));
    const Rigidbody2DComponent& loaded = scene.getComponent<Rigidbody2DComponent>(crate);
    ASSERT_TRUE(b2Body_IsValid(loaded.body));
    ASSERT_TRUE(b2Body_IsEnabled(loaded.body));
    anim::PropertyRef ref;
    ASSERT_TRUE(anim::parsePropertyRef("rigidbody2d", "enabled", ref));
    const anim::PropertyBinding binding = anim::bindProperty(&scene, crate, ref, 1);
    ASSERT_EQ(binding.status, anim::PropertyBindStatus::Bound);
    float off = 0.0f, on = 1.0f;
    ASSERT_TRUE(anim::writePropertyKeySpace(&scene, crate, binding, &off));
    EXPECT_FALSE(loaded.enabled);
    EXPECT_FALSE(b2Body_IsEnabled(loaded.body)) << "out of the world now, not on the next step";
    ASSERT_TRUE(anim::writePropertyKeySpace(&scene, crate, binding, &on));
    EXPECT_TRUE(b2Body_IsEnabled(loaded.body));
}
#endif

// ---- the game's GameOverPanel (GameStatsShowing, GameOverMenu, go, EarnAnim, falling.anim) ----

TEST_F(Properties, GameOverPanelPortPlaysLikeTheUnityClips){
    // the GameOverMenu prefab: SafeArea (3)/GameOVER/{puntaje, record, coins, DupCoins, Play, Exit}
    scene.setEntityName(root, "GameOverMenu");
    scene.addComponent<CanvasGroupComponent>(root, CanvasGroupComponent());
    RectTransformComponent rootRect;
    rootRect.usingAnchors = true;
    rootRect.width = 400;
    rootRect.height = 300;
    scene.addComponent<RectTransformComponent>(root, rootRect);
    Entity safeArea = child(root, "SafeArea (3)");
    Entity over = child(safeArea, "GameOVER");
    std::vector<Entity> stats;
    for (const char* name : {"puntaje", "record", "coins", "DupCoins", "Play", "Exit"}){
        Entity entity = image(over, name, Vector4(1, 1, 1, 1));
        scene.setEntityActive(entity, false);
        stats.push_back(entity);
    }
    Entity money = image(over, "Money", Vector4(1, 1, 1, 1));
    Entity skull = child(over, "Skull");
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    scene.addComponent<MeshComponent>(skull, mesh);
    SpriteComponent sprite;
    for (int i = 0; i < 2; i++){
        SpriteFrameData frame;
        frame.rect = Rect(0.5f * (float)i, 0.0f, 0.5f, 1.0f);
        sprite.framesRect[(size_t)i] = frame;
    }
    sprite.numFramesRect = 2;
    scene.addComponent<SpriteComponent>(skull, sprite);

    // GameStatsShowing: stepped m_IsActive at 0.5 / 1.333 / 2.167 / 3 s, three events
    ClipData show;
    show.length = 3.0f;
    const std::string base = "SafeArea (3)/GameOVER/";
    const std::vector<std::pair<std::string, float>> times = {{"puntaje", 0.5f}, {"record", 1.333333f}, {"coins", 2.166667f},
                                                              {"DupCoins", 3.0f}, {"Play", 3.0f}, {"Exit", 3.0f}};
    for (const auto& entry : times){
        show.tracks.push_back(property(base + entry.first, "gameobject", "active", {key(0, {0}), key(entry.second, {1})}));
    }
    show.events = {event(0.5f, "CountThisGameScore"), event(1.333333f, "CheckHighscore"), event(2.166667f, "CountMoney")};
    // GameOverMenu / go: the CanvasGroup m_Alpha 0 -> 1 -> 1 -> 0 and m_AnchoredPosition.y (Unity's
    // y-up 150 -> -70 -> 0: from above, through below, to rest) in the y-down layout space,
    // negated (docs: Porting Unity clips); EarnAnim's TMP colour; falling.anim's sprite swap
    show.tracks.push_back(property("", "canvasgroup", "alpha", {key(0, {0}), key(0.333333f, {1}), key(0.583333f, {1}), key(1.666667f, {0})}));
    show.tracks.push_back(property("", "recttransform", "positionOffset.y", {key(0, {-150}), key(0.333333f, {70}), key(0.583333f, {0})}));
    show.tracks.push_back(property(base + "Money", "ui", "color", {key(0, {1, 1, 1, 1}), key(0.5f, {0, 1, 0, 1})}));
    show.tracks.push_back(property(base + "Skull", "sprite", "frame", {key(0, {0}), key(0.25f, {1})}));
    AnimationPlayer object = player({{"GameStatsShowing", show}});
    std::vector<std::string> log;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){
        int active = 0;
        for (Entity entity : stats) active += scene.isEntityActive(entity) ? 1 : 0;
        log.push_back(e.functionName + ":" + std::to_string(active));
    }));
    // where the panel is drawn: the layout's y (y-down: smaller is higher on screen)
    auto screenY = [&](){
        scene.getSystem<UISystem>()->update(0.0);
        return scene.getComponent<Transform>(root).position.y;
    };
    object.play("GameStatsShowing");
    step(0.0);
    EXPECT_FLOAT_EQ(scene.getComponent<CanvasGroupComponent>(root).alpha, 0.0f);
    EXPECT_NEAR(scene.getComponent<RectTransformComponent>(root).positionOffset.y, -150.0f, 1e-4f);
    const float startY = screenY();
    const double frame = 1.0 / 60.0;
    double t = 0.0;
    auto runTo = [&](double target){
        while (t + frame <= target + 1e-9){
            step(frame);
            t += frame;
        }
    };
    runTo(0.25);
    EXPECT_NEAR(scene.getComponent<CanvasGroupComponent>(root).alpha, 0.75f, 0.02f);
    EXPECT_FLOAT_EQ(scene.getComponent<MeshComponent>(skull).submeshes[0].textureRect.getX(), 0.5f) << "the sprite swap at 0.25 s";
    runTo(1.0 / 3.0);
    const float lowY = screenY();
    runTo(0.48);
    EXPECT_FALSE(scene.isEntityActive(stats[0]));
    runTo(0.5);
    EXPECT_TRUE(scene.isEntityActive(stats[0]));
    EXPECT_FALSE(scene.isEntityActive(stats[1]));
    EXPECT_NEAR(scene.getComponent<UIComponent>(money).color.x, 0.0f, 1e-5f);
    runTo(1.0);
    EXPECT_NEAR(scene.getComponent<RectTransformComponent>(root).positionOffset.y, 0.0f, 1e-4f);
    const float restY = screenY();
    // as in Unity: it comes in from 150 above its place, swings 70 below it, then settles
    EXPECT_NEAR(restY - startY, 150.0f, 0.5f) << "starts above its rest on screen";
    EXPECT_NEAR(lowY - restY, 70.0f, 0.5f) << "overshoots below its rest";
    runTo(1.35);
    EXPECT_TRUE(scene.isEntityActive(stats[1]));
    EXPECT_FALSE(scene.isEntityActive(stats[2]));
    runTo(2.2);
    EXPECT_TRUE(scene.isEntityActive(stats[2]));
    EXPECT_FALSE(scene.isEntityActive(stats[5]));
    EXPECT_FLOAT_EQ(scene.getComponent<CanvasGroupComponent>(root).alpha, 0.0f);
    runTo(3.05);
    for (Entity entity : stats) EXPECT_TRUE(scene.isEntityActive(entity));
    EXPECT_EQ(log, (std::vector<std::string>{"CountThisGameScore:1", "CheckHighscore:2", "CountMoney:3"}));
    object.getOnEvent().clear();
}

// ---- 14. performance (Release only) ----

#ifdef NDEBUG
TEST_F(Properties, PerformanceBudget){
    // 200 animated properties: UI fades and flickering lights (spec 14: <= 0.08 ms per frame on
    // the host in Release). A shared box under load cannot time it: skipped above its core count
    const double cores = (double)std::max(1u, std::thread::hardware_concurrency());
#if defined(__unix__) || defined(__APPLE__)
    double load[1] = {0.0};
    const bool timed = !(getloadavg(load, 1) == 1 && load[0] > cores);
#else
    (void)cores;
    const bool timed = true;   // no load average on Windows
#endif
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    for (int i = 0; i < 100; i++){
        const std::string name = "I" + std::to_string(i);
        image(root, name, Vector4(1, 1, 1, 1));
        data.tracks.push_back(property(name, "ui", "color.a", {key(0, {0}), key(1, {1})}));
        const std::string lampName = "L" + std::to_string(i);
        light(root, lampName, 1.0f);
        data.tracks.push_back(property(lampName, "light", "intensity", {key(0, {0.5f}), key(0.5f, {2}), key(1, {0.5f})}));
    }
    AnimationPlayer object = player({{"flicker", data}});
    object.play("flicker");
    step(0.0);
    step(0.016);
    double best = 1e9;
    for (int round = 0; round < 8; round++){
        const auto start = std::chrono::steady_clock::now();
        for (int f = 0; f < 30; f++) step(0.016);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 30.0;
        best = std::min(best, ms);
    }
    std::printf("[ property tracks ] 200 properties: %.4f ms per frame\n", best);
    if (timed){
        EXPECT_LE(best, 0.08);
    }
}
#endif
