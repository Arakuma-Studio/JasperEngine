// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A material, its extra pass and stencil state (docs/specs/S08-reflection.md P4: the mesh's
// codec writes them; these tables give their properties, schema and asset locators)

#include "reflect/components/MaterialReflect.h"

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
        JE_FIELD(MaterialPass, colorWrite, .updateFlags = kMeshReload, .min = -1.0f, .max = 15.0f),
        JE_FIELD(MaterialPass, order, .enumDesc = &enums::kMaterialPassOrder, .updateFlags = kMeshReload),
    };

}

const StructDesc kMaterialPassStruct = JE_STRUCT_DESC(MaterialPass, "MaterialPass", kMaterialPassFields);

namespace {

    constexpr uint64_t kTexture = UpdateFlags_Mesh_Texture;

    // the extra pass is made on access (MaterialExtraPass::edit), as editing it always did
    void* extraPassRef(void* material) {
        return &static_cast<Material*>(material)->extraPass.edit();
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
        JE_FIELD(Material, blendMode, .enumDesc = &enums::kMaterialBlendMode, .updateFlags = kMeshReload),
        JE_FIELD(Material, cullMode, .enumDesc = &enums::kMaterialCullMode, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthWrite, .enumDesc = &enums::kMaterialDepthWrite, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthTest, .enumDesc = &enums::kMaterialDepthTest, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthOffsetFactor, .updateFlags = kMeshReload),
        JE_FIELD(Material, depthOffsetUnits, .updateFlags = kMeshReload),
        JE_FIELD(Material, sortPriority, .updateFlags = kMeshReload),
        JE_FIELD(Material, stencil, .structDesc = &kMaterialStencilStruct, .flags = ListWhole, .updateFlags = kMeshReload),
        JE_FIELD(Material, colorWrite, .updateFlags = kMeshReload, .min = -1.0f, .max = 15.0f),
        detail::makeField<MaterialPass>("extraPass", &extraPassRef,
                                        FieldMeta{.flags = ListWhole, .updateFlags = kMeshReload | kTexture | UpdateFlags_Shader_Uniforms}),
    };

}

const StructDesc kMaterialStruct = JE_STRUCT_DESC(Material, "Material", kMaterialFields);

}
