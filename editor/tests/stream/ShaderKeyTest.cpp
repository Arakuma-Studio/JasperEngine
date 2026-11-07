// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shader variant keys with 64 property bits: the key itself (pack, unpack, order, text),
// normalization, the scene/project encoding (Stream), the .sdat format (ShaderDataSerializer)
// and the shader names (getShaderStr) every existing variant keeps.

#include <gtest/gtest.h>

#include "Stream.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderDataSerializer.h"

#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace jasperengine;
using editor::Stream;

namespace jasperengine {
    // failures print the key as the tools do
    void PrintTo(const ShaderKey& key, std::ostream* os) {
        *os << ShaderPool::getShaderKeyText(key);
    }
}

namespace {

    // the packed uint64 layout keys had before 64 property bits
    uint64_t oldPackedKey(ShaderType type, uint32_t properties, uint16_t customId) {
        return ((uint64_t)customId << 48) | (((uint64_t)type & 0xFFFF) << 32) | properties;
    }

    std::string oldKeyText(uint64_t packed) {
        std::ostringstream text;
        text << "0x" << std::hex << packed;
        return text.str();
    }

    std::string emit(const YAML::Node& node) {
        YAML::Emitter out;
        out << node;
        return out.c_str();
    }

    const ShaderType allTypes[] = {
        ShaderType::POINTS, ShaderType::LINES, ShaderType::MESH, ShaderType::SKYBOX, ShaderType::DEPTH,
        ShaderType::GBUFFER, ShaderType::UI, ShaderType::SSAO, ShaderType::SSAO_BLUR, ShaderType::SSR,
        ShaderType::SSR_BLUR, ShaderType::COMPOSITE, ShaderType::SHADOW2D, ShaderType::BLIT,
        ShaderType::POSTPROCESS, ShaderType::BLOOM, ShaderType::HTMLUI, ShaderType::HTMLUI_GRADIENT,
        ShaderType::HTMLUI_POST, ShaderType::UI_EFFECT, ShaderType::PROBE_FILTER, ShaderType::FLUID,
        ShaderType::SHADOW_CACHE, ShaderType::ATMOSPHERE_DEPTH, ShaderType::VOLUMETRIC_FOG, ShaderType::ATMOSPHERE_DENOISE,
        ShaderType::VOLUMETRIC_CLOUDS, ShaderType::SKY_COMPOSITE, ShaderType::VOLUMETRIC_COMPOSITE};

    // MESH has every one of its 32 bits (bit 5, the retired 'Pcf', is Stochastic Tiling since
    // October 2026, see StochasticTilingTest.cpp)
    // bits 0-31, bit 32, the lattice deformer ('Ltc'), bit 33, S10's high shadow filters ('Shq'),
    // and bits 34-35, the Vertex Animation ('Vat', 'Vab')
    const uint64_t meshValidMask = 0xFFFFFFFFFULL;
    // every MESH bit but 5: the valid mask before Stochastic Tiling
    const uint64_t meshMaskWithoutBit5 = 0xFFFFFFDFULL;

    ShaderData sampleShaderData() {
        ShaderData data;
        data.lang = ShaderLang::GLSL;
        data.version = 410;
        data.es = false;
        ShaderStage stage;
        stage.type = ShaderStageType::FRAGMENT;
        stage.name = "main";
        stage.source = "#version 410\nvoid main(){}\n";
        stage.bytecode.data = nullptr;
        stage.bytecode.size = 0;
        data.stages.push_back(stage);
        return data;
    }

    void writeU32(std::vector<unsigned char>& out, size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) out[at + i] = (unsigned char)(v >> (8 * i));
    }

}

// ---- (a) the key: pack, unpack, order, hash, text


TEST(ShaderKey, OrderIsTheOldPackedKeysOrder) {
    // sorted keys (std::set: export lists, the files written from them) keep their order
    std::vector<ShaderKey> keys;
    for (uint16_t customId : {0, 1, 2}) {
        for (ShaderType type : {ShaderType::POINTS, ShaderType::MESH, ShaderType::DEPTH, ShaderType::UI_EFFECT}) {
            for (uint32_t properties : {0u, 1u, 0x41u, 0x80000000u, 0xFFFFFFDFu}) {
                keys.push_back(ShaderPool::getShaderKey(type, properties, customId));
            }
        }
    }
    for (const ShaderKey& a : keys) {
        for (const ShaderKey& b : keys) {
            const uint64_t pa = oldPackedKey(ShaderPool::getShaderTypeFromKey(a), (uint32_t)a.properties, a.customId);
            const uint64_t pb = oldPackedKey(ShaderPool::getShaderTypeFromKey(b), (uint32_t)b.properties, b.customId);
            EXPECT_EQ(a < b, pa < pb);
            EXPECT_EQ(a == b, pa == pb);
        }
    }
    // the properties order last, high bits above the low ones
    EXPECT_LT(ShaderPool::getShaderKey(ShaderType::MESH, 0xFFFFFFFFULL), ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(32)));
    EXPECT_LT(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(63)), ShaderPool::getShaderKey(ShaderType::SKYBOX, 0));
    EXPECT_LT(ShaderPool::getShaderKey(ShaderType::UI_EFFECT, ~0ULL), ShaderPool::getShaderKey(ShaderType::POINTS, 0, 1));
}

TEST(ShaderKey, HashAndSetsTellHighBitsApart) {
    std::unordered_set<ShaderKey> hashed;
    std::set<ShaderKey> ordered;
    for (int bit = 0; bit < SHADER_PROPERTY_BITS; bit++) {
        hashed.insert(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(bit)));
        ordered.insert(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(bit)));
        hashed.insert(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(bit), 1));
        ordered.insert(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(bit), 1));
    }
    EXPECT_EQ(hashed.size(), 128u);
    EXPECT_EQ(ordered.size(), 128u);
    EXPECT_EQ(hashed.count(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(40), 1)), 1u);
    EXPECT_EQ(hashed.count(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(40), 2)), 0u);
}

TEST(ShaderKey, TextKeepsTheOldFormAndWidens) {
    // the keys tools printed (get_build_info "0x..."): the same text while the properties fit in 32 bits
    for (ShaderType type : allTypes) {
        for (uint32_t properties : {0u, 1u, 0x41u, 0x80000000u, 0xFFFFFFFFu}) {
            for (uint16_t customId : {0, 3, 0xFFFF}) {
                const ShaderKey key = ShaderPool::getShaderKey(type, properties, customId);
                const std::string text = ShaderPool::getShaderKeyText(key);
                EXPECT_EQ(text, oldKeyText(oldPackedKey(type, properties, customId)));
                ShaderKey parsed;
                ASSERT_TRUE(ShaderPool::parseShaderKeyText(text, parsed)) << text;
                EXPECT_EQ(parsed, key) << text;
            }
        }
    }
    EXPECT_EQ(ShaderPool::getShaderKeyText(ShaderPool::getShaderKey(ShaderType::MESH, 0)), "0x200000000");
    EXPECT_EQ(ShaderPool::getShaderKeyText(ShaderPool::getShaderKey(ShaderType::POINTS, 0)), "0x0");

    // wider: customId, type and properties in 4, 4 and 16 hex digits
    const ShaderKey wide = ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(40) | 0x41, 7);
    EXPECT_EQ(ShaderPool::getShaderKeyText(wide), "0x000700020000010000000041");
    ShaderKey parsed;
    ASSERT_TRUE(ShaderPool::parseShaderKeyText("0x000700020000010000000041", parsed));
    EXPECT_EQ(parsed, wide);
    ASSERT_TRUE(ShaderPool::parseShaderKeyText("0X0007000200000100000000AB", parsed));
    EXPECT_EQ(parsed, ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(40) | 0xAB, 7));
    const ShaderKey top = ShaderPool::getShaderKey(ShaderType::UI_EFFECT, ~0ULL, 0xFFFF);
    ASSERT_TRUE(ShaderPool::parseShaderKeyText(ShaderPool::getShaderKeyText(top), parsed));
    EXPECT_EQ(parsed, top);

    // decimal packed keys (JSON numbers as strings), and what is not a key
    ASSERT_TRUE(ShaderPool::parseShaderKeyText("8589934657", parsed));
    EXPECT_EQ(parsed, ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
    for (const char* bad : {"", "mesh:Ult", "0x", "0xg", "0x12345678901234567", "0x0007000200000100000000411",
                            "0x00070002000001000000004g", "-1", " 0x1", "0x1 ", "18446744073709551616"}) {
        EXPECT_FALSE(ShaderPool::parseShaderKeyText(bad, parsed)) << bad;
    }
}

TEST(ShaderKey, StorageKeyDropsOnlyTheCustomId) {
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(63) | shaderPropertyBit(33) | 0x41, 9);
    const ShaderKey storage = ShaderPool::getStorageKey(key);
    EXPECT_EQ(ShaderPool::getCustomIdFromKey(storage), 0);
    EXPECT_EQ(ShaderPool::getShaderTypeFromKey(storage), ShaderType::MESH);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(storage), shaderPropertyBit(63) | shaderPropertyBit(33) | 0x41);
    EXPECT_EQ(ShaderPool::getStorageKey(storage), storage);
}

TEST(ShaderKey, SpecsTakeSixtyFourBitMasks) {
    ShaderKey key;
    ASSERT_TRUE(ShaderPool::parseShaderSpec("mesh:0x100000041", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), 0x100000041ULL);
    ASSERT_TRUE(ShaderPool::parseShaderSpec("mesh:18446744073709551615", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), ~0ULL);
    std::string error;
    EXPECT_FALSE(ShaderPool::parseShaderSpec("mesh:18446744073709551616", key, &error));
    EXPECT_FALSE(ShaderPool::parseShaderSpec("mesh:0x10000000000000000", key, &error));
    ASSERT_TRUE(ShaderPool::parseShaderSpec("mesh:Ult,Pcl", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), shaderPropertyBit(0) | shaderPropertyBit(31));
}

// ---- (b) normalization

TEST(ShaderKeyNormalize, StripsUnnamedBitsKeepsTheRest) {
    // MESH: every bit past 35 (the Vertex Animation's) goes; bit 5 (Stochastic Tiling) stays: a stale
    // 'Pcf' of an old file is cleared where files are read (Stream), not here
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, ~0ULL)),
              ShaderPool::getShaderKey(ShaderType::MESH, meshValidMask));
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(5) | shaderPropertyBit(47) | 0x41)),
              ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(5) | 0x41));
    // the high shadow filters ('Shq', S10) only with shadows ('Shw', bit 4)
    const uint64_t shq = ShaderPool::getShadowsHQProperty();
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, shq | 0x41)),
              ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, shq | shaderPropertyBit(4) | 0x41)),
              ShaderPool::getShaderKey(ShaderType::MESH, shq | shaderPropertyBit(4) | 0x41));
}

TEST(ShaderKeyNormalize, EveryNamedBitRoundTripsThroughItsName) {
    // a bit added to getShaderPropertyName (past 31 too) is in names, specs and keys at once
    for (ShaderType type : allTypes) {
        if (type == ShaderType::UI_EFFECT)
            continue; // named by its effect, see EffectShaderTest
        const int count = ShaderPool::getShaderPropertyCount(type);
        for (int bit = 0; bit < count; bit++) {
            if (ShaderPool::getShaderPropertyName(type, bit, true) == "?")
                continue;
            const ShaderKey key = ShaderPool::getShaderKey(type, shaderPropertyBit(bit));
            const std::string name = ShaderPool::getShaderStr(type, shaderPropertyBit(bit));
            ShaderKey parsed;
            ASSERT_TRUE(ShaderPool::parseShaderStr(name, parsed)) << name;
            EXPECT_EQ(parsed, key) << name;
        }
        const uint64_t valid = ShaderPool::getValidPropertyMask(type);
        ShaderKey parsed;
        ASSERT_TRUE(ShaderPool::parseShaderStr(ShaderPool::getShaderStr(type, valid), parsed)) << ShaderPool::getShaderStr(type, valid);
        EXPECT_EQ(parsed, ShaderPool::getShaderKey(type, valid));
    }
}

// ---- (c) the scene and project encoding (Stream)

TEST(ShaderKeyStream, ExistingFilesSaveByteIdentical) {
    // what scenes (shaderKeys) and project settings (shaderAdditions/Exclusions) hold: 32-bit
    // properties, left out when 0, a fork by its path
    const std::string file =
        "- type: points\n"
        "  properties: 16\n"
        "- type: mesh\n"
        "- type: mesh\n"
        "  properties: 65\n"
        "- type: mesh\n"
        "  properties: 2147483713\n"
        "- type: mesh\n"
        "  properties: 4294967263\n"
        "- type: sky\n"
        "  properties: 1\n"
        "- type: depth\n"
        "  properties: 3\n"
        "- type: htmluipost\n"
        "  properties: 64\n"
        "- type: mesh\n"
        "  properties: 65\n"
        "  customShader: shaders/keyTestMesh";
    // ids come from the session's registry: the fork's is after the built-in ones
    const uint16_t forkId = ShaderPool::registerCustomShader("shaders/keyTestMesh");
    ASSERT_NE(forkId, 0);

    const std::vector<ShaderKey> decoded = Stream::decodeShaderKeys(YAML::Load(file));
    ASSERT_EQ(decoded.size(), 9u);
    EXPECT_EQ(decoded[2], ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
    EXPECT_EQ(decoded[3], ShaderPool::getShaderKey(ShaderType::MESH, 0x80000041ULL));
    EXPECT_EQ(decoded[4], ShaderPool::getShaderKey(ShaderType::MESH, meshMaskWithoutBit5));
    EXPECT_EQ(decoded[8], ShaderPool::getShaderKey(ShaderType::MESH, 0x41, forkId));

    const std::set<ShaderKey> keys(decoded.begin(), decoded.end());
    EXPECT_EQ(emit(Stream::encodeShaderKeys(keys)), file);

    // and the same text the 32-bit encoding made
    YAML::Node old(YAML::NodeType::Sequence);
    for (const ShaderKey& key : keys) {
        YAML::Node entry(YAML::NodeType::Map);
        entry["type"] = ShaderPool::getShaderTypeName(ShaderPool::getShaderTypeFromKey(key), true);
        if (key.properties) entry["properties"] = (uint32_t)key.properties;
        if (key.customId) entry["customShader"] = ShaderPool::getCustomShaderName(key.customId);
        old.push_back(entry);
    }
    EXPECT_EQ(emit(old), file);
}

TEST(ShaderKeyStream, LegacyPackedKeysDecode) {
    // scenes saved before forks were named by path: the packed key, its custom id dropped
    const uint64_t mesh41 = oldPackedKey(ShaderType::MESH, 0x41, 0);
    const uint64_t forkedDepth = oldPackedKey(ShaderType::DEPTH, 0x3, 12);
    const uint64_t retiredBit = oldPackedKey(ShaderType::MESH, 0x61, 0);  // bit 5 set
    const uint64_t postColor = oldPackedKey(ShaderType::MESH, 0x80000041u, 3);
    const std::string file = "- " + std::to_string(mesh41) + "\n- " + std::to_string(forkedDepth) + "\n- " +
                             std::to_string(retiredBit) + "\n- " + std::to_string(postColor) + "\n- type: ui\n  properties: 1";
    const std::vector<ShaderKey> decoded = Stream::decodeShaderKeys(YAML::Load(file));
    ASSERT_EQ(decoded.size(), 5u);
    EXPECT_EQ(decoded[0], ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
    EXPECT_EQ(decoded[1], ShaderPool::getShaderKey(ShaderType::DEPTH, 0x3));
    EXPECT_EQ(decoded[2], ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
    EXPECT_EQ(decoded[3], ShaderPool::getShaderKey(ShaderType::MESH, 0x80000041ULL));
    EXPECT_EQ(decoded[4], ShaderPool::getShaderKey(ShaderType::UI, 0x1));

    // a hex scalar too (yaml-cpp reads 0x as hex)
    ShaderKey key;
    ASSERT_TRUE(Stream::decodeShaderKey(YAML::Load("0x200000041"), key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::MESH, 0x41));
    // not a key
    EXPECT_FALSE(Stream::decodeShaderKey(YAML::Load("[1, 2]"), key));
    EXPECT_FALSE(Stream::decodeShaderKey(YAML::Load("{type: nope, properties: 1}"), key));
    EXPECT_TRUE(Stream::decodeShaderKeys(YAML::Load("- {type: nope}\n- [1]")).empty());
}

TEST(ShaderKeyStream, SixtyFourBitPropertiesRoundTrip) {
    // a key past bit 31 (none is named yet, so the list functions would normalize it away):
    // a plain YAML integer of up to 20 digits
    const uint16_t forkId = ShaderPool::registerCustomShader("shaders/keyTestWide");
    const uint64_t propertySets[] = {shaderPropertyBit(32), shaderPropertyBit(40) | 0x41, shaderPropertyBit(63), ~uint64_t(0), 0xFFFFFFFFULL};
    for (uint64_t properties : propertySets) {
        for (uint16_t customId : {(uint16_t)0, forkId}) {
            const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, properties, customId);
            const YAML::Node node = Stream::encodeShaderKey(key);
            ASSERT_TRUE(node.IsMap());
            EXPECT_EQ(node["properties"].Scalar(), std::to_string(properties));
            // through text, as a file
            ShaderKey decoded;
            ASSERT_TRUE(Stream::decodeShaderKey(YAML::Load(emit(node)), decoded)) << emit(node);
            EXPECT_EQ(decoded, key) << emit(node);
        }
    }
    EXPECT_EQ(emit(Stream::encodeShaderKey(ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(40) | 0x41))),
              "type: mesh\nproperties: 1099511627841");
    // a custom id without a name has nothing stable to write
    EXPECT_TRUE(Stream::encodeShaderKey(ShaderPool::getShaderKey(ShaderType::MESH, 1, 0xFFFE)).IsNull());
    std::set<ShaderKey> unnamed = {ShaderPool::getShaderKey(ShaderType::MESH, 1, 0xFFFE), ShaderPool::getShaderKey(ShaderType::MESH, 1)};
    EXPECT_EQ(emit(Stream::encodeShaderKeys(unnamed)), "- type: mesh\n  properties: 1");
}

// ---- (d) the .sdat format

TEST(ShaderKeySdat, StorageKeyRoundTrips) {
    EXPECT_EQ(ShaderDataSerializer::getFormatVersion(), 2u);
    const ShaderData data = sampleShaderData();
    const uint64_t propertySets[] = {0, 0x80000041ULL, shaderPropertyBit(32), shaderPropertyBit(63) | 0x41, ~uint64_t(0)};
    for (uint64_t properties : propertySets) {
        const ShaderKey storage = ShaderPool::getStorageKey(ShaderPool::getShaderKey(ShaderType::MESH, properties, 4));
        std::vector<unsigned char> bytes;
        ASSERT_TRUE(ShaderDataSerializer::writeToBytes(bytes, storage, data));

        ShaderData read;
        std::string error;
        ASSERT_TRUE(ShaderDataSerializer::readFromBytes(bytes, storage, read, &error)) << error;
        ASSERT_EQ(read.stages.size(), 1u);
        EXPECT_EQ(read.stages[0].source, data.stages[0].source);
        EXPECT_EQ(read.version, 410u);

        // a key differing in any part, a high bit included, is rejected
        for (const ShaderKey& other : {ShaderPool::getShaderKey(ShaderType::MESH, properties ^ shaderPropertyBit(40)),
                                       ShaderPool::getShaderKey(ShaderType::MESH, properties ^ 1),
                                       ShaderPool::getShaderKey(ShaderType::DEPTH, properties),
                                       ShaderPool::getShaderKey(ShaderType::MESH, properties, 4)}) {
            EXPECT_FALSE(ShaderDataSerializer::readFromBytes(bytes, other, read, &error));
            EXPECT_NE(error.find("Shader key mismatch"), std::string::npos) << error;
        }
    }
    // (files: writeToFile/readFromFile are these bytes through File, which needs the engine's
    // System; tools/mcp/tests/test_shader_keys.py reads the files an export writes)
}

TEST(ShaderKeySdat, TheOldFormatIsRejected) {
    const ShaderData data = sampleShaderData();
    const ShaderKey storage = ShaderPool::getShaderKey(ShaderType::MESH, 0x41);
    std::vector<unsigned char> bytes;
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(bytes, storage, data));
    ASSERT_GT(bytes.size(), 24u);

    // version 1: magic, version, the packed u64 key, then the same data
    std::vector<unsigned char> old(bytes.begin(), bytes.begin() + 8);
    writeU32(old, 4, 1);
    const uint64_t packed = oldPackedKey(ShaderType::MESH, 0x41, 0);
    for (int i = 0; i < 8; i++) old.push_back((unsigned char)(packed >> (8 * i)));
    old.insert(old.end(), bytes.begin() + 24, bytes.end());

    ShaderData read;
    std::string error;
    EXPECT_FALSE(ShaderDataSerializer::readFromBytes(old, storage, read, &error));
    EXPECT_NE(error.find("Unsupported SDAT version"), std::string::npos) << error;
    // nor read with the key it was written for in the old layout
    EXPECT_FALSE(ShaderDataSerializer::readFromBytes(old, ShaderPool::unpackLegacyKey(packed), read, &error));

    // a current file whose version says 1 is not misread either
    std::vector<unsigned char> relabelled = bytes;
    writeU32(relabelled, 4, 1);
    EXPECT_FALSE(ShaderDataSerializer::readFromBytes(relabelled, storage, read, &error));
    EXPECT_NE(error.find("Unsupported SDAT version"), std::string::npos) << error;

    // a file cut inside the key
    std::vector<unsigned char> cut(bytes.begin(), bytes.begin() + 14);
    EXPECT_FALSE(ShaderDataSerializer::readFromBytes(cut, storage, read, &error));
}

// ---- (e) the shader names of existing variants

TEST(ShaderKeyNames, ExistingVariantsKeepTheirNames) {
    struct Case { ShaderType type; uint64_t properties; const char* name; };
    const Case cases[] = {
        {ShaderType::MESH, 0, "mesh"},
        {ShaderType::MESH, 0x3, "mesh_UltUv1"},
        {ShaderType::MESH, 0x41, "mesh_UltNor"},
        {ShaderType::MESH, (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 12) | (1u << 19) | (1u << 31), "mesh_Uv1PucShwNorFogIblPcl"},
        {ShaderType::MESH, (1u << 3) | (1u << 28) | (1u << 29), "mesh_PucVtlLc4"},
        {ShaderType::MESH, (1u << 3) | (1u << 30), "mesh_PucL16"},
        {ShaderType::MESH, meshMaskWithoutBit5, "mesh_UltUv1Uv2PucShwNorNmpTanVc3Vc4TxrFogSkiMtaMnrMtgTerIstIblMirSaoL2dS2dAmsAopIfdTpbVtlLc4L16Pcl"},
        // bits past the count add nothing
        {ShaderType::MESH, 0x41 | shaderPropertyBit(36) | shaderPropertyBit(63), "mesh_UltNor"},
        // bit 33, S10's high shadow filters
        {ShaderType::MESH, 0x51 | shaderPropertyBit(33), "mesh_UltShwNorShq"},
        // bit 32, the first past the 32 older keys use, is the lattice deformer
        {ShaderType::MESH, 0x41 | shaderPropertyBit(32), "mesh_UltNorLtc"},
        // bits 34-35 (upstream's 33-34, after S10's 'Shq'): a Vertex Animator's Vertices and Bones modes
        {ShaderType::MESH, 0x41 | shaderPropertyBit(34), "mesh_UltNorVat"},
        {ShaderType::MESH, 0x41 | shaderPropertyBit(35), "mesh_UltNorVab"},
        {ShaderType::DEPTH, shaderPropertyBit(16), "depth_Vat"},
        {ShaderType::DEPTH, shaderPropertyBit(17), "depth_Vab"},
        {ShaderType::GBUFFER, shaderPropertyBit(14), "gbuffer_Vat"},
        {ShaderType::GBUFFER, shaderPropertyBit(15), "gbuffer_Vab"},
        // bit 5, a '?' while it was retired, is Stochastic Tiling
        {ShaderType::MESH, 0x41 | (1u << 5), "mesh_UltStoNor"},
        {ShaderType::MESH, meshValidMask, "mesh_UltUv1Uv2PucShwStoNorNmpTanVc3Vc4TxrFogSkiMtaMnrMtgTerIstIblMirSaoL2dS2dAmsAopIfdTpbVtlLc4L16PclLtcShqVatVab"},
        {ShaderType::DEPTH, 0x3, "depth_TexSki"},
        {ShaderType::DEPTH, 0x3FFF, "depth_TexSkiMtaMnrMtgTerIstAmsIfdNorVc3Vc4Uv1Txr"},
        {ShaderType::DEPTH, 0x7FFF, "depth_TexSkiMtaMnrMtgTerIstAmsIfdNorVc3Vc4Uv1TxrSto"},
        {ShaderType::GBUFFER, 0x3FF, "gbuffer_BctNorSkiMtaMnrMtgTerIstMrtTpb"},
        {ShaderType::GBUFFER, 0x7FF, "gbuffer_BctNorSkiMtaMnrMtgTerIstMrtTpbSto"},
        {ShaderType::UI, 0xF, "ui_TexFtxVc3Vc4"},
        {ShaderType::POINTS, 0x1F, "points_TexVc3Vc4TxrPcl"},
        {ShaderType::LINES, 0x6, "lines_Vc4Pcl"},
        {ShaderType::SKYBOX, 0, "sky"},
        {ShaderType::SKYBOX, 1, "sky_Pcl"},
        {ShaderType::SSAO, 0, "ssao"},
        {ShaderType::SHADOW2D, 0, "shadow2d"},
        {ShaderType::BLIT, 0, "blit"},
        {ShaderType::BLIT, 0x1FFF, "blit_FxaCabPixCrtEdgGrnShpLutVigDthBlmBlrScl"},
        {ShaderType::BLIT, 0x3E000, "blit_VhsHalFdmGlnGfm"},
        {ShaderType::BLOOM, 0x3, "bloom_PreThr"},
        {ShaderType::POSTPROCESS, 0, "postprocess"},
        {ShaderType::HTMLUI, 1, "htmlui_Tex"},
        {ShaderType::HTMLUI_GRADIENT, 0, "htmluigradient"},
        {ShaderType::HTMLUI_POST, 1u << 6, "htmluipost_Yuv"},
        {ShaderType::PROBE_FILTER, 0, "probefilter"},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(ShaderPool::getShaderStr(c.type, c.properties), c.name);
    }

    // a fork's basename, and a library UI effect's
    const uint16_t fork = ShaderPool::registerCustomShader("shaders/myMesh");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::MESH, 0x41, fork), "mesh_shaders_myMesh_UltNor");
    const uint16_t effect = ShaderPool::registerCustomShader("@effects/stripes");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::UI_EFFECT, 1, effect), "uieffect_fx_stripes_Flt");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::UI_EFFECT, 0, effect), "uieffect_fx_stripes");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::UI_EFFECT, 3, effect), "uieffect_fx_stripes_FltStp");
}

// T3 R20, D2: bits are names plus an order, numbers taken at landing. This pins the number 'Thl' got on
// each type; a stream landing before it moves them here and bumps the shader cache.
TEST(ShaderKey, TerrainHolesBitIsPinned) {
    EXPECT_EQ(ShaderPool::getTerrainHolesProperty(ShaderType::MESH), shaderPropertyBit(36));
    EXPECT_EQ(ShaderPool::getTerrainHolesProperty(ShaderType::DEPTH), shaderPropertyBit(18));
    EXPECT_EQ(ShaderPool::getTerrainHolesProperty(ShaderType::GBUFFER), shaderPropertyBit(16));
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 36, true), "Thl");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::DEPTH, 18, true), "Thl");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 16, true), "Thl");
    EXPECT_EQ(ShaderPool::getTerrainHolesProperty(ShaderType::UI), 0u);
    // no fallback may drop it: a dropped hole would draw over the hole (T3 R34)
    const uint64_t withFeatures = shaderPropertyBit(36) | ShaderPool::getShadowsHQProperty() | shaderPropertyBit(4);
    for (uint64_t candidate : ShaderPool::getFallbackProperties(ShaderType::MESH, withFeatures)) {
        EXPECT_NE(candidate & shaderPropertyBit(36), 0u);
    }
}

// T3 D2: Medium and Low are Lit defines on MESH, and on a G-buffer, which they do not reach, these bits
TEST(ShaderKey, TerrainTierBitsArePinned) {
    using terrain::TerrainQuality;
    EXPECT_EQ(ShaderPool::getTerrainTierProperty(TerrainQuality::MEDIUM), shaderPropertyBit(17));
    EXPECT_EQ(ShaderPool::getTerrainTierProperty(TerrainQuality::LOW), shaderPropertyBit(18));
    EXPECT_EQ(ShaderPool::getTerrainTierProperty(TerrainQuality::HIGH), 0u);
    EXPECT_EQ(ShaderPool::getTerrainExtendedProperty(), shaderPropertyBit(19));
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 17, true), "Tqm");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 18, true), "Tql");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::GBUFFER, 19, true), "Txl");
}
