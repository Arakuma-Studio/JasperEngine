// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_EDITOR_TEST_STREAM_ACCESS_H
#define JASPER_EDITOR_TEST_STREAM_ACCESS_H

#include "Stream.h"

#include <string>

namespace jasperengine::editor {

    // The private conversions of Stream, for the round-trip tests (Stream befriends this class)
    class StreamTestAccess {
    public:
#define JASPER_STREAM_ENUM(Type, toString, fromString) \
        static std::string toString(Type value){ return Stream::toString(value); } \
        static Type fromString(const std::string& text){ return Stream::fromString(text); }

        JASPER_STREAM_ENUM(ActionState, actionStateToString, stringToActionState)
        JASPER_STREAM_ENUM(AnchorPreset, anchorPresetToString, stringToAnchorPreset)
        JASPER_STREAM_ENUM(AttributeDataType, attributeDataTypeToString, stringToAttributeDataType)
        JASPER_STREAM_ENUM(AttributeType, attributeTypeToString, stringToAttributeType)
        JASPER_STREAM_ENUM(AudioSourceAttenuation, audioSourceAttenuationToString, stringToAudioSourceAttenuation)
        JASPER_STREAM_ENUM(AudioSourceState, audioSourceStateToString, stringToAudioSourceState)
        JASPER_STREAM_ENUM(BodyType, bodyTypeToString, stringToBodyType)
        JASPER_STREAM_ENUM(BufferType, bufferTypeToString, stringToBufferType)
        JASPER_STREAM_ENUM(BufferUsage, bufferUsageToString, stringToBufferUsage)
        JASPER_STREAM_ENUM(CameraType, cameraTypeToString, stringToCameraType)
        JASPER_STREAM_ENUM(Collider2DType, collider2DTypeToString, stringToCollider2DType)
        JASPER_STREAM_ENUM(Collider3DType, collider3DTypeToString, stringToCollider3DType)
        JASPER_STREAM_ENUM(CullingMode, cullingModeToString, stringToCullingMode)
        JASPER_STREAM_ENUM(EaseType, easeTypeToString, stringToEaseType)
        JASPER_STREAM_ENUM(FogType, fogTypeToString, stringToFogType)
        JASPER_STREAM_ENUM(Joint2DType, joint2DTypeToString, stringToJoint2DType)
        JASPER_STREAM_ENUM(Joint3DType, joint3DTypeToString, stringToJoint3DType)
        JASPER_STREAM_ENUM(LayoutGroupType, layoutGroupTypeToString, stringToLayoutGroupType)
        JASPER_STREAM_ENUM(LightShading, lightShadingToString, stringToLightShading)
        JASPER_STREAM_ENUM(LightState, lightStateToString, stringToLightState)
        JASPER_STREAM_ENUM(LightType, lightTypeToString, stringToLightType)
        JASPER_STREAM_ENUM(MaterialAlphaMode, materialAlphaModeToString, stringToMaterialAlphaMode)
        JASPER_STREAM_ENUM(MaterialBlendMode, materialBlendModeToString, stringToMaterialBlendMode)
        JASPER_STREAM_ENUM(MaterialCullMode, materialCullModeToString, stringToMaterialCullMode)
        JASPER_STREAM_ENUM(MaterialDepthTest, materialDepthTestToString, stringToMaterialDepthTest)
        JASPER_STREAM_ENUM(MaterialDepthWrite, materialDepthWriteToString, stringToMaterialDepthWrite)
        JASPER_STREAM_ENUM(MaterialLighting, materialLightingToString, stringToMaterialLighting)
        JASPER_STREAM_ENUM(MaterialStencilCompare, materialStencilCompareToString, stringToMaterialStencilCompare)
        JASPER_STREAM_ENUM(MaterialStencilOp, materialStencilOpToString, stringToMaterialStencilOp)
        JASPER_STREAM_ENUM(ParticleEmitterShape, particleEmitterShapeToString, stringToParticleEmitterShape)
        JASPER_STREAM_ENUM(PivotPreset, pivotPresetToString, stringToPivotPreset)
        JASPER_STREAM_ENUM(PrimitiveType, primitiveTypeToString, stringToPrimitiveType)
        JASPER_STREAM_ENUM(ProgressbarType, progressbarTypeToString, stringToProgressbarType)
        JASPER_STREAM_ENUM(RigidbodyMotionQuality, rigidbodyMotionQualityToString, stringToRigidbodyMotionQuality)
        JASPER_STREAM_ENUM(Scaling, scalingModeToString, stringToScalingMode)
        JASPER_STREAM_ENUM(SceneType, sceneTypeToString, stringToSceneType)
        JASPER_STREAM_ENUM(ScriptPropertyType, scriptPropertyTypeToString, stringToScriptPropertyType)
        JASPER_STREAM_ENUM(ScriptType, scriptTypeToString, stringToScriptType)
        JASPER_STREAM_ENUM(ScrollbarType, scrollbarTypeToString, stringToScrollbarType)
        JASPER_STREAM_ENUM(TextureFilter, textureFilterToString, stringToTextureFilter)
        JASPER_STREAM_ENUM(TextureStrategy, textureStrategyToString, stringToTextureStrategy)
        JASPER_STREAM_ENUM(TextureWrap, textureWrapToString, stringToTextureWrap)
        JASPER_STREAM_ENUM(UIDocumentRenderMode, uiDocumentRenderModeToString, stringToUIDocumentRenderMode)
        JASPER_STREAM_ENUM(UIEventState, uiEventStateToString, stringToUIEventState)
        JASPER_STREAM_ENUM(WindingOrder, windingOrderToString, stringToWindingOrder)
        JASPER_STREAM_ENUM(WindowMode, windowModeToString, stringToWindowMode)

#undef JASPER_STREAM_ENUM

        static const char* colorFormatToString(ColorFormat format){ return Stream::colorFormatToString(format); }
        static ColorFormat stringToColorFormat(const std::string& text, int channels){ return Stream::stringToColorFormat(text, channels); }
        static std::string colorWriteToString(int colorWrite){ return Stream::colorWriteToString(colorWrite); }
        static int stringToColorWrite(const std::string& text){ return Stream::stringToColorWrite(text); }
        static std::string materialPassOrderToString(MaterialPassOrder order){ return Stream::materialPassOrderToString(order); }
        static MaterialPassOrder stringToMaterialPassOrder(const std::string& text){ return Stream::stringToMaterialPassOrder(text); }

        static YAML::Node encodeVector2(const Vector2& value){ return Stream::encodeVector2(value); }
        static Vector2 decodeVector2(const YAML::Node& node){ return Stream::decodeVector2(node); }
        static YAML::Node encodeVector3(const Vector3& value){ return Stream::encodeVector3(value); }
        static Vector3 decodeVector3(const YAML::Node& node){ return Stream::decodeVector3(node); }
        static YAML::Node encodeVector4(const Vector4& value){ return Stream::encodeVector4(value); }
        static Vector4 decodeVector4(const YAML::Node& node){ return Stream::decodeVector4(node); }
        static YAML::Node encodeQuaternion(const Quaternion& value){ return Stream::encodeQuaternion(value); }
        static Quaternion decodeQuaternion(const YAML::Node& node){ return Stream::decodeQuaternion(node); }
        static YAML::Node encodeMatrix4(const Matrix4& value){ return Stream::encodeMatrix4(value); }
        static Matrix4 decodeMatrix4(const YAML::Node& node){ return Stream::decodeMatrix4(node); }
        static YAML::Node encodeRect(const Rect& value){ return Stream::encodeRect(value); }
        static Rect decodeRect(const YAML::Node& node){ return Stream::decodeRect(node); }
        static YAML::Node encodeAABB(const AABB& value){ return Stream::encodeAABB(value); }
        static AABB decodeAABB(const YAML::Node& node){ return Stream::decodeAABB(node); }
        static YAML::Node encodeEase(const Ease& value){ return Stream::encodeEase(value); }
        static Ease decodeEase(const YAML::Node& node){ return Stream::decodeEase(node); }
        static YAML::Node encodeStencil(const MaterialStencil& value){ return Stream::encodeStencil(value); }
        static MaterialStencil decodeStencil(const YAML::Node& node){ return Stream::decodeStencil(node); }
        static float finiteOr(float value, float fallback){ return Stream::finiteOr(value, fallback); }
        static float positiveFiniteOr(float value, float fallback){ return Stream::positiveFiniteOr(value, fallback); }
        static uint32_t decodeLayerMask(const YAML::Node& node, uint32_t fallback){ return Stream::decodeLayerMask(node, fallback); }
    };

}

#endif //JASPER_EDITOR_TEST_STREAM_ACCESS_H
