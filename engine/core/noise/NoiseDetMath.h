// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISEDETMATH_H
#define NOISE_NOISEDETMATH_H

// Deterministic elementary functions (T2 R5): libm's sin, pow and friends differ between standard
// libraries, so plans and kernels use these polynomials, written once over a back end so that one
// lane and four lanes give the same bits. Include only from sources built with the noise flags.
// Only +, -, *, /, sqrt and floor run here, which IEEE 754 defines exactly.

#include "noise/NoiseBackend.h"

namespace jasperengine { namespace noise { namespace detmath {

    inline constexpr float kPi = 3.14159265358979f;
    inline constexpr float kTwoPi = 6.28318530717959f;
    inline constexpr float kHalfPi = 1.57079632679490f;

    template<class B>
    struct DetMath {
        typedef typename B::f4 f4;
        typedef typename B::u4 u4;

        static f4 clampTo(f4 v, float lo, float hi){
            return B::min(B::max(v, B::set1(lo)), B::set1(hi));
        }

        // A value in [-pi, pi]: whole turns off, which is what makes a huge angle defined
        static f4 reduceTurn(f4 radians){
            f4 turns = B::mul(radians, B::set1(1.0f / kTwoPi));
            turns = B::sub(turns, B::floor(B::add(turns, B::set1(0.5f))));
            return B::mul(turns, B::set1(kTwoPi));
        }

        // sin on [-pi/2, pi/2]
        static f4 sinKernel(f4 x){
            const f4 x2 = B::mul(x, x);
            f4 p = B::set1(-1.0f / 39916800.0f);
            p = B::add(B::mul(p, x2), B::set1(1.0f / 362880.0f));
            p = B::add(B::mul(p, x2), B::set1(-1.0f / 5040.0f));
            p = B::add(B::mul(p, x2), B::set1(1.0f / 120.0f));
            p = B::add(B::mul(p, x2), B::set1(-1.0f / 6.0f));
            p = B::add(B::mul(p, x2), B::set1(1.0f));
            return B::mul(p, x);
        }

        static f4 sin(f4 radians){
            f4 x = reduceTurn(radians);
            const f4 half = B::set1(kHalfPi);
            const f4 pi = B::set1(kPi);
            x = B::select(B::cmpgt(x, half), B::sub(pi, x), x);
            x = B::select(B::cmplt(x, B::neg(half)), B::sub(B::neg(pi), x), x);
            return clampTo(sinKernel(x), -1.0f, 1.0f);
        }

        static f4 cos(f4 radians){
            return sin(B::add(radians, B::set1(kHalfPi)));
        }

        // cos of 0 gives 0 (R6: divide by zero gives 0)
        static f4 tan(f4 radians){
            const f4 s = sin(radians);
            const f4 c = cos(radians);
            return B::select(B::cmpeq(c, B::zero()), B::zero(), B::div(s, c));
        }

        // Clamped to the normal floats' exponents, so the result is finite and never denormal
        static f4 exp2(f4 x){
            x = clampTo(x, -126.0f, 127.0f);
            const f4 whole = B::floor(B::add(x, B::set1(0.5f)));
            const f4 t = B::mul(B::sub(x, whole), B::set1(0.69314718056f));
            // e^t for |t| <= ln2 / 2
            f4 p = B::set1(1.0f / 5040.0f);
            p = B::add(B::mul(p, t), B::set1(1.0f / 720.0f));
            p = B::add(B::mul(p, t), B::set1(1.0f / 120.0f));
            p = B::add(B::mul(p, t), B::set1(1.0f / 24.0f));
            p = B::add(B::mul(p, t), B::set1(1.0f / 6.0f));
            p = B::add(B::mul(p, t), B::set1(0.5f));
            p = B::add(B::mul(p, t), B::set1(1.0f));
            p = B::add(B::mul(p, t), B::set1(1.0f));
            // whole + 127 is in [1, 254]: truncation is exact, and its shift is a normal power of two
            const u4 exponent = B::cvtt(B::add(whole, B::set1(127.0f)));
            return B::mul(p, B::castf(B::template isll<23>(exponent)));
        }

        // x <= 0 gives 0 (R6)
        static f4 log2(f4 x){
            const u4 positive = B::cmpgt(x, B::zero());
            // a denormal scaled into the normals, with the exponent paying for it
            const u4 denormal = B::cmplt(x, B::set1(1.17549435e-38f));
            const f4 scaled = B::select(denormal, B::mul(x, B::set1(8388608.0f)), x);
            const u4 bits = B::castu(scaled);
            const u4 biased = B::iand(B::template isrl<23>(bits), B::iset1(0xffu));
            f4 e = B::sub(B::cvt(biased), B::set1(127.0f));
            e = B::select(denormal, B::sub(e, B::set1(23.0f)), e);
            f4 m = B::castf(B::ior(B::iand(bits, B::iset1(0x007fffffu)), B::iset1(0x3f800000u)));
            // m into [sqrt(1/2), sqrt(2)), where the series below converges fast
            const u4 high = B::cmpgt(m, B::set1(1.41421356f));
            m = B::select(high, B::mul(m, B::set1(0.5f)), m);
            e = B::select(high, B::add(e, B::set1(1.0f)), e);
            // ln(m) = 2 atanh(s), with s = (m - 1) / (m + 1)
            const f4 s = B::div(B::sub(m, B::set1(1.0f)), B::add(m, B::set1(1.0f)));
            const f4 s2 = B::mul(s, s);
            f4 p = B::set1(1.0f / 9.0f);
            p = B::add(B::mul(p, s2), B::set1(1.0f / 7.0f));
            p = B::add(B::mul(p, s2), B::set1(1.0f / 5.0f));
            p = B::add(B::mul(p, s2), B::set1(1.0f / 3.0f));
            p = B::add(B::mul(p, s2), B::set1(1.0f));
            const f4 log2m = B::mul(B::mul(p, s), B::set1(2.0f * 1.44269504089f));
            return B::select(positive, B::add(e, log2m), B::zero());
        }

        // A negative base gives 0 (R6); x^0 is 1 for x >= 0, as 0^0 is on every libm; 0^y is 0 for
        // y > 0, and for y < 0 too, since the sanitiser would make its infinity 0 anyway
        static f4 pow(f4 base, f4 exponent){
            const f4 r = exp2(B::mul(exponent, log2(base)));
            const u4 zeroBase = B::cmpeq(base, B::zero());
            const u4 zeroExponent = B::cmpeq(exponent, B::zero());
            f4 out = B::select(zeroBase, B::zero(), r);
            out = B::select(zeroExponent, B::set1(1.0f), out);
            return B::select(B::cmplt(base, B::zero()), B::zero(), out);
        }
    };

}}}

#endif // NOISE_NOISEDETMATH_H
