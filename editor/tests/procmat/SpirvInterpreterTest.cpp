// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU tier's interpreter (editor/procmat) on glslang's real output for
// tools/procmat/interpreter_fixtures.glsl. Each expected value is computed here in C++ with the
// GLSL definition of the operation, so a wrong instruction shows as a wrong number. The whole
// node library is compared against a GPU tier by `procmat-sweep crosstier` (S90 M0).

#include "procmat/SpirvInterpreter.h"

#include "SpirvInterpreterFixtures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine::editor::procmat;

namespace {

template <size_t N>
std::unique_ptr<SpirvProgram> loadFixture(const uint32_t (&words)[N]){
    std::string error;
    std::unique_ptr<SpirvProgram> program = SpirvProgram::load(words, N, error);
    EXPECT_TRUE(program != nullptr) << error;
    return program;
}

float glslFract(float x){ return x - std::floor(x); }
float glslMod(float x, float y){ return x - y * std::floor(x / y); }
float glslSmoothstep(float e0, float e1, float x){
    const float t = std::min(std::max((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

const float samplePoints[][2] = {{0.1f, 0.2f}, {0.37f, 0.83f}, {0.5f, 0.5f}, {0.76f, 0.26f}, {0.99f, 0.01f}};

// A module as words, to corrupt: instructions are found by opcode, in order
template <size_t N>
std::vector<uint32_t> wordsOf(const uint32_t (&fixture)[N]){
    return std::vector<uint32_t>(fixture, fixture + N);
}

size_t findInstruction(const std::vector<uint32_t>& words, uint32_t op, int nth = 0){
    for (size_t i = 5; i < words.size();){
        const uint32_t count = words[i] >> 16;
        if ((words[i] & 0xffff) == op && nth-- == 0)
            return i;
        if (count == 0)
            break;
        i += count;
    }
    ADD_FAILURE() << "no instruction with opcode " << op;
    return 5;
}

void expectRefused(const std::vector<uint32_t>& words, const char* what){
    std::string error;
    EXPECT_EQ(SpirvProgram::load(words.data(), words.size(), error), nullptr) << what;
    EXPECT_FALSE(error.empty()) << what;
}

// A small assembler, for shapes glslang does not produce on demand
struct Assembler {
    std::vector<uint32_t> words{0x07230203u, 0x00010000u, 0u, 64u, 0u};

    void add(uint32_t op, std::initializer_list<uint32_t> operands){
        words.push_back(((uint32_t)(operands.size() + 1) << 16) | op);
        words.insert(words.end(), operands);
    }
};

// Opcodes and ids of the assembled modules
enum : uint32_t {
    opName = 5, opExtInst = 12, opEntryPoint = 15, opCapability = 17, opTypeVoid = 19, opTypeBool = 20, opTypeFloat = 22, opTypeVector = 23,
    opTypePointer = 32, opTypeFunction = 33, opConstant = 43, opConstantComposite = 44, opFunction = 54, opFunctionEnd = 56, opVariable = 59,
    opLoad = 61, opStore = 62, opCompositeConstruct = 80, opCompositeExtract = 81, opCopyObject = 83, opFAdd = 129, opFOrdLessThan = 184,
    opPhi = 245, opLabel = 248, opBranch = 249, opBranchConditional = 250, opReturn = 253, opNop = 0,
    opTypeArray = 28, opFunctionCall = 57,

    idVoid = 1, idMainType, idFloat, idVec2, idVec4, idBool, idOutputPointer, idOutput, idMain, idZero, idOne, idTwo, idInitial, idNext,
    idEntry, idLoop, idBack, idExit, idA, idB, idI, idY, idIPlus, idLess, idColor,
};

void assembleHeader(Assembler& a){
    a.add(opCapability, {1});
    a.add(opEntryPoint, {4, idMain, 0x6e69616du, 0u, idOutput});
    a.add(opTypeVoid, {idVoid});
    a.add(opTypeFunction, {idMainType, idVoid});
    a.add(opTypeFloat, {idFloat, 32});
    a.add(opTypeVector, {idVec2, idFloat, 2});
    a.add(opTypeVector, {idVec4, idFloat, 4});
    a.add(opTypeBool, {idBool});
    a.add(opTypePointer, {idOutputPointer, 3, idVec4});
    a.add(opVariable, {idOutputPointer, idOutput, 3});
    const auto bitsOf = [](float x){ uint32_t u; std::memcpy(&u, &x, 4); return u; };
    a.add(opConstant, {idFloat, idZero, bitsOf(0.0f)});
    a.add(opConstant, {idFloat, idOne, bitsOf(1.0f)});
    a.add(opConstant, {idFloat, idTwo, bitsOf(2.0f)});
    a.add(opConstantComposite, {idVec2, idInitial, idOne, idTwo});
    a.add(opConstantComposite, {idVec2, idNext, idTwo, idOne});
}

}

TEST(SpirvInterpreter, ArithmeticMatchesGlslDefinitions){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_arithmetic);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    ASSERT_EQ(program->uniformWords(), 8u);
    const float uniforms[8] = {0.75f, 0, 0, 0, 0, 0, 0, 0};
    machine.setUniforms(uniforms, 8);
    for (const auto& point : samplePoints){
        const float u = point[0], v = point[1];
        float out[4];
        ASSERT_EQ(machine.shade(u, v, out), SpirvMachine::Result::Ok);
        const float a[3] = {u, v, 0.75f};
        const float b[3] = {a[2] * 2.0f + 1.0f, a[0] * 2.0f - 1.0f, a[1] * 2.0f + 0.5f};
        const float m = glslMod(-b[0], 0.75f) + glslFract(b[1]) + (a[0] < 0.5f ? 0.0f : 1.0f) + glslSmoothstep(0.0f, 2.0f, b[2]);
        EXPECT_NEAR(out[0], m, 1e-5f);
        const float nx = a[0] + 1.0f, ny = a[1] + 1.0f, nl = std::sqrt(nx * nx + ny * ny);
        EXPECT_NEAR(out[1], (nx / nl) * (ny / nl), 1e-5f);
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) + a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
                      + std::sqrt(dx * dx + dy * dy + dz * dz) + a[0] * b[1] - a[1] * b[0];
        const float c = std::min(std::max(b[0], 0.25f), 0.5f) + (a[0] * 0.75f + a[1] * 0.25f) + std::min(a[0], b[0]) + std::max(a[1], b[1])
                      + std::fabs(a[0]) + (-b[1] > 0.0f ? 1.0f : (-b[1] < 0.0f ? -1.0f : 0.0f)) + std::floor(b[2]) + std::ceil(b[2]);
        EXPECT_NEAR(out[2], l + c, 1e-4f);
        const float t = std::sin(a[0]) + std::cos(a[1]) + std::pow(a[0] + 1.0f, 1.5f) + std::exp(a[0]) + std::log(a[1] + 1.0f) + std::sqrt(a[0])
                      + std::atan2(a[1], a[0] + 1.0f) + a[0] * 3.14159265f / 180.0f + std::nearbyint(b[0] * 3.0f);
        EXPECT_NEAR(out[3], t, 1e-4f);
    }
}

TEST(SpirvInterpreter, IntegersAreExactAndDivisionByZeroDoesNotTrap){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_integers);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    for (const auto& point : samplePoints){
        const float u = point[0], v = point[1];
        float out[4];
        ASSERT_EQ(machine.shade(u, v, out), SpirvMachine::Result::Ok);
        const int32_t i = (int32_t)(u * 1000.0f) - 300;
        const uint32_t w = (uint32_t)(v * 1000.0f);
        // / truncates, % is SPIR-V's OpSMod (the sign of the divisor), >> is arithmetic
        const int32_t d = i / 7 + ((i % 7) + 7) % 7 + (int32_t)((uint32_t)i << 3) + (i >> 2) + (i & 255) + (i | 8) + (i ^ 77) + std::abs(i)
                        + std::min(i, 5) + std::max(i, -5) + std::min(std::max(i, -2), 2) + (i > 0 ? 1 : (i < 0 ? -1 : 0));
        EXPECT_EQ(out[0], (float)d);
        uint32_t e = w * 2654435761u;
        e ^= e >> 16u;
        e = (e << 7u) | (e >> 25u);
        e += std::min(w, 5u) + std::max(w, 9u) + w / 3u + w % 3u;
        EXPECT_EQ(out[1], (float)(e >> 8u));
        const int32_t px = i * 3, py = (i + 1) * -2;
        EXPECT_EQ(out[2], (float)(px + py));
        EXPECT_EQ(out[3], (float)((uint32_t)px >> 28u));
    }
}

TEST(SpirvInterpreter, ControlFlowFunctionsArraysAndMatrices){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_control_flow);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    const float table[4] = {0.125f, 0.25f, 0.5f, 1.0f};
    for (const auto& point : samplePoints){
        const float u = point[0], v = point[1];
        float out[4];
        ASSERT_EQ(machine.shade(u, v, out), SpirvMachine::Result::Ok);
        // weight(): an inout, an out and a by-value struct
        EXPECT_NEAR(out[0], u * 0.5f + 1.5f + 3.0f, 1e-5f);
        float sum = 0.0f;
        for (int i = 0; i < 6; i++){
            if (i == 2)
                continue;
            if ((float)i > u * 8.0f)
                break;
            sum += table[i % 4];
        }
        const int index = (int)(u * 4.0f);
        const float choose = index == 0 ? 1.5f : ((index == 1 || index == 2) ? 2.5f : -1.0f);
        const bool both = u > 0.25f && v > 0.25f;
        const bool either = u > 0.75f || v > 0.75f;
        const float picked = both ? 1.0f : (either ? 2.0f : 3.0f);
        EXPECT_NEAR(out[1], sum + choose + picked, 1e-5f);
        const float a2 = (index % 2 == 0 ? u : v) + 1.0f;
        EXPECT_NEAR(out[2], (float)(index + 1) + a2 + 2.0f, 1e-5f);
        // mat2(1, 2, 3, u): columns (1, 2) and (3, u)
        const float mvx = 1.0f * u + 3.0f * v + (u * 1.0f + v * 2.0f);
        const float mvy = 2.0f * u + u * v + (u * 3.0f + v * u);
        // row 0 of column 1 of m * m
        const float mm10 = 1.0f * 3.0f + 3.0f * u;
        EXPECT_NEAR(out[3], mvx + mvy + mm10, 1e-4f);
    }
}

// The hash of R10, the same bits on every tier: this C++ is the third implementation of it
TEST(SpirvInterpreter, IntegerHashGivesTheExactBits){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_rand);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    const auto quantize = [](float x){ return (int32_t)std::floor(std::min(std::max(x, -32767.0f), 32767.0f) * 65536.0f + 0.5f); };
    const auto mix = [](uint32_t h, int32_t value){
        uint32_t k = (uint32_t)value * 0xcc9e2d51u;
        k = ((k << 15u) | (k >> 17u)) * 0x1b873593u;
        h ^= k;
        h = (h << 13u) | (h >> 19u);
        return h * 5u + 0xe6546b64u;
    };
    const auto finish = [](uint32_t h, uint32_t stream){
        h ^= stream * 0x9e3779b9u;
        h ^= h >> 16u;
        h *= 0x85ebca6bu;
        h ^= h >> 13u;
        h *= 0xc2b2ae35u;
        h ^= h >> 16u;
        return (float)(h >> 8u) * (1.0f / 16777216.0f);
    };
    for (const auto& point : samplePoints){
        float out[4];
        ASSERT_EQ(machine.shade(point[0], point[1], out), SpirvMachine::Result::Ok);
        const uint32_t h = mix(mix(0x2f8b15c7u, quantize(point[0])), quantize(point[1]));
        EXPECT_EQ(out[0], finish(h, 1u));
        EXPECT_EQ(out[1], finish(h, 2u));
        EXPECT_EQ(out[2], finish(h, 3u));
        EXPECT_GE(out[0], 0.0f);
        EXPECT_LT(out[0], 1.0f);
    }
}

TEST(SpirvInterpreter, SamplerIsBilinearRepeatWithBoxMips){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_sampler);
    ASSERT_TRUE(program);
    // 2x2, red: 0.25 0.5 / 0.75 1.0
    const SpirvImage image(2, 2, std::vector<float>{0.25f, 0, 0, 1, 0.5f, 0, 0, 1, 0.75f, 0, 0, 1, 1.0f, 0, 0, 1});
    EXPECT_EQ(image.mipCount(), 2);
    SpirvMachine machine(*program);
    const int slot = program->textureSlot("t_input");
    ASSERT_GE(slot, 0);
    EXPECT_EQ(program->textureSlot("nothing"), -1);
    machine.setTexture(slot, &image);
    float out[4];
    // texel centers: exact
    ASSERT_EQ(machine.shade(0.25f, 0.25f, out), SpirvMachine::Result::Ok);
    EXPECT_NEAR(out[0], 0.25f, 1e-6f);
    EXPECT_NEAR(out[1], 0.25f, 1e-6f); // the same texel after wrapping
    EXPECT_NEAR(out[2], 0.625f, 1e-6f); // the box average, mip 1
    EXPECT_NEAR(out[3], (0.25f + 0.625f) * 0.5f, 1e-6f); // halfway to mip 1
    // between the four texels, across the wrap in x
    ASSERT_EQ(machine.shade(0.0f, 0.5f, out), SpirvMachine::Result::Ok);
    EXPECT_NEAR(out[0], 0.625f, 1e-6f);
}

// A node's 0/0 must give what the GPU tiers give: a smooth minimum with k = 0 differed from llvmpipe
// by 127/255 until min, max and clamp followed minNum and maxNum
TEST(SpirvInterpreter, NaNLosesInMinMaxAndClamp){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_nan_rules);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    float out[4];
    ASSERT_EQ(machine.shade(0.5f, 0.5f, out), SpirvMachine::Result::Ok);
    EXPECT_EQ(out[0], 0.25f);
    EXPECT_EQ(out[1], 0.5f);
    EXPECT_EQ(out[2], 0.5f);
    EXPECT_EQ(out[3], 2.0f);
}

TEST(SpirvInterpreter, AnEndlessLoopStopsThePixel){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_endless);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    float out[4];
    EXPECT_EQ(machine.shade(0.25f, 0.5f, out), SpirvMachine::Result::TooLong);
    EXPECT_EQ(machine.shade(0.25f, 0.5f, out), SpirvMachine::Result::TooLong);
}

// R11: found at load, naming the instruction, not per pixel
TEST(SpirvInterpreter, AnUnsupportedInstructionFailsAtLoad){
    std::string error;
    const std::unique_ptr<SpirvProgram> program = SpirvProgram::load(fixture_uses_derivative, std::size(fixture_uses_derivative), error);
    EXPECT_EQ(program, nullptr);
    EXPECT_NE(error.find("unsupported SPIR-V instruction"), std::string::npos) << error;
    // OpDPdx is opcode 207
    EXPECT_NE(error.find("207"), std::string::npos) << error;
}

TEST(SpirvInterpreter, RejectsWhatIsNotAModule){
    std::string error;
    const uint32_t garbage[] = {1, 2, 3, 4, 5, 6};
    EXPECT_EQ(SpirvProgram::load(garbage, std::size(garbage), error), nullptr);
    EXPECT_FALSE(error.empty());
    error.clear();
    // cut before the last instruction, and in the middle of the last but one (OpReturn, OpFunctionEnd)
    for (const size_t missing : {1u, 3u}){
        error.clear();
        EXPECT_EQ(SpirvProgram::load(fixture_rand, std::size(fixture_rand) - missing, error), nullptr);
        EXPECT_FALSE(error.empty());
    }
}

// Every prefix of a module is refused: a cut after OpEntryPoint once loaded, and then overflowed the
// call stack in shade()
TEST(SpirvInterpreter, EveryTruncationIsRefused){
    std::string error;
    for (size_t length = 0; length < std::size(fixture_rand); length++){
        error.clear();
        EXPECT_EQ(SpirvProgram::load(fixture_rand, length, error), nullptr) << length;
        EXPECT_FALSE(error.empty()) << length;
    }
}

TEST(SpirvInterpreter, AnEntryPointMustNameAFunction){
    std::vector<uint32_t> words = wordsOf(fixture_rand);
    words[findInstruction(words, opEntryPoint) + 2] = words[3] - 1;
    expectRefused(words, "an entry point naming nothing");
}

// R11: discard is refused by name, before any later stage reads operands it does not have
TEST(SpirvInterpreter, DiscardIsAnUnsupportedInstruction){
    std::string error;
    EXPECT_EQ(SpirvProgram::load(fixture_uses_discard, std::size(fixture_uses_discard), error), nullptr);
    EXPECT_NE(error.find("opcode 252"), std::string::npos) << error;
}

TEST(SpirvInterpreter, CorruptModulesAreRefusedAtLoad){
    {
        std::vector<uint32_t> words = wordsOf(fixture_rand);
        words[findInstruction(words, opLoad) + 3] = words[3] + 100;
        expectRefused(words, "an id past the bound");
    }
    {
        std::vector<uint32_t> words = wordsOf(fixture_rand);
        words[findInstruction(words, opLoad)] = (3u << 16) | opLoad;
        expectRefused(words, "an instruction too short");
    }
    {
        std::vector<uint32_t> words = wordsOf(fixture_rand);
        words[findInstruction(words, opTypeVector) + 3] = 1000;
        expectRefused(words, "a vector of 1000");
    }
    for (const uint32_t length : {0u, 0x40000000u}){
        std::vector<uint32_t> words = wordsOf(fixture_control_flow);
        const uint32_t lengthId = words[findInstruction(words, opTypeArray) + 3];
        for (int nth = 0;; nth++){
            const size_t at = findInstruction(words, opConstant, nth);
            if (words[at + 2] == lengthId){
                words[at + 3] = length;
                break;
            }
        }
        expectRefused(words, "an array of a size that does not fit");
    }
    {
        // a load through a float: it would be an indirection driven by shader data
        std::vector<uint32_t> words = wordsOf(fixture_rand);
        const uint32_t floatConstant = words[findInstruction(words, opConstant) + 2];
        words[findInstruction(words, opLoad) + 3] = floatConstant;
        expectRefused(words, "a load through a value");
    }
    {
        std::vector<uint32_t> words = wordsOf(fixture_control_flow);
        const size_t call = findInstruction(words, opFunctionCall);
        words[call] += 1u << 16;
        words.insert(words.begin() + (long)call + (words[call] >> 16) - 1, words[call + 4]);
        expectRefused(words, "a call with an extra argument");
    }
    {
        std::vector<uint32_t> words = wordsOf(fixture_rand);
        size_t last = 0;
        for (int nth = 0;; nth++){
            bool found = false;
            for (size_t i = 5; i < words.size(); i += words[i] >> 16){
                if ((words[i] & 0xffff) == opReturn)
                    last = i, found = true;
            }
            if (found)
                break;
            ADD_FAILURE() << nth;
            break;
        }
        words[last] = (1u << 16) | opNop;
        expectRefused(words, "a block with no terminator");
    }
    {
        std::vector<uint32_t> words = wordsOf(fixture_arithmetic);
        words[findInstruction(words, opCompositeExtract) + 4] = 0x7fffffffu;
        expectRefused(words, "an index past its composite");
    }
    {
        Assembler a;
        assembleHeader(a);
        a.add(opFunction, {idVoid, idMain, 0, idMainType});
        a.add(opLabel, {idEntry});
        a.add(opCopyObject, {idFloat, idA, idB});
        a.add(opCopyObject, {idFloat, idB, idA});
        a.add(opReturn, {});
        a.add(opFunctionEnd, {});
        expectRefused(a.words, "two renames of each other");
    }
}

// glslang's shape for `v.xy + f(v)`: the swizzle reads v before the call writes it, and renames its
// registers. It once kept reading them after the call's copy-out.
TEST(SpirvInterpreter, ASwizzleKeepsTheValueItWasTaken){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_swizzle_alias);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    float out[4];
    ASSERT_EQ(machine.shade(0.25f, 0.5f, out), SpirvMachine::Result::Ok);
    EXPECT_EQ(out[0], 0.25f);
    EXPECT_EQ(out[1], 0.5f);
    EXPECT_EQ(out[2], 1.0f);
}

// AC6: a pixel's value must not depend on the rows this worker happened to shade before it
TEST(SpirvInterpreter, GlobalsAndTheOutputStartEachPixelCleared){
    {
        const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_private_uninitialized);
        ASSERT_TRUE(program);
        SpirvMachine machine(*program);
        float out[4];
        machine.shade(0.25f, 0.5f, out);
        EXPECT_EQ(out[0], 1.0f);
        machine.shade(0.75f, 0.5f, out);
        EXPECT_EQ(out[0], 0.0f);
    }
    {
        const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_output_partly_written);
        ASSERT_TRUE(program);
        SpirvMachine machine(*program);
        float out[4];
        machine.shade(0.25f, 0.5f, out);
        EXPECT_EQ(out[0], 1.0f);
        machine.shade(0.75f, 0.5f, out);
        EXPECT_EQ(out[0], 0.0f);
    }
}

TEST(SpirvInterpreter, OnlyPlain2DImagesLoad){
    std::string error;
    EXPECT_EQ(SpirvProgram::load(fixture_image_array, std::size(fixture_image_array), error), nullptr);
    EXPECT_NE(error.find("2D"), std::string::npos) << error;
}

TEST(SpirvInterpreter, ABlockWithPaddingIsRefused){
    std::string error;
    EXPECT_EQ(SpirvProgram::load(fixture_block_with_padding, std::size(fixture_block_with_padding), error), nullptr);
    EXPECT_NE(error.find("padding"), std::string::npos) << error;
}

// phi b takes the old a.y. Copied after a, it read the new one: the conflict test compared only
// the first register of each phi.
TEST(SpirvInterpreter, APhiReadingPartOfAnotherPhiGetsTheOldValue){
    Assembler a;
    assembleHeader(a);
    a.add(opFunction, {idVoid, idMain, 0, idMainType});
    a.add(opLabel, {idEntry});
    a.add(opBranch, {idLoop});
    a.add(opLabel, {idLoop});
    a.add(opPhi, {idVec2, idA, idInitial, idEntry, idNext, idLoop});
    a.add(opPhi, {idFloat, idB, idZero, idEntry, idY, idLoop});
    a.add(opPhi, {idFloat, idI, idZero, idEntry, idIPlus, idLoop});
    a.add(opCompositeExtract, {idFloat, idY, idA, 1});
    a.add(opFAdd, {idFloat, idIPlus, idI, idOne});
    a.add(opFOrdLessThan, {idBool, idLess, idIPlus, idTwo});
    a.add(opBranchConditional, {idLess, idLoop, idExit});
    a.add(opLabel, {idExit});
    a.add(opCompositeExtract, {idFloat, idColor, idA, 1});
    a.add(opCompositeConstruct, {idVec4, idBack, idColor, idB, idI, idOne});
    a.add(opStore, {idOutput, idBack});
    a.add(opReturn, {});
    a.add(opFunctionEnd, {});
    std::string error;
    const std::unique_ptr<SpirvProgram> program = SpirvProgram::load(a.words.data(), a.words.size(), error);
    ASSERT_TRUE(program) << error;
    SpirvMachine machine(*program);
    float out[4];
    ASSERT_EQ(machine.shade(0.5f, 0.5f, out), SpirvMachine::Result::Ok);
    EXPECT_EQ(out[0], 1.0f); // a.y after the second pass: idNext is (2, 1)
    EXPECT_EQ(out[1], 2.0f); // b, the a.y before it
    EXPECT_EQ(out[2], 1.0f);
}

TEST(SpirvInterpreter, ASampleOfNaNIsDefinedAndABadImageIsATexel){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_sample_nan);
    ASSERT_TRUE(program);
    const SpirvImage image(2, 2, std::vector<float>{0.25f, 0, 0, 1, 0.5f, 0, 0, 1, 0.75f, 0, 0, 1, 1.0f, 0, 0, 1});
    SpirvMachine machine(*program);
    machine.setTexture(program->textureSlot("t_input"), &image);
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    machine.setUniforms(zero, 4);
    float out[4];
    ASSERT_EQ(machine.shade(0.5f, 0.5f, out), SpirvMachine::Result::Ok);
    // both coordinates count as 0: between the four texels, across the wrap
    EXPECT_NEAR(out[0], 0.625f, 1e-6f);

    float texel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const SpirvImage empty(0, 4, {});
    empty.sampleBilinear(0.5f, 0.5f, 0, texel);
    EXPECT_EQ(texel[3], 0.0f);
    const SpirvImage shortData(2, 2, std::vector<float>(3, 1.0f));
    shortData.sampleBilinear(0.5f, 0.5f, 0, texel);
    EXPECT_EQ(texel[0], 0.0f);
}

TEST(SpirvInterpreter, RoundingKeepsTheSignOfZero){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_sign_of_zero);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    float out[4];
    ASSERT_EQ(machine.shade(0.5f, 0.5f, out), SpirvMachine::Result::Ok);
    EXPECT_NEAR(out[0], 3.14159265f, 1e-5f);
    EXPECT_NEAR(out[1], 3.14159265f, 1e-5f);
    EXPECT_LT(out[2], 0.0f);
    EXPECT_TRUE(std::isinf(out[2]));
    EXPECT_NEAR(out[3], 3.14159265f, 1e-5f);
}

// What the library uses and no other fixture reaches, so a regression shows without a GL sweep
TEST(SpirvInterpreter, ComparisonsNegationsAndTheRestOfTheMath){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_coverage);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    for (const auto& point : samplePoints){
        const float a = point[0], b = point[1];
        float out[4];
        ASSERT_EQ(machine.shade(a, b, out), SpirvMachine::Result::Ok);
        const int32_t i = (int32_t)(a * 100.0f), j = (int32_t)(b * 100.0f);
        float k = 0.0f;
        if (i != j) k += 1.0f;
        if (i > j) k += 2.0f;
        if (i >= j) k += 4.0f;
        if (i <= j) k += 8.0f;
        if (a != b) k += 16.0f;
        if (a <= b) k += 32.0f;
        if (a >= b) k += 64.0f;
        if (a < b || a > 0.0f) k += 128.0f;
        if (a < b && a > 0.0f) k += 256.0f;
        EXPECT_EQ(out[0], k);
        EXPECT_EQ(out[1], (float)(-i) + (float)(~j));
        const float t = std::trunc(a * 7.0f) + a * 57.29577951f + std::tan(a) + std::asin(a) + std::acos(a) + std::atan(a) + std::sinh(a)
                      + std::cosh(a) + std::tanh(a) + std::exp2(a) + std::log2(a + 1.0f);
        EXPECT_NEAR(out[2], t, 1e-4f);
        EXPECT_NEAR(out[3], (a - b) + (a < 0.5f ? -a : a), 1e-5f);
    }
}

// R13: one thread's results do not depend on what it shaded before
TEST(SpirvInterpreter, APixelIsIndependentOfThePreviousOne){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_control_flow);
    ASSERT_TRUE(program);
    SpirvMachine used(*program);
    float ignored[4];
    used.shade(0.9f, 0.9f, ignored);
    used.shade(0.05f, 0.95f, ignored);
    for (const auto& point : samplePoints){
        SpirvMachine fresh(*program);
        float a[4], b[4];
        fresh.shade(point[0], point[1], a);
        used.shade(point[0], point[1], b);
        for (int k = 0; k < 4; k++)
            EXPECT_EQ(a[k], b[k]);
    }
}
