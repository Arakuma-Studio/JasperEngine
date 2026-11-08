// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The registry of the component descriptions (docs/specs/S08-reflection.md R1): by ComponentType,
// by key, in the order scenes write them and in the order GameObjects list them.

#include "reflect/Components.h"
#include "reflect/ReflectIndex.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    // ComponentType order (Catalog.h, append-only)
    constexpr const ComponentDesc* kAll[] = {
        &kTransformDesc,
        &kScriptDesc,
        &kMeshDesc,
        &kLineRendererDesc,
        &kLightDesc,
        &kTerrainDesc,
        &kPointsDesc,
        &kSkyboxDesc,
        &kModelDesc,
        &kUIDesc,
        &kRectTransformDesc,
        &kActionDesc,
        &kTweenDesc,
        &kAlphaTweenDesc,
        &kAnimationDesc,
        &kAudioSourceDesc,
        &kRigidbody2DDesc,
        &kRigidbodyDesc,
        &kBoneDesc,
        &kPrefabDesc,
        &kButtonDesc,
        &kCameraDesc,
        &kColorTweenDesc,
        &kFogDesc,
        &kImageDesc,
        &kInstancedMeshDesc,
        &kJoint2DDesc,
        &kJoint3DDesc,
        &kKeyframeTracksDesc,
        &kMeshPolygonDesc,
        &kMorphTracksDesc,
        &kPanelDesc,
        &kParticleSystemDesc,
        &kPolygonDesc,
        &kPositionTweenDesc,
        &kProgressbarDesc,
        &kRotateTracksDesc,
        &kRotationTweenDesc,
        &kScaleTweenDesc,
        &kScaleTracksDesc,
        &kScrollbarDesc,
        &kSpriteAnimationDesc,
        &kSpriteDesc,
        &kTextDesc,
        &kInputFieldDesc,
        &kTilemapDesc,
        &kTranslateTracksDesc,
        &kLayoutGroupDesc,
        &kMirrorDesc,
        &kLight2DDesc,
        &kShadowCaster2DDesc,
        &kReflectionProbeDesc,
        &kUIDocumentDesc,
        &kFluidSimulationDesc,
        &kLatticeDesc,
        &kLatticeModifierDesc,
        &kLatticeFollowerDesc,
        &kLatticeTracksDesc,
        &kLocalVolumetricFogDesc,
        &kVolumetricCloudsDesc,
        &kForceFieldDesc,
        &kCharacterController2DDesc,
        &kCharacterControllerDesc,
        &kWaterVolumeDesc,
        &kBuoyancyDesc,
        &kPoseAnimatorDesc,
        &kAnimationPlayerDesc,
        &kNavMeshSurfaceDesc,
        &kNavMeshAgentDesc,
        &kNavMeshObstacleDesc,
        &kNavMeshLinkDesc,
        &kNavMeshModifierDesc,
        &kNavGridDesc,
        &kSplineDesc,
        &kSplineExtrudeDesc,
        &kSplineInstantiateDesc,
        &kSplineAnimateDesc,
        &kAnimatorDesc,
        &kVertexAnimatorDesc,
        &kTrailRendererDesc,
        &kCanvasGroupDesc,
        &kOnScreenStickDesc,
        &kOnScreenButtonDesc,
        &kTouchAreaDesc,
        &kRagdollDesc,
        &kCameraShakerDesc,
        &kCameraFollowDesc,
        &kTextEffectDesc,
        &kVideoPlayerDesc,
        &kSafeAreaDesc,
        &kCapsuleShadowCasterDesc,
        &kAudioSnapshotDesc,
    };
    static_assert(std::size(kAll) == static_cast<size_t>(LAST_COMPONENT_TYPE) + 1, "a ComponentType without a description");

    // the order of a scene's `components` map (Stream::encodeComponentsLive, decodeComponents)
    constexpr const ComponentDesc* kSerialization[] = {
        &kTransformDesc,
        &kMeshDesc,
        &kUIDesc,
        &kButtonDesc,
        &kScrollbarDesc,
        &kProgressbarDesc,
        &kInputFieldDesc,
        &kPanelDesc,
        &kRectTransformDesc,
        &kLayoutGroupDesc,
        &kTextDesc,
        &kImageDesc,
        &kSpriteDesc,
        &kTilemapDesc,
        &kTerrainDesc,
        &kLightDesc,
        &kLight2DDesc,
        &kShadowCaster2DDesc,
        &kCapsuleShadowCasterDesc,
        &kFogDesc,
        &kForceFieldDesc,
        &kLocalVolumetricFogDesc,
        &kVolumetricCloudsDesc,
        &kMirrorDesc,
        &kReflectionProbeDesc,
        &kUIDocumentDesc,
        &kFluidSimulationDesc,
        &kLatticeDesc,
        &kLatticeModifierDesc,
        &kLatticeFollowerDesc,
        &kLatticeTracksDesc,
        &kPoseAnimatorDesc,
        &kAnimationPlayerDesc,
        &kSplineDesc,
        &kSplineExtrudeDesc,
        &kSplineInstantiateDesc,
        &kSplineAnimateDesc,
        &kAnimatorDesc,
        &kVertexAnimatorDesc,
        &kCameraDesc,
        &kAudioSourceDesc,
        &kScriptDesc,
        &kSkyboxDesc,
        &kModelDesc,
        &kRigidbody2DDesc,
        &kRigidbodyDesc,
        &kJoint2DDesc,
        &kNavMeshSurfaceDesc,
        &kNavMeshAgentDesc,
        &kNavMeshObstacleDesc,
        &kNavMeshLinkDesc,
        &kNavMeshModifierDesc,
        &kNavGridDesc,
        &kCharacterController2DDesc,
        &kWaterVolumeDesc,
        &kBuoyancyDesc,
        &kCharacterControllerDesc,
        &kJoint3DDesc,
        &kActionDesc,
        &kTweenDesc,
        &kPositionTweenDesc,
        &kRotationTweenDesc,
        &kScaleTweenDesc,
        &kColorTweenDesc,
        &kAlphaTweenDesc,
        &kSpriteAnimationDesc,
        &kAnimationDesc,
        &kBoneDesc,
        &kKeyframeTracksDesc,
        &kTranslateTracksDesc,
        &kRotateTracksDesc,
        &kScaleTracksDesc,
        &kMorphTracksDesc,
        &kParticleSystemDesc,
        &kPointsDesc,
        &kLineRendererDesc,
        &kPolygonDesc,
        &kMeshPolygonDesc,
        &kInstancedMeshDesc,
        &kPrefabDesc,
        // upstream's (UpstreamReflect.cpp)
        &kTrailRendererDesc,
        &kCanvasGroupDesc,
        &kOnScreenStickDesc,
        &kOnScreenButtonDesc,
        &kTouchAreaDesc,
        &kRagdollDesc,
        &kCameraShakerDesc,
        &kCameraFollowDesc,
        &kTextEffectDesc,
        &kVideoPlayerDesc,
        &kSafeAreaDesc,
        &kAudioSnapshotDesc,
    };

    // the order a GameObject lists its components in (Catalog::findComponents: the exported code)
    constexpr const ComponentDesc* kListing[] = {
        &kActionDesc,
        &kAlphaTweenDesc,
        &kAnimationDesc,
        &kAudioSourceDesc,
        &kRigidbody2DDesc,
        &kRigidbodyDesc,
        &kBoneDesc,
        &kPrefabDesc,
        &kButtonDesc,
        &kCameraDesc,
        &kCapsuleShadowCasterDesc,
        &kColorTweenDesc,
        &kFogDesc,
        &kLocalVolumetricFogDesc,
        &kVolumetricCloudsDesc,
        &kForceFieldDesc,
        &kMirrorDesc,
        &kReflectionProbeDesc,
        &kUIDocumentDesc,
        &kFluidSimulationDesc,
        &kLatticeDesc,
        &kLatticeModifierDesc,
        &kLatticeFollowerDesc,
        &kLatticeTracksDesc,
        &kLight2DDesc,
        &kShadowCaster2DDesc,
        &kImageDesc,
        &kInstancedMeshDesc,
        &kJoint2DDesc,
        &kNavMeshSurfaceDesc,
        &kNavMeshAgentDesc,
        &kNavMeshObstacleDesc,
        &kNavMeshLinkDesc,
        &kNavMeshModifierDesc,
        &kNavGridDesc,
        &kCharacterController2DDesc,
        &kCharacterControllerDesc,
        &kWaterVolumeDesc,
        &kBuoyancyDesc,
        &kJoint3DDesc,
        &kKeyframeTracksDesc,
        &kLightDesc,
        &kLineRendererDesc,
        &kMeshDesc,
        &kMeshPolygonDesc,
        &kModelDesc,
        &kPoseAnimatorDesc,
        &kAnimationPlayerDesc,
        &kAnimatorDesc,
        &kVertexAnimatorDesc,
        &kMorphTracksDesc,
        &kPanelDesc,
        &kParticleSystemDesc,
        &kPointsDesc,
        &kPolygonDesc,
        &kPositionTweenDesc,
        &kProgressbarDesc,
        &kRotateTracksDesc,
        &kRotationTweenDesc,
        &kScaleTweenDesc,
        &kScaleTracksDesc,
        &kScriptDesc,
        &kScrollbarDesc,
        &kSkyboxDesc,
        &kSplineDesc,
        &kSplineAnimateDesc,
        &kSplineExtrudeDesc,
        &kSplineInstantiateDesc,
        &kSpriteAnimationDesc,
        &kSpriteDesc,
        &kTerrainDesc,
        &kTextDesc,
        &kInputFieldDesc,
        &kTilemapDesc,
        &kTweenDesc,
        &kTransformDesc,
        &kTranslateTracksDesc,
        &kUIDesc,
        &kLayoutGroupDesc,
        &kRectTransformDesc,
        &kTrailRendererDesc,
        &kCanvasGroupDesc,
        &kOnScreenStickDesc,
        &kOnScreenButtonDesc,
        &kTouchAreaDesc,
        &kRagdollDesc,
        &kCameraShakerDesc,
        &kCameraFollowDesc,
        &kTextEffectDesc,
        &kVideoPlayerDesc,
        &kSafeAreaDesc,
        &kAudioSnapshotDesc,
    };

    constexpr const EnumDesc* kEnums[] = {
        &enums::kActionState, &enums::kAnchorPreset, &enums::kAtmosphereQualityOverride, &enums::kAudioSourceAttenuation,
        &enums::kAudioSourceState, &enums::kBodyType, &enums::kBuoyancyColumns, &enums::kCameraType, &enums::kClipWrapMode, &enums::kAnimationLayerBlend, &enums::kAnimatorUpdateMode, &enums::kAnimatorCullingMode, &enums::kAnimatorWriteBones,
        &enums::kAnimatorEventMode, &enums::kAnimatorRetarget, &enums::kAnimatorIKType, &enums::kCascadeSplitMode, &enums::kCloudsPreset,
        &enums::kCloudsRendering, &enums::kCollider2DType, &enums::kCollider3DSource, &enums::kCollider3DType, &enums::kCullingMode,
        &enums::kEaseType, &enums::kFogColorMode, &enums::kFogType, &enums::kFogVolumeFalloff, &enums::kFogVolumeShape,
        &enums::kFluidOutput, &enums::kForceFieldKind, &enums::kForceFieldShape, &enums::kWaterVolumeWaves, &enums::kJoint2DType, &enums::kJoint3DType, &enums::kLatticeInterpolation, &enums::kLatticeJiggleAnchor,
        &enums::kLatticeMaskChannel, &enums::kLatticeOutside, &enums::kLatticeStage, &enums::kLayoutGroupType, &enums::kLightShading,
        &enums::kLightType, &enums::kLightUnit, &enums::kLodFadeMode,
        &enums::kMaterialAlphaMode, &enums::kMaterialBlendMode, &enums::kMaterialCullMode, &enums::kMaterialDepthTest,
        &enums::kMaterialDepthWrite, &enums::kMaterialLighting, &enums::kTerrainQualitySetting, &enums::kTerrainOffscreenShadows,
        &enums::kTerrainMacroMode, &enums::kTerrainCliffProjection, &enums::kMaterialPassOrder, &enums::kMaterialStencilCompare,
        &enums::kMaterialStencilOp, &enums::kParticleEmitterShape, &enums::kPivotPreset, &enums::kPrimitiveType,
        &enums::kProgressbarType, &enums::kReflectionProbeMode, &enums::kReflectionProbeUpdateMode, &enums::kRigidbodyMotionQuality,
        &enums::kButtonTransition, &enums::kNavigationMode, &enums::kScriptType, &enums::kScrollbarType, &enums::kShadowCaster2DShape, &enums::kShadowUpdateMode,
        &enums::kTextureAlphaBorder, &enums::kTextureFilter, &enums::kTextureWrap, &enums::kUIDocumentRenderMode,
        &enums::kUIEventState, &enums::kWindingOrder,
        &enums::kNavCollectObjects, &enums::kNavUseGeometry, &enums::kNavPlane, &enums::kNavAvoidanceQuality, &enums::kNavAgentDriver,
        &enums::kNavObstacleShape, &enums::kNavModifierInclusion, &enums::kNavGridLayout, &enums::kNavGridMovement, &enums::kNavGridSource,
        &enums::kSplineType, &enums::kSplineTangentMode, &enums::kSplineTerrainRegion, &enums::kSplineFlattenMode,
        &enums::kSplineExtrudeShape, &enums::kSplineWidthSource, &enums::kSplineCornerStyle, &enums::kSplineGrade, &enums::kSplineConform,
        &enums::kSplineCollision, &enums::kSplineInstantiateMode, &enums::kSplineSpacingFit, &enums::kSplinePlantMode,
        &enums::kSplineSpanOverrideKind, &enums::kSplineAnimateMethod, &enums::kSplineLoop, &enums::kSplineEasing, &enums::kSplineAlignment,
        &enums::kLineRendererMode, &enums::kLineWidthUnit, &enums::kLineAlignment, &enums::kLineTextureMode, &enums::kCanvasGroupRenderMode, &enums::kOnScreenStickMode,
        &enums::kOnScreenStickAxes, &enums::kOnScreenVisibility, &enums::kJointAnchorSpace, &enums::kCollisionDetection2D, &enums::kSleepMode2D, &enums::kRigidbodyInterpolation, &enums::kJointBreakAction,
        &enums::kJointMotion, &enums::kJointSwingType, &enums::kAlphaTweenTarget, &enums::kTransparencyMode, &enums::kCapsuleCastShadows,
        &enums::kCapsuleDirection,
    };

    bool equalsIgnoreCase(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

}

const ComponentDesc* reflect::find(ComponentType type) {
    const size_t index = static_cast<size_t>(type);
    return index < std::size(kAll) ? kAll[index] : nullptr;
}

const ComponentDesc* reflect::findByKey(std::string_view name) {
    // "LightComponent", "lightcomponent" and "light" name the same; so do the aliases
    std::string_view key = name;
    if (key.size() > 9 && equalsIgnoreCase(key.substr(key.size() - 9), "component")) key = key.substr(0, key.size() - 9);
    for (const ComponentDesc* desc : kAll) {
        if (equalsIgnoreCase(key, desc->key) || equalsIgnoreCase(name, desc->cppName)) return desc;
    }
    for (const ComponentDesc* desc : kAll) {
        for (const char* alias : desc->keyAliases) {
            if (equalsIgnoreCase(key, alias) || equalsIgnoreCase(name, alias)) return desc;
        }
    }
    return nullptr;
}

std::span<const ComponentDesc* const> reflect::all() {
    return kAll;
}

std::span<const ComponentDesc* const> reflect::serializationOrder() {
    return kSerialization;
}

std::span<const ComponentDesc* const> reflect::listingOrder() {
    return kListing;
}

std::span<const EnumDesc* const> reflect::enumTables() {
    return kEnums;
}
