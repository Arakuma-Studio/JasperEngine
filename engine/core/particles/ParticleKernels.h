// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The module maths are ported from OpenEngine's particle processors (Engine/Modules/Particles/
// Source/Processors/*Processor.cpp: shape, velocity cone, acceleration, drag, limit speed,
// noise, orbit, collision), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License (see NOTICE.md), rewritten as 4-wide kernels over SoA channels.

#ifndef PARTICLES_PARTICLEKERNELS_H
#define PARTICLES_PARTICLEKERNELS_H

// The simulation kernels, written once over a SIMD back end (math/simd/SimdFloat4.h) and
// instantiated per back end (ParticleKernels.cpp: the native one and Scalar). Every operation
// is a fixed sequence of the back end's operations, so the Scalar instantiation gives the same
// bits as the SIMD one: the determinism tests compare them.

#include "math/noise/CurlNoiseBatch.h"
#include "math/noise/GradientCurl3DSimd.h"
#include "math/simd/SimdFloat4.h"
#include "particles/ParticleKernelContext.h"
#include "particles/ParticleModules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace jasperengine { namespace particles {

    // The hash of OpenEngine's ParticleRandom: a stateless value per (seed, particle, module,
    // stream). The module and stream are folded into a salt at compile time.
    constexpr uint32_t randomSalt(uint32_t module, uint32_t stream){
        return module * 0x85ebca6bu ^ stream * 0xc2b2ae35u;
    }
    inline uint32_t hashRandom(uint32_t key, uint32_t particle){
        uint32_t x = key ^ (particle * 0x9e3779b9u);
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }
    inline float random01(uint32_t key, uint32_t particle){
        return (float)(hashRandom(key, particle) & 0xffffffu) * (1.0f / 16777216.0f);
    }

    template<class B>
    struct Kernels {
        typedef typename B::f4 f4;
        typedef typename B::u4 u4;
        typedef simd::Math<B> M;

        // ------------------------------------------------------------ basics

        static f4 ld(const float* base, uint32_t i){ return B::load(base + i); }
        static void st(float* base, uint32_t i, f4 v){ B::store(base + i, v); }
        static u4 ldu(const float* base, uint32_t i){ return B::loadu4(reinterpret_cast<const uint32_t*>(base) + i); }
        static void stu(float* base, uint32_t i, u4 v){ B::storeu4(reinterpret_cast<uint32_t*>(base) + i, v); }
        static f4 k(float v){ return B::set1(v); }
        static f4 sin(f4 x){ f4 s, c; M::sincos(x, s, c); return s; }

        static u4 hash(u4 key, u4 particle){
            u4 x = B::ixor(key, B::imul(particle, B::iset1(0x9e3779b9u)));
            x = B::ixor(x, B::template isrl<16>(x));
            x = B::imul(x, B::iset1(0x7feb352du));
            x = B::ixor(x, B::template isrl<15>(x));
            x = B::imul(x, B::iset1(0x846ca68bu));
            return B::ixor(x, B::template isrl<16>(x));
        }
        static f4 rand01(uint32_t key, u4 particle){
            return B::mul(B::cvt(B::iand(hash(B::iset1(key), particle), B::iset1(0xffffffu))), k(1.0f / 16777216.0f));
        }
        // -1 to 1
        static f4 randSigned(uint32_t key, u4 particle){ return B::sub(B::mul(rand01(key, particle), k(2.0f)), k(1.0f)); }

        static f4 length3(f4 x, f4 y, f4 z){ return B::sqrt(B::add(B::add(B::mul(x, x), B::mul(y, y)), B::mul(z, z))); }
        static f4 dot3(f4 ax, f4 ay, f4 az, f4 bx, f4 by, f4 bz){ return B::add(B::add(B::mul(ax, bx), B::mul(ay, by)), B::mul(az, bz)); }
        // x / length, 0 where the length is 0
        static f4 safeDiv(f4 x, f4 len){ return B::select(B::cmpgt(len, k(1.0e-20f)), B::div(x, len), B::zero()); }

        // m * (x, y, z, 1), m column-major 3x4
        static void xformPoint(const float* m, f4 x, f4 y, f4 z, f4& ox, f4& oy, f4& oz){
            ox = B::add(B::add(B::add(B::mul(k(m[0]), x), B::mul(k(m[3]), y)), B::mul(k(m[6]), z)), k(m[9]));
            oy = B::add(B::add(B::add(B::mul(k(m[1]), x), B::mul(k(m[4]), y)), B::mul(k(m[7]), z)), k(m[10]));
            oz = B::add(B::add(B::add(B::mul(k(m[2]), x), B::mul(k(m[5]), y)), B::mul(k(m[8]), z)), k(m[11]));
        }
        // m * (x, y, z), m column-major 3x3
        static void xformDir(const float* m, f4 x, f4 y, f4 z, f4& ox, f4& oy, f4& oz){
            ox = B::add(B::add(B::mul(k(m[0]), x), B::mul(k(m[3]), y)), B::mul(k(m[6]), z));
            oy = B::add(B::add(B::mul(k(m[1]), x), B::mul(k(m[4]), y)), B::mul(k(m[7]), z));
            oz = B::add(B::add(B::mul(k(m[2]), x), B::mul(k(m[5]), y)), B::mul(k(m[8]), z));
        }
        // a transform blended per lane: prev + (cur - prev) * f (the emitter between two ticks)
        static void xformPointLerp(const float* prev, const float* cur, f4 f, f4 x, f4 y, f4 z, f4& ox, f4& oy, f4& oz){
            f4 m[12];
            for (int i = 0; i < 12; i++) m[i] = B::madd(k(cur[i] - prev[i]), f, k(prev[i]));
            ox = B::add(B::add(B::add(B::mul(m[0], x), B::mul(m[3], y)), B::mul(m[6], z)), m[9]);
            oy = B::add(B::add(B::add(B::mul(m[1], x), B::mul(m[4], y)), B::mul(m[7], z)), m[10]);
            oz = B::add(B::add(B::add(B::mul(m[2], x), B::mul(m[5], y)), B::mul(m[8], z)), m[11]);
        }
        static void xformDirLerp(const float* prev, const float* cur, f4 f, f4 x, f4 y, f4 z, f4& ox, f4& oy, f4& oz){
            f4 m[9];
            for (int i = 0; i < 9; i++) m[i] = B::madd(k(cur[i] - prev[i]), f, k(prev[i]));
            ox = B::add(B::add(B::mul(m[0], x), B::mul(m[3], y)), B::mul(m[6], z));
            oy = B::add(B::add(B::mul(m[1], x), B::mul(m[4], y)), B::mul(m[7], z));
            oz = B::add(B::add(B::mul(m[2], x), B::mul(m[5], y)), B::mul(m[8], z));
        }

        // e * q for a uniform quaternion e (x, y, z, w) and per-lane q
        static void quatMulUniform(const float e[4], f4& qx, f4& qy, f4& qz, f4& qw){
            const f4 ex = k(e[0]), ey = k(e[1]), ez = k(e[2]), ew = k(e[3]);
            const f4 nx = B::add(B::add(B::mul(ew, qx), B::mul(ex, qw)), B::sub(B::mul(ey, qz), B::mul(ez, qy)));
            const f4 ny = B::add(B::add(B::mul(ew, qy), B::mul(ey, qw)), B::sub(B::mul(ez, qx), B::mul(ex, qz)));
            const f4 nz = B::add(B::add(B::mul(ew, qz), B::mul(ez, qw)), B::sub(B::mul(ex, qy), B::mul(ey, qx)));
            const f4 nw = B::sub(B::mul(ew, qw), dot3(ex, ey, ez, qx, qy, qz));
            qx = nx; qy = ny; qz = nz; qw = nw;
        }

        // e * q, both per lane
        static void quatMulLanes(f4 ex, f4 ey, f4 ez, f4 ew, f4& qx, f4& qy, f4& qz, f4& qw){
            const f4 nx = B::add(B::add(B::mul(ew, qx), B::mul(ex, qw)), B::sub(B::mul(ey, qz), B::mul(ez, qy)));
            const f4 ny = B::add(B::add(B::mul(ew, qy), B::mul(ey, qw)), B::sub(B::mul(ez, qx), B::mul(ex, qz)));
            const f4 nz = B::add(B::add(B::mul(ew, qz), B::mul(ez, qw)), B::sub(B::mul(ex, qy), B::mul(ey, qx)));
            const f4 nw = B::sub(B::mul(ew, qw), dot3(ex, ey, ez, qx, qy, qz));
            qx = nx; qy = ny; qz = nz; qw = nw;
        }

        // the shortest rotation taking +Y onto the unit direction d, as a quaternion (x, y, z, w);
        // d = -Y turns about X by 180 degrees
        static void fromUpTo(f4 dx, f4 dy, f4 dz, f4& qx, f4& qy, f4& qz, f4& qw){
            // q = (up x d, 1 + up . d) normalized, up = (0, 1, 0): (dz, 0, -dx, 1 + dy)
            const f4 w = B::add(k(1.0f), dy);
            const u4 opposite = B::cmplt(w, k(1.0e-6f));
            f4 x = B::select(opposite, k(1.0f), dz);
            f4 y = B::zero();
            f4 z = B::select(opposite, B::zero(), B::neg(dx));
            f4 ww = B::select(opposite, B::zero(), w);
            const f4 len = B::sqrt(B::add(B::add(B::mul(x, x), B::mul(y, y)), B::add(B::mul(z, z), B::mul(ww, ww))));
            qx = B::div(x, len); qy = B::div(y, len); qz = B::div(z, len); qw = B::div(ww, len);
        }

        // a uniform direction on the unit sphere from two randoms
        static void unitSphere(f4 u, f4 v, f4& x, f4& y, f4& z){
            const f4 cy = B::sub(B::mul(u, k(2.0f)), k(1.0f));
            const f4 r = B::sqrt(B::max(B::sub(k(1.0f), B::mul(cy, cy)), B::zero()));
            f4 s, c;
            M::sincos(B::mul(v, k(6.28318530717958647692f)), s, c);
            x = B::mul(r, c);
            y = cy;
            z = B::mul(r, s);
        }

        // ------------------------------------------------------------ curves and gradients

        // Per-block driver values of the curves (computed on first use)
        struct Lane {
            const KernelContext& ctx;
            uint32_t i;
            f4 age, invLife, normAge;
            u4 spawn;
            f4 speed;
            bool hasSpeed = false;
            Lane(const KernelContext& c, uint32_t index): ctx(c), i(index){
                age = ld(c.ch[ChAge], index);
                invLife = ld(c.ch[ChInvLife], index);
                normAge = B::mul(age, invLife);
                spawn = ldu(c.ch[ChSpawnIndex], index);
            }
            f4 getSpeed(){
                if (!hasSpeed){
                    speed = length3(ld(ctx.ch[ChVelX], i), ld(ctx.ch[ChVelY], i), ld(ctx.ch[ChVelZ], i));
                    hasSpeed = true;
                }
                return speed;
            }
        };

        static f4 wrap(f4 t, CurveWrap w){
            switch (w){
                case CurveWrap::Repeat: return M::fract(t);
                case CurveWrap::PingPong: {
                    const f4 m = B::sub(t, B::mul(B::floor(B::mul(t, k(0.5f))), k(2.0f)));
                    return B::select(B::cmpgt(m, k(1.0f)), B::sub(k(2.0f), m), m);
                }
                case CurveWrap::Clamp:
                default: return M::clamp01(t);
            }
        }

        static void gatherIndex(const f4 t, uint32_t size, uint32_t idx[4], f4& frac){
            const f4 x = B::mul(t, k((float)(size - 1)));
            const u4 i = B::cvtt(x);
            alignas(16) uint32_t v[4];
            B::storeu4(v, i);
            for (int l = 0; l < 4; l++){
                const uint32_t max = size - 2;
                idx[l] = v[l] > max ? max : v[l];
            }
            frac = B::sub(x, B::cvt(B::iset(idx[0], idx[1], idx[2], idx[3])));
        }

        static f4 lut(const unsigned char* pool, uint32_t offset, uint32_t size, f4 t){
            const float* table = reinterpret_cast<const float*>(pool + offset);
            uint32_t idx[4];
            f4 f;
            gatherIndex(t, size, idx, f);
            const f4 a = B::set(table[idx[0]], table[idx[1]], table[idx[2]], table[idx[3]]);
            const f4 b = B::set(table[idx[0] + 1], table[idx[1] + 1], table[idx[2] + 1], table[idx[3] + 1]);
            return B::madd(B::sub(b, a), f, a);
        }

        static void lut4(const unsigned char* pool, uint32_t offset, uint32_t size, f4 t, f4 out[4]){
            const float* table = reinterpret_cast<const float*>(pool + offset);
            uint32_t idx[4];
            f4 f;
            gatherIndex(t, size, idx, f);
            for (int c = 0; c < 4; c++){
                const f4 a = B::set(table[idx[0] * 4 + c], table[idx[1] * 4 + c], table[idx[2] * 4 + c], table[idx[3] * 4 + c]);
                const f4 b = B::set(table[idx[0] * 4 + 4 + c], table[idx[1] * 4 + 4 + c], table[idx[2] * 4 + 4 + c], table[idx[3] * 4 + 4 + c]);
                out[c] = B::madd(B::sub(b, a), f, a);
            }
        }

        static f4 driverValue(CurveDriver driver, uint32_t stream, float scale, float offset, Lane& L){
            f4 raw;
            switch (driver){
                case CurveDriver::Age: raw = L.age; break;
                case CurveDriver::Speed: raw = L.getSpeed(); break;
                case CurveDriver::EmitterTime: raw = k(L.ctx.emitterTime); break;
                case CurveDriver::Random: raw = rand01(L.ctx.seed ^ stream ^ 0x68e31da4u, L.spawn); break;
                case CurveDriver::NormalizedAge:
                default: raw = L.normAge; break;
            }
            return B::madd(raw, k(scale), k(offset));
        }

        // `salt` changes the random pick (per-tick randomness)
        static f4 curve(const CurveParam& c, Lane& L, uint32_t salt = 0){
            switch (c.kind){
                case CurveParam::Const: return k(c.c0);
                case CurveParam::Range: return B::madd(k(c.c1 - c.c0), rand01(L.ctx.seed ^ c.stream ^ salt, L.spawn), k(c.c0));
                case CurveParam::Lut: {
                    const f4 t = wrap(driverValue(c.driver, c.stream, c.scale, c.offset, L), c.wrap);
                    return lut(L.ctx.pool, c.lut0, c.lutSize, t);
                }
                case CurveParam::LutRange: {
                    const f4 t = wrap(driverValue(c.driver, c.stream, c.scale, c.offset, L), c.wrap);
                    const f4 a = lut(L.ctx.pool, c.lut0, c.lutSize, t);
                    const f4 b = lut(L.ctx.pool, c.lut1, c.lutSize, t);
                    return B::madd(B::sub(b, a), rand01(L.ctx.seed ^ c.stream ^ salt, L.spawn), a);
                }
            }
            return k(c.c0);
        }

        // A Ramp's step per lane (S49 R12, pixelfx at()): floor(t^bias n), reversed on request
        static void rampIndex(const GradientParam& g, Lane& L, uint32_t idx[4]){
            f4 t = wrap(driverValue(g.driver, g.stream, g.scale, g.offset, L), g.wrap);
            if (g.rampBias != 1.0f)
                t = pow(t, g.rampBias);
            const float n = (float)g.lutSize;
            const f4 step = B::min(B::max(B::floor(B::mul(t, k(n))), B::zero()), k(n - 1.0f));
            alignas(16) float v[4];
            B::storeu(v, step);
            for (int l = 0; l < 4; l++){
                const uint32_t s = (uint32_t)v[l];
                idx[l] = g.rampReverse ? g.lutSize - 1u - s : s;
            }
        }

        static void gradient(const GradientParam& g, Lane& L, f4 out[4]){
            switch (g.kind){
                case GradientParam::Const:
                    for (int c = 0; c < 4; c++) out[c] = k(g.c0[c]);
                    return;
                case GradientParam::Range: {
                    const f4 r = rand01(L.ctx.seed ^ g.stream, L.spawn);
                    for (int c = 0; c < 4; c++) out[c] = B::madd(k(g.c1[c] - g.c0[c]), r, k(g.c0[c]));
                    return;
                }
                case GradientParam::Lut: {
                    const f4 t = wrap(driverValue(g.driver, g.stream, g.scale, g.offset, L), g.wrap);
                    lut4(L.ctx.pool, g.lut0, g.lutSize, t, out);
                    return;
                }
                case GradientParam::Ramp: {
                    alignas(16) uint32_t idx[4];
                    rampIndex(g, L, idx);
                    const float* table = reinterpret_cast<const float*>(L.ctx.pool + g.lut0);
                    for (int c = 0; c < 4; c++)
                        out[c] = B::set(table[idx[0] * 4 + c], table[idx[1] * 4 + c], table[idx[2] * 4 + c], table[idx[3] * 4 + c]);
                    return;
                }
                case GradientParam::LutRange: {
                    const f4 t = wrap(driverValue(g.driver, g.stream, g.scale, g.offset, L), g.wrap);
                    f4 a[4], b[4];
                    lut4(L.ctx.pool, g.lut0, g.lutSize, t, a);
                    lut4(L.ctx.pool, g.lut1, g.lutSize, t, b);
                    const f4 r = rand01(L.ctx.seed ^ g.stream, L.spawn);
                    for (int c = 0; c < 4; c++) out[c] = B::madd(B::sub(b[c], a[c]), r, a[c]);
                    return;
                }
            }
        }

        static f4 dtAt(const KernelContext& ctx, uint32_t i){
            return ctx.dtLanes ? ld(ctx.dtLanes, i) : k(ctx.dt);
        }

        // ln x for x > 0: the float's exponent and an odd series of its mantissa (error < 3e-6)
        static f4 log(f4 x){
            const u4 bits = B::castu(x);
            const f4 e = B::cvt(B::isub(B::template isrl<23>(bits), B::iset1(127)));
            const f4 m = B::castf(B::ior(B::iand(bits, B::iset1(0x007fffffu)), B::iset1(0x3f800000u)));   // [1, 2)
            const f4 t = B::div(B::sub(m, k(1.0f)), B::add(m, k(1.0f)));
            const f4 t2 = B::mul(t, t);
            f4 p = k(2.0f / 9.0f);
            p = B::madd(p, t2, k(2.0f / 7.0f));
            p = B::madd(p, t2, k(2.0f / 5.0f));
            p = B::madd(p, t2, k(2.0f / 3.0f));
            p = B::madd(p, t2, k(2.0f));
            return B::madd(e, k(0.69314718056f), B::mul(p, t));
        }

        // x^power for x >= 0 (0 stays 0)
        static f4 pow(f4 x, float power){
            const f4 v = M::exp(B::mul(log(B::max(x, k(1.0e-10f))), k(power)));
            return B::select(B::cmple(x, k(1.0e-10f)), B::zero(), v);
        }

        // RGBA8 with the rgb sRGB-encoded (the gamma 2.2 of util/Color) from linear rgba
        static u4 packColor(f4 r, f4 g, f4 b, f4 a){
            auto enc = [](f4 x){
                const f4 v = pow(M::clamp01(x), 1.0f / 2.2f);
                return B::cvtt(B::add(B::mul(M::clamp01(v), k(255.0f)), k(0.5f)));
            };
            const u4 ri = enc(r), gi = enc(g), bi = enc(b);
            const u4 ai = B::cvtt(B::add(B::mul(M::clamp01(a), k(255.0f)), k(0.5f)));
            return B::ior(B::ior(ri, B::template isll<8>(gi)), B::ior(B::template isll<16>(bi), B::template isll<24>(ai)));
        }

        // linear rgba from RGBA8 sRGB (x^2.2 through the table of decodeTable())
        static void unpackColor(u4 c, const float* decode, f4 out[4]){
            alignas(16) uint32_t v[4];
            B::storeu4(v, c);
            for (int ch = 0; ch < 3; ch++){
                const int s = ch * 8;
                out[ch] = B::set(decode[(v[0] >> s) & 255u], decode[(v[1] >> s) & 255u], decode[(v[2] >> s) & 255u], decode[(v[3] >> s) & 255u]);
            }
            out[3] = B::mul(B::cvt(B::template isrl<24>(c)), k(1.0f / 255.0f));
        }

        // ------------------------------------------------------------ spawn: shape

        // A shape mode's 0..1 value (Unity's ParticleSystemShapeMultiModeValue): the random pick,
        // the phase around (Loop) or back and forth (Ping-Pong), a burst's particles spread
        // evenly (Burst Spread; out of a burst at random), then held at multiples of `spread`
        static f4 multiMode(ShapeMultiMode mode, float spread, f4 random, f4 phase, const KernelContext& ctx, uint32_t first){
            f4 v;
            switch (mode){
                case ShapeMultiMode::Loop:
                    v = M::fract(phase);
                    break;
                case ShapeMultiMode::PingPong: {
                    const f4 m = B::sub(phase, B::mul(B::floor(B::mul(phase, k(0.5f))), k(2.0f)));
                    v = B::select(B::cmpgt(m, k(1.0f)), B::sub(k(2.0f), m), m);
                    break;
                }
                case ShapeMultiMode::BurstSpread:
                    if (ctx.spreadCount > 0){
                        const float base = (float)(ctx.spreadBase + first);
                        v = B::div(B::add(k(base), B::set(0.0f, 1.0f, 2.0f, 3.0f)), k((float)ctx.spreadCount));
                    }else{
                        v = random;
                    }
                    break;
                case ShapeMultiMode::Random:
                default:
                    v = random;
                    break;
            }
            if (spread > 0.0f){
                const float step = std::min(spread, 1.0f);
                v = B::min(B::mul(B::floor(B::add(B::div(v, k(step)), k(1.0e-4f))), k(step)), k(1.0f));
            }
            return v;
        }

        static void spawnShape(const KernelContext& ctx, const ShapeParams& p, uint32_t begin, uint32_t end){
            const uint32_t key = ctx.seed ^ randomSalt((uint32_t)ModuleId::Shape, 0);
            const bool arcModed = p.arcMode != ShapeMultiMode::Random || p.arcSpread > 0.0f;
            const bool radiusModed = p.radiusMode != ShapeMultiMode::Random || p.radiusSpread > 0.0f;
            for (uint32_t i = begin; i < end; i += 4){
                const u4 spawn = ldu(ctx.ch[ChSpawnIndex], i);
                auto rnd = [&](uint32_t stream){ return rand01(key ^ (stream * 0x27d4eb2fu), spawn); };
                f4 px = B::zero(), py = B::zero(), pz = B::zero();
                f4 dx = B::zero(), dy = k(1.0f), dz = B::zero();
                // the arc's and the radius's 0..1 picks (where in the tick it spawns moves the phases)
                f4 arcPick = rnd(1);
                if (arcModed){
                    const f4 at = B::mul(ld(ctx.spawnFraction, i), k(ctx.dt));
                    const f4 phase = p.arcDegrees > 0.0f ? B::div(B::madd(at, k(ctx.arcRate), k(ctx.arcPhase)), k(p.arcDegrees)) : B::zero();
                    arcPick = multiMode(p.arcMode, p.arcSpread, arcPick, phase, ctx, i);
                }
                auto radiusPick = [&](uint32_t stream){
                    const f4 random = rnd(stream);
                    if (!radiusModed) return random;
                    const f4 at = B::mul(ld(ctx.spawnFraction, i), k(ctx.dt));
                    return multiMode(p.radiusMode, p.radiusSpread, random, B::madd(at, k(ctx.radiusRate), k(ctx.radiusPhase)), ctx, i);
                };
                const f4 arcAngle = B::mul(arcPick, k(p.arc));
                f4 sa, ca;
                M::sincos(arcAngle, sa, ca);
                switch (p.kind){
                    case SpawnShapeKind::Point:
                        break;
                    case SpawnShapeKind::Sphere:
                    case SpawnShapeKind::Hemisphere: {
                        // y uniform in [-1, 1] (Hemisphere: [0, 1]), the azimuth within the arc
                        const f4 u = rnd(2);
                        const f4 cy = p.kind == SpawnShapeKind::Sphere ? B::sub(B::mul(u, k(2.0f)), k(1.0f)) : u;
                        const f4 rr = B::sqrt(B::max(B::sub(k(1.0f), B::mul(cy, cy)), B::zero()));
                        dx = B::mul(rr, ca);
                        dy = cy;
                        dz = B::mul(rr, sa);
                        // radius uniform in the shell's volume: cbrt(lerp(inner^3, 1, u))
                        const float inner3 = p.innerRatio * p.innerRatio * p.innerRatio;
                        const f4 r = B::mul(M::cbrt(B::madd(k(1.0f - inner3), radiusPick(3), k(inner3))), k(p.radius));
                        px = B::mul(dx, r);
                        py = B::mul(dy, r);
                        pz = B::mul(dz, r);
                        break;
                    }
                    case SpawnShapeKind::Circle: {
                        const float inner2 = p.innerRatio * p.innerRatio;
                        const f4 r = B::mul(B::sqrt(B::madd(k(1.0f - inner2), radiusPick(3), k(inner2))), k(p.radius));
                        dx = ca;
                        dy = B::zero();
                        dz = sa;
                        px = B::mul(ca, r);
                        pz = B::mul(sa, r);
                        break;
                    }
                    case SpawnShapeKind::Cone: {
                        // base disc radius R at y = 0 opening by the angle along +Y; a base point at
                        // normalized radius rho leaves tilted by rho * tan(angle)
                        const float inner2 = p.innerRatio * p.innerRatio;
                        const bool shell = p.emitFrom == ShapeEmitFrom::Shell || p.emitFrom == ShapeEmitFrom::Edge;
                        const f4 rho = shell ? k(1.0f) : B::sqrt(B::madd(k(1.0f - inner2), radiusPick(3), k(inner2)));
                        const f4 tilt = B::mul(rho, k(p.tanAngle));
                        const f4 inv = B::div(k(1.0f), B::sqrt(B::add(B::mul(tilt, tilt), k(1.0f))));
                        dx = B::mul(B::mul(ca, tilt), inv);
                        dy = inv;
                        dz = B::mul(B::mul(sa, tilt), inv);
                        f4 h = B::zero();
                        if (p.emitFrom == ShapeEmitFrom::Volume){
                            // height with the density of the frustum's area: inverse CDF of (R + y tan)^2
                            const f4 u = rnd(4);
                            if (p.tanAngle > 1.0e-6f){
                                const float r0 = p.radius;
                                const float r1 = p.radius + p.length * p.tanAngle;
                                const float r03 = r0 * r0 * r0;
                                const f4 c = M::cbrt(B::madd(k(r1 * r1 * r1 - r03), u, k(r03)));
                                h = B::div(B::sub(c, k(r0)), k(p.tanAngle));
                            }else{
                                h = B::mul(u, k(p.length));
                            }
                        }
                        const f4 radiusAt = B::madd(h, k(p.tanAngle), k(p.radius));
                        px = B::mul(B::mul(ca, rho), radiusAt);
                        py = h;
                        pz = B::mul(B::mul(sa, rho), radiusAt);
                        break;
                    }
                    case SpawnShapeKind::Box: {
                        const f4 ux = B::sub(B::mul(rnd(2), k(2.0f)), k(1.0f));
                        const f4 uy = B::sub(B::mul(rnd(3), k(2.0f)), k(1.0f));
                        const f4 uz = B::sub(B::mul(rnd(4), k(2.0f)), k(1.0f));
                        if (p.emitFrom == ShapeEmitFrom::Volume){
                            px = B::mul(ux, k(p.boxHalf[0]));
                            py = B::mul(uy, k(p.boxHalf[1]));
                            pz = B::mul(uz, k(p.boxHalf[2]));
                        }else{
                            // Shell: a face picked by area, pushed in by the thickness; Edge: one of 12 edges by length
                            const f4 pick = rnd(5);
                            const float hx = p.boxHalf[0], hy = p.boxHalf[1], hz = p.boxHalf[2];
                            f4 sx = ux, sy = uy, sz = uz;
                            const f4 sign = B::select(B::cmplt(rnd(6), k(0.5f)), k(-1.0f), k(1.0f));
                            if (p.emitFrom == ShapeEmitFrom::Shell){
                                const float ax = hy * hz, ay = hx * hz, az = hx * hy;
                                const float total = ax + ay + az;
                                const u4 onX = B::cmplt(pick, k(total > 0.0f ? ax / total : 1.0f / 3.0f));
                                const u4 onY = B::iandnot(onX, B::cmplt(pick, k(total > 0.0f ? (ax + ay) / total : 2.0f / 3.0f)));
                                const u4 onZ = B::iandnot(B::ior(onX, onY), B::iset1(0xffffffffu));
                                // the face coordinate between the inner and outer half extent
                                const f4 depth = rnd(7);
                                sx = B::select(onX, B::mul(sign, B::madd(k(1.0f - p.boxInner[0]), depth, k(p.boxInner[0]))), sx);
                                sy = B::select(onY, B::mul(sign, B::madd(k(1.0f - p.boxInner[1]), depth, k(p.boxInner[1]))), sy);
                                sz = B::select(onZ, B::mul(sign, B::madd(k(1.0f - p.boxInner[2]), depth, k(p.boxInner[2]))), sz);
                            }else{
                                // an edge along an axis picked by length: that coordinate uniform, the other two at a corner
                                const float total = hx + hy + hz;
                                const u4 alongX = B::cmplt(pick, k(total > 0.0f ? hx / total : 1.0f / 3.0f));
                                const u4 alongY = B::iandnot(alongX, B::cmplt(pick, k(total > 0.0f ? (hx + hy) / total : 2.0f / 3.0f)));
                                const u4 alongZ = B::iandnot(B::ior(alongX, alongY), B::iset1(0xffffffffu));
                                const f4 sign2 = B::select(B::cmplt(rnd(7), k(0.5f)), k(-1.0f), k(1.0f));
                                sx = B::select(alongX, ux, sign);
                                sy = B::select(alongY, uy, B::select(alongX, sign, sign2));
                                sz = B::select(alongZ, uz, sign2);
                            }
                            px = B::mul(sx, k(hx));
                            py = B::mul(sy, k(hy));
                            pz = B::mul(sz, k(hz));
                        }
                        break;
                    }
                    case SpawnShapeKind::Donut: {
                        // a ring of radius R in the XZ plane, a tube of radius donutRadius around it
                        const f4 tube = B::mul(rnd(2), k(6.28318530717958647692f));
                        f4 sinT, cosT;
                        M::sincos(tube, sinT, cosT);
                        const float inner2 = p.innerRatio * p.innerRatio;
                        const f4 rt = B::mul(B::sqrt(B::madd(k(1.0f - inner2), radiusPick(3), k(inner2))), k(p.donutRadius));
                        // outward from the tube's centre line
                        dx = B::mul(ca, cosT);
                        dy = sinT;
                        dz = B::mul(sa, cosT);
                        px = B::madd(dx, rt, B::mul(ca, k(p.radius)));
                        py = B::mul(dy, rt);
                        pz = B::madd(dz, rt, B::mul(sa, k(p.radius)));
                        break;
                    }
                    case SpawnShapeKind::Edge: {
                        // a line along X, radius on each side (the radius mode picks along it)
                        px = B::mul(B::sub(B::mul(radiusPick(2), k(2.0f)), k(1.0f)), k(p.radius));
                        break;
                    }
                    case SpawnShapeKind::Rectangle: {
                        px = B::mul(B::sub(B::mul(rnd(2), k(2.0f)), k(1.0f)), k(p.boxHalf[0]));
                        pz = B::mul(B::sub(B::mul(rnd(3), k(2.0f)), k(1.0f)), k(p.boxHalf[2]));
                        break;
                    }
                    case SpawnShapeKind::Mesh: {
                        meshSample(ctx, p, spawn, key, px, py, pz, dx, dy, dz);
                        break;
                    }
                }
                if (p.randomizePosition > 0.0f){
                    f4 rx, ry, rz;
                    unitSphere(rnd(10), rnd(11), rx, ry, rz);
                    const f4 amount = B::mul(rnd(12), k(p.randomizePosition));
                    px = B::madd(rx, amount, px);
                    py = B::madd(ry, amount, py);
                    pz = B::madd(rz, amount, pz);
                }
                if (p.spherizeDirection > 0.0f){
                    const f4 len = length3(px, py, pz);
                    const f4 sx = safeDiv(px, len), sy = safeDiv(py, len), sz = safeDiv(pz, len);
                    dx = M::lerp(dx, sx, k(p.spherizeDirection));
                    dy = M::lerp(dy, sy, k(p.spherizeDirection));
                    dz = M::lerp(dz, sz, k(p.spherizeDirection));
                }
                if (p.randomizeDirection > 0.0f){
                    f4 rx, ry, rz;
                    unitSphere(rnd(13), rnd(14), rx, ry, rz);
                    dx = M::lerp(dx, rx, k(p.randomizeDirection));
                    dy = M::lerp(dy, ry, k(p.randomizeDirection));
                    dz = M::lerp(dz, rz, k(p.randomizeDirection));
                }
                {
                    const f4 len = length3(dx, dy, dz);
                    dx = safeDiv(dx, len);
                    dy = safeDiv(dy, len);
                    dz = safeDiv(dz, len);
                }
                // the shape's own transform, then the emitter's (blended to the spawn's moment)
                f4 sx, sy, sz, tx, ty, tz;
                xformPoint(p.transform, px, py, pz, sx, sy, sz);
                xformDir(p.rotation, dx, dy, dz, tx, ty, tz);
                const f4 f = ld(ctx.spawnFraction, i);
                f4 wx, wy, wz, vx, vy, vz;
                xformPointLerp(ctx.prevEmitterToSim, ctx.emitterToSim, f, sx, sy, sz, wx, wy, wz);
                xformDirLerp(ctx.prevEmitterToSimDir, ctx.emitterToSimDir, f, tx, ty, tz, vx, vy, vz);
                st(ctx.ch[ChPosX], i, wx);
                st(ctx.ch[ChPosY], i, wy);
                st(ctx.ch[ChPosZ], i, wz);
                // the direction, until SpawnVelocity scales it by the start speed
                st(ctx.ch[ChVelX], i, vx);
                st(ctx.ch[ChVelY], i, vy);
                st(ctx.ch[ChVelZ], i, vz);
                // Align to Direction: kept for SpawnStartValues (the velocity is no direction by then)
                if (p.alignToDirection && ctx.scratch){
                    st(ctx.scratch->ax, i, vx);
                    st(ctx.scratch->ay, i, vy);
                    st(ctx.scratch->az, i, vz);
                }
            }
        }

        static void meshSample(const KernelContext& ctx, const ShapeParams& p, u4 spawn, uint32_t key,
                               f4& px, f4& py, f4& pz, f4& dx, f4& dy, f4& dz){
            const float* verts = reinterpret_cast<const float*>(ctx.pool + p.meshVertices);
            const float* normals = reinterpret_cast<const float*>(ctx.pool + p.meshNormals);
            const uint32_t* tris = reinterpret_cast<const uint32_t*>(ctx.pool + p.meshIndices);
            const float* cdf = reinterpret_cast<const float*>(ctx.pool + p.meshAreaCdf);
            alignas(16) float u[4], v[4], w[4], outP[3][4], outN[3][4];
            B::storeu(u, rand01(key ^ 0x3c6ef372u, spawn));
            B::storeu(v, rand01(key ^ 0xa54ff53au, spawn));
            B::storeu(w, rand01(key ^ 0x510e527fu, spawn));
            for (int l = 0; l < 4; l++){
                float pos[3] = {0, 0, 0}, nrm[3] = {0, 1, 0};
                if (p.emitFrom == ShapeEmitFrom::Vertex && p.meshVertexCount > 0){
                    uint32_t vi = (uint32_t)(u[l] * (float)p.meshVertexCount);
                    if (vi >= p.meshVertexCount) vi = p.meshVertexCount - 1;
                    for (int c = 0; c < 3; c++){ pos[c] = verts[vi * 3 + c]; nrm[c] = normals[vi * 3 + c]; }
                }else if (p.meshTriangles > 0){
                    // the triangle by area (binary search of the cumulative table)
                    uint32_t lo = 0, hi = p.meshTriangles - 1;
                    while (lo < hi){
                        const uint32_t mid = (lo + hi) / 2;
                        if (cdf[mid] < u[l]) lo = mid + 1; else hi = mid;
                    }
                    const uint32_t a = tris[lo * 3], b = tris[lo * 3 + 1], c = tris[lo * 3 + 2];
                    float s = v[l], t = w[l];
                    if (p.emitFrom == ShapeEmitFrom::Edge){
                        // a point on one of the three edges
                        const uint32_t edge = (uint32_t)(t * 3.0f);
                        const uint32_t e0 = edge == 0 ? a : (edge == 1 ? b : c);
                        const uint32_t e1 = edge == 0 ? b : (edge == 1 ? c : a);
                        for (int d = 0; d < 3; d++){
                            pos[d] = verts[e0 * 3 + d] + (verts[e1 * 3 + d] - verts[e0 * 3 + d]) * s;
                            nrm[d] = normals[e0 * 3 + d] + (normals[e1 * 3 + d] - normals[e0 * 3 + d]) * s;
                        }
                    }else{
                        if (s + t > 1.0f){ s = 1.0f - s; t = 1.0f - t; }
                        const float r = 1.0f - s - t;
                        for (int d = 0; d < 3; d++){
                            pos[d] = verts[a * 3 + d] * r + verts[b * 3 + d] * s + verts[c * 3 + d] * t;
                            nrm[d] = normals[a * 3 + d] * r + normals[b * 3 + d] * s + normals[c * 3 + d] * t;
                        }
                    }
                }
                for (int c = 0; c < 3; c++){ outP[c][l] = pos[c]; outN[c][l] = nrm[c]; }
            }
            px = B::loadu(outP[0]); py = B::loadu(outP[1]); pz = B::loadu(outP[2]);
            dx = B::loadu(outN[0]); dy = B::loadu(outN[1]); dz = B::loadu(outN[2]);
        }

        // ------------------------------------------------------------ spawn: velocity

        static void spawnVelocity(const KernelContext& ctx, const VelocityParams& p, uint32_t begin, uint32_t end){
            const uint32_t keyV = ctx.seed ^ randomSalt((uint32_t)ModuleId::StartVelocity, 0);
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                const f4 speed = curve(p.startSpeed, L);
                f4 vx = B::mul(ld(ctx.ch[ChVelX], i), speed);
                f4 vy = B::mul(ld(ctx.ch[ChVelY], i), speed);
                f4 vz = B::mul(ld(ctx.ch[ChVelZ], i), speed);
                if (p.hasStartVelocity){
                    f4 rx, ry, rz;
                    if (p.startVelocityUniform){
                        rx = ry = rz = rand01(keyV, L.spawn);
                    }else{
                        rx = rand01(keyV ^ 0x1u, L.spawn);
                        ry = rand01(keyV ^ 0x2u, L.spawn);
                        rz = rand01(keyV ^ 0x3u, L.spawn);
                    }
                    const f4 lx = B::madd(k(p.velMax[0] - p.velMin[0]), rx, k(p.velMin[0]));
                    const f4 ly = B::madd(k(p.velMax[1] - p.velMin[1]), ry, k(p.velMin[1]));
                    const f4 lz = B::madd(k(p.velMax[2] - p.velMin[2]), rz, k(p.velMin[2]));
                    f4 sx, sy, sz;
                    const f4 f = ld(ctx.spawnFraction, i);
                    xformDirLerp(ctx.prevEmitterToSimDir, ctx.emitterToSimDir, f, lx, ly, lz, sx, sy, sz);
                    vx = B::add(vx, sx);
                    vy = B::add(vy, sy);
                    vz = B::add(vz, sz);
                }
                if (p.hasInherit){
                    const f4 share = curve(p.inherit, L);
                    vx = B::madd(k(ctx.emitterVelocity[0]), share, vx);
                    vy = B::madd(k(ctx.emitterVelocity[1]), share, vy);
                    vz = B::madd(k(ctx.emitterVelocity[2]), share, vz);
                }
                st(ctx.ch[ChVelX], i, vx);
                st(ctx.ch[ChVelY], i, vy);
                st(ctx.ch[ChVelZ], i, vz);
            }
        }

        // ------------------------------------------------------------ spawn: start values

        static void spawnStartValues(const KernelContext& ctx, const StartParams& p, uint32_t begin, uint32_t end){
            const uint32_t flipKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::Main, 60);
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                const f4 life = B::max(curve(p.lifetime, L), k(1.0e-4f));
                st(ctx.ch[ChInvLife], i, B::div(k(1.0f), life));
                st(ctx.ch[ChAge], i, B::zero());
                const f4 size = B::mul(B::max(curve(p.size, L), B::zero()), k(ctx.sizeScale));
                st(ctx.ch[ChStartSize], i, size);
                f4 sizeY = size, sizeZ = size;
                if (p.size3D){
                    sizeY = B::mul(B::max(curve(p.sizeY, L), B::zero()), k(ctx.sizeScale));
                    sizeZ = B::mul(B::max(curve(p.sizeZ, L), B::zero()), k(ctx.sizeScale));
                    st(ctx.ch[ChStartSizeY], i, sizeY);
                    st(ctx.ch[ChStartSizeZ], i, sizeZ);
                }
                f4 color[4];
                gradient(p.color, L, color);
                if (p.color.kind == GradientParam::Ramp){
                    // a ramp step's own bytes (S49 R13): no round trip through linear
                    alignas(16) uint32_t idx[4];
                    rampIndex(p.color, L, idx);
                    const uint32_t* bytes = reinterpret_cast<const uint32_t*>(ctx.pool + p.color.lut1);
                    stu(ctx.ch[ChStartColor], i, B::iset(bytes[idx[0]], bytes[idx[1]], bytes[idx[2]], bytes[idx[3]]));
                }else{
                    stu(ctx.ch[ChStartColor], i, packColor(color[0], color[1], color[2], color[3]));
                }
                f4 rot = B::zero();
                if (p.storeRotation || p.initRotation || p.storeQuat){
                    rot = curve(p.rotation, L);
                    if (p.flipRotation > 0.0f){
                        rot = B::select(B::cmplt(rand01(flipKey, L.spawn), k(p.flipRotation)), B::neg(rot), rot);
                    }
                }
                const bool align = p.alignToDirection && ctx.scratch;
                f4 adx = B::zero(), ady = k(1.0f), adz = B::zero();
                if (align){
                    adx = ld(ctx.scratch->ax, i);
                    ady = ld(ctx.scratch->ay, i);
                    adz = ld(ctx.scratch->az, i);
                    if (!p.storeQuat){
                        // a billboard's roll: its up turned towards the direction's X and Y (clockwise)
                        const f4 xy = B::add(B::mul(adx, adx), B::mul(ady, ady));
                        rot = B::add(rot, B::select(B::cmpgt(xy, k(1.0e-12f)), M::atan2(adx, ady), B::zero()));
                    }
                }
                if (p.storeRotation)
                    st(ctx.ch[ChStartRotation], i, rot);
                if (p.initRotation){
                    st(ctx.ch[ChRotation], i, rot);
                    st(ctx.ch[ChPrevRotation], i, rot);
                }
                if (p.storeQuat){
                    // Euler (x, y, z) radians to a quaternion (z applied first, then y, then x... Unity: ZXY)
                    const f4 rx = p.rotation3D ? curve(p.rotationX, L) : B::zero();
                    const f4 ry = p.rotation3D ? curve(p.rotationY, L) : B::zero();
                    f4 sx, cx, sy, cy, sz, cz;
                    M::sincos(B::mul(rx, k(0.5f)), sx, cx);
                    M::sincos(B::mul(ry, k(0.5f)), sy, cy);
                    M::sincos(B::mul(rot, k(0.5f)), sz, cz);
                    // q = qy * qx * qz
                    f4 qw = B::add(B::mul(B::mul(cy, cx), cz), B::mul(B::mul(sy, sx), sz));
                    f4 qx = B::add(B::mul(B::mul(cy, sx), cz), B::mul(B::mul(sy, cx), sz));
                    f4 qy = B::sub(B::mul(B::mul(sy, cx), cz), B::mul(B::mul(cy, sx), sz));
                    f4 qz = B::sub(B::mul(B::mul(cy, cx), sz), B::mul(B::mul(sy, sx), cz));
                    if (align){
                        // up (+Y) turned onto the direction (simulation space, the emitter's turn in it)
                        // as the shortest rotation, applied after the start rotation
                        f4 ax, ay, az, aw;
                        fromUpTo(adx, ady, adz, ax, ay, az, aw);
                        quatMulLanes(ax, ay, az, aw, qx, qy, qz, qw);
                    }else{
                        // in World space the emitter's rotation at birth turns it
                        quatMulUniform(ctx.emitterToSimQuat, qx, qy, qz, qw);
                    }
                    st(ctx.ch[ChQuatX], i, qx);
                    st(ctx.ch[ChQuatY], i, qy);
                    st(ctx.ch[ChQuatZ], i, qz);
                    st(ctx.ch[ChQuatW], i, qw);
                }
                if (p.initAngularVelocity){
                    st(ctx.ch[ChAngVelX], i, curve(p.angularVelocityX, L));
                    st(ctx.ch[ChAngVelY], i, curve(p.angularVelocityY, L));
                    st(ctx.ch[ChAngVelZ], i, curve(p.angularVelocity, L));
                }
                if (p.initSize){
                    st(ctx.ch[ChSizeX], i, size);
                    st(ctx.ch[ChPrevSizeX], i, size);
                    if (ctx.ch[ChSizeY]) st(ctx.ch[ChSizeY], i, sizeY);
                    if (ctx.ch[ChSizeZ]) st(ctx.ch[ChSizeZ], i, sizeZ);
                }
                if (p.initColor){
                    st(ctx.ch[ChColorR], i, color[0]);
                    st(ctx.ch[ChColorG], i, color[1]);
                    st(ctx.ch[ChColorB], i, color[2]);
                    st(ctx.ch[ChColorA], i, color[3]);
                }
                if (p.initAccel){
                    f4 ax = curve(p.accelX, L), ay = curve(p.accelY, L), az = curve(p.accelZ, L);
                    f4 sx, sy, sz;
                    xformDir(p.accelWorld ? ctx.worldToSimDir : ctx.emitterToSimDir, ax, ay, az, sx, sy, sz);
                    st(ctx.ch[ChAccelX], i, sx);
                    st(ctx.ch[ChAccelY], i, sy);
                    st(ctx.ch[ChAccelZ], i, sz);
                }
                if (p.initFrame){
                    f4 frame = B::floor(B::add(curve(p.startFrame, L), k(0.5f)));
                    if (p.startFrameListCount > 0){
                        // the pick the renderer makes too (same key)
                        const uint32_t* list = reinterpret_cast<const uint32_t*>(ctx.pool + p.startFrameList);
                        alignas(16) float r[4], picked[4];
                        B::storeu(r, rand01(ctx.seed ^ randomSalt((uint32_t)ModuleId::TextureSheetAnimation, 4), L.spawn));
                        for (int l = 0; l < 4; l++){
                            uint32_t idx = (uint32_t)(r[l] * (float)p.startFrameListCount);
                            if (idx >= p.startFrameListCount) idx = p.startFrameListCount - 1;
                            picked[l] = (float)list[idx];
                        }
                        frame = B::add(frame, B::loadu(picked));
                    }
                    st(ctx.ch[ChFrame], i, frame);
                }
                if (ctx.ch[ChNoiseSize])
                    st(ctx.ch[ChNoiseSize], i, k(1.0f));
                if (ctx.ch[ChSettled])
                    st(ctx.ch[ChSettled], i, B::zero());
                if (ctx.ch[ChPlaneTick])
                    stu(ctx.ch[ChPlaneTick], i, B::iset1(0u));
                if (ctx.ch[ChAnimVelX]){
                    st(ctx.ch[ChAnimVelX], i, B::zero());
                    st(ctx.ch[ChAnimVelY], i, B::zero());
                    st(ctx.ch[ChAnimVelZ], i, B::zero());
                }
                // the previous position is the birth position (no interpolation from elsewhere)
                st(ctx.ch[ChPrevX], i, ld(ctx.ch[ChPosX], i));
                st(ctx.ch[ChPrevY], i, ld(ctx.ch[ChPosY], i));
                st(ctx.ch[ChPrevZ], i, ld(ctx.ch[ChPosZ], i));
            }
        }

        // ------------------------------------------------------------ update: Set over Age

        static void slerpLanes(const float* a, const float* b, bool shortest, f4 t, f4 q[4]){
            f4 bx = k(b[0]), by = k(b[1]), bz = k(b[2]), bw = k(b[3]);
            f4 d = B::add(B::add(B::mul(k(a[0]), bx), B::mul(k(a[1]), by)), B::add(B::mul(k(a[2]), bz), B::mul(k(a[3]), bw)));
            if (shortest){
                const u4 flip = B::cmplt(d, B::zero());
                bx = B::select(flip, B::neg(bx), bx);
                by = B::select(flip, B::neg(by), by);
                bz = B::select(flip, B::neg(bz), bz);
                bw = B::select(flip, B::neg(bw), bw);
                d = B::abs(d);
            }
            // nlerp where the arc is tiny, the exact slerp elsewhere
            const f4 ad = B::min(B::abs(d), k(1.0f));
            const f4 theta = M::atan2(B::sqrt(B::max(B::sub(k(1.0f), B::mul(ad, ad)), B::zero())), ad);
            const f4 st0 = sin(theta);
            const f4 s0 = sin(B::mul(B::sub(k(1.0f), t), theta));
            const f4 s1 = sin(B::mul(t, theta));
            const u4 small = B::cmplt(st0, k(1.0e-4f));
            f4 wa = B::select(small, B::sub(k(1.0f), t), B::div(s0, st0));
            f4 wb = B::select(small, t, B::div(s1, st0));
            // a negative dot (not taking the shortest path) turns the b side
            wb = B::select(B::cmplt(d, B::zero()), B::neg(wb), wb);
            q[0] = B::madd(k(a[0]), wa, B::mul(bx, wb));
            q[1] = B::madd(k(a[1]), wa, B::mul(by, wb));
            q[2] = B::madd(k(a[2]), wa, B::mul(bz, wb));
            q[3] = B::madd(k(a[3]), wa, B::mul(bw, wb));
        }

        static void setOverAge(const KernelContext& ctx, const SetOverAgeParams& p, uint32_t begin, uint32_t end){
            const SetParams* entries = reinterpret_cast<const SetParams*>(ctx.pool + p.entries);
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                for (uint32_t e = 0; e < p.count; e++){
                    const SetParams& s = entries[e];
                    if (s.fromTime == s.toTime)
                        continue;
                    // the old modifier: inside [from, to] of age, set while the ease is in [0, 1]
                    const f4 t = B::mul(B::sub(L.age, k(s.fromTime)), k(s.invSpan));
                    const u4 inWindow = B::iand(B::cmpge(L.age, k(s.fromTime)), B::cmple(L.age, k(s.toTime)));
                    if (B::movemask(inWindow) == 0)
                        continue;
                    const f4 v = lut(ctx.pool, s.curveLut, s.curveLutSize, M::clamp01(t));
                    const u4 on = B::iand(inWindow, B::iand(B::cmpge(v, B::zero()), B::cmple(v, k(1.0f))));
                    auto value = [&](int c){ return B::madd(k(s.to[c] - s.from[c]), v, k(s.from[c])); };
                    auto put = [&](Channel c, f4 x){ if (ctx.ch[c]) st(ctx.ch[c], i, B::select(on, x, ld(ctx.ch[c], i))); };
                    switch (s.property){
                        case SetProperty::Position: {
                            f4 x = value(0), y = value(1), z = value(2);
                            if (s.worldSpace) xformPoint(ctx.emitterToSim, value(0), value(1), value(2), x, y, z);
                            put(ChPosX, x); put(ChPosY, y); put(ChPosZ, z);
                            break;
                        }
                        case SetProperty::Velocity:
                        case SetProperty::Acceleration: {
                            f4 x = value(0), y = value(1), z = value(2);
                            if (s.worldSpace) xformDir(ctx.emitterToSimDir, value(0), value(1), value(2), x, y, z);
                            if (s.property == SetProperty::Velocity){ put(ChVelX, x); put(ChVelY, y); put(ChVelZ, z); }
                            else { put(ChAccelX, x); put(ChAccelY, y); put(ChAccelZ, z); }
                            break;
                        }
                        case SetProperty::Color: {
                            f4 r = value(0), g = value(1), b = value(2);
                            if (s.srgb){
                                // gamma 2.2 per lane through exp/log of the encode
                                r = srgbToLinear(r); g = srgbToLinear(g); b = srgbToLinear(b);
                            }
                            put(ChColorR, r); put(ChColorG, g); put(ChColorB, b);
                            break;
                        }
                        case SetProperty::Alpha:
                            put(ChColorA, value(0));
                            break;
                        case SetProperty::Size:
                            put(ChSizeX, value(0));
                            break;
                        case SetProperty::Scale:
                            put(ChSizeX, value(0)); put(ChSizeY, value(1)); put(ChSizeZ, value(2));
                            break;
                        case SetProperty::Rotation: {
                            f4 q[4];
                            slerpLanes(s.from, s.to, s.shortestPath != 0, v, q);
                            if (ctx.ch[ChQuatX]){
                                if (s.worldSpace) quatMulUniform(ctx.emitterToSimQuat, q[0], q[1], q[2], q[3]);
                                put(ChQuatX, q[0]); put(ChQuatY, q[1]); put(ChQuatZ, q[2]); put(ChQuatW, q[3]);
                            }
                            if (ctx.ch[ChRotation]){
                                // the roll (rotation about Z) of the quaternion, as the old points took it
                                const f4 roll = M::atan2(B::mul(k(2.0f), B::add(B::mul(q[3], q[2]), B::mul(q[0], q[1]))),
                                                         B::sub(k(1.0f), B::mul(k(2.0f), B::add(B::mul(q[1], q[1]), B::mul(q[2], q[2])))));
                                put(ChRotation, roll);
                            }
                            break;
                        }
                        case SetProperty::Frame: {
                            const uint32_t* frames = reinterpret_cast<const uint32_t*>(ctx.pool + s.frames);
                            alignas(16) float vv[4], out[4];
                            B::storeu(vv, v);
                            for (int l = 0; l < 4; l++){
                                // the window's end (1) is the last frame and an ease below 0 the
                                // first: the position is clamped before it becomes an index
                                const float position = std::min(std::max(vv[l], 0.0f), 1.0f) * (float)s.frameCount;
                                uint32_t idx = (uint32_t)position;
                                if (idx >= s.frameCount) idx = s.frameCount - 1;
                                out[l] = (float)frames[idx];
                            }
                            put(ChFrame, B::loadu(out));
                            break;
                        }
                    }
                }
                if (p.replaceColor){
                    f4 c[4];
                    gradient(p.color, L, c);
                    st(ctx.ch[ChColorR], i, c[0]);
                    st(ctx.ch[ChColorG], i, c[1]);
                    st(ctx.ch[ChColorB], i, c[2]);
                }
            }
        }

        static f4 srgbToLinear(f4 x){ return pow(B::max(x, B::zero()), 2.2f); }

        // ------------------------------------------------------------ update: forces

        static void forces(const KernelContext& ctx, const ForceParams& p, uint32_t begin, uint32_t end){
            ChunkScratch& s = *ctx.scratch;
            const uint32_t perTick = p.forcePerTick ? ctx.tick * 0x632be5abu : 0u;
            const float* fdir = p.forceWorld ? ctx.worldToSimDir : ctx.emitterToSimDir;
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                f4 ax = B::zero(), ay = B::zero(), az = B::zero();
                if (p.gravity){
                    const f4 g = curve(p.gravityModifier, L);
                    ax = B::mul(k(ctx.gravity[0]), g);
                    ay = B::mul(k(ctx.gravity[1]), g);
                    az = B::mul(k(ctx.gravity[2]), g);
                }
                if (p.ownAcceleration){
                    ax = B::add(ax, ld(ctx.ch[ChAccelX], i));
                    ay = B::add(ay, ld(ctx.ch[ChAccelY], i));
                    az = B::add(az, ld(ctx.ch[ChAccelZ], i));
                }else if (p.force){
                    f4 fx, fy, fz;
                    xformDir(fdir, curve(p.fx, L, perTick), curve(p.fy, L, perTick), curve(p.fz, L, perTick), fx, fy, fz);
                    ax = B::add(ax, fx);
                    ay = B::add(ay, fy);
                    az = B::add(az, fz);
                }
                if (p.inheritCurrent){
                    // Inherit Velocity (Current): the emitter's change of velocity this tick, a share
                    // of it over the particle's life, straight into the velocity
                    const f4 share = curve(p.inherit, L);
                    st(ctx.ch[ChVelX], i, B::madd(k(ctx.emitterVelocityDelta[0]), share, ld(ctx.ch[ChVelX], i)));
                    st(ctx.ch[ChVelY], i, B::madd(k(ctx.emitterVelocityDelta[1]), share, ld(ctx.ch[ChVelY], i)));
                    st(ctx.ch[ChVelZ], i, B::madd(k(ctx.emitterVelocityDelta[2]), share, ld(ctx.ch[ChVelZ], i)));
                }
                const uint32_t j = i - begin;
                st(s.ax, j, ax);
                st(s.ay, j, ay);
                st(s.az, j, az);
            }
        }

        static void externalForces(const KernelContext& ctx, const ProgramExternalForces& p, uint32_t begin, uint32_t end){
            ChunkScratch& s = *ctx.scratch;
            for (uint32_t i = begin; i < end; i += 4){
                const uint32_t j = i - begin;
                const f4 px = ld(ctx.ch[ChPosX], i), py = ld(ctx.ch[ChPosY], i), pz = ld(ctx.ch[ChPosZ], i);
                const f4 vx = ld(ctx.ch[ChVelX], i), vy = ld(ctx.ch[ChVelY], i), vz = ld(ctx.ch[ChVelZ], i);
                f4 ax = s.hasAccel ? ld(s.ax, j) : B::zero();
                f4 ay = s.hasAccel ? ld(s.ay, j) : B::zero();
                f4 az = s.hasAccel ? ld(s.az, j) : B::zero();
                for (uint32_t fi = 0; fi < ctx.fieldCount; fi++){
                    const ForceFieldSample& f = ctx.fields[fi];
                    f4 ux, uy, uz;
                    xformPoint(f.toField, px, py, pz, ux, uy, uz);
                    f4 d;
                    switch (f.shape){
                        case ForceFieldShape::Sphere: d = length3(ux, uy, uz); break;
                        case ForceFieldShape::Box: d = B::max(B::max(B::abs(ux), B::abs(uy)), B::abs(uz)); break;
                        case ForceFieldShape::Cylinder: d = B::max(B::sqrt(B::add(B::mul(ux, ux), B::mul(uz, uz))), B::abs(uy)); break;
                        case ForceFieldShape::Infinite:
                        default: d = B::zero(); break;
                    }
                    const u4 inside = B::cmple(d, k(1.0f));
                    if (B::movemask(inside) == 0)
                        continue;
                    // the falloff table: 17 samples over 0..1
                    const f4 fall = falloff(f.falloff, M::clamp01(d));
                    const f4 strength = B::mul(fall, k(f.strength * p.multiplier));
                    f4 fx = B::zero(), fy = B::zero(), fz = B::zero();
                    const f4 rx = B::sub(px, k(f.center[0])), ry = B::sub(py, k(f.center[1])), rz = B::sub(pz, k(f.center[2]));
                    switch (f.type){
                        case ForceFieldType::Directional: {
                            fx = k(f.axis[0]); fy = k(f.axis[1]); fz = k(f.axis[2]);
                            if (f.turbulence > 0.0f){
                                const typename noise::GradientCurlSimd<B>::V3 n = noise::GradientCurlSimd<B>::curl2(
                                    B::mul(px, k(f.frequency)), B::add(B::mul(py, k(f.frequency)), k(ctx.time)), B::mul(pz, k(f.frequency)), f.seed);
                                fx = B::madd(n.x, k(f.turbulence), fx);
                                fy = B::madd(n.y, k(f.turbulence), fy);
                                fz = B::madd(n.z, k(f.turbulence), fz);
                            }
                            break;
                        }
                        case ForceFieldType::Radial: {
                            const f4 len = length3(rx, ry, rz);
                            fx = safeDiv(rx, len); fy = safeDiv(ry, len); fz = safeDiv(rz, len);
                            break;
                        }
                        case ForceFieldType::Vortex: {
                            // around the axis: axis x r, normalized
                            const f4 cx = B::sub(B::mul(k(f.axis[1]), rz), B::mul(k(f.axis[2]), ry));
                            const f4 cy = B::sub(B::mul(k(f.axis[2]), rx), B::mul(k(f.axis[0]), rz));
                            const f4 cz = B::sub(B::mul(k(f.axis[0]), ry), B::mul(k(f.axis[1]), rx));
                            const f4 len = length3(cx, cy, cz);
                            fx = safeDiv(cx, len); fy = safeDiv(cy, len); fz = safeDiv(cz, len);
                            break;
                        }
                        case ForceFieldType::Turbulence: {
                            const typename noise::GradientCurlSimd<B>::V3 n = noise::GradientCurlSimd<B>::curl2(
                                B::mul(px, k(f.frequency)), B::add(B::mul(py, k(f.frequency)), k(ctx.time)), B::mul(pz, k(f.frequency)), f.seed);
                            fx = n.x; fy = n.y; fz = n.z;
                            break;
                        }
                        case ForceFieldType::Drag:
                            fx = B::neg(vx); fy = B::neg(vy); fz = B::neg(vz);
                            break;
                    }
                    ax = B::add(ax, B::select(inside, B::mul(fx, strength), B::zero()));
                    ay = B::add(ay, B::select(inside, B::mul(fy, strength), B::zero()));
                    az = B::add(az, B::select(inside, B::mul(fz, strength), B::zero()));
                }
                st(s.ax, j, ax);
                st(s.ay, j, ay);
                st(s.az, j, az);
            }
        }

        static f4 falloff(const float* table, f4 d){
            const f4 x = B::mul(d, k(16.0f));
            alignas(16) uint32_t idx[4];
            B::storeu4(idx, B::cvtt(x));
            for (int l = 0; l < 4; l++) idx[l] = idx[l] > 15 ? 15 : idx[l];
            const f4 f = B::sub(x, B::cvt(B::iset(idx[0], idx[1], idx[2], idx[3])));
            const f4 a = B::set(table[idx[0]], table[idx[1]], table[idx[2]], table[idx[3]]);
            const f4 b = B::set(table[idx[0] + 1], table[idx[1] + 1], table[idx[2] + 1], table[idx[3] + 1]);
            return B::madd(B::sub(b, a), f, a);
        }

        // ------------------------------------------------------------ update: animated velocity

        static void animatedVelocity(const KernelContext& ctx, const AnimatedVelocityParams& p, uint32_t begin, uint32_t end){
            ChunkScratch& s = *ctx.scratch;
            const float* ldir = p.linearWorld ? ctx.worldToSimDir : ctx.emitterToSimDir;
            float center[3];
            {
                // the orbital and radial centre: the emitter plus the offset (simulation space)
                const float* m = ctx.emitterToSim;
                for (int c = 0; c < 3; c++)
                    center[c] = m[c] * p.offset[0] + m[3 + c] * p.offset[1] + m[6 + c] * p.offset[2] + m[9 + c];
            }
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                const uint32_t j = i - begin;
                f4 vx = B::zero(), vy = B::zero(), vz = B::zero();
                if (p.linear){
                    xformDir(ldir, curve(p.vx, L), curve(p.vy, L), curve(p.vz, L), vx, vy, vz);
                }
                if (p.orbital || p.radial){
                    f4 cx = k(center[0]), cy = k(center[1]), cz = k(center[2]);
                    if (!p.offsetConst){
                        // the offset over the particle's life (emitter space)
                        xformPoint(ctx.emitterToSim, curve(p.offX, L), curve(p.offY, L), curve(p.offZ, L), cx, cy, cz);
                    }
                    const f4 rx = B::sub(ld(ctx.ch[ChPosX], i), cx);
                    const f4 ry = B::sub(ld(ctx.ch[ChPosY], i), cy);
                    const f4 rz = B::sub(ld(ctx.ch[ChPosZ], i), cz);
                    if (p.orbital){
                        f4 wx, wy, wz;
                        xformDir(ctx.emitterToSimDir, curve(p.ox, L), curve(p.oy, L), curve(p.oz, L), wx, wy, wz);
                        vx = B::add(vx, B::sub(B::mul(wy, rz), B::mul(wz, ry)));
                        vy = B::add(vy, B::sub(B::mul(wz, rx), B::mul(wx, rz)));
                        vz = B::add(vz, B::sub(B::mul(wx, ry), B::mul(wy, rx)));
                    }
                    if (p.radial){
                        const f4 len = length3(rx, ry, rz);
                        const f4 r = curve(p.radialSpeed, L);
                        vx = B::madd(safeDiv(rx, len), r, vx);
                        vy = B::madd(safeDiv(ry, len), r, vy);
                        vz = B::madd(safeDiv(rz, len), r, vz);
                    }
                }
                if (s.hasAnimated){
                    vx = B::add(vx, ld(s.vx, j));
                    vy = B::add(vy, ld(s.vy, j));
                    vz = B::add(vz, ld(s.vz, j));
                }
                st(s.vx, j, vx);
                st(s.vy, j, vy);
                st(s.vz, j, vz);
            }
        }

        // ------------------------------------------------------------ update: noise

        static void noiseOp(const KernelContext& ctx, const NoiseParams& p, uint32_t begin, uint32_t end){
            ChunkScratch& s = *ctx.scratch;
            typedef noise::GradientCurlSimd<B> Curl;
            const float scroll = p.scrollAccumulated ? ctx.noiseScroll : p.scrollSpeed * ctx.time;
            // High and Medium 8 samples at a time where the processor has it: the same values
            // (CurlNoiseAvx2.cpp), summed over the octaves in the same order
            bool wide = false;
            if constexpr (std::is_same<B, simd::Native>::value){
                wide = p.quality != NoiseQuality::Low && noise::curlWideAvailable();
            }
            if (wide){
                const uint32_t count = (end - begin + 7u) & ~7u;
                const int potentials = p.quality == NoiseQuality::High ? 3 : 2;
                float frequency = p.frequency;
                float amplitude = 1.0f;
                for (uint32_t o = 0; o < p.octaves; o++){
                    // the first octave (amplitude 1) is the sum so far: written in place
                    const bool first = o == 0;
                    noise::curlNoiseWide(potentials, ctx.ch[ChPosX] + begin, ctx.ch[ChPosY] + begin, ctx.ch[ChPosZ] + begin, count, frequency, scroll,
                                         p.seed + o * 0x632be5abu, first ? s.noiseX : s.curlX, first ? s.noiseY : s.curlY, first ? s.noiseZ : s.curlZ);
                    for (uint32_t j = 0; !first && j < count; j += 4){
                        st(s.noiseX, j, B::madd(ld(s.curlX, j), k(amplitude), ld(s.noiseX, j)));
                        st(s.noiseY, j, B::madd(ld(s.curlY, j), k(amplitude), ld(s.noiseY, j)));
                        st(s.noiseZ, j, B::madd(ld(s.curlZ, j), k(amplitude), ld(s.noiseZ, j)));
                    }
                    frequency *= p.octaveScale;
                    amplitude *= p.octaveMultiplier;
                }
            }
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                const uint32_t j = i - begin;
                f4 nx = B::zero(), ny = B::zero(), nz = B::zero();
                if (wide){
                    nx = ld(s.noiseX, j);
                    ny = ld(s.noiseY, j);
                    nz = ld(s.noiseZ, j);
                }else{
                    const f4 px = ld(ctx.ch[ChPosX], i), py = ld(ctx.ch[ChPosY], i), pz = ld(ctx.ch[ChPosZ], i);
                    float frequency = p.frequency;
                    float amplitude = 1.0f;
                    for (uint32_t o = 0; o < p.octaves; o++){
                        const f4 sx = B::add(B::mul(px, k(frequency)), k(scroll));
                        const f4 sy = B::add(B::mul(py, k(frequency)), k(scroll));
                        const f4 sz = B::add(B::mul(pz, k(frequency)), k(scroll));
                        const uint32_t seed = p.seed + o * 0x632be5abu;
                        typename Curl::V3 c;
                        if (p.quality == NoiseQuality::High){
                            c = Curl::curl3(sx, sy, sz, seed);
                        }else if (p.quality == NoiseQuality::Medium){
                            c = Curl::curl2(sx, sy, sz, seed);
                        }else{
                            c = lowField(ctx, p, sx, sy, sz);
                        }
                        if (o == 0){
                            // the first octave's amplitude is 1: the sum starts as it (as the wide path)
                            nx = c.x; ny = c.y; nz = c.z;
                        }else{
                            nx = B::madd(c.x, k(amplitude), nx);
                            ny = B::madd(c.y, k(amplitude), ny);
                            nz = B::madd(c.z, k(amplitude), nz);
                        }
                        frequency *= p.octaveScale;
                        amplitude *= p.octaveMultiplier;
                    }
                }
                nx = B::mul(nx, k(p.strengthScale));
                ny = B::mul(ny, k(p.strengthScale));
                nz = B::mul(nz, k(p.strengthScale));
                const f4 strengthX = curve(p.strengthX, L);
                const f4 strengthY = p.separateAxes ? curve(p.strengthY, L) : strengthX;
                const f4 strengthZ = p.separateAxes ? curve(p.strengthZ, L) : strengthX;
                const f4 amount = curve(p.positionAmount, L);
                f4 vx = B::mul(B::mul(nx, strengthX), amount);
                f4 vy = B::mul(B::mul(ny, strengthY), amount);
                f4 vz = B::mul(B::mul(nz, strengthZ), amount);
                if (s.hasAnimated){
                    vx = B::add(vx, ld(s.vx, j));
                    vy = B::add(vy, ld(s.vy, j));
                    vz = B::add(vz, ld(s.vz, j));
                }
                st(s.vx, j, vx);
                st(s.vy, j, vy);
                st(s.vz, j, vz);
                if (p.affectsRotation){
                    // the noise's x turns the sprite (radians per second at amount 1)
                    const f4 turn = B::mul(B::mul(nx, strengthX), curve(p.rotationAmount, L));
                    st(ctx.ch[ChRotation], i, B::madd(turn, dtAt(ctx, i), ld(ctx.ch[ChRotation], i)));
                }
                if (p.affectsSize){
                    const f4 factor = B::max(B::madd(B::mul(ny, strengthY), curve(p.sizeAmount, L), k(1.0f)), B::zero());
                    st(ctx.ch[ChNoiseSize], i, factor);
                }
            }
        }

        // the Low quality field: trilinear in a periodic grid baked at compile time
        static typename noise::GradientCurlSimd<B>::V3 lowField(const KernelContext& ctx, const NoiseParams& p, f4 x, f4 y, f4 z){
            const float* field = reinterpret_cast<const float*>(ctx.pool + p.lowField);
            const float cellsPerUnit = (float)kNoiseLowCells / kNoiseLowPeriod;
            const f4 period = k(kNoiseLowPeriod);
            auto wrapCoord = [&](f4 v){ return B::mul(B::sub(v, B::mul(B::floor(B::div(v, period)), period)), k(cellsPerUnit)); };
            const f4 gx = wrapCoord(x), gy = wrapCoord(y), gz = wrapCoord(z);
            alignas(16) float fx[4], fy[4], fz[4];
            B::storeu(fx, gx); B::storeu(fy, gy); B::storeu(fz, gz);
            alignas(16) float out[3][4];
            const uint32_t n = kNoiseLowCells;
            for (int l = 0; l < 4; l++){
                const float cx = std::floor(fx[l]), cy = std::floor(fy[l]), cz = std::floor(fz[l]);
                const float tx = fx[l] - cx, ty = fy[l] - cy, tz = fz[l] - cz;
                const uint32_t x0 = ((uint32_t)cx) % n, y0 = ((uint32_t)cy) % n, z0 = ((uint32_t)cz) % n;
                const uint32_t x1 = (x0 + 1) % n, y1 = (y0 + 1) % n, z1 = (z0 + 1) % n;
                for (int c = 0; c < 3; c++){
                    auto at = [&](uint32_t a, uint32_t b, uint32_t d){ return field[((d * n + b) * n + a) * 3 + c]; };
                    const float c00 = at(x0, y0, z0) + (at(x1, y0, z0) - at(x0, y0, z0)) * tx;
                    const float c10 = at(x0, y1, z0) + (at(x1, y1, z0) - at(x0, y1, z0)) * tx;
                    const float c01 = at(x0, y0, z1) + (at(x1, y0, z1) - at(x0, y0, z1)) * tx;
                    const float c11 = at(x0, y1, z1) + (at(x1, y1, z1) - at(x0, y1, z1)) * tx;
                    const float c0 = c00 + (c10 - c00) * ty;
                    const float c1 = c01 + (c11 - c01) * ty;
                    out[c][l] = c0 + (c1 - c0) * tz;
                }
            }
            typename noise::GradientCurlSimd<B>::V3 r;
            r.x = B::loadu(out[0]);
            r.y = B::loadu(out[1]);
            r.z = B::loadu(out[2]);
            return r;
        }

        // ------------------------------------------------------------ update: integrate

        static void integrate(const KernelContext& ctx, const IntegrateParams& p, const AnimatedVelocityParams* av, uint32_t begin, uint32_t end){
            ChunkScratch& s = *ctx.scratch;
            // The update's step is one for every lane (only spawning passes per-lane steps): what
            // depends on nothing else is worked out once, with the same operations as per lane
            // (the same bits). A constant linear drag's decay and which formula it takes:
            const bool uniformStep = ctx.dtLanes == nullptr;
            const bool dragConst = p.drag && p.dragLinear.kind == CurveParam::Const && !p.dragBySize && !p.dragByVelocity;
            const float dragK1 = dragConst ? std::max(p.dragLinear.c0, 0.0f) : 0.0f;
            const bool decayHoisted = dragConst && uniformStep;
            const f4 decay = decayHoisted ? M::exp(B::neg(B::mul(B::max(k(p.dragLinear.c0), B::zero()), k(ctx.dt)))) : B::zero();
            // the limit's share of the excess removed over the step
            const bool shareHoisted = p.limit && p.dampen < 1.0f && uniformStep;
            const f4 shareUniform = shareHoisted ? B::sub(k(1.0f), M::exp(B::mul(B::mul(k(ctx.dt), k(30.0f)), k(p.logKeep)))) : B::zero();
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                const uint32_t j = i - begin;
                const f4 dt = dtAt(ctx, i);
                const f4 v0x = ld(ctx.ch[ChVelX], i), v0y = ld(ctx.ch[ChVelY], i), v0z = ld(ctx.ch[ChVelZ], i);
                f4 v1x = v0x, v1y = v0y, v1z = v0z;
                if (s.hasAccel){
                    v1x = B::madd(ld(s.ax, j), dt, v1x);
                    v1y = B::madd(ld(s.ay, j), dt, v1y);
                    v1z = B::madd(ld(s.az, j), dt, v1z);
                }
                if (p.drag){
                    // dv/dt = -(k1 + k2 |v|) v, solved in closed form along v over the tick
                    f4 k1 = B::max(curve(p.dragLinear, L), B::zero());
                    f4 k2 = k(p.dragQuadratic);
                    const f4 speed = length3(v1x, v1y, v1z);
                    if (p.dragBySize){
                        const f4 size = ctx.ch[ChSizeX] ? ld(ctx.ch[ChSizeX], i) : ld(ctx.ch[ChStartSize], i);
                        const f4 area = B::mul(size, size);
                        k1 = B::mul(k1, area);
                        k2 = B::mul(k2, area);
                    }
                    if (p.dragByVelocity){
                        // the linear drag grows with the speed: a quadratic term
                        k2 = B::add(k2, k1);
                        k1 = B::zero();
                    }
                    const f4 e = decayHoisted ? decay : M::exp(B::neg(B::mul(k1, dt)));
                    // k1 > 0: s' = k1 s e / (k1 + k2 s (1 - e)); k1 = 0: s' = s / (1 + k2 s dt)
                    // (a constant k1 takes one of the two: the other is not worked out)
                    f4 slowed;
                    if (dragConst && dragK1 > 1.0e-12f){
                        slowed = B::div(B::mul(B::mul(k1, speed), e), B::max(B::add(k1, B::mul(B::mul(k2, speed), B::sub(k(1.0f), e))), k(1.0e-30f)));
                    }else if (dragConst){
                        slowed = B::div(speed, B::add(k(1.0f), B::mul(B::mul(k2, speed), dt)));
                    }else{
                        const f4 withLinear = B::div(B::mul(B::mul(k1, speed), e), B::max(B::add(k1, B::mul(B::mul(k2, speed), B::sub(k(1.0f), e))), k(1.0e-30f)));
                        const f4 quadOnly = B::div(speed, B::add(k(1.0f), B::mul(B::mul(k2, speed), dt)));
                        slowed = B::select(B::cmpgt(k1, k(1.0e-12f)), withLinear, quadOnly);
                    }
                    const f4 scale = B::select(B::cmpgt(speed, k(1.0e-20f)), B::div(slowed, speed), k(1.0f));
                    v1x = B::mul(v1x, scale);
                    v1y = B::mul(v1y, scale);
                    v1z = B::mul(v1z, scale);
                }
                if (p.limit && p.limitAxes){
                    // a limit per axis, in the emitter's or the world's axes: each axis's excess
                    // above its limit dampened as the speed's would be
                    const float* toAxes = p.limitWorld ? ctx.worldToSimDir : ctx.emitterToSimDir;
                    f4 ax, ay, az;
                    // simulation space into the limit's axes: the transpose of its rotation
                    ax = B::add(B::add(B::mul(k(toAxes[0]), v1x), B::mul(k(toAxes[1]), v1y)), B::mul(k(toAxes[2]), v1z));
                    ay = B::add(B::add(B::mul(k(toAxes[3]), v1x), B::mul(k(toAxes[4]), v1y)), B::mul(k(toAxes[5]), v1z));
                    az = B::add(B::add(B::mul(k(toAxes[6]), v1x), B::mul(k(toAxes[7]), v1y)), B::mul(k(toAxes[8]), v1z));
                    f4 share;
                    if (p.dampen >= 1.0f){
                        share = k(1.0f);
                    }else if (shareHoisted){
                        share = shareUniform;
                    }else{
                        share = B::sub(k(1.0f), M::exp(B::mul(B::mul(dt, k(30.0f)), k(p.logKeep))));
                    }
                    auto clampAxis = [&](f4 v, f4 limit){
                        const f4 a = B::abs(v);
                        const f4 target = B::madd(B::sub(limit, a), share, a);
                        const f4 sign = B::select(B::cmplt(v, B::zero()), k(-1.0f), k(1.0f));
                        return B::select(B::cmpgt(a, limit), B::mul(target, sign), v);
                    };
                    ax = clampAxis(ax, B::abs(curve(p.limitSpeed, L)));
                    ay = clampAxis(ay, B::abs(curve(p.limitY, L)));
                    az = clampAxis(az, B::abs(curve(p.limitZ, L)));
                    xformDir(toAxes, ax, ay, az, v1x, v1y, v1z);
                }else if (p.limit){
                    const f4 limit = B::max(curve(p.limitSpeed, L), B::zero());
                    const f4 speed = length3(v1x, v1y, v1z);
                    // the share of the excess removed: dampen per 1/30 s, at this tick's length
                    const u4 over = B::cmpgt(speed, limit);
                    // under the limit everywhere: the scale is 1 (nothing to change)
                    if (B::movemask(over) != 0){
                        f4 share;
                        if (p.dampen >= 1.0f){
                            share = k(1.0f);
                        }else if (shareHoisted){
                            share = shareUniform;
                        }else{
                            share = B::sub(k(1.0f), M::exp(B::mul(B::mul(dt, k(30.0f)), k(p.logKeep))));
                        }
                        const f4 target = B::madd(B::sub(limit, speed), share, speed);
                        const f4 scale = B::select(over, safeDiv(target, speed), k(1.0f));
                        v1x = B::mul(v1x, scale);
                        v1y = B::mul(v1y, scale);
                        v1z = B::mul(v1z, scale);
                    }
                }
                // velocity-Verlet with the tick's constant acceleration: the mean of the two velocities
                f4 mx = B::mul(B::add(v0x, v1x), k(0.5f));
                f4 my = B::mul(B::add(v0y, v1y), k(0.5f));
                f4 mz = B::mul(B::add(v0z, v1z), k(0.5f));
                if (s.hasAnimated){
                    mx = B::add(mx, ld(s.vx, j));
                    my = B::add(my, ld(s.vy, j));
                    mz = B::add(mz, ld(s.vz, j));
                }
                if (av && av->speedModifier){
                    const f4 sm = curve(av->speed, L);
                    mx = B::mul(mx, sm);
                    my = B::mul(my, sm);
                    mz = B::mul(mz, sm);
                }
                f4 nx = B::madd(mx, dt, ld(ctx.ch[ChPosX], i));
                f4 ny = B::madd(my, dt, ld(ctx.ch[ChPosY], i));
                f4 nz = B::madd(mz, dt, ld(ctx.ch[ChPosZ], i));
                if (ctx.ch[ChSettled]){
                    // at rest (S49 R14): no force, gravity or animated velocity moves it
                    const u4 rest = B::cmpgt(ld(ctx.ch[ChSettled], i), B::zero());
                    nx = B::select(rest, ld(ctx.ch[ChPosX], i), nx);
                    ny = B::select(rest, ld(ctx.ch[ChPosY], i), ny);
                    nz = B::select(rest, ld(ctx.ch[ChPosZ], i), nz);
                    v1x = B::select(rest, B::zero(), v1x);
                    v1y = B::select(rest, B::zero(), v1y);
                    v1z = B::select(rest, B::zero(), v1z);
                }
                st(ctx.ch[ChPosX], i, nx);
                st(ctx.ch[ChPosY], i, ny);
                st(ctx.ch[ChPosZ], i, nz);
                st(ctx.ch[ChVelX], i, v1x);
                st(ctx.ch[ChVelY], i, v1y);
                st(ctx.ch[ChVelZ], i, v1z);
                if (ctx.ch[ChAnimVelX]){
                    // the renderer's total velocity: the stored one plus this
                    st(ctx.ch[ChAnimVelX], i, s.hasAnimated ? ld(s.vx, j) : B::zero());
                    st(ctx.ch[ChAnimVelY], i, s.hasAnimated ? ld(s.vy, j) : B::zero());
                    st(ctx.ch[ChAnimVelZ], i, s.hasAnimated ? ld(s.vz, j) : B::zero());
                }
                st(ctx.ch[ChAge], i, B::add(L.age, dt));
            }
        }

        // ------------------------------------------------------------ update: orbit

        static void orbit(const KernelContext& ctx, const OrbitParams& p, uint32_t begin, uint32_t end){
            // axis and centre in simulation space
            float axis[3], center[3];
            const float* dirM = p.world ? ctx.worldToSimDir : ctx.emitterToSimDir;
            for (int c = 0; c < 3; c++)
                axis[c] = dirM[c] * p.axis[0] + dirM[3 + c] * p.axis[1] + dirM[6 + c] * p.axis[2];
            const float al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            for (int c = 0; c < 3; c++) axis[c] = al > 0.0f ? axis[c] / al : (c == 1 ? 1.0f : 0.0f);
            if (p.world){
                for (int c = 0; c < 3; c++) center[c] = p.center[c];
            }else{
                const float* m = ctx.emitterToSim;
                for (int c = 0; c < 3; c++)
                    center[c] = m[c] * p.center[0] + m[3 + c] * p.center[1] + m[6 + c] * p.center[2] + m[9 + c];
            }
            const f4 ax = k(axis[0]), ay = k(axis[1]), az = k(axis[2]);
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                const f4 dt = dtAt(ctx, i);
                const f4 angle = B::mul(curve(p.angular, L), dt);
                f4 sn, cs;
                M::sincos(angle, sn, cs);
                // Rodrigues: r' = r cos + (a x r) sin + a (a . r)(1 - cos)
                auto turn = [&](f4& x, f4& y, f4& z){
                    const f4 cx = B::sub(B::mul(ay, z), B::mul(az, y));
                    const f4 cy = B::sub(B::mul(az, x), B::mul(ax, z));
                    const f4 cz = B::sub(B::mul(ax, y), B::mul(ay, x));
                    const f4 d = B::mul(dot3(ax, ay, az, x, y, z), B::sub(k(1.0f), cs));
                    const f4 nx = B::add(B::add(B::mul(x, cs), B::mul(cx, sn)), B::mul(ax, d));
                    const f4 ny = B::add(B::add(B::mul(y, cs), B::mul(cy, sn)), B::mul(ay, d));
                    const f4 nz = B::add(B::add(B::mul(z, cs), B::mul(cz, sn)), B::mul(az, d));
                    x = nx; y = ny; z = nz;
                };
                f4 rx = B::sub(ld(ctx.ch[ChPosX], i), k(center[0]));
                f4 ry = B::sub(ld(ctx.ch[ChPosY], i), k(center[1]));
                f4 rz = B::sub(ld(ctx.ch[ChPosZ], i), k(center[2]));
                turn(rx, ry, rz);
                f4 vx = ld(ctx.ch[ChVelX], i), vy = ld(ctx.ch[ChVelY], i), vz = ld(ctx.ch[ChVelZ], i);
                turn(vx, vy, vz);
                if (!p.pull.isZero()){
                    // toward the axis by pull * dt, never past it
                    const f4 along = dot3(ax, ay, az, rx, ry, rz);
                    const f4 ox = B::sub(rx, B::mul(ax, along)), oy = B::sub(ry, B::mul(ay, along)), oz = B::sub(rz, B::mul(az, along));
                    const f4 dist = length3(ox, oy, oz);
                    const f4 move = B::min(B::mul(curve(p.pull, L), dt), dist);
                    const f4 f = safeDiv(move, dist);
                    rx = B::sub(rx, B::mul(ox, f));
                    ry = B::sub(ry, B::mul(oy, f));
                    rz = B::sub(rz, B::mul(oz, f));
                }
                st(ctx.ch[ChPosX], i, B::add(rx, k(center[0])));
                st(ctx.ch[ChPosY], i, B::add(ry, k(center[1])));
                st(ctx.ch[ChPosZ], i, B::add(rz, k(center[2])));
                st(ctx.ch[ChVelX], i, vx);
                st(ctx.ch[ChVelY], i, vy);
                st(ctx.ch[ChVelZ], i, vz);
            }
        }

        // ------------------------------------------------------------ update: ticked appearance

        static void storedAppearance(const KernelContext& ctx, const StoredAppearanceParams& p, uint32_t begin, uint32_t end){
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                const f4 dt = dtAt(ctx, i);
                if ((p.rotationBySpeed || p.angularVelocity) && ctx.ch[ChRotation]){
                    f4 w = B::zero();
                    if (p.angularVelocity) w = curve(p.angular, L);
                    if (p.rotationBySpeed) w = B::add(w, curve(p.rotationSpeed, L));
                    if (ctx.ch[ChSettled])
                        w = B::select(B::cmpgt(ld(ctx.ch[ChSettled], i), B::zero()), B::zero(), w);
                    st(ctx.ch[ChRotation], i, B::madd(w, dt, ld(ctx.ch[ChRotation], i)));
                }
                if ((p.angularVelocity3D || p.rotationBySpeed3D) && ctx.ch[ChQuatX]){
                    // q' = q + 0.5 (0, w) q dt, normalized; w: the particle's angular velocity and
                    // Rotation by Speed's (its axes)
                    f4 wx = B::zero(), wy = B::zero(), wz = B::zero();
                    if (p.angularVelocity3D && ctx.ch[ChAngVelX]){
                        wx = ld(ctx.ch[ChAngVelX], i);
                        wy = ld(ctx.ch[ChAngVelY], i);
                        wz = ld(ctx.ch[ChAngVelZ], i);
                    }
                    if (p.rotationBySpeed3D){
                        wz = B::add(wz, curve(p.rotationSpeed, L));
                        if (!p.rotationSpeedX.isZero()) wx = B::add(wx, curve(p.rotationSpeedX, L));
                        if (!p.rotationSpeedY.isZero()) wy = B::add(wy, curve(p.rotationSpeedY, L));
                    }
                    const f4 qx = ld(ctx.ch[ChQuatX], i), qy = ld(ctx.ch[ChQuatY], i), qz = ld(ctx.ch[ChQuatZ], i), qw = ld(ctx.ch[ChQuatW], i);
                    const f4 h = B::mul(dt, k(0.5f));
                    f4 nx = B::madd(B::add(B::sub(B::mul(wx, qw), B::mul(wz, qy)), B::mul(wy, qz)), h, qx);
                    f4 ny = B::madd(B::add(B::sub(B::mul(wy, qw), B::mul(wx, qz)), B::mul(wz, qx)), h, qy);
                    f4 nz = B::madd(B::add(B::sub(B::mul(wz, qw), B::mul(wy, qx)), B::mul(wx, qy)), h, qz);
                    f4 nw = B::madd(B::neg(dot3(wx, wy, wz, qx, qy, qz)), h, qw);
                    const f4 len = B::sqrt(B::add(B::add(B::mul(nx, nx), B::mul(ny, ny)), B::add(B::mul(nz, nz), B::mul(nw, nw))));
                    st(ctx.ch[ChQuatX], i, B::div(nx, len));
                    st(ctx.ch[ChQuatY], i, B::div(ny, len));
                    st(ctx.ch[ChQuatZ], i, B::div(nz, len));
                    st(ctx.ch[ChQuatW], i, B::div(nw, len));
                }
            }
        }

        // ------------------------------------------------------------ update: collision

        // The response to a contact at (hx, hy, hz) with normal n: the particle sits on the
        // surface (plus its radius), its velocity is reflected (bounce) and slowed (dampen);
        // it loses lifetime, and dies under/over the kill speeds or on any hit with kill
        static void respond(const KernelContext& ctx, const CollisionParams& p, Lane& L, uint32_t i, u4 hit,
                            f4 hx, f4 hy, f4 hz, f4 nx, f4 ny, f4 nz, f4 radius){
            if (B::movemask(hit) == 0)
                return;
            const f4 vx = ld(ctx.ch[ChVelX], i), vy = ld(ctx.ch[ChVelY], i), vz = ld(ctx.ch[ChVelZ], i);
            const f4 vn = dot3(vx, vy, vz, nx, ny, nz);
            const f4 tx = B::sub(vx, B::mul(nx, vn)), ty = B::sub(vy, B::mul(ny, vn)), tz = B::sub(vz, B::mul(nz, vn));
            const f4 bounce = curve(p.bounce, L);
            const f4 keep = B::sub(k(1.0f), M::clamp01(curve(p.dampen, L)));
            // only a velocity into the surface reflects
            const f4 rn = B::select(B::cmplt(vn, B::zero()), B::mul(B::neg(vn), bounce), vn);
            const f4 ox = B::mul(B::madd(nx, rn, tx), keep);
            const f4 oy = B::mul(B::madd(ny, rn, ty), keep);
            const f4 oz = B::mul(B::madd(nz, rn, tz), keep);
            st(ctx.ch[ChVelX], i, B::select(hit, ox, vx));
            st(ctx.ch[ChVelY], i, B::select(hit, oy, vy));
            st(ctx.ch[ChVelZ], i, B::select(hit, oz, vz));
            st(ctx.ch[ChPosX], i, B::select(hit, B::madd(nx, radius, hx), ld(ctx.ch[ChPosX], i)));
            st(ctx.ch[ChPosY], i, B::select(hit, B::madd(ny, radius, hy), ld(ctx.ch[ChPosY], i)));
            st(ctx.ch[ChPosZ], i, B::select(hit, B::madd(nz, radius, hz), ld(ctx.ch[ChPosZ], i)));
            // lifetime loss: a share of the whole lifetime, added to the age
            f4 age = L.age;
            if (!p.lifetimeLoss.isZero()){
                const f4 loss = B::div(M::clamp01(curve(p.lifetimeLoss, L)), L.invLife);
                age = B::select(hit, B::add(age, loss), age);
            }
            const f4 speed = length3(ox, oy, oz);
            if (p.settleSpeed > 0.0f && ctx.ch[ChSettled]){
                // pixelfx's rest: a hit slower than Settle Speed stops it where it landed
                const u4 settle = B::iand(hit, B::cmplt(speed, k(p.settleSpeed)));
                if (B::movemask(settle) != 0){
                    st(ctx.ch[ChVelX], i, B::select(settle, B::zero(), ld(ctx.ch[ChVelX], i)));
                    st(ctx.ch[ChVelY], i, B::select(settle, B::zero(), ld(ctx.ch[ChVelY], i)));
                    st(ctx.ch[ChVelZ], i, B::select(settle, B::zero(), ld(ctx.ch[ChVelZ], i)));
                    st(ctx.ch[ChSettled], i, B::select(settle, k(1.0f), ld(ctx.ch[ChSettled], i)));
                }
            }
            u4 kill = B::iand(hit, B::ior(B::cmplt(speed, k(p.minKillSpeed)), B::cmpgt(speed, k(p.maxKillSpeed))));
            if (p.kill) kill = hit;
            // a killed particle reaches the end of its life
            age = B::select(kill, B::max(age, B::add(B::div(k(1.0f), L.invLife), k(1.0f))), age);
            st(ctx.ch[ChAge], i, age);
            if (p.events && ctx.out){
                ChunkOutput& out = *ctx.out;
                const uint32_t bits = B::movemask(hit);
                const uint32_t killBits = B::movemask(kill);
                alignas(16) float a[3][4], n[3][4], v[3][4];
                alignas(16) uint32_t ids[4];
                B::storeu(a[0], hx); B::storeu(a[1], hy); B::storeu(a[2], hz);
                B::storeu(n[0], nx); B::storeu(n[1], ny); B::storeu(n[2], nz);
                B::storeu(v[0], vx); B::storeu(v[1], vy); B::storeu(v[2], vz);
                B::storeu4(ids, L.spawn);
                for (int l = 0; l < 4; l++){
                    if (!(bits & (1u << l)) || out.eventCount >= out.maxEvents)
                        continue;
                    CollisionEvent& e = out.events[out.eventCount++];
                    for (int c = 0; c < 3; c++){ e.position[c] = a[c][l]; e.normal[c] = n[c][l]; e.velocity[c] = v[c][l]; }
                    e.particleId = ids[l];
                    e.index = L.i + (uint32_t)l;
                    e.killed = (killBits & (1u << l)) != 0;
                }
            }
        }

        static f4 particleRadius(const KernelContext& ctx, const CollisionParams& p, uint32_t i){
            const f4 size = ctx.ch[ChSizeX] ? ld(ctx.ch[ChSizeX], i) : ld(ctx.ch[ChStartSize], i);
            return B::mul(size, k(0.5f * p.radiusScale));
        }

        static void collision(const KernelContext& ctx, const CollisionParams& p, uint32_t begin, uint32_t end){
            for (uint32_t i = begin; i < end; i += 4){
                Lane L(ctx, i);
                // dying particles of this tick do not collide, nor do those at rest
                u4 alive = B::cmplt(L.normAge, k(1.0f));
                if (ctx.ch[ChSettled])
                    alive = B::iandnot(B::cmpgt(ld(ctx.ch[ChSettled], i), B::zero()), alive);
                if (B::movemask(alive) == 0)
                    continue;
                const f4 radius = particleRadius(ctx, p, i);
                const f4 px = ld(ctx.ch[ChPosX], i), py = ld(ctx.ch[ChPosY], i), pz = ld(ctx.ch[ChPosZ], i);
                switch (p.kind){
                    case CollisionKind::Planes: {
                        // the first plane crossed between the previous and the current position
                        for (uint32_t pl = 0; pl < ctx.collisionPlaneCount; pl++){
                            const float* plane = ctx.collisionPlanes[pl];
                            const f4 nx = k(plane[0]), ny = k(plane[1]), nz = k(plane[2]);
                            const f4 d1 = B::sub(B::add(dot3(nx, ny, nz, px, py, pz), k(plane[3])), radius);
                            const u4 hit = B::iand(alive, B::cmplt(d1, B::zero()));
                            if (B::movemask(hit) == 0)
                                continue;
                            // the contact point: the current position projected onto the plane
                            const f4 cx = B::sub(px, B::mul(nx, B::add(d1, radius)));
                            const f4 cy = B::sub(py, B::mul(ny, B::add(d1, radius)));
                            const f4 cz = B::sub(pz, B::mul(nz, B::add(d1, radius)));
                            respond(ctx, p, L, i, hit, cx, cy, cz, nx, ny, nz, radius);
                            L.age = ld(ctx.ch[ChAge], i);
                            break;
                        }
                        break;
                    }
                    case CollisionKind::Terrain: {
                        if (!ctx.collision || !ctx.collision->terrain)
                            break;
                        // under the surface (along its normal) by more than nothing: a hit at the surface point
                        alignas(16) float pt[3][4], surf[3][4], n[3][4];
                        B::storeu(pt[0], px); B::storeu(pt[1], py); B::storeu(pt[2], pz);
                        alignas(16) uint32_t hitBits[4] = {0, 0, 0, 0};
                        const uint32_t aliveBits = B::movemask(alive);
                        alignas(16) float rad[4];
                        B::storeu(rad, radius);
                        for (int l = 0; l < 4; l++){
                            const float p3[3] = {pt[0][l], pt[1][l], pt[2][l]};
                            float s3[3] = {p3[0], p3[1], p3[2]}, n3[3] = {0.0f, 1.0f, 0.0f};
                            if ((aliveBits & (1u << l)) && ctx.collision->terrain(ctx.collision->context, p3, s3, n3)){
                                const float d = (p3[0] - s3[0]) * n3[0] + (p3[1] - s3[1]) * n3[1] + (p3[2] - s3[2]) * n3[2];
                                if (d < rad[l]) hitBits[l] = 0xffffffffu;
                            }
                            for (int c = 0; c < 3; c++){ surf[c][l] = s3[c]; n[c][l] = n3[c]; }
                        }
                        const u4 hit = B::loadu4(hitBits);
                        respond(ctx, p, L, i, hit, B::loadu(surf[0]), B::loadu(surf[1]), B::loadu(surf[2]),
                                B::loadu(n[0]), B::loadu(n[1]), B::loadu(n[2]), radius);
                        break;
                    }
                    case CollisionKind::World:
                    case CollisionKind::World2D: {
                        worldCollision(ctx, p, L, i, alive, px, py, pz, ld(ctx.ch[ChPrevX], i), ld(ctx.ch[ChPrevY], i), ld(ctx.ch[ChPrevZ], i), radius);
                        break;
                    }
                }
            }
        }

        // the grid cell of a point (world collision's shared planes)
        static uint32_t planeCell(float x, float y, float z, float cellInv){
            const int32_t cx = (int32_t)std::floor(x * cellInv), cy = (int32_t)std::floor(y * cellInv), cz = (int32_t)std::floor(z * cellInv);
            uint32_t h = (uint32_t)cx * 0x8da6b343u ^ (uint32_t)cy * 0xd8163841u ^ (uint32_t)cz * 0xcb1ab31fu;
            h ^= h >> 15;
            return h | 1u;
        }

        // World collision (D8, R8): the queried particles write ray requests for the collision
        // stage; every particle with a contact plane (its own, or its grid cell's) tests it here
        static void worldCollision(const KernelContext& ctx, const CollisionParams& p, Lane& L, uint32_t i, u4 alive,
                                   f4 px, f4 py, f4 pz, f4 qx, f4 qy, f4 qz, f4 radius){
            // who queries this tick: everyone (High); the due, the planeless and the stale (Medium,
            // capped by the collision stage); the due only, every 16 ticks (Low)
            const u4 planeTick = ldu(ctx.ch[ChPlaneTick], i);
            const u4 noPlane = B::icmpeq(planeTick, B::iset1(0u));
            u4 query;
            if (p.quality == CollisionQuality::High){
                query = alive;
            }else{
                const uint32_t period = p.quality == CollisionQuality::Medium ? p.planeCacheTicks : 16u;
                // (spawn + tick) % period == 0, in float arithmetic (exact below 2^24)
                const f4 s = B::cvt(B::iand(B::iadd(L.spawn, B::iset1((uint32_t)ctx.tick)), B::iset1(0xffffffu)));
                const f4 q = B::floor(B::div(s, k((float)period)));
                const u4 due = B::cmpeq(B::sub(s, B::mul(q, k((float)period))), B::zero());
                const u4 stale = B::icmpgt_s(B::isub(B::iset1((uint32_t)ctx.tick), planeTick), B::iset1(p.planeCacheTicks));
                query = p.quality == CollisionQuality::Medium ? B::iand(alive, B::ior(due, B::ior(noPlane, stale))) : B::iand(alive, due);
            }
            const uint32_t queryBits = B::movemask(query);
            if (queryBits && ctx.out){
                ChunkOutput& out = *ctx.out;
                alignas(16) float a[3][4], b[3][4], r[4];
                B::storeu(a[0], qx); B::storeu(a[1], qy); B::storeu(a[2], qz);
                B::storeu(b[0], px); B::storeu(b[1], py); B::storeu(b[2], pz);
                B::storeu(r, radius);
                for (int l = 0; l < 4; l++){
                    if (!(queryBits & (1u << l)))
                        continue;
                    RayRequest& req = out.rays[out.rayCount++];
                    req.particle = i + (uint32_t)l;
                    for (int c = 0; c < 3; c++){ req.from[c] = a[c][l]; req.to[c] = b[c][l]; }
                    req.radius = r[l];
                }
            }
            // planeless particles, and those whose probe found a clear path, take the plane of the
            // cell they were in last tick (in front of any surface they cross now: the hits store
            // their plane on the front side)
            f4 nx = ld(ctx.ch[ChPlaneNX], i), ny = ld(ctx.ch[ChPlaneNY], i), nz = ld(ctx.ch[ChPlaneNZ], i), d = ld(ctx.ch[ChPlaneD], i);
            u4 hasPlane = B::iandnot(noPlane, alive);
            const u4 emptyPlane = B::cmpge(d, k(kEmptyPlaneD));
            const uint32_t noPlaneBits = B::movemask(B::iand(B::ior(noPlane, emptyPlane), alive));
            if (noPlaneBits && ctx.planeGrid){
                alignas(16) float x[4], y[4], z[4], g[4][4];
                alignas(16) uint32_t found[4] = {0, 0, 0, 0};
                B::storeu(x, qx); B::storeu(y, qy); B::storeu(z, qz);
                B::storeu(g[0], nx); B::storeu(g[1], ny); B::storeu(g[2], nz); B::storeu(g[3], d);
                for (int l = 0; l < 4; l++){
                    if (!(noPlaneBits & (1u << l)))
                        continue;
                    const uint32_t key = planeCell(x[l], y[l], z[l], ctx.planeCellInv);
                    const CachedPlane& cell = ctx.planeGrid[key & (kPlaneGridSize - 1)];
                    if (cell.key != key || cell.tick == 0)
                        continue;
                    for (int c = 0; c < 4; c++) g[c][l] = cell.plane[c];
                    found[l] = 0xffffffffu;
                }
                nx = B::loadu(g[0]); ny = B::loadu(g[1]); nz = B::loadu(g[2]); d = B::loadu(g[3]);
                hasPlane = B::ior(hasPlane, B::loadu4(found));
            }
            if (B::movemask(hasPlane) == 0)
                return;
            const f4 d1 = B::sub(B::add(dot3(nx, ny, nz, px, py, pz), d), radius);
            const u4 hit = B::iand(hasPlane, B::cmplt(d1, B::zero()));
            const f4 cx = B::sub(px, B::mul(nx, B::add(d1, radius)));
            const f4 cy = B::sub(py, B::mul(ny, B::add(d1, radius)));
            const f4 cz = B::sub(pz, B::mul(nz, B::add(d1, radius)));
            respond(ctx, p, L, i, hit, cx, cy, cz, nx, ny, nz, radius);
        }

        // ------------------------------------------------------------ update: kill and bounds

        static void kill(const KernelContext& ctx, uint32_t begin, uint32_t end){
            ChunkOutput& out = *ctx.out;
            f4 mnx = k(3.0e38f), mny = k(3.0e38f), mnz = k(3.0e38f);
            f4 mxx = k(-3.0e38f), mxy = k(-3.0e38f), mxz = k(-3.0e38f);
            for (uint32_t i = begin; i < end; i += 4){
                const f4 normAge = B::mul(ld(ctx.ch[ChAge], i), ld(ctx.ch[ChInvLife], i));
                // lanes past the end are unused slots
                const uint32_t valid = end - i >= 4 ? 0xfu : ((1u << (end - i)) - 1u);
                const u4 dead = B::cmpge(normAge, k(1.0f));
                const uint32_t deadBits = B::movemask(dead) & valid;
                for (uint32_t l = 0; l < 4; l++){
                    if (deadBits & (1u << l))
                        out.dead[out.deadCount++] = i + l;
                }
                const uint32_t liveBits = valid & ~deadBits;
                if (!liveBits)
                    continue;
                const u4 live = B::iset((liveBits & 1u) ? ~0u : 0u, (liveBits & 2u) ? ~0u : 0u, (liveBits & 4u) ? ~0u : 0u, (liveBits & 8u) ? ~0u : 0u);
                const f4 x = ld(ctx.ch[ChPosX], i), y = ld(ctx.ch[ChPosY], i), z = ld(ctx.ch[ChPosZ], i);
                mnx = B::min(mnx, B::select(live, x, k(3.0e38f)));
                mny = B::min(mny, B::select(live, y, k(3.0e38f)));
                mnz = B::min(mnz, B::select(live, z, k(3.0e38f)));
                mxx = B::max(mxx, B::select(live, x, k(-3.0e38f)));
                mxy = B::max(mxy, B::select(live, y, k(-3.0e38f)));
                mxz = B::max(mxz, B::select(live, z, k(-3.0e38f)));
            }
            for (int l = 0; l < 4; l++){
                out.boundsMin[0] = std::min(out.boundsMin[0], B::lane(mnx, l));
                out.boundsMin[1] = std::min(out.boundsMin[1], B::lane(mny, l));
                out.boundsMin[2] = std::min(out.boundsMin[2], B::lane(mnz, l));
                out.boundsMax[0] = std::max(out.boundsMax[0], B::lane(mxx, l));
                out.boundsMax[1] = std::max(out.boundsMax[1], B::lane(mxy, l));
                out.boundsMax[2] = std::max(out.boundsMax[2], B::lane(mxz, l));
            }
        }

        // ------------------------------------------------------------ dispatch

        static void spawnOp(const KernelContext& ctx, const Op& op, uint32_t begin, uint32_t end){
            switch (op.code){
                case OpCode::SpawnShape: spawnShape(ctx, param<ShapeParams>(ctx, op), begin, end); break;
                case OpCode::SpawnVelocity: spawnVelocity(ctx, param<VelocityParams>(ctx, op), begin, end); break;
                case OpCode::SpawnStartValues: spawnStartValues(ctx, param<StartParams>(ctx, op), begin, end); break;
                default: break;
            }
        }

        static void updateOp(const KernelContext& ctx, const Op& op, uint32_t begin, uint32_t end){
            switch (op.code){
                case OpCode::SetOverAge: setOverAge(ctx, param<SetOverAgeParams>(ctx, op), begin, end); break;
                case OpCode::Forces: forces(ctx, param<ForceParams>(ctx, op), begin, end); ctx.scratch->hasAccel = true; break;
                case OpCode::ExternalForces:
                    if (ctx.fieldCount > 0){
                        externalForces(ctx, param<ProgramExternalForces>(ctx, op), begin, end);
                        ctx.scratch->hasAccel = true;
                    }
                    break;
                case OpCode::AnimatedVelocity:
                    animatedVelocity(ctx, param<AnimatedVelocityParams>(ctx, op), begin, end);
                    ctx.scratch->hasAnimated = true;
                    break;
                case OpCode::Noise:
                    noiseOp(ctx, param<NoiseParams>(ctx, op), begin, end);
                    ctx.scratch->hasAnimated = true;
                    break;
                case OpCode::Integrate: {
                    const IntegrateOp& io = param<IntegrateOp>(ctx, op);
                    integrate(ctx, io.integrate, io.hasAnimatedVelocity ? &param<AnimatedVelocityParams>(ctx, io.animatedVelocity) : nullptr, begin, end);
                    break;
                }
                case OpCode::Orbit: orbit(ctx, param<OrbitParams>(ctx, op), begin, end); break;
                case OpCode::StoredAppearance: storedAppearance(ctx, param<StoredAppearanceParams>(ctx, op), begin, end); break;
                case OpCode::Collision: collision(ctx, param<CollisionParams>(ctx, op), begin, end); break;
                case OpCode::Kill: if (ctx.out) kill(ctx, begin, end); break;
                default: break;
            }
        }

        template<class T>
        static const T& param(const KernelContext& ctx, const Op& op){ return *reinterpret_cast<const T*>(ctx.pool + op.params); }
        template<class T>
        static const T& param(const KernelContext& ctx, uint32_t offset){ return *reinterpret_cast<const T*>(ctx.pool + offset); }
    };

}}

#endif // PARTICLES_PARTICLEKERNELS_H
