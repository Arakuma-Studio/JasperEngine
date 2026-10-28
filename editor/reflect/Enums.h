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
    extern const EnumDesc kClipWrapMode;
    extern const EnumDesc kLatticeInterpolation;
    extern const EnumDesc kLatticeOutside;
    extern const EnumDesc kLatticeJiggleAnchor;
    extern const EnumDesc kLatticeStage;
    extern const EnumDesc kLatticeMaskChannel;
    extern const EnumDesc kLightUnit;
    extern const EnumDesc kLightShading;
    extern const EnumDesc kLightType;
    extern const EnumDesc kMaterialAlphaMode;
    extern const EnumDesc kMaterialBlendMode;
    extern const EnumDesc kMaterialCullMode;
    extern const EnumDesc kMaterialDepthTest;
    extern const EnumDesc kMaterialDepthWrite;
    extern const EnumDesc kMaterialLighting;
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
    extern const EnumDesc kScrollbarType;
    extern const EnumDesc kShadowCaster2DShape;
    extern const EnumDesc kShadowUpdateMode;
    extern const EnumDesc kTextureAlphaBorder;
    extern const EnumDesc kTextureFilter;
    extern const EnumDesc kTextureWrap;
    extern const EnumDesc kUIDocumentRenderMode;
    extern const EnumDesc kUIEventState;
    extern const EnumDesc kWindingOrder;

    // the values the Inspector and MCP list, in their order (the YAML order unless a table says
    // otherwise), hidden ones left out
    std::vector<const EnumItem*> offered(const EnumDesc& desc, uint64_t mask = 0);

    // masks of enum values a field offers (FieldDesc::enumMask)
    constexpr uint64_t bit(int value) { return uint64_t(1) << value; }

}
