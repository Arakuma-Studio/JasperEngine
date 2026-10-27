// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The curl noise eight samples at a time on x86-64 processors with AVX2 (the particle Noise
// module's High and Medium fields, docs/specs/S20-particles.md R21): the kernels of
// GradientCurl3DSimd.h on an 8-wide back end. The operations are the 4-wide ones (no fused
// multiply-add, exact floor), so the samples are bit-identical to the SSE2 and scalar paths;
// processors without AVX2 take those. CMake builds this file with -mavx2 (/arch:AVX2) on x86-64;
// elsewhere it compiles to the "not available" answer. Only this file's own types are built for
// AVX2, so nothing it defines can stand in for a baseline function at link time.

#include "CurlNoiseBatch.h"

#if (defined(__x86_64__) || defined(_M_X64)) && defined(__AVX2__)
    #define JE_CURL_AVX2 1
    #include <immintrin.h>
    #if defined(_MSC_VER) && !defined(__clang__)
        #include <intrin.h>
    #endif
    #include "math/noise/GradientCurl3DSimd.h"
#endif

using namespace jasperengine;
using namespace jasperengine::noise;

#ifdef JE_CURL_AVX2
namespace {

    // the operations GradientCurlSimd uses, 8 lanes wide
    struct Avx2 {
        typedef __m256 f4;
        typedef __m256i u4;

        static f4 loadu(const float* p){ return _mm256_loadu_ps(p); }
        static void storeu(float* p, f4 a){ _mm256_storeu_ps(p, a); }
        static f4 set1(float a){ return _mm256_set1_ps(a); }
        static f4 zero(){ return _mm256_setzero_ps(); }
        static u4 iset1(uint32_t a){ return _mm256_set1_epi32((int)a); }
        static f4 add(f4 a, f4 b){ return _mm256_add_ps(a, b); }
        static f4 sub(f4 a, f4 b){ return _mm256_sub_ps(a, b); }
        static f4 mul(f4 a, f4 b){ return _mm256_mul_ps(a, b); }
        static f4 abs(f4 a){ return _mm256_and_ps(a, _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff))); }
        static f4 neg(f4 a){ return _mm256_xor_ps(a, _mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000u))); }
        static f4 floor(f4 a){ return _mm256_floor_ps(a); }
        static u4 cmple(f4 a, f4 b){ return _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_LE_OQ)); }
        static f4 select(u4 m, f4 a, f4 b){ return _mm256_blendv_ps(b, a, _mm256_castsi256_ps(m)); }
        static u4 iand(u4 a, u4 b){ return _mm256_and_si256(a, b); }
        static u4 ior(u4 a, u4 b){ return _mm256_or_si256(a, b); }
        static u4 ixor(u4 a, u4 b){ return _mm256_xor_si256(a, b); }
        static u4 iandnot(u4 a, u4 b){ return _mm256_andnot_si256(a, b); }
        static u4 iadd(u4 a, u4 b){ return _mm256_add_epi32(a, b); }
        static u4 isub(u4 a, u4 b){ return _mm256_sub_epi32(a, b); }
        static u4 imul(u4 a, u4 b){ return _mm256_mullo_epi32(a, b); }
        static u4 icmpeq(u4 a, u4 b){ return _mm256_cmpeq_epi32(a, b); }
        template<int n> static u4 isrl(u4 a){ return _mm256_srli_epi32(a, n); }
        template<int n> static u4 isll(u4 a){ return _mm256_slli_epi32(a, n); }
        static u4 cvtt(f4 a){ return _mm256_cvttps_epi32(a); }
        static f4 cvt(u4 a){ return _mm256_cvtepi32_ps(a); }
        static u4 castu(f4 a){ return _mm256_castps_si256(a); }
        static f4 castf(u4 a){ return _mm256_castsi256_ps(a); }
    };

    bool cpuHasAvx2(){
#if defined(_MSC_VER) && !defined(__clang__)
        int info[4];
        __cpuid(info, 0);
        if (info[0] < 7)
            return false;
        __cpuid(info, 1);
        // the OS saves the AVX state (OSXSAVE, and XCR0's SSE and AVX bits)
        if (!(info[2] & (1 << 27)) || (_xgetbv(0) & 6) != 6)
            return false;
        __cpuidex(info, 7, 0);
        return (info[1] & (1 << 5)) != 0;
#else
        return __builtin_cpu_supports("avx2");
#endif
    }

}
#endif

bool noise::curlWideAvailable(){
#ifdef JE_CURL_AVX2
    static const bool available = cpuHasAvx2();
    return available;
#else
    return false;
#endif
}

void noise::curlNoiseWide(int potentials, const float* x, const float* y, const float* z, size_t count, float frequency, float offset,
                          uint32_t seed, float* outX, float* outY, float* outZ){
#ifdef JE_CURL_AVX2
    typedef GradientCurlSimd<Avx2> Curl;
    const Avx2::f4 f = Avx2::set1(frequency), o = Avx2::set1(offset);
    for (size_t i = 0; i < count; i += 8){
        // the sample point as the 4-wide path forms it: p * frequency + offset
        const Avx2::f4 sx = Avx2::add(Avx2::mul(Avx2::loadu(x + i), f), o);
        const Avx2::f4 sy = Avx2::add(Avx2::mul(Avx2::loadu(y + i), f), o);
        const Avx2::f4 sz = Avx2::add(Avx2::mul(Avx2::loadu(z + i), f), o);
        const Curl::V3 c = potentials == 2 ? Curl::curl2(sx, sy, sz, seed) : Curl::curl3(sx, sy, sz, seed);
        Avx2::storeu(outX + i, c.x);
        Avx2::storeu(outY + i, c.y);
        Avx2::storeu(outZ + i, c.z);
    }
#else
    (void)potentials; (void)x; (void)y; (void)z; (void)count; (void)frequency; (void)offset; (void)seed;
    (void)outX; (void)outY; (void)outZ;
#endif
}
