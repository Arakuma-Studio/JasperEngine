// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 R20 and Deviation 28's iridescence fix: Title Lit's thin film (titleart/title_iridescence.glsl,
// three.js's evalIridescence, which Pachislop's looks are made with) compiled as C++ over a shim of
// the GLSL it uses, so the shader runs these very lines. Obsidian's face (Pachislop f-3d-obsidian:
// a black dielectric base, F0 0.04, under the default 1.6 film at 520 nm) reflects purple: green is
// its weakest channel head on and at a slant, and Title Lit's F0 (Schlick_to_F0 of the film) keeps
// that. Red control: the engine's film (includes/iridescence.glsl, the base's reflectance against
// air, no phase jump), which Title Lit used before the fix, reflects green there. CPU only.

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>

namespace tlglsl {

    struct vec3 {
        float x = 0, y = 0, z = 0;
        vec3() = default;
        explicit vec3(double s) : x((float)s), y((float)s), z((float)s) {}
        vec3(double x, double y, double z) : x((float)x), y((float)y), z((float)z) {}
        vec3& operator+=(vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
        vec3& operator*=(vec3 o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
        vec3& operator/=(double s) { x /= (float)s; y /= (float)s; z /= (float)s; return *this; }
    };
    inline vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
    inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
    inline vec3 operator/(vec3 a, vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
    inline vec3 operator+(vec3 a, double s) { return {a.x + s, a.y + s, a.z + s}; }
    inline vec3 operator*(vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    inline vec3 operator*(double s, vec3 a) { return a * s; }
    inline vec3 operator/(vec3 a, double s) { return {a.x / s, a.y / s, a.z / s}; }
    inline vec3 sqrt(vec3 v) { return {std::sqrt(v.x), std::sqrt(v.y), std::sqrt(v.z)}; }
    inline vec3 cos(vec3 v) { return {std::cos(v.x), std::cos(v.y), std::cos(v.z)}; }
    inline vec3 exp(vec3 v) { return {std::exp(v.x), std::exp(v.y), std::exp(v.z)}; }
    inline vec3 pow(vec3 v, double e) { return {std::pow(v.x, e), std::pow(v.y, e), std::pow(v.z, e)}; }
    inline vec3 max(vec3 a, vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
    inline vec3 clamp(vec3 v, double lo, double hi) {
        return {std::clamp((double)v.x, lo, hi), std::clamp((double)v.y, lo, hi), std::clamp((double)v.z, lo, hi)};
    }
    inline vec3 clamp(vec3 v, vec3 lo, vec3 hi) {
        return {std::clamp(v.x, lo.x, hi.x), std::clamp(v.y, lo.y, hi.y), std::clamp(v.z, lo.z, hi.z)};
    }
    inline float sqrt(float v) { return std::sqrt(v); }
    inline float cos(float v) { return std::cos(v); }
    inline float exp(float v) { return std::exp(v); }
    inline float pow(float v, double e) { return (float)std::pow(v, e); }
    inline float max(float a, double b) { return std::max(a, (float)b); }
    inline float clamp(float v, double lo, double hi) { return std::clamp(v, (float)lo, (float)hi); }
    inline float mix(double a, double b, float t) { return (float)(a + (b - a) * t); }
    inline float smoothstep(double e0, double e1, float x) {
        const float t = std::clamp((x - (float)e0) / (float)(e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    // GLSL's column-major mat3 of nine scalars
    struct mat3 {
        float m[9];
        mat3(double a, double b, double c, double d, double e, double f, double g, double h, double i)
            : m{(float)a, (float)b, (float)c, (float)d, (float)e, (float)f, (float)g, (float)h, (float)i} {}
    };
    inline vec3 operator*(const mat3& a, vec3 v) {
        return {a.m[0] * v.x + a.m[3] * v.y + a.m[6] * v.z, a.m[1] * v.x + a.m[4] * v.y + a.m[7] * v.z, a.m[2] * v.x + a.m[5] * v.y + a.m[8] * v.z};
    }

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
// `precision highp float;` declares nothing in C++
#define precision using tlglslPrecision =
#define highp
#include "../../../../shaderlib/includes/iridescence.glsl"
#include "../../../../shaderlib/titleart/title_iridescence.glsl"
#undef highp
#undef precision

}

using namespace tlglsl;

namespace {

    // obsidian's face: a dielectric base (IOR 1.5, F0 0.04) under the default film (IOR 1.6,
    // thickness 520 nm: the KHR rule without a map); Title Lit's iridescence factor 0.6
    constexpr float FILM_IOR = 1.6f, THICKNESS = 520.0f, FACTOR = 0.6f;
    const vec3 BASE_F0(0.04);

    // the F0 Title Lit lights obsidian's face with (title_lit.frag's mix by the factor)
    vec3 titleLitF0(float cosTheta) {
        const vec3 film = tlSchlickToF0(tlIridescenceFresnel(cosTheta, FILM_IOR, THICKNESS, BASE_F0), 1.0f, cosTheta);
        return BASE_F0 + (film - BASE_F0) * FACTOR;
    }

    bool purple(vec3 c) { return c.y < std::min(c.x, c.z); }
    bool green(vec3 c) { return c.y > std::max(c.x, c.z); }

}

TEST(TitleIridescence, ObsidianReflectsPurple) {
    for (float cosTheta : {1.0f, 0.9f, 0.7f}) {
        SCOPED_TRACE(cosTheta);
        const vec3 film = tlIridescenceFresnel(cosTheta, FILM_IOR, THICKNESS, BASE_F0);
        EXPECT_TRUE(purple(film)) << film.x << " " << film.y << " " << film.z;
        const vec3 f0 = titleLitF0(cosTheta);
        EXPECT_TRUE(purple(f0)) << f0.x << " " << f0.y << " " << f0.z;
        for (float c : {film.x, film.y, film.z}) {
            EXPECT_GE(c, 0.0f);
            EXPECT_LE(c, 1.0f);
        }
    }
}

TEST(TitleIridescence, TheEngineFilmReflectsGreenThere) {
    // red control: the film Title Lit used before the fix (the base against air, no phase jump)
    for (float cosTheta : {1.0f, 0.9f}) {
        SCOPED_TRACE(cosTheta);
        const vec3 film = iridescenceFresnel(cosTheta, FILM_IOR, THICKNESS, BASE_F0);
        EXPECT_TRUE(green(film)) << film.x << " " << film.y << " " << film.z;
    }
}
