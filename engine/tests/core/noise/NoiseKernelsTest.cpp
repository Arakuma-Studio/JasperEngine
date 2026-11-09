// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 AC1-AC5, AC7: the kernels, their back ends, numerics and fractals, and the golden table.

#include "NoiseGoldenCases.h"
#include "NoiseTestSupport.h"
#include "noise/NoiseDetMath.h"
#include "noise/NoiseKernels.h"
#include "noise_presets.gen.h"

#include <gtest/gtest.h>

#include <cfenv>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::noise;
using noisetest::bitsOf;

namespace {

    std::vector<golden::Preset> embeddedPresets() {
        std::vector<golden::Preset> presets;
        for (const NoisePresetEmbeddedFile* file = noise_preset_embedded_files; file->id; file++) {
            presets.push_back(golden::Preset{file->id, std::string(reinterpret_cast<const char*>(file->data), file->size)});
        }
        return presets;
    }

    struct Named { BasisParams params; std::string name; };

    std::vector<Named> everyKernel() {
        std::vector<Named> out;
        for (int dims : {2, 3}) {
            for (Basis basis : {Basis::Perlin, Basis::Simplex, Basis::Value, Basis::Cellular}) {
                BasisParams params;
                params.basis = basis;
                params.dimensions = dims;
                params.frequency = 1.0f;
                params.seed = 42u;
                out.push_back({params, "basis " + std::to_string(static_cast<int>(basis)) + " dims " + std::to_string(dims)});
            }
            for (CellularReturn ret : {CellularReturn::F2, CellularReturn::F2MinusF1, CellularReturn::CellValue}) {
                for (CellularMetric metric : {CellularMetric::Euclidean, CellularMetric::Manhattan, CellularMetric::Chebyshev}) {
                    BasisParams params;
                    params.basis = Basis::Cellular;
                    params.dimensions = dims;
                    params.frequency = 1.0f;
                    params.seed = 42u;
                    params.cellReturn = ret;
                    params.metric = metric;
                    params.jitter = 0.8f;
                    out.push_back({params, "cellular ret " + std::to_string(static_cast<int>(ret)) + " metric " + std::to_string(static_cast<int>(metric)) + " dims " + std::to_string(dims)});
                }
            }
        }
        return out;
    }

    struct Points { std::vector<float> x, y, z; size_t size() const { return x.size(); } };

    // Negative cells, the wrap and tiny coordinates (AC1)
    Points scatteredPoints(size_t count) {
        Points points;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> wide(-70000.0f, 70000.0f);
        std::uniform_real_distribution<float> near(-3.0f, 3.0f);
        std::uniform_real_distribution<float> tiny(-1e-30f, 1e-30f);
        for (size_t i = 0; i < count; i++) {
            const auto pick = [&]() {
                switch (i % 3) {
                    case 0: return wide(rng);
                    case 1: return near(rng);
                    default: return tiny(rng);
                }
            };
            points.x.push_back(pick());
            points.y.push_back(pick());
            points.z.push_back(pick());
        }
        return points;
    }

    std::vector<float> extremes() {
        const float inf = std::numeric_limits<float>::infinity();
        return {1e30f, -1e30f, std::numeric_limits<float>::quiet_NaN(), inf, -inf, 1e-40f, -1e-40f, 0.0f, -0.0f, 65536.0f, -65536.0f, 3e38f, -3e38f};
    }

}

TEST(NoiseDeterminism, ScalarMatchesNativeBitForBit) {
    ASSERT_TRUE(hasScalarBackend());
    const Points points = scatteredPoints(10000);
    std::vector<float> native(points.size()), scalar(points.size());
    for (const Named& kernel : everyKernel()) {
        basisBatch(kernel.params, points.x.data(), points.y.data(), points.z.data(), points.size(), native.data(), Backend::Native);
        basisBatch(kernel.params, points.x.data(), points.y.data(), points.z.data(), points.size(), scalar.data(), Backend::Scalar);
        for (size_t i = 0; i < points.size(); i++) {
            ASSERT_EQ(bitsOf(native[i]), bitsOf(scalar[i])) << kernel.name << " at " << points.x[i] << " " << points.y[i] << " " << points.z[i];
        }
    }
}

// N2: no code sets a flush mode, and denormals survive arithmetic, so VFP and arm64 keep what they compute
TEST(NoiseDeterminism, FloatingPointEnvironmentIsDefault) {
    EXPECT_EQ(std::fegetround(), FE_TONEAREST);
    volatile float small = 1e-40f;
    volatile float half = 0.5f;
    const float product = small * half;
    EXPECT_NE(product, 0.0f) << "denormals are flushed in this thread";
    EXPECT_EQ(bitsOf(simd::Native::lane(simd::Native::mul(simd::Native::set1(1e-40f), simd::Native::set1(0.5f)), 0)), bitsOf(product))
        << "the SIMD unit flushes denormals: this back end cannot match Scalar";
}

TEST(NoiseKernels, SinglePointMatchesBatch) {
    const Points points = scatteredPoints(2000);
    std::vector<float> batch(points.size());
    for (const Named& kernel : everyKernel()) {
        basisBatch(kernel.params, points.x.data(), points.y.data(), points.z.data(), points.size(), batch.data());
        for (size_t i = 0; i < points.size(); i++) {
            ASSERT_EQ(bitsOf(basisPoint(kernel.params, points.x[i], points.y[i], points.z[i])), bitsOf(batch[i])) << kernel.name;
        }
    }
    // The named single-point functions are those same kernels
    EXPECT_EQ(bitsOf(perlin2(1.3f, -2.7f, 9u)), bitsOf(basisPoint(BasisParams{Basis::Perlin, 2, 1.0f, 9u}, 1.3f, 0.0f, -2.7f)));
    EXPECT_EQ(bitsOf(simplex3(1.3f, 4.1f, -2.7f, 9u)), bitsOf(basisPoint(BasisParams{Basis::Simplex, 3, 1.0f, 9u}, 1.3f, 4.1f, -2.7f)));
}

TEST(NoiseKernels, ChunkingDoesNotChangeBits) {
    const Points points = scatteredPoints(97);
    std::vector<float> whole(points.size()), split(points.size());
    for (const Named& kernel : everyKernel()) {
        basisBatch(kernel.params, points.x.data(), points.y.data(), points.z.data(), points.size(), whole.data());
        for (size_t offset = 0; offset < 8; offset++) {
            basisBatch(kernel.params, points.x.data(), points.y.data(), points.z.data(), offset, split.data());
            basisBatch(kernel.params, points.x.data() + offset, points.y.data() + offset, points.z.data() + offset, points.size() - offset, split.data() + offset);
            for (size_t i = 0; i < points.size(); i++) {
                ASSERT_EQ(bitsOf(whole[i]), bitsOf(split[i])) << kernel.name << " split at " << offset << ", point " << i;
            }
        }
    }
}

TEST(NoiseKernels, ExtremeInputsStayDefined) {
    const std::vector<float> values = extremes();
    std::vector<float> x, y, z;
    for (float a : values) {
        for (float b : values) {
            for (float c : values) {
                x.push_back(a);
                y.push_back(b);
                z.push_back(c);
            }
        }
    }
    std::vector<float> out(x.size());
    for (const Named& kernel : everyKernel()) {
        for (Backend backend : {Backend::Native, Backend::Scalar}) {
            basisBatch(kernel.params, x.data(), y.data(), z.data(), x.size(), out.data(), backend);
            for (size_t i = 0; i < out.size(); i++) {
                ASSERT_TRUE(std::isfinite(out[i])) << kernel.name << " at " << x[i] << " " << y[i] << " " << z[i];
            }
        }
    }
    // A non-finite coordinate is coordinate 0 (R6)
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(bitsOf(perlin2(nan, 0.25f, 3u)), bitsOf(perlin2(0.0f, 0.25f, 3u)));
    EXPECT_EQ(bitsOf(value3(0.25f, std::numeric_limits<float>::infinity(), 0.5f, 3u)), bitsOf(value3(0.25f, 0.0f, 0.5f, 3u)));
    // The wrap
    EXPECT_EQ(bitsOf(value2(65536.0f + 3.25f, 0.5f, 3u)), bitsOf(value2(3.25f, 0.5f, 3u)));
    EXPECT_EQ(bitsOf(value2(-65536.0f + 3.25f, 0.5f, 3u)), bitsOf(value2(3.25f, 0.5f, 3u)));
}

// R8, from a slow reference written here
TEST(NoiseKernels, FbmDefinition) {
    for (Basis basis : {Basis::Perlin, Basis::Simplex, Basis::Value, Basis::Cellular}) {
        FractalParams params;
        params.basis.basis = basis;
        params.basis.frequency = 1.3f;
        params.basis.seed = 5u;
        params.octaves = 6;
        params.lacunarity = 2.1f;
        params.gain = 0.45f;
        for (float x : {0.25f, 17.5f, -3.75f}) {
            float weight = 1.0f, weightSum = 0.0f, sum = 0.0f, frequency = params.basis.frequency;
            BasisParams octave = params.basis;
            for (int i = 0; i < params.octaves; i++) {
                octave.seed = params.basis.seed + static_cast<uint32_t>(i) * 1013u;
                octave.frequency = frequency;
                sum = sum + basisPoint(octave, x, 0.0f, x * 0.5f) * weight;
                weightSum = weightSum + weight;
                weight = weight * params.gain;
                frequency = frequency * params.lacunarity;
            }
            EXPECT_EQ(bitsOf(fbmPoint(params, x, 0.0f, x * 0.5f)), bitsOf(sum / weightSum)) << static_cast<int>(basis) << " " << x;
        }
    }
}

TEST(NoiseKernels, RidgedDefinition) {
    FractalParams params;
    params.basis.basis = Basis::Perlin;
    params.basis.frequency = 1.3f;
    params.basis.seed = 5u;
    params.octaves = 5;
    params.lacunarity = 2.0f;
    params.gain = 0.5f;
    for (float x : {0.25f, 17.5f, -3.75f, 100.125f}) {
        float weight = 1.0f, weightSum = 0.0f, sum = 0.0f, previous = 1.0f, frequency = params.basis.frequency;
        BasisParams octave = params.basis;
        for (int i = 0; i < params.octaves; i++) {
            octave.seed = params.basis.seed + static_cast<uint32_t>(i) * 1013u;
            octave.frequency = frequency;
            const float inverted = 1.0f - std::fabs(basisPoint(octave, x, 0.0f, x * 0.5f));
            const float r = inverted * inverted;
            sum = sum + r * previous * weight;
            previous = r;
            weightSum = weightSum + weight;
            weight = weight * params.gain;
            frequency = frequency * params.lacunarity;
        }
        const float got = ridgedPoint(params, x, 0.0f, x * 0.5f);
        EXPECT_EQ(bitsOf(got), bitsOf(sum / weightSum)) << x;
        EXPECT_GE(got, 0.0f);
        EXPECT_LE(got, 1.0f);
    }
}

TEST(NoiseDetMath, CloseToLibm) {
    typedef detmath::DetMath<Lane1> M;
    float worstSin = 0.0f, worstCos = 0.0f, worstTan = 0.0f, worstExp = 0.0f, worstLog = 0.0f, worstPow = 0.0f;
    for (int i = -6000; i <= 6000; i++) {
        const float angle = static_cast<float>(i) * 0.001f;
        worstSin = std::max(worstSin, std::fabs(M::sin(angle) - std::sin(angle)));
        worstCos = std::max(worstCos, std::fabs(M::cos(angle) - std::cos(angle)));
        if (std::fabs(std::cos(angle)) > 0.2f) worstTan = std::max(worstTan, std::fabs(M::tan(angle) - std::tan(angle)));
    }
    for (int i = -2000; i <= 2000; i++) {
        const float e = static_cast<float>(i) * 0.01f;
        worstExp = std::max(worstExp, std::fabs(M::exp2(e) / std::exp2(e) - 1.0f));
    }
    for (int i = 1; i <= 20000; i++) {
        const float v = static_cast<float>(i) * 0.01f;
        worstLog = std::max(worstLog, std::fabs(M::log2(v) - std::log2(v)));
        worstPow = std::max(worstPow, std::fabs(M::pow(v, 1.7f) / std::pow(v, 1.7f) - 1.0f));
    }
    EXPECT_LT(worstSin, 5e-6f);
    EXPECT_LT(worstCos, 5e-6f);
    EXPECT_LT(worstTan, 5e-5f);
    EXPECT_LT(worstExp, 1e-5f);
    EXPECT_LT(worstLog, 1e-5f);
    EXPECT_LT(worstPow, 5e-5f);
    // R6's rules
    EXPECT_EQ(M::pow(-2.0f, 0.5f), 0.0f);
    EXPECT_EQ(M::log2(-1.0f), 0.0f);
    EXPECT_EQ(M::log2(0.0f), 0.0f);
    EXPECT_EQ(M::pow(0.0f, 2.0f), 0.0f);
    EXPECT_EQ(M::pow(3.0f, 0.0f), 1.0f);
    EXPECT_TRUE(std::isfinite(M::exp2(1e30f)));
    EXPECT_TRUE(std::isfinite(M::sin(1e30f)));
}

// One lane and four lanes are one formula: pinned by the table on every target
TEST(NoiseDetMath, Golden) {
    typedef detmath::DetMath<Lane1> One;
    typedef detmath::DetMath<simd::Scalar> Four;
    for (int i = -500; i <= 500; i++) {
        const float v = static_cast<float>(i) * 0.137f;
        simd::Scalar::f4 four = simd::Scalar::set1(v);
        EXPECT_EQ(bitsOf(One::sin(v)), bitsOf(simd::Scalar::lane(Four::sin(four), 0)));
        EXPECT_EQ(bitsOf(One::tan(v)), bitsOf(simd::Scalar::lane(Four::tan(four), 2)));
        EXPECT_EQ(bitsOf(One::exp2(v)), bitsOf(simd::Scalar::lane(Four::exp2(four), 1)));
        EXPECT_EQ(bitsOf(One::log2(v)), bitsOf(simd::Scalar::lane(Four::log2(four), 3)));
    }
    size_t count = 0;
    const golden::Expected* table = golden::expectedTable(count);
    bool found = false;
    for (size_t i = 0; i < count; i++) found = found || std::string(table[i].name) == "detmath/pow";
    EXPECT_TRUE(found);
}

// AC3: fails when a case is missing from the table
TEST(NoiseGolden, AllCasesMatch) {
    const std::vector<golden::Preset> presets = embeddedPresets();
    size_t count = 0;
    const golden::Expected* table = golden::expectedTable(count);
    for (Backend backend : {Backend::Native, Backend::Scalar}) {
        const auto cases = golden::computeCases(backend, presets);
        ASSERT_FALSE(cases.empty());
        for (const auto& entry : cases) {
            const golden::Expected* expected = nullptr;
            for (size_t i = 0; i < count; i++) {
                if (entry.first == table[i].name) expected = &table[i];
            }
            ASSERT_NE(expected, nullptr) << entry.first << " is not in the table";
            EXPECT_EQ(entry.second, expected->hash) << entry.first << " on " << (backend == Backend::Native ? nativeBackendName() : "scalar");
        }
        for (size_t i = 0; i < count; i++) {
            bool computed = false;
            for (const auto& entry : cases) computed = computed || entry.first == table[i].name;
            EXPECT_TRUE(computed) << table[i].name << " is in the table but nothing computes it";
        }
    }
}

TEST(NoiseGoldenTool, ReportFormat) {
    const std::string line = golden::reportLine("linux-x86_64", "sse2", 142, true);
    char target[32], backend[32], verdict[8], table[32];
    int cases = 0;
    ASSERT_EQ(std::sscanf(line.c_str(), "noise-golden %31s %31s %d %7s table=%31s", target, backend, &cases, verdict, table), 5) << line;
    EXPECT_STREQ(target, "linux-x86_64");
    EXPECT_STREQ(backend, "sse2");
    EXPECT_EQ(cases, 142);
    EXPECT_STREQ(verdict, "pass");
    EXPECT_EQ(std::string(table).size(), 16u);
    EXPECT_NE(golden::reportLine("linux-x86_64", "sse2", 142, false).find(" FAIL "), std::string::npos);
}
