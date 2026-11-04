// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MATERIAL_H
#define MATERIAL_H

#include "asset/AssetRef.h"
#include "math/Vector3.h"
#include "pixelart/PaletteBinding.h"
#include "texture/Texture.h"
#include "texture/MaterialPbr.h"
#include "util/OutOfLineValue.h"
#include "shader/ShaderUniforms.h"
#include <memory>
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
    // LightComponent::shading. AUTO is the shader's default (built-in Lit: PIXEL); NONE draws
    // a shader that lists it (manifest @lighting none; the built-in Lit lists every mode) unlit,
    // in its base colour: what a glTF material with KHR_materials_unlit loads as. A mode the
    // shader does not list falls back to its default (RenderSystem::resolveLighting).
    enum class MaterialLighting {
        AUTO,
        PIXEL,
        VERTEX,
        MIXED,
        NONE
    };

    // Stencil test of a draw (shader library, phase 2). AUTO takes the shader manifest's
    // @state stencil rule, DISABLED turns stencil off; any other value makes the whole
    // MaterialStencil explicit.
    enum class MaterialStencilCompare {
        AUTO,
        DISABLED,
        NEVER,
        LESS,
        EQUAL,
        LESS_EQUAL,
        GREATER,
        NOT_EQUAL,
        GREATER_EQUAL,
        ALWAYS
    };

    enum class MaterialStencilOp {
        KEEP,
        ZERO,
        REPLACE,
        INCR_CLAMP,
        DECR_CLAMP,
        INVERT,
        INCR_WRAP,
        DECR_WRAP
    };

    // same state for front and back faces
    struct JASPERENGINE_API MaterialStencil {
        MaterialStencilCompare compare = MaterialStencilCompare::AUTO;
        int ref = 0;          // 0..255
        int readMask = 255;   // 0..255
        int writeMask = 255;  // 0..255
        MaterialStencilOp pass = MaterialStencilOp::KEEP;
        MaterialStencilOp fail = MaterialStencilOp::KEEP;
        MaterialStencilOp depthFail = MaterialStencilOp::KEEP;

        bool operator == (const MaterialStencil& other) const {
            return compare == other.compare && ref == other.ref && readMask == other.readMask &&
                   writeMask == other.writeMask && pass == other.pass && fail == other.fail &&
                   depthFail == other.depthFail;
        }
        bool operator != (const MaterialStencil& other) const { return !(*this == other); }
    };

    // colour channels a draw writes: -1 = AUTO (manifest @state colorwrite, else all),
    // otherwise a mask of these bits (0 = depth/stencil only)
    enum MaterialColorWriteBits {
        MATERIAL_COLOR_WRITE_R = 1,
        MATERIAL_COLOR_WRITE_G = 2,
        MATERIAL_COLOR_WRITE_B = 4,
        MATERIAL_COLOR_WRITE_A = 8,
        MATERIAL_COLOR_WRITE_ALL = 15,
        // named masks (Lua's MaterialColorWrite)
        MATERIAL_COLOR_WRITE_NONE = 0,
        MATERIAL_COLOR_WRITE_RGB = 7,
        MATERIAL_COLOR_WRITE_AUTO = -1
    };

    enum class MaterialPassOrder {
        AFTER,  // drawn right after the submesh's main draw
        BEFORE  // drawn right before it
    };

    // One optional extra draw of a submesh with its own shader and state (outlines,
    // stencil masks). An empty shader means no extra pass and costs nothing.
    struct JASPERENGINE_API MaterialPass {
        ShaderRef shader;
        ShaderUniformValues shaderParams;
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        std::vector<std::string> shaderFeatures;
        MaterialBlendMode blendMode = MaterialBlendMode::AUTO;
        MaterialCullMode cullMode = MaterialCullMode::AUTO;
        MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
        MaterialDepthTest depthTest = MaterialDepthTest::AUTO;
        float depthOffsetFactor = 0.0f;
        float depthOffsetUnits = 0.0f;
        MaterialStencil stencil;
        int colorWrite = -1;
        MaterialPassOrder order = MaterialPassOrder::AFTER;

        bool operator == (const MaterialPass& other) const {
            return shader == other.shader && shaderParams == other.shaderParams &&
                   shaderTextures == other.shaderTextures && shaderFeatures == other.shaderFeatures &&
                   blendMode == other.blendMode && cullMode == other.cullMode &&
                   depthWrite == other.depthWrite && depthTest == other.depthTest &&
                   depthOffsetFactor == other.depthOffsetFactor && depthOffsetUnits == other.depthOffsetUnits &&
                   stencil == other.stencil && colorWrite == other.colorWrite && order == other.order;
        }
        bool operator != (const MaterialPass& other) const { return !(*this == other); }
    };

    // Material::extraPass: a MaterialPass kept out of line, so the materials without one (most
    // of them, 16 inline in every MeshComponent of a game build) pay a pointer instead of the
    // whole pass (see OutOfLineValue)
    using MaterialExtraPass = OutOfLineValue<MaterialPass>;

    struct JASPERENGINE_API Material{
        // --- start shader part
        Vector4 baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);  // linear color
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float alphaCutoff = 0.5f;
        uint8_t _pad_28[4];
        Vector3 emissiveFactor = Vector3(0.0f, 0.0f, 0.0f);  // linear color
        float foliageTransmission = 0.0f; // thin-leaf backlighting, 0 disables
        // Stochastic Tiling's values (stochasticTiling below; includes/stochastic.glsl): floats
        // 12-15 of the shader part, uploaded (16 floats) only to the variants that have it ('Sto')

        // Stochastic Tiling: the width of a hex cell in texture repeats (0.25-4)
        float stochasticCellSize = 1.0f;
        // Stochastic Tiling: how narrow the blend between neighbouring cells is (0-1)
        float stochasticBlendSharpness = 0.4f;
        // Stochastic Tiling: keeps the texture's contrast where cells blend (0-1, 0 blends linearly)
        float stochasticContrast = 0.45f;
        // Stochastic Tiling: a random turn per cell, the share of +-180 degrees (0-1)
        float stochasticRotation = 0.0f;
        // --- end shader part

        Texture baseColorTexture;
        Texture emissiveTexture;
        Texture metallicRoughnessTexture;
        Texture occlusionTexture;
        Texture normalTexture;

        MaterialAlphaMode alphaMode = MaterialAlphaMode::AUTO;
        MaterialLighting lighting = MaterialLighting::AUTO;

        // Stochastic Tiling of the built-in Lit material: its texture maps sampled with a random
        // offset (and turn) per hex cell, blended, so a tiled texture shows no repetition. Off by
        // default; the four values above shape it. The colour, depth (alpha clip) and G-buffer
        // passes all sample the same way. Not for terrains (their layers have their own), sprite
        // atlases (Texture Rect: the offsets would read neighbouring frames) or shaders with a
        // manifest (the library shaders' STOCHASTIC feature).
        bool stochasticTiling = false;

        // glTF texCoord index (UV set) each texture samples: 0 = primary (a_texcoord1),
        // 1 = secondary (a_texcoord2). Only two sets are supported; higher indices are
        // clamped to 1 on load. Not part of the shader UBO prefix; uploaded separately.
        int baseColorTexCoord = 0;
        int metallicRoughnessTexCoord = 0;
        int occlusionTexCoord = 0;
        int emissiveTexCoord = 0;
        int normalTexCoord = 0;

        // the linked .material file (project-relative), empty when the material is not linked
        AssetRef name;

        // --- shader library (docs/superpowers/specs/2026-10-10-shader-library-phase1-design.md)
        // "" = built-in Lit, "library:<name>" = engine library shader, else a project fork
        ShaderRef shader;
        // @property values by member name; an unset member takes the manifest default
        ShaderUniformValues shaderParams;
        // @texture bindings by uniform name; an unset texture binds the manifest default
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        // enabled @feature defines and the chosen @keywords defines
        std::vector<std::string> shaderFeatures;
        // custom-block members that follow a global shader value (Shader.setGlobal*): member ->
        // global name, over shaderParams and the manifest default (shader globals spec 6.2)
        ShaderGlobalBindings shaderGlobalBindings;

        MaterialBlendMode blendMode = MaterialBlendMode::AUTO;
        MaterialCullMode cullMode = MaterialCullMode::AUTO;
        MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
        MaterialDepthTest depthTest = MaterialDepthTest::AUTO;
        float depthOffsetFactor = 0.0f; // polygon offset, slope scale
        float depthOffsetUnits = 0.0f;  // polygon offset, constant
        // queue order, lower draws first: the transparent queue's, and the opaque queue's
        // before the distance (lowest of the mesh's opaque submeshes, see MeshComponent)
        int sortPriority = 0;

        // --- shader library phase 2 (docs/superpowers/specs/2026-10-10-shader-library-phase2-design.md)
        MaterialStencil stencil;
        int colorWrite = -1;  // -1 AUTO, else MaterialColorWriteBits
        // KHR_materials_emissive_strength: scales emissiveFactor (the upload multiplies the
        // shader part's copy), so the factor keeps glTF's 0..1 colour
        float emissiveStrength = 1.0f;
        // one optional extra draw of the submesh, out of line (MaterialExtraPass)
        MaterialExtraPass extraPass;
        // the built-in Lit's extended lobes and hex tiling, out of line (MaterialPbr.h)
        OutOfLineValue<MaterialPbrExtensions> pbr;
        // @palette bindings by uniform name (S43 Palette Swap), out of line: almost no
        // material has one; an unset binding draws unchanged
        OutOfLineValue<pixelart::ShaderPalettes> shaderPalettes;

        bool operator == (const Material& other) const {
            return baseColorFactor == other.baseColorFactor &&
                   metallicFactor == other.metallicFactor &&
                   roughnessFactor == other.roughnessFactor &&
                   alphaCutoff == other.alphaCutoff &&
                   emissiveFactor == other.emissiveFactor &&
                   foliageTransmission == other.foliageTransmission &&
                   stochasticCellSize == other.stochasticCellSize &&
                   stochasticBlendSharpness == other.stochasticBlendSharpness &&
                   stochasticContrast == other.stochasticContrast &&
                   stochasticRotation == other.stochasticRotation &&
                   stochasticTiling == other.stochasticTiling &&
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
                   shaderGlobalBindings == other.shaderGlobalBindings &&
                   blendMode == other.blendMode &&
                   cullMode == other.cullMode &&
                   depthWrite == other.depthWrite &&
                   depthTest == other.depthTest &&
                   depthOffsetFactor == other.depthOffsetFactor &&
                   depthOffsetUnits == other.depthOffsetUnits &&
                   sortPriority == other.sortPriority &&
                   stencil == other.stencil &&
                   colorWrite == other.colorWrite &&
                   emissiveStrength == other.emissiveStrength &&
                   extraPass == other.extraPass &&
                   pbr == other.pbr &&
                   shaderPalettes == other.shaderPalettes;
        }

        bool operator != (const Material& other) const {
            return !(*this == other);
        }
    };

}

#endif //MATERIAL_H
