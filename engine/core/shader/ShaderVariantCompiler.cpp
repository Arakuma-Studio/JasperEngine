// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderVariantCompiler.h"

#include "Engine.h"
#include "pool/ShaderPool.h"
#include "component/TerrainComponent.h"
#include "shader/ShaderManifest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>
#include <unordered_set>

#ifdef JASPERENGINE_RUNTIME_SHADERS
#include "shadercompiler.h"
#include "shaderlib.h"
#endif

using namespace jasperengine;

namespace {

// Bump when the compile pipeline changes its output for the same sources (glslang or
// SPIRV-Cross update, different options), so runtime caches of the old output go stale.
constexpr const char* compilerPipelineVersion = "jasperengine-shadercompiler-1";

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

void addMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop) {
    if (prop & (1 << 0))  defs.push_back({"MATERIAL_UNLIT", "1"});            // 'Ult'
    if (prop & (1 << 1))  defs.push_back({"HAS_UV_SET1", "1"});               // 'Uv1'
    if (prop & (1 << 2))  defs.push_back({"HAS_UV_SET2", "1"});               // 'Uv2'
    if (prop & (1 << 3))  defs.push_back({"USE_PUNCTUAL", "1"});              // 'Puc'
    if (prop & (1 << 4))  defs.push_back({"USE_SHADOWS", "1"});               // 'Shw'
    // bit 5 reserved (was USE_SHADOWS_PCF; the PCF filter is uniform-driven now)
    if (prop & (1 << 6))  defs.push_back({"HAS_NORMALS", "1"});               // 'Nor'
    if (prop & (1 << 7))  defs.push_back({"HAS_NORMAL_MAP", "1"});            // 'Nmp'
    if (prop & (1 << 8))  defs.push_back({"HAS_TANGENTS", "1"});              // 'Tan'
    if (prop & (1 << 9))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});     // 'Vc3'
    if (prop & (1 << 10)) defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});     // 'Vc4'
    if (prop & (1 << 11)) defs.push_back({"HAS_TEXTURERECT", "1"});           // 'Txr'
    if (prop & (1 << 12)) defs.push_back({"HAS_FOG", "1"});                   // 'Fog'
    if (prop & (1 << 13)) defs.push_back({"HAS_SKINNING", "1"});              // 'Ski'
    if (prop & (1 << 14)) defs.push_back({"HAS_MORPHTARGET", "1"});           // 'Mta'
    if (prop & (1 << 15)) defs.push_back({"HAS_MORPHNORMAL", "1"});           // 'Mnr'
    if (prop & (1 << 16)) defs.push_back({"HAS_MORPHTANGENT", "1"});          // 'Mtg'
    if (prop & (1 << 17)) defs.push_back({"HAS_TERRAIN", "1"});               // 'Ter'
    if (prop & (1 << 18)) defs.push_back({"HAS_INSTANCING", "1"});            // 'Ist'
    if (prop & (1 << 19)) defs.push_back({"USE_IBL", "1"});                   // 'Ibl'
    if (prop & (1 << 20)) defs.push_back({"USE_MIRROR", "1"});                // 'Mir'
    if (prop & (1 << 21)) defs.push_back({"USE_SSAO", "1"});                  // 'Sao'
    if (prop & (1 << 22)) defs.push_back({"USE_LIGHT2D", "1"});               // 'L2d'
    if (prop & (1 << 23)) defs.push_back({"USE_SHADOWS_2D", "1"});            // 'S2d'
    if (prop & (1 << 24)) defs.push_back({"ALPHA_MASK", "1"});                 // 'Ams'
    if (prop & (1 << 25)) defs.push_back({"ALPHA_OPAQUE", "1"});               // 'Aop'
    if (prop & (1 << 26)) defs.push_back({"USE_INSTANCE_FADE", "1"});          // 'Ifd'
    if (prop & (1 << 27)) defs.push_back({"HAS_TERRAIN_PBR", "1"});            // 'Tpb'
    if (prop & (1 << 28)) defs.push_back({"USE_VERTEX_LIGHTS", "1"});          // 'Vtl'
    // bits 29-30: capacity tier of the per-object lights blocks ('Lc4', 'L16', neither = 8)
    defs.push_back({"LIGHT_CAPACITY", (prop & (1 << 29)) ? "4" : ((prop & (1 << 30)) ? "16" : "8")});
}

void addDepthMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop) {
    if (prop & (1 << 0))  defs.push_back({"HAS_TEXTURE", "1"});       // 'Tex'
    if (prop & (1 << 1))  defs.push_back({"HAS_SKINNING", "1"});      // 'Ski'
    if (prop & (1 << 2))  defs.push_back({"HAS_MORPHTARGET", "1"});   // 'Mta'
    if (prop & (1 << 3))  defs.push_back({"HAS_MORPHNORMAL", "1"});   // 'Mnr'
    if (prop & (1 << 4))  defs.push_back({"HAS_MORPHTANGENT", "1"});  // 'Mtg'
    if (prop & (1 << 5))  defs.push_back({"HAS_TERRAIN", "1"});       // 'Ter'
    if (prop & (1 << 6))  defs.push_back({"HAS_INSTANCING", "1"});    // 'Ist'
    if (prop & (1 << 7))  defs.push_back({"ALPHA_MASK", "1"});         // 'Ams'
    if (prop & (1 << 8))  defs.push_back({"USE_INSTANCE_FADE", "1"}); // 'Ifd'
    if (prop & (1 << 9))  defs.push_back({"HAS_NORMALS", "1"});           // 'Nor'
    if (prop & (1 << 10)) defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"}); // 'Vc3'
    if (prop & (1 << 11)) defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"}); // 'Vc4'
    if (prop & (1 << 12)) defs.push_back({"HAS_UV_SET1", "1"});           // 'Uv1'
}

void addGBufferMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop) {
    if (prop & (1 << 0))  defs.push_back({"HAS_BASECOLOR_TEXTURE", "1"});         // 'Bct'
    if (prop & (1 << 1))  defs.push_back({"HAS_NORMALS", "1"});                   // 'Nor'
    if (prop & (1 << 2))  defs.push_back({"HAS_SKINNING", "1"});                  // 'Ski'
    if (prop & (1 << 3))  defs.push_back({"HAS_MORPHTARGET", "1"});               // 'Mta'
    if (prop & (1 << 4))  defs.push_back({"HAS_MORPHNORMAL", "1"});               // 'Mnr'
    if (prop & (1 << 5))  defs.push_back({"HAS_MORPHTANGENT", "1"});              // 'Mtg'
    if (prop & (1 << 6))  defs.push_back({"HAS_TERRAIN", "1"});                   // 'Ter'
    if (prop & (1 << 7))  defs.push_back({"HAS_INSTANCING", "1"});                // 'Ist'
    if (prop & (1 << 8))  defs.push_back({"HAS_METALLICROUGHNESS_TEXTURE", "1"}); // 'Mrt'
    if (prop & (1 << 9))  defs.push_back({"HAS_TERRAIN_PBR", "1"});               // 'Tpb'
}

void addUIPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop) {
    if (prop & (1 << 0))  defs.push_back({"HAS_TEXTURE", "1"});              // 'Tex'
    if (prop & (1 << 1))  defs.push_back({"HAS_FONTATLAS_TEXTURE", "1"});    // 'Ftx'
    if (prop & (1 << 2))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});    // 'Vc3'
    if (prop & (1 << 3))  defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});    // 'Vc4'
}

void addPointsPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop) {
    if (prop & (1 << 0))  defs.push_back({"HAS_TEXTURE", "1"});              // 'Tex'
    if (prop & (1 << 1))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});    // 'Vc3'
    if (prop & (1 << 2))  defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});    // 'Vc4'
    if (prop & (1 << 3))  defs.push_back({"HAS_TEXTURERECT", "1"});          // 'Txr'
}

void addLinesPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop) {
    if (prop & (1 << 0))  defs.push_back({"HAS_VERTEX_COLOR_VEC3", "1"});    // 'Vc3'
    if (prop & (1 << 1))  defs.push_back({"HAS_VERTEX_COLOR_VEC4", "1"});    // 'Vc4'
}

// Entry points and #defines of a built-in variant
bool setupShaderArgs(shadercompiler::args_t& args, ShaderType shaderType, uint32_t properties) {
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
    }else if (shaderType == ShaderType::LINES){
        args.vert_file = "lines.vert";
        args.frag_file = "lines.frag";
        addLinesPropertyDefinitions(args.defines, properties);
    }else if (shaderType == ShaderType::SKYBOX){
        args.vert_file = "sky.vert";
        args.frag_file = "sky.frag";
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
        args.vert_file = "fullscreen.vert";
        args.frag_file = "blit.frag";
    }else if (shaderType == ShaderType::POSTPROCESS){
        args.vert_file = "fullscreen.vert";
        args.frag_file = "postprocess.frag";
    }else{
        return false;
    }

    if (shaderType == ShaderType::MESH || shaderType == ShaderType::GBUFFER){
        args.defines.push_back({"MAX_TERRAIN_LAYERS", std::to_string(MAX_TERRAIN_LAYERS)});
    }
    if (shaderType == ShaderType::MESH){
        args.defines.push_back({"MAX_LIGHTS", std::to_string(MAX_LIGHTS)}); // legacy u_fs_lighting
        args.defines.push_back({"SPOT_MASK_TILES", std::to_string(SPOT_MASK_ATLAS_TILES)});
        args.defines.push_back({"MAX_LIGHTS_2D", std::to_string(MAX_LIGHTS_2D)});
        args.defines.push_back({"MAX_SHADOW_ATLAS_SLOTS", std::to_string(MAX_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_POINT_SHADOW_ATLAS_SLOTS", std::to_string(MAX_POINT_SHADOW_ATLAS_SLOTS)});
        args.defines.push_back({"MAX_SHADOWCASCADES", std::to_string(MAX_SHADOWCASCADES)});
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
            std::to_string(MAX_TERRAIN_LAYERS) + "," + std::to_string(MAX_LIGHTS_2D) + "," +
            std::to_string(MAX_SHADOW_ATLAS_SLOTS) + "," + std::to_string(MAX_POINT_SHADOW_ATLAS_SLOTS) + "," +
            std::to_string(MAX_SHADOWCASCADES) + "," + std::to_string(MAX_LIGHTS) + "," +
            std::to_string(SPOT_MASK_ATLAS_TILES));

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
    }else{
        return fail("Library shader '" + name + "' has no vertex shader (library/" + name + ".vert)");
    }
    return true;
}

std::string ShaderVariantCompiler::getCustomShaderIncludeRoot(const std::string& customShader){
    std::string vertFile, fragFile;
    getCustomShaderPaths(customShader, vertFile, fragFile);
    if (vertFile.empty())
        return "";
    return std::filesystem::path(vertFile).parent_path().generic_string();
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
        // the varyings its @require asks surface_vs.glsl for (a depth program too)
        if (std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(base)){
            for (const std::string& define : manifest->getRequireDefines())
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

    if (!loadEntryPoint(result.vertFile, "vertex") || !loadEntryPoint(result.fragFile, "fragment"))
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
    const std::string vertSource = result.files[result.vertFile];
    const std::string fragSource = result.files[result.fragFile];
    visitSource(vertSource);
    visitSource(fragSource);

    // a fork with a manifest compiles with the varyings its @require asks for, as a library
    // shader does (the entry sources are already in the signature)
    ShaderManifest manifest;
    manifest.parse(fragSource, result.fragFile);
    manifest.parse(vertSource, result.vertFile);
    for (const std::string& define : manifest.getRequireDefines())
        result.defines.push_back(define);
    result.manifest = manifest.present;

    result.signature = signatureInput;
    result.valid = true;
    return result;
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
    const uint32_t properties = ShaderPool::getPropertiesFromKey(key);
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);

    shadercompiler::args_t args = shadercompiler::initialize_args();
    args.useBuffers = true;
    args.fileBuffers = getLibrarySources();
    applyShaderBackend(args, backend);

    std::string compileLog;
    args.error_output = &compileLog;

    // Built-in entry-point files + property #defines (the variant system is identical
    // for forked shaders)
    if (!setupShaderArgs(args, shaderType, properties))
        return fail("Unknown shader type");

    if (args.lang == shadercompiler::LANG_GLSL &&
            (shaderType == ShaderType::MESH || shaderType == ShaderType::DEPTH ||
             shaderType == ShaderType::GBUFFER))
        args.defines.push_back({"SKINNING_TEXTURE", "1"});

    if (customId != 0) {
        if (!customSources)
            return fail("Custom shader requested without its sources");
        if (!customSources->valid)
            return fail(customSources->error.empty() ? "Could not resolve custom shader sources" : customSources->error);

        // Overlay the fork's entry points and every project include they reach onto the
        // engine library. Resolution order is the entry point's own directory, the
        // project root, then the engine library.
        for (const auto& [path, content] : customSources->files)
            args.fileBuffers[path] = content;
        args.vert_file = customSources->vertFile;
        args.frag_file = customSources->fragFile;
        // enabled @feature / @keywords defines of the variant
        for (const std::string& define : customSources->defines)
            args.defines.push_back({define, "1"});
        // a pixel-lit (USE_PUNCTUAL) mesh variant of a manifest shader: its fragment stage
        // evaluates the lights at the world position and normal (includes/lights.glsl), so
        // surface_vs.glsl writes them whatever @require says
        if (customSources->manifest && shaderType == ShaderType::MESH && (properties & (1u << 3)) && !(properties & (1u << 0))){
            for (const char* define : {"SURFACE_POSITION", "SURFACE_NORMALS"}){
                if (std::find(customSources->defines.begin(), customSources->defines.end(), define) == customSources->defines.end())
                    args.defines.push_back({define, "1"});
            }
        }
    }

    if (stopped())
        return fail("Cancelled");
    report(0.3f);

    std::vector<shadercompiler::input_t> inputs;
    if (!shadercompiler::load_input(inputs, args))
        return fail("Error loading shader input");

    if (stopped())
        return fail("Cancelled");
    report(0.5f);

    std::vector<shadercompiler::spirv_t> spirvvec;
    spirvvec.resize(inputs.size());
    if (!shadercompiler::compile_to_spirv(spirvvec, inputs, args)) {
        if (log)
            *log = compileLog;
        return fail("Error compiling to SPIRV");
    }

    if (stopped())
        return fail("Cancelled");
    report(0.8f);

    std::vector<shadercompiler::spirvcross_t> spirvcrossvec;
    spirvcrossvec.resize(inputs.size());
    bool crossCompiled = false;
    try {
        crossCompiled = shadercompiler::compile_to_lang(spirvcrossvec, spirvvec, inputs, args);
    } catch (const std::exception& e) {
        compileLog += std::string("SPIRV-Cross: ") + e.what() + "\n";
    }
    if (!crossCompiled) {
        if (log)
            *log = compileLog;
        return fail("Error cross-compiling");
    }

    if (stopped())
        return fail("Cancelled");
    report(0.9f);

    // A deterministic basename names the stages
    args.output_basename = ShaderPool::getShaderStr(shaderType, properties, customId) +
                           getLangSuffix(args.lang, args.version, args.es, args.platform);
    out = convertToShaderData(spirvcrossvec, args);

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
