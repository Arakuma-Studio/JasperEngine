// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU tier's interpreter (editor/procmat) on glslang's real output for
// tools/procmat/interpreter_fixtures.glsl. Each expected value is computed here in C++ with the
// GLSL definition of the operation, so a wrong instruction shows as a wrong number. The whole
// node library is compared against a GPU tier by `procmat-sweep crosstier` (S53 M0).

#include "procmat/SpirvInterpreter.h"

#include "SpirvInterpreterFixtures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
        EXPECT_NEAR(out[2], (float)(index + 1) + a2, 1e-5f);
        // mat2(1, 2, 3, u): columns (1, 2) and (3, u)
        const float mvx = 1.0f * u + 3.0f * v + (u * 1.0f + v * 2.0f);
        const float mvy = 2.0f * u + u * v + (u * 3.0f + v * u);
        const float mm10 = 1.0f * 2.0f + 3.0f * u;
        const float det = 1.0f * u - 3.0f * 2.0f;
        EXPECT_NEAR(out[3], mvx + mvy + mm10 + det, 1e-4f);
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

TEST(SpirvInterpreter, DiscardIsReported){
    const std::unique_ptr<SpirvProgram> program = loadFixture(fixture_discard_half);
    ASSERT_TRUE(program);
    SpirvMachine machine(*program);
    float out[4];
    EXPECT_EQ(machine.shade(0.25f, 0.5f, out), SpirvMachine::Result::Discarded);
    EXPECT_EQ(machine.shade(0.75f, 0.5f, out), SpirvMachine::Result::Ok);
    EXPECT_EQ(out[0], 1.0f);
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
    // cut in the middle of an instruction
    const size_t cut = std::size(fixture_rand) - 1;
    EXPECT_EQ(SpirvProgram::load(fixture_rand, cut, error), nullptr);
    EXPECT_FALSE(error.empty());
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
