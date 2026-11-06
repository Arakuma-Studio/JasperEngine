// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPONENTFWD_H
#define COMPONENTFWD_H

// The component types by name, for a header that only passes them around: the file of a component
// is included by the sources that read it, which is what an edit of it rebuilds (tools/check_fanout.py)
namespace jasperengine{

    struct ActionComponent;
    struct AlphaTweenComponent;
    struct AnimationComponent;
    struct AnimationPlayerComponent;
    struct AnimatorComponent;
    struct AudioSourceComponent;
    struct BoneComponent;
    struct BuoyancyComponent;
    struct ButtonComponent;
    struct CameraComponent;
    struct CameraFollowComponent;
    struct CameraShakerComponent;
    struct CanvasGroupComponent;
    struct CharacterController2DComponent;
    struct CharacterControllerComponent;
    struct ColorTweenComponent;
    struct FluidSimulationComponent;
    struct FogComponent;
    struct ForceFieldComponent;
    struct ImageComponent;
    struct InputFieldComponent;
    struct InstancedMeshComponent;
    struct Joint2DComponent;
    struct Joint3DComponent;
    struct KeyframeTracksComponent;
    struct LatticeComponent;
    struct LatticeFollowerComponent;
    struct LatticeModifierComponent;
    struct LatticeTracksComponent;
    struct LayoutGroupComponent;
    struct Light2DComponent;
    struct LightComponent;
    struct LineRendererComponent;
    struct LocalVolumetricFogComponent;
    struct MeshComponent;
    struct MeshPolygonComponent;
    struct MirrorComponent;
    struct ModelComponent;
    struct MorphTracksComponent;
    struct NavGridComponent;
    struct NavMeshAgentComponent;
    struct NavMeshLinkComponent;
    struct NavMeshModifierComponent;
    struct NavMeshObstacleComponent;
    struct NavMeshSurfaceComponent;
    struct OnScreenButtonComponent;
    struct OnScreenStickComponent;
    struct PanelComponent;
    struct ParticleSystemComponent;
    struct PixelPerfectCameraComponent;
    struct PointsComponent;
    struct PolygonComponent;
    struct PoseAnimatorComponent;
    struct PositionTweenComponent;
    struct PrefabComponent;
    struct ProgressbarComponent;
    struct RagdollComponent;
    struct RectTransformComponent;
    struct ReflectionProbeComponent;
    struct Rigidbody2DComponent;
    struct RigidbodyComponent;
    struct RotateTracksComponent;
    struct RotationTweenComponent;
    struct SafeAreaComponent;
    struct AudioSnapshotComponent;
    struct ScaleTracksComponent;
    struct ScaleTweenComponent;
    struct ScriptComponent;
    struct ScrollbarComponent;
    struct ShadowCaster2DComponent;
    struct SkyboxComponent;
    struct SplineAnimateComponent;
    struct SplineComponent;
    struct SplineExtrudeComponent;
    struct SplineInstantiateComponent;
    struct SpriteAnimationComponent;
    struct SpriteComponent;
    struct TerrainComponent;
    struct TextComponent;
    struct TextEffectComponent;
    struct TilemapComponent;
    struct TouchAreaComponent;
    struct TrailRendererComponent;
    struct Transform;
    struct TranslateTracksComponent;
    struct TweenComponent;
    struct UIComponent;
    struct UIDocumentComponent;
    struct VertexAnimatorComponent;
    struct VideoPlayerComponent;
    struct VolumetricCloudsComponent;
    struct WaterVolumeComponent;

}

#endif //COMPONENTFWD_H
