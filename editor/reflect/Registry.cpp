// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The registry of the component descriptions (docs/specs/S08-reflection.md R1): by ComponentType,
// by key, in the order scenes write them and in the order GameObjects list them, and the checks
// the tables must pass (ReflectRegistry.SelfCheck).

#include "reflect/Components.h"
#include "reflect/ReflectIndex.h"

#include <algorithm>
#include <cctype>
#include <set>

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
        &enums::kLightType, &enums::kLightUnit,
        &enums::kMaterialAlphaMode, &enums::kMaterialBlendMode, &enums::kMaterialCullMode, &enums::kMaterialDepthTest,
        &enums::kMaterialDepthWrite, &enums::kMaterialLighting, &enums::kMaterialPassOrder, &enums::kMaterialStencilCompare,
        &enums::kMaterialStencilOp, &enums::kParticleEmitterShape, &enums::kPivotPreset, &enums::kPrimitiveType,
        &enums::kProgressbarType, &enums::kReflectionProbeMode, &enums::kReflectionProbeUpdateMode, &enums::kRigidbodyMotionQuality,
        &enums::kScriptType, &enums::kScrollbarType, &enums::kShadowCaster2DShape, &enums::kShadowUpdateMode,
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
        &enums::kJointMotion, &enums::kJointSwingType, &enums::kAlphaTweenTarget,
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

// ---------------------------------------------------------------- self check

namespace {

    // ownCodec: the component writes its YAML with a codec of its own, the fields need none
    void checkStruct(const std::string& owner, const StructDesc& desc, std::vector<std::string>& problems, std::set<const StructDesc*>& seen,
                     bool ownCodec) {
        if (!seen.insert(&desc).second) return;
        std::set<std::string> names;
        std::set<std::string> keys;
        for (const FieldDesc& field : desc.fields) {
            const std::string where = owner + "." + field.name;
            if (!names.insert(field.name).second) problems.push_back(where + ": the name is used twice");
            if (isSaved(field) && !keys.insert(field.yamlKey).second) problems.push_back(where + ": the YAML key " + field.yamlKey + " is used twice");
            if (!field.ops) {
                problems.push_back(where + ": no TypeOps");
                continue;
            }
            if (!ownCodec && isSaved(field) && !field.ops->toYaml && !field.ops->scalar && !field.ops->encodeKey && !field.ops->fromYaml && !field.ops->decodeKey) {
                problems.push_back(where + ": saved but its type has no YAML codec");
            }
            if (field.kind == Kind::Enum && !field.enumDesc) problems.push_back(where + ": an enum without an EnumDesc");
            if (field.kind == Kind::Struct && !field.structDesc) problems.push_back(where + ": a struct without a StructDesc");
            if (field.kind == Kind::Array && (!field.arrayOps || (!field.elementOps && !field.structDesc))) {
                problems.push_back(where + ": an array without element ops");
            }
            if ((field.flags & Proxy) && desc.defaults) {
                // a proxy resolves inside its owner (the default instance's storage)
                const char* begin = static_cast<const char*>(desc.defaults());
                const char* ref = static_cast<const char*>(field.ref(const_cast<void*>(desc.defaults())));
                if (!desc.ops || ref < begin || ref + field.ops->size > begin + desc.ops->size) problems.push_back(where + ": a proxy outside its owner");
            }
            if (field.structDesc) checkStruct(owner + "." + field.name, *field.structDesc, problems, seen, ownCodec);
        }
        // an alias never names a live field
        for (const FieldDesc& field : desc.fields) {
            for (const char* alias : field.aliases) {
                if (names.count(alias)) problems.push_back(owner + "." + field.name + ": the alias " + alias + " is a field's name");
                if (keys.count(alias)) problems.push_back(owner + "." + field.name + ": the alias " + alias + " is a field's YAML key");
            }
        }
        std::set<std::string> aliases;
        for (const FieldDesc& field : desc.fields) {
            for (const char* alias : field.aliases) {
                if (!aliases.insert(alias).second) problems.push_back(owner + ": the alias " + std::string(alias) + " is used twice");
            }
        }
    }

    void checkEnum(const EnumDesc& desc, std::vector<std::string>& problems) {
        const std::string owner = desc.cppType;
        std::set<std::string> tokens;
        std::set<int> values;
        for (const EnumItem& item : desc.items) {
            if (!tokens.insert(item.token).second) problems.push_back(owner + ": the token " + item.token + " is used twice");
            if (item.mcpToken && std::string_view(item.mcpToken) != item.token && !tokens.insert(item.mcpToken).second) {
                problems.push_back(owner + ": the MCP token " + std::string(item.mcpToken) + " is used twice");
            }
            if (!values.insert(item.value).second) problems.push_back(owner + ": the value " + std::to_string(item.value) + " is listed twice");
        }
        for (const EnumItem& item : desc.items) {
            for (const char* alias : item.aliases) {
                if (!tokens.insert(alias).second) problems.push_back(owner + ": the alias " + std::string(alias) + " is a token");
            }
            // token -> value -> token
            const EnumItem* found = enumItemByToken(desc, item.token);
            if (!found || found->value != item.value || std::string_view(enumToken(desc, found->value)) != item.token) {
                problems.push_back(owner + ": " + item.token + " does not round-trip");
            }
            if (item.mcpToken) {
                const EnumItem* byMcp = enumItemByToken(desc, item.mcpToken);
                if (!byMcp || byMcp->value != item.value) problems.push_back(owner + ": the MCP token " + std::string(item.mcpToken) + " does not round-trip");
            }
        }
        if (!enumItem(desc, desc.fallback)) problems.push_back(owner + ": the fallback is not a value");
        for (int value : desc.uiOrder) {
            if (!enumItem(desc, value)) problems.push_back(owner + ": the UI order lists " + std::to_string(value) + ", not a value");
        }
        if (!desc.uiOrder.empty() && desc.uiOrder.size() != desc.items.size()) problems.push_back(owner + ": the UI order misses values");
    }

}

std::vector<std::string> reflect::selfCheck() {
    std::vector<std::string> problems;
    std::set<std::string> keys;
    std::set<const StructDesc*> seen;
    for (size_t i = 0; i < std::size(kAll); i++) {
        const ComponentDesc& desc = *kAll[i];
        if (static_cast<size_t>(desc.type) != i) problems.push_back(std::string(desc.cppName) + ": listed at the place of another type");
        if (!keys.insert(desc.key).second) problems.push_back(std::string(desc.cppName) + ": the key " + desc.key + " is used twice");
        for (const char* alias : desc.keyAliases) {
            if (!keys.insert(alias).second) problems.push_back(std::string(desc.cppName) + ": the key alias " + alias + " is used twice");
        }
        if (!desc.find || !desc.add || !desc.componentId) problems.push_back(std::string(desc.cppName) + ": no storage access");
        if (desc.codec && (!desc.codec->encode || !desc.codec->decode)) problems.push_back(std::string(desc.cppName) + ": an incomplete codec");
        if (!desc.codegen && !desc.codegenVar) problems.push_back(std::string(desc.cppName) + ": no exported code");
        checkStruct(desc.cppName, desc.body, problems, seen, desc.codec != nullptr);
        for (const char* name : desc.codegenOrder) {
            if (!findSegment(desc.body, name)) problems.push_back(std::string(desc.cppName) + ": the codegen order lists " + name + ", not a field");
        }
    }
    for (const auto* list : {&kSerialization, &kListing}) {
        std::set<const ComponentDesc*> listed(std::begin(*list), std::end(*list));
        if (listed.size() != std::size(kAll)) problems.push_back("an order list misses or repeats a component");
    }
    for (const EnumDesc* desc : kEnums) checkEnum(*desc, problems);
    return problems;
}
