// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderVariantCompiler.h"
#include "ShaderGlobals.h"

#include "Engine.h"
#include "Log.h"
#include "pool/ShaderPool.h"
#include "component/TerrainComponent.h"
#include "lattice/LatticeTypes.h"
#include "animation/VertexAnimation.h"
#include "shader/ShaderManifest.h"
#include "texture/MaterialPbr.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>
#include <set>
#include <mutex>
#include <unordered_set>

#ifdef JASPERENGINE_RUNTIME_SHADERS
#include "shadercompiler.h"
#include "shaderlib.h"
#endif

using namespace jasperengine;

namespace {

// Bump when the compile pipeline changes its output for the same sources (glslang or
// SPIRV-Cross update, different options), so runtime caches of the old output go stale.
constexpr const char* compilerPipelineVersion = "jasperengine-shadercompiler-2"; // 2: fragment inputs located by name

// Length prefixes keep the concatenation unambiguous, so two different graphs
// cannot produce the same signature.
void appendSignaturePart(std::string& input, const std::string& label, const std::string& content) {
    input += std::to_string(label.size()) + ':' + label;
    input += ':' + std::to_string(content.size()) + ':' + content + '\n';
}

#ifdef JASPERENGINE_RUNTIME_SHADERS

ShaderVertexType mapVertexType(shadercompiler::attribute_type_t type) {
    using namespace shadercompiler;
    switch(type) {
        case attribute_type_t::FLOAT:  return ShaderVertexType::FLOAT;
        case attribute_type_t::FLOAT2: return ShaderVertexType::FLOAT2;
        case attribute_type_t::FLOAT3: return ShaderVertexType::FLOAT3;
        case attribute_type_t::FLOAT4: return ShaderVertexType::FLOAT4;
        case attribute_type_t::INT:    return ShaderVertexType::INT;
        case attribute_type_t::INT2:   return ShaderVertexType::INT2;
        case attribute_type_t::INT3:   return ShaderVertexType::INT3;
        case attribute_type_t::INT4:   return ShaderVertexType::INT4;
        default:                       return ShaderVertexType::FLOAT;
    }
}

ShaderUniformType mapUniformType(shadercompiler::uniform_type_t type) {
    using namespace shadercompiler;
    switch(type) {
        case uniform_type_t::FLOAT:  return ShaderUniformType::FLOAT;
        case uniform_type_t::FLOAT2: return ShaderUniformType::FLOAT2;
        case uniform_type_t::FLOAT3: return ShaderUniformType::FLOAT3;
        case uniform_type_t::FLOAT4: return ShaderUniformType::FLOAT4;
        case uniform_type_t::INT:    return ShaderUniformType::INT;
        case uniform_type_t::INT2:   return ShaderUniformType::INT2;
        case uniform_type_t::INT3:   return ShaderUniformType::INT3;
        case uniform_type_t::INT4:   return ShaderUniformType::INT4;
        case uniform_type_t::MAT3:   return ShaderUniformType::MAT3;
        case uniform_type_t::MAT4:   return ShaderUniformType::MAT4;
        default:                     return ShaderUniformType::FLOAT;
    }
}

TextureType mapTextureType(shadercompiler::texture_type_t type) {
    using namespace shadercompiler;
    switch(type) {
        case texture_type_t::TEXTURE_2D:    return TextureType::TEXTURE_2D;
        case texture_type_t::TEXTURE_3D:    return TextureType::TEXTURE_3D;
        case texture_type_t::TEXTURE_CUBE:  return TextureType::TEXTURE_CUBE;
        case texture_type_t::TEXTURE_ARRAY: return TextureType::TEXTURE_ARRAY;
        default:                            return TextureType::TEXTURE_2D;
    }
}

TextureSamplerType mapSamplerType(shadercompiler::texture_samplertype_t type) {
    using namespace shadercompiler;
    switch(type) {
        case texture_samplertype_t::FLOAT: return TextureSamplerType::FLOAT;
        case texture_samplertype_t::SINT:  return TextureSamplerType::SINT;
        case texture_samplertype_t::UINT:  return TextureSamplerType::UINT;
        case texture_samplertype_t::DEPTH: return TextureSamplerType::DEPTH;
        default:                           return TextureSamplerType::FLOAT;
    }
}

SamplerType mapSamplerFilterType(shadercompiler::sampler_type_t type) {
    using namespace shadercompiler;
    switch(type) {
        case sampler_type_t::FILTERING:   return SamplerType::FILTERING;
        case sampler_type_t::COMPARISON:  return SamplerType::COMPARISON;
        default:                          return SamplerType::FILTERING;
    }
}

ShaderStageType mapStageType(shadercompiler::stage_type_t type) {
    using namespace shadercompiler;
    switch(type) {
        case STAGE_VERTEX:   return ShaderStageType::VERTEX;
        case STAGE_FRAGMENT: return ShaderStageType::FRAGMENT;
        default:             return ShaderStageType::VERTEX;
    }
}

ShaderLang mapLang(shadercompiler::lang_type_t lang) {
    using namespace shadercompiler;
    switch(lang) {
        case LANG_GLSL:  return ShaderLang::GLSL;
        case LANG_HLSL:  return ShaderLang::HLSL;
        case LANG_MSL:   return ShaderLang::MSL;
        case LANG_SPIRV: return ShaderLang::SPIRV;
        default:         return ShaderLang::GLSL;
    }
}

// Target selection must stay in step with ShaderPool::getShaderLangStr(backend), which
// names the file the result is stored under.
void applyShaderBackend(shadercompiler::args_t& args, ShaderBackend backend) {
    switch (backend) {
        case ShaderBackend::MetalMacOS:
        case ShaderBackend::MetalIOS:
            args.lang = shadercompiler::LANG_MSL;
            args.version = 20100; // SPIRV-Cross MSL 2.1, as MMmmpp
            args.platform = (backend == ShaderBackend::MetalIOS)
                ? shadercompiler::SHADER_IOS : shadercompiler::SHADER_MACOS;
            break;
        case ShaderBackend::D3D11:
            args.lang = shadercompiler::LANG_HLSL;
            args.version = 50;
            break;
        case ShaderBackend::Vulkan:
            args.lang = shadercompiler::LANG_SPIRV;
            args.version = 10;
            break;
        case ShaderBackend::GLES3:
            args.lang = shadercompiler::LANG_GLSL;
            args.version = 300;
            args.es = true;
            break;
        default:
            args.lang = shadercompiler::LANG_GLSL;
            args.version = 410;
            break;
    }
}

std::string getLangSuffix(shadercompiler::lang_type_t lang, int version, bool es, shadercompiler::platform_t platform) {
    if (lang == shadercompiler::LANG_GLSL) {
        return es ? "_glsl" + std::to_string(version) + "es" : "_glsl" + std::to_string(version);
    } else if (lang == shadercompiler::LANG_HLSL) {
        return "_hlsl" + std::to_string(version);
    } else if (lang == shadercompiler::LANG_MSL) {
        return (platform == shadercompiler::SHADER_IOS) ? "_msl" + std::to_string(version) + "ios" : "_msl" + std::to_string(version) + "macos";
    } else if (lang == shadercompiler::LANG_SPIRV) {
        return "_spirv" + std::to_string(version);
    }
    return "";
}

// The defines and annotations of a stateful UI effect (phase 6), read from its manifest block (the
// "// @" lines right after #version 450): JFX_PASS_<NAME> = the index of each @init/@step/@frame pass in
// declaration order, JFX_PASSES (their count), JFX_STATEFUL; and the 32-bit float states' textures and
// samplers annotated unfilterable (GLES3/WebGL2 cannot filter them), in a buffer of their own.
void addUiEffectStateDefinitions(const std::string& source, shadercompiler::args_t& args) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos)
            end = source.size();
        std::string line = source.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
        start = end + 1;
    }
    size_t i = 0;
    while (i < lines.size() && lines[i].find_first_not_of(" \t") == std::string::npos)
        i++;
    if (i >= lines.size() || lines[i].find("#version") == std::string::npos)
        return;
    auto words = [](const std::string& text) {
        std::vector<std::string> out;
        size_t p = 0;
        while (p < text.size()) {
            while (p < text.size() && std::isspace((unsigned char)text[p]))
                p++;
            size_t q = p;
            while (q < text.size() && !std::isspace((unsigned char)text[q]))
                q++;
            if (q > p)
                out.push_back(text.substr(p, q - p));
            p = q;
        }
        return out;
    };
    int passes = 0;
    bool stateful = false;
    std::string annotations;
    for (i++; i < lines.size(); i++) {
        const std::vector<std::string> w = words(lines[i]);
        if (w.size() < 2 || w[0] != "//" || w[1].empty() || w[1][0] != '@')
            break;
        if ((w[1] == "@init" || w[1] == "@step" || w[1] == "@frame") && w.size() >= 3) {
            std::string macro = "JFX_PASS_";
            for (char c : w[2])
                macro += c == '-' ? '_' : (char)std::toupper((unsigned char)c);
            args.defines.push_back({macro, std::to_string(passes++)});
        } else if (w[1] == "@state" && w.size() >= 4) {
            stateful = true;
            if (w[3] == "rgba32f" || w[3] == "r32f") {
                std::string name = w[2];
                for (char& c : name)
                    if (c == '-')
                        c = '_';
                annotations += "// @image_sample_type u_state_" + name + " unfilterable_float\n";
                annotations += "// @sampler_type u_state_" + name + "_smp nonfiltering\n";
            }
        }
    }
    if (!stateful)
        return;
    args.defines.push_back({"JFX_STATEFUL", "1"});
    args.defines.push_back({"JFX_PASSES", std::to_string(passes)});
    if (!annotations.empty())
        args.fileBuffers["effects/__state_annotations__.glsl"] = annotations;
}

bool hasAnnotation(const shadercompiler::args_t& args, const char* tag,
                   const std::string& name, const char* value) {
    const std::string annotation = std::string("@") + tag + " " + name + " " + value;
    for (const auto& file : args.fileBuffers)
        if (file.second.find(annotation) != std::string::npos)
            return true;
    return false;
}

ShaderData convertToShaderData(
    const std::vector<shadercompiler::spirvcross_t>& spirvcrossvec,
    const shadercompiler::args_t& args) {

    ShaderData shaderData;

    shaderData.lang = mapLang(args.lang);
    shaderData.version = args.version;
    shaderData.es = args.es;

    unsigned int texCount = 0, samplerCount = 0, ubCount = 0, sbCount = 0, pairCount = 0;

    for (const auto& cross : spirvcrossvec) {
        ShaderStage stage;
        stage.type = mapStageType(cross.stage_type);
        stage.name = args.output_basename;  // Using output basename for stage name
        stage.source = cross.source;

        // SPIRV (Vulkan) outputs bytecode instead of source
        if (!cross.bytecode.empty()) {
            stage.bytecode.size = static_cast<uint32_t>(cross.bytecode.size() * sizeof(uint32_t));
            stage.bytecode.data = new unsigned char[stage.bytecode.size];
            memcpy(stage.bytecode.data, cross.bytecode.data(), stage.bytecode.size);
        }

        // Attributes
        for (const auto& attr : cross.inputs) {
            ShaderAttr sa;
            sa.name = attr.name;
            sa.semanticName = attr.semantic_name;
            sa.semanticIndex = attr.semantic_index;
            sa.location = attr.location;
            sa.type = mapVertexType(attr.type);
            stage.attributes.push_back(sa);
        }

        // Uniform Blocks
        for (const auto& ub : cross.uniform_blocks) {
            ShaderUniformBlock sub;
            sub.name = ub.name;
            sub.instName = ub.inst_name;
            sub.set = ub.set;
            sub.binding = ub.binding;
            sub.slot = ubCount++;
            sub.sizeBytes = ub.size_bytes;
            sub.flattened = ub.flattened;

            for (const auto& u : ub.uniforms) {
                ShaderUniform su;
                su.name = ub.inst_name + "." + u.name;  // Qualified name
                su.type = mapUniformType(u.type);
                su.arrayCount = u.array_count;
                su.offset = u.offset;
                sub.uniforms.push_back(su);
            }
            stage.uniformblocks.push_back(sub);
        }

        // Storage Buffers
        for (const auto& sb : cross.storage_buffers) {
            ShaderStorageBuffer ssb;
            ssb.name = sb.name;
            ssb.instName = sb.inst_name;
            ssb.set = sb.set;
            ssb.binding = sb.binding;
            ssb.slot = sbCount++;
            ssb.sizeBytes = sb.size_bytes;
            ssb.readonly = sb.readonly;
            ssb.type = ShaderStorageBufferType::STRUCT;
            stage.storagebuffers.push_back(ssb);
        }

        // Textures
        for (const auto& tex : cross.textures) {
            ShaderTexture st;
            st.name = tex.name;
            st.set = tex.set;
            st.binding = tex.binding;
            st.slot = texCount++;
            st.type = mapTextureType(tex.type);
            st.samplerType = hasAnnotation(args, "image_sample_type", tex.name, "unfilterable_float")
                ? TextureSamplerType::UNFILTERABLE_FLOAT : mapSamplerType(tex.sampler_type);
            stage.textures.push_back(st);
        }

        // Samplers
        for (const auto& s : cross.samplers) {
            ShaderSampler ss;
            ss.name = s.name;
            ss.set = s.set;
            ss.binding = s.binding;
            ss.slot = samplerCount++;
            ss.type = hasAnnotation(args, "sampler_type", s.name, "nonfiltering")
                ? SamplerType::NONFILTERING : mapSamplerFilterType(s.type);
            stage.samplers.push_back(ss);
        }

        // Texture-Sampler Pairs
        for (const auto& tsp : cross.texture_sampler_pairs) {
            ShaderTextureSamplerPair pair;
            pair.name = tsp.name;
            pair.textureName = tsp.texture_name;
            pair.samplerName = tsp.sampler_name;
            pair.slot = pairCount++;
            stage.textureSamplerPairs.push_back(pair);
        }

        shaderData.stages.push_back(stage);
    }

    return shaderData;
}

// The #defines of each variant property bit (see ShaderPool::getShaderPropertyName)

void addMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"MATERIAL_UNLIT", "1"});            // 'Ult'
    if (prop & shaderPropertyBit(1))  defs.push_back({"HAS_UV_SET1", "1"});               // 'Uv1'
    if (prop & shaderPropertyBit(2))  defs.push_back({"HAS_UV_SET2", "1"});               // 'Uv2'
    if (prop & shaderPropertyBit(3))  defs.push_back({"USE_PUNCTUAL", "1"});              // 'Puc'
    if (prop & shaderPropertyBit(4))  defs.push_back({"USE_SHADOWS", "1"});               // 'Shw'
    if (prop & shaderPropertyBit(5))  defs.push_back({"USE_STOCHASTIC_TILING", "1"});     // 'Sto' (was 'Pcf' until July 2026)
    if (prop & shaderPropertyBit(6))  defs.push_back({"HAS_NORMALS", "1"});               // 'Nor'
    if (prop & shaderPropertyBit(7))  defs.push_back({"HAS_NORMAL_MAP", "1"});            // 'Nmp'
    if (prop & shaderPropertyBit(8))  defs.push_back({"HAS_TANGENTS", "1"});              // 'Tan'
    if (prop & shaderPropertyBit(9))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});     // 'Vc3'
    if (prop & shaderPropertyBit(10)) defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});     // 'Vc4'
    if (prop & shaderPropertyBit(11)) defs.push_back({"HAS_TEXTURERECT", "1"});           // 'Txr'
    if (prop & shaderPropertyBit(12)) {                                                    // 'Fog'
        defs.push_back({"HAS_FOG", "1"});
        // the height fog, chosen by its block at run time (includes/fog.glsl, S12)
        defs.push_back({"HAS_HEIGHT_FOG", "1"});
    }
    // a Vertex Animation ('Vat', 'Vab'; never on a terrain) replaces skinning and morph targets
    const bool vat = (prop & ShaderPool::getVatProperties(ShaderType::MESH)) && !(prop & shaderPropertyBit(17));
    if ((prop & shaderPropertyBit(13)) && !vat) defs.push_back({"HAS_SKINNING", "1"});    // 'Ski'
    if ((prop & shaderPropertyBit(14)) && !vat) defs.push_back({"HAS_MORPHTARGET", "1"}); // 'Mta'
    if ((prop & shaderPropertyBit(15)) && !vat) defs.push_back({"HAS_MORPHNORMAL", "1"}); // 'Mnr'
    if ((prop & shaderPropertyBit(16)) && !vat) defs.push_back({"HAS_MORPHTANGENT", "1"}); // 'Mtg'
    if (prop & shaderPropertyBit(17)) defs.push_back({"HAS_TERRAIN", "1"});               // 'Ter'
    if (prop & shaderPropertyBit(18)) defs.push_back({"HAS_INSTANCING", "1"});            // 'Ist'
    if (prop & shaderPropertyBit(19)) defs.push_back({"USE_IBL", "1"});                   // 'Ibl'
    if (prop & shaderPropertyBit(20)) defs.push_back({"USE_MIRROR", "1"});                // 'Mir'
    if (prop & shaderPropertyBit(21)) defs.push_back({"USE_SSAO", "1"});                  // 'Sao'
    if (prop & shaderPropertyBit(22)) defs.push_back({"USE_LIGHT2D", "1"});               // 'L2d'
    if (prop & shaderPropertyBit(23)) defs.push_back({"USE_SHADOWS_2D", "1"});            // 'S2d'
    if (prop & shaderPropertyBit(24)) defs.push_back({"ALPHA_MASK", "1"});                 // 'Ams'
    if (prop & shaderPropertyBit(25)) defs.push_back({"ALPHA_OPAQUE", "1"});               // 'Aop'
    if (prop & shaderPropertyBit(26)) defs.push_back({"USE_INSTANCE_FADE", "1"});          // 'Ifd'
    if (prop & shaderPropertyBit(27)) defs.push_back({"HAS_TERRAIN_PBR", "1"});            // 'Tpb'
    if (prop & shaderPropertyBit(28)) defs.push_back({"USE_VERTEX_LIGHTS", "1"});          // 'Vtl'
    // bits 29-30: capacity tier of the per-object lights blocks ('Lc4', 'L16', neither = 8)
    defs.push_back({"LIGHT_CAPACITY", (prop & shaderPropertyBit(29)) ? "4" : ((prop & shaderPropertyBit(30)) ? "16" : "8")});
    if (prop & shaderPropertyBit(31)) defs.push_back({"USE_POST_COLOR", "1"});          // 'Pcl'
    // a terrain is never deformed (its variant keeps the bit inert)
    if ((prop & shaderPropertyBit(32)) && !(prop & shaderPropertyBit(17))) defs.push_back({"HAS_LATTICE", "1"}); // 'Ltc'
    if (prop & shaderPropertyBit(33)) defs.push_back({"USE_SHADOWS_HQ", "1"});         // 'Shq' (S10; bit 5 is 'Sto')
    // includes/vat.glsl: Bones mode ('Vab') wins when a key has both
    if (vat) defs.push_back({"HAS_VAT", "1"});                                              // 'Vat', 'Vab'
    if (vat && (prop & ShaderPool::getVatProperty(ShaderType::MESH, true))) defs.push_back({"HAS_VAT_BONES", "1"}); // 'Vab'
    if (prop & ShaderPool::getAutoInstancingProperty(ShaderType::MESH)) defs.push_back({"HAS_AUTO_INSTANCING", "1"}); // 'Aui'
    if (prop & ShaderPool::getDirectionalOnlyProperty()) defs.push_back({"DIRECTIONAL_LIGHTS_ONLY", "1"}); // 'Dlo'
    if (prop & ShaderPool::getLodFadeProperty()) defs.push_back({"USE_LOD_FADE", "1"});    // 'Lfd'
    if (prop & ShaderPool::getWaterExclusionProperty()) defs.push_back({"USE_WATER_EXCLUSION", "1"}); // 'Wex'
    if (prop & ShaderPool::getWaterWakeProperty()) defs.push_back({"USE_WATER_WAKE", "1"});           // 'Wwk'
    // T5's foliage (ShaderPool::getFoliageProperties): 'Cin', 'Wnd', 'Dth'
    if (prop & ShaderPool::getFoliageProperties(ShaderType::MESH, true, false, false)) defs.push_back({"HAS_COMPACT_INSTANCE", "1"});
    if (prop & ShaderPool::getFoliageProperties(ShaderType::MESH, false, true, false)) defs.push_back({"USE_FOLIAGE_WIND", "1"});
    if (prop & ShaderPool::getFoliageProperties(ShaderType::MESH, false, false, true)) defs.push_back({"USE_FOLIAGE_DITHER", "1"});
    // S57's lighting: the screen-tile lights of a pixel-lit variant, the capsule and contact
    // shadows of a shadowed one, and the OIT accumulation (its main is wrapped, prepareVariantArgs)
    if (prop & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::TILED_LIGHTS)) defs.push_back({"USE_TILED_LIGHTS", "1"});       // 'Til'
    if (prop & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::CAPSULE_SHADOWS)) defs.push_back({"USE_CAPSULE_SHADOWS", "1"}); // 'Cap'
    if (prop & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::CONTACT_SHADOWS)) defs.push_back({"USE_CONTACT_SHADOWS", "1"}); // 'Cts'
    if (prop & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::OIT)) defs.push_back({"USE_OIT", "1"});                         // 'Oit'
    if (prop & ShaderPool::getPixelSnapProperty(ShaderType::MESH)) defs.push_back({"PIXEL_SNAP", "1"}); // 'Pxs' (S41)
    if (prop & ShaderPool::getTerrainHolesProperty(ShaderType::MESH)) defs.push_back({"TERRAIN_HOLES", "1"});       // 'Thl'
    if (prop & ShaderPool::getTerrainWorldProperty(ShaderType::MESH)) defs.push_back({"HAS_TERRAIN_WORLD", "1"});   // 'Twd'
    if (prop & ShaderPool::getLight2DStepsProperty()) defs.push_back({"LIGHT2D_STEPS", "1"});                        // 'Lst' (S78)
    // S52: a batchable sprite's FX slots, frame rect and snap pivot
    if (prop & ShaderPool::getSpriteBatchProperty(ShaderType::MESH, ShaderPool::SpriteBatchInput::FX)) defs.push_back({"HAS_SPRITE_FX", "1"});         // 'Sfx'
    if (prop & ShaderPool::getSpriteBatchProperty(ShaderType::MESH, ShaderPool::SpriteBatchInput::FRAME_RECT)) defs.push_back({"HAS_FRAME_RECT", "1"}); // 'Sfr'
    if (prop & ShaderPool::getTileClampProperty()) defs.push_back({"TILE_CLAMP_FRAME", "1"}); // 'Tcl'
    if (prop & ShaderPool::getSpriteBatchProperty(ShaderType::MESH, ShaderPool::SpriteBatchInput::SNAP_PIVOT)) defs.push_back({"HAS_SNAP_PIVOT", "1"}); // 'Spv'
}

void addDepthMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"HAS_TEXTURE", "1"});       // 'Tex'
    // a Vertex Animation ('Vat', 'Vab'; never on a terrain) replaces skinning and morph targets
    const bool vat = (prop & (shaderPropertyBit(16) | shaderPropertyBit(17))) && !(prop & shaderPropertyBit(5));
    if ((prop & shaderPropertyBit(1)) && !vat) defs.push_back({"HAS_SKINNING", "1"});      // 'Ski'
    if ((prop & shaderPropertyBit(2)) && !vat) defs.push_back({"HAS_MORPHTARGET", "1"});   // 'Mta'
    if ((prop & shaderPropertyBit(3)) && !vat) defs.push_back({"HAS_MORPHNORMAL", "1"});   // 'Mnr'
    if ((prop & shaderPropertyBit(4)) && !vat) defs.push_back({"HAS_MORPHTANGENT", "1"});  // 'Mtg'
    if (prop & shaderPropertyBit(5))  defs.push_back({"HAS_TERRAIN", "1"});       // 'Ter'
    if (prop & shaderPropertyBit(6))  defs.push_back({"HAS_INSTANCING", "1"});    // 'Ist'
    if (prop & shaderPropertyBit(7))  defs.push_back({"ALPHA_MASK", "1"});         // 'Ams'
    if (prop & shaderPropertyBit(8))  defs.push_back({"USE_INSTANCE_FADE", "1"}); // 'Ifd'
    if (prop & shaderPropertyBit(9))  defs.push_back({"HAS_NORMALS", "1"});           // 'Nor'
    if (prop & shaderPropertyBit(10)) defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"}); // 'Vc3'
    if (prop & shaderPropertyBit(11)) defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"}); // 'Vc4'
    if (prop & shaderPropertyBit(12)) defs.push_back({"HAS_UV_SET1", "1"});           // 'Uv1'
    if (prop & shaderPropertyBit(13)) defs.push_back({"HAS_TEXTURERECT", "1"});       // 'Txr'
    if (prop & shaderPropertyBit(14)) defs.push_back({"USE_STOCHASTIC_TILING", "1"}); // 'Sto'
    if ((prop & shaderPropertyBit(15)) && !(prop & shaderPropertyBit(5))) defs.push_back({"HAS_LATTICE", "1"}); // 'Ltc' (never on a terrain)
    if (vat) defs.push_back({"HAS_VAT", "1"});                                              // 'Vat', 'Vab'
    if (vat && (prop & shaderPropertyBit(17))) defs.push_back({"HAS_VAT_BONES", "1"});      // 'Vab'
    if (prop & ShaderPool::getAutoInstancingProperty(ShaderType::DEPTH)) defs.push_back({"HAS_AUTO_INSTANCING", "1"}); // 'Aui'
    if (prop & ShaderPool::getPixelSnapProperty(ShaderType::DEPTH)) defs.push_back({"PIXEL_SNAP", "1"}); // 'Pxs' (S41)
    if (prop & ShaderPool::getTerrainHolesProperty(ShaderType::DEPTH)) defs.push_back({"TERRAIN_HOLES", "1"});      // 'Thl'
    if (prop & ShaderPool::getTerrainWorldProperty(ShaderType::DEPTH)) defs.push_back({"HAS_TERRAIN_WORLD", "1"});  // 'Twd'
    // T5's foliage (ShaderPool::getFoliageProperties): 'Cin', 'Wnd', 'Dth'
    if (prop & ShaderPool::getFoliageProperties(ShaderType::DEPTH, true, false, false)) defs.push_back({"HAS_COMPACT_INSTANCE", "1"});
    if (prop & ShaderPool::getFoliageProperties(ShaderType::DEPTH, false, true, false)) defs.push_back({"USE_FOLIAGE_WIND", "1"});
    if (prop & ShaderPool::getFoliageProperties(ShaderType::DEPTH, false, false, true)) defs.push_back({"USE_FOLIAGE_DITHER", "1"});
    if (prop & ShaderPool::getSpriteBatchProperty(ShaderType::DEPTH, ShaderPool::SpriteBatchInput::SNAP_PIVOT)) defs.push_back({"HAS_SNAP_PIVOT", "1"}); // 'Spv' (S52)
}

void addGBufferMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"HAS_BASECOLOR_TEXTURE", "1"});         // 'Bct'
    if (prop & shaderPropertyBit(1))  defs.push_back({"HAS_NORMALS", "1"});                   // 'Nor'
    // a Vertex Animation ('Vat', 'Vab'; never on a terrain) replaces skinning and morph targets
    const bool vat = (prop & (shaderPropertyBit(14) | shaderPropertyBit(15))) && !(prop & shaderPropertyBit(6));
    if ((prop & shaderPropertyBit(2)) && !vat) defs.push_back({"HAS_SKINNING", "1"});         // 'Ski'
    if ((prop & shaderPropertyBit(3)) && !vat) defs.push_back({"HAS_MORPHTARGET", "1"});      // 'Mta'
    if ((prop & shaderPropertyBit(4)) && !vat) defs.push_back({"HAS_MORPHNORMAL", "1"});      // 'Mnr'
    if ((prop & shaderPropertyBit(5)) && !vat) defs.push_back({"HAS_MORPHTANGENT", "1"});     // 'Mtg'
    if (prop & shaderPropertyBit(6))  defs.push_back({"HAS_TERRAIN", "1"});                   // 'Ter'
    if (prop & shaderPropertyBit(7))  defs.push_back({"HAS_INSTANCING", "1"});                // 'Ist'
    if (prop & shaderPropertyBit(8))  defs.push_back({"HAS_METALLICROUGHNESS_TEXTURE", "1"}); // 'Mrt'
    if (prop & shaderPropertyBit(9))  defs.push_back({"HAS_TERRAIN_PBR", "1"});               // 'Tpb'
    if (prop & shaderPropertyBit(10)) defs.push_back({"USE_STOCHASTIC_TILING", "1"});         // 'Sto'
    if ((prop & shaderPropertyBit(11)) && !(prop & shaderPropertyBit(6))) defs.push_back({"HAS_LATTICE", "1"}); // 'Ltc' (never on a terrain)
    if (prop & shaderPropertyBit(12)) defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});         // 'Vc3' (a lattice mask's)
    if (prop & shaderPropertyBit(13)) defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});         // 'Vc4'
    if (vat) defs.push_back({"HAS_VAT", "1"});                                                  // 'Vat', 'Vab'
    if (vat && (prop & shaderPropertyBit(15))) defs.push_back({"HAS_VAT_BONES", "1"});          // 'Vab'
    if (prop & shaderPropertyBit(16)) defs.push_back({"HAS_OCCLUSION_TEXTURE", "1"});         // 'Occ'
    if ((prop & shaderPropertyBit(17)) && !(prop & shaderPropertyBit(6))) defs.push_back({"HAS_UV_SET2", "1"}); // 'Uv2' (never on a terrain)
    if (prop & shaderPropertyBit(18)) defs.push_back({"USE_INSTANCE_FADE", "1"});             // 'Ifd'
    if (prop & ShaderPool::getTerrainHolesProperty(ShaderType::GBUFFER)) defs.push_back({"TERRAIN_HOLES", "1"});    // 'Thl'
    if (prop & ShaderPool::getTerrainWorldProperty(ShaderType::GBUFFER)) defs.push_back({"HAS_TERRAIN_WORLD", "1"}); // 'Twd'
    // T3 D2: Lit defines do not reach a G-buffer, so its tier and its twelve layers are bits
    if (prop & ShaderPool::getTerrainTierProperty(terraincore::TerrainQuality::MEDIUM)) defs.push_back({"TERRAIN_QUALITY_MEDIUM", "1"}); // 'Tqm'
    if (prop & ShaderPool::getTerrainTierProperty(terraincore::TerrainQuality::LOW)) defs.push_back({"TERRAIN_QUALITY_LOW", "1"});       // 'Tql'
    if (prop & ShaderPool::getTerrainExtendedProperty()) defs.push_back({"TERRAIN_EXTENDED_LAYERS", "1"});                            // 'Txl'
    // T5's foliage (ShaderPool::getFoliageProperties): 'Cin', 'Wnd', 'Dth'
    if (prop & ShaderPool::getFoliageProperties(ShaderType::GBUFFER, true, false, false)) defs.push_back({"HAS_COMPACT_INSTANCE", "1"});
    if (prop & ShaderPool::getFoliageProperties(ShaderType::GBUFFER, false, true, false)) defs.push_back({"USE_FOLIAGE_WIND", "1"});
    if (prop & ShaderPool::getFoliageProperties(ShaderType::GBUFFER, false, false, true)) defs.push_back({"USE_FOLIAGE_DITHER", "1"});
    if (prop & ShaderPool::getGBufferVoxelProperty()) defs.push_back({"HAS_VOXEL", "1"});                       // 'Vox'
}

void addUIPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"HAS_TEXTURE", "1"});              // 'Tex'
    if (prop & shaderPropertyBit(1))  defs.push_back({"HAS_FONTATLAS_TEXTURE", "1"});    // 'Ftx'
    if (prop & shaderPropertyBit(2))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});    // 'Vc3'
    if (prop & shaderPropertyBit(3))  defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});    // 'Vc4'
    if (prop & shaderPropertyBit(4))  defs.push_back({"HAS_FONTATLAS_SDF", "1"});        // 'Sdf' (with 'Ftx')
    if (prop & shaderPropertyBit(5))  defs.push_back({"PIXEL_SNAP", "1"});               // 'Pxs' (S41)
}

void addHtmlUIPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"HAS_TEXTURE", "1"});              // 'Tex'
}

// exclusive: one of them per variant (ShaderPool::getHtmlUIPostProperties)
void addHtmlUIPostPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"HAS_PASSTHROUGH", "1"});          // 'Pas'
    if (prop & shaderPropertyBit(1))  defs.push_back({"HAS_COLOR_MATRIX", "1"});         // 'Cmx'
    if (prop & shaderPropertyBit(2))  defs.push_back({"HAS_BLEND_MASK", "1"});           // 'Msk'
    if (prop & shaderPropertyBit(3))  defs.push_back({"HAS_DROP_SHADOW", "1"});          // 'Shd'
    if (prop & shaderPropertyBit(4))  defs.push_back({"HAS_BLUR", "1"});                 // 'Blr'
    if (prop & shaderPropertyBit(5))  defs.push_back({"HAS_UNPREMULTIPLY", "1"});        // 'Unp'
    if (prop & shaderPropertyBit(6))  defs.push_back({"HAS_YUV", "1"});                  // 'Yuv'
}

void addPointsPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"HAS_TEXTURE", "1"});              // 'Tex'
    if (prop & shaderPropertyBit(1))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});    // 'Vc3'
    if (prop & shaderPropertyBit(2))  defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});    // 'Vc4'
    if (prop & shaderPropertyBit(3))  defs.push_back({"HAS_TEXTURERECT", "1"});          // 'Txr'
    if (prop & shaderPropertyBit(4))  defs.push_back({"USE_POST_COLOR", "1"});           // 'Pcl'
}

void addLinesPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});    // 'Vc3'
    if (prop & shaderPropertyBit(1))  defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});    // 'Vc4'
    if (prop & shaderPropertyBit(2))  defs.push_back({"USE_POST_COLOR", "1"});           // 'Pcl'
    if (prop & shaderPropertyBit(3))  defs.push_back({"LINE_RIBBON", "1"});              // 'Rib'
    if (prop & shaderPropertyBit(4))  defs.push_back({"HAS_TEXTURE", "1"});              // 'Tex'
    if (prop & shaderPropertyBit(5))  defs.push_back({"LINE_PIXEL_WIDTH", "1"});         // 'Pxw'
}

// ShaderPool::getParticlesProperties; the effect's modules pick them (the Renderer module, Texture
// Sheet Animation's frame blending, Custom Data)
void addParticlesPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop) {
    if (prop & (1 << 0))  defs.push_back({"HAS_TEXTURE", "1"});              // 'Tex'
    if (prop & (1 << 1))  defs.push_back({"PARTICLE_MESH", "1"});            // 'Msh'
    if (prop & (1 << 2))  defs.push_back({"PARTICLE_TRAIL", "1"});           // 'Trl'
    if (prop & (1 << 3))  defs.push_back({"STRETCH", "1"});                  // 'Str'
    if (prop & (1 << 4))  defs.push_back({"FRAME_BLEND", "1"});              // 'Fbl'
    if (prop & (1 << 5))  defs.push_back({"LIT", "1"});                      // 'Lit'
    if (prop & (1 << 6))  defs.push_back({"SIXWAY", "1"});                   // 'Sxw'
    if (prop & (1 << 7))  defs.push_back({"CUSTOM_DATA", "1"});              // 'Cus'
    if (prop & (1 << 8))  defs.push_back({"RECEIVE_SHADOWS", "1"});          // 'Rsh'
    if (prop & (1 << 9))  defs.push_back({"HAS_FOG", "1"});                  // 'Fog'
    if (prop & (1 << 10)) defs.push_back({"USE_POST_COLOR", "1"});           // 'Pcl'
    if (prop & (1 << 11)) defs.push_back({"PIXEL_SNAP", "1"});               // 'Pxs' (S41)
    if (prop & (1 << 12)) defs.push_back({"PARTICLE_PIXEL", "1"});           // 'Pxl' (S49)
    if (prop & (1 << 13)) defs.push_back({"PARTICLE_ANALYTIC", "1"});        // 'Ana' (S49)
}

// the uber pass's features (RenderSystem::UberFeature)
void addUberPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    static const char* const defines[] = {"UBER_FXAA", "UBER_CHROMATIC", "UBER_PIXELATE", "UBER_CRT", "UBER_EDGE", "UBER_GRAIN",
                                          "UBER_SHARPEN", "UBER_LUT", "UBER_VIGNETTE", "UBER_DITHER", "UBER_BLOOM", "UBER_BLUR",
                                          "UBER_UPSCALE", "UBER_VHS", "UBER_HALATION", "UBER_FILM_DAMAGE", "UBER_GRAIN_LENS",
                                          "UBER_GRAIN_FILMIC", "UBER_SHARP_BILINEAR", "UBER_PALETTE", "UBER_GLITCH"};
    for (int bit = 0; bit < (int)(sizeof(defines) / sizeof(defines[0])); bit++) {
        if (prop & shaderPropertyBit(bit)) defs.push_back({defines[bit], "1"});
    }
}

void addBloomPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"BLOOM_PREFILTER", "1"});          // 'Pre'
    if (prop & shaderPropertyBit(1))  defs.push_back({"BLOOM_THRESHOLD", "1"});          // 'Thr'
    if (prop & shaderPropertyBit(2))  defs.push_back({"BLOOM_UPSAMPLE", "1"});           // 'Ups'
    if (prop & shaderPropertyBit(3))  defs.push_back({"BLOOM_MIX", "1"});                // 'Mix'
}

// FluidSimulator's programs: the solver (none), the look passes, the solver without linear filtering
void addFluidPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"FLUID_LOOK", "1"});               // 'Lok'
    if (prop & shaderPropertyBit(1))  defs.push_back({"FLUID_MANUAL_FILTERING", "1"});   // 'Mfl'
}

// titleart.frag: the program number in bits 0-6, the RGBA8 encodings after it
void addTitleArtPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    defs.push_back({"TA_PROGRAM", std::to_string(prop & 0x7Fu)});
    if (prop & shaderPropertyBit(7))  defs.push_back({"TA_SEEDS_FIXED", "1"});           // 'Sfx'
    if (prop & shaderPropertyBit(8))  defs.push_back({"TA_DISTANCE_FIXED", "1"});        // 'Dfx'
    if (prop & shaderPropertyBit(10)) defs.push_back({"TFX_LIVE", "1"});                 // 'Liv'
    if (prop & shaderPropertyBit(11)) defs.push_back({"TA_GEN", "1"});                   // 'Gen' (S83's generators)
    // the ELEMENT program's variants (titleart/render/TitleElementVariant.h): bits 16-20 the
    // pattern kind, 21 its relief, 22-25 the texture kind, then glass, reflection, satin, spans
    // and the whole program
    const uint64_t pattern = (prop >> 16) & 0x1Fu, texture = (prop >> 22) & 0xFu;
    if (pattern) defs.push_back({"TA_EL_PATTERN", std::to_string(pattern)});           // 'Ek0'..'Ek4'
    if (prop & shaderPropertyBit(21)) defs.push_back({"TA_EL_RELIEF", "1"});             // 'Erl'
    if (texture) defs.push_back({"TA_EL_TEXTURE", std::to_string(texture)});           // 'Et0'..'Et3'
    if (prop & shaderPropertyBit(26)) defs.push_back({"TA_EL_GLASS", "1"});              // 'Egl'
    if (prop & shaderPropertyBit(27)) defs.push_back({"TA_EL_REFLECTION", "1"});         // 'Erf'
    if (prop & shaderPropertyBit(28)) defs.push_back({"TA_EL_SATIN", "1"});              // 'Esa'
    if (prop & shaderPropertyBit(29)) defs.push_back({"TA_EL_SPAN", "1"});               // 'Esp'
    if (prop & shaderPropertyBit(30)) defs.push_back({"TA_EL_WHOLE", "1"});              // 'Ewh'
}

// the TL_* bits of TitleTierFeatures.h (titleart/title_lit.frag)
void addTitleLitPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    static const char* const FLAGS[25] = {"TL_RAMP", "TL_CLEARCOAT", "TL_IRIDESCENCE", "TL_GEM", "TL_GEM_BEHIND", "TL_DISPERSION",
                                          "TL_GLASS_DEPTH", "TL_FLAKES", "TL_PATTERN", nullptr, nullptr, "TL_ENV_LOCK",
                                          "TL_BASE_TEXTURE", "TL_WRAP_UV", "TL_EMISSIVE", "TL_TRANSPARENT", "TL_RIGS", "TL_LIGHTS",
                                          "TL_HDR_COMPRESS", nullptr, nullptr, "TL_MATCAP", "TL_SPECULAR_AA", "TL_SURFACE_LITE",
                                          "TL_FACE_IMAGE"};
    for (int bit = 0; bit < 25; bit++) {
        if (FLAGS[bit] && (prop & shaderPropertyBit(bit))) defs.push_back({FLAGS[bit], "1"});
    }
    const uint64_t surface = (prop >> 9) & 3u;
    if (surface) defs.push_back({"TL_SURFACE_KIND", std::to_string(surface)});
    defs.push_back({"TL_OUTPUT", std::to_string((prop >> 19) & 3u)});
}

void addSkyPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint64_t prop) {
    if (prop & shaderPropertyBit(0))  defs.push_back({"USE_POST_COLOR", "1"});           // 'Pcl'
}

// Entry points and #defines of a built-in variant
bool setupShaderArgs(shadercompiler::args_t& args, ShaderType shaderType, uint64_t properties) {
    if (shaderType == ShaderType::MESH){
        args.vert_file = "mesh.vert";
        args.frag_file = "mesh.frag";
        addMeshPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::DEPTH){
        args.vert_file = "depth.vert";
        args.frag_file = "depth.frag";
        addDepthMeshPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::GBUFFER){
        args.vert_file = "gbuffer.vert";
        args.frag_file = "gbuffer.frag";
        addGBufferMeshPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::UI){
        args.vert_file = "ui.vert";
        args.frag_file = "ui.frag";
        addUIPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::POINTS){
        args.vert_file = "points.vert";
        args.frag_file = "points.frag";
        addPointsPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::PARTICLES){
        args.vert_file = "particles.vert";
        args.frag_file = "particles.frag";
        addParticlesPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::LINES){
        args.vert_file = "lines.vert";
        args.frag_file = "lines.frag";
        addLinesPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::SKYBOX){
        args.vert_file = "sky.vert";
        args.frag_file = "sky.frag";
        addSkyPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::SSAO){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "ssao.frag";
    }else if (shaderType == ShaderType::SSAO_BLUR){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "ssao_blur.frag";
    }else if (shaderType == ShaderType::SSR){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "ssr.frag";
    }else if (shaderType == ShaderType::SSR_BLUR){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "ssr_blur.frag";
    }else if (shaderType == ShaderType::COMPOSITE){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "composite.frag";
    }else if (shaderType == ShaderType::SHADOW2D){
        args.vert_file = "shadow2d.vert";
        args.frag_file = "shadow2d.frag";
    }else if (shaderType == ShaderType::BLIT){
        if (properties == 0){
            args.vert_file = "fullscreen.vert";
            args.frag_file = "blit.frag";
        }else{
            // the uber pass (phase 4): the blit with post-processing features
            args.vert_file = "uber.vert";
            args.frag_file = "uber.frag";
            addUberPropertyDefinitions(args.defines, properties);
        }
    }else if (shaderType == ShaderType::POSTPROCESS){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "postprocess.frag";
    }else if (shaderType == ShaderType::PROBE_FILTER){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "probe_filter.frag";
    }else if (shaderType == ShaderType::BLOOM){
        args.vert_file = "bloom.vert";
        args.frag_file = "bloom.frag";
        addBloomPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::HTMLUI){
        args.vert_file = "htmlui.vert";
        args.frag_file = "htmlui.frag";
        addHtmlUIPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::HTMLUI_GRADIENT){
        // the geometry vertex shader with its texture coordinates (the gradient's positions)
        args.vert_file = "htmlui.vert";
        args.frag_file = "htmlui_gradient.frag";
        args.defines.push_back({"HAS_TEXTURE", "1"});
    }else if (shaderType == ShaderType::HTMLUI_PRIM){
        args.vert_file = "htmlui_prim.vert";
        args.frag_file = "htmlui_prim.frag";
    }else if (shaderType == ShaderType::HTMLUI_POST){
        args.vert_file = "htmlui_post.vert";
        args.frag_file = "htmlui_post.frag";
        addHtmlUIPostPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::SHADOW_CACHE){
        args.vert_file = "fullscreen.vert";
        args.frag_file = (properties & (1 << 0)) ? "shadow_atlas_view.frag" : "shadow_restore.frag";  // 'Vis'
    }else if (shaderType == ShaderType::UI_EFFECT){
        // the fragment is the effect's (compile: effects/<name>.frag, or a project's .frag)
        const bool filterForm = (properties & shaderPropertyBit(0)) != 0;                   // 'Flt'
        const bool stepForm = (properties & shaderPropertyBit(1)) != 0;                     // 'Stp' (phase 6)
        const bool scatterForm = (properties & shaderPropertyBit(2)) != 0;                  // 'Sct' (phase 6)
        if (scatterForm)
            args.vert_file = "effects/effect_scatter.vert";
        else if (stepForm)
            args.vert_file = "effects/effect_step.vert";
        else
            args.vert_file = filterForm ? "effects/effect_filter.vert" : "effects/effect_paint.vert";
        args.frag_file = "";
        args.defines.push_back({"JFX", "1"});
        if (filterForm)
            args.defines.push_back({"JFX_FILTER_FORM", "1"});
        if (stepForm || scatterForm)
            args.defines.push_back({"JFX_STEP_FORM", "1"});
        if (scatterForm)
            args.defines.push_back({"JFX_SCATTER_FORM", "1"});
    }else if (shaderType == ShaderType::FLUID){
        args.vert_file = "fluid.vert";
        args.frag_file = "fluid.frag";
        addFluidPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::TITLE_ART){
        args.vert_file = "titleart.vert";
        args.frag_file = "titleart.frag";
        addTitleArtPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::TITLE_LIT){
        args.vert_file = "titleart/title_lit.vert";
        args.frag_file = "titleart/title_lit.frag";
        addTitleLitPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::TITLE_3D_POST){
        args.vert_file = "titleart.vert";
        args.frag_file = "titleart/title3d_post.frag";
        args.defines.push_back({"TP_PROGRAM", std::to_string(properties & 0xFu)});
    }else if (shaderType == ShaderType::TITLE_ENV){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "titleart/title_env.frag";
        args.defines.push_back({"TE_PROGRAM", std::to_string(properties & 0xFu)});
    }else if (shaderType == ShaderType::TITLE_GLINT){
        args.vert_file = "titleart.vert";
        args.frag_file = "titleart/glint3d.frag";
        args.defines.push_back({"TG_PROGRAM", std::to_string(properties & 0xFu)});
    }else if (shaderType == ShaderType::TITLE_GLINT_SPRITE){
        args.vert_file = "titleart/glint_sprite.vert";
        args.frag_file = "titleart/glint_sprite.frag";
    }else if (shaderType == ShaderType::ATMOSPHERE_DEPTH){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "atmosphere_depth.frag";
    }else if (shaderType == ShaderType::VOLUMETRIC_FOG){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "volumetric_fog.frag";
    }else if (shaderType == ShaderType::ATMOSPHERE_DENOISE){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "atmosphere_denoise.frag";
    }else if (shaderType == ShaderType::VOLUMETRIC_CLOUDS){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "volumetric_clouds.frag";
    }else if (shaderType == ShaderType::SKY_COMPOSITE){
        // at the far plane: the depth test passes on the sky pixels only
        args.vert_file = "fullscreen.vert";
        args.frag_file = "sky_composite.frag";
        args.defines.push_back({"FULLSCREEN_AT_FAR", "1"});
    }else if (shaderType == ShaderType::VOLUMETRIC_COMPOSITE){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "volumetric_composite.frag";
    }else if (shaderType == ShaderType::LIGHTING_PASS){
        args.vert_file = "fullscreen.vert";
        args.frag_file = (properties & (1 << 0)) ? "oit_resolve.frag" : "contact_shadows.frag";   // 'Oir'
    }else if (shaderType == ShaderType::TRANSITION){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "transition.frag";
    }else if (shaderType == ShaderType::PASS_PREVIEW){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "pass_preview.frag";
        if (properties & (1 << 0)) args.defines.push_back({"PREVIEW_ARRAY", "1"});   // 'Arr'
        if (properties & (1 << 1)) args.defines.push_back({"PREVIEW_CUBE", "1"});    // 'Cub'
        if (properties & (1 << 2)) args.defines.push_back({"PREVIEW_3D", "1"});      // 'Tx3'
    }else{
        return false;
    }

    if (shaderType == ShaderType::MESH || shaderType == ShaderType::GBUFFER){
        args.defines.push_back({"MAX_TERRAIN_LAYERS", std::to_string(TERRAIN_SHADER_BASE_LAYERS)});
    }
    if (shaderType == ShaderType::MESH || shaderType == ShaderType::DEPTH || shaderType == ShaderType::GBUFFER){
        // the record layout of includes/lattice.glsl (lattice/LatticeTypes.h)
        args.defines.push_back({"LATTICE_MAX_PER_MESH", std::to_string(LATTICE_MAX_PER_MESH)});
        args.defines.push_back({"LATTICE_TEXTURE_WIDTH", std::to_string(LATTICE_TEXTURE_WIDTH)});
        args.defines.push_back({"LATTICE_RECORD_HEADER", std::to_string(LATTICE_RECORD_HEADER)});
        // the clip table of includes/vat.glsl (animation/VertexAnimation.h)
        args.defines.push_back({"VAT_MAX_CLIPS", std::to_string(vat::VAT_MAX_CLIPS)});
    }
    if (shaderType == ShaderType::VOLUMETRIC_FOG){
        // the light shafts sample S10's directional cascades (includes/shadow_sampling.glsl)
        args.defines.push_back({"MAX_SHADOW_ATLAS_SLOTS", std::to_string(MAX_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_POINT_SHADOW_ATLAS_SLOTS", std::to_string(MAX_POINT_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_SHADOWCASCADES", std::to_string(MAX_SHADOWCASCADES)});
        args.defines.push_back({"MAX_POINT_SHADOW_LIGHTS", std::to_string(MAX_POINT_SHADOW_LIGHTS)});
        args.defines.push_back({"SHADOW_CUBE_FACES", std::to_string(SHADOW_CUBE_FACES)});
    }
    if (shaderType == ShaderType::PARTICLES){
        // RECEIVE_SHADOWS samples S10's directional cascades (includes/shadow_sampling.glsl)
        args.defines.push_back({"MAX_SHADOW_ATLAS_SLOTS", std::to_string(MAX_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_POINT_SHADOW_ATLAS_SLOTS", std::to_string(MAX_POINT_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_SHADOWCASCADES", std::to_string(MAX_SHADOWCASCADES)});
        args.defines.push_back({"MAX_POINT_SHADOW_LIGHTS", std::to_string(MAX_POINT_SHADOW_LIGHTS)});
        args.defines.push_back({"SHADOW_CUBE_FACES", std::to_string(SHADOW_CUBE_FACES)});
        args.defines.push_back({"MAX_PARTICLE_LIGHTS", std::to_string(MAX_PARTICLE_LIGHTS)});
    }
    if (shaderType == ShaderType::MESH){
        args.defines.push_back({"MAX_LIGHTS", std::to_string(MAX_LIGHTS)}); // legacy u_fs_lighting
        args.defines.push_back({"SPOT_MASK_TILES", std::to_string(SPOT_MASK_ATLAS_TILES)});
        args.defines.push_back({"MAX_LIGHTS_2D", std::to_string(MAX_LIGHTS_2D)});
        args.defines.push_back({"MAX_SHADOW_ATLAS_SLOTS", std::to_string(MAX_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_POINT_SHADOW_ATLAS_SLOTS", std::to_string(MAX_POINT_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_SHADOWCASCADES", std::to_string(MAX_SHADOWCASCADES)});
        args.defines.push_back({"MAX_POINT_SHADOW_LIGHTS", std::to_string(MAX_POINT_SHADOW_LIGHTS)});
        args.defines.push_back({"SHADOW_CUBE_FACES", std::to_string(SHADOW_CUBE_FACES)});
    }
    if (shaderType == ShaderType::GBUFFER){
        // suppresses the terrain texture-coordinate varyings in the shared includes
        // (the G-buffer fragment does not consume them); mirrors depth.vert
        args.defines.push_back({"DEPTH_SHADER", "1"});
    }
    if (shaderType == ShaderType::SSAO){
        args.defines.push_back({"SSAO_KERNEL_SIZE", "32"});
    }
    if (shaderType == ShaderType::SSR){
        args.defines.push_back({"SSR_MAX_STEPS", "128"});
    }
    if (shaderType == ShaderType::SSR_BLUR){
        args.defines.push_back({"SSR_BLUR_TAPS", "6"});
    }
    if (shaderType == ShaderType::TITLE_ART && (properties & shaderPropertyBit(9))){
        // S83's effect programs (titleart/effects/TfxPrograms.h): their own numbers under 'Efx'
        args.frag_file = "titleart_fx.frag";
    }else if (shaderType == ShaderType::TITLE_ART && (properties & 0x7Fu) >= 18){
        // the Title Art renderer's programs (render/TitlePrograms.h); 1-17 are titleart.frag's
        args.frag_file = "titleart_render.frag";
    }
    if (shaderType == ShaderType::TITLE_ART && (properties & shaderPropertyBit(11))){
        // S83's generator programs (generators/TitleGen.h): a number space of their own, some
        // pulling instanced quads from a data texture in the vertex stage
        args.vert_file = "titleart_gen.vert";
        args.frag_file = "titleart_gen.frag";
    }

    return true;
}

// Keeps glslang's built-in symbol tables between compiles: without a standing process
// reference every compile rebuilds them, which is most of a small shader's compile time
void retainCompilerProcess() {
    static const bool retained = [](){
        shadercompiler::initialize_process();
        return true;
    }();
    (void)retained;
}

#endif // JASPERENGINE_RUNTIME_SHADERS

} // namespace

bool ShaderVariantCompiler::isAvailable(){
#ifdef JASPERENGINE_RUNTIME_SHADERS
    return !getLibrarySources().empty();
#else
    return false;
#endif
}

const std::unordered_map<std::string, std::string>& ShaderVariantCompiler::getLibrarySources(){
    // Built on first use (thread-safe), never destroyed: a worker may still compile
    // while static destructors run at exit
    static const std::unordered_map<std::string, std::string>* sources = [](){
        auto* map = new std::unordered_map<std::string, std::string>();
#ifdef JASPERENGINE_RUNTIME_SHADERS
        for (size_t i = 0; i < shaderlib::fileCount; i++){
            map->emplace(shaderlib::files[i].name, shaderlib::files[i].source);
        }
#endif
        return map;
    }();
    return *sources;
}

uint64_t ShaderVariantCompiler::hash(const std::string& data, uint64_t seed){
    uint64_t h = seed;
    for (unsigned char c : data){
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string ShaderVariantCompiler::toHex(uint64_t value){
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)value);
    return buffer;
}

uint64_t ShaderVariantCompiler::getLibraryHash(){
    static const uint64_t libraryHash = [](){
        std::string input;
        appendSignaturePart(input, "pipeline", compilerPipelineVersion);
        // engine limits the variants are compiled with (see setupShaderArgs)
        appendSignaturePart(input, "limits",
            std::to_string(TERRAIN_SHADER_BASE_LAYERS) + "," + std::to_string(MAX_LIGHTS_2D) + "," +
            std::to_string(MAX_SHADOW_ATLAS_SLOTS) + "," + std::to_string(MAX_POINT_SHADOW_ATLAS_SLOTS) + "," +
            std::to_string(MAX_SHADOWCASCADES) + "," + std::to_string(MAX_LIGHTS) + "," +
            std::to_string(SPOT_MASK_ATLAS_TILES) + "," + std::to_string(LATTICE_MAX_PER_MESH) + "," +
            std::to_string(LATTICE_TEXTURE_WIDTH) + "," + std::to_string(LATTICE_RECORD_HEADER) + "," +
            std::to_string(MAX_POINT_SHADOW_LIGHTS) + "," + std::to_string(SHADOW_CUBE_FACES) + "," +
            std::to_string(vat::VAT_MAX_CLIPS));

        const auto& sources = getLibrarySources();
        std::vector<std::string> names;
        names.reserve(sources.size());
        for (const auto& file : sources)
            names.push_back(file.first);
        std::sort(names.begin(), names.end());
        for (const std::string& name : names)
            appendSignaturePart(input, name, sources.at(name));

        return hash(input);
    }();
    return libraryHash;
}

void ShaderVariantCompiler::getCustomShaderPaths(const std::string& customShader, std::string& vertFile, std::string& fragFile){
    vertFile.clear();
    fragFile.clear();
    const std::string base = ShaderReference::getBase(customShader);
    if (base.empty())
        return;

    const size_t separator = base.find('|');
    if (separator != std::string::npos){
        vertFile = base.substr(0, separator);
        fragFile = base.substr(separator + 1);
    }else if (base.size() > 5 && base.compare(base.size() - 5, 5, ".frag") == 0){
        // fragment-only (a project UI effect): the vertex stage is the library's
        fragFile = base;
    }else{
        vertFile = base + ".vert";
        fragFile = base + ".frag";
    }
}

bool ShaderVariantCompiler::getLibraryShaderPaths(const std::string& reference, ShaderType shaderType, std::string& vertFile, std::string& fragFile, std::string* error){
    auto fail = [error](const std::string& message){
        if (error)
            *error = message;
        return false;
    };
    vertFile.clear();
    fragFile.clear();

    const std::string name = ShaderReference::getLibraryName(reference);
    if (!ShaderReference::isValidLibraryName(name))
        return fail("Invalid library shader name '" + name + "' (lower case snake_case)");

    // the manifests are always embedded; the full sources only with the runtime compiler,
    // so a build without it still tells which files a variant is made of
    auto exists = [](const std::string& path){
        return ShaderManifests::hasLibraryFile(path) || getLibrarySources().count(path) > 0;
    };
    const std::string prefix = "library/" + name;
    if (!exists(prefix + ".frag"))
        return fail("Unknown library shader '" + name + "'");

    if (shaderType == ShaderType::DEPTH){
        vertFile = exists(prefix + "_depth.vert") ? prefix + "_depth.vert" : "library/surface_depth.vert";
        fragFile = exists(prefix + "_depth.frag") ? prefix + "_depth.frag" : "library/surface_depth.frag";
        return true;
    }

    fragFile = prefix + ".frag";
    if (exists(prefix + ".vert")){
        vertFile = prefix + ".vert";
    }else if (shaderType == ShaderType::MESH){
        vertFile = "library/surface.vert";
    }else if (shaderType == ShaderType::POSTPROCESS){
        // a post-process pass: the fullscreen triangle every pass draws
        vertFile = "fullscreen.vert";
    }else{
        return fail("Library shader '" + name + "' has no vertex shader (library/" + name + ".vert)");
    }
    return true;
}

std::string ShaderVariantCompiler::getCustomShaderIncludeRoot(const std::string& customShader){
    std::string vertFile, fragFile;
    getCustomShaderPaths(customShader, vertFile, fragFile);
    // a fragment-only shader resolves against the fragment's directory
    const std::string& entry = vertFile.empty() ? fragFile : vertFile;
    if (entry.empty())
        return "";
    return std::filesystem::path(entry).parent_path().generic_string();
}

std::vector<std::string> ShaderVariantCompiler::getIncludeKeys(const std::string& source){
    auto skipBlanks = [](std::string_view line, size_t i) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        return i;
    };

    std::vector<std::string> keys;
    for (size_t lineStart = 0; lineStart <= source.size(); ) {
        size_t lineEnd = source.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = source.size();

        std::string_view line(source.data() + lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;

        size_t i = skipBlanks(line, 0);
        if (i >= line.size() || line[i] != '#') continue;
        i = skipBlanks(line, i + 1);
        if (i + 7 > line.size() || line.compare(i, 7, "include") != 0) continue;
        i = skipBlanks(line, i + 7);
        if (i >= line.size() || line[i] != '"') continue;

        size_t close = line.find('"', ++i);
        if (close != std::string_view::npos && close > i)
            keys.push_back(std::string(line.substr(i, close - i)));
    }
    return keys;
}

bool ShaderVariantCompiler::isSafeRelativePath(const std::string& path){
    if (path.empty())
        return false;
    const std::filesystem::path fsPath(path);
    if (fsPath.is_absolute() || fsPath.has_root_name() || fsPath.has_root_directory())
        return false;

    const std::filesystem::path normalized = fsPath.lexically_normal();
    return !normalized.empty() && *normalized.begin() != "..";
}

CustomShaderSources ShaderVariantCompiler::resolveCustomShaderSources(const std::string& customShader, const FileReader& readProjectFile,
                                                                     ShaderType shaderType){
    CustomShaderSources result;
    const std::string base = ShaderReference::getBase(customShader);
    std::string signatureInput;

    // the variant's feature defines ("library:toon#RAMP,RIM")
    for (const std::string& define : ShaderReference::getFeatures(customShader)){
        if (!ShaderReference::isValidDefine(define)){
            result.error = "Invalid shader feature '" + define + "' in " + customShader;
            return result;
        }
        result.defines.push_back(define);
        appendSignaturePart(signatureInput, "define", define);
    }

    // the built-in Lit with the defines its property bits have no room for (MaterialPbr)
    if (ShaderReference::isBuiltin(base)){
        if (base != ShaderReference::BUILTIN_LIT || shaderType != ShaderType::MESH){
            result.error = "Unknown built-in shader: " + customShader;
            return result;
        }
        for (const std::string& define : result.defines){
            if (!MaterialPbr::isBuiltinLitDefine(define)){
                result.error = "The built-in Lit has no define '" + define + "' (" + customShader + ")";
                return result;
            }
        }
        result.library = true;
        result.vertFile = "mesh.vert";
        result.fragFile = "mesh.frag";
        if (!getLibrarySources().count(result.vertFile) || !getLibrarySources().count(result.fragFile)){
            result.error = "This build has no engine shader library to compile " + base + " from";
            return result;
        }
        appendSignaturePart(signatureInput, "library-hash", std::to_string(getLibraryHash()));
        result.signature = signatureInput;
        result.valid = true;
        return result;
    }

    if (ShaderReference::isLibrary(base)){
        result.library = true;
        result.manifest = true;
        if (!getLibraryShaderPaths(base, shaderType, result.vertFile, result.fragFile, &result.error))
            return result;
        const auto& library = getLibrarySources();
        for (const std::string& entry : {result.vertFile, result.fragFile}){
            auto it = library.find(entry);
            if (it == library.end()){
                result.error = "This build has no engine shader library to compile " + base + " from";
                return result;
            }
            appendSignaturePart(signatureInput, "library:" + entry, it->second);
        }
        // the includes it reaches are the embedded library's too: a cached build of it is
        // stale once the library changes
        appendSignaturePart(signatureInput, "library-hash", std::to_string(getLibraryHash()));
        // the varyings its @require asks surface_vs.glsl for (a depth program too), with the
        // feature= ones of the variant's features
        if (std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(base)){
            const std::vector<std::string> enabled = result.defines;
            for (const std::string& define : manifest->getRequireDefines(enabled))
                result.defines.push_back(define);
        }
        result.signature = signatureInput;
        result.valid = true;
        return result;
    }

    if (base.empty() || !readProjectFile){
        result.error = "Custom shader project or path is unavailable.";
        return result;
    }

    getCustomShaderPaths(base, result.vertFile, result.fragFile);
    const std::string includeRoot = getCustomShaderIncludeRoot(base);

    auto loadEntryPoint = [&](const std::string& rel, const char* stage) -> bool {
        std::string content;
        if (!isSafeRelativePath(rel)) {
            result.error = std::string("Unsafe custom ") + stage + " shader path: " + rel;
            return false;
        }
        if (!readProjectFile(rel, content)) {
            result.error = std::string("Could not open custom ") + stage + " shader: " + rel;
            return false;
        }

        result.files[rel] = content;
        appendSignaturePart(signatureInput, std::string("entry:") + rel, content);
        return true;
    };

    // a fragment-only shader has no vertex entry point of its own
    if ((!result.vertFile.empty() && !loadEntryPoint(result.vertFile, "vertex")) || !loadEntryPoint(result.fragFile, "fragment"))
        return result;

    // An empty include root means the entry point sits at the project root, where the
    // first two candidates would be the same file.
    const bool hasLocalRoot = !includeRoot.empty() && includeRoot != ".";
    const auto& library = getLibrarySources();

    std::unordered_set<std::string> visited;
    std::function<void(const std::string&)> visitSource = [&](const std::string& source) {
        for (const std::string& key : getIncludeKeys(source)) {
            if (!visited.insert(key).second)
                continue;

            if (!isSafeRelativePath(key)) {
                appendSignaturePart(signatureInput, "unsafe:" + key, "");
                continue;
            }

            std::string content;
            std::string origin;
            if (hasLocalRoot && readProjectFile(includeRoot + "/" + key, content)) {
                origin = "local:" + key;
                result.files[key] = content;
            } else if (readProjectFile(key, content)) {
                origin = "project:" + key;
                result.files[key] = content;
            } else if (auto engineIt = library.find(key); engineIt != library.end()) {
                origin = "engine:" + key;
                content = engineIt->second;
            } else {
                appendSignaturePart(signatureInput, "missing:" + key, "");
                continue;
            }

            appendSignaturePart(signatureInput, origin, content);
            visitSource(content);
        }
    };

    // copies: visiting can insert into result.files
    const std::string vertSource = result.vertFile.empty() ? std::string() : result.files[result.vertFile];
    const std::string fragSource = result.files[result.fragFile];
    visitSource(vertSource);
    visitSource(fragSource);

    // a fork with a manifest compiles with the varyings its @require asks for, as a library
    // shader does (the entry sources are already in the signature)
    ShaderManifest manifest;
    manifest.parse(fragSource, result.fragFile);
    manifest.parse(vertSource, result.vertFile);
    const std::vector<std::string> enabled = result.defines;
    for (const std::string& define : manifest.getRequireDefines(enabled))
        result.defines.push_back(define);
    result.manifest = manifest.present;

    result.signature = signatureInput;
    result.valid = true;
    return result;
}

#ifdef JASPERENGINE_RUNTIME_SHADERS
namespace {

// past the fragment uniforms the backend guarantees: on GLES3 / WebGL2 224 vectors and on desktop GL
// 256 (GL_MAX_FRAGMENT_UNIFORM_VECTORS, 1024 components in GL 3.3 and 4.1: sokol's GL backends upload
// each block as plain uniforms), and on every backend 8 blocks a stage (sokol's
// _SG_MAX_UNIFORMBLOCK_BINDINGS_PER_STAGE; D3D11, Metal and Vulkan have no total of vectors, and
// their per-block sizes, 4096 vectors and up, are far from the engine's). extraVectors and
// extraBlocks are what a sibling variant adds (a colour variant's OIT twin: u_fs_oit)
bool isOverUniformLimits(const ShaderData& data, ShaderBackend backend, size_t extraVectors, size_t extraBlocks){
    size_t vectorLimit = SIZE_MAX;
    if (backend == ShaderBackend::GLES3)
        vectorLimit = 224;
    else if (backend == ShaderBackend::GLCore)
        vectorLimit = 256;
    for (const ShaderStage& stage : data.stages){
        if (stage.type != ShaderStageType::FRAGMENT)
            continue;
        size_t vectors = extraVectors;
        for (const ShaderUniformBlock& block : stage.uniformblocks)
            vectors += (block.sizeBytes + 15) / 16;
        return vectors > vectorLimit || stage.uniformblocks.size() + extraBlocks > 8;
    }
    return false;
}

// past what every backend can bind: the 16 samplers sokol has for both stages together
// (SG_MAX_SAMPLER_BINDSLOTS), or the 16 fragment texture units of GLES3, WebGL2 and macOS GL
bool isOverBindLimits(const ShaderData& data){
    size_t samplers = 0;
    for (const ShaderStage& stage : data.stages){
        samplers += stage.samplers.size();
        if (stage.type == ShaderStageType::FRAGMENT && (stage.textures.size() > 16 || stage.textureSamplerPairs.size() > 16))
            return true;
    }
    return samplers > 16;
}

// The compiler arguments of one variant for a backend: entry points, sources and #defines
// (compile and preprocess). False with an error when the key or its sources are unusable.
bool prepareVariantArgs(ShaderKey key, ShaderBackend backend, const CustomShaderSources* customSources,
                        shadercompiler::args_t& args, std::string& error){
    const ShaderType shaderType = ShaderPool::getShaderTypeFromKey(key);
    const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);

    args = shadercompiler::initialize_args();
    args.useBuffers = true;
    args.fileBuffers = ShaderVariantCompiler::getLibrarySources();
    applyShaderBackend(args, backend);

    // Built-in entry-point files + property #defines (the variant system is identical
    // for forked shaders)
    if (!setupShaderArgs(args, shaderType, properties)){
        error = "Unknown shader type";
        return false;
    }

    if (args.lang == shadercompiler::LANG_GLSL &&
            (shaderType == ShaderType::MESH || shaderType == ShaderType::DEPTH ||
             shaderType == ShaderType::GBUFFER))
        args.defines.push_back({"SKINNING_TEXTURE", "1"});

    if (shaderType == ShaderType::UI_EFFECT && customId == 0){
        error = "A UI effect shader needs its effect (the custom shader id)";
        return false;
    }

    if (ShaderPool::isLibraryCustomShader(customId)) {
        // an effect of the engine library ("@effects/<name>"): its sources are built in
        const std::string custom = ShaderPool::getCustomShaderName(customId);
        const std::string prefix = "@effects/";
        if (shaderType != ShaderType::UI_EFFECT || custom.rfind(prefix, 0) != 0){
            error = "Unknown library shader: " + custom;
            return false;
        }
        const std::string fragFile = "effects/" + custom.substr(prefix.size()) + ".frag";
        if (!args.fileBuffers.count(fragFile)){
            error = "Unknown UI effect: " + custom.substr(prefix.size());
            return false;
        }
        args.frag_file = fragFile;
    } else if (customId != 0) {
        if (!customSources){
            error = "Custom shader requested without its sources";
            return false;
        }
        if (!customSources->valid){
            error = customSources->error.empty() ? "Could not resolve custom shader sources" : customSources->error;
            return false;
        }

        // Overlay the fork's entry points and every project include they reach onto the
        // engine library. Resolution order is the entry point's own directory, the
        // project root, then the engine library. A fragment-only shader keeps the
        // library's vertex stage.
        for (const auto& [path, content] : customSources->files)
            args.fileBuffers[path] = content;
        if (!customSources->vertFile.empty())
            args.vert_file = customSources->vertFile;
        args.frag_file = customSources->fragFile;
        // enabled @feature / @keywords defines of the variant
        for (const std::string& define : customSources->defines)
            args.defines.push_back({define, "1"});
        // a pixel-lit (USE_PUNCTUAL) mesh variant of a manifest shader: its fragment stage
        // evaluates the lights at the world position and normal (includes/lights.glsl), so
        // surface_vs.glsl writes them whatever @require says
        if (customSources->manifest && shaderType == ShaderType::MESH && (properties & shaderPropertyBit(3)) && !(properties & shaderPropertyBit(0))){
            for (const char* define : {"SURFACE_POSITION", "SURFACE_NORMALS"}){
                if (std::find(customSources->defines.begin(), customSources->defines.end(), define) == customSources->defines.end())
                    args.defines.push_back({define, "1"});
            }
        }
    }

    // S57's OIT accumulation variant ('Oit'): the shader's own main runs, then
    // includes/oit.glsl turns its straight-alpha g_finalColor into the two accumulation targets
    if (shaderType == ShaderType::MESH && (properties & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::OIT))){
        auto source = args.fileBuffers.find(args.frag_file);
        if (source == args.fileBuffers.end() || source->second.find("g_finalColor") == std::string::npos){
            error = "Order Independent transparency needs a fragment shader writing g_finalColor (" + args.frag_file + ")";
            return false;
        }
        std::string wrapped = source->second;
        const size_t versionEnd = wrapped.find('\n');
        if (wrapped.compare(0, 8, "#version") != 0 || versionEnd == std::string::npos){
            error = "Order Independent transparency needs a fragment shader starting with #version (" + args.frag_file + ")";
            return false;
        }
        // the line numbers of errors stay the file's
        wrapped.insert(versionEnd + 1, "#define main oitShadeMain\n#line 2\n");
        wrapped += "\n#undef main\n#include \"includes/oit.glsl\"\nvoid main(){\n    oitShadeMain();\n    g_finalColor = oitAccumulate(g_finalColor);\n}\n";
        source->second = wrapped;
    }

    // phase 6: a stateful effect's passes and states, from its manifest (any form: the draw reads
    // the states too)
    if (shaderType == ShaderType::UI_EFFECT) {
        auto source = args.fileBuffers.find(args.frag_file);
        if (source != args.fileBuffers.end())
            addUiEffectStateDefinitions(source->second, args);
    }
    return true;
}

} // namespace
#endif // JASPERENGINE_RUNTIME_SHADERS

bool ShaderVariantCompiler::preprocess(ShaderKey key, ShaderBackend backend, const CustomShaderSources* customSources,
                                       std::string& vertexSource, std::string& fragmentSource, std::string* error){
    vertexSource.clear();
    fragmentSource.clear();
    auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };

#ifdef JASPERENGINE_RUNTIME_SHADERS
    if (!isAvailable())
        return fail("This build has no shader library to preprocess");

    retainCompilerProcess();

    shadercompiler::args_t args;
    std::string setupError;
    if (!prepareVariantArgs(key, backend, customSources, args, setupError))
        return fail(setupError);
    std::string log;
    args.error_output = &log;

    std::vector<shadercompiler::input_t> inputs;
    if (!shadercompiler::load_input(inputs, args))
        return fail("Error loading shader input");

    std::vector<std::string> outputs;
    if (!shadercompiler::preprocess(outputs, inputs, args))
        return fail("Error preprocessing: " + log);

    for (size_t i = 0; i < inputs.size(); i++)
        (inputs[i].stage_type == shadercompiler::STAGE_VERTEX ? vertexSource : fragmentSource) = outputs[i];
    return true;
#else
    (void)key; (void)backend; (void)customSources;
    return fail("This build has no runtime shader compiler (JASPERENGINE_RUNTIME_SHADERS is off)");
#endif
}

std::string ShaderVariantCompiler::normalizePreprocessed(const std::string& source){
    std::string normalized;
    normalized.reserve(source.size());
    size_t start = 0;
    while (start < source.size()){
        size_t end = source.find('\n', start);
        if (end == std::string::npos)
            end = source.size();
        size_t first = start;
        while (first < end && (source[first] == ' ' || source[first] == '\t' || source[first] == '\r'))
            first++;
        size_t last = end;
        while (last > first && (source[last - 1] == ' ' || source[last - 1] == '\t' || source[last - 1] == '\r'))
            last--;
        // a #line directive only numbers what follows: an added comment or a guarded-out
        // block moves it without changing the program
        if (last > first && source.compare(first, 5, "#line") != 0){
            normalized.append(source, first, last - first);
            normalized += '\n';
        }
        start = end + 1;
    }
    return normalized;
}

bool ShaderVariantCompiler::compileSources(const std::string& vertexSource, const std::string& fragmentSource, ShaderBackend backend,
                                           ShaderData& out, std::string* error, std::string* log){
    auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
#if defined(JASPERENGINE_RUNTIME_SHADERS)
    retainCompilerProcess();
    shadercompiler::args_t args = shadercompiler::initialize_args();
    args.useBuffers = true;
    args.fileBuffers["source.vert"] = vertexSource;
    args.fileBuffers["source.frag"] = fragmentSource;
    args.vert_file = "source.vert";
    args.frag_file = "source.frag";
    applyShaderBackend(args, backend);
    std::string compileLog;
    args.error_output = &compileLog;
    auto failWithLog = [&](const std::string& message) {
        if (log)
            *log = compileLog;
        return fail(message);
    };

    std::vector<shadercompiler::input_t> inputs;
    if (!shadercompiler::load_input(inputs, args))
        return failWithLog("Error loading shader input");
    std::vector<shadercompiler::spirv_t> spirvvec(inputs.size());
    if (!shadercompiler::compile_to_spirv(spirvvec, inputs, args))
        return failWithLog("Error compiling to SPIRV");
    std::vector<shadercompiler::spirvcross_t> spirvcrossvec(inputs.size());
    bool crossCompiled = false;
    try {
        crossCompiled = shadercompiler::compile_to_lang(spirvcrossvec, spirvvec, inputs, args);
    } catch (const std::exception& e) {
        compileLog += std::string("SPIRV-Cross: ") + e.what() + "\n";
    }
    if (!crossCompiled)
        return failWithLog("Error cross-compiling");
    args.output_basename = "procedural" + getLangSuffix(args.lang, args.version, args.es, args.platform);
    out = convertToShaderData(spirvcrossvec, args);
    if (log)
        *log = compileLog;
    return true;
#else
    (void)vertexSource; (void)fragmentSource; (void)backend; (void)out; (void)log;
    return fail("This build has no runtime shader compiler (JASPERENGINE_RUNTIME_SHADERS is off)");
#endif
}

bool ShaderVariantCompiler::compileFragmentToSpirv(const std::string& fragmentSource, std::vector<uint32_t>& spirv,
                                                   std::string* error, std::string* log){
    auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
#if defined(JASPERENGINE_RUNTIME_SHADERS)
    retainCompilerProcess();
    shadercompiler::args_t args = shadercompiler::initialize_args();
    args.useBuffers = true;
    args.fileBuffers["source.frag"] = fragmentSource;
    args.frag_file = "source.frag";
    applyShaderBackend(args, ShaderBackend::Vulkan);
    std::string compileLog;
    args.error_output = &compileLog;
    std::vector<shadercompiler::input_t> inputs;
    std::vector<shadercompiler::spirv_t> spirvvec(1);
    const bool loaded = shadercompiler::load_input(inputs, args);
    if (!loaded || inputs.size() != 1 || !shadercompiler::compile_to_spirv(spirvvec, inputs, args)) {
        if (log)
            *log = compileLog;
        return fail(loaded ? "Error compiling to SPIRV" : "Error loading shader input");
    }
    spirv = spirvvec[0].bytecode;
    if (log)
        *log = compileLog;
    return true;
#else
    (void)fragmentSource; (void)spirv; (void)log;
    return fail("This build has no runtime shader compiler (JASPERENGINE_RUNTIME_SHADERS is off)");
#endif
}

bool ShaderVariantCompiler::compile(ShaderKey key, ShaderBackend backend, const CustomShaderSources* customSources,
                                    ShaderData& out, std::string* error, std::string* log,
                                    const ProgressFn& progress, const std::function<bool()>& cancelled){
    auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
    auto report = [&](float value) {
        if (progress)
            progress(value);
    };
    auto stopped = [&]() {
        return cancelled && cancelled();
    };

#ifdef JASPERENGINE_RUNTIME_SHADERS
    if (!isAvailable())
        return fail("This build has no shader library to compile from");

    retainCompilerProcess();

    const ShaderType shaderType = ShaderPool::getShaderTypeFromKey(key);
    const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);

    std::string compileLog;
    // one pass of the pipeline: "" when out holds the variant, else what failed
    // what a pass leaves out or reads otherwise (the fallbacks below, in their order)
    struct Fallbacks {
        bool lightRecords = false;
        bool rawViewOff = false;
        bool spotMasksOff = false;
        bool shadows2DOff = false;
    };
    Fallbacks taken;
    auto build = [&](const Fallbacks& fallbacks) -> std::string {
        // (a retry reports no progress: the first pass did)
        const bool retry = fallbacks.lightRecords || fallbacks.rawViewOff || fallbacks.spotMasksOff || fallbacks.shadows2DOff;
        shadercompiler::args_t args;
        std::string setupError;
        if (!prepareVariantArgs(key, backend, customSources, args, setupError))
            return setupError;
        if (fallbacks.lightRecords)
            args.defines.push_back({"LIGHTS_FS_RECORDS", "1"});
        if (fallbacks.rawViewOff)
            args.defines.push_back({"SHADOW_RAW_VIEW_OFF", "1"});
        if (fallbacks.spotMasksOff)
            args.defines.push_back({"SPOT_MASKS_OFF", "1"});
        if (fallbacks.shadows2DOff)
            args.defines.push_back({"SHADOWS_2D_OFF", "1"});
        args.error_output = &compileLog;

        if (stopped())
            return "Cancelled";
        if (!retry)
            report(0.3f);

        std::vector<shadercompiler::input_t> inputs;
        if (!shadercompiler::load_input(inputs, args))
            return "Error loading shader input";

        if (stopped())
            return "Cancelled";
        if (!retry)
            report(0.5f);

        std::vector<shadercompiler::spirv_t> spirvvec;
        spirvvec.resize(inputs.size());
        if (!shadercompiler::compile_to_spirv(spirvvec, inputs, args))
            return "Error compiling to SPIRV";

        if (stopped())
            return "Cancelled";
        if (!retry)
            report(0.8f);

        std::vector<shadercompiler::spirvcross_t> spirvcrossvec;
        spirvcrossvec.resize(inputs.size());
        bool crossCompiled = false;
        try {
            crossCompiled = shadercompiler::compile_to_lang(spirvcrossvec, spirvvec, inputs, args);
        } catch (const std::exception& e) {
            compileLog += std::string("SPIRV-Cross: ") + e.what() + "\n";
        }
        if (!crossCompiled)
            return "Error cross-compiling";

        if (stopped())
            return "Cancelled";
        if (!retry)
            report(0.9f);

        // A deterministic basename names the stages
        args.output_basename = ShaderPool::getShaderStr(shaderType, properties, customId) +
                               getLangSuffix(args.lang, args.version, args.es, args.platform);
        out = convertToShaderData(spirvcrossvec, args);
        return "";
    };

    std::string failure = build(taken);
    // a lit variant whose lights' blocks would take it past this backend's fragment uniforms (GLES3
    // and WebGL2 224 vectors, desktop GL 256, 8 blocks everywhere) reads its lights, capsules and 2D
    // lights from u_lightRecords (includes/lights.glsl, LIGHTS_FS_RECORDS): it draws the same. A
    // backend it fits keeps the blocks, and the textures they leave for PCSS and the masks. A colour
    // variant decides with its OIT twin's block (u_fs_oit, one vector), so both take the same form
    // (the OIT draws apply the colour variant's slots)
    const uint64_t oitBit = ShaderPool::getLightingProperty(ShaderPool::LightingFeature::OIT);
    const bool oitTwin = shaderType == ShaderType::MESH && !(properties & oitBit);
    if (failure.empty() && isOverUniformLimits(out, backend, oitTwin ? 1 : 0, oitTwin ? 1 : 0) &&
            out.getUniformBlockIndexByName("u_fs_objectLights") >= 0 && out.getTextureIndexByName("u_lightRecords").first < 0) {
        const std::string blockForm = compileLog;
        compileLog += std::string(LIGHT_RECORDS_TAKEN) + ": the lights' blocks would take the variant past this backend's fragment "
                      "uniforms; it reads its lights from the frame's light records\n";
        taken.lightRecords = true;
        failure = build(taken);
        // a fork reading the lights' blocks itself (objectLights.*, lighting2d.*) builds in them
        if (!failure.empty() && failure != "Cancelled") {
            const std::string name = ShaderPool::getShaderStr(shaderType, properties, customId) +
                                     (customId ? " (" + ShaderPool::getCustomShaderName(customId) + ")" : std::string());
            // (once a variant, whatever the backends it builds for)
            static std::mutex warnedMutex;
            static std::set<std::string> warned;
            bool first = false;
            {
                std::lock_guard<std::mutex> lock(warnedMutex);
                first = warned.insert(name).second;
            }
            if (first)
                Log::warn("Shader %s reads the lights' blocks itself: it keeps them, past the guaranteed fragment uniforms "
                          "(GLES3 and WebGL2: 224 vectors and 8 blocks); read the lights through includes/lights.glsl's functions",
                          name.c_str());
            compileLog = blockForm + std::string(LIGHT_RECORDS_FAILED) + ": " + name + " does not build with the light records; "
                         "it keeps the lights' blocks\n";
            taken.lightRecords = false;
            failure = build(taken);
        }
    }
    // a variant the raw-depth view of the shadow atlas (PCSS) takes past the bind limits is built
    // without it (on every backend: the same program everywhere), and filters the Ultra tier's
    // shadows with the High tier's Vogel disk
    if (failure.empty() && isOverBindLimits(out) && out.getTextureIndex(TextureShaderType::SHADOWATLASRAW).first >= 0) {
        compileLog += std::string(PCSS_LEFT_OUT) + ": the raw-depth shadow view would take the variant past 16 samplers or "
                      "16 fragment textures; it filters Ultra shadows with the Vogel disk\n";
        taken.rawViewOff = true;
        failure = build(taken);
    }
    // a variant whose lights' records (u_lightRecords, includes/lights.glsl) still leave it past
    // them is built without the spot lights' masks (SPOT_MASKS_OFF): a masked spot light lights it
    // with its plain cone
    if (failure.empty() && isOverBindLimits(out) && out.getTextureIndexByName("u_lightRecords").first >= 0 &&
            out.getTextureIndex(TextureShaderType::SPOTMASKATLAS).first >= 0) {
        compileLog += std::string(SPOT_MASKS_LEFT_OUT) + ": the lights' records would take the variant past 16 samplers or "
                      "16 fragment textures; masked spot lights light it with their cones\n";
        taken.spotMasksOff = true;
        failure = build(taken);
    }
    // then without the 2D lights' shadows (SHADOWS_2D_OFF: the painted terrain lit by 2D lights
    // with their shadows): its 2D lights light it unshadowed
    if (failure.empty() && isOverBindLimits(out) && out.getTextureIndexByName("u_lightRecords").first >= 0 &&
            out.getTextureIndexByName("u_shadow2DAtlas").first >= 0) {
        compileLog += std::string(SHADOWS_2D_LEFT_OUT) + ": the lights' records would take the variant past 16 samplers or "
                      "16 fragment textures; its 2D lights light it unshadowed\n";
        taken.shadows2DOff = true;
        failure = build(taken);
    }
    if (!failure.empty()) {
        if (log && failure != "Cancelled")
            *log = compileLog;
        return fail(failure);
    }

    // a globals block (ShaderGlobals) takes at most 64 vec4 slots of a stage's uniform space
    // (GLES3 guarantees 256 vertex uniform vectors in all): a larger one fails like any error
    const std::string globalsProblem = checkGlobalsBlocks(out);
    if (!globalsProblem.empty()) {
        compileLog += globalsProblem + "\n";
        if (log)
            *log = compileLog;
        return fail(globalsProblem);
    }

    if (log)
        *log = compileLog;
    report(1.0f);
    return true;
#else
    (void)key; (void)backend; (void)customSources; (void)out; (void)log;
    (void)report; (void)stopped;
    return fail("This build has no runtime shader compiler (JASPERENGINE_RUNTIME_SHADERS is off)");
#endif
}

std::string ShaderVariantCompiler::checkGlobalsBlocks(const ShaderData& data) {
    for (const ShaderStage& stage : data.stages) {
        for (const ShaderUniformBlock& block : stage.uniformblocks) {
            if ((block.name == ShaderGlobals::VERTEX_BLOCK || block.name == ShaderGlobals::FRAGMENT_BLOCK) &&
                    block.sizeBytes > ShaderGlobals::MAX_BLOCK_BYTES) {
                return "globals-block-too-large: " + block.name + " is " + std::to_string(block.sizeBytes) + " bytes, over the " +
                       std::to_string(ShaderGlobals::MAX_BLOCK_BYTES) + " (64 vec4) a globals block may take: declare only the globals the stage reads";
            }
        }
    }
    return "";
}

std::string ShaderVariantCompiler::checkResourceBudget(const ShaderData& data) {
    std::string over;
    for (const ShaderStage& stage : data.stages) {
        const char* name = stage.type == ShaderStageType::VERTEX ? "vertex" : "fragment";
        auto check = [&](size_t count, size_t limit, const char* what) {
            if (count > limit) {
                if (!over.empty()) over += "; ";
                over += std::string(name) + " stage: " + std::to_string(count) + " " + what + " (limit " + std::to_string(limit) + ")";
            }
        };
        // which blocks: the one to fold into another is the fix
        std::string blocks = "uniform blocks";
        if (stage.uniformblocks.size() > 8) {
            for (size_t i = 0; i < stage.uniformblocks.size(); i++) blocks += (i ? ", " : ": ") + stage.uniformblocks[i].name;
        }
        check(stage.uniformblocks.size(), 8, blocks.c_str());
        check(stage.textures.size(), 16, "textures");
        check(stage.samplers.size(), 16, "samplers");
        check(stage.textureSamplerPairs.size(), 16, "texture-sampler pairs");
    }
    // sokol's sampler slots are shared by the stages (SG_MAX_SAMPLER_BINDSLOTS)
    size_t samplers = 0;
    for (const ShaderStage& stage : data.stages)
        samplers += stage.samplers.size();
    if (samplers > 16) {
        if (!over.empty()) over += "; ";
        over += "all stages: " + std::to_string(samplers) + " samplers (limit 16)";
    }
    return over;
}
