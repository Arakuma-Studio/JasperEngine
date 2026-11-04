// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC14 (R17), static half: what the pixel-snap rule adds to a sprite's vertex stage, counted
// in the SPIR-V of mesh.vert with and without 'Pxs' (glslang runs without an optimizer, so the
// rule's helpers stay functions and each one's arithmetic is counted on its own). The per-vertex
// path is pixelSnapClip: pixelSnapToPixels, two pixelSnapRound and pixelSnapFromPixels. The
// variant without the bit must stay exactly the variant of before (R14).

#include "pool/ShaderPool.h"
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
    const size_t perVertex = own("pixelSnapClip") + own("pixelSnapToPixels") + 2 * own("pixelSnapRound") + own("pixelSnapFromPixels");
    std::printf("[bench] S41 mesh.vert arithmetic: %zu without Pxs, %zu with (the per-vertex snap path %zu, pixelSnapApply %zu, "
                "the pivot path pixelSnapCenterDelta %zu)\n", plain.arithmetic, snapped.arithmetic, perVertex, own("pixelSnapApply"),
                own("pixelSnapCenterDelta"));
    ASSERT_GT(own("pixelSnapRound"), 0u) << "the snapped variant runs the rule";
    // the rule's own float32 sequence (two additions for the bias, the GLSL twin of the CPU's):
    // 6 to pixels, 3 per axis to round, 6 back
    EXPECT_LE(perVertex, 18u);
    EXPECT_EQ(plain.functions.count("pixelSnapRound"), 0u) << "without the bit nothing of the rule is compiled";
}
