// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 AC18 and R30: the 3D post's tone maps (titleart/title_tonemap.glsl) and light encodings
// (titleart/hdr_compress.glsl, the AO prepass's RGBA8 packing included), compiled as C++ over a shim of the GLSL they use, so the shader
// runs these very lines. The ACES Filmic and AgX references are three.js's formulas evaluated in
// double precision by a separate script (the values below); Neutral is checked against the
// scene's PostColorLut. CPU only.

#include "gtest/gtest.h"

#include "util/PostColorLut.h"

#include <algorithm>
#include <cmath>

namespace tlglsl {

    struct vec3 {
        float x = 0, y = 0, z = 0;
        vec3() = default;
        explicit vec3(float s) : x(s), y(s), z(s) {}
        explicit vec3(double s) : x((float)s), y((float)s), z((float)s) {}
        vec3(float x, float y, float z) : x(x), y(y), z(z) {}
        vec3(double x, double y, double z) : x((float)x), y((float)y), z((float)z) {}
    };
    struct vec4 {
        float x = 0, y = 0, z = 0, w = 0;
        vec4() = default;
        explicit vec4(double s) : x((float)s), y((float)s), z((float)s), w((float)s) {}
        vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    };
    inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
    inline vec3 operator/(vec3 a, vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
    inline vec3 operator*(vec3 a, double s) { return {a.x * (float)s, a.y * (float)s, a.z * (float)s}; }
    inline vec3 operator/(vec3 a, double s) { return {a.x / (float)s, a.y / (float)s, a.z / (float)s}; }
    inline vec3 max(vec3 a, vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
    inline vec3 clamp(vec3 v, vec3 lo, vec3 hi) {
        return {std::clamp(v.x, lo.x, hi.x), std::clamp(v.y, lo.y, hi.y), std::clamp(v.z, lo.z, hi.z)};
    }
    inline vec3 log2(vec3 v) { return {std::log2(v.x), std::log2(v.y), std::log2(v.z)}; }
    inline vec3 pow(vec3 v, vec3 e) { return {std::pow(v.x, e.x), std::pow(v.y, e.y), std::pow(v.z, e.z)}; }
    inline vec3 mix(vec3 a, vec3 b, double t) { return a + (b - a) * t; }
    inline float max(float a, float b) { return std::max(a, b); }
    inline float min(float a, float b) { return std::min(a, b); }
    inline float min(float a, double b) { return std::min(a, (float)b); }
    inline float max(double a, float b) { return std::max((float)a, b); }
    inline float clamp(float v, double lo, double hi) { return std::clamp(v, (float)lo, (float)hi); }
    inline float abs(float v) { return std::fabs(v); }
    inline float floor(double v) { return (float)std::floor(v); }
    inline float sqrt(float v) { return std::sqrt(v); }
    inline float smoothstep(double e0, double e1, float x) {
        const float t = std::clamp((x - (float)e0) / (float)(e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

#include "../../../../shaderlib/titleart/title_tonemap.glsl"
#include "../../../../shaderlib/titleart/hdr_compress.glsl"

}

using namespace tlglsl;

namespace {

    struct Ref {
        float in[3];
        float aces[3];
        float agx[3];
    };
    const Ref REFS[] = {
        {{0.0f, 0.0f, 0.0f}, {0.0000000f, 0.0000000f, 0.0000000f}, {0.0000000f, 0.0000000f, 0.0000000f}},
        {{0.05f, 0.05f, 0.05f}, {0.0316811f, 0.0316811f, 0.0316808f}, {0.0613825f, 0.0613660f, 0.0613675f}},
        {{0.18f, 0.18f, 0.18f}, {0.2131054f, 0.2131054f, 0.2131033f}, {0.2145486f, 0.2145024f, 0.2144992f}},
        {{0.5f, 0.25f, 0.1f}, {0.5679585f, 0.3206169f, 0.1250164f}, {0.4369136f, 0.2720101f, 0.1551523f}},
        {{1.0f, 1.0f, 1.0f}, {0.7633974f, 0.7633974f, 0.7633898f}, {0.5902294f, 0.5901361f, 0.5901023f}},
        {{2.0f, 1.0f, 0.5f}, {0.9091400f, 0.7719614f, 0.6122296f}, {0.7656451f, 0.5908922f, 0.4645145f}},
        {{0.9f, 0.1f, 0.05f}, {0.8691019f, 0.1408123f, 0.0578603f}, {0.6456987f, 0.1863963f, 0.1198974f}},
        {{4.0f, 4.0f, 4.0f}, {0.9522369f, 0.9522369f, 0.9522273f}, {0.8610164f, 0.8609102f, 0.8608389f}},
        {{10.0f, 5.0f, 1.0f}, {1.0000000f, 0.9672921f, 0.8652050f}, {0.9747972f, 0.8885914f, 0.7392520f}},
        {{0.02f, 0.3f, 0.8f}, {0.0467125f, 0.3746715f, 0.6965164f}, {0.1108831f, 0.3476877f, 0.5793507f}},
        {{60.0f, 40.0f, 10.0f}, {1.0000000f, 1.0000000f, 0.9992382f}, {0.9969783f, 0.9968787f, 0.9967790f}},
        {{0.3f, 0.6f, 0.2f}, {0.4255752f, 0.6170890f, 0.2991295f}, {0.3368332f, 0.4533981f, 0.2558055f}},
    };

}

TEST(TitleToneMapTest, AcesFilmicAndAgXMatchThreeJs) {
    for (const Ref& r : REFS) {
        const vec3 c(r.in[0], r.in[1], r.in[2]);
        const vec3 a = tl_toneMap(c, 1), g = tl_toneMap(c, 2);
        EXPECT_NEAR(a.x, r.aces[0], 1e-4);
        EXPECT_NEAR(a.y, r.aces[1], 1e-4);
        EXPECT_NEAR(a.z, r.aces[2], 1e-4);
        EXPECT_NEAR(g.x, r.agx[0], 1e-4);
        EXPECT_NEAR(g.y, r.agx[1], 1e-4);
        EXPECT_NEAR(g.z, r.agx[2], 1e-4);
    }
}

TEST(TitleToneMapTest, NeutralEqualsTheSceneNeutral) {
    for (const Ref& r : REFS) {
        const vec3 n = tl_toneMap(vec3(r.in[0], r.in[1], r.in[2]), 3);
        const jasperengine::Vector3 s = jasperengine::PostColorLut::toneMap(jasperengine::TonemapMode::NEUTRAL,
                                                                           jasperengine::Vector3(r.in[0], r.in[1], r.in[2]));
        EXPECT_NEAR(n.x, s.x, 1e-4);
        EXPECT_NEAR(n.y, s.y, 1e-4);
        EXPECT_NEAR(n.z, s.z, 1e-4);
    }
}

TEST(TitleToneMapTest, PunchKeepsHueAboveTheKnee) {
    // below the knee: the colour minus Neutral's toe
    const vec3 low(0.5f, 0.3f, 0.2f);
    const vec3 p = tl_toneMap(low, 4);
    EXPECT_NEAR(p.x, 0.5f - 0.04f, 1e-6);
    EXPECT_NEAR(p.z, 0.2f - 0.04f, 1e-6);
    // above it the channel ratios stay (after the toe)
    for (float scale : {1.0f, 2.0f, 2.9f}) {
        const vec3 c(1.0f * scale, 0.6f * scale, 0.2f * scale);
        const vec3 t = c - vec3(0.04f);
        const vec3 out = tl_toneMap(c, 4);
        EXPECT_NEAR(out.y / out.x, t.y / t.x, 1e-5) << scale;
        EXPECT_NEAR(out.z / out.x, t.z / t.x, 1e-5) << scale;
    }
    // fully white at peak 16
    const vec3 hot = tl_toneMap(vec3(16.04f, 8.0f, 2.0f), 4);
    EXPECT_NEAR(hot.x, hot.y, 1e-5);
    EXPECT_NEAR(hot.y, hot.z, 1e-5);
}

TEST(TitleToneMapTest, ShoulderAndCompressedLightInvert) {
    for (float m : {0.5f, 0.9f, 2.0f, 20.0f, 60.0f}) {
        const vec3 c(m, m * 0.5f, m * 0.25f);
        const vec3 back = hd_expand(hd_shoulder(c, 1.0f), 1.0f);
        EXPECT_NEAR(back.x / c.x, 1.0f, m > 20 ? 0.05f : 2e-3f) << m;
        // the RGBA8 encoding round trips through 8-bit codes within its precision
        const vec4 e = hd_encodeLight(vec4(c.x * 0.5f, c.y * 0.5f, c.z * 0.5f, 0.5f));
        auto q = [](float v) { return std::round(v * 255.0f) / 255.0f; };
        const vec4 d = hd_decodeLight(vec4(q(e.x), q(e.y), q(e.z), q(e.w)));
        // up to 20 x white within its 8-bit precision; above, it stays far above white (the bright
        // pass and the tone map need no more)
        if (m <= 20) EXPECT_NEAR(d.x / 0.5f, c.x, c.x * (m >= 20 ? 0.25f : 0.03f) + 0.01f) << m;
        else EXPECT_GT(d.x / 0.5f, 20.0f) << m;
        // red control: reading the encoding as plain colour loses the highlight
        if (m >= 2) { EXPECT_LT(e.x / 0.5f, 1.0f); }
    }
}

// R30: the MSAA resolve averages the encoded samples. Supersampled, the fallback's linear encoding
// averages light, as the float path's resolve does where a highlight meets a dark sample (a bevel's
// glint at an edge); red control: the highlight-compressing encoding resolves 12 over 0.02 to 0.89,
// not 6.01, and the glint picks other stars
TEST(TitleToneMapTest, SupersampledFallbackResolvesToTheMeanLight) {
    auto resolve = [](vec4 (*encode)(vec4), float mode, float bright, float dark) {
        const vec4 a = encode(vec4(bright, bright * 0.5f, bright * 0.25f, 1.0f));
        const vec4 b = encode(vec4(dark, dark * 0.5f, dark * 0.25f, 1.0f));
        return hd_decode(vec4(0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z), 0.5f * (a.w + b.w)), mode);
    };
    for (float bright : {1.5f, 4.0f, 12.0f, 40.0f}) {
        for (float dark : {0.0f, 0.02f, 0.3f}) {
            const float mean = 0.5f * (bright + dark);
            const vec4 linear = resolve(hd_encodeLinear, 2.0f, bright, dark);
            EXPECT_NEAR(linear.x, mean, mean * 1e-4f) << bright << " over " << dark;
            EXPECT_NEAR(linear.z, 0.25f * mean, mean * 1e-4f) << bright << " over " << dark;
        }
    }
    EXPECT_LT(resolve(hd_encodeLight, 1.0f, 12.0f, 0.02f).x, 0.2f * 6.01f) << "the compressed encoding resolves to the mean light";
    // the linear encoding holds 3D highlights (they reach 60) and decodes plain light as mode 0
    const vec4 high = hd_decode(hd_encodeLinear(vec4(60.0f, 30.0f, 15.0f, 1.0f)), 2.0f);
    EXPECT_NEAR(high.x, 60.0f, 1e-3f);
    EXPECT_NEAR(hd_decode(vec4(0.25f, 0.5f, 0.75f, 1.0f), 0.0f).y, 0.5f, 1e-6f);
}

// R30, R31: without float targets the AO prepass packs the view normal (octahedral, 8 bits a
// component) and the title-relative depth (16 bits over +-8 radii) into RGBA8; codes round-trip
// within a degree and 1/4000 of a radius, and an empty texel (code 0) reads as no surface
TEST(TitleToneMapTest, PackedAoPrepassRoundTrips) {
    auto code = [](float v) { return std::round(v * 255.0f) / 255.0f; };
    int checked = 0;
    for (int i = 0; i < 200; i++) {
        const float z = -1.0f + 2.0f * (float)((i * 37) % 200) / 199.0f;
        const float phi = 0.7f * (float)i;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const vec4 nd(r * std::cos(phi), r * std::sin(phi), z, -7.5f + 15.0f * (float)i / 199.0f);
        const vec4 packed = hd_packNormalDepth(nd);
        const vec4 back = hd_unpackNormalDepth(vec4(code(packed.x), code(packed.y), code(packed.z), code(packed.w)));
        const float dot = back.x * nd.x + back.y * nd.y + back.z * nd.z;
        EXPECT_GT(dot, std::cos(1.5f * 3.14159265f / 180.0f)) << i;
        EXPECT_NEAR(back.w, nd.w, 16.0f / 65534.0f) << i;
        checked++;
    }
    EXPECT_EQ(checked, 200);
    const vec4 empty = hd_unpackNormalDepth(vec4(0.0f, 0.0f, 0.0f, 0.0f));
    EXPECT_EQ(empty.x * empty.x + empty.y * empty.y + empty.z * empty.z, 0.0f) << "nothing drawn reads as no surface";
    // red control: the depth's low byte dropped moves it by up to 1/16 of a radius
    const vec4 packed = hd_packNormalDepth(vec4(0.0f, 0.0f, 1.0f, 0.1234f));
    EXPECT_GT(std::abs(hd_unpackNormalDepth(vec4(code(packed.x), code(packed.y), code(packed.z), 0.0f)).w - 0.1234f), 1e-3f);
}
