// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// One table per enum of the components (docs/specs/S08-reflection.md R5): the YAML token, the MCP
// token where it differs, the Inspector label and the C++ name of each value, older spellings as
// aliases. The scene codec (StreamSchema's tables), MCP's options, the Inspector's dropdowns and
// the exported code all read these.

#include "reflect/Reflect.h"

namespace jasperengine::editor::reflect::enums {

    extern const EnumDesc kActionState;
    extern const EnumDesc kAnchorPreset;
    extern const EnumDesc kAtmosphereQualityOverride;
    extern const EnumDesc kAudioSourceAttenuation;
    extern const EnumDesc kAudioSourceState;
    extern const EnumDesc kBodyType;
    extern const EnumDesc kCameraType;
    extern const EnumDesc kCascadeSplitMode;
    extern const EnumDesc kCloudsPreset;
    extern const EnumDesc kCloudsRendering;
    extern const EnumDesc kCollider2DType;
    extern const EnumDesc kCollider3DSource;
    extern const EnumDesc kCollider3DType;
    extern const EnumDesc kCullingMode;
    extern const EnumDesc kEaseType;
    extern const EnumDesc kFogColorMode;
    extern const EnumDesc kFogType;
    extern const EnumDesc kFogVolumeFalloff;
    extern const EnumDesc kFogVolumeShape;
    extern const EnumDesc kJoint2DType;
    extern const EnumDesc kJoint3DType;
    extern const EnumDesc kLayoutGroupType;
    extern const EnumDesc kFluidOutput;
    extern const EnumDesc kForceFieldShape;
    extern const EnumDesc kForceFieldKind;
    extern const EnumDesc kWaterVolumeWaves;
    extern const EnumDesc kBuoyancyColumns;
    // upstream's components (UpstreamReflect.cpp: Trail Renderer, Canvas Group, on-screen controls)
    extern const EnumDesc kLineRendererMode;
    extern const EnumDesc kLineWidthUnit;
    extern const EnumDesc kLineAlignment;
    extern const EnumDesc kLineTextureMode;
    extern const EnumDesc kCanvasGroupRenderMode;
    extern const EnumDesc kOnScreenStickMode;
    extern const EnumDesc kOnScreenStickAxes;
    extern const EnumDesc kOnScreenVisibility;
    extern const EnumDesc kJointAnchorSpace;
    extern const EnumDesc kCollisionDetection2D;
    extern const EnumDesc kSleepMode2D;
    extern const EnumDesc kRigidbodyInterpolation;
    extern const EnumDesc kJointBreakAction;
    extern const EnumDesc kJointMotion;
    extern const EnumDesc kJointSwingType;
    extern const EnumDesc kAlphaTweenTarget;
    extern const EnumDesc kClipWrapMode;
    extern const EnumDesc kAnimationLayerBlend;
    extern const EnumDesc kAnimatorUpdateMode;
    extern const EnumDesc kAnimatorCullingMode;
    extern const EnumDesc kAnimatorWriteBones;
    extern const EnumDesc kAnimatorEventMode;
    extern const EnumDesc kAnimatorRetarget;
    extern const EnumDesc kAnimatorIKType;
    extern const EnumDesc kLatticeInterpolation;
    extern const EnumDesc kLatticeOutside;
    extern const EnumDesc kLatticeJiggleAnchor;
    extern const EnumDesc kLatticeStage;
    extern const EnumDesc kLatticeMaskChannel;
    extern const EnumDesc kLightUnit;
    extern const EnumDesc kLightShading;
    extern const EnumDesc kLightType;
    extern const EnumDesc kLodFadeMode;
    extern const EnumDesc kMaterialAlphaMode;
    extern const EnumDesc kMaterialBlendMode;
    extern const EnumDesc kMaterialCullMode;
    extern const EnumDesc kMaterialDepthTest;
    extern const EnumDesc kMaterialDepthWrite;
    extern const EnumDesc kMaterialLighting;
    extern const EnumDesc kTerrainQualitySetting;
    extern const EnumDesc kTerrainOffscreenShadows;
    extern const EnumDesc kTerrainMacroMode;
    extern const EnumDesc kTerrainCliffProjection;
    extern const EnumDesc kMaterialPassOrder;
    extern const EnumDesc kMaterialStencilCompare;
    extern const EnumDesc kMaterialStencilOp;
    extern const EnumDesc kParticleEmitterShape;
    extern const EnumDesc kPivotPreset;
    extern const EnumDesc kPrimitiveType;
    extern const EnumDesc kProgressbarType;
    extern const EnumDesc kReflectionProbeMode;
    extern const EnumDesc kReflectionProbeUpdateMode;
    extern const EnumDesc kRigidbodyMotionQuality;
    extern const EnumDesc kScriptType;
    extern const EnumDesc kButtonTransition;
    extern const EnumDesc kNavigationMode;
    extern const EnumDesc kScrollbarType;
    extern const EnumDesc kShadowCaster2DShape;
    extern const EnumDesc kShadowUpdateMode;
    extern const EnumDesc kTransparencyMode;
    // the Capsule Shadow Caster's (S57, LightingReflect.cpp)
    extern const EnumDesc kCapsuleCastShadows;
    extern const EnumDesc kCapsuleDirection;
    extern const EnumDesc kTextureAlphaBorder;
    extern const EnumDesc kTextureFilter;
    extern const EnumDesc kTextureWrap;
    extern const EnumDesc kUIDocumentRenderMode;
    extern const EnumDesc kUIEventState;
    extern const EnumDesc kWindingOrder;
    // navigation (S19, editor/reflect/components/NavigationReflect.cpp)
    extern const EnumDesc kNavCollectObjects;
    extern const EnumDesc kNavUseGeometry;
    extern const EnumDesc kNavPlane;
    extern const EnumDesc kNavAvoidanceQuality;
    extern const EnumDesc kNavAgentDriver;
    extern const EnumDesc kNavObstacleShape;
    extern const EnumDesc kNavModifierInclusion;
    extern const EnumDesc kNavGridLayout;
    extern const EnumDesc kNavGridMovement;
    extern const EnumDesc kNavGridSource;
    // the spline components' (editor/reflect/components/SplinesReflect.cpp)
    extern const EnumDesc kSplineType;
    extern const EnumDesc kSplineTangentMode;
    extern const EnumDesc kSplineTerrainRegion;
    extern const EnumDesc kSplineFlattenMode;
    extern const EnumDesc kSplineExtrudeShape;
    extern const EnumDesc kSplineWidthSource;
    extern const EnumDesc kSplineCornerStyle;
    extern const EnumDesc kSplineGrade;
    extern const EnumDesc kSplineConform;
    extern const EnumDesc kSplineCollision;
    extern const EnumDesc kSplineInstantiateMode;
    extern const EnumDesc kSplineSpacingFit;
    extern const EnumDesc kSplinePlantMode;
    extern const EnumDesc kSplineSpanOverrideKind;
    extern const EnumDesc kSplineAnimateMethod;
    extern const EnumDesc kSplineLoop;
    extern const EnumDesc kSplineEasing;
    extern const EnumDesc kSplineAlignment;

    // the values the Inspector and MCP list, in their order (the YAML order unless a table says
    // otherwise), hidden ones left out
    std::vector<const EnumItem*> offered(const EnumDesc& desc, uint64_t mask = 0);

    // masks of enum values a field offers (FieldDesc::enumMask)
    constexpr uint64_t bit(int value) { return uint64_t(1) << value; }

}
