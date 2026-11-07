// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC14 (R17), static half: what the pixel-snap rule adds to a sprite's vertex stage, counted
// in the SPIR-V of mesh.vert with and without 'Pxs' (glslang runs without an optimizer, so the
// rule's helpers stay functions and each one's arithmetic is counted on its own). The per-vertex
// path is pixelSnapClip: pixelSnapToPixels, pixelSnapRound2 and pixelSnapFromPixels. The variant
// without the bit must stay exactly the variant of before (R14).

#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"
#include "shader/ShaderVariantCompiler.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // SPIR-V opcodes (the unified specification, section 3.49)
    constexpr uint32_t OP_NAME = 5;
    constexpr uint32_t OP_EXT_INST = 12;
    constexpr uint32_t OP_FUNCTION = 54;
    constexpr uint32_t OP_FUNCTION_END = 56;
    constexpr uint32_t OP_FNEGATE = 127;
    constexpr uint32_t OP_FADD = 129;
    constexpr uint32_t OP_FSUB = 131;
    constexpr uint32_t OP_FMUL = 133;
    constexpr uint32_t OP_FDIV = 136;
    constexpr uint32_t OP_VECTOR_TIMES_SCALAR = 142;
    constexpr uint32_t OP_MATRIX_TIMES_VECTOR = 145;

    bool isArithmetic(uint32_t opcode){
        return opcode == OP_EXT_INST || opcode == OP_FNEGATE || opcode == OP_FADD || opcode == OP_FSUB || opcode == OP_FMUL ||
               opcode == OP_FDIV || opcode == OP_VECTOR_TIMES_SCALAR || opcode == OP_MATRIX_TIMES_VECTOR;
    }

    struct VertexCount {
        bool ok = false;
        size_t arithmetic = 0;                       // the whole module
        std::map<std::string, size_t> functions;     // by the function's name (its prefix before '(')
    };

    VertexCount countVertexStage(uint64_t properties, std::string& error){
        VertexCount count;
        ShaderData data;
        std::string log;
        if (!ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, properties), ShaderBackend::Vulkan, nullptr, data,
                                            &error, &log)){
            error += "\n" + log;
            return count;
        }
        for (const ShaderStage& stage : data.stages){
            if (stage.type != ShaderStageType::VERTEX || !stage.bytecode.data || stage.bytecode.size < 20)
                continue;
            std::vector<uint32_t> words(stage.bytecode.size / sizeof(uint32_t));
            std::memcpy(words.data(), stage.bytecode.data, words.size() * sizeof(uint32_t));
            std::map<uint32_t, std::string> names;
            uint32_t current = 0;
            for (size_t offset = 5; offset < words.size();){
                const uint32_t opcode = words[offset] & 0xffffu;
                const uint32_t length = words[offset] >> 16;
                if (length == 0 || offset + length > words.size())
                    return count;
                if (opcode == OP_NAME && length > 2){
                    std::string name(reinterpret_cast<const char*>(&words[offset + 2]));
                    names[words[offset + 1]] = name.substr(0, name.find('('));
                }else if (opcode == OP_FUNCTION && length > 2){
                    current = words[offset + 2];
                }else if (opcode == OP_FUNCTION_END){
                    current = 0;
                }else if (isArithmetic(opcode) && current != 0){
                    count.arithmetic++;
                    count.functions[names[current]]++;
                }
                offset += length;
            }
            count.ok = true;
        }
        return count;
    }

}

TEST(PixelSnapShaderTest, VertexCost){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    // a sprite's variant: Unlit, UV1, vertex colour, texture rect
    const uint64_t sprite = shaderPropertyBit(0) | shaderPropertyBit(1) | shaderPropertyBit(10) | shaderPropertyBit(11);
    std::string error;
    const VertexCount plain = countVertexStage(sprite, error);
    ASSERT_TRUE(plain.ok) << error;
    const VertexCount snapped = countVertexStage(sprite | ShaderPool::getPixelSnapProperty(ShaderType::MESH), error);
    ASSERT_TRUE(snapped.ok) << error;

    auto own = [&snapped](const char* name){
        const auto it = snapped.functions.find(name);
        return it == snapped.functions.end() ? (size_t)0 : it->second;
    };
    const size_t perVertex = own("pixelSnapClip") + own("pixelSnapToPixels") + own("pixelSnapRound2") + own("pixelSnapFromPixels");
    std::printf("[bench] S41 mesh.vert arithmetic: %zu without Pxs, %zu with (the per-vertex snap path %zu, pixelSnapApply %zu, "
                "the pivot path pixelSnapCenterDelta %zu)\n", plain.arithmetic, snapped.arithmetic, perVertex, own("pixelSnapApply"),
                own("pixelSnapCenterDelta"));
    ASSERT_GT(own("pixelSnapRound2"), 0u) << "the snapped variant runs the rule";
    // R17's budget: the rule's float32 sequence (the GLSL twin of the CPU's) at most 14: 4 to
    // pixels (divide, flip, scale, centre), 3 to round both axes (two additions for the bias,
    // floor), 4 back
    EXPECT_LE(perVertex, 14u);
    EXPECT_EQ(plain.functions.count("pixelSnapRound2"), 0u) << "without the bit nothing of the rule is compiled";
}

// S41's variant bits stay where saved shader keys and caches put them: MESH 45 'Pxs' (42-43 are
// the water interaction's, 44 draw-perf's), DEPTH 19, UI 5, PARTICLES 11, and BLIT 18 'Psb'
// (Stretch Fill); each type's count covers it, so a later spec takes the next free bit
TEST(PixelSnapShaderTest, VariantBitsArePinned){
    EXPECT_EQ(ShaderPool::getPixelSnapProperty(ShaderType::MESH), shaderPropertyBit(45));
    EXPECT_EQ(ShaderPool::getPixelSnapProperty(ShaderType::DEPTH), shaderPropertyBit(19));
    EXPECT_EQ(ShaderPool::getPixelSnapProperty(ShaderType::UI), shaderPropertyBit(5));
    EXPECT_EQ(ShaderPool::getPixelSnapProperty(ShaderType::PARTICLES), shaderPropertyBit(11));
    for (ShaderType type : {ShaderType::MESH, ShaderType::DEPTH, ShaderType::UI, ShaderType::PARTICLES}){
        const uint64_t bit = ShaderPool::getPixelSnapProperty(type);
        int index = 0;
        while (index < 64 && shaderPropertyBit(index) != bit) index++;
        EXPECT_EQ(ShaderPool::getShaderPropertyName(type, index, true), "Pxs");
        EXPECT_EQ(ShaderPool::getShaderPropertyCount(type), index + 1) << "'Pxs' is the last bit of its type";
    }
    EXPECT_EQ(UBER_SHARP_BILINEAR, 1u << 18);
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::BLIT, 18, true), "Psb");
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::BLIT), 19);
}

// a block the engine extends (pixelSnapDraw) is uploaded at most as large as the shader declares
// it: a fork made without the member gets the prefix it reads, sokol's size check holds
TEST(PixelSnapShaderTest, UploadsFitTheReflectedBlock){
    EXPECT_EQ(RenderSystem::uniformUploadSize(208, 192), 192u);
    EXPECT_EQ(RenderSystem::uniformUploadSize(208, 208), 208u);
    EXPECT_EQ(RenderSystem::uniformUploadSize(192, 0), 192u) << "a block not reflected keeps the engine's size";
    // a shadow map is never snapped on the camera's grid; other depth passes follow their colour pass
    EXPECT_EQ(RenderSystem::depthSnapMode(PIP_SHADOW_DEPTH, 1.0f), 0.0f);
    EXPECT_EQ(RenderSystem::depthSnapMode(PIP_SHADOW_DEPTH, 2.0f), 0.0f);
    EXPECT_EQ(RenderSystem::depthSnapMode(PIP_DEPTH, 1.0f), 1.0f);
    EXPECT_EQ(RenderSystem::depthSnapMode(PIP_ZPREPASS, 2.0f), 2.0f);
}
