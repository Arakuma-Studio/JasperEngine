// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A material, its extra pass and stencil state (docs/specs/S08-reflection.md P4: the mesh's
// codec writes them; these tables give their properties, schema and asset locators)

#include "reflect/components/MaterialReflect.h"

#include <limits>

namespace jasperengine::editor::reflect {

namespace {

    constexpr uint64_t kShaderReload = UpdateFlags_Shader_Reload;
    constexpr uint64_t kMeshReload = UpdateFlags_Mesh_Reload;

#define JE_STENCIL_FIELDS(flags) \
        JE_FIELD(MaterialStencil, compare, .enumDesc = &enums::kMaterialStencilCompare, .updateFlags = flags), \
        JE_FIELD(MaterialStencil, ref, .updateFlags = flags, .min = 0.0f, .max = 255.0f), \
        JE_FIELD(MaterialStencil, readMask, .updateFlags = flags, .min = 0.0f, .max = 255.0f), \
        JE_FIELD(MaterialStencil, writeMask, .updateFlags = flags, .min = 0.0f, .max = 255.0f), \
        JE_FIELD(MaterialStencil, pass, .enumDesc = &enums::kMaterialStencilOp, .updateFlags = flags), \
        JE_FIELD(MaterialStencil, fail, .enumDesc = &enums::kMaterialStencilOp, .updateFlags = flags), \
        JE_FIELD(MaterialStencil, depthFail, .enumDesc = &enums::kMaterialStencilOp, .updateFlags = flags)

    constexpr FieldDesc kStencilFields[] = {JE_STENCIL_FIELDS(kShaderReload)};
    constexpr FieldDesc kMaterialStencilFields[] = {JE_STENCIL_FIELDS(kMeshReload)};

#undef JE_STENCIL_FIELDS

}

const StructDesc kStencilStruct = JE_STRUCT_DESC(MaterialStencil, "MaterialStencil", kStencilFields);
const StructDesc kMaterialStencilStruct = JE_STRUCT_DESC(MaterialStencil, "MaterialStencil", kMaterialStencilFields);

namespace {

    constexpr FieldDesc kMaterialPassFields[] = {
        JE_FIELD(MaterialPass, shader, .updateFlags = kMeshReload | UpdateFlags_Shader_Uniforms),
        JE_FIELD(MaterialPass, shaderParams, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(MaterialPass, shaderTextures, .ops = &ops::ShaderTextures, .updateFlags = kMeshReload | UpdateFlags_Mesh_Texture),
        JE_FIELD(MaterialPass, shaderFeatures, .ops = &ops::ShaderFeatures, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, blendMode, .enumDesc = &enums::kMaterialBlendMode, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, cullMode, .enumDesc = &enums::kMaterialCullMode, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, depthWrite, .enumDesc = &enums::kMaterialDepthWrite, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, depthTest, .enumDesc = &enums::kMaterialDepthTest, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, depthOffsetFactor, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, depthOffsetUnits, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, stencil, .structDesc = &kMaterialStencilStruct, .flags = ListWhole, .updateFlags = kMeshReload),
        JE_FIELD(MaterialPass, colorWrite, .ops = &ops::ColorWrite, .updateFlags = kMeshReload, .min = -1.0f, .max = 15.0f),
        JE_FIELD(MaterialPass, order, .enumDesc = &enums::kMaterialPassOrder, .updateFlags = kMeshReload),
    };

}

const StructDesc kMaterialPassStruct = JE_STRUCT_DESC(MaterialPass, "MaterialPass", kMaterialPassFields);

namespace {

    constexpr uint64_t kTexture = UpdateFlags_Mesh_Texture;

}

namespace {

    // the built-in Lit's extended lobes and hex tiling (MaterialPbr's table, texture/MaterialPbr.cpp):
    // an enabled flag, a map or its UV set picks the variant (a texture update); the factors are uniforms
    using PBR = MaterialPbrExtensions;
    constexpr float kInf = std::numeric_limits<float>::infinity();

    constexpr FieldDesc kClearcoatFields[] = {
        JE_FIELD(PBR::Clearcoat, enabled, .updateFlags = kTexture, .label = "Clear Coat"),
        JE_FIELD(PBR::Clearcoat, factor, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Clearcoat, roughness, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Clearcoat, normalScale),
        JE_FIELD(PBR::Clearcoat, map, .updateFlags = kTexture),
        JE_FIELD(PBR::Clearcoat, mapTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Map UV Set"),
        JE_FIELD(PBR::Clearcoat, normalMap, .updateFlags = kTexture),
        JE_FIELD(PBR::Clearcoat, normalTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Normal Map UV Set"),
    };
    constexpr FieldDesc kSheenFields[] = {
        JE_FIELD(PBR::Sheen, enabled, .updateFlags = kTexture, .label = "Sheen"),
        JE_FIELD(PBR::Sheen, color, .flags = ColorLinear, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Sheen, roughness, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Sheen, map, .updateFlags = kTexture),
        JE_FIELD(PBR::Sheen, mapTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Map UV Set"),
    };
    constexpr FieldDesc kAnisotropyFields[] = {
        JE_FIELD(PBR::Anisotropy, enabled, .updateFlags = kTexture, .label = "Anisotropy"),
        JE_FIELD(PBR::Anisotropy, strength, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Anisotropy, rotation, .unit = Unit::Radians),
        JE_FIELD(PBR::Anisotropy, map, .updateFlags = kTexture),
        JE_FIELD(PBR::Anisotropy, mapTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Map UV Set"),
    };
    constexpr FieldDesc kIridescenceFields[] = {
        JE_FIELD(PBR::Iridescence, enabled, .updateFlags = kTexture, .label = "Iridescence"),
        JE_FIELD(PBR::Iridescence, factor, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Iridescence, ior, .min = 1.0f, .max = kInf, .label = "IOR"),
        JE_FIELD(PBR::Iridescence, thicknessNm, .min = 0.0f, .max = kInf, .label = "Thickness (nm)"),
        JE_FIELD(PBR::Iridescence, map, .updateFlags = kTexture),
        JE_FIELD(PBR::Iridescence, mapTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Map UV Set"),
    };
    constexpr FieldDesc kTransmissionFields[] = {
        JE_FIELD(PBR::Transmission, enabled, .updateFlags = kTexture, .label = "Transmission"),
        JE_FIELD(PBR::Transmission, factor, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Transmission, map, .updateFlags = kTexture),
        JE_FIELD(PBR::Transmission, mapTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Map UV Set"),
    };
    constexpr FieldDesc kVolumeFields[] = {
        JE_FIELD(PBR::Volume, enabled, .updateFlags = kTexture, .label = "Volume"),
        JE_FIELD(PBR::Volume, thickness, .min = 0.0f, .max = kInf),
        JE_FIELD(PBR::Volume, attenuationColor, .flags = ColorLinear, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Volume, attenuationDistance, .min = 1e-6f, .max = kInf),
    };
    constexpr FieldDesc kSpecularFields[] = {
        JE_FIELD(PBR::Specular, enabled, .updateFlags = kTexture, .label = "Specular & IOR"),
        JE_FIELD(PBR::Specular, factor, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Specular, color, .flags = ColorLinear, .min = 0.0f, .max = kInf),
        JE_FIELD(PBR::Specular, ior, .min = 1.0f, .max = kInf, .label = "IOR"),
        JE_FIELD(PBR::Specular, map, .updateFlags = kTexture),
        JE_FIELD(PBR::Specular, mapTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Map UV Set"),
    };
    constexpr FieldDesc kHexFields[] = {
        JE_FIELD(PBR::Hex, enabled, .updateFlags = kTexture, .label = "Hex Tiling"),
        JE_FIELD(PBR::Hex, rotation, .min = 0.0f, .max = 1.0f),
        JE_FIELD(PBR::Hex, contrast, .min = 0.0f, .max = 1.0f),
    };

    // the Hair shading model (S57 R5): enabled is Shading Model Hair in the Inspector
    constexpr FieldDesc kHairFields[] = {
        JE_FIELD(PBR::Hair, enabled, .updateFlags = kTexture, .label = "Hair"),
        JE_FIELD(PBR::Hair, primaryColor, .flags = ColorLinear, .min = 0.0f, .max = kInf),
        JE_FIELD(PBR::Hair, primaryShift, .min = -1.0f, .max = 1.0f),
        JE_FIELD(PBR::Hair, primaryWidth, .min = 0.01f, .max = 1.0f),
        JE_FIELD(PBR::Hair, secondaryColor, .flags = ColorLinear, .min = 0.0f, .max = kInf),
        JE_FIELD(PBR::Hair, secondaryShift, .min = -1.0f, .max = 1.0f),
        JE_FIELD(PBR::Hair, secondaryWidth, .min = 0.01f, .max = 1.0f),
        JE_FIELD(PBR::Hair, rotation, .unit = Unit::Radians, .label = "Strand Rotation"),
        JE_FIELD(PBR::Hair, flowMap, .updateFlags = kTexture),
        JE_FIELD(PBR::Hair, flowMapTexCoord, .updateFlags = kTexture, .min = 0.0f, .max = 1.0f, .label = "Flow Map UV Set"),
    };

    const StructDesc kClearcoatStruct = JE_STRUCT_DESC(PBR::Clearcoat, "Clearcoat", kClearcoatFields);
    const StructDesc kSheenStruct = JE_STRUCT_DESC(PBR::Sheen, "Sheen", kSheenFields);
    const StructDesc kAnisotropyStruct = JE_STRUCT_DESC(PBR::Anisotropy, "Anisotropy", kAnisotropyFields);
    const StructDesc kIridescenceStruct = JE_STRUCT_DESC(PBR::Iridescence, "Iridescence", kIridescenceFields);
    const StructDesc kTransmissionStruct = JE_STRUCT_DESC(PBR::Transmission, "Transmission", kTransmissionFields);
    const StructDesc kVolumeStruct = JE_STRUCT_DESC(PBR::Volume, "Volume", kVolumeFields);
    const StructDesc kSpecularStruct = JE_STRUCT_DESC(PBR::Specular, "Specular", kSpecularFields);
    const StructDesc kHexStruct = JE_STRUCT_DESC(PBR::Hex, "Hex", kHexFields);
    const StructDesc kHairStruct = JE_STRUCT_DESC(PBR::Hair, "Hair", kHairFields);

    constexpr FieldDesc kPbrFields[] = {
        JE_FIELD(PBR, clearcoat, .ops = &ops::Value<PBR::Clearcoat>, .structDesc = &kClearcoatStruct, .label = "Clear Coat"),
        JE_FIELD(PBR, sheen, .ops = &ops::Value<PBR::Sheen>, .structDesc = &kSheenStruct),
        JE_FIELD(PBR, anisotropy, .ops = &ops::Value<PBR::Anisotropy>, .structDesc = &kAnisotropyStruct),
        JE_FIELD(PBR, iridescence, .ops = &ops::Value<PBR::Iridescence>, .structDesc = &kIridescenceStruct),
        JE_FIELD(PBR, transmission, .ops = &ops::Value<PBR::Transmission>, .structDesc = &kTransmissionStruct),
        JE_FIELD(PBR, volume, .ops = &ops::Value<PBR::Volume>, .structDesc = &kVolumeStruct),
        JE_FIELD(PBR, specular, .ops = &ops::Value<PBR::Specular>, .structDesc = &kSpecularStruct, .label = "Specular & IOR"),
        JE_FIELD(PBR, hex, .ops = &ops::Value<PBR::Hex>, .structDesc = &kHexStruct, .label = "Hex Tiling"),
        JE_FIELD(PBR, hair, .ops = &ops::Value<PBR::Hair>, .structDesc = &kHairStruct),
    };

}

const StructDesc kMaterialPbrStruct = JE_STRUCT_DESC(MaterialPbrExtensions, "MaterialPbrExtensions", kPbrFields);

namespace {

    // the extra pass is made on access (MaterialExtraPass::edit), as editing it always did
    void* extraPassRef(void* material) {
        return &static_cast<Material*>(material)->extraPass.edit();
    }

    // so are the extended lobes (OutOfLineValue::edit)
    void* pbrRef(void* material) {
        return &static_cast<Material*>(material)->pbr.edit();
    }

    // and the palette bindings (S43)
    void* shaderPalettesRef(void* material) {
        return &static_cast<Material*>(material)->shaderPalettes.edit();
    }

    // read, an unset one is its defaults and nothing is made
    const void* extraPassPeek(const void* material) {
        return &static_cast<const Material*>(material)->extraPass.get();
    }

    const void* pbrPeek(const void* material) {
        return &static_cast<const Material*>(material)->pbr.get();
    }

    const void* shaderPalettesPeek(const void* material) {
        return &static_cast<const Material*>(material)->shaderPalettes.get();
    }

    constexpr FieldDesc kMaterialFields[] = {
        JE_FIELD(Material, name, .asset = AssetKind::Material),
        JE_FIELD(Material, baseColorFactor, .flags = ColorLinear, .label = "Base Color"),
        JE_FIELD(Material, metallicFactor, .min = 0.0f, .max = 1.0f, .label = "Metallic"),
        JE_FIELD(Material, roughnessFactor, .min = 0.0f, .max = 1.0f, .label = "Roughness"),
        JE_FIELD(Material, alphaCutoff, .min = 0.0f, .max = 1.0f),
        JE_FIELD(Material, emissiveFactor, .flags = ColorLinear | Hdr, .label = "Emission"),
        JE_FIELD(Material, foliageTransmission, .min = 0.0f, .max = 1.0f),
        JE_FIELD(Material, baseColorTexture, .updateFlags = kTexture),
        JE_FIELD(Material, emissiveTexture, .updateFlags = kTexture),
        JE_FIELD(Material, metallicRoughnessTexture, .updateFlags = kTexture),
        JE_FIELD(Material, occlusionTexture, .updateFlags = kTexture),
        JE_FIELD(Material, normalTexture, .updateFlags = kTexture),
        JE_FIELD(Material, alphaMode, .enumDesc = &enums::kMaterialAlphaMode, .updateFlags = kMeshReload),
        // Stochastic Tiling: the switch picks the variants, the values ride in the uniforms
        JE_FIELD(Material, stochasticTiling, .updateFlags = kMeshReload),
        JE_FIELD(Material, stochasticCellSize),
        JE_FIELD(Material, stochasticBlendSharpness),
        JE_FIELD(Material, stochasticContrast),
        JE_FIELD(Material, stochasticRotation),
        JE_FIELD(Material, lighting, .enumDesc = &enums::kMaterialLighting, .updateFlags = kMeshReload),
        // the UV set of each texture: written by the model import
        JE_FIELD(Material, baseColorTexCoord, .flags = NotListed | Hidden),
        JE_FIELD(Material, metallicRoughnessTexCoord, .flags = NotListed | Hidden),
        JE_FIELD(Material, occlusionTexCoord, .flags = NotListed | Hidden),
        JE_FIELD(Material, emissiveTexCoord, .flags = NotListed | Hidden),
        JE_FIELD(Material, normalTexCoord, .flags = NotListed | Hidden),
        JE_FIELD(Material, shader, .updateFlags = kShaderReload | UpdateFlags_Shader_Uniforms),
        JE_FIELD(Material, shaderParams, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(Material, shaderTextures, .ops = &ops::ShaderTextures, .updateFlags = kMeshReload | kTexture),
        JE_FIELD(Material, shaderFeatures, .ops = &ops::ShaderFeatures, .updateFlags = kMeshReload),
        JE_FIELD(Material, shaderGlobalBindings, .ops = &ops::ShaderGlobalBindings, .flags = Hidden, .updateFlags = UpdateFlags_Shader_Uniforms),
        // the draw binds a changed binding by itself (RenderSystem::updateShaderPalettes)
        detail::makeField<jasperengine::pixelart::ShaderPalettes>("shaderPalettes", &shaderPalettesRef,
                                                                   FieldMeta{.ops = &ops::ShaderPalettes, .flags = Hidden, .peek = &shaderPalettesPeek}),
        JE_FIELD(Material, blendMode, .enumDesc = &enums::kMaterialBlendMode, .updateFlags = kMeshReload),
        JE_FIELD(Material, cullMode, .enumDesc = &enums::kMaterialCullMode, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthWrite, .enumDesc = &enums::kMaterialDepthWrite, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthTest, .enumDesc = &enums::kMaterialDepthTest, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthOffsetFactor, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthOffsetUnits, .updateFlags = kMeshReload),
        JE_FIELD(Material, sortPriority, .updateFlags = kMeshReload),
        JE_FIELD(Material, stencil, .structDesc = &kMaterialStencilStruct, .flags = ListWhole, .updateFlags = kMeshReload),
        JE_FIELD(Material, colorWrite, .ops = &ops::ColorWrite, .updateFlags = kMeshReload, .min = -1.0f, .max = 15.0f),
        detail::makeField<MaterialPass>("extraPass", &extraPassRef,
                                        FieldMeta{.flags = ListWhole, .updateFlags = kMeshReload | kTexture | UpdateFlags_Shader_Uniforms, .peek = &extraPassPeek}),
        JE_FIELD(Material, emissiveStrength),
        detail::makeField<MaterialPbrExtensions>("pbr", &pbrRef,
                                                 FieldMeta{.ops = &ops::Value<MaterialPbrExtensions>, .structDesc = &kMaterialPbrStruct, .peek = &pbrPeek}),
    };

}

const StructDesc kMaterialStruct = JE_STRUCT_DESC(Material, "Material", kMaterialFields);

}
