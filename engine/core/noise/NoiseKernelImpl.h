// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISEKERNELIMPL_H
#define NOISE_NOISEKERNELIMPL_H

// The kernels, one template per back end (T2 N1). Include only from sources built with the noise
// flags (R38): an inline kernel compiled without them could contract and give other bits. Written
// from the papers (Perlin 2002, Gustavson 2005, Worley 1996). Only B:: operations run here (R5).

#include "noise/NoiseBackend.h"
#include "noise/NoiseKernels.h"

namespace jasperengine { namespace noise {

    // The lattice period of R6: coordinates wrap here, so every conversion to integer is defined
    inline constexpr float kNoisePeriod = 65536.0f;
    // The largest float below the period
    inline constexpr float kNoisePeriodBelow = 65535.9921875f;
    inline constexpr uint32_t kCellMask = 0xffffu;
    inline constexpr uint32_t kOctaveSeedStep = 1013u;

    template<class B>
    struct NoiseKernel {
        typedef typename B::f4 f4;
        typedef typename B::u4 u4;

        // R6: non-finite is coordinate 0; then the curl formula's wrap, clamped because a tiny
        // negative lands on the period itself once rounded
        static f4 wrapCoord(f4 p){
            const u4 finite = B::cmplt(B::abs(p), B::set1(HUGE_VALF));
            const f4 clean = B::select(finite, p, B::zero());
            const f4 wrapped = B::sub(clean, B::mul(B::floor(B::mul(clean, B::set1(1.0f / kNoisePeriod))), B::set1(kNoisePeriod)));
            return B::min(B::max(wrapped, B::zero()), B::set1(kNoisePeriodBelow));
        }

        static u4 mixStart(uint32_t seed){
            return B::iset1(seed ^ 0x9E3779B9u);
        }

        static u4 mixIn(u4 h, u4 cell, uint32_t salt){
            const u4 add = B::iadd(B::iadd(B::iadd(cell, B::iset1(salt)), B::template isll<6>(h)), B::template isrl<2>(h));
            return B::ixor(h, add);
        }

        static u4 finish(u4 h){
            h = B::ixor(h, B::template isrl<16>(h));
            h = B::imul(h, B::iset1(0x7FEB352Du));
            h = B::ixor(h, B::template isrl<15>(h));
            h = B::imul(h, B::iset1(0x846CA68Bu));
            return B::ixor(h, B::template isrl<16>(h));
        }

        // The hash of the old value noise (N13), so the library extends it instead of being a second one
        static u4 hash2(u4 x, u4 z, uint32_t seed){
            return finish(mixIn(mixIn(mixStart(seed), x, 0x85EBCA6Bu), z, 0xC2B2AE35u));
        }

        static u4 hash3(u4 x, u4 y, u4 z, uint32_t seed){
            return finish(mixIn(mixIn(mixIn(mixStart(seed), x, 0x85EBCA6Bu), z, 0xC2B2AE35u), y, 0x27D4EB2Fu));
        }

        static f4 hash01(u4 h){
            return B::div(B::cvt(B::iand(h, B::iset1(0x00FFFFFFu))), B::set1(16777215.0f));
        }

        static u4 bit(u4 h, uint32_t mask){
            return B::icmpeq(B::iand(h, B::iset1(mask)), B::iset1(mask));
        }

        static f4 lerp(f4 a, f4 b, f4 t){
            return B::add(a, B::mul(B::sub(b, a), t));
        }

        static f4 clampSigned(f4 v){
            return B::min(B::max(v, B::set1(-1.0f)), B::set1(1.0f));
        }

        // ---- Value

        static f4 smooth(f4 t){
            return B::mul(B::mul(t, t), B::sub(B::set1(3.0f), B::mul(B::set1(2.0f), t)));
        }

        static f4 value2(f4 px, f4 pz, uint32_t seed){
            const f4 x = wrapCoord(px);
            const f4 z = wrapCoord(pz);
            const u4 x0 = B::cvtt(x);
            const u4 z0 = B::cvtt(z);
            const u4 x1 = B::iand(B::iadd(x0, B::iset1(1u)), B::iset1(kCellMask));
            const u4 z1 = B::iand(B::iadd(z0, B::iset1(1u)), B::iset1(kCellMask));
            const f4 tx = smooth(B::sub(x, B::cvt(x0)));
            const f4 tz = smooth(B::sub(z, B::cvt(z0)));
            const f4 a = hash01(hash2(x0, z0, seed));
            const f4 b = hash01(hash2(x1, z0, seed));
            const f4 c = hash01(hash2(x0, z1, seed));
            const f4 d = hash01(hash2(x1, z1, seed));
            const f4 v = lerp(lerp(a, b, tx), lerp(c, d, tx), tz);
            return B::sub(B::mul(v, B::set1(2.0f)), B::set1(1.0f));
        }

        static f4 value3(f4 px, f4 py, f4 pz, uint32_t seed){
            const f4 x = wrapCoord(px);
            const f4 y = wrapCoord(py);
            const f4 z = wrapCoord(pz);
            const u4 mask = B::iset1(kCellMask);
            const u4 one = B::iset1(1u);
            const u4 x0 = B::cvtt(x), y0 = B::cvtt(y), z0 = B::cvtt(z);
            const u4 x1 = B::iand(B::iadd(x0, one), mask), y1 = B::iand(B::iadd(y0, one), mask), z1 = B::iand(B::iadd(z0, one), mask);
            const f4 tx = smooth(B::sub(x, B::cvt(x0)));
            const f4 ty = smooth(B::sub(y, B::cvt(y0)));
            const f4 tz = smooth(B::sub(z, B::cvt(z0)));
            const f4 lower = lerp(
                lerp(hash01(hash3(x0, y0, z0, seed)), hash01(hash3(x1, y0, z0, seed)), tx),
                lerp(hash01(hash3(x0, y0, z1, seed)), hash01(hash3(x1, y0, z1, seed)), tx), tz);
            const f4 upper = lerp(
                lerp(hash01(hash3(x0, y1, z0, seed)), hash01(hash3(x1, y1, z0, seed)), tx),
                lerp(hash01(hash3(x0, y1, z1, seed)), hash01(hash3(x1, y1, z1, seed)), tx), tz);
            const f4 v = lerp(lower, upper, ty);
            return B::sub(B::mul(v, B::set1(2.0f)), B::set1(1.0f));
        }

        // ---- Perlin

        static f4 fade(f4 t){
            const f4 t3 = B::mul(B::mul(t, t), t);
            return B::mul(t3, B::add(B::mul(t, B::sub(B::mul(t, B::set1(6.0f)), B::set1(15.0f))), B::set1(10.0f)));
        }

        static f4 flipSign(f4 v, u4 mask){
            return B::castf(B::ixor(B::castu(v), B::iand(mask, B::iset1(0x80000000u))));
        }

        // Eight gradients of length sqrt(2): the four diagonals and the four axes
        static f4 grad2(u4 h, f4 dx, f4 dz){
            const u4 first = bit(h, 1u);
            const u4 second = bit(h, 2u);
            const f4 diagonal = B::add(flipSign(dx, first), flipSign(dz, second));
            const f4 axis = B::mul(flipSign(B::select(second, dz, dx), first), B::set1(1.41421356f));
            return B::select(bit(h, 4u), axis, diagonal);
        }

        // "Improving Noise"'s twelve gradients, over four bits
        static f4 grad3(u4 h, f4 dx, f4 dy, f4 dz){
            const u4 low = B::iand(h, B::iset1(15u));
            const u4 uIsX = B::icmpgt_s(B::iset1(8u), low);
            const f4 u = B::select(uIsX, dx, dy);
            const u4 vIsY = B::icmpgt_s(B::iset1(4u), low);
            const u4 vIsX = B::ior(B::icmpeq(low, B::iset1(12u)), B::icmpeq(low, B::iset1(14u)));
            const f4 v = B::select(vIsY, dy, B::select(vIsX, dx, dz));
            return B::add(flipSign(u, bit(h, 1u)), flipSign(v, bit(h, 2u)));
        }

        static f4 perlin2(f4 px, f4 pz, uint32_t seed){
            const f4 x = wrapCoord(px);
            const f4 z = wrapCoord(pz);
            const u4 mask = B::iset1(kCellMask);
            const u4 one = B::iset1(1u);
            const u4 x0 = B::cvtt(x), z0 = B::cvtt(z);
            const u4 x1 = B::iand(B::iadd(x0, one), mask), z1 = B::iand(B::iadd(z0, one), mask);
            const f4 dx0 = B::sub(x, B::cvt(x0));
            const f4 dz0 = B::sub(z, B::cvt(z0));
            const f4 dx1 = B::sub(dx0, B::set1(1.0f));
            const f4 dz1 = B::sub(dz0, B::set1(1.0f));
            const f4 tx = fade(dx0);
            const f4 tz = fade(dz0);
            const f4 a = grad2(hash2(x0, z0, seed), dx0, dz0);
            const f4 b = grad2(hash2(x1, z0, seed), dx1, dz0);
            const f4 c = grad2(hash2(x0, z1, seed), dx0, dz1);
            const f4 d = grad2(hash2(x1, z1, seed), dx1, dz1);
            return clampSigned(lerp(lerp(a, b, tx), lerp(c, d, tx), tz));
        }

        static f4 perlin3(f4 px, f4 py, f4 pz, uint32_t seed){
            const f4 x = wrapCoord(px);
            const f4 y = wrapCoord(py);
            const f4 z = wrapCoord(pz);
            const u4 mask = B::iset1(kCellMask);
            const u4 one = B::iset1(1u);
            const u4 x0 = B::cvtt(x), y0 = B::cvtt(y), z0 = B::cvtt(z);
            const u4 x1 = B::iand(B::iadd(x0, one), mask), y1 = B::iand(B::iadd(y0, one), mask), z1 = B::iand(B::iadd(z0, one), mask);
            const f4 dx0 = B::sub(x, B::cvt(x0));
            const f4 dy0 = B::sub(y, B::cvt(y0));
            const f4 dz0 = B::sub(z, B::cvt(z0));
            const f4 dx1 = B::sub(dx0, B::set1(1.0f));
            const f4 dy1 = B::sub(dy0, B::set1(1.0f));
            const f4 dz1 = B::sub(dz0, B::set1(1.0f));
            const f4 tx = fade(dx0);
            const f4 ty = fade(dy0);
            const f4 tz = fade(dz0);
            const f4 lower = lerp(
                lerp(grad3(hash3(x0, y0, z0, seed), dx0, dy0, dz0), grad3(hash3(x1, y0, z0, seed), dx1, dy0, dz0), tx),
                lerp(grad3(hash3(x0, y0, z1, seed), dx0, dy0, dz1), grad3(hash3(x1, y0, z1, seed), dx1, dy0, dz1), tx), tz);
            const f4 upper = lerp(
                lerp(grad3(hash3(x0, y1, z0, seed), dx0, dy1, dz0), grad3(hash3(x1, y1, z0, seed), dx1, dy1, dz0), tx),
                lerp(grad3(hash3(x0, y1, z1, seed), dx0, dy1, dz1), grad3(hash3(x1, y1, z1, seed), dx1, dy1, dz1), tx), tz);
            return clampSigned(lerp(lower, upper, ty));
        }

        // ---- Simplex

        // A corner's t^4 * (gradient . offset), zero outside its radius
        static f4 corner(f4 t, f4 gradient){
            const f4 t2 = B::mul(t, t);
            return B::select(B::cmpgt(t, B::zero()), B::mul(B::mul(t2, t2), gradient), B::zero());
        }

        static f4 simplex2(f4 px, f4 pz, uint32_t seed){
            const f4 F2 = B::set1(0.366025403784f);
            const f4 G2 = B::set1(0.211324865405f);
            const f4 half = B::set1(0.5f);
            const f4 x = wrapCoord(px);
            const f4 z = wrapCoord(pz);
            const f4 s = B::mul(B::add(x, z), F2);
            // both are not negative: truncation is the floor
            const u4 i = B::cvtt(B::add(x, s));
            const u4 j = B::cvtt(B::add(z, s));
            const f4 fi = B::cvt(i);
            const f4 fj = B::cvt(j);
            const f4 t = B::mul(B::add(fi, fj), G2);
            const f4 x0 = B::sub(x, B::sub(fi, t));
            const f4 z0 = B::sub(z, B::sub(fj, t));
            const u4 xLarger = B::cmpgt(x0, z0);
            const u4 one = B::iset1(1u);
            const u4 i1 = B::iand(xLarger, one);
            const u4 j1 = B::iandnot(xLarger, one);
            const f4 x1 = B::add(B::sub(x0, B::cvt(i1)), G2);
            const f4 z1 = B::add(B::sub(z0, B::cvt(j1)), G2);
            const f4 twoG2 = B::add(G2, G2);
            const f4 x2 = B::add(B::sub(x0, B::set1(1.0f)), twoG2);
            const f4 z2 = B::add(B::sub(z0, B::set1(1.0f)), twoG2);
            const f4 n0 = corner(B::sub(B::sub(half, B::mul(x0, x0)), B::mul(z0, z0)), grad2(hash2(i, j, seed), x0, z0));
            const f4 n1 = corner(B::sub(B::sub(half, B::mul(x1, x1)), B::mul(z1, z1)), grad2(hash2(B::iadd(i, i1), B::iadd(j, j1), seed), x1, z1));
            const f4 n2 = corner(B::sub(B::sub(half, B::mul(x2, x2)), B::mul(z2, z2)), grad2(hash2(B::iadd(i, one), B::iadd(j, one), seed), x2, z2));
            return clampSigned(B::mul(B::add(B::add(n0, n1), n2), B::set1(70.0f)));
        }

        static f4 simplex3(f4 px, f4 py, f4 pz, uint32_t seed){
            const f4 F3 = B::set1(1.0f / 3.0f);
            const f4 G3 = B::set1(1.0f / 6.0f);
            const f4 radius = B::set1(0.5f);
            const f4 x = wrapCoord(px);
            const f4 y = wrapCoord(py);
            const f4 z = wrapCoord(pz);
            const f4 s = B::mul(B::add(B::add(x, y), z), F3);
            const u4 i = B::cvtt(B::add(x, s));
            const u4 j = B::cvtt(B::add(y, s));
            const u4 k = B::cvtt(B::add(z, s));
            const f4 fi = B::cvt(i), fj = B::cvt(j), fk = B::cvt(k);
            const f4 t = B::mul(B::add(B::add(fi, fj), fk), G3);
            const f4 x0 = B::sub(x, B::sub(fi, t));
            const f4 y0 = B::sub(y, B::sub(fj, t));
            const f4 z0 = B::sub(z, B::sub(fk, t));
            const u4 xy = B::cmpge(x0, y0);
            const u4 yz = B::cmpge(y0, z0);
            const u4 xz = B::cmpge(x0, z0);
            const u4 one = B::iset1(1u);
            const u4 i1 = B::iand(B::iand(xy, xz), one);
            const u4 j1 = B::iand(B::iandnot(xy, yz), one);
            const u4 k1 = B::iandnot(B::ior(yz, xz), one);
            const u4 i2 = B::iand(B::ior(xy, xz), one);
            const u4 j2 = B::iandnot(B::iandnot(yz, xy), one);
            const u4 k2 = B::iandnot(B::iand(yz, xz), one);
            const f4 x1 = B::add(B::sub(x0, B::cvt(i1)), G3);
            const f4 y1 = B::add(B::sub(y0, B::cvt(j1)), G3);
            const f4 z1 = B::add(B::sub(z0, B::cvt(k1)), G3);
            const f4 twoG3 = B::add(G3, G3);
            const f4 x2 = B::add(B::sub(x0, B::cvt(i2)), twoG3);
            const f4 y2 = B::add(B::sub(y0, B::cvt(j2)), twoG3);
            const f4 z2 = B::add(B::sub(z0, B::cvt(k2)), twoG3);
            const f4 threeG3 = B::set1(0.5f);
            const f4 unit = B::set1(1.0f);
            const f4 x3 = B::add(B::sub(x0, unit), threeG3);
            const f4 y3 = B::add(B::sub(y0, unit), threeG3);
            const f4 z3 = B::add(B::sub(z0, unit), threeG3);
            const auto r = [&radius](f4 a, f4 b, f4 c){
                return B::sub(B::sub(B::sub(radius, B::mul(a, a)), B::mul(b, b)), B::mul(c, c));
            };
            const f4 n0 = corner(r(x0, y0, z0), grad3(hash3(i, j, k, seed), x0, y0, z0));
            const f4 n1 = corner(r(x1, y1, z1), grad3(hash3(B::iadd(i, i1), B::iadd(j, j1), B::iadd(k, k1), seed), x1, y1, z1));
            const f4 n2 = corner(r(x2, y2, z2), grad3(hash3(B::iadd(i, i2), B::iadd(j, j2), B::iadd(k, k2), seed), x2, y2, z2));
            const f4 n3 = corner(r(x3, y3, z3), grad3(hash3(B::iadd(i, one), B::iadd(j, one), B::iadd(k, one), seed), x3, y3, z3));
            return clampSigned(B::mul(B::add(B::add(B::add(n0, n1), n2), n3), B::set1(76.0f)));
        }

        // ---- Cellular

        static f4 distanceOf(CellularMetric metric, f4 dx, f4 dz){
            switch (metric){
                case CellularMetric::Manhattan: return B::add(B::abs(dx), B::abs(dz));
                case CellularMetric::Chebyshev: return B::max(B::abs(dx), B::abs(dz));
                default: return B::add(B::mul(dx, dx), B::mul(dz, dz));
            }
        }

        static f4 distanceOf(CellularMetric metric, f4 dx, f4 dy, f4 dz){
            switch (metric){
                case CellularMetric::Manhattan: return B::add(B::add(B::abs(dx), B::abs(dy)), B::abs(dz));
                case CellularMetric::Chebyshev: return B::max(B::max(B::abs(dx), B::abs(dy)), B::abs(dz));
                default: return B::add(B::add(B::mul(dx, dx), B::mul(dy, dy)), B::mul(dz, dz));
            }
        }

        struct Nearest {
            f4 f1, f2;
            u4 cell;    // an integer: hash bits passed around as a float could lose a NaN's payload
        };

        static void consider(Nearest& nearest, f4 distance, u4 h){
            const u4 closer = B::cmplt(distance, nearest.f1);
            nearest.f2 = B::select(closer, nearest.f1, B::min(nearest.f2, distance));
            nearest.cell = B::iselect(closer, h, nearest.cell);
            nearest.f1 = B::select(closer, distance, nearest.f1);
        }

        static f4 cellResult(Nearest& nearest, CellularMetric metric, CellularReturn cellReturn){
            if (cellReturn == CellularReturn::CellValue){
                const f4 v = hash01(nearest.cell);
                return B::sub(B::mul(v, B::set1(2.0f)), B::set1(1.0f));
            }
            const bool squared = metric == CellularMetric::Euclidean;
            const f4 f1 = squared ? B::sqrt(nearest.f1) : nearest.f1;
            if (cellReturn == CellularReturn::F1) return f1;
            const f4 f2 = squared ? B::sqrt(nearest.f2) : nearest.f2;
            if (cellReturn == CellularReturn::F2) return f2;
            return B::sub(f2, f1);
        }

        // A feature point stays inside its cell at any jitter <= 1, so the eight neighbours hold F1
        static f4 cellular2(f4 px, f4 pz, const BasisParams& params){
            const f4 x = wrapCoord(px);
            const f4 z = wrapCoord(pz);
            const u4 x0 = B::cvtt(x), z0 = B::cvtt(z);
            const f4 tx = B::sub(x, B::cvt(x0));
            const f4 tz = B::sub(z, B::cvt(z0));
            const f4 jitter = B::set1(params.jitter);
            const f4 half = B::set1(0.5f);
            const f4 scale = B::set1(1.0f / 65535.0f);
            const u4 mask = B::iset1(kCellMask);
            Nearest nearest{B::set1(1e30f), B::set1(1e30f), B::iset1(0u)};
            for (int oz = -1; oz <= 1; oz++){
                const u4 cz = B::iand(B::iadd(z0, B::iset1(static_cast<uint32_t>(oz))), mask);
                const f4 offsetZ = B::set1(static_cast<float>(oz));
                for (int ox = -1; ox <= 1; ox++){
                    const u4 cx = B::iand(B::iadd(x0, B::iset1(static_cast<uint32_t>(ox))), mask);
                    const u4 h = hash2(cx, cz, params.seed);
                    const f4 fx = B::mul(B::cvt(B::iand(h, mask)), scale);
                    const f4 fz = B::mul(B::cvt(B::template isrl<16>(h)), scale);
                    const f4 featureX = B::add(B::add(B::set1(static_cast<float>(ox)), half), B::mul(jitter, B::sub(fx, half)));
                    const f4 featureZ = B::add(B::add(offsetZ, half), B::mul(jitter, B::sub(fz, half)));
                    consider(nearest, distanceOf(params.metric, B::sub(featureX, tx), B::sub(featureZ, tz)), h);
                }
            }
            return cellResult(nearest, params.metric, params.cellReturn);
        }

        static f4 cellular3(f4 px, f4 py, f4 pz, const BasisParams& params){
            const f4 x = wrapCoord(px);
            const f4 y = wrapCoord(py);
            const f4 z = wrapCoord(pz);
            const u4 x0 = B::cvtt(x), y0 = B::cvtt(y), z0 = B::cvtt(z);
            const f4 tx = B::sub(x, B::cvt(x0));
            const f4 ty = B::sub(y, B::cvt(y0));
            const f4 tz = B::sub(z, B::cvt(z0));
            const f4 jitter = B::set1(params.jitter);
            const f4 half = B::set1(0.5f);
            const f4 scale11 = B::set1(1.0f / 2047.0f);
            const f4 scale10 = B::set1(1.0f / 1023.0f);
            const u4 mask = B::iset1(kCellMask);
            const u4 mask11 = B::iset1(0x7ffu);
            Nearest nearest{B::set1(1e30f), B::set1(1e30f), B::iset1(0u)};
            for (int oy = -1; oy <= 1; oy++){
                const u4 cy = B::iand(B::iadd(y0, B::iset1(static_cast<uint32_t>(oy))), mask);
                for (int oz = -1; oz <= 1; oz++){
                    const u4 cz = B::iand(B::iadd(z0, B::iset1(static_cast<uint32_t>(oz))), mask);
                    for (int ox = -1; ox <= 1; ox++){
                        const u4 cx = B::iand(B::iadd(x0, B::iset1(static_cast<uint32_t>(ox))), mask);
                        const u4 h = hash3(cx, cy, cz, params.seed);
                        const f4 fx = B::mul(B::cvt(B::iand(h, mask11)), scale11);
                        const f4 fy = B::mul(B::cvt(B::iand(B::template isrl<11>(h), mask11)), scale11);
                        const f4 fz = B::mul(B::cvt(B::template isrl<22>(h)), scale10);
                        const f4 featureX = B::add(B::add(B::set1(static_cast<float>(ox)), half), B::mul(jitter, B::sub(fx, half)));
                        const f4 featureY = B::add(B::add(B::set1(static_cast<float>(oy)), half), B::mul(jitter, B::sub(fy, half)));
                        const f4 featureZ = B::add(B::add(B::set1(static_cast<float>(oz)), half), B::mul(jitter, B::sub(fz, half)));
                        consider(nearest, distanceOf(params.metric, B::sub(featureX, tx), B::sub(featureY, ty), B::sub(featureZ, tz)), h);
                    }
                }
            }
            return cellResult(nearest, params.metric, params.cellReturn);
        }

        // ---- dispatch over a basis, with the frequency applied

        static f4 basis(const BasisParams& params, f4 x, f4 y, f4 z){
            const f4 frequency = B::set1(params.frequency);
            const f4 sx = B::mul(x, frequency);
            const f4 sz = B::mul(z, frequency);
            if (params.dimensions == 3){
                const f4 sy = B::mul(y, frequency);
                switch (params.basis){
                    case Basis::Perlin: return perlin3(sx, sy, sz, params.seed);
                    case Basis::Simplex: return simplex3(sx, sy, sz, params.seed);
                    case Basis::Value: return value3(sx, sy, sz, params.seed);
                    default: return cellular3(sx, sy, sz, params);
                }
            }
            switch (params.basis){
                case Basis::Perlin: return perlin2(sx, sz, params.seed);
                case Basis::Simplex: return simplex2(sx, sz, params.seed);
                case Basis::Value: return value2(sx, sz, params.seed);
                default: return cellular2(sx, sz, params);
            }
        }

        // R8: Σ w_i n_i(p f_i) / Σ w_i, iterated, never pow
        static f4 fbm(const FractalParams& params, f4 x, f4 y, f4 z){
            BasisParams octave = params.basis;
            float weight = 1.0f;
            float weightSum = 0.0f;
            f4 sum = B::zero();
            for (int i = 0; i < params.octaves; i++){
                octave.seed = params.basis.seed + static_cast<uint32_t>(i) * kOctaveSeedStep;
                sum = B::add(sum, B::mul(basis(octave, x, y, z), B::set1(weight)));
                weightSum += weight;
                weight *= params.gain;
                octave.frequency *= params.lacunarity;
            }
            return B::select(B::cmpgt(B::set1(weightSum), B::zero()), B::div(sum, B::set1(weightSum)), B::zero());
        }

        // R8: r_i = (1 - |n_i|)^2, weighted by the previous octave's r clamped to 0..1. A Cellular
        // basis can reach past 1, so r itself is clamped too: the result stays 0..1 on any basis.
        static f4 ridged(const FractalParams& params, f4 x, f4 y, f4 z){
            BasisParams octave = params.basis;
            float weight = 1.0f;
            float weightSum = 0.0f;
            f4 sum = B::zero();
            f4 previous = B::set1(1.0f);
            const f4 one = B::set1(1.0f);
            for (int i = 0; i < params.octaves; i++){
                octave.seed = params.basis.seed + static_cast<uint32_t>(i) * kOctaveSeedStep;
                const f4 inverted = B::sub(one, B::abs(basis(octave, x, y, z)));
                const f4 r = B::min(B::mul(inverted, inverted), one);
                sum = B::add(sum, B::mul(B::mul(r, previous), B::set1(weight)));
                previous = r;
                weightSum += weight;
                weight *= params.gain;
                octave.frequency *= params.lacunarity;
            }
            return B::select(B::cmpgt(B::set1(weightSum), B::zero()), B::div(sum, B::set1(weightSum)), B::zero());
        }
    };

    // Runs fn over a whole batch; the tail lanes are padded with zeros and never stored
    template<class B, class Fn>
    inline void forEachLaneGroup(const float* x, const float* y, const float* z, size_t count, float* out, Fn&& fn){
        constexpr size_t lanes = laneCount<B>;
        const size_t whole = count - count % lanes;
        for (size_t i = 0; i < whole; i += lanes){
            const typename B::f4 px = B::loadu(x + i);
            const typename B::f4 py = y ? B::loadu(y + i) : B::zero();
            const typename B::f4 pz = B::loadu(z + i);
            B::storeu(out + i, fn(px, py, pz));
        }
        if (whole < count){
            float tx[4] = {0.0f, 0.0f, 0.0f, 0.0f}, ty[4] = {0.0f, 0.0f, 0.0f, 0.0f}, tz[4] = {0.0f, 0.0f, 0.0f, 0.0f}, to[4];
            const size_t rest = count - whole;
            for (size_t i = 0; i < rest; i++){
                tx[i] = x[whole + i];
                if (y) ty[i] = y[whole + i];
                tz[i] = z[whole + i];
            }
            B::storeu(to, fn(B::loadu(tx), B::loadu(ty), B::loadu(tz)));
            for (size_t i = 0; i < rest; i++) out[whole + i] = to[i];
        }
    }

}}

#endif // NOISE_NOISEKERNELIMPL_H
