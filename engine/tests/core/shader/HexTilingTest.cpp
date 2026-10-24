// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The hex lattice of includes/hex_tiling.glsl (docs/specs/S11-materials-lighting.md AC9),
// compiled as C++: the block between HEX_LATTICE_BEGIN and HEX_LATTICE_END, extracted by
// engine/tests/core/CMakeLists.txt, over a shim of the GLSL types it uses. The shader runs
// these very lines. CPU only.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>

namespace hexglsl {

    using uint = unsigned int;

    struct vec2{
        float x = 0, y = 0;
        vec2() = default;
        vec2(float x, float y): x(x), y(y) {}
    };
    struct vec3{
        float x = 0, y = 0, z = 0;
        vec3() = default;
        vec3(float x, float y, float z): x(x), y(y), z(z) {}
    };
    struct ivec2{
        int x = 0, y = 0;
        ivec2() = default;
        ivec2(int x, int y): x(x), y(y) {}
        bool operator==(const ivec2& o) const { return x == o.x && y == o.y; }
        bool operator!=(const ivec2& o) const { return !(*this == o); }
    };
    struct uvec2{
        uint x = 0, y = 0;
        uvec2() = default;
        uvec2(uint x, uint y): x(x), y(y) {}
    };

    inline vec2 operator*(vec2 a, float s){ return vec2(a.x * s, a.y * s); }
    inline vec2 operator+(vec2 a, vec2 b){ return vec2(a.x + b.x, a.y + b.y); }
    inline vec2 operator-(vec2 a, vec2 b){ return vec2(a.x - b.x, a.y - b.y); }
    inline vec3 operator*(vec3 a, float s){ return vec3(a.x * s, a.y * s, a.z * s); }
    inline uvec2 operator*(uvec2 a, uint s){ return uvec2(a.x * s, a.y * s); }
    inline uvec2 operator+(uvec2 a, uint s){ return uvec2(a.x + s, a.y + s); }
    inline uvec2 operator^(uvec2 a, uvec2 b){ return uvec2(a.x ^ b.x, a.y ^ b.y); }
    inline uvec2 operator>>(uvec2 a, uint s){ return uvec2(a.x >> s, a.y >> s); }

    using std::cos;
    using std::floor;
    using std::sin;
    inline float pow(float a, float b){ return std::pow(a, b); }
    inline float max(float a, float b){ return a > b ? a : b; }

#include "hex_lattice_block.inl"

}

using namespace hexglsl;

namespace {

    // points over many tiles, near the origin and far from it (fp32 ids of millions of tiles)
    std::vector<vec2> samplePoints(){
        std::mt19937 random(1234);
        std::uniform_real_distribution<float> near(-8.0f, 8.0f);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<vec2> points;
        for (int i = 0; i < 4000; i++)
            points.push_back(vec2(near(random), near(random)));
        for (float origin : {-3000.0f, 1000.0f, 4096.0f}){
            for (int i = 0; i < 500; i++)
                points.push_back(vec2(origin + unit(random), -origin + unit(random)));
        }
        return points;
    }

}

TEST(HexTiling, WeightsSumToOne){
    for (const vec2& uv : samplePoints()){
        const HexSimplex simplex = hexSimplex(uv);
        const float sum = simplex.w.x + simplex.w.y + simplex.w.z;
        ASSERT_NEAR(sum, 1.0f, 1e-5f) << "at (" << uv.x << ", " << uv.y << ")";
        ASSERT_GE(simplex.w.x, -1e-5f);
        ASSERT_GE(simplex.w.y, -1e-5f);
        ASSERT_GE(simplex.w.z, -1e-5f);
    }
}

TEST(HexTiling, OneVertexPerColour){
    for (const vec2& uv : samplePoints()){
        const HexSimplex simplex = hexSimplex(uv);
        ASSERT_EQ(hexColor(simplex.v0), 0) << "at (" << uv.x << ", " << uv.y << ")";
        ASSERT_EQ(hexColor(simplex.v1), 1);
        ASSERT_EQ(hexColor(simplex.v2), 2);
    }
    // the colouring itself, for ids whose difference does not fit an int
    EXPECT_EQ(hexColor(ivec2(INT32_MIN, INT32_MAX)), (int)((((int64_t)INT32_MIN - INT32_MAX) % 3 + 3) % 3));
    EXPECT_EQ(hexColor(ivec2(INT32_MAX, INT32_MIN)), (int)((((int64_t)INT32_MAX - INT32_MIN) % 3 + 3) % 3));
    EXPECT_EQ(hexColor(ivec2(-7, 5)), (int)(((-12 % 3) + 3) % 3));
}

TEST(HexTiling, ChangingSlotHasNoWeight){
    // across a triangle edge only the slot whose vertex changes may change, with no weight
    // there: a tap never jumps where it counts. Lines in random directions are walked in steps
    // of a hundredth of a cell; each edge crossed is bisected to 1e-6 of a cell, and the slots
    // on its two sides compared.
    std::mt19937 random(99);
    std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);
    const float cell = 1.0f / HEX_DENSITY;
    const auto same = [](const HexSimplex& a, const HexSimplex& b){
        return a.v0 == b.v0 && a.v1 == b.v1 && a.v2 == b.v2;
    };
    int crossings = 0;
    // near the origin (far away float precision, not the lattice, limits how close to an edge a
    // point can be)
    const std::vector<vec2> points = samplePoints();
    for (size_t n = 0; n < 1000; n++){
        const vec2 start = points[n];
        const float a = angle(random);
        const vec2 dir = vec2(std::cos(a), std::sin(a));
        for (int i = 0; i < 300; i++){
            vec2 p = start + dir * (cell * 0.01f * float(i));
            vec2 q = p + dir * (cell * 0.01f);
            if (same(hexSimplex(p), hexSimplex(q)))
                continue;
            for (int k = 0; k < 24; k++){
                const vec2 mid = (p + q) * 0.5f;
                if (same(hexSimplex(p), hexSimplex(mid))) p = mid; else q = mid;
            }
            const HexSimplex s0 = hexSimplex(p);
            const HexSimplex s1 = hexSimplex(q);
            const ivec2 v0[3] = {s0.v0, s0.v1, s0.v2};
            const ivec2 v1[3] = {s1.v0, s1.v1, s1.v2};
            const float w0[3] = {s0.w.x, s0.w.y, s0.w.z};
            const float w1[3] = {s1.w.x, s1.w.y, s1.w.z};
            int changed = 0;
            for (int k = 0; k < 3; k++){
                if (v0[k] != v1[k]){
                    changed++;
                    EXPECT_LT(std::fabs(w0[k]), 1e-4f) << "slot " << k << " at (" << p.x << ", " << p.y << ")";
                    EXPECT_LT(std::fabs(w1[k]), 1e-4f) << "slot " << k << " at (" << q.x << ", " << q.y << ")";
                }
            }
            // a vertex of the lattice crossed exactly can change two; an edge changes one
            EXPECT_LE(changed, 2);
            crossings++;
        }
    }
    EXPECT_GT(crossings, 100) << "the walks cross edges (the check ran)";
}

TEST(HexTiling, HashIsDeterministicForAnyId){
    // the same id hashes alike every time, also negative and huge ones, into 0..1
    const ivec2 ids[] = {ivec2(0, 0), ivec2(-1, -1), ivec2(-5, 3), ivec2(1 << 30, -(1 << 30)), ivec2(-(1 << 30), 1 << 30),
                         ivec2(INT32_MAX, INT32_MIN), ivec2(123456, -654321)};
    for (const ivec2& id : ids){
        const uvec2 first = hexHash(id);
        const uvec2 second = hexHash(id);
        EXPECT_EQ(first.x, second.x);
        EXPECT_EQ(first.y, second.y);
        const vec2 offset = hexOffset(id);
        EXPECT_GE(offset.x, 0.0f);
        EXPECT_LT(offset.x, 1.0f);
        EXPECT_GE(offset.y, 0.0f);
        EXPECT_LT(offset.y, 1.0f);
        const vec2 cs = hexRotation(id, 1.0f);
        EXPECT_NEAR(cs.x * cs.x + cs.y * cs.y, 1.0f, 1e-5f);
    }
    // neighbouring tiles differ (the hash mixes both coordinates)
    EXPECT_NE(hexHash(ivec2(1 << 30, 0)).x, hexHash(ivec2(1 << 30, 1)).x);
    EXPECT_NE(hexHash(ivec2(-(1 << 30), 5)).y, hexHash(ivec2(-(1 << 30) + 1, 5)).y);
    // no rotation at 0
    const vec2 none = hexRotation(ivec2(17, -3), 0.0f);
    EXPECT_FLOAT_EQ(none.x, 1.0f);
    EXPECT_FLOAT_EQ(none.y, 0.0f);
}

TEST(HexTiling, WeightsFollowContrast){
    const vec3 barycentrics(0.6f, 0.3f, 0.1f);
    // no contrast: the barycentrics to the exponent, normalized
    const vec3 plain = hexWeights(barycentrics, vec3(1.0f, 0.0f, 0.0f), 0.0f);
    const float sum = std::pow(0.6f, HEX_WEIGHT_EXPONENT) + std::pow(0.3f, HEX_WEIGHT_EXPONENT) + std::pow(0.1f, HEX_WEIGHT_EXPONENT);
    EXPECT_NEAR(plain.x, std::pow(0.6f, HEX_WEIGHT_EXPONENT) / sum, 1e-6f);
    EXPECT_NEAR(plain.x + plain.y + plain.z, 1.0f, 1e-6f);
    // full contrast: a black tap gets no weight
    const vec3 dark = hexWeights(barycentrics, vec3(1.0f, 0.0f, 1.0f), 1.0f);
    EXPECT_NEAR(dark.y, 0.0f, 1e-6f);
    EXPECT_NEAR(dark.x + dark.y + dark.z, 1.0f, 1e-6f);
}
