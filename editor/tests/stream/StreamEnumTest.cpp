// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every enum Stream writes as text: each enumerator round-trips through its string, and a
// string Stream does not know gives the documented default (the value an old or hand-edited
// file falls back to).

#include "StreamTestAccess.h"
#include "component/ActionComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/FogComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/LightComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/SpriteComponent.h"
#include "component/UIDocumentComponent.h"
#include "camera/CameraType.h"
#include "component/ScriptComponent.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    template<typename T>
    void expectRoundTrip(std::string (*toString)(T), T (*fromString)(const std::string&), std::initializer_list<T> values, T unknownDefault){
        std::set<std::string> names;
        for (T value : values){
            const std::string name = toString(value);
            EXPECT_FALSE(name.empty()) << "enumerator " << static_cast<int>(value);
            EXPECT_EQ(fromString(name), value) << "'" << name << "'";
            names.insert(name);
        }
        EXPECT_EQ(names.size(), values.size()) << "two enumerators share a name";
        EXPECT_EQ(fromString("no such value"), unknownDefault);
    }

}

TEST(StreamEnum, ActionStateRoundTrips){
    expectRoundTrip<ActionState>(StreamTestAccess::actionStateToString, StreamTestAccess::stringToActionState, {
        ActionState::Running,
        ActionState::Paused,
        ActionState::Stopped
    }, ActionState::Stopped);
}

TEST(StreamEnum, AnchorPresetRoundTrips){
    expectRoundTrip<AnchorPreset>(StreamTestAccess::anchorPresetToString, StreamTestAccess::stringToAnchorPreset, {
        AnchorPreset::NONE,
        AnchorPreset::TOP_LEFT,
        AnchorPreset::TOP_RIGHT,
        AnchorPreset::BOTTOM_LEFT,
        AnchorPreset::BOTTOM_RIGHT,
        AnchorPreset::CENTER_LEFT,
        AnchorPreset::CENTER_TOP,
        AnchorPreset::CENTER_RIGHT,
        AnchorPreset::CENTER_BOTTOM,
        AnchorPreset::CENTER,
        AnchorPreset::LEFT_WIDE,
        AnchorPreset::TOP_WIDE,
        AnchorPreset::RIGHT_WIDE,
        AnchorPreset::BOTTOM_WIDE,
        AnchorPreset::VERTICAL_CENTER_WIDE,
        AnchorPreset::HORIZONTAL_CENTER_WIDE,
        AnchorPreset::FULL_LAYOUT
    }, AnchorPreset::NONE);
}

TEST(StreamEnum, AttributeDataTypeRoundTrips){
    expectRoundTrip<AttributeDataType>(StreamTestAccess::attributeDataTypeToString, StreamTestAccess::stringToAttributeDataType, {
        AttributeDataType::BYTE,
        AttributeDataType::UNSIGNED_BYTE,
        AttributeDataType::SHORT,
        AttributeDataType::UNSIGNED_SHORT,
        AttributeDataType::INT,
        AttributeDataType::UNSIGNED_INT,
        AttributeDataType::FLOAT,
        AttributeDataType::HALF
    }, AttributeDataType::FLOAT);
}

TEST(StreamEnum, AttributeTypeRoundTrips){
    expectRoundTrip<AttributeType>(StreamTestAccess::attributeTypeToString, StreamTestAccess::stringToAttributeType, {
        AttributeType::INDEX,
        AttributeType::POSITION,
        AttributeType::TEXCOORD1,
        AttributeType::TEXCOORD2,
        AttributeType::NORMAL,
        AttributeType::TANGENT,
        AttributeType::COLOR,
        AttributeType::POINTSIZE,
        AttributeType::POINTROTATION,
        AttributeType::TEXTURERECT,
        AttributeType::BONEWEIGHTS,
        AttributeType::BONEIDS,
        AttributeType::MORPHTARGET0,
        AttributeType::MORPHTARGET1,
        AttributeType::MORPHTARGET2,
        AttributeType::MORPHTARGET3,
        AttributeType::MORPHTARGET4,
        AttributeType::MORPHTARGET5,
        AttributeType::MORPHTARGET6,
        AttributeType::MORPHTARGET7,
        AttributeType::MORPHNORMAL0,
        AttributeType::MORPHNORMAL1,
        AttributeType::MORPHNORMAL2,
        AttributeType::MORPHNORMAL3,
        AttributeType::MORPHTANGENT0,
        AttributeType::MORPHTANGENT1,
        AttributeType::INSTANCEMATRIXCOL1,
        AttributeType::INSTANCEMATRIXCOL2,
        AttributeType::INSTANCEMATRIXCOL3,
        AttributeType::INSTANCEMATRIXCOL4,
        AttributeType::INSTANCECOLOR,
        AttributeType::INSTANCETEXTURERECT,
        AttributeType::TERRAINNODEPOSITION,
        AttributeType::TERRAINNODESIZE,
        AttributeType::TERRAINNODERANGE,
        AttributeType::TERRAINNODERESOLUTION,
        AttributeType::SMOOTHNORMAL,
        AttributeType::PARTICLECORNER,
        AttributeType::PARTICLEPOSITION,
        AttributeType::PARTICLECOLOR,
        AttributeType::PARTICLESIZE,
        AttributeType::PARTICLEROTATIONFRAME,
        AttributeType::PARTICLEVELOCITY,
        AttributeType::PARTICLEROTATION,
        AttributeType::PARTICLECUSTOM,
        AttributeType::VAT_INDEX,
        AttributeType::INSTANCEVAT0,
        AttributeType::INSTANCEVAT1
    }, AttributeType::POSITION);
}

TEST(StreamEnum, AudioSourceAttenuationRoundTrips){
    expectRoundTrip<AudioSourceAttenuation>(StreamTestAccess::audioSourceAttenuationToString, StreamTestAccess::stringToAudioSourceAttenuation, {
        AudioSourceAttenuation::NO_ATTENUATION,
        AudioSourceAttenuation::INVERSE_DISTANCE,
        AudioSourceAttenuation::LINEAR_DISTANCE,
        AudioSourceAttenuation::EXPONENTIAL_DISTANCE
    }, AudioSourceAttenuation::NO_ATTENUATION);
}

TEST(StreamEnum, AudioSourceStateRoundTrips){
    expectRoundTrip<AudioSourceState>(StreamTestAccess::audioSourceStateToString, StreamTestAccess::stringToAudioSourceState, {
        AudioSourceState::Playing,
        AudioSourceState::Paused,
        AudioSourceState::Stopped
    }, AudioSourceState::Stopped);
}

TEST(StreamEnum, BodyTypeRoundTrips){
    expectRoundTrip<BodyType>(StreamTestAccess::bodyTypeToString, StreamTestAccess::stringToBodyType, {
        BodyType::STATIC,
        BodyType::KINEMATIC,
        BodyType::DYNAMIC
    }, BodyType::STATIC);
}

TEST(StreamEnum, BufferTypeRoundTrips){
    expectRoundTrip<BufferType>(StreamTestAccess::bufferTypeToString, StreamTestAccess::stringToBufferType, {
        BufferType::VERTEX_BUFFER,
        BufferType::INDEX_BUFFER,
        BufferType::STORAGE_BUFFER
    }, BufferType::VERTEX_BUFFER);
}

TEST(StreamEnum, BufferUsageRoundTrips){
    expectRoundTrip<BufferUsage>(StreamTestAccess::bufferUsageToString, StreamTestAccess::stringToBufferUsage, {
        BufferUsage::IMMUTABLE,
        BufferUsage::DYNAMIC,
        BufferUsage::STREAM
    }, BufferUsage::IMMUTABLE);
}

TEST(StreamEnum, CameraTypeRoundTrips){
    expectRoundTrip<CameraType>(StreamTestAccess::cameraTypeToString, StreamTestAccess::stringToCameraType, {
        CameraType::CAMERA_UI,
        CameraType::CAMERA_ORTHO,
        CameraType::CAMERA_PERSPECTIVE
    }, CameraType::CAMERA_PERSPECTIVE);
}

TEST(StreamEnum, Collider2DTypeRoundTrips){
    expectRoundTrip<Collider2DType>(StreamTestAccess::collider2DTypeToString, StreamTestAccess::stringToCollider2DType, {
        Collider2DType::POLYGON,
        Collider2DType::CIRCLE,
        Collider2DType::CAPSULE,
        Collider2DType::SEGMENT,
        Collider2DType::CHAIN
    }, Collider2DType::POLYGON);
}

TEST(StreamEnum, Collider3DTypeRoundTrips){
    expectRoundTrip<Collider3DType>(StreamTestAccess::collider3DTypeToString, StreamTestAccess::stringToCollider3DType, {
        Collider3DType::SPHERE,
        Collider3DType::BOX,
        Collider3DType::CAPSULE,
        Collider3DType::TAPERED_CAPSULE,
        Collider3DType::CYLINDER,
        Collider3DType::CONVEX_HULL,
        Collider3DType::MESH,
        Collider3DType::HEIGHTFIELD
    }, Collider3DType::SPHERE);
}

TEST(StreamEnum, CullingModeRoundTrips){
    expectRoundTrip<CullingMode>(StreamTestAccess::cullingModeToString, StreamTestAccess::stringToCullingMode, {
        CullingMode::BACK,
        CullingMode::FRONT
    }, CullingMode::BACK);
}

TEST(StreamEnum, EaseTypeRoundTrips){
    expectRoundTrip<EaseType>(StreamTestAccess::easeTypeToString, StreamTestAccess::stringToEaseType, {
        EaseType::LINEAR,
        EaseType::QUAD_IN,
        EaseType::QUAD_OUT,
        EaseType::QUAD_IN_OUT,
        EaseType::CUBIC_IN,
        EaseType::CUBIC_OUT,
        EaseType::CUBIC_IN_OUT,
        EaseType::QUART_IN,
        EaseType::QUART_OUT,
        EaseType::QUART_IN_OUT,
        EaseType::QUINT_IN,
        EaseType::QUINT_OUT,
        EaseType::QUINT_IN_OUT,
        EaseType::SINE_IN,
        EaseType::SINE_OUT,
        EaseType::SINE_IN_OUT,
        EaseType::EXPO_IN,
        EaseType::EXPO_OUT,
        EaseType::EXPO_IN_OUT,
        EaseType::CIRC_IN,
        EaseType::CIRC_OUT,
        EaseType::CIRC_IN_OUT,
        EaseType::ELASTIC_IN,
        EaseType::ELASTIC_OUT,
        EaseType::ELASTIC_IN_OUT,
        EaseType::BACK_IN,
        EaseType::BACK_OUT,
        EaseType::BACK_IN_OUT,
        EaseType::BOUNCE_IN,
        EaseType::BOUNCE_OUT,
        EaseType::BOUNCE_IN_OUT,
        EaseType::STEP,
        EaseType::CUSTOM
    }, EaseType::LINEAR);
}

TEST(StreamEnum, FogTypeRoundTrips){
    expectRoundTrip<FogType>(StreamTestAccess::fogTypeToString, StreamTestAccess::stringToFogType, {
        FogType::LINEAR,
        FogType::EXPONENTIAL,
        FogType::EXPONENTIALSQUARED
    }, FogType::LINEAR);
}

TEST(StreamEnum, Joint2DTypeRoundTrips){
    expectRoundTrip<Joint2DType>(StreamTestAccess::joint2DTypeToString, StreamTestAccess::stringToJoint2DType, {
        Joint2DType::DISTANCE,
        Joint2DType::REVOLUTE,
        Joint2DType::PRISMATIC,
        Joint2DType::MOUSE,
        Joint2DType::WHEEL,
        Joint2DType::WELD,
        Joint2DType::MOTOR
    }, Joint2DType::DISTANCE);
}

TEST(StreamEnum, Joint3DTypeRoundTrips){
    expectRoundTrip<Joint3DType>(StreamTestAccess::joint3DTypeToString, StreamTestAccess::stringToJoint3DType, {
        Joint3DType::FIXED,
        Joint3DType::DISTANCE,
        Joint3DType::POINT,
        Joint3DType::HINGE,
        Joint3DType::CONE,
        Joint3DType::PRISMATIC,
        Joint3DType::SWINGTWIST,
        Joint3DType::SIXDOF,
        Joint3DType::PATH,
        Joint3DType::GEAR,
        Joint3DType::RACKANDPINON,
        Joint3DType::PULLEY
    }, Joint3DType::FIXED);
}

TEST(StreamEnum, LayoutGroupTypeRoundTrips){
    expectRoundTrip<LayoutGroupType>(StreamTestAccess::layoutGroupTypeToString, StreamTestAccess::stringToLayoutGroupType, {
        LayoutGroupType::VERTICAL,
        LayoutGroupType::HORIZONTAL,
        LayoutGroupType::VERTICAL_WRAP,
        LayoutGroupType::HORIZONTAL_WRAP
    }, LayoutGroupType::VERTICAL);
}

TEST(StreamEnum, LightShadingRoundTrips){
    expectRoundTrip<LightShading>(StreamTestAccess::lightShadingToString, StreamTestAccess::stringToLightShading, {
        LightShading::BOTH,
        LightShading::PIXEL,
        LightShading::VERTEX
    }, LightShading::BOTH);
}

TEST(StreamEnum, LightStateRoundTrips){
    expectRoundTrip<LightState>(StreamTestAccess::lightStateToString, StreamTestAccess::stringToLightState, {
        LightState::OFF,
        LightState::ON,
        LightState::AUTO
    }, LightState::AUTO);
}

TEST(StreamEnum, LightTypeRoundTrips){
    expectRoundTrip<LightType>(StreamTestAccess::lightTypeToString, StreamTestAccess::stringToLightType, {
        LightType::DIRECTIONAL,
        LightType::POINT,
        LightType::SPOT
    }, LightType::DIRECTIONAL);
}

TEST(StreamEnum, MaterialAlphaModeRoundTrips){
    expectRoundTrip<MaterialAlphaMode>(StreamTestAccess::materialAlphaModeToString, StreamTestAccess::stringToMaterialAlphaMode, {
        MaterialAlphaMode::AUTO,
        MaterialAlphaMode::ALPHA_OPAQUE,
        MaterialAlphaMode::MASK,
        MaterialAlphaMode::BLEND
    }, MaterialAlphaMode::AUTO);
}

TEST(StreamEnum, MaterialBlendModeRoundTrips){
    expectRoundTrip<MaterialBlendMode>(StreamTestAccess::materialBlendModeToString, StreamTestAccess::stringToMaterialBlendMode, {
        MaterialBlendMode::AUTO,
        MaterialBlendMode::NONE,
        MaterialBlendMode::ALPHA,
        MaterialBlendMode::PREMULTIPLIED,
        MaterialBlendMode::ADDITIVE,
        MaterialBlendMode::MULTIPLY
    }, MaterialBlendMode::AUTO);
}

TEST(StreamEnum, MaterialCullModeRoundTrips){
    expectRoundTrip<MaterialCullMode>(StreamTestAccess::materialCullModeToString, StreamTestAccess::stringToMaterialCullMode, {
        MaterialCullMode::AUTO,
        MaterialCullMode::BACK,
        MaterialCullMode::FRONT,
        MaterialCullMode::OFF
    }, MaterialCullMode::AUTO);
}

TEST(StreamEnum, MaterialDepthTestRoundTrips){
    expectRoundTrip<MaterialDepthTest>(StreamTestAccess::materialDepthTestToString, StreamTestAccess::stringToMaterialDepthTest, {
        MaterialDepthTest::AUTO,
        MaterialDepthTest::LESS,
        MaterialDepthTest::LESS_EQUAL,
        MaterialDepthTest::EQUAL,
        MaterialDepthTest::GREATER_EQUAL,
        MaterialDepthTest::GREATER,
        MaterialDepthTest::NOT_EQUAL,
        MaterialDepthTest::ALWAYS
    }, MaterialDepthTest::AUTO);
}

TEST(StreamEnum, MaterialDepthWriteRoundTrips){
    expectRoundTrip<MaterialDepthWrite>(StreamTestAccess::materialDepthWriteToString, StreamTestAccess::stringToMaterialDepthWrite, {
        MaterialDepthWrite::AUTO,
        MaterialDepthWrite::ENABLED,
        MaterialDepthWrite::DISABLED
    }, MaterialDepthWrite::AUTO);
}

TEST(StreamEnum, MaterialLightingRoundTrips){
    expectRoundTrip<MaterialLighting>(StreamTestAccess::materialLightingToString, StreamTestAccess::stringToMaterialLighting, {
        MaterialLighting::AUTO,
        MaterialLighting::PIXEL,
        MaterialLighting::VERTEX,
        MaterialLighting::MIXED,
        MaterialLighting::NONE
    }, MaterialLighting::AUTO);
}

TEST(StreamEnum, MaterialStencilCompareRoundTrips){
    expectRoundTrip<MaterialStencilCompare>(StreamTestAccess::materialStencilCompareToString, StreamTestAccess::stringToMaterialStencilCompare, {
        MaterialStencilCompare::AUTO,
        MaterialStencilCompare::DISABLED,
        MaterialStencilCompare::NEVER,
        MaterialStencilCompare::LESS,
        MaterialStencilCompare::EQUAL,
        MaterialStencilCompare::LESS_EQUAL,
        MaterialStencilCompare::GREATER,
        MaterialStencilCompare::NOT_EQUAL,
        MaterialStencilCompare::GREATER_EQUAL,
        MaterialStencilCompare::ALWAYS
    }, MaterialStencilCompare::AUTO);
}

TEST(StreamEnum, MaterialStencilOpRoundTrips){
    expectRoundTrip<MaterialStencilOp>(StreamTestAccess::materialStencilOpToString, StreamTestAccess::stringToMaterialStencilOp, {
        MaterialStencilOp::KEEP,
        MaterialStencilOp::ZERO,
        MaterialStencilOp::REPLACE,
        MaterialStencilOp::INCR_CLAMP,
        MaterialStencilOp::DECR_CLAMP,
        MaterialStencilOp::INVERT,
        MaterialStencilOp::INCR_WRAP,
        MaterialStencilOp::DECR_WRAP
    }, MaterialStencilOp::KEEP);
}

TEST(StreamEnum, ParticleEmitterShapeRoundTrips){
    expectRoundTrip<ParticleEmitterShape>(StreamTestAccess::particleEmitterShapeToString, StreamTestAccess::stringToParticleEmitterShape, {
        ParticleEmitterShape::Box,
        ParticleEmitterShape::Sphere,
        ParticleEmitterShape::Hemisphere,
        ParticleEmitterShape::Circle,
        ParticleEmitterShape::Cone
    }, ParticleEmitterShape::Box);
}

TEST(StreamEnum, PivotPresetRoundTrips){
    expectRoundTrip<PivotPreset>(StreamTestAccess::pivotPresetToString, StreamTestAccess::stringToPivotPreset, {
        PivotPreset::CENTER,
        PivotPreset::TOP_CENTER,
        PivotPreset::BOTTOM_CENTER,
        PivotPreset::LEFT_CENTER,
        PivotPreset::RIGHT_CENTER,
        PivotPreset::TOP_LEFT,
        PivotPreset::BOTTOM_LEFT,
        PivotPreset::TOP_RIGHT,
        PivotPreset::BOTTOM_RIGHT
    }, PivotPreset::BOTTOM_LEFT);
}

TEST(StreamEnum, PrimitiveTypeRoundTrips){
    expectRoundTrip<PrimitiveType>(StreamTestAccess::primitiveTypeToString, StreamTestAccess::stringToPrimitiveType, {
        PrimitiveType::TRIANGLES,
        PrimitiveType::TRIANGLE_STRIP,
        PrimitiveType::POINTS,
        PrimitiveType::LINES
    }, PrimitiveType::TRIANGLES);
}

TEST(StreamEnum, ProgressbarTypeRoundTrips){
    expectRoundTrip<ProgressbarType>(StreamTestAccess::progressbarTypeToString, StreamTestAccess::stringToProgressbarType, {
        ProgressbarType::VERTICAL,
        ProgressbarType::HORIZONTAL
    }, ProgressbarType::HORIZONTAL);
}

TEST(StreamEnum, RigidbodyMotionQualityRoundTrips){
    expectRoundTrip<RigidbodyMotionQuality>(StreamTestAccess::rigidbodyMotionQualityToString, StreamTestAccess::stringToRigidbodyMotionQuality, {
        RigidbodyMotionQuality::LINEAR_CAST,
        RigidbodyMotionQuality::DISCRETE
    }, RigidbodyMotionQuality::DISCRETE);
}

TEST(StreamEnum, ScalingModeRoundTrips){
    expectRoundTrip<Scaling>(StreamTestAccess::scalingModeToString, StreamTestAccess::stringToScalingMode, {
        Scaling::FITWIDTH,
        Scaling::FITHEIGHT,
        Scaling::LETTERBOX,
        Scaling::CROP,
        Scaling::STRETCH,
        Scaling::NATIVE
    }, Scaling::FITWIDTH);
}

TEST(StreamEnum, SceneTypeRoundTrips){
    expectRoundTrip<SceneType>(StreamTestAccess::sceneTypeToString, StreamTestAccess::stringToSceneType, {
        SceneType::SCENE_3D,
        SceneType::SCENE_2D,
        SceneType::SCENE_UI
    }, SceneType::SCENE_3D);
}

TEST(StreamEnum, ScriptPropertyTypeRoundTrips){
    expectRoundTrip<ScriptPropertyType>(StreamTestAccess::scriptPropertyTypeToString, StreamTestAccess::stringToScriptPropertyType, {
        ScriptPropertyType::Bool,
        ScriptPropertyType::Int,
        ScriptPropertyType::Float,
        ScriptPropertyType::String,
        ScriptPropertyType::Vector2,
        ScriptPropertyType::Vector3,
        ScriptPropertyType::Vector4,
        ScriptPropertyType::Color3,
        ScriptPropertyType::Color4,
        ScriptPropertyType::EntityReference,
        ScriptPropertyType::Prefab,
        ScriptPropertyType::Asset
    }, ScriptPropertyType::Bool);
}

TEST(StreamEnum, ScriptTypeRoundTrips){
    expectRoundTrip<ScriptType>(StreamTestAccess::scriptTypeToString, StreamTestAccess::stringToScriptType, {
        ScriptType::CPP,
        ScriptType::LUA
    }, ScriptType::CPP);
}

TEST(StreamEnum, ScrollbarTypeRoundTrips){
    expectRoundTrip<ScrollbarType>(StreamTestAccess::scrollbarTypeToString, StreamTestAccess::stringToScrollbarType, {
        ScrollbarType::VERTICAL,
        ScrollbarType::HORIZONTAL
    }, ScrollbarType::VERTICAL);
}

TEST(StreamEnum, TextureFilterRoundTrips){
    expectRoundTrip<TextureFilter>(StreamTestAccess::textureFilterToString, StreamTestAccess::stringToTextureFilter, {
        TextureFilter::NEAREST,
        TextureFilter::LINEAR,
        TextureFilter::NEAREST_MIPMAP_NEAREST,
        TextureFilter::NEAREST_MIPMAP_LINEAR,
        TextureFilter::LINEAR_MIPMAP_NEAREST,
        TextureFilter::LINEAR_MIPMAP_LINEAR
    }, TextureFilter::LINEAR);
}

TEST(StreamEnum, TextureStrategyRoundTrips){
    expectRoundTrip<TextureStrategy>(StreamTestAccess::textureStrategyToString, StreamTestAccess::stringToTextureStrategy, {
        TextureStrategy::FIT,
        TextureStrategy::RESIZE,
        TextureStrategy::NONE
    }, TextureStrategy::NONE);
}

TEST(StreamEnum, TextureWrapRoundTrips){
    expectRoundTrip<TextureWrap>(StreamTestAccess::textureWrapToString, StreamTestAccess::stringToTextureWrap, {
        TextureWrap::REPEAT,
        TextureWrap::MIRRORED_REPEAT,
        TextureWrap::CLAMP_TO_EDGE,
        TextureWrap::CLAMP_TO_BORDER
    }, TextureWrap::REPEAT);
}

TEST(StreamEnum, UiDocumentRenderModeRoundTrips){
    expectRoundTrip<UIDocumentRenderMode>(StreamTestAccess::uiDocumentRenderModeToString, StreamTestAccess::stringToUIDocumentRenderMode, {
        UIDocumentRenderMode::TEXTURE,
        UIDocumentRenderMode::SCREEN
    }, UIDocumentRenderMode::SCREEN);
}

TEST(StreamEnum, UiEventStateRoundTrips){
    expectRoundTrip<UIEventState>(StreamTestAccess::uiEventStateToString, StreamTestAccess::stringToUIEventState, {
        UIEventState::NOT_SET,
        UIEventState::ENABLED,
        UIEventState::DISABLED
    }, UIEventState::NOT_SET);
}

TEST(StreamEnum, WindingOrderRoundTrips){
    expectRoundTrip<WindingOrder>(StreamTestAccess::windingOrderToString, StreamTestAccess::stringToWindingOrder, {
        WindingOrder::CCW,
        WindingOrder::CW
    }, WindingOrder::CCW);
}

TEST(StreamEnum, WindowModeRoundTrips){
    expectRoundTrip<WindowMode>(StreamTestAccess::windowModeToString, StreamTestAccess::stringToWindowMode, {
        WindowMode::WINDOWED,
        WindowMode::MAXIMIZED,
        WindowMode::FULLSCREEN
    }, WindowMode::WINDOWED);
}

TEST(StreamEnum, ColorFormatKeepsRedAndRgba){
    EXPECT_STREQ(StreamTestAccess::colorFormatToString(ColorFormat::RED), "red");
    EXPECT_STREQ(StreamTestAccess::colorFormatToString(ColorFormat::RGBA), "rgba");
    EXPECT_STREQ(StreamTestAccess::colorFormatToString(ColorFormat::RGBA16F), "rgba");
    EXPECT_EQ(StreamTestAccess::stringToColorFormat("red", 4), ColorFormat::RED);
    EXPECT_EQ(StreamTestAccess::stringToColorFormat("rgba", 1), ColorFormat::RGBA);
    // unknown: from the channel count
    EXPECT_EQ(StreamTestAccess::stringToColorFormat("?", 1), ColorFormat::RED);
    EXPECT_EQ(StreamTestAccess::stringToColorFormat("?", 3), ColorFormat::RGBA);
}

TEST(StreamEnum, ColorWriteRoundTrips){
    for (int mask : {-1, 0, 1, 2, 4, 8, 7, 15, 9}){
        EXPECT_EQ(StreamTestAccess::stringToColorWrite(StreamTestAccess::colorWriteToString(mask)), mask) << mask;
    }
    EXPECT_EQ(StreamTestAccess::stringToColorWrite("AUTO"), -1);
    EXPECT_EQ(StreamTestAccess::stringToColorWrite("RGB"), 7);
    EXPECT_EQ(StreamTestAccess::stringToColorWrite("purple"), -1);
}

TEST(StreamEnum, MaterialPassOrderRoundTrips){
    EXPECT_EQ(StreamTestAccess::stringToMaterialPassOrder(StreamTestAccess::materialPassOrderToString(MaterialPassOrder::BEFORE)), MaterialPassOrder::BEFORE);
    EXPECT_EQ(StreamTestAccess::stringToMaterialPassOrder(StreamTestAccess::materialPassOrderToString(MaterialPassOrder::AFTER)), MaterialPassOrder::AFTER);
    EXPECT_EQ(StreamTestAccess::stringToMaterialPassOrder("sideways"), MaterialPassOrder::AFTER);
}

TEST(StreamEnum, TextureAlphaBorderRoundTrips){
    for (TextureAlphaBorder border : {TextureAlphaBorder::AUTO, TextureAlphaBorder::FIX, TextureAlphaBorder::KEEP})
        EXPECT_EQ(Stream::stringToTextureAlphaBorder(Stream::textureAlphaBorderToString(border)), border);
    EXPECT_EQ(Stream::textureAlphaBorderToString(TextureAlphaBorder::AUTO), "auto");
    EXPECT_EQ(Stream::stringToTextureAlphaBorder("?"), TextureAlphaBorder::AUTO);
}
