// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SIMD_SIMDFLOAT4_H
#define SIMD_SIMDFLOAT4_H

// A minimal 4-wide float/int set for the particle kernels (docs/specs/S20-particles.md D4).
// Each back end is a struct of static functions over its own f4 (4 floats) and u4 (4 x 32-bit
// integers, also the lane masks: all bits set is true) types, so a kernel is a template on the
// back end and is compiled once per back end:
//
//   Scalar  four plain floats, always compiled (the reference and the runtime fallback)
//   Sse2    x86-64 baseline (no SSE4.1: floor and the 32-bit multiply are emulated)
//   Neon    arm64 and armv7 with NEON (armv7 has no vector divide or square root: per lane)
//   Wasm    WebAssembly simd128 (-msimd128)
//
// Every operation gives the same bits on every back end: no reciprocal or square-root
// estimates, no fused multiply-add (madd is a multiply then an add; particle sources build with
// -ffp-contract=off), min/max with SSE's operand order (min(a, b) = a < b ? a : b), floor exact
// for every float. So a kernel's Scalar instantiation is bit-identical to its SIMD one, which is
// what the particle determinism tests check. Native is the widest back end of the build.

#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(JASPERENGINE_SIMD_DISABLE)
    // a build without SIMD (JASPERENGINE_WASM_SIMD=OFF on the web): Native is Scalar
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define JE_SIMD_SSE2 1
    #include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
    #define JE_SIMD_NEON 1
    #include <arm_neon.h>
    #if defined(__aarch64__) || defined(_M_ARM64)
        #define JE_SIMD_NEON_A64 1
    #endif
#elif defined(__wasm_simd128__)
    #define JE_SIMD_WASM 1
    #include <wasm_simd128.h>
#endif

namespace jasperengine { namespace simd {

    // ---------------------------------------------------------------- scalar

    struct Scalar {
        static constexpr const char* name = "scalar";

        struct f4 { float v[4]; };
        struct u4 { uint32_t v[4]; };

        static f4 load(const float* p){ f4 r; r.v[0] = p[0]; r.v[1] = p[1]; r.v[2] = p[2]; r.v[3] = p[3]; return r; }
        static f4 loadu(const float* p){ return load(p); }
        static void store(float* p, f4 a){ p[0] = a.v[0]; p[1] = a.v[1]; p[2] = a.v[2]; p[3] = a.v[3]; }
        static void storeu(float* p, f4 a){ store(p, a); }
        static u4 loadu4(const uint32_t* p){ u4 r; r.v[0] = p[0]; r.v[1] = p[1]; r.v[2] = p[2]; r.v[3] = p[3]; return r; }
        static void storeu4(uint32_t* p, u4 a){ p[0] = a.v[0]; p[1] = a.v[1]; p[2] = a.v[2]; p[3] = a.v[3]; }

        static f4 set1(float a){ f4 r; r.v[0] = r.v[1] = r.v[2] = r.v[3] = a; return r; }
        static f4 set(float a, float b, float c, float d){ f4 r; r.v[0] = a; r.v[1] = b; r.v[2] = c; r.v[3] = d; return r; }
        static f4 zero(){ return set1(0.0f); }
        static u4 iset1(uint32_t a){ u4 r; r.v[0] = r.v[1] = r.v[2] = r.v[3] = a; return r; }
        static u4 iset(uint32_t a, uint32_t b, uint32_t c, uint32_t d){ u4 r; r.v[0] = a; r.v[1] = b; r.v[2] = c; r.v[3] = d; return r; }

        #define JE_SIMD_SCALAR_F2(fname, expr) static f4 fname(f4 a, f4 b){ f4 r; for (int i = 0; i < 4; i++){ const float x = a.v[i]; const float y = b.v[i]; r.v[i] = (expr); } return r; }
        JE_SIMD_SCALAR_F2(add, x + y)
        JE_SIMD_SCALAR_F2(sub, x - y)
        JE_SIMD_SCALAR_F2(mul, x * y)
        JE_SIMD_SCALAR_F2(div, x / y)
        JE_SIMD_SCALAR_F2(min, x < y ? x : y)
        JE_SIMD_SCALAR_F2(max, x > y ? x : y)
        #undef JE_SIMD_SCALAR_F2

        static f4 madd(f4 a, f4 b, f4 c){ return add(mul(a, b), c); }
        static f4 sqrt(f4 a){ f4 r; for (int i = 0; i < 4; i++) r.v[i] = std::sqrt(a.v[i]); return r; }
        static f4 abs(f4 a){ return castf(iand(castu(a), iset1(0x7fffffffu))); }
        static f4 neg(f4 a){ return castf(ixor(castu(a), iset1(0x80000000u))); }
        static f4 floor(f4 a){ f4 r; for (int i = 0; i < 4; i++) r.v[i] = std::floor(a.v[i]); return r; }

        #define JE_SIMD_SCALAR_CMP(fname, op) static u4 fname(f4 a, f4 b){ u4 r; for (int i = 0; i < 4; i++) r.v[i] = (a.v[i] op b.v[i]) ? 0xffffffffu : 0u; return r; }
        JE_SIMD_SCALAR_CMP(cmplt, <)
        JE_SIMD_SCALAR_CMP(cmple, <=)
        JE_SIMD_SCALAR_CMP(cmpgt, >)
        JE_SIMD_SCALAR_CMP(cmpge, >=)
        JE_SIMD_SCALAR_CMP(cmpeq, ==)
        JE_SIMD_SCALAR_CMP(cmpneq, !=)
        #undef JE_SIMD_SCALAR_CMP

        static f4 select(u4 m, f4 a, f4 b){ return castf(ior(iand(m, castu(a)), iandnot(m, castu(b)))); }
        static u4 iselect(u4 m, u4 a, u4 b){ return ior(iand(m, a), iandnot(m, b)); }

        #define JE_SIMD_SCALAR_I2(fname, expr) static u4 fname(u4 a, u4 b){ u4 r; for (int i = 0; i < 4; i++){ const uint32_t x = a.v[i]; const uint32_t y = b.v[i]; r.v[i] = (expr); } return r; }
        JE_SIMD_SCALAR_I2(iand, x & y)
        JE_SIMD_SCALAR_I2(ior, x | y)
        JE_SIMD_SCALAR_I2(ixor, x ^ y)
        JE_SIMD_SCALAR_I2(iandnot, ~x & y)   // (not mask) and b: iandnot(m, b) = b where m is false
        JE_SIMD_SCALAR_I2(iadd, x + y)
        JE_SIMD_SCALAR_I2(isub, x - y)
        JE_SIMD_SCALAR_I2(imul, x * y)
        JE_SIMD_SCALAR_I2(icmpeq, x == y ? 0xffffffffu : 0u)
        #undef JE_SIMD_SCALAR_I2
        static u4 icmpgt_s(u4 a, u4 b){ u4 r; for (int i = 0; i < 4; i++) r.v[i] = ((int32_t)a.v[i] > (int32_t)b.v[i]) ? 0xffffffffu : 0u; return r; }

        template<int n> static u4 isrl(u4 a){ u4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] >> n; return r; }
        template<int n> static u4 isll(u4 a){ u4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] << n; return r; }

        // float to int32, truncating (callers keep values within int32)
        static u4 cvtt(f4 a){ u4 r; for (int i = 0; i < 4; i++) r.v[i] = (uint32_t)(int32_t)a.v[i]; return r; }
        // int32 to float
        static f4 cvt(u4 a){ f4 r; for (int i = 0; i < 4; i++) r.v[i] = (float)(int32_t)a.v[i]; return r; }
        static u4 castu(f4 a){ u4 r; std::memcpy(r.v, a.v, sizeof(r.v)); return r; }
        static f4 castf(u4 a){ f4 r; std::memcpy(r.v, a.v, sizeof(r.v)); return r; }

        // bit i set when lane i's mask is true
        static uint32_t movemask(u4 m){ return (m.v[0] >> 31) | ((m.v[1] >> 31) << 1) | ((m.v[2] >> 31) << 2) | ((m.v[3] >> 31) << 3); }
        static float lane(f4 a, int i){ return a.v[i]; }
        static uint32_t ilane(u4 a, int i){ return a.v[i]; }
    };

    // ---------------------------------------------------------------- SSE2

#if defined(JE_SIMD_SSE2)
    struct Sse2 {
        static constexpr const char* name = "sse2";

        typedef __m128 f4;
        typedef __m128i u4;

        static f4 load(const float* p){ return _mm_load_ps(p); }
        static f4 loadu(const float* p){ return _mm_loadu_ps(p); }
        static void store(float* p, f4 a){ _mm_store_ps(p, a); }
        static void storeu(float* p, f4 a){ _mm_storeu_ps(p, a); }
        static u4 loadu4(const uint32_t* p){ return _mm_loadu_si128((const __m128i*)p); }
        static void storeu4(uint32_t* p, u4 a){ _mm_storeu_si128((__m128i*)p, a); }

        static f4 set1(float a){ return _mm_set1_ps(a); }
        static f4 set(float a, float b, float c, float d){ return _mm_setr_ps(a, b, c, d); }
        static f4 zero(){ return _mm_setzero_ps(); }
        static u4 iset1(uint32_t a){ return _mm_set1_epi32((int)a); }
        static u4 iset(uint32_t a, uint32_t b, uint32_t c, uint32_t d){ return _mm_setr_epi32((int)a, (int)b, (int)c, (int)d); }

        static f4 add(f4 a, f4 b){ return _mm_add_ps(a, b); }
        static f4 sub(f4 a, f4 b){ return _mm_sub_ps(a, b); }
        static f4 mul(f4 a, f4 b){ return _mm_mul_ps(a, b); }
        static f4 div(f4 a, f4 b){ return _mm_div_ps(a, b); }
        static f4 min(f4 a, f4 b){ return _mm_min_ps(a, b); }
        static f4 max(f4 a, f4 b){ return _mm_max_ps(a, b); }
        static f4 madd(f4 a, f4 b, f4 c){ return _mm_add_ps(_mm_mul_ps(a, b), c); }
        static f4 sqrt(f4 a){ return _mm_sqrt_ps(a); }
        static f4 abs(f4 a){ return _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff))); }
        static f4 neg(f4 a){ return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u))); }
        static f4 floor(f4 a){
            // truncate, minus one where that rounded up (negatives); floats of 2^23 and more are
            // whole already (and may not fit the int32 conversion)
            const f4 truncated = _mm_cvtepi32_ps(_mm_cvttps_epi32(a));
            const f4 down = _mm_sub_ps(truncated, _mm_and_ps(_mm_cmpgt_ps(truncated, a), _mm_set1_ps(1.0f)));
            // (not below 2^23: NaN too, which passes through as std::floor does)
            const f4 whole = _mm_cmpnlt_ps(abs(a), _mm_set1_ps(8388608.0f));
            // the sign bit keeps -0.0 negative
            return _mm_or_ps(_mm_and_ps(whole, a), _mm_andnot_ps(whole, _mm_or_ps(down, _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u))))));
        }

        static u4 cmplt(f4 a, f4 b){ return _mm_castps_si128(_mm_cmplt_ps(a, b)); }
        static u4 cmple(f4 a, f4 b){ return _mm_castps_si128(_mm_cmple_ps(a, b)); }
        static u4 cmpgt(f4 a, f4 b){ return _mm_castps_si128(_mm_cmpgt_ps(a, b)); }
        static u4 cmpge(f4 a, f4 b){ return _mm_castps_si128(_mm_cmpge_ps(a, b)); }
        static u4 cmpeq(f4 a, f4 b){ return _mm_castps_si128(_mm_cmpeq_ps(a, b)); }
        static u4 cmpneq(f4 a, f4 b){ return _mm_castps_si128(_mm_cmpneq_ps(a, b)); }

        static f4 select(u4 m, f4 a, f4 b){ const f4 mf = _mm_castsi128_ps(m); return _mm_or_ps(_mm_and_ps(mf, a), _mm_andnot_ps(mf, b)); }
        static u4 iselect(u4 m, u4 a, u4 b){ return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }

        static u4 iand(u4 a, u4 b){ return _mm_and_si128(a, b); }
        static u4 ior(u4 a, u4 b){ return _mm_or_si128(a, b); }
        static u4 ixor(u4 a, u4 b){ return _mm_xor_si128(a, b); }
        static u4 iandnot(u4 a, u4 b){ return _mm_andnot_si128(a, b); }
        static u4 iadd(u4 a, u4 b){ return _mm_add_epi32(a, b); }
        static u4 isub(u4 a, u4 b){ return _mm_sub_epi32(a, b); }
        static u4 imul(u4 a, u4 b){
            // the low 32 bits of each product (SSE4.1's pmulld) from two 32x32->64 multiplies
            const __m128i even = _mm_mul_epu32(a, b);
            const __m128i odd = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));
            return _mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)), _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)));
        }
        static u4 icmpeq(u4 a, u4 b){ return _mm_cmpeq_epi32(a, b); }
        static u4 icmpgt_s(u4 a, u4 b){ return _mm_cmpgt_epi32(a, b); }

        template<int n> static u4 isrl(u4 a){ return _mm_srli_epi32(a, n); }
        template<int n> static u4 isll(u4 a){ return _mm_slli_epi32(a, n); }

        static u4 cvtt(f4 a){ return _mm_cvttps_epi32(a); }
        static f4 cvt(u4 a){ return _mm_cvtepi32_ps(a); }
        static u4 castu(f4 a){ return _mm_castps_si128(a); }
        static f4 castf(u4 a){ return _mm_castsi128_ps(a); }

        static uint32_t movemask(u4 m){ return (uint32_t)_mm_movemask_ps(_mm_castsi128_ps(m)); }
        static float lane(f4 a, int i){ alignas(16) float v[4]; _mm_store_ps(v, a); return v[i]; }
        static uint32_t ilane(u4 a, int i){ alignas(16) uint32_t v[4]; _mm_store_si128((__m128i*)v, a); return v[i]; }
    };
#endif

    // ---------------------------------------------------------------- NEON

#if defined(JE_SIMD_NEON)
    struct Neon {
        static constexpr const char* name = "neon";

        typedef float32x4_t f4;
        typedef uint32x4_t u4;

        static f4 load(const float* p){ return vld1q_f32(p); }
        static f4 loadu(const float* p){ return vld1q_f32(p); }
        static void store(float* p, f4 a){ vst1q_f32(p, a); }
        static void storeu(float* p, f4 a){ vst1q_f32(p, a); }
        static u4 loadu4(const uint32_t* p){ return vld1q_u32(p); }
        static void storeu4(uint32_t* p, u4 a){ vst1q_u32(p, a); }

        static f4 set1(float a){ return vdupq_n_f32(a); }
        static f4 set(float a, float b, float c, float d){ const float v[4] = {a, b, c, d}; return vld1q_f32(v); }
        static f4 zero(){ return vdupq_n_f32(0.0f); }
        static u4 iset1(uint32_t a){ return vdupq_n_u32(a); }
        static u4 iset(uint32_t a, uint32_t b, uint32_t c, uint32_t d){ const uint32_t v[4] = {a, b, c, d}; return vld1q_u32(v); }

        static f4 add(f4 a, f4 b){ return vaddq_f32(a, b); }
        static f4 sub(f4 a, f4 b){ return vsubq_f32(a, b); }
        static f4 mul(f4 a, f4 b){ return vmulq_f32(a, b); }
#if defined(JE_SIMD_NEON_A64)
        static f4 div(f4 a, f4 b){ return vdivq_f32(a, b); }
        static f4 sqrt(f4 a){ return vsqrtq_f32(a); }
#else
        // armv7 NEON has only estimates: the exact operation per lane
        static f4 div(f4 a, f4 b){
            float x[4], y[4];
            vst1q_f32(x, a);
            vst1q_f32(y, b);
            for (int i = 0; i < 4; i++) x[i] = x[i] / y[i];
            return vld1q_f32(x);
        }
        static f4 sqrt(f4 a){
            float x[4];
            vst1q_f32(x, a);
            for (int i = 0; i < 4; i++) x[i] = std::sqrt(x[i]);
            return vld1q_f32(x);
        }
#endif
        // SSE's operand order: the second operand when either is NaN or both compare equal
        static f4 min(f4 a, f4 b){ return vbslq_f32(vcltq_f32(a, b), a, b); }
        static f4 max(f4 a, f4 b){ return vbslq_f32(vcgtq_f32(a, b), a, b); }
        // vmulq then vaddq: never vmlaq/vfmaq, which fuse or round differently
        static f4 madd(f4 a, f4 b, f4 c){ return vaddq_f32(vmulq_f32(a, b), c); }
        static f4 abs(f4 a){ return vabsq_f32(a); }
        static f4 neg(f4 a){ return vnegq_f32(a); }
        static f4 floor(f4 a){
#if defined(JE_SIMD_NEON_A64)
            return vrndmq_f32(a);
#else
            const f4 truncated = vcvtq_f32_s32(vcvtq_s32_f32(a));
            const f4 down = vsubq_f32(truncated, vreinterpretq_f32_u32(vandq_u32(vcgtq_f32(truncated, a), vreinterpretq_u32_f32(vdupq_n_f32(1.0f)))));
            const uint32x4_t whole = vmvnq_u32(vcltq_f32(vabsq_f32(a), vdupq_n_f32(8388608.0f)));
            const f4 signedDown = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(down), vandq_u32(vreinterpretq_u32_f32(a), vdupq_n_u32(0x80000000u))));
            return vbslq_f32(whole, a, signedDown);
#endif
        }

        static u4 cmplt(f4 a, f4 b){ return vcltq_f32(a, b); }
        static u4 cmple(f4 a, f4 b){ return vcleq_f32(a, b); }
        static u4 cmpgt(f4 a, f4 b){ return vcgtq_f32(a, b); }
        static u4 cmpge(f4 a, f4 b){ return vcgeq_f32(a, b); }
        static u4 cmpeq(f4 a, f4 b){ return vceqq_f32(a, b); }
        static u4 cmpneq(f4 a, f4 b){ return vmvnq_u32(vceqq_f32(a, b)); }

        static f4 select(u4 m, f4 a, f4 b){ return vbslq_f32(m, a, b); }
        static u4 iselect(u4 m, u4 a, u4 b){ return vbslq_u32(m, a, b); }

        static u4 iand(u4 a, u4 b){ return vandq_u32(a, b); }
        static u4 ior(u4 a, u4 b){ return vorrq_u32(a, b); }
        static u4 ixor(u4 a, u4 b){ return veorq_u32(a, b); }
        static u4 iandnot(u4 a, u4 b){ return vbicq_u32(b, a); }
        static u4 iadd(u4 a, u4 b){ return vaddq_u32(a, b); }
        static u4 isub(u4 a, u4 b){ return vsubq_u32(a, b); }
        static u4 imul(u4 a, u4 b){ return vmulq_u32(a, b); }
        static u4 icmpeq(u4 a, u4 b){ return vceqq_u32(a, b); }
        static u4 icmpgt_s(u4 a, u4 b){ return vcgtq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b)); }

        template<int n> static u4 isrl(u4 a){ return vshrq_n_u32(a, n); }
        template<int n> static u4 isll(u4 a){ return vshlq_n_u32(a, n); }

        static u4 cvtt(f4 a){ return vreinterpretq_u32_s32(vcvtq_s32_f32(a)); }
        static f4 cvt(u4 a){ return vcvtq_f32_s32(vreinterpretq_s32_u32(a)); }
        static u4 castu(f4 a){ return vreinterpretq_u32_f32(a); }
        static f4 castf(u4 a){ return vreinterpretq_f32_u32(a); }

        static uint32_t movemask(u4 m){
            uint32_t v[4];
            vst1q_u32(v, m);
            return (v[0] >> 31) | ((v[1] >> 31) << 1) | ((v[2] >> 31) << 2) | ((v[3] >> 31) << 3);
        }
        static float lane(f4 a, int i){ float v[4]; vst1q_f32(v, a); return v[i]; }
        static uint32_t ilane(u4 a, int i){ uint32_t v[4]; vst1q_u32(v, a); return v[i]; }
    };
#endif

    // ---------------------------------------------------------------- WebAssembly simd128

#if defined(JE_SIMD_WASM)
    struct Wasm {
        static constexpr const char* name = "wasm-simd128";

        typedef v128_t f4;
        typedef v128_t u4;

        static f4 load(const float* p){ return wasm_v128_load(p); }
        static f4 loadu(const float* p){ return wasm_v128_load(p); }
        static void store(float* p, f4 a){ wasm_v128_store(p, a); }
        static void storeu(float* p, f4 a){ wasm_v128_store(p, a); }
        static u4 loadu4(const uint32_t* p){ return wasm_v128_load(p); }
        static void storeu4(uint32_t* p, u4 a){ wasm_v128_store(p, a); }

        static f4 set1(float a){ return wasm_f32x4_splat(a); }
        static f4 set(float a, float b, float c, float d){ return wasm_f32x4_make(a, b, c, d); }
        static f4 zero(){ return wasm_f32x4_splat(0.0f); }
        static u4 iset1(uint32_t a){ return wasm_i32x4_splat((int32_t)a); }
        static u4 iset(uint32_t a, uint32_t b, uint32_t c, uint32_t d){ return wasm_i32x4_make((int32_t)a, (int32_t)b, (int32_t)c, (int32_t)d); }

        static f4 add(f4 a, f4 b){ return wasm_f32x4_add(a, b); }
        static f4 sub(f4 a, f4 b){ return wasm_f32x4_sub(a, b); }
        static f4 mul(f4 a, f4 b){ return wasm_f32x4_mul(a, b); }
        static f4 div(f4 a, f4 b){ return wasm_f32x4_div(a, b); }
        // pmin/pmax are SSE's min/max: b < a ? b : a is a < b ? a : b with the NaN rule of minps
        static f4 min(f4 a, f4 b){ return wasm_v128_bitselect(a, b, wasm_f32x4_lt(a, b)); }
        static f4 max(f4 a, f4 b){ return wasm_v128_bitselect(a, b, wasm_f32x4_gt(a, b)); }
        static f4 madd(f4 a, f4 b, f4 c){ return wasm_f32x4_add(wasm_f32x4_mul(a, b), c); }
        static f4 sqrt(f4 a){ return wasm_f32x4_sqrt(a); }
        static f4 abs(f4 a){ return wasm_f32x4_abs(a); }
        static f4 neg(f4 a){ return wasm_f32x4_neg(a); }
        static f4 floor(f4 a){ return wasm_f32x4_floor(a); }

        static u4 cmplt(f4 a, f4 b){ return wasm_f32x4_lt(a, b); }
        static u4 cmple(f4 a, f4 b){ return wasm_f32x4_le(a, b); }
        static u4 cmpgt(f4 a, f4 b){ return wasm_f32x4_gt(a, b); }
        static u4 cmpge(f4 a, f4 b){ return wasm_f32x4_ge(a, b); }
        static u4 cmpeq(f4 a, f4 b){ return wasm_f32x4_eq(a, b); }
        static u4 cmpneq(f4 a, f4 b){ return wasm_f32x4_ne(a, b); }

        static f4 select(u4 m, f4 a, f4 b){ return wasm_v128_bitselect(a, b, m); }
        static u4 iselect(u4 m, u4 a, u4 b){ return wasm_v128_bitselect(a, b, m); }

        static u4 iand(u4 a, u4 b){ return wasm_v128_and(a, b); }
        static u4 ior(u4 a, u4 b){ return wasm_v128_or(a, b); }
        static u4 ixor(u4 a, u4 b){ return wasm_v128_xor(a, b); }
        static u4 iandnot(u4 a, u4 b){ return wasm_v128_andnot(b, a); }
        static u4 iadd(u4 a, u4 b){ return wasm_i32x4_add(a, b); }
        static u4 isub(u4 a, u4 b){ return wasm_i32x4_sub(a, b); }
        static u4 imul(u4 a, u4 b){ return wasm_i32x4_mul(a, b); }
        static u4 icmpeq(u4 a, u4 b){ return wasm_i32x4_eq(a, b); }
        static u4 icmpgt_s(u4 a, u4 b){ return wasm_i32x4_gt(a, b); }

        template<int n> static u4 isrl(u4 a){ return wasm_u32x4_shr(a, n); }
        template<int n> static u4 isll(u4 a){ return wasm_i32x4_shl(a, n); }

        static u4 cvtt(f4 a){ return wasm_i32x4_trunc_sat_f32x4(a); }
        static f4 cvt(u4 a){ return wasm_f32x4_convert_i32x4(a); }
        static u4 castu(f4 a){ return a; }
        static f4 castf(u4 a){ return a; }

        static uint32_t movemask(u4 m){ return (uint32_t)wasm_i32x4_bitmask(m); }
        static float lane(f4 a, int i){ float v[4]; wasm_v128_store(v, a); return v[i]; }
        static uint32_t ilane(u4 a, int i){ uint32_t v[4]; wasm_v128_store(v, a); return v[i]; }
    };
#endif

#if defined(JE_SIMD_SSE2)
    typedef Sse2 Native;
#elif defined(JE_SIMD_NEON)
    typedef Neon Native;
#elif defined(JE_SIMD_WASM)
    typedef Wasm Native;
#else
    typedef Scalar Native;
#endif

    // Whether Native is a SIMD back end (false in a scalar-only build)
    constexpr bool kNativeIsSimd =
#if defined(JE_SIMD_SSE2) || defined(JE_SIMD_NEON) || defined(JE_SIMD_WASM)
        true;
#else
        false;
#endif

    // ---------------------------------------------------------------- shared maths
    //
    // Written once over the back end's operations, so Scalar and SIMD give the same bits.

    template<class B>
    struct Math {
        typedef typename B::f4 f4;
        typedef typename B::u4 u4;

        static f4 clamp(f4 x, f4 lo, f4 hi){ return B::min(B::max(x, lo), hi); }
        static f4 clamp01(f4 x){ return clamp(x, B::zero(), B::set1(1.0f)); }
        static f4 lerp(f4 a, f4 b, f4 t){ return B::madd(B::sub(b, a), t, a); }
        static f4 fract(f4 x){ return B::sub(x, B::floor(x)); }

        // sin and cos of x (radians, any range) by one reduction to [-pi/2, pi/2] and odd/even
        // minimax polynomials (error < 2e-7 over a period)
        static void sincos(f4 x, f4& s, f4& c){
            // quadrant q = round(x / (pi/2)), r = x - q * pi/2 in two parts (Cody-Waite)
            const f4 q = B::floor(B::madd(x, B::set1(0.636619772367581343f), B::set1(0.5f)));
            f4 r = B::sub(x, B::mul(q, B::set1(1.5703125f)));
            r = B::sub(r, B::mul(q, B::set1(4.8375129699707031e-4f)));
            r = B::sub(r, B::mul(q, B::set1(7.5497894158615964e-8f)));
            const f4 r2 = B::mul(r, r);
            // sin(r) and cos(r) on [-pi/4, pi/4]
            f4 ps = B::set1(-1.9515295891e-4f);
            ps = B::madd(ps, r2, B::set1(8.3321608736e-3f));
            ps = B::madd(ps, r2, B::set1(-1.6666654611e-1f));
            ps = B::madd(B::mul(ps, r2), r, r);
            f4 pc = B::set1(2.443315711809948e-5f);
            pc = B::madd(pc, r2, B::set1(-1.388731625493765e-3f));
            pc = B::madd(pc, r2, B::set1(4.166664568298827e-2f));
            pc = B::madd(B::mul(pc, r2), r2, B::sub(B::set1(1.0f), B::mul(B::set1(0.5f), r2)));
            // q mod 4 picks (sin, cos) = (s, c), (c, -s), (-s, -c), (-c, s)
            const u4 qi = B::cvtt(B::sub(q, B::mul(B::floor(B::mul(q, B::set1(0.25f))), B::set1(4.0f))));
            const u4 swap = B::icmpeq(B::iand(qi, B::iset1(1)), B::iset1(1));
            const u4 negS = B::icmpeq(B::iand(qi, B::iset1(2)), B::iset1(2));
            const u4 negC = B::icmpeq(B::iand(B::iadd(qi, B::iset1(1)), B::iset1(2)), B::iset1(2));
            const f4 sv = B::select(swap, pc, ps);
            const f4 cv = B::select(swap, ps, pc);
            s = B::castf(B::ixor(B::castu(sv), B::iand(negS, B::iset1(0x80000000u))));
            c = B::castf(B::ixor(B::castu(cv), B::iand(negC, B::iset1(0x80000000u))));
        }

        // e^x for x in [-87, 88] (relative error < 3e-7): 2^(x log2 e) split into an integer
        // exponent and a polynomial of the fraction
        static f4 exp(f4 x){
            x = clamp(x, B::set1(-87.0f), B::set1(88.0f));
            const f4 t = B::mul(x, B::set1(1.44269504088896341f));
            const f4 n = B::floor(B::add(t, B::set1(0.5f)));
            // f = x - n ln2 in two parts
            f4 f = B::sub(x, B::mul(n, B::set1(0.693359375f)));
            f = B::sub(f, B::mul(n, B::set1(-2.12194440e-4f)));
            f4 p = B::set1(1.9875691500e-4f);
            p = B::madd(p, f, B::set1(1.3981999507e-3f));
            p = B::madd(p, f, B::set1(8.3334519073e-3f));
            p = B::madd(p, f, B::set1(4.1665795894e-2f));
            p = B::madd(p, f, B::set1(1.6666665459e-1f));
            p = B::madd(p, f, B::set1(5.0000001201e-1f));
            p = B::madd(B::mul(p, f), f, B::add(f, B::set1(1.0f)));
            const u4 bits = B::template isll<23>(B::iadd(B::cvtt(n), B::iset1(127)));
            return B::mul(p, B::castf(bits));
        }

        // x^(1/3) for x >= 0: an exponent-bits first guess and three Newton steps
        static f4 cbrt(f4 x){
            // guess: the float's bits read as an integer, divided by 3, plus two thirds of the
            // exponent bias (a positive float's bits fit an int32)
            const u4 guessBits = B::iadd(B::cvtt(B::div(B::cvt(B::castu(x)), B::set1(3.0f))), B::iset1(709921077u));
            f4 y = B::castf(guessBits);
            const f4 third = B::set1(1.0f / 3.0f);
            const f4 two = B::set1(2.0f);
            for (int i = 0; i < 3; i++){
                // y = (2y + x / y^2) / 3
                y = B::mul(B::add(B::mul(two, y), B::div(x, B::mul(y, y))), third);
            }
            return B::select(B::cmple(x, B::zero()), B::zero(), y);
        }

        // atan2(y, x) (error < 1e-5 rad), for the orbit and noise rotation modules
        static f4 atan2(f4 y, f4 x){
            const f4 ax = B::abs(x), ay = B::abs(y);
            const f4 mx = B::max(ax, ay), mn = B::min(ax, ay);
            const f4 a = B::div(mn, B::max(mx, B::set1(1e-30f)));
            const f4 s = B::mul(a, a);
            f4 r = B::set1(-0.0464964749f);
            r = B::madd(r, s, B::set1(0.15931422f));
            r = B::madd(r, s, B::set1(-0.327622764f));
            r = B::madd(B::mul(r, s), a, a);
            r = B::select(B::cmpgt(ay, ax), B::sub(B::set1(1.57079637f), r), r);
            r = B::select(B::cmplt(x, B::zero()), B::sub(B::set1(3.14159274f), r), r);
            return B::select(B::cmplt(y, B::zero()), B::neg(r), r);
        }

        // 1 / sqrt(x) exactly (division of the square root: no estimate, so every back end agrees)
        static f4 rsqrt(f4 x){ return B::div(B::set1(1.0f), B::sqrt(x)); }
    };

}}

#endif // SIMD_SIMDFLOAT4_H
