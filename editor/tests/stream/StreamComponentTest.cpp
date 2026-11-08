// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every component type Stream writes: encode, decode, encode again, and the two YAML texts are
// the same, so loading and saving a scene does not drift. Each case starts from the defaults
// and changes a few fields, so a value that is written but not read back shows up.

#include "Stream.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/BoneComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/FogComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/MirrorComponent.h"
#include "component/ModelComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/PanelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/PrefabComponent.h"
#include "component/ScriptComponent.h"
#include "component/LineRendererComponent.h"
#include "Scene.h"
#include "Catalog.h"
#include "component/BuoyancyComponent.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/WaterVolumeComponent.h"
#include "particles/ParticleEffectCodec.h"
#include "reflect/Reflect.h"
#include "component/ActionComponent.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <functional>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    // what a component value owns outside a scene (CameraComponent allocates its framebuffer, which
    // RenderSystem deletes when the entity goes)
    template<typename T>
    void releaseOwned(T&){}
    void releaseOwned(CameraComponent& camera){
        delete camera.framebuffer;
        camera.framebuffer = nullptr;
    }

    template<typename T>
    void expectStable(T& component, std::function<YAML::Node(const T&)> encode, std::function<T(const YAML::Node&)> decode){
        const YAML::Node first = encode(component);
        ASSERT_TRUE(first.IsDefined());
        T decoded = decode(YAML::Load(YAML::Dump(first)));
        const YAML::Node second = encode(decoded);
        EXPECT_EQ(YAML::Dump(first), YAML::Dump(second));
        releaseOwned(decoded);
        releaseOwned(component);
    }

}

// STABLE(<component type>, <its ComponentType>, <statements changing `component`>): through its
// reflection table, the generic codec or the component's own (editor/reflect/components)
#define STABLE(Type, Name, ...) \
    TEST(StreamComponent, Name##ReEncodesIdentically){ \
        Type component{}; \
        __VA_ARGS__; \
        expectStable<Type>(component, [](const Type& c){ return reflect::encodeValue(ComponentType::Name, &c); }, \
                           [](const YAML::Node& node){ return reflect::decodeValue<Type>(ComponentType::Name, node); }); \
        EXPECT_NO_ENGINE_ERRORS(); \
    }

STABLE(Transform, Transform,
    component.position = Vector3(1, 2, 3); component.scale = Vector3(2, 2, 2);
    component.rotation = Quaternion(30.0f, Vector3::UNIT_Y); component.visible = false)
STABLE(MeshComponent, MeshComponent, component.castShadows = false; component.receiveLights = false; component.allowGpuInstancing = false)
STABLE(UIComponent, UIComponent, component.color = Vector4(0.5f, 0.25f, 1.0f, 0.75f))
STABLE(ButtonComponent, ButtonComponent, component.colorHovered = Vector4(0.2f, 0.3f, 0.4f, 1.0f))
STABLE(ScrollbarComponent, ScrollbarComponent, component.type = ScrollbarType::HORIZONTAL)
STABLE(ProgressbarComponent, ProgressbarComponent, component.type = ProgressbarType::VERTICAL)
STABLE(InputFieldComponent, InputFieldComponent, component.cursorBlink = 0.8f; component.cursorWidth = 3)
STABLE(PanelComponent, PanelComponent, component.headerMarginLeft = 4; component.resizeMargin = 9)
STABLE(RectTransformComponent, RectTransformComponent, component.anchorPreset = AnchorPreset::CENTER)
STABLE(LayoutGroupComponent, LayoutGroupComponent, component.type = LayoutGroupType::HORIZONTAL)
STABLE(TextComponent, TextComponent, component.text = "Hello, YAML: [1, 2]"; component.multiline = false)
STABLE(ImageComponent, ImageComponent, component.textureScaleFactor = 2.0f)
STABLE(SpriteComponent, SpriteComponent, component.flipY = true; component.textureScaleFactor = 0.5f)
STABLE(TilemapComponent, TilemapComponent, )
STABLE(TerrainComponent, TerrainComponent, component.surfaceLayers.resize(1); component.surfaceLayers[0].hexTiling = true;
    component.surfaceLayers[0].hexRotation = 0.5f; component.surfaceLayers[0].stochasticTiling = true;
    component.surfaceLayers[0].stochasticCellSize = 2.0f)
STABLE(LightComponent, LightComponent,
    component.type = LightType::SPOT; component.color = Vector3(1, 0.5f, 0.25f); component.direction = Vector3(0, -1, 0);
    component.intensityUnit = LightUnit::LUMEN; component.useColorTemperature = true; component.colorTemperature = 3200.0f;
    component.shadows = true; component.contactShadows = true; component.contactShadowLength = 0.3f; component.contactShadowThickness = 0.1f;
    component.contactShadowSampleCount = 16)
STABLE(Light2DComponent, Light2DComponent, )
STABLE(ShadowCaster2DComponent, ShadowCaster2DComponent, )
STABLE(CapsuleShadowCasterComponent, CapsuleShadowCasterComponent,
    component.castShadows = CapsuleCastShadows::CAPSULES_ONLY;
    component.capsules.push_back({"Hips", 0.15f, 0.4f, Vector3(0, 0.1f, 0), CapsuleDirection::Y_AXIS});
    component.capsules.push_back({"", 0.05f, 0.2f, Vector3(0, 0, 0), CapsuleDirection::Z_AXIS}))
STABLE(FogComponent, FogComponent, component.type = FogType::EXPONENTIAL; component.density = 0.05f)
STABLE(MirrorComponent, MirrorComponent, )
STABLE(ReflectionProbeComponent, ReflectionProbeComponent, )
STABLE(UIDocumentComponent, UIDocumentComponent, )
STABLE(CameraComponent, CameraComponent,
    component.type = CameraType::CAMERA_PERSPECTIVE; component.target = Vector3(0, 1, -5); component.cullingMask = 0x5u;
    component.transparencyMode = TransparencyMode::ORDER_INDEPENDENT; component.oitWeightScale = 0.5f)
STABLE(AudioSourceComponent, AudioSourceComponent, component.filename = "sounds/hit.wav")
STABLE(ScriptComponent, ScriptComponent, )
STABLE(SkyboxComponent, SkyboxComponent, )
STABLE(ModelComponent, ModelComponent, )
STABLE(PrefabComponent, PrefabComponent, )
STABLE(Rigidbody2DComponent, Rigidbody2DComponent, )
STABLE(RigidbodyComponent, RigidbodyComponent, component.buoyancy = 1.5f; component.waterDrag = 0.25f; component.waterAngularDrag = 0.1f)
STABLE(Joint2DComponent, Joint2DComponent, )
STABLE(Joint3DComponent, Joint3DComponent, )
STABLE(ActionComponent, ActionComponent, component.state = ActionState::Paused)
STABLE(TweenComponent, TweenComponent, component.pingPong = true)
STABLE(PositionTweenComponent, PositionTweenComponent, )
STABLE(RotationTweenComponent, RotationTweenComponent, component.spin = true)
STABLE(ScaleTweenComponent, ScaleTweenComponent, )
STABLE(ColorTweenComponent, ColorTweenComponent, )
STABLE(AlphaTweenComponent, AlphaTweenComponent, )
STABLE(SpriteAnimationComponent, SpriteAnimationComponent, )
STABLE(AnimationComponent, AnimationComponent, )
STABLE(BoneComponent, BoneComponent, )
STABLE(KeyframeTracksComponent, KeyframeTracksComponent, component.loop = true; component.relative = true)
STABLE(TranslateTracksComponent, TranslateTracksComponent, )
STABLE(RotateTracksComponent, RotateTracksComponent, )
STABLE(ScaleTracksComponent, ScaleTracksComponent, )
STABLE(MorphTracksComponent, MorphTracksComponent, )
STABLE(ParticleSystemComponent, ParticleSystemComponent,
    component.playOnAwake = false; component.seed = 42; component.playbackSpeed = 1.5f;
    component.inlineEffect.duration = 3.0f; component.inlineEffect.noise.present = true;
    component.inlineEffect.emission.rateOverTime = particles::MinMaxCurve::twoConstants(2.0f, 8.0f);
    component.exposedValues.push_back({"Intensity", Vector4(2.0f, 0.0f, 0.0f, 0.0f)}))
STABLE(ForceFieldComponent, ForceFieldComponent,
    component.shape = ForceFieldShapeType::Box; component.kind = ForceFieldKind::Vortex; component.strength = -3.0f;
    component.falloff = Curve::linear(1.0f, 0.0f); component.layers = 0x6u)
STABLE(InstancedMeshComponent, InstancedMeshComponent, )
STABLE(PointsComponent, PointsComponent, )
STABLE(LineRendererComponent, LineRendererComponent, )
STABLE(PolygonComponent, PolygonComponent, )
STABLE(MeshPolygonComponent, MeshPolygonComponent, )
STABLE(FluidSimulationComponent, FluidSimulationComponent, component.output = FluidOutput::SCREEN; component.curl = 12.5f; component.seed = 7;
    component.backColor = Vector3(0.1f, 0.2f, 0.3f))
STABLE(LatticeComponent, LatticeComponent, component.resolutionX = 2; component.offsets.assign(18, Vector3(0.25f, 0, 0));
    component.interpolation = LatticeInterpolation::SMOOTH; component.poses.push_back(LatticePose{"bend", component.offsets}))
STABLE(LatticeModifierComponent, LatticeModifierComponent, component.bindings.push_back(LatticeBinding{5, 0.5f, LatticeStage::BEFORE_SKINNING, 1,
    LatticeMaskChannel::GREEN}); component.subdivisions = 4)
STABLE(LatticeFollowerComponent, LatticeFollowerComponent, component.lattice = 3; component.applyScale = true; component.weight = 0.75f)
STABLE(LatticeTracksComponent, LatticeTracksComponent, component.points = {0, 4}; component.values = {{Vector3(1, 0, 0), Vector3(0, 1, 0)}})

STABLE(CharacterController2DComponent, CharacterController2DComponent, component.center = Vector2(4.0f, 8.0f); component.height = 96.0f;
    component.pushBodies = false; component.maskBits = 0x00F0)
STABLE(CharacterControllerComponent, CharacterControllerComponent, component.radius = 0.4f; component.mass = 80.0f;
    component.enhancedEdgeRemoval = true; component.categoryBits = 4)
STABLE(WaterVolumeComponent, WaterVolumeComponent, component.infinite = true; component.flow = Vector3(1.0f, 0.0f, 0.5f);
    component.waves = WaterVolumeWaves::NONE)
STABLE(BuoyancyComponent, BuoyancyComponent, component.columns = BuoyancyColumns::CUSTOM;
    component.customPoints = {Vector3(-1.0f, 0.0f, -1.0f), Vector3(1.0f, 0.0f, 1.0f)}; component.volumeScale = 0.8f)
#undef STABLE

// The fields added after the S08 base (S11's photometric light, terrain hex tiling, the 64-bit
// submesh override mask and the material emissive strength, upstream's water, tween and keyframe
// switches) are read back with their values, not only re-encoded the same.
namespace {

    template<typename T>
    T roundTrip(ComponentType type, const T& component){
        return reflect::decodeValue<T>(type, YAML::Load(YAML::Dump(reflect::encodeValue(type, &component))));
    }

}

TEST(StreamComponent, LightPhotometricFieldsRoundTrip){
    LightComponent light{};
    light.intensityUnit = LightUnit::CANDELA;
    light.useColorTemperature = true;
    light.colorTemperature = 2700.0f;
    const LightComponent decoded = roundTrip(ComponentType::LightComponent, light);
    EXPECT_EQ(decoded.intensityUnit, LightUnit::CANDELA);
    EXPECT_TRUE(decoded.useColorTemperature);
    EXPECT_FLOAT_EQ(decoded.colorTemperature, 2700.0f);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamComponent, RigidbodyWaterFieldsRoundTrip){
    RigidbodyComponent body{};
    body.buoyancy = 1.25f;
    body.waterDrag = 0.75f;
    body.waterAngularDrag = 0.2f;
    const RigidbodyComponent decoded = roundTrip(ComponentType::RigidbodyComponent, body);
    EXPECT_FLOAT_EQ(decoded.buoyancy, 1.25f);
    EXPECT_FLOAT_EQ(decoded.waterDrag, 0.75f);
    EXPECT_FLOAT_EQ(decoded.waterAngularDrag, 0.2f);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamComponent, TerrainLayerTilingFieldsRoundTrip){
    TerrainComponent terrain{};
    terrain.surfaceLayers.resize(2);
    terrain.surfaceLayers[1].hexTiling = true;
    terrain.surfaceLayers[1].hexRotation = 0.25f;
    terrain.surfaceLayers[1].stochasticTiling = true;
    terrain.surfaceLayers[1].stochasticCellSize = 3.0f;
    terrain.surfaceLayers[1].stochasticContrast = 0.6f;
    const TerrainComponent decoded = roundTrip(ComponentType::TerrainComponent, terrain);
    ASSERT_GE(decoded.surfaceLayers.size(), 2u);
    EXPECT_FALSE(decoded.surfaceLayers[0].hexTiling);
    EXPECT_TRUE(decoded.surfaceLayers[1].hexTiling);
    EXPECT_FLOAT_EQ(decoded.surfaceLayers[1].hexRotation, 0.25f);
    EXPECT_TRUE(decoded.surfaceLayers[1].stochasticTiling);
    EXPECT_FLOAT_EQ(decoded.surfaceLayers[1].stochasticCellSize, 3.0f);
    EXPECT_FLOAT_EQ(decoded.surfaceLayers[1].stochasticContrast, 0.6f);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamComponent, TweenAndKeyframeSwitchesRoundTrip){
    TweenComponent tween{};
    tween.pingPong = true;
    EXPECT_TRUE(roundTrip(ComponentType::TweenComponent, tween).pingPong);
    RotationTweenComponent rotation{};
    rotation.spin = true;
    EXPECT_TRUE(roundTrip(ComponentType::RotationTweenComponent, rotation).spin);
    KeyframeTracksComponent tracks{};
    tracks.loop = true;
    tracks.relative = true;
    const KeyframeTracksComponent decoded = roundTrip(ComponentType::KeyframeTracksComponent, tracks);
    EXPECT_TRUE(decoded.loop);
    EXPECT_TRUE(decoded.relative);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamComponent, SubmeshOverrideMaskKeepsItsHighBits){
    MeshComponent mesh{};
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].overrideFields = SubmeshOverride_EmissiveStrength | SubmeshOverride_StochasticTiling;
    mesh.submeshes[0].material.emissiveStrength = 4.0f;
    const MeshComponent decoded = roundTrip(ComponentType::MeshComponent, mesh);
    ASSERT_EQ(decoded.numSubmeshes, 1u);
    EXPECT_EQ(decoded.submeshes[0].overrideFields, (uint64_t)(SubmeshOverride_EmissiveStrength | SubmeshOverride_StochasticTiling))
        << "bit 32 and up survive the save";
    EXPECT_FLOAT_EQ(decoded.submeshes[0].material.emissiveStrength, 4.0f);
    EXPECT_NO_ENGINE_ERRORS();
}

// an asset effect: its path and GUID, and the inline effect left as it was
TEST(StreamComponent, ParticleSystemComponentAssetReEncodesIdentically){
    ParticleSystemComponent component{};
    component.effect.set("fx/fire.particles", Guid(1, 2));
    expectStable<ParticleSystemComponent>(component,
        [](const ParticleSystemComponent& c){ return reflect::encodeValue(ComponentType::ParticleSystemComponent, &c); },
        [](const YAML::Node& node){ return reflect::decodeValue<ParticleSystemComponent>(ComponentType::ParticleSystemComponent, node); });
    EXPECT_NO_ENGINE_ERRORS();
}

// S20's components through their tables: every field read back with its value
TEST(StreamComponent, ParticleSystemFieldsRoundTrip){
    ParticleSystemComponent inlineSystem{};
    inlineSystem.inlineEffect.duration = 4.5f;
    inlineSystem.inlineEffect.emission.rateOverTime = particles::MinMaxCurve::twoConstants(3.0f, 9.0f);
    inlineSystem.playOnAwake = false;
    inlineSystem.seed = 42;
    inlineSystem.playbackSpeed = 1.5f;
    inlineSystem.exposedValues.push_back({"Intensity", Vector4(2.0f, 0.5f, 0.0f, 1.0f)});
    inlineSystem.collisionPlanes = {7, 9};
    ParticleSystemComponent decoded = roundTrip(ComponentType::ParticleSystemComponent, inlineSystem);
    EXPECT_TRUE(decoded.effect.empty());
    EXPECT_FLOAT_EQ(decoded.inlineEffect.duration, 4.5f);
    EXPECT_EQ(particles::ParticleEffectCodec::write(decoded.inlineEffect), particles::ParticleEffectCodec::write(inlineSystem.inlineEffect));
    EXPECT_FALSE(decoded.playOnAwake);
    EXPECT_EQ(decoded.seed, 42u);
    EXPECT_FLOAT_EQ(decoded.playbackSpeed, 1.5f);
    ASSERT_EQ(decoded.exposedValues.size(), 1u);
    EXPECT_EQ(decoded.exposedValues[0], inlineSystem.exposedValues[0]);
    EXPECT_EQ(decoded.collisionPlanes, (std::vector<Entity>{7, 9}));

    ParticleSystemComponent assetSystem{};
    assetSystem.effect.set("fx/smoke.particles", Guid(3, 4));
    assetSystem.enabled = false;
    decoded = roundTrip(ComponentType::ParticleSystemComponent, assetSystem);
    EXPECT_EQ(decoded.effect.path, "fx/smoke.particles");
    EXPECT_EQ(decoded.effect.guid, Guid(3, 4));
    EXPECT_FALSE(decoded.enabled);
    EXPECT_NO_ENGINE_ERRORS();
}

// (ui-scaling-text) the GameObject's Unscaled Time: written only when on (a file from before it
// reads and writes as it was), read back; absent over a component that had it is off
TEST(StreamComponent, ParticleSystemUnscaledTimeOnlyWhenOn){
    ParticleSystemComponent plain{};
    const YAML::Node plainNode = reflect::encodeValue(ComponentType::ParticleSystemComponent, &plain);
    EXPECT_FALSE(plainNode["useUnscaledTime"]) << "off: not written";

    ParticleSystemComponent unscaled{};
    unscaled.useUnscaledTime = true;
    const YAML::Node node = reflect::encodeValue(ComponentType::ParticleSystemComponent, &unscaled);
    ASSERT_TRUE(node["useUnscaledTime"]);
    EXPECT_TRUE(node["useUnscaledTime"].as<bool>());
    EXPECT_TRUE(roundTrip(ComponentType::ParticleSystemComponent, unscaled).useUnscaledTime);

    // the old component decoded over: an absent key is off again
    const ParticleSystemComponent over = Stream::decodeParticleSystemComponent(plainNode, &unscaled);
    EXPECT_FALSE(over.useUnscaledTime);
    EXPECT_NO_ENGINE_ERRORS();
}

// (runtime-core) an action's Start Offset and Random Start: written only away from their defaults
// (scenes from before them read and write as they were), read back, exported when set
TEST(StreamComponent, ActionStartOffsetRoundTripsAndOmitsDefaults){
    ActionComponent plain{};
    const YAML::Node plainNode = reflect::encodeValue(ComponentType::ActionComponent, &plain);
    EXPECT_FALSE(plainNode["startOffset"]) << "the default is not written";
    EXPECT_FALSE(plainNode["randomStart"]);

    ActionComponent set{};
    set.startOffset = 0.35f;
    set.randomStart = true;
    set.phase = 0.2f;
    set.runs = 3;
    const YAML::Node node = reflect::encodeValue(ComponentType::ActionComponent, &set);
    ASSERT_TRUE(node["startOffset"]);
    EXPECT_FLOAT_EQ(node["startOffset"].as<float>(), 0.35f);
    EXPECT_TRUE(node["randomStart"].as<bool>());
    EXPECT_FALSE(node["phase"]) << "runtime state is not saved";
    const ActionComponent decoded = roundTrip(ComponentType::ActionComponent, set);
    EXPECT_FLOAT_EQ(decoded.startOffset, 0.35f);
    EXPECT_TRUE(decoded.randomStart);
    EXPECT_FLOAT_EQ(decoded.phase, 0.0f);
    EXPECT_EQ(decoded.runs, 0u);

    // a file from before them (no keys) decodes to the defaults
    const ActionComponent old = reflect::decodeValue<ActionComponent>(ComponentType::ActionComponent, YAML::Load("speed: 2"));
    EXPECT_FLOAT_EQ(old.startOffset, 0.0f);
    EXPECT_FALSE(old.randomStart);

    // exported code: only when set
    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::ActionComponent);
    reflect::CodegenContext ctx;
    ctx.variable = "action";
    EXPECT_EQ(reflect::cppAssignments(desc, &plain, ctx).find("startOffset"), std::string::npos);
    const std::string code = reflect::cppAssignments(desc, &set, ctx);
    EXPECT_NE(code.find("action.startOffset = 0.35"), std::string::npos) << code;
    EXPECT_NE(code.find("action.randomStart = true"), std::string::npos) << code;
    EXPECT_EQ(code.find("phase"), std::string::npos) << code;
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamComponent, ForceFieldFieldsRoundTrip){
    ForceFieldComponent field{};
    field.enabled = false;
    field.shape = ForceFieldShapeType::Cylinder;
    field.size = Vector3(2.0f, 3.0f, 4.0f);
    field.kind = ForceFieldKind::Turbulence;
    field.strength = -2.5f;
    field.direction = Vector3(0.0f, 1.0f, 0.0f);
    field.turbulence = 0.25f;
    field.pulseFrequency = 0.5f;
    field.frequency = 2.0f;
    field.falloff = Curve::linear(1.0f, 0.0f);
    field.layers = 0x5u;
    const ForceFieldComponent decoded = roundTrip(ComponentType::ForceFieldComponent, field);
    EXPECT_FALSE(decoded.enabled);
    EXPECT_EQ(decoded.shape, ForceFieldShapeType::Cylinder);
    EXPECT_EQ(decoded.size, Vector3(2.0f, 3.0f, 4.0f));
    EXPECT_EQ(decoded.kind, ForceFieldKind::Turbulence);
    EXPECT_FLOAT_EQ(decoded.strength, -2.5f);
    EXPECT_EQ(decoded.direction, Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_FLOAT_EQ(decoded.turbulence, 0.25f);
    EXPECT_FLOAT_EQ(decoded.pulseFrequency, 0.5f);
    EXPECT_FLOAT_EQ(decoded.frequency, 2.0f);
    EXPECT_EQ(decoded.falloff, field.falloff);
    EXPECT_EQ(decoded.layers, 0x5u);
    EXPECT_NO_ENGINE_ERRORS();
}

// S22's components through their tables: every field read back with its value
TEST(StreamComponent, CharacterControllersRoundTrip){
    CharacterControllerComponent controller{};
    controller.enabled = false;
    controller.center = Vector3(0.0f, 1.0f, 0.0f);
    controller.height = 1.8f;
    controller.radius = 0.4f;
    controller.slopeLimit = 50.0f;
    controller.stepOffset = 0.25f;
    controller.skinWidth = 0.03f;
    controller.minMoveDistance = 0.002f;
    controller.mass = 90.0f;
    controller.pushStrength = 50.0f;
    controller.pushBodies = false;
    controller.movingPlatforms = false;
    controller.inheritPlatformYaw = false;
    controller.detectCollisions = false;
    controller.characterCollisions = false;
    controller.enhancedEdgeRemoval = true;
    controller.categoryBits = 2;
    controller.maskBits = 0x00FF;
    const CharacterControllerComponent decoded = roundTrip(ComponentType::CharacterControllerComponent, controller);
    EXPECT_FALSE(decoded.enabled);
    EXPECT_EQ(decoded.center, Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_FLOAT_EQ(decoded.height, 1.8f);
    EXPECT_FLOAT_EQ(decoded.radius, 0.4f);
    EXPECT_FLOAT_EQ(decoded.slopeLimit, 50.0f);
    EXPECT_FLOAT_EQ(decoded.stepOffset, 0.25f);
    EXPECT_FLOAT_EQ(decoded.skinWidth, 0.03f);
    EXPECT_FLOAT_EQ(decoded.minMoveDistance, 0.002f);
    EXPECT_FLOAT_EQ(decoded.mass, 90.0f);
    EXPECT_FLOAT_EQ(decoded.pushStrength, 50.0f);
    EXPECT_FALSE(decoded.pushBodies);
    EXPECT_FALSE(decoded.movingPlatforms);
    EXPECT_FALSE(decoded.inheritPlatformYaw);
    EXPECT_FALSE(decoded.detectCollisions);
    EXPECT_FALSE(decoded.characterCollisions);
    EXPECT_TRUE(decoded.enhancedEdgeRemoval);
    EXPECT_EQ(decoded.categoryBits, 2);
    EXPECT_EQ(decoded.maskBits, 0x00FF);

    CharacterController2DComponent controller2d{};
    controller2d.center = Vector2(3.0f, -2.0f);
    controller2d.height = 100.0f;
    controller2d.radius = 20.0f;
    controller2d.slopeLimit = 30.0f;
    controller2d.stepOffset = 10.0f;
    controller2d.skinWidth = 2.0f;
    controller2d.minMoveDistance = 0.1f;
    controller2d.pushStrength = 20.0f;
    controller2d.pushBodies = false;
    controller2d.movingPlatforms = false;
    controller2d.categoryBits = 8;
    controller2d.maskBits = 0x0F0F;
    const CharacterController2DComponent decoded2d = roundTrip(ComponentType::CharacterController2DComponent, controller2d);
    EXPECT_EQ(decoded2d.center, Vector2(3.0f, -2.0f));
    EXPECT_FLOAT_EQ(decoded2d.height, 100.0f);
    EXPECT_FLOAT_EQ(decoded2d.radius, 20.0f);
    EXPECT_FLOAT_EQ(decoded2d.slopeLimit, 30.0f);
    EXPECT_FLOAT_EQ(decoded2d.stepOffset, 10.0f);
    EXPECT_FLOAT_EQ(decoded2d.skinWidth, 2.0f);
    EXPECT_FLOAT_EQ(decoded2d.minMoveDistance, 0.1f);
    EXPECT_FLOAT_EQ(decoded2d.pushStrength, 20.0f);
    EXPECT_FALSE(decoded2d.pushBodies);
    EXPECT_FALSE(decoded2d.movingPlatforms);
    EXPECT_EQ(decoded2d.categoryBits, 8);
    EXPECT_EQ(decoded2d.maskBits, 0x0F0F);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamComponent, WaterVolumeAndBuoyancyRoundTrip){
    WaterVolumeComponent volume{};
    volume.enabled = false;
    volume.size = Vector3(20.0f, 3.0f, 8.0f);
    volume.surfaceOffset = 0.5f;
    volume.infinite = true;
    volume.density = 1025.0f;
    volume.flow = Vector3(0.5f, 0.0f, -1.0f);
    volume.linearDrag = 2.0f;
    volume.angularDrag = 0.25f;
    volume.waves = WaterVolumeWaves::NONE;
    const WaterVolumeComponent decodedVolume = roundTrip(ComponentType::WaterVolumeComponent, volume);
    EXPECT_FALSE(decodedVolume.enabled);
    EXPECT_EQ(decodedVolume.size, Vector3(20.0f, 3.0f, 8.0f));
    EXPECT_FLOAT_EQ(decodedVolume.surfaceOffset, 0.5f);
    EXPECT_TRUE(decodedVolume.infinite);
    EXPECT_FLOAT_EQ(decodedVolume.density, 1025.0f);
    EXPECT_EQ(decodedVolume.flow, Vector3(0.5f, 0.0f, -1.0f));
    EXPECT_FLOAT_EQ(decodedVolume.linearDrag, 2.0f);
    EXPECT_FLOAT_EQ(decodedVolume.angularDrag, 0.25f);
    EXPECT_EQ(decodedVolume.waves, WaterVolumeWaves::NONE);

    BuoyancyComponent buoyancy{};
    buoyancy.enabled = false;
    buoyancy.columns = BuoyancyColumns::CUSTOM;
    buoyancy.customPoints = {Vector3(-0.5f, -0.5f, 0.0f), Vector3(0.5f, -0.5f, 0.0f), Vector3(0.0f, -0.5f, 1.0f)};
    buoyancy.volumeScale = 1.2f;
    buoyancy.dragScale = 0.5f;
    const BuoyancyComponent decodedBuoyancy = roundTrip(ComponentType::BuoyancyComponent, buoyancy);
    EXPECT_FALSE(decodedBuoyancy.enabled);
    EXPECT_EQ(decodedBuoyancy.columns, BuoyancyColumns::CUSTOM);
    EXPECT_EQ(decodedBuoyancy.customPoints, buoyancy.customPoints);
    EXPECT_FLOAT_EQ(decodedBuoyancy.volumeScale, 1.2f);
    EXPECT_FLOAT_EQ(decodedBuoyancy.dragScale, 0.5f);
    EXPECT_NO_ENGINE_ERRORS();
}

// prefab Apply/Revert copy one property between instances: a value no type branch names (the
// Buoyancy's customPoints) is copied by its table's ops
TEST(StreamComponent, CopyPropertyValueCopiesTableValues){
    Scene source;
    Scene target;
    const Entity from = source.createEntity();
    const Entity to = target.createEntity();
    BuoyancyComponent buoyancy{};
    buoyancy.customPoints = {Vector3(1.0f, 2.0f, 3.0f), Vector3(4.0f, 5.0f, 6.0f)};
    source.addComponent<BuoyancyComponent>(from, buoyancy);
    target.addComponent<BuoyancyComponent>(to, BuoyancyComponent{});
    Catalog::copyPropertyValue(&source, from, &target, to, ComponentType::BuoyancyComponent, "customPoints");
    EXPECT_EQ(target.getComponent<BuoyancyComponent>(to).customPoints, buoyancy.customPoints);
    EXPECT_NO_ENGINE_ERRORS();
}

// T3 AC42: every field of R49 round-trips, and a terrain at its defaults writes none of them
TEST(StreamComponent, TerrainMaterialsRoundTrip){
    TerrainComponent terrain{};
    terrain.surfaceLayers.resize(1);
    terrain.surfaceLayers[0].cliffProjection = TerrainCliffProjection::FLAT_ONLY;
    terrain.quality = TerrainQualitySetting::MEDIUM;
    terrain.limitLayersPerPixel = true;
    terrain.offscreenShadows = TerrainOffscreenShadows::OFF;
    terrain.macroMode = TerrainMacroMode::BASE_MAP;
    terrain.macroScale = 12.0f;
    terrain.macroStrength = 0.5f;
    terrain.macroTintA = Vector3(0.1f, 0.2f, 0.3f);
    terrain.macroTintB = Vector3(0.4f, 0.5f, 0.6f);
    terrain.macroSlopeFade = 0.1f;
    terrain.resampleEnabled = true;
    terrain.resampleNear = 5.0f;
    terrain.resampleFar = 50.0f;
    terrain.resampleReduction = 0.5f;
    terrain.heightBlendSharpness = 0.7f;
    terrain.macroNormal = true;
    terrain.holeMapHolds = 3;
    const TerrainComponent decoded = roundTrip(ComponentType::TerrainComponent, terrain);
    EXPECT_EQ(decoded.surfaceLayers[0].cliffProjection, TerrainCliffProjection::FLAT_ONLY);
    EXPECT_EQ(decoded.quality, TerrainQualitySetting::MEDIUM);
    EXPECT_TRUE(decoded.limitLayersPerPixel);
    EXPECT_EQ(decoded.offscreenShadows, TerrainOffscreenShadows::OFF);
    EXPECT_EQ(decoded.macroMode, TerrainMacroMode::BASE_MAP);
    EXPECT_FLOAT_EQ(decoded.macroScale, 12.0f);
    EXPECT_FLOAT_EQ(decoded.macroStrength, 0.5f);
    EXPECT_EQ(decoded.macroTintA, Vector3(0.1f, 0.2f, 0.3f));
    EXPECT_EQ(decoded.macroTintB, Vector3(0.4f, 0.5f, 0.6f));
    EXPECT_FLOAT_EQ(decoded.macroSlopeFade, 0.1f);
    EXPECT_TRUE(decoded.resampleEnabled);
    EXPECT_FLOAT_EQ(decoded.resampleNear, 5.0f);
    EXPECT_FLOAT_EQ(decoded.resampleFar, 50.0f);
    EXPECT_FLOAT_EQ(decoded.resampleReduction, 0.5f);
    EXPECT_FLOAT_EQ(decoded.heightBlendSharpness, 0.7f);
    EXPECT_TRUE(decoded.macroNormal);
    EXPECT_EQ(decoded.holeMapHolds, 0) << "a hold is not saved";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamComponent, OldTerrainResavesIdentical){
    TerrainComponent terrain{};
    terrain.surfaceLayers.resize(1);
    const YAML::Node node = Stream::encodeTerrainComponent(terrain);
    for (const char* key : {"quality", "limitLayersPerPixel", "offscreenShadows", "macroMode", "macroScale", "macroStrength", "macroTintA",
                            "macroTintB", "macroSlopeFade", "resampleEnabled", "resampleNear", "resampleFar", "resampleReduction",
                            "heightBlendSharpness", "macroNormal", "holeMap"}){
        EXPECT_FALSE(node[key]) << key << " written at its default";
    }
    EXPECT_FALSE(node["surfaceLayers"][0]["cliffProjection"]);
}
