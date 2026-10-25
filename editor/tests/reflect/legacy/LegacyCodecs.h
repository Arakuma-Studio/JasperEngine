// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The hand-written component codecs the reflection tables replaced (docs/specs/S08-reflection.md
// D5), kept verbatim as the oracle of the parity tests (ReflectParity.*): the table's encode of a
// component must give the same text, and its decode the same values.

#include "Stream.h"

namespace jasperengine::editor {

    class LegacyCodecs : public Stream {
    public:
        static YAML::Node encodeLightComponent(const LightComponent& light);
        static LightComponent decodeLightComponent(const YAML::Node& node, const LightComponent* oldLight = nullptr);
        static YAML::Node encodeLight2DComponent(const Light2DComponent& light);
        static Light2DComponent decodeLight2DComponent(const YAML::Node& node, const Light2DComponent* oldLight = nullptr);
        static YAML::Node encodeShadowCaster2DComponent(const ShadowCaster2DComponent& occluder);
        static ShadowCaster2DComponent decodeShadowCaster2DComponent(const YAML::Node& node, const ShadowCaster2DComponent* oldOccluder = nullptr);
        static YAML::Node encodeFogComponent(const FogComponent& fog);
        static FogComponent decodeFogComponent(const YAML::Node& node, const FogComponent* oldFog = nullptr);
        static YAML::Node encodeLocalVolumetricFogComponent(const LocalVolumetricFogComponent& volume);
        static LocalVolumetricFogComponent decodeLocalVolumetricFogComponent(const YAML::Node& node, const LocalVolumetricFogComponent* oldVolume = nullptr);
        static YAML::Node encodeVolumetricCloudsComponent(const VolumetricCloudsComponent& clouds);
        static VolumetricCloudsComponent decodeVolumetricCloudsComponent(const YAML::Node& node, const VolumetricCloudsComponent* oldClouds = nullptr);
        static YAML::Node encodeMirrorComponent(const MirrorComponent& mirror);
        static MirrorComponent decodeMirrorComponent(const YAML::Node& node, const MirrorComponent* oldMirror = nullptr);
        static YAML::Node encodeReflectionProbeComponent(const ReflectionProbeComponent& probe);
        static ReflectionProbeComponent decodeReflectionProbeComponent(const YAML::Node& node, const ReflectionProbeComponent* oldProbe = nullptr);
        static YAML::Node encodeUIDocumentComponent(const UIDocumentComponent& document);
        static UIDocumentComponent decodeUIDocumentComponent(const YAML::Node& node, const UIDocumentComponent* oldDocument = nullptr);
        static YAML::Node encodeCameraComponent(const CameraComponent& camera);
        static CameraComponent decodeCameraComponent(const YAML::Node& node, const CameraComponent* oldCamera = nullptr);
        static YAML::Node encodeAudioSourceComponent(const AudioSourceComponent& audio);
        static AudioSourceComponent decodeAudioSourceComponent(const YAML::Node& node, const AudioSourceComponent* oldAudio = nullptr);
        static YAML::Node encodeButtonComponent(const ButtonComponent& button);
        static ButtonComponent decodeButtonComponent(const YAML::Node& node, const ButtonComponent* oldButton = nullptr);
        static YAML::Node encodeScrollbarComponent(const ScrollbarComponent& scrollbar);
        static ScrollbarComponent decodeScrollbarComponent(const YAML::Node& node, const ScrollbarComponent* oldScrollbar = nullptr);
        static YAML::Node encodeProgressbarComponent(const ProgressbarComponent& progressbar);
        static ProgressbarComponent decodeProgressbarComponent(const YAML::Node& node, const ProgressbarComponent* oldProgressbar = nullptr);
        static YAML::Node encodeInputFieldComponent(const InputFieldComponent& inputfield);
        static InputFieldComponent decodeInputFieldComponent(const YAML::Node& node, const InputFieldComponent* oldInputField = nullptr);
        static YAML::Node encodePanelComponent(const PanelComponent& panel);
        static PanelComponent decodePanelComponent(const YAML::Node& node, const PanelComponent* oldPanel = nullptr);
        static YAML::Node encodeRectTransformComponent(const RectTransformComponent& layout);
        static RectTransformComponent decodeRectTransformComponent(const YAML::Node& node, const RectTransformComponent* oldLayout = nullptr);
        static YAML::Node encodeLayoutGroupComponent(const LayoutGroupComponent& container);
        static LayoutGroupComponent decodeLayoutGroupComponent(const YAML::Node& node, const LayoutGroupComponent* oldContainer = nullptr);
        static YAML::Node encodeTextComponent(const TextComponent& text);
        static TextComponent decodeTextComponent(const YAML::Node& node, const TextComponent* oldText = nullptr);
        static YAML::Node encodeImageComponent(const ImageComponent& image);
        static ImageComponent decodeImageComponent(const YAML::Node& node, const ImageComponent* oldImage = nullptr);
        static YAML::Node encodeTransform(const Transform& transform);
        static Transform decodeTransform(const YAML::Node& node, const Transform* oldTransform = nullptr);
        static YAML::Node encodeJoint2DComponent(const Joint2DComponent& joint);
        static Joint2DComponent decodeJoint2DComponent(const YAML::Node& node, const Joint2DComponent* oldJoint = nullptr);
        static YAML::Node encodeJoint3DComponent(const Joint3DComponent& joint);
        static Joint3DComponent decodeJoint3DComponent(const YAML::Node& node, const Joint3DComponent* oldJoint = nullptr);
        static YAML::Node encodeActionComponent(const ActionComponent& action);
        static ActionComponent decodeActionComponent(const YAML::Node& node, const ActionComponent* oldAction = nullptr);
        static YAML::Node encodeTweenComponent(const TweenComponent& timed);
        static TweenComponent decodeTweenComponent(const YAML::Node& node, const TweenComponent* oldTimed = nullptr);
        static YAML::Node encodePositionTweenComponent(const PositionTweenComponent& posAction);
        static PositionTweenComponent decodePositionTweenComponent(const YAML::Node& node, const PositionTweenComponent* oldPosAction = nullptr);
        static YAML::Node encodeRotationTweenComponent(const RotationTweenComponent& rotAction);
        static RotationTweenComponent decodeRotationTweenComponent(const YAML::Node& node, const RotationTweenComponent* oldRotAction = nullptr);
        static YAML::Node encodeScaleTweenComponent(const ScaleTweenComponent& scaleTween);
        static ScaleTweenComponent decodeScaleTweenComponent(const YAML::Node& node, const ScaleTweenComponent* oldScaleTween = nullptr);
        static YAML::Node encodeColorTweenComponent(const ColorTweenComponent& colorTween);
        static ColorTweenComponent decodeColorTweenComponent(const YAML::Node& node, const ColorTweenComponent* oldColorTween = nullptr);
        static YAML::Node encodeAlphaTweenComponent(const AlphaTweenComponent& alphaTween);
        static AlphaTweenComponent decodeAlphaTweenComponent(const YAML::Node& node, const AlphaTweenComponent* oldAlphaTween = nullptr);
        static YAML::Node encodeAnimationComponent(const AnimationComponent& animation);
        static AnimationComponent decodeAnimationComponent(const YAML::Node& node, const AnimationComponent* oldAnimation = nullptr);
        static YAML::Node encodeKeyframeTracksComponent(const KeyframeTracksComponent& tracks);
        static KeyframeTracksComponent decodeKeyframeTracksComponent(const YAML::Node& node, const KeyframeTracksComponent* oldTracks = nullptr);
        static YAML::Node encodeTranslateTracksComponent(const TranslateTracksComponent& tracks);
        static TranslateTracksComponent decodeTranslateTracksComponent(const YAML::Node& node, const TranslateTracksComponent* oldTracks = nullptr);
        static YAML::Node encodeRotateTracksComponent(const RotateTracksComponent& tracks);
        static RotateTracksComponent decodeRotateTracksComponent(const YAML::Node& node, const RotateTracksComponent* oldTracks = nullptr);
        static YAML::Node encodeScaleTracksComponent(const ScaleTracksComponent& tracks);
        static ScaleTracksComponent decodeScaleTracksComponent(const YAML::Node& node, const ScaleTracksComponent* oldTracks = nullptr);
        static YAML::Node encodePostProcessing(const PostProcessingSettings& settings);
        static PostProcessingSettings decodePostProcessing(const YAML::Node& node);
        static YAML::Node encodeBoneComponent(const BoneComponent& bone);
        static BoneComponent decodeBoneComponent(const YAML::Node& node, const BoneComponent* oldBone = nullptr);
    };

}
