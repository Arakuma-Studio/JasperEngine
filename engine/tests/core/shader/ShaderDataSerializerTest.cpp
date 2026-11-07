// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .sdat blob compiled shader variants are stored in (the runtime shader cache, exports).

#include "shader/ShaderDataSerializer.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // type 2 with property bits 0x41, built in (ShaderKey is a struct since the 64-bit keys)
    const ShaderKey key(2, 0x41, 0);

    ShaderData shaderDataSample(){
        ShaderData data;
        data.lang = ShaderLang::GLSL;
        data.version = 410;
        data.es = false;

        ShaderStage vertex;
        vertex.type = ShaderStageType::VERTEX;
        vertex.name = "main";
        vertex.source = "#version 410\nvoid main(){}\n";
        vertex.bytecode.data = nullptr;
        vertex.bytecode.size = 0;
        vertex.attributes.push_back({"a_position", "POSITION", 0, 0, ShaderVertexType::FLOAT3});
        vertex.attributes.push_back({"a_texcoord1", "TEXCOORD", 1, 1, ShaderVertexType::FLOAT2});
        ShaderUniformBlock block{"u_vs_params", "vs", 0, 1, 0, 64, true, {}};
        block.uniforms.push_back({"mvpMatrix", ShaderUniformType::FLOAT4, 4, 0});
        vertex.uniformblocks.push_back(block);
        data.stages.push_back(vertex);

        ShaderStage fragment;
        fragment.type = ShaderStageType::FRAGMENT;
        fragment.name = "main";
        fragment.source = "#version 410\nout vec4 frag;\nvoid main(){ frag = vec4(1); }\n";
        fragment.bytecode.size = 3;
        fragment.bytecode.data = new unsigned char[3]{1, 2, 3};   // owned by the ShaderData
        fragment.textures.push_back({"u_baseTexture", 0, 2, 0, TextureType::TEXTURE_2D, TextureSamplerType::FLOAT});
        fragment.samplers.push_back({"u_baseSampler", 0, 3, 0, SamplerType::FILTERING});
        fragment.textureSamplerPairs.push_back({"u_base", "u_baseTexture", "u_baseSampler", 0});
        data.stages.push_back(fragment);
        return data;
    }

    void expectSame(const ShaderData& a, const ShaderData& b){
        EXPECT_EQ(a.lang, b.lang);
        EXPECT_EQ(a.version, b.version);
        EXPECT_EQ(a.es, b.es);
        ASSERT_EQ(a.stages.size(), b.stages.size());
        for (size_t s = 0; s < a.stages.size(); s++){
            const ShaderStage& x = a.stages[s];
            const ShaderStage& y = b.stages[s];
            EXPECT_EQ(x.type, y.type);
            EXPECT_EQ(x.name, y.name);
            EXPECT_EQ(x.source, y.source);
            EXPECT_EQ(x.bytecode.size, y.bytecode.size);
            if (x.bytecode.size && x.bytecode.data && y.bytecode.data){
                EXPECT_EQ(std::memcmp(x.bytecode.data, y.bytecode.data, x.bytecode.size), 0);
            }
            ASSERT_EQ(x.attributes.size(), y.attributes.size());
            for (size_t i = 0; i < x.attributes.size(); i++){
                EXPECT_EQ(x.attributes[i].name, y.attributes[i].name);
                EXPECT_EQ(x.attributes[i].semanticName, y.attributes[i].semanticName);
                EXPECT_EQ(x.attributes[i].semanticIndex, y.attributes[i].semanticIndex);
                EXPECT_EQ(x.attributes[i].location, y.attributes[i].location);
                EXPECT_EQ(x.attributes[i].type, y.attributes[i].type);
            }
            ASSERT_EQ(x.uniformblocks.size(), y.uniformblocks.size());
            for (size_t i = 0; i < x.uniformblocks.size(); i++){
                EXPECT_EQ(x.uniformblocks[i].name, y.uniformblocks[i].name);
                EXPECT_EQ(x.uniformblocks[i].instName, y.uniformblocks[i].instName);
                EXPECT_EQ(x.uniformblocks[i].binding, y.uniformblocks[i].binding);
                EXPECT_EQ(x.uniformblocks[i].sizeBytes, y.uniformblocks[i].sizeBytes);
                EXPECT_EQ(x.uniformblocks[i].flattened, y.uniformblocks[i].flattened);
                ASSERT_EQ(x.uniformblocks[i].uniforms.size(), y.uniformblocks[i].uniforms.size());
                for (size_t u = 0; u < x.uniformblocks[i].uniforms.size(); u++){
                    EXPECT_EQ(x.uniformblocks[i].uniforms[u].name, y.uniformblocks[i].uniforms[u].name);
                    EXPECT_EQ(x.uniformblocks[i].uniforms[u].type, y.uniformblocks[i].uniforms[u].type);
                    EXPECT_EQ(x.uniformblocks[i].uniforms[u].arrayCount, y.uniformblocks[i].uniforms[u].arrayCount);
                }
            }
            ASSERT_EQ(x.textures.size(), y.textures.size());
            for (size_t i = 0; i < x.textures.size(); i++){
                EXPECT_EQ(x.textures[i].name, y.textures[i].name);
                EXPECT_EQ(x.textures[i].binding, y.textures[i].binding);
                EXPECT_EQ(x.textures[i].type, y.textures[i].type);
                EXPECT_EQ(x.textures[i].samplerType, y.textures[i].samplerType);
            }
            ASSERT_EQ(x.samplers.size(), y.samplers.size());
            for (size_t i = 0; i < x.samplers.size(); i++){
                EXPECT_EQ(x.samplers[i].name, y.samplers[i].name);
                EXPECT_EQ(x.samplers[i].type, y.samplers[i].type);
            }
            ASSERT_EQ(x.textureSamplerPairs.size(), y.textureSamplerPairs.size());
            for (size_t i = 0; i < x.textureSamplerPairs.size(); i++){
                EXPECT_EQ(x.textureSamplerPairs[i].textureName, y.textureSamplerPairs[i].textureName);
                EXPECT_EQ(x.textureSamplerPairs[i].samplerName, y.textureSamplerPairs[i].samplerName);
            }
        }
    }

}

TEST(ShaderDataSerializer, RoundTripsThroughBytes){
    const ShaderData original = shaderDataSample();
    std::vector<unsigned char> bytes;
    std::string error;
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(bytes, key, original, &error)) << error;
    EXPECT_FALSE(bytes.empty());
    ShaderData read;
    ASSERT_TRUE(ShaderDataSerializer::readFromBytes(bytes, key, read, &error)) << error;
    expectSame(original, read);
}

TEST(ShaderDataSerializer, RoundTripsThroughAFile){
    TempDir dir;
    const ShaderData original = shaderDataSample();
    const std::string path = dir.file("variant.sdat").string();
    std::string error;
    ASSERT_TRUE(ShaderDataSerializer::writeToFile(path, key, original, &error)) << error;
    ShaderData read;
    ASSERT_TRUE(ShaderDataSerializer::readFromFile(path, key, read, &error)) << error;
    expectSame(original, read);
}

TEST(ShaderDataSerializer, WritingIsDeterministic){
    std::vector<unsigned char> first, second;
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(first, key, shaderDataSample()));
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(second, key, shaderDataSample()));
    EXPECT_EQ(first, second);
}

TEST(ShaderDataSerializer, BlobStartsWithTheMagic){
    std::vector<unsigned char> bytes;
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(bytes, key, shaderDataSample()));
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "SDAT");
}

TEST(ShaderDataSerializer, RefusesAnotherShadersKey){
    std::vector<unsigned char> bytes;
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(bytes, key, shaderDataSample()));
    ShaderData read;
    std::string error;
    EXPECT_FALSE(ShaderDataSerializer::readFromBytes(bytes, ShaderKey(2, 0x42, 0), read, &error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderDataSerializer, RefusesDamagedData){
    std::vector<unsigned char> bytes;
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(bytes, key, shaderDataSample()));
    ShaderData read;
    std::string error;

    std::vector<unsigned char> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
    EXPECT_FALSE(ShaderDataSerializer::readFromBytes(truncated, key, read, &error));

    std::vector<unsigned char> badMagic = bytes;
    badMagic[0] ^= 0xFF;
    EXPECT_FALSE(ShaderDataSerializer::readFromBytes(badMagic, key, read, &error));

    EXPECT_FALSE(ShaderDataSerializer::readFromBytes({}, key, read, &error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderDataSerializer, MissingFileIsAnError){
    TempDir dir;
    ShaderData read;
    std::string error;
    EXPECT_FALSE(ShaderDataSerializer::readFromFile(dir.file("none.sdat").string(), key, read, &error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderDataSerializer, JsonDumpNamesTheStages){
    TempDir dir;
    const std::string path = dir.file("variant.json").string();
    ASSERT_TRUE(ShaderDataSerializer::writeJsonToFile(path, key, shaderDataSample()));
    const std::string json = dir.read("variant.json");
    EXPECT_NE(json.find("u_vs_params"), std::string::npos);
    EXPECT_NE(json.find("u_baseTexture"), std::string::npos);
}

// A D3D11 export built on Windows ships each HLSL stage as bytecode with no source
// (ShaderBuilder::compileHlslBytecode); a game reads it back as it was written.
TEST(ShaderDataSerializer, HlslBytecodeRoundTrips){
    ShaderData original = shaderDataSample();
    original.lang = ShaderLang::HLSL;
    original.version = 50;
    for (ShaderStage& stage : original.stages){
        stage.source.clear();
        delete[] stage.bytecode.data;
        stage.bytecode.size = 4;
        stage.bytecode.data = new unsigned char[4]{'D', 'X', 'B', 'C'};   // owned by the ShaderData
    }
    std::vector<unsigned char> bytes;
    std::string error;
    ASSERT_TRUE(ShaderDataSerializer::writeToBytes(bytes, key, original, &error)) << error;
    ShaderData read;
    ASSERT_TRUE(ShaderDataSerializer::readFromBytes(bytes, key, read, &error)) << error;
    expectSame(original, read);
    for (const ShaderStage& stage : read.stages){
        EXPECT_TRUE(stage.source.empty());
        ASSERT_EQ(stage.bytecode.size, 4u);
        EXPECT_EQ(std::memcmp(stage.bytecode.data, "DXBC", 4), 0);
    }
}
