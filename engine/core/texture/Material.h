// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MATERIAL_H
#define MATERIAL_H

#include "math/Vector3.h"
#include "texture/Texture.h"
#include "shader/ShaderUniforms.h"
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    // AUTO preserves JasperEngine's historical texture-alpha detection for materials
    // created in the editor. The other values map directly to glTF alphaMode.
    // The editor's generic enum property path stores enum values as int, matching
    // the engine's other editable enums.
    enum class MaterialAlphaMode {
        AUTO,
        ALPHA_OPAQUE,
        MASK,
        BLEND
    };

    // Render state of a material (shader library). AUTO resolves to the shader manifest's
    // @state default, else to the engine's legacy behaviour, so a default material renders
    // exactly as before. NONE in MaterialBlendMode means opaque (no blending); the value is
    // not called OPAQUE because wingdi.h defines that name as a macro.
    enum class MaterialBlendMode {
        AUTO,
        NONE,
        ALPHA,
        PREMULTIPLIED,
        ADDITIVE,
        MULTIPLY
    };

    enum class MaterialCullMode {
        AUTO,
        BACK,
        FRONT,
        OFF
    };

    enum class MaterialDepthWrite {
        AUTO,
        ENABLED,
        DISABLED
    };

    enum class MaterialDepthTest {
        AUTO,
        LESS,
        LESS_EQUAL,
        EQUAL,
        GREATER_EQUAL,
        GREATER,
        NOT_EQUAL,
        ALWAYS
    };

    // Which shader stage evaluates the object's lights. MIXED follows each light's
    // LightComponent::shading. AUTO is the shader's default (built-in Lit: PIXEL).
    enum class MaterialLighting {
        AUTO,
        PIXEL,
        VERTEX,
        MIXED
    };

    struct JASPERENGINE_API Material{
        // --- start shader part
        Vector4 baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);  // linear color
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float alphaCutoff = 0.5f;
        uint8_t _pad_28[4];
        Vector3 emissiveFactor = Vector3(0.0f, 0.0f, 0.0f);  // linear color
        float foliageTransmission = 0.0f; // thin-leaf backlighting, 0 disables
        // --- end shader part

        Texture baseColorTexture;
        Texture emissiveTexture;
        Texture metallicRoughnessTexture;
        Texture occlusionTexture;
        Texture normalTexture;

        MaterialAlphaMode alphaMode = MaterialAlphaMode::AUTO;
        MaterialLighting lighting = MaterialLighting::AUTO;

        // glTF texCoord index (UV set) each texture samples: 0 = primary (a_texcoord1),
        // 1 = secondary (a_texcoord2). Only two sets are supported; higher indices are
        // clamped to 1 on load. Not part of the shader UBO prefix; uploaded separately.
        int baseColorTexCoord = 0;
        int metallicRoughnessTexCoord = 0;
        int occlusionTexCoord = 0;
        int emissiveTexCoord = 0;
        int normalTexCoord = 0;

        // material name or file
        std::string name = "";

        // --- shader library (docs/superpowers/specs/2026-10-10-shader-library-phase1-design.md)
        // "" = built-in Lit, "library:<name>" = engine library shader, else a project fork
        std::string shader;
        // @property values by member name; an unset member takes the manifest default
        ShaderUniformValues shaderParams;
        // @texture bindings by uniform name; an unset texture binds the manifest default
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        // enabled @feature defines and the chosen @keywords defines
        std::vector<std::string> shaderFeatures;

        MaterialBlendMode blendMode = MaterialBlendMode::AUTO;
        MaterialCullMode cullMode = MaterialCullMode::AUTO;
        MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
        MaterialDepthTest depthTest = MaterialDepthTest::AUTO;
        float depthOffsetFactor = 0.0f; // polygon offset, slope scale
        float depthOffsetUnits = 0.0f;  // polygon offset, constant
        int sortPriority = 0;           // transparent queue order, lower draws first

        bool operator == (const Material& other) const {
            return baseColorFactor == other.baseColorFactor &&
                   metallicFactor == other.metallicFactor &&
                   roughnessFactor == other.roughnessFactor &&
                   alphaCutoff == other.alphaCutoff &&
                   emissiveFactor == other.emissiveFactor &&
                   foliageTransmission == other.foliageTransmission &&
                   baseColorTexture == other.baseColorTexture &&
                   emissiveTexture == other.emissiveTexture &&
                   metallicRoughnessTexture == other.metallicRoughnessTexture &&
                   occlusionTexture == other.occlusionTexture &&
                   normalTexture == other.normalTexture &&
                   alphaMode == other.alphaMode &&
                   lighting == other.lighting &&
                   baseColorTexCoord == other.baseColorTexCoord &&
                   metallicRoughnessTexCoord == other.metallicRoughnessTexCoord &&
                   occlusionTexCoord == other.occlusionTexCoord &&
                   emissiveTexCoord == other.emissiveTexCoord &&
                   normalTexCoord == other.normalTexCoord &&
                   name == other.name &&
                   shader == other.shader &&
                   shaderParams == other.shaderParams &&
                   shaderTextures == other.shaderTextures &&
                   shaderFeatures == other.shaderFeatures &&
                   blendMode == other.blendMode &&
                   cullMode == other.cullMode &&
                   depthWrite == other.depthWrite &&
                   depthTest == other.depthTest &&
                   depthOffsetFactor == other.depthOffsetFactor &&
                   depthOffsetUnits == other.depthOffsetUnits &&
                   sortPriority == other.sortPriority;
        }

        bool operator != (const Material& other) const {
            return !(*this == other);
        }
    };

}

#endif //MATERIAL_H
