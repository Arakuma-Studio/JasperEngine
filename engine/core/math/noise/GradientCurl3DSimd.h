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

// the corner loops unrolled at any optimisation level: their side indices become constants
#if defined(__clang__)
    #define JE_CURL_UNROLL _Pragma("clang loop unroll(full)")
#elif defined(__GNUC__)
    #define JE_CURL_UNROLL _Pragma("GCC unroll 8")
#else
    #define JE_CURL_UNROLL
#endif

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
        // s = a + b < 2^17: s / 3 is within 2^-7 of its exact value as a float product with
        // 1/3, never across a whole number, so truncating it is the exact quotient
        static u4 mod3(u4 h){
            const u4 s = B::iadd(B::template isrl<16>(h), B::iand(h, B::iset1(0xffffu)));
            const u4 q = B::cvtt(B::mul(B::cvt(s), B::set1(0.333333343f)));
            return B::isub(s, B::iadd(q, B::template isll<1>(q)));
        }

        // What the lattice gives one axis of the samples: the two cells a corner picks (masked to
        // the 256-cell period), and per side (0 lower, 1 upper) the offset into the cell, the
        // fade weight and its slope. The three potentials read the same axes in other slots.
        struct Axis {
            u4 cell[2];
            f4 delta[2], weight[2], slope[2];
        };

        static Axis axisOf(f4 p){
            const f4 period = B::set1(gradientcurl3d::kPeriod);
            const f4 one = B::set1(1.0f);
            // fmod(p, 256) moved into [0, 256]: the same value as the reference's
            const f4 wrapped = B::sub(p, B::mul(B::floor(B::mul(p, B::set1(1.0f / gradientcurl3d::kPeriod))), period));
            // not negative: truncation is the floor
            const u4 cell = B::cvtt(wrapped);
            const f4 t = B::sub(wrapped, B::cvt(cell));
            const f4 t3 = B::mul(B::mul(t, t), t);
            const f4 fade = B::mul(t3, B::add(B::mul(t, B::sub(B::mul(t, B::set1(6.0f)), B::set1(15.0f))), B::set1(10.0f)));
            const f4 tm1 = B::sub(t, one);
            const f4 slope = B::mul(B::mul(B::mul(B::mul(B::set1(30.0f), t), t), tm1), tm1);
            Axis a;
            const u4 mask255 = B::iset1(255u);
            a.cell[0] = B::iand(cell, mask255);
            a.cell[1] = B::iand(B::iadd(cell, B::iset1(1u)), mask255);
            a.delta[0] = B::sub(t, B::zero());
            a.delta[1] = B::sub(t, one);
            a.weight[0] = B::sub(one, fade);
            a.weight[1] = fade;
            a.slope[0] = B::neg(slope);
            a.slope[1] = slope;
            return a;
        }

        // the sign bit of each lane where `bitMask` of `bits` is clear (a -1 gradient component)
        static f4 negativeWhereClear(u4 bits, uint32_t bitMask){
            const u4 clear = B::icmpeq(B::iand(bits, B::iset1(bitMask)), B::iset1(0u));
            return B::castf(B::iand(clear, B::iset1(0x80000000u)));
        }

        // g * v for a gradient component g of +1, -1 (its sign bit set) or 0 (its axis absent): exact
        static f4 signedMasked(f4 v, f4 sign, u4 absent){
            return B::castf(B::iandnot(absent, B::ixor(B::castu(v), B::castu(sign))));
        }

        // The gradient of one potential, which reads axis `ax[j]` in its slot j; `terms[axis][j][u]`
        // is that axis' cell on side u times slot j's prime (the hash input, shared by the three
        // potentials). The same values, in the same order of operations, as the reference.
        static V3 potentialGradient(const Axis (&axes)[3], const int (&ax)[3], const u4 (&terms)[3][3][2], u4 seed){
            const Axis& A0 = axes[ax[0]];
            const Axis& A1 = axes[ax[1]];
            const Axis& A2 = axes[ax[2]];
            // the weight products the corners share: (w0 w1), (s0 w1), (s1 w0), (s2 w0)
            f4 w01[2][2], s0w1[2][2], s1w0[2][2], s2w0[2][2];
            JE_CURL_UNROLL
            for (int u = 0; u < 2; u++){
                JE_CURL_UNROLL
                for (int v = 0; v < 2; v++){
                    w01[u][v] = B::mul(A0.weight[u], A1.weight[v]);
                    s0w1[u][v] = B::mul(A0.slope[u], A1.weight[v]);
                    s1w0[u][v] = B::mul(A1.slope[u], A0.weight[v]);
                    s2w0[u][v] = B::mul(A2.slope[u], A0.weight[v]);
                }
            }
            V3 result{B::zero(), B::zero(), B::zero()};
            JE_CURL_UNROLL
            for (uint32_t corner = 0; corner < 8; corner++){
                const int u0 = (int)(corner & 1u), u1 = (int)((corner >> 1) & 1u), u2 = (int)((corner >> 2) & 1u);
                u4 h = B::ixor(seed, terms[ax[0]][0][u0]);
                h = B::ixor(h, terms[ax[1]][1][u1]);
                h = B::ixor(h, terms[ax[2]][2][u2]);
                h = hash(h);
                const u4 absent = mod3(h);
                const u4 absent0 = B::icmpeq(absent, B::iset1(0u));
                const u4 absent1 = B::icmpeq(absent, B::iset1(1u));
                const u4 absent2 = B::icmpeq(absent, B::iset1(2u));
                const u4 sign = B::template isrl<2>(h);
                // the first present axis takes bit 0 of the sign, the second bit 1
                const f4 signA = negativeWhereClear(sign, 1u);
                const f4 signB = negativeWhereClear(sign, 2u);
                const f4 sign1 = B::castf(B::ior(B::iand(absent0, B::castu(signA)), B::iandnot(absent0, B::castu(signB))));
                const f4 dot = B::add(B::add(signedMasked(A0.delta[u0], signA, absent0), signedMasked(A1.delta[u1], sign1, absent1)),
                                      signedMasked(A2.delta[u2], signB, absent2));
                const f4 weight = B::mul(w01[u0][u1], A2.weight[u2]);
                const f4 ws0 = B::mul(s0w1[u0][u1], A2.weight[u2]);
                const f4 ws1 = B::mul(s1w0[u1][u0], A2.weight[u2]);
                const f4 ws2 = B::mul(s2w0[u2][u0], A1.weight[u1]);
                result.x = B::add(result.x, B::add(signedMasked(weight, signA, absent0), B::mul(dot, ws0)));
                result.y = B::add(result.y, B::add(signedMasked(weight, sign1, absent1), B::mul(dot, ws1)));
                result.z = B::add(result.z, B::add(signedMasked(weight, signB, absent2), B::mul(dot, ws2)));
            }
            return result;
        }

        // the hash inputs of every (axis, slot, side): the axis' cell times the slot's prime
        static void hashTerms(const Axis (&axes)[3], u4 (&terms)[3][3][2]){
            for (int axis = 0; axis < 3; axis++){
                for (int slot = 0; slot < 3; slot++){
                    const u4 prime = B::iset1(gradientcurl3d::kAxisPrimes[slot]);
                    terms[axis][slot][0] = B::imul(axes[axis].cell[0], prime);
                    terms[axis][slot][1] = B::imul(axes[axis].cell[1], prime);
                }
            }
        }

        // gradientcurl3d::hash, as this back end's own function: a file built for another
        // instruction set (CurlNoiseAvx2.cpp) never emits a copy of a baseline inline function
        static uint32_t scalarHash(uint32_t x){
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            return x ^ (x >> 16);
        }

        static u4 potentialSeed(uint32_t seed, uint32_t component){
            return B::iset1(scalarHash(seed + component * 0x9e3779b9u));
        }

        // non-finite lanes sample the origin (and give zero below), so no conversion sees them
        static u4 finite(f4 x, f4 y, f4 z){
            const f4 big = B::set1(3.40282347e38f);
            return B::iand(B::iand(B::cmple(B::abs(x), big), B::cmple(B::abs(y), big)), B::cmple(B::abs(z), big));
        }

        static V3 curl3(f4 x, f4 y, f4 z, uint32_t seed){
            const u4 ok = finite(x, y, z);
            const Axis axes[3] = {axisOf(B::select(ok, x, B::zero())), axisOf(B::select(ok, y, B::zero())), axisOf(B::select(ok, z, B::zero()))};
            u4 terms[3][3][2];
            hashTerms(axes, terms);
            // potential c reads (p[(c+1)%3], p[(c+2)%3], p[c]) and its gradient lands on those axes
            const int read0[3] = {1, 2, 0}, read1[3] = {2, 0, 1}, read2[3] = {0, 1, 2};
            const V3 a = potentialGradient(axes, read0, terms, potentialSeed(seed, 0));   // d0/d(y, z, x)
            const V3 b = potentialGradient(axes, read1, terms, potentialSeed(seed, 1));   // d1/d(z, x, y)
            const V3 c = potentialGradient(axes, read2, terms, potentialSeed(seed, 2));   // d2/d(x, y, z)
            // potential[0] = (a.z, a.x, a.y), potential[1] = (b.y, b.z, b.x), potential[2] = (c.x, c.y, c.z)
            V3 r;
            r.x = B::select(ok, B::sub(c.y, b.x), B::zero());
            r.y = B::select(ok, B::sub(a.y, c.x), B::zero());
            r.z = B::select(ok, B::sub(b.y, a.x), B::zero());
            return r;
        }

        static V3 curl2(f4 x, f4 y, f4 z, uint32_t seed){
            const u4 ok = finite(x, y, z);
            const Axis axes[3] = {axisOf(B::select(ok, x, B::zero())), axisOf(B::select(ok, y, B::zero())), axisOf(B::select(ok, z, B::zero()))};
            u4 terms[3][3][2];
            hashTerms(axes, terms);
            const int read0[3] = {1, 2, 0}, read1[3] = {2, 0, 1};
            const V3 a = potentialGradient(axes, read0, terms, potentialSeed(seed, 0));
            const V3 b = potentialGradient(axes, read1, terms, potentialSeed(seed, 1));
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

#undef JE_CURL_UNROLL

#endif // NOISE_GRADIENTCURL3DSIMD_H
