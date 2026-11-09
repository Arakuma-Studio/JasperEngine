// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISEBACKEND_H
#define NOISE_NOISEBACKEND_H

// The back ends of the noise kernels (T2 N1): the four of SimdFloat4.h, plus Lane1, one float with
// the same static functions, so a single-point call does not compute four lanes and still runs the
// same arithmetic. Include only from sources built with NoiseSources.cmake's flags (R38).

#include "math/simd/SimdFloat4.h"
#include "noise/NoiseTypes.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jasperengine { namespace noise {

    struct Lane1 {
        static constexpr const char* name = "lane1";

        typedef float f4;
        typedef uint32_t u4;

        static f4 loadu(const float* p){ return *p; }
        static void storeu(float* p, f4 a){ *p = a; }
        static void storeu4(uint32_t* p, u4 a){ *p = a; }
        static u4 loadu4(const uint32_t* p){ return *p; }
        static f4 set1(float a){ return a; }
        static f4 zero(){ return 0.0f; }
        static u4 iset1(uint32_t a){ return a; }

        static f4 add(f4 a, f4 b){ return a + b; }
        static f4 sub(f4 a, f4 b){ return a - b; }
        static f4 mul(f4 a, f4 b){ return a * b; }
        static f4 div(f4 a, f4 b){ return a / b; }
        static f4 min(f4 a, f4 b){ return a < b ? a : b; }
        static f4 max(f4 a, f4 b){ return a > b ? a : b; }
        static f4 madd(f4 a, f4 b, f4 c){ return add(mul(a, b), c); }
        static f4 sqrt(f4 a){ return std::sqrt(a); }
        static f4 abs(f4 a){ return castf(castu(a) & 0x7fffffffu); }
        static f4 neg(f4 a){ return castf(castu(a) ^ 0x80000000u); }
        static f4 floor(f4 a){ return std::floor(a); }

        static u4 cmplt(f4 a, f4 b){ return a < b ? 0xffffffffu : 0u; }
        static u4 cmple(f4 a, f4 b){ return a <= b ? 0xffffffffu : 0u; }
        static u4 cmpgt(f4 a, f4 b){ return a > b ? 0xffffffffu : 0u; }
        static u4 cmpge(f4 a, f4 b){ return a >= b ? 0xffffffffu : 0u; }
        static u4 cmpeq(f4 a, f4 b){ return a == b ? 0xffffffffu : 0u; }
        static u4 cmpneq(f4 a, f4 b){ return a != b ? 0xffffffffu : 0u; }

        static f4 select(u4 m, f4 a, f4 b){ return castf((m & castu(a)) | (~m & castu(b))); }
        static u4 iselect(u4 m, u4 a, u4 b){ return (m & a) | (~m & b); }

        static u4 iand(u4 a, u4 b){ return a & b; }
        static u4 ior(u4 a, u4 b){ return a | b; }
        static u4 ixor(u4 a, u4 b){ return a ^ b; }
        static u4 iandnot(u4 a, u4 b){ return ~a & b; }
        static u4 iadd(u4 a, u4 b){ return a + b; }
        static u4 isub(u4 a, u4 b){ return a - b; }
        static u4 imul(u4 a, u4 b){ return a * b; }
        static u4 icmpeq(u4 a, u4 b){ return a == b ? 0xffffffffu : 0u; }
        static u4 icmpgt_s(u4 a, u4 b){ return static_cast<int32_t>(a) > static_cast<int32_t>(b) ? 0xffffffffu : 0u; }

        template<int n> static u4 isrl(u4 a){ return a >> n; }
        template<int n> static u4 isll(u4 a){ return a << n; }

        static u4 cvtt(f4 a){ return static_cast<uint32_t>(static_cast<int32_t>(a)); }
        static f4 cvt(u4 a){ return static_cast<float>(static_cast<int32_t>(a)); }
        static u4 castu(f4 a){ u4 r; std::memcpy(&r, &a, sizeof(r)); return r; }
        static f4 castf(u4 a){ f4 r; std::memcpy(&r, &a, sizeof(r)); return r; }
    };

    // armv7 stays on Scalar (R39, N2): its NEON flushes denormals where VFP does not, so the same
    // lattice fractions could give other bits. Every other target takes its widest back end.
#if defined(__arm__) && !defined(__aarch64__)
    typedef simd::Scalar Native;
#else
    typedef simd::Native Native;
#endif

    // Lanes of one back-end register
    template<class B>
    inline constexpr size_t laneCount = sizeof(typename B::f4) / sizeof(float);

}}

#endif // NOISE_NOISEBACKEND_H
