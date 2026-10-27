// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every component type Stream writes: encode, decode, encode again, and the two YAML texts are
// the same, so loading and saving a scene does not drift. Each case starts from the defaults
// and changes a few fields, so a value that is written but not read back shows up.

#include "Stream.h"

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

// STABLE(<component type>, <encode/decode suffix>, <statements changing `component`>)
#define STABLE(Type, Name, ...) \
    TEST(StreamComponent, Name##ReEncodesIdentically){ \
        Type component{}; \
        __VA_ARGS__; \
        expectStable<Type>(component, [](const Type& c){ return Stream::encode##Name(c); }, \
                           [](const YAML::Node& node){ return Stream::decode##Name(node); }); \
        EXPECT_NO_ENGINE_ERRORS(); \
    }

STABLE(Transform, Transform,
    component.position = Vector3(1, 2, 3); component.scale = Vector3(2, 2, 2);
    component.rotation = Quaternion(30.0f, Vector3::UNIT_Y); component.visible = false)
STABLE(MeshComponent, MeshComponent, component.castShadows = false; component.receiveLights = false)
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
STABLE(TerrainComponent, TerrainComponent, )
STABLE(LightComponent, LightComponent,
    component.type = LightType::SPOT; component.color = Vector3(1, 0.5f, 0.25f); component.direction = Vector3(0, -1, 0))
STABLE(Light2DComponent, Light2DComponent, )
STABLE(ShadowCaster2DComponent, ShadowCaster2DComponent, )
STABLE(FogComponent, FogComponent, component.type = FogType::EXPONENTIAL; component.density = 0.05f)
STABLE(MirrorComponent, MirrorComponent, )
STABLE(ReflectionProbeComponent, ReflectionProbeComponent, )
STABLE(UIDocumentComponent, UIDocumentComponent, )
STABLE(CameraComponent, CameraComponent,
    component.type = CameraType::CAMERA_PERSPECTIVE; component.target = Vector3(0, 1, -5); component.cullingMask = 0x5u)
STABLE(AudioSourceComponent, AudioSourceComponent, component.filename = "sounds/hit.wav")
STABLE(ScriptComponent, ScriptComponent, )
STABLE(SkyboxComponent, SkyboxComponent, )
STABLE(ModelComponent, ModelComponent, )
STABLE(PrefabComponent, PrefabComponent, )
STABLE(Rigidbody2DComponent, Rigidbody2DComponent, )
STABLE(RigidbodyComponent, RigidbodyComponent, )
STABLE(Joint2DComponent, Joint2DComponent, )
STABLE(Joint3DComponent, Joint3DComponent, )
STABLE(ActionComponent, ActionComponent, component.state = ActionState::Paused)
STABLE(TweenComponent, TweenComponent, )
STABLE(PositionTweenComponent, PositionTweenComponent, )
STABLE(RotationTweenComponent, RotationTweenComponent, )
STABLE(ScaleTweenComponent, ScaleTweenComponent, )
STABLE(ColorTweenComponent, ColorTweenComponent, )
STABLE(AlphaTweenComponent, AlphaTweenComponent, )
STABLE(SpriteAnimationComponent, SpriteAnimationComponent, )
STABLE(AnimationComponent, AnimationComponent, )
STABLE(BoneComponent, BoneComponent, )
STABLE(KeyframeTracksComponent, KeyframeTracksComponent, )
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

#undef STABLE

// an asset effect: its path and GUID, and the inline effect left as it was
TEST(StreamComponent, ParticleSystemComponentAssetReEncodesIdentically){
    ParticleSystemComponent component{};
    component.effect.set("fx/fire.particles", Guid(1, 2));
    expectStable<ParticleSystemComponent>(component, [](const ParticleSystemComponent& c){ return Stream::encodeParticleSystemComponent(c); },
                                          [](const YAML::Node& node){ return Stream::decodeParticleSystemComponent(node); });
    EXPECT_NO_ENGINE_ERRORS();
}
