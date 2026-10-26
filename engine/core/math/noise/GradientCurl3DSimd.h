// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_GRADIENTCURL3DSIMD_H
#define NOISE_GRADIENTCURL3DSIMD_H

// Four samples at a time of the curl noise of GradientCurl3D.h, on any back end of
// math/simd/SimdFloat4.h. The operations and their order follow the scalar reference, so it
// matches it to the last bit or two (CurlNoise.SimdMatchesReference allows 1e-5); its Scalar
// instantiation and its SIMD one are bit-identical.
//
// curl3      the reference field (three potentials): the particle Noise module's High quality
// curl2      two potentials, (-dB/dz, dA/dz, dB/dx - dA/dy): still divergence-free, two thirds
//            of the cost (Medium)

#include "math/noise/GradientCurl3D.h"
#include "math/simd/SimdFloat4.h"

namespace jasperengine { namespace noise {

    template<class B>
    struct GradientCurlSimd {
        typedef typename B::f4 f4;
        typedef typename B::u4 u4;

        struct V3 { f4 x, y, z; };

        static u4 hash(u4 x){
            x = B::ixor(x, B::template isrl<16>(x));
            x = B::imul(x, B::iset1(0x7feb352du));
            x = B::ixor(x, B::template isrl<15>(x));
            x = B::imul(x, B::iset1(0x846ca68bu));
            return B::ixor(x, B::template isrl<16>(x));
        }

        // h % 3, exact: h = a * 65536 + b and 65536 = 1 (mod 3), so h = a + b (mod 3) with
        // a + b < 2^17, which a float holds and divides by 3 exactly enough to floor
        static u4 mod3(u4 h){
            const f4 s = B::cvt(B::iadd(B::template isrl<16>(h), B::iand(h, B::iset1(0xffffu))));
            const f4 q = B::floor(B::div(s, B::set1(3.0f)));
            return B::cvtt(B::sub(s, B::mul(q, B::set1(3.0f))));
        }

        static V3 potentialGradient(f4 px, f4 py, f4 pz, u4 seed){
            const f4 period = B::set1(gradientcurl3d::kPeriod);
            const f4 invPeriod = B::set1(1.0f / gradientcurl3d::kPeriod);
            const f4 one = B::set1(1.0f);
            f4 p[3] = {px, py, pz};
            u4 cell[3];
            f4 fraction[3], fade[3], slope[3];
            for (int axis = 0; axis < 3; axis++){
                // fmod(p, 256) moved into [0, 256): the same value as the reference's
                const f4 wrapped = B::sub(p[axis], B::mul(B::floor(B::mul(p[axis], invPeriod)), period));
                const f4 whole = B::floor(wrapped);
                cell[axis] = B::cvtt(whole);
                const f4 t = B::sub(wrapped, whole);
                fraction[axis] = t;
                const f4 t3 = B::mul(B::mul(t, t), t);
                fade[axis] = B::mul(t3, B::add(B::mul(t, B::sub(B::mul(t, B::set1(6.0f)), B::set1(15.0f))), B::set1(10.0f)));
                const f4 tm1 = B::sub(t, one);
                slope[axis] = B::mul(B::mul(B::mul(B::mul(B::set1(30.0f), t), t), tm1), tm1);
            }
            V3 result{B::zero(), B::zero(), B::zero()};
            const u4 mask255 = B::iset1(255u);
            for (uint32_t corner = 0; corner < 8; corner++){
                u4 h = seed;
                f4 delta[3], weights[3], slopes[3];
                for (int axis = 0; axis < 3; axis++){
                    const uint32_t upper = (corner >> axis) & 1u;
                    h = B::ixor(h, B::imul(B::iand(B::iadd(cell[axis], B::iset1(upper)), mask255), B::iset1(gradientcurl3d::kAxisPrimes[axis])));
                    delta[axis] = upper ? B::sub(fraction[axis], one) : B::sub(fraction[axis], B::zero());
                    weights[axis] = upper ? fade[axis] : B::sub(one, fade[axis]);
                    slopes[axis] = upper ? slope[axis] : B::neg(slope[axis]);
                }
                h = hash(h);
                const u4 absent = mod3(h);
                const u4 sign = B::template isrl<2>(h);
                const u4 bit0 = B::icmpeq(B::iand(sign, B::iset1(1u)), B::iset1(1u));
                const u4 bit1 = B::icmpeq(B::iand(sign, B::iset1(2u)), B::iset1(2u));
                const f4 s0 = B::select(bit0, one, B::neg(one));
                const f4 s1 = B::select(bit1, one, B::neg(one));
                const u4 absent0 = B::icmpeq(absent, B::iset1(0u));
                const u4 absent1 = B::icmpeq(absent, B::iset1(1u));
                const u4 absent2 = B::icmpeq(absent, B::iset1(2u));
                // the first present axis takes bit 0, the second bit 1
                const f4 g0 = B::select(absent0, B::zero(), s0);
                const f4 g1 = B::select(absent0, s0, B::select(absent1, B::zero(), s1));
                const f4 g2 = B::select(absent2, B::zero(), s1);
                const f4 dot = B::add(B::add(B::mul(g0, delta[0]), B::mul(g1, delta[1])), B::mul(g2, delta[2]));
                const f4 weight = B::mul(B::mul(weights[0], weights[1]), weights[2]);
                const f4 ws0 = B::mul(B::mul(slopes[0], weights[1]), weights[2]);
                const f4 ws1 = B::mul(B::mul(slopes[1], weights[0]), weights[2]);
                const f4 ws2 = B::mul(B::mul(slopes[2], weights[0]), weights[1]);
                result.x = B::add(result.x, B::add(B::mul(g0, weight), B::mul(dot, ws0)));
                result.y = B::add(result.y, B::add(B::mul(g1, weight), B::mul(dot, ws1)));
                result.z = B::add(result.z, B::add(B::mul(g2, weight), B::mul(dot, ws2)));
            }
            return result;
        }

        static u4 potentialSeed(uint32_t seed, uint32_t component){
            return B::iset1(gradientcurl3d::hash(seed + component * 0x9e3779b9u));
        }

        // non-finite lanes sample the origin (and give zero below), so no conversion sees them
        static u4 finite(f4 x, f4 y, f4 z){
            const f4 big = B::set1(3.40282347e38f);
            return B::iand(B::iand(B::cmple(B::abs(x), big), B::cmple(B::abs(y), big)), B::cmple(B::abs(z), big));
        }

        static V3 curl3(f4 x, f4 y, f4 z, uint32_t seed){
            const u4 ok = finite(x, y, z);
            x = B::select(ok, x, B::zero());
            y = B::select(ok, y, B::zero());
            z = B::select(ok, z, B::zero());
            // potential c reads (p[(c+1)%3], p[(c+2)%3], p[c]) and its gradient lands on those axes
            const V3 a = potentialGradient(y, z, x, potentialSeed(seed, 0));   // d0/d(y, z, x)
            const V3 b = potentialGradient(z, x, y, potentialSeed(seed, 1));   // d1/d(z, x, y)
            const V3 c = potentialGradient(x, y, z, potentialSeed(seed, 2));   // d2/d(x, y, z)
            // potential[0] = (a.z, a.x, a.y), potential[1] = (b.y, b.z, b.x), potential[2] = (c.x, c.y, c.z)
            V3 r;
            r.x = B::select(ok, B::sub(c.y, b.x), B::zero());
            r.y = B::select(ok, B::sub(a.y, c.x), B::zero());
            r.z = B::select(ok, B::sub(b.y, a.x), B::zero());
            return r;
        }

        static V3 curl2(f4 x, f4 y, f4 z, uint32_t seed){
            const u4 ok = finite(x, y, z);
            x = B::select(ok, x, B::zero());
            y = B::select(ok, y, B::zero());
            z = B::select(ok, z, B::zero());
            const V3 a = potentialGradient(y, z, x, potentialSeed(seed, 0));
            const V3 b = potentialGradient(z, x, y, potentialSeed(seed, 1));
            // (-d1/dz, d0/dz, d1/dx - d0/dy)
            V3 r;
            r.x = B::select(ok, B::neg(b.x), B::zero());
            r.y = B::select(ok, a.y, B::zero());
            r.z = B::select(ok, B::sub(b.y, a.x), B::zero());
            return r;
        }
    };

    // The scalar form of curl2 (Medium), for tests: the reference potentials without the third
    inline Curl3 gradientCurl2(const Curl3& point, uint32_t seed){
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2]))
            return {};
        Curl3 potential[2];
        for (uint32_t component = 0; component < 2; ++component){
            Curl3 rotated;
            for (uint32_t axis = 0; axis < 3; ++axis)
                rotated[axis] = point[(component + axis + 1) % 3];
            const Curl3 gradient = gradientcurl3d::potentialGradient(rotated, gradientcurl3d::hash(seed + component * 0x9e3779b9u));
            for (uint32_t axis = 0; axis < 3; ++axis)
                potential[component][(component + axis + 1) % 3] = gradient[axis];
        }
        Curl3 curl;
        curl[0] = -potential[1][2];
        curl[1] = potential[0][2];
        curl[2] = potential[1][0] - potential[0][1];
        return curl;
    }

}}

#endif // NOISE_GRADIENTCURL3DSIMD_H
