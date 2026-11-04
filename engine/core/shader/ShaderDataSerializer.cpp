// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderDataSerializer.h"
#include "io/AtomicFile.h"
#include "io/FileData.h"
#include "System.h"

#include "io/Data.h"
#include "io/File.h"
#include "json.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

namespace jasperengine {
namespace {

constexpr uint32_t SDAT_MAGIC = 0x54414453; // 'S''D''A''T' little-endian
// Serialization format version of the .sdat blob. Bump only when the on-disk
// layout changes; shader source changes are covered by the library hash.
//   1: the key as one packed u64 (customId << 48 | type << 32 | 32 property bits)
//   2: the key as u64 properties, u32 type, u32 customId (64 property bits)
constexpr uint32_t SDAT_VERSION = 2;

// Sanity limits to guard against malformed files
constexpr uint32_t kMaxString = 32u * 1024u * 1024u; // 32MB
constexpr uint32_t kMaxStages = 16;
constexpr uint32_t kMaxAttributes = 64;
constexpr uint32_t kMaxUniformBlocks = 32;
constexpr uint32_t kMaxUniforms = 128;
constexpr uint32_t kMaxStorageBuffers = 32;
constexpr uint32_t kMaxTextures = 64;
constexpr uint32_t kMaxSamplers = 64;
constexpr uint32_t kMaxTextureSamplerPairs = 64;

bool setErr(std::string* err, const std::string& msg) {
    if (err) {
        *err = msg;
    }
    return false;
}

// Little-endian write helpers (portable across architectures)
void writeU32(std::vector<unsigned char>& out, uint32_t v) {
    uint8_t buf[4] = {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24)
    };
    out.insert(out.end(), buf, buf + 4);
}

void writeU64(std::vector<unsigned char>& out, uint64_t v) {
    uint8_t buf[8] = {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24),
        static_cast<uint8_t>(v >> 32),
        static_cast<uint8_t>(v >> 40),
        static_cast<uint8_t>(v >> 48),
        static_cast<uint8_t>(v >> 56)
    };
    out.insert(out.end(), buf, buf + 8);
}

void writeI32(std::vector<unsigned char>& out, int32_t v) {
    writeU32(out, static_cast<uint32_t>(v));
}

void writeU8(std::vector<unsigned char>& out, uint8_t v) {
    out.push_back(v);
}

void writeBool(std::vector<unsigned char>& out, bool v) {
    uint8_t b = v ? 1 : 0;
    writeU8(out, b);
}

void writeString(std::vector<unsigned char>& out, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    writeU32(out, len);
    if (len > 0) {
        out.insert(out.end(), s.data(), s.data() + len);
    }
}

void writeShaderKey(std::vector<unsigned char>& out, ShaderKey shaderKey) {
    writeU64(out, shaderKey.properties);
    writeU32(out, shaderKey.type);
    writeU32(out, shaderKey.customId);
}

void writeShaderData(std::vector<unsigned char>& out, ShaderKey shaderKey, const ShaderData& shaderData) {
    writeU32(out, SDAT_MAGIC);
    writeU32(out, SDAT_VERSION);
    writeShaderKey(out, shaderKey);

    writeU32(out, static_cast<uint32_t>(shaderData.lang));
    writeU32(out, static_cast<uint32_t>(shaderData.version));
    writeBool(out, shaderData.es);

    writeU32(out, static_cast<uint32_t>(shaderData.stages.size()));

    for (const auto& stage : shaderData.stages) {
        writeU32(out, static_cast<uint32_t>(stage.type));
        writeString(out, stage.name);
        writeString(out, stage.source);

        // Bytecode: SPIR-V (Vulkan), and DXBC for D3D11 exports built on Windows (the source is
        // then empty); the other languages write 0
        if (stage.bytecode.data && stage.bytecode.size > 0) {
            writeU32(out, stage.bytecode.size);
            out.insert(out.end(), stage.bytecode.data, stage.bytecode.data + stage.bytecode.size);
        } else {
            writeU32(out, 0);
        }

        writeU32(out, static_cast<uint32_t>(stage.attributes.size()));
        for (const auto& a : stage.attributes) {
            writeString(out, a.name);
            writeString(out, a.semanticName);
            writeU32(out, static_cast<uint32_t>(a.semanticIndex));
            writeI32(out, static_cast<int32_t>(a.location));
            writeU32(out, static_cast<uint32_t>(a.type));
        }

        writeU32(out, static_cast<uint32_t>(stage.uniformblocks.size()));
        for (const auto& ub : stage.uniformblocks) {
            writeString(out, ub.name);
            writeString(out, ub.instName);
            writeI32(out, static_cast<int32_t>(ub.set));
            writeI32(out, static_cast<int32_t>(ub.binding));
            writeU32(out, static_cast<uint32_t>(ub.slot));
            writeU32(out, static_cast<uint32_t>(ub.sizeBytes));
            writeBool(out, ub.flattened);

            writeU32(out, static_cast<uint32_t>(ub.uniforms.size()));
            for (const auto& u : ub.uniforms) {
                writeString(out, u.name);
                writeU32(out, static_cast<uint32_t>(u.type));
                writeU32(out, static_cast<uint32_t>(u.arrayCount));
                writeU32(out, static_cast<uint32_t>(u.offset));
            }
        }

        writeU32(out, static_cast<uint32_t>(stage.storagebuffers.size()));
        for (const auto& sb : stage.storagebuffers) {
            writeString(out, sb.name);
            writeString(out, sb.instName);
            writeI32(out, static_cast<int32_t>(sb.set));
            writeI32(out, static_cast<int32_t>(sb.binding));
            writeU32(out, static_cast<uint32_t>(sb.slot));
            writeU32(out, static_cast<uint32_t>(sb.sizeBytes));
            writeBool(out, sb.readonly);
            writeU32(out, static_cast<uint32_t>(sb.type));
        }

        writeU32(out, static_cast<uint32_t>(stage.textures.size()));
        for (const auto& t : stage.textures) {
            writeString(out, t.name);
            writeI32(out, static_cast<int32_t>(t.set));
            writeI32(out, static_cast<int32_t>(t.binding));
            writeU32(out, static_cast<uint32_t>(t.slot));
            writeU32(out, static_cast<uint32_t>(t.type));
            writeU32(out, static_cast<uint32_t>(t.samplerType));
        }

        writeU32(out, static_cast<uint32_t>(stage.samplers.size()));
        for (const auto& s : stage.samplers) {
            writeString(out, s.name);
            writeI32(out, static_cast<int32_t>(s.set));
            writeI32(out, static_cast<int32_t>(s.binding));
            writeU32(out, static_cast<uint32_t>(s.slot));
            writeU32(out, static_cast<uint32_t>(s.type));
        }

        writeU32(out, static_cast<uint32_t>(stage.textureSamplerPairs.size()));
        for (const auto& p : stage.textureSamplerPairs) {
            writeString(out, p.name);
            writeString(out, p.textureName);
            writeString(out, p.samplerName);
            writeU32(out, static_cast<uint32_t>(p.slot));
        }
    }
}

const char* shaderLangName(ShaderLang lang) {
    switch (lang) {
        case ShaderLang::GLSL:  return "GLSL";
        case ShaderLang::MSL:   return "MSL";
        case ShaderLang::HLSL:  return "HLSL";
        case ShaderLang::SPIRV: return "SPIRV";
        default:                       return "Unknown";
    }
}

const char* shaderStageName(ShaderStageType type) {
    switch (type) {
        case ShaderStageType::VERTEX:   return "vertex";
        case ShaderStageType::FRAGMENT: return "fragment";
        default:                                return "unknown";
    }
}

nlohmann::json shaderDataToJson(ShaderKey shaderKey, const ShaderData& shaderData) {
    nlohmann::json root;
    root["magic"] = "SDAT";
    root["version"] = SDAT_VERSION;
    root["shaderKey"] = {
        {"type", shaderKey.type},
        {"properties", shaderKey.properties},
        {"customId", shaderKey.customId}
    };
    root["lang"] = shaderLangName(shaderData.lang);
    root["langValue"] = static_cast<uint32_t>(shaderData.lang);
    root["shaderVersion"] = shaderData.version;
    root["es"] = shaderData.es;
    root["stages"] = nlohmann::json::array();

    for (const auto& stage : shaderData.stages) {
        nlohmann::json stageJson;
        stageJson["type"] = shaderStageName(stage.type);
        stageJson["typeValue"] = static_cast<uint32_t>(stage.type);
        stageJson["name"] = stage.name;
        stageJson["source"] = stage.source;
        stageJson["bytecodeSize"] = stage.bytecode.data ? stage.bytecode.size : 0;

        stageJson["attributes"] = nlohmann::json::array();
        for (const auto& attr : stage.attributes) {
            stageJson["attributes"].push_back({
                {"name", attr.name},
                {"semanticName", attr.semanticName},
                {"semanticIndex", attr.semanticIndex},
                {"location", attr.location},
                {"type", static_cast<uint32_t>(attr.type)}
            });
        }

        stageJson["uniformBlocks"] = nlohmann::json::array();
        for (const auto& block : stage.uniformblocks) {
            nlohmann::json blockJson = {
                {"name", block.name},
                {"instanceName", block.instName},
                {"set", block.set},
                {"binding", block.binding},
                {"slot", block.slot},
                {"sizeBytes", block.sizeBytes},
                {"flattened", block.flattened},
                {"uniforms", nlohmann::json::array()}
            };
            for (const auto& uniform : block.uniforms) {
                blockJson["uniforms"].push_back({
                    {"name", uniform.name},
                    {"type", static_cast<uint32_t>(uniform.type)},
                    {"arrayCount", uniform.arrayCount},
                    {"offset", uniform.offset}
                });
            }
            stageJson["uniformBlocks"].push_back(blockJson);
        }

        stageJson["storageBuffers"] = nlohmann::json::array();
        for (const auto& buffer : stage.storagebuffers) {
            stageJson["storageBuffers"].push_back({
                {"name", buffer.name},
                {"instanceName", buffer.instName},
                {"set", buffer.set},
                {"binding", buffer.binding},
                {"slot", buffer.slot},
                {"sizeBytes", buffer.sizeBytes},
                {"readonly", buffer.readonly},
                {"type", static_cast<uint32_t>(buffer.type)}
            });
        }

        stageJson["textures"] = nlohmann::json::array();
        for (const auto& texture : stage.textures) {
            stageJson["textures"].push_back({
                {"name", texture.name},
                {"set", texture.set},
                {"binding", texture.binding},
                {"slot", texture.slot},
                {"type", static_cast<uint32_t>(texture.type)},
                {"samplerType", static_cast<uint32_t>(texture.samplerType)}
            });
        }

        stageJson["samplers"] = nlohmann::json::array();
        for (const auto& sampler : stage.samplers) {
            stageJson["samplers"].push_back({
                {"name", sampler.name},
                {"set", sampler.set},
                {"binding", sampler.binding},
                {"slot", sampler.slot},
                {"type", static_cast<uint32_t>(sampler.type)}
            });
        }

        stageJson["textureSamplerPairs"] = nlohmann::json::array();
        for (const auto& pair : stage.textureSamplerPairs) {
            stageJson["textureSamplerPairs"].push_back({
                {"name", pair.name},
                {"textureName", pair.textureName},
                {"samplerName", pair.samplerName},
                {"slot", pair.slot}
            });
        }

        root["stages"].push_back(stageJson);
    }

    return root;
}

// Little-endian read helpers (portable across architectures)
bool readU32(FileData& in, uint32_t& v) {
    uint8_t buf[4];
    if (in.read(buf, 4) != 4) return false;
    v = static_cast<uint32_t>(buf[0])
      | (static_cast<uint32_t>(buf[1]) << 8)
      | (static_cast<uint32_t>(buf[2]) << 16)
      | (static_cast<uint32_t>(buf[3]) << 24);
    return true;
}

bool readU64(FileData& in, uint64_t& v) {
    uint8_t buf[8];
    if (in.read(buf, 8) != 8) return false;
    v = static_cast<uint64_t>(buf[0])
      | (static_cast<uint64_t>(buf[1]) << 8)
      | (static_cast<uint64_t>(buf[2]) << 16)
      | (static_cast<uint64_t>(buf[3]) << 24)
      | (static_cast<uint64_t>(buf[4]) << 32)
      | (static_cast<uint64_t>(buf[5]) << 40)
      | (static_cast<uint64_t>(buf[6]) << 48)
      | (static_cast<uint64_t>(buf[7]) << 56);
    return true;
}

bool readI32(FileData& in, int32_t& v) {
    uint32_t u = 0;
    if (!readU32(in, u)) return false;
    v = static_cast<int32_t>(u);
    return true;
}

bool readU8(FileData& in, uint8_t& v) {
    return in.read(&v, sizeof(v)) == sizeof(v);
}

bool readBool(FileData& in, bool& v) {
    uint8_t b = 0;
    if (!readU8(in, b)) {
        return false;
    }
    v = (b != 0);
    return true;
}

bool readString(FileData& in, std::string& s) {
    uint32_t len = 0;
    if (!readU32(in, len)) {
        return false;
    }

    if (len > kMaxString) {
        return false;
    }

    s.clear();
    if (len == 0) {
        return true;
    }

    s.resize(len);
    return in.read((unsigned char*)&s[0], len) == len;
}

bool readShaderKey(FileData& in, ShaderKey& key) {
    uint64_t properties = 0;
    uint32_t type = 0;
    uint32_t customId = 0;
    if (!readU64(in, properties) || !readU32(in, type) || !readU32(in, customId) || type > 0xFFFF || customId > 0xFFFF) {
        return false;
    }
    key = ShaderKey(static_cast<uint16_t>(type), properties, static_cast<uint16_t>(customId));
    return true;
}

bool readShaderData(FileData& in, const std::string& filepath, ShaderKey expectedShaderKey, ShaderData& outShaderData, std::string* err) {
    uint32_t magic = 0;
    uint32_t version = 0;
    if (!readU32(in, magic) || !readU32(in, version)) {
        return setErr(err, "Failed to read header: " + filepath);
    }

    if (magic != SDAT_MAGIC) {
        return setErr(err, "Invalid SDAT magic: " + filepath);
    }

    if (version != SDAT_VERSION) {
        return setErr(err, "Unsupported SDAT version " + std::to_string(version) + " (expected " + std::to_string(SDAT_VERSION) + "): " + filepath);
    }

    ShaderKey fileShaderKey;
    if (!readShaderKey(in, fileShaderKey)) {
        return setErr(err, "Failed to read shader key: " + filepath);
    }

    if (fileShaderKey != expectedShaderKey) {
        return setErr(err, "Shader key mismatch: " + filepath);
    }

    uint32_t lang = 0;
    uint32_t shaderVersion = 0;
    bool es = false;

    if (!readU32(in, lang) || !readU32(in, shaderVersion) || !readBool(in, es)) {
        return setErr(err, "Failed to read ShaderData header: " + filepath);
    }

    ShaderData tmp;
    tmp.lang = static_cast<ShaderLang>(lang);
    tmp.version = shaderVersion;
    tmp.es = es;

    uint32_t stageCount = 0;
    if (!readU32(in, stageCount)) {
        return setErr(err, "Failed to read stage count: " + filepath);
    }

    if (stageCount > kMaxStages) {
        return setErr(err, "Too many stages in file: " + filepath);
    }

    tmp.stages.clear();
    tmp.stages.reserve(stageCount);

    for (uint32_t i = 0; i < stageCount; i++) {
        ShaderStage stage;

        uint32_t stageType = 0;
        if (!readU32(in, stageType)) {
            return setErr(err, "Failed to read stage type: " + filepath);
        }
        stage.type = static_cast<ShaderStageType>(stageType);

        if (!readString(in, stage.name) || !readString(in, stage.source)) {
            return setErr(err, "Failed to read stage strings: " + filepath);
        }

        // Bytecode is only present for SPIRV (Vulkan)
        uint32_t bytecodeSize = 0;
        if (!readU32(in, bytecodeSize)) {
            return setErr(err, "Failed to read bytecode size: " + filepath);
        }
        if (bytecodeSize > kMaxString) {
            return setErr(err, "Bytecode too large: " + filepath);
        }
        if (bytecodeSize > 0) {
            stage.bytecode.data = new unsigned char[bytecodeSize];
            stage.bytecode.size = bytecodeSize;
            if (in.read(stage.bytecode.data, bytecodeSize) != bytecodeSize) {
                delete[] stage.bytecode.data;
                stage.bytecode.data = nullptr;
                stage.bytecode.size = 0;
                return setErr(err, "Failed to read bytecode: " + filepath);
            }
        } else {
            stage.bytecode.data = nullptr;
            stage.bytecode.size = 0;
        }

        uint32_t attrCount = 0;
        if (!readU32(in, attrCount)) {
            return setErr(err, "Failed to read attribute count: " + filepath);
        }
        if (attrCount > kMaxAttributes) {
            return setErr(err, "Too many attributes in stage: " + filepath);
        }
        stage.attributes.reserve(attrCount);
        for (uint32_t a = 0; a < attrCount; a++) {
            ShaderAttr attr;
            uint32_t semanticIndex = 0;
            int32_t location = 0;
            uint32_t attrType = 0;

            if (!readString(in, attr.name) || !readString(in, attr.semanticName) || !readU32(in, semanticIndex) || !readI32(in, location) || !readU32(in, attrType)) {
                return setErr(err, "Failed to read attribute: " + filepath);
            }

            attr.semanticIndex = semanticIndex;
            attr.location = location;
            attr.type = static_cast<ShaderVertexType>(attrType);

            stage.attributes.push_back(attr);
        }

        uint32_t ubCount = 0;
        if (!readU32(in, ubCount)) {
            return setErr(err, "Failed to read uniform block count: " + filepath);
        }
        if (ubCount > kMaxUniformBlocks) {
            return setErr(err, "Too many uniform blocks in stage: " + filepath);
        }
        stage.uniformblocks.reserve(ubCount);
        for (uint32_t ub = 0; ub < ubCount; ub++) {
            ShaderUniformBlock block;
            int32_t set = 0;
            int32_t binding = 0;
            uint32_t slot = 0;
            uint32_t sizeBytes = 0;
            bool flattened = false;

            if (!readString(in, block.name) || !readString(in, block.instName) || !readI32(in, set) || !readI32(in, binding) || !readU32(in, slot) || !readU32(in, sizeBytes) || !readBool(in, flattened)) {
                return setErr(err, "Failed to read uniform block: " + filepath);
            }

            block.set = set;
            block.binding = binding;
            block.slot = slot;
            block.sizeBytes = sizeBytes;
            block.flattened = flattened;

            uint32_t uniformCount = 0;
            if (!readU32(in, uniformCount)) {
                return setErr(err, "Failed to read uniform count: " + filepath);
            }
            if (uniformCount > kMaxUniforms) {
                return setErr(err, "Too many uniforms in block: " + filepath);
            }

            block.uniforms.reserve(uniformCount);
            for (uint32_t u = 0; u < uniformCount; u++) {
                ShaderUniform uniform;
                uint32_t uniformType = 0;
                uint32_t arrayCount = 0;
                uint32_t offset = 0;
                if (!readString(in, uniform.name) || !readU32(in, uniformType) || !readU32(in, arrayCount) || !readU32(in, offset)) {
                    return setErr(err, "Failed to read uniform: " + filepath);
                }

                uniform.type = static_cast<ShaderUniformType>(uniformType);
                uniform.arrayCount = arrayCount;
                uniform.offset = offset;

                block.uniforms.push_back(uniform);
            }

            stage.uniformblocks.push_back(block);
        }

        uint32_t sbCount = 0;
        if (!readU32(in, sbCount)) {
            return setErr(err, "Failed to read storage buffer count: " + filepath);
        }
        if (sbCount > kMaxStorageBuffers) {
            return setErr(err, "Too many storage buffers in stage: " + filepath);
        }
        stage.storagebuffers.reserve(sbCount);
        for (uint32_t sb = 0; sb < sbCount; sb++) {
            ShaderStorageBuffer storage;
            int32_t set = 0;
            int32_t binding = 0;
            uint32_t slot = 0;
            uint32_t sizeBytes = 0;
            bool readonly = false;
            uint32_t sbType = 0;

            if (!readString(in, storage.name) || !readString(in, storage.instName) || !readI32(in, set) || !readI32(in, binding) || !readU32(in, slot) || !readU32(in, sizeBytes) || !readBool(in, readonly) || !readU32(in, sbType)) {
                return setErr(err, "Failed to read storage buffer: " + filepath);
            }

            storage.set = set;
            storage.binding = binding;
            storage.slot = slot;
            storage.sizeBytes = sizeBytes;
            storage.readonly = readonly;
            storage.type = static_cast<ShaderStorageBufferType>(sbType);

            stage.storagebuffers.push_back(storage);
        }

        uint32_t texCount = 0;
        if (!readU32(in, texCount)) {
            return setErr(err, "Failed to read texture count: " + filepath);
        }
        if (texCount > kMaxTextures) {
            return setErr(err, "Too many textures in stage: " + filepath);
        }
        stage.textures.reserve(texCount);
        for (uint32_t t = 0; t < texCount; t++) {
            ShaderTexture tex;
            int32_t set = 0;
            int32_t binding = 0;
            uint32_t slot = 0;
            uint32_t type = 0;
            uint32_t samplerType = 0;

            if (!readString(in, tex.name) || !readI32(in, set) || !readI32(in, binding) || !readU32(in, slot) || !readU32(in, type) || !readU32(in, samplerType)) {
                return setErr(err, "Failed to read texture: " + filepath);
            }

            tex.set = set;
            tex.binding = binding;
            tex.slot = slot;
            tex.type = static_cast<TextureType>(type);
            tex.samplerType = static_cast<TextureSamplerType>(samplerType);

            stage.textures.push_back(tex);
        }

        uint32_t samplerCount = 0;
        if (!readU32(in, samplerCount)) {
            return setErr(err, "Failed to read sampler count: " + filepath);
        }
        if (samplerCount > kMaxSamplers) {
            return setErr(err, "Too many samplers in stage: " + filepath);
        }
        stage.samplers.reserve(samplerCount);
        for (uint32_t s = 0; s < samplerCount; s++) {
            ShaderSampler sampler;
            int32_t set = 0;
            int32_t binding = 0;
            uint32_t slot = 0;
            uint32_t type = 0;

            if (!readString(in, sampler.name) || !readI32(in, set) || !readI32(in, binding) || !readU32(in, slot) || !readU32(in, type)) {
                return setErr(err, "Failed to read sampler: " + filepath);
            }

            sampler.set = set;
            sampler.binding = binding;
            sampler.slot = slot;
            sampler.type = static_cast<SamplerType>(type);

            stage.samplers.push_back(sampler);
        }

        uint32_t pairCount = 0;
        if (!readU32(in, pairCount)) {
            return setErr(err, "Failed to read texture sampler pair count: " + filepath);
        }
        if (pairCount > kMaxTextureSamplerPairs) {
            return setErr(err, "Too many texture sampler pairs in stage: " + filepath);
        }
        stage.textureSamplerPairs.reserve(pairCount);
        for (uint32_t p = 0; p < pairCount; p++) {
            ShaderTextureSamplerPair pair;
            uint32_t slot = 0;

            if (!readString(in, pair.name) || !readString(in, pair.textureName) || !readString(in, pair.samplerName) || !readU32(in, slot)) {
                return setErr(err, "Failed to read texture sampler pair: " + filepath);
            }

            pair.slot = slot;
            stage.textureSamplerPairs.push_back(pair);
        }

        tmp.stages.push_back(stage);
    }

    outShaderData = tmp;
    return true;
}

} // namespace

bool ShaderDataSerializer::writeToFile(const std::string& filepath, ShaderKey shaderKey, const ShaderData& shaderData, std::string* err) {
    std::vector<unsigned char> bytes;
    if (!writeToBytes(bytes, shaderKey, shaderData, err)) {
        return false;
    }

    // atomic: a run reading the cache, or a crash mid-write, never sees half a file
    const std::string systemPath = FileData::getSystemPath(filepath);
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult result = io::writeFileAtomic(systemPath,
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
    if (!result.ok()) {
        return setErr(err, "Cannot write shader file " + filepath + ": " + io::describeWriteError(result));
    }
    System::instance().syncFileSystem();

    return true;
}

bool ShaderDataSerializer::writeToBytes(std::vector<unsigned char>& outBytes, ShaderKey shaderKey, const ShaderData& shaderData, std::string* err) {
    (void)err;
    outBytes.clear();
    writeShaderData(outBytes, shaderKey, shaderData);
    return true;
}

bool ShaderDataSerializer::writeJsonToFile(const std::string& filepath, ShaderKey shaderKey, const ShaderData& shaderData, std::string* err) {
    const std::string json = shaderDataToJson(shaderKey, shaderData).dump(4) + "\n";

    File out;
    errno = 0;
    if (out.open(filepath.c_str(), true) != FileErrors::FILEDATA_OK) {
        std::string detail = errno != 0 ? std::string(" (") + std::strerror(errno) + ")" : "";
        return setErr(err, "Cannot open file for writing: " + filepath + detail);
    }

    if (json.size() > std::numeric_limits<unsigned int>::max()) {
        return setErr(err, "JSON shader data is too large to write: " + filepath);
    }

    if (!json.empty() && out.write((unsigned char*)json.data(), static_cast<unsigned int>(json.size())) != json.size()) {
        return setErr(err, "Failed to write JSON file: " + filepath);
    }

    out.flush();

    return true;
}

bool ShaderDataSerializer::readFromFile(const std::string& filepath, ShaderKey expectedShaderKey, ShaderData& outShaderData, std::string* err) {
    File in;
    if (in.open(filepath.c_str()) != FileErrors::FILEDATA_OK) {
        return setErr(err, "Cannot open file for reading: " + filepath);
    }

    return readShaderData(in, filepath, expectedShaderKey, outShaderData, err);
}

bool ShaderDataSerializer::readFromBytes(const std::vector<unsigned char>& bytes, ShaderKey expectedShaderKey, ShaderData& outShaderData, std::string* err) {
    if (bytes.empty()) {
        return setErr(err, "Cannot read empty SDAT data");
    }

    Data in;
    if (in.open(const_cast<unsigned char*>(bytes.data()), static_cast<unsigned int>(bytes.size()), false, false) != FileErrors::FILEDATA_OK) {
        return setErr(err, "Cannot open SDAT memory buffer");
    }

    return readShaderData(in, "<memory>", expectedShaderKey, outShaderData, err);
}

uint32_t ShaderDataSerializer::getFormatVersion() {
    return SDAT_VERSION;
}

} // namespace jasperengine
