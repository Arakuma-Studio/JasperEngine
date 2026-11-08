// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleRender.h"

#include "jobs/JobSystem.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleEval.h"
#include "particles/ParticleKernels.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    #include <xmmintrin.h>
#endif

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    std::atomic<uint64_t> gPackCount{0};

    // a hint to start loading what a later iteration reads
    inline void prefetchRead(const void* p){
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(p, 0, 1);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T1);
#else
        (void)p;
#endif
    }

    // linear to sRGB8 (gamma 2.2), indexed by sqrt(linear) * 4095: fine steps in the darks
    const uint8_t* encodeTable(){
        static const std::vector<uint8_t> table = [](){
            std::vector<uint8_t> t(4097);
            for (int i = 0; i <= 4096; i++){
                const float s = std::min((float)i / 4095.0f, 1.0f);
                const float linear = s * s;
                t[i] = (uint8_t)std::min(255.0f, std::pow(linear, 1.0f / 2.2f) * 255.0f + 0.5f);
            }
            return t;
        }();
        return table.data();
    }

    struct PackJob {
        const EmitterRuntime* emitter = nullptr;
        const ParticleProgram* prog = nullptr;
        const RenderView* view = nullptr;
        float alpha = 1.0f;
        float tick = 0.0f;
        float keepThreshold = 1.0f;     // spawn hash below it: drawn
        uint32_t thinKey = 0;
        float sortLo = 0.0f, sortScale = 0.0f;
        ParticleInstance* instances = nullptr;
        ParticleMeshInstance* mesh = nullptr;
        ParticleCustomInstance* custom = nullptr;
        ParticlePixelInstance* pixel = nullptr;
        double time = 0.0;              // the emitter's seconds (Flicker)
        uint16_t* keys = nullptr;
        uint8_t* meshIndex = nullptr;   // the mesh of each record (more than one mesh)
    };

    template<class B>
    struct Pack {
        typedef typename B::f4 f4;
        typedef typename B::u4 u4;
        typedef Kernels<B> K;
        typedef simd::Math<B> M;

        // float to IEEE half (round to nearest even), in the low 16 bits; after F. Giesen's
        // float_to_half_fast3_rtne
        static u4 half(f4 f){
            const u4 bits = B::castu(f);
            const u4 sign = B::iand(bits, B::iset1(0x80000000u));
            const u4 x = B::ixor(bits, sign);
            const u4 overflow = B::icmpgt_s(x, B::iset1(0x477fefffu));   // >= 65520: infinity
            const u4 isNan = B::icmpgt_s(x, B::iset1(0x7f800000u));
            const u4 denormal = B::icmpgt_s(B::iset1(0x38800000u), x);  // < 2^-14
            // denormals: add 0.5 and take the low bits
            const f4 dn = B::add(B::castf(x), B::castf(B::iset1(0x3f000000u)));
            const u4 denormBits = B::isub(B::castu(dn), B::iset1(0x3f000000u));
            // normals: rebias the exponent, round to nearest even
            const u4 odd = B::iand(B::template isrl<13>(x), B::iset1(1u));
            const u4 normBits = B::template isrl<13>(B::iadd(B::iadd(x, B::iset1(0xc8000fffu)), odd));
            u4 h = B::iselect(denormal, denormBits, normBits);
            h = B::iselect(overflow, B::iset1(0x7c00u), h);
            h = B::iselect(isNan, B::iset1(0x7e00u), h);
            return B::ior(h, B::template isrl<16>(sign));
        }

        static u4 pair(u4 lo, u4 hi){ return B::ior(B::iand(lo, B::iset1(0xffffu)), B::template isll<16>(hi)); }

        static u4 unorm16(f4 v){ return B::cvtt(B::add(B::mul(M::clamp01(v), K::k(65535.0f)), K::k(0.5f))); }

        static u4 snorm16(f4 v){
            v = M::clamp(v, K::k(-1.0f), K::k(1.0f));
            const f4 r = B::mul(v, K::k(32767.0f));
            const f4 rounded = B::add(r, B::select(B::cmplt(r, B::zero()), K::k(-0.5f), K::k(0.5f)));
            return B::iand(B::cvtt(rounded), B::iset1(0xffffu));
        }

        static u4 encodeColor(f4 r, f4 g, f4 b, f4 a){
            const uint8_t* table = encodeTable();
            auto enc = [&](f4 x){
                const u4 idx = B::cvtt(B::mul(B::sqrt(M::clamp01(x)), K::k(4095.0f)));
                alignas(16) uint32_t v[4];
                B::storeu4(v, idx);
                return B::iset(table[v[0]], table[v[1]], table[v[2]], table[v[3]]);
            };
            const u4 ai = B::cvtt(B::add(B::mul(M::clamp01(a), K::k(255.0f)), K::k(0.5f)));
            return B::ior(B::ior(enc(r), B::template isll<8>(enc(g))), B::ior(B::template isll<16>(enc(b)), B::template isll<24>(ai)));
        }

        // appearance and packing of [begin, end) into out[outBegin...], the thinned left out
        static uint32_t run(const PackJob& job, const KernelContext& ctx, uint32_t begin, uint32_t end, uint32_t outBegin){
            const ParticleProgram& prog = *job.prog;
            const RenderParams& rp = prog.render;
            const float* decode = ParticleCompiler::srgbDecodeTable();
            const bool interp = job.alpha < 1.0f;
            const f4 alpha = K::k(job.alpha);
            const f4 back = K::k((1.0f - job.alpha) * job.tick);
            const PixelParams& pix = rp.pixel;
            const bool colorWork = rp.colorOverLifetime || rp.colorBySpeed || rp.storedColor || pix.fadeCurve || pix.exactColor;
            const uint32_t phaseKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::PixelArt, 1);
            const uint32_t wobbleXKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::PixelArt, 2);
            const uint32_t wobbleYKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::PixelArt, 3);
            uint32_t written = outBegin;
            const uint32_t flipKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::Main, 60);
            const uint32_t rowKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::TextureSheetAnimation, 3);
            const uint32_t listKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::TextureSheetAnimation, 4);
            const uint32_t meshKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::Renderer, 1);
            const uint32_t flipXKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::Renderer, 2);
            const uint32_t flipYKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::Renderer, 3);
            const bool flipX = rp.flip[0] > 0.0f && !rp.mesh, flipY = rp.flip[1] > 0.0f && !rp.mesh;
            // the Texture Sheet's Flip U / V: the frame mirrored on the quad (another mirror of it,
            // so the two compose)
            const uint32_t flipUKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::TextureSheetAnimation, 5);
            const uint32_t flipVKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::TextureSheetAnimation, 6);
            const bool flipU = rp.flipU > 0.0f && !rp.mesh && rp.sheet != SheetKind::None;
            const bool flipV = rp.flipV > 0.0f && !rp.mesh && rp.sheet != SheetKind::None;
            for (uint32_t i = begin; i < end; i += 4){
                typename K::Lane L(ctx, i);
                const uint32_t valid = end - i >= 4 ? 0xfu : ((1u << (end - i)) - 1u);
                uint32_t keepBits = valid;
                if (job.keepThreshold < 1.0f){
                    const f4 r = K::rand01(job.thinKey, L.spawn);
                    keepBits &= B::movemask(B::cmplt(r, K::k(job.keepThreshold)));
                }
                if (!keepBits)
                    continue;
                // the interpolated position and age
                f4 px = K::ld(ctx.ch[ChPosX], i), py = K::ld(ctx.ch[ChPosY], i), pz = K::ld(ctx.ch[ChPosZ], i);
                if (interp){
                    const f4 qx = K::ld(ctx.ch[ChPrevX], i), qy = K::ld(ctx.ch[ChPrevY], i), qz = K::ld(ctx.ch[ChPrevZ], i);
                    px = B::madd(B::sub(px, qx), alpha, qx);
                    py = B::madd(B::sub(py, qy), alpha, qy);
                    pz = B::madd(B::sub(pz, qz), alpha, qz);
                    L.age = B::max(B::sub(L.age, back), B::zero());
                    L.normAge = B::mul(L.age, L.invLife);
                }
                if (pix.flickerBelow > 0.0f){
                    keepBits &= ~flickerHidden(job, L, phaseKey);
                    if (!keepBits)
                        continue;
                }
                if (pix.wobble){
                    // A sin(2 pi f age + phase) base pixels along the screen, before the shader snaps (R7)
                    const f4 tau = K::k(6.28318530717958647692f);
                    const f4 phx = pix.wobbleRandomPhase ? B::mul(K::rand01(wobbleXKey, L.spawn), tau) : B::zero();
                    const f4 phy = pix.wobbleRandomPhase ? B::mul(K::rand01(wobbleYKey, L.spawn), tau) : B::zero();
                    const f4 ox = B::mul(K::sin(B::madd(B::mul(L.age, K::k(pix.wobbleFrequency[0])), tau, phx)), K::k(pix.wobbleAmplitude[0]));
                    const f4 oy = B::mul(K::sin(B::madd(B::mul(L.age, K::k(pix.wobbleFrequency[1])), tau, phy)), K::k(pix.wobbleAmplitude[1]));
                    const RenderView& v = *job.view;
                    px = B::add(px, B::add(B::mul(ox, K::k(v.pixelRight[0])), B::mul(oy, K::k(v.pixelUp[0]))));
                    py = B::add(py, B::add(B::mul(ox, K::k(v.pixelRight[1])), B::mul(oy, K::k(v.pixelUp[1]))));
                    pz = B::add(pz, B::add(B::mul(ox, K::k(v.pixelRight[2])), B::mul(oy, K::k(v.pixelUp[2]))));
                }
                f4 vx = K::ld(ctx.ch[ChVelX], i), vy = K::ld(ctx.ch[ChVelY], i), vz = K::ld(ctx.ch[ChVelZ], i);
                if (ctx.ch[ChAnimVelX]){
                    // along the total velocity (Velocity over Lifetime and Noise included)
                    vx = B::add(vx, K::ld(ctx.ch[ChAnimVelX], i));
                    vy = B::add(vy, K::ld(ctx.ch[ChAnimVelY], i));
                    vz = B::add(vz, K::ld(ctx.ch[ChAnimVelZ], i));
                }

                // size
                f4 sx, sy, sz;
                if (rp.storedSize){
                    sx = K::ld(ctx.ch[ChSizeX], i);
                    if (interp) sx = M::lerp(K::ld(ctx.ch[ChPrevSizeX], i), sx, alpha);
                    sy = ctx.ch[ChSizeY] ? K::ld(ctx.ch[ChSizeY], i) : sx;
                    sz = ctx.ch[ChSizeZ] ? K::ld(ctx.ch[ChSizeZ], i) : sx;
                }else{
                    sx = K::ld(ctx.ch[ChStartSize], i);
                    sy = ctx.ch[ChStartSizeY] ? K::ld(ctx.ch[ChStartSizeY], i) : sx;
                    sz = ctx.ch[ChStartSizeZ] ? K::ld(ctx.ch[ChStartSizeZ], i) : sx;
                }
                if (rp.sizeOverLifetime){
                    const f4 m = K::curve(rp.sizeX, L);
                    sx = B::mul(sx, m);
                    sy = B::mul(sy, rp.sizeOverLifetime3D ? K::curve(rp.sizeY, L) : m);
                    sz = B::mul(sz, rp.sizeOverLifetime3D ? K::curve(rp.sizeZ, L) : m);
                }
                if (rp.sizeBySpeed){
                    const f4 m = K::curve(rp.speedSizeX, L);
                    sx = B::mul(sx, m);
                    sy = B::mul(sy, rp.sizeBySpeed3D ? K::curve(rp.speedSizeY, L) : m);
                    sz = B::mul(sz, rp.sizeBySpeed3D ? K::curve(rp.speedSizeZ, L) : m);
                }
                if (rp.noiseSize){
                    const f4 n = K::ld(ctx.ch[ChNoiseSize], i);
                    sx = B::mul(sx, n); sy = B::mul(sy, n); sz = B::mul(sz, n);
                }
                // the Renderer's Flip (the share of the particles mirrored on each axis): a
                // negative size, which mirrors the quad and its texture
                if (flipX)
                    sx = B::select(B::cmplt(K::rand01(flipXKey, L.spawn), K::k(rp.flip[0])), B::neg(sx), sx);
                if (flipY)
                    sy = B::select(B::cmplt(K::rand01(flipYKey, L.spawn), K::k(rp.flip[1])), B::neg(sy), sy);
                if (flipU)
                    sx = B::select(B::cmplt(K::rand01(flipUKey, L.spawn), K::k(rp.flipU)), B::neg(sx), sx);
                if (flipV)
                    sy = B::select(B::cmplt(K::rand01(flipVKey, L.spawn), K::k(rp.flipV)), B::neg(sy), sy);

                // colour
                u4 color;
                if (!colorWork){
                    color = K::ldu(ctx.ch[ChStartColor], i);
                }else{
                    f4 c[4];
                    if (rp.storedColor){
                        c[0] = K::ld(ctx.ch[ChColorR], i); c[1] = K::ld(ctx.ch[ChColorG], i);
                        c[2] = K::ld(ctx.ch[ChColorB], i); c[3] = K::ld(ctx.ch[ChColorA], i);
                    }else{
                        K::unpackColor(K::ldu(ctx.ch[ChStartColor], i), decode, c);
                    }
                    if (rp.colorOverLifetime){
                        f4 m[4];
                        K::gradient(rp.color, L, m);
                        for (int k = 0; k < 4; k++) c[k] = B::mul(c[k], m[k]);
                    }
                    if (rp.colorBySpeed){
                        f4 m[4];
                        K::gradient(rp.speedColor, L, m);
                        for (int k = 0; k < 4; k++) c[k] = B::mul(c[k], m[k]);
                    }
                    if (pix.fadeCurve)
                        c[3] = B::mul(c[3], M::clamp01(K::curve(pix.fadeOverLifetime, L)));
                    color = encodeColor(c[0], c[1], c[2], c[3]);
                    if (pix.exactColor){
                        // a palette ramp's step drawn as its own bytes (R13): the rgb is not re-encoded
                        u4 rgb;
                        if (pix.rampSource == 2){
                            alignas(16) uint32_t idx[4];
                            K::rampIndex(pix.ramp, L, idx);
                            const uint32_t* bytes = reinterpret_cast<const uint32_t*>(ctx.pool + pix.ramp.lut1);
                            rgb = B::iset(bytes[idx[0]], bytes[idx[1]], bytes[idx[2]], bytes[idx[3]]);
                        }else{
                            rgb = K::ldu(ctx.ch[ChStartColor], i);
                        }
                        color = B::ior(B::iand(rgb, B::iset1(0x00ffffffu)), B::iand(color, B::iset1(0xff000000u)));
                    }
                }

                // rotation
                f4 rot;
                if (rp.storedRotation){
                    rot = K::ld(ctx.ch[ChRotation], i);
                    if (interp) rot = M::lerp(K::ld(ctx.ch[ChPrevRotation], i), rot, alpha);
                }else{
                    if (rp.startRotationStored){
                        rot = K::ld(ctx.ch[ChStartRotation], i);
                    }else{
                        rot = K::curve(rp.startRotation, L);
                        if (rp.flipRotation > 0.0f)
                            rot = B::select(B::cmplt(K::rand01(flipKey, L.spawn), K::k(rp.flipRotation)), B::neg(rot), rot);
                    }
                    if (rp.rotationOverLifetime){
                        const CurveParam& w = rp.rotationRate;
                        if (w.kind == CurveParam::Const || w.kind == CurveParam::Range)
                            rot = B::madd(K::curve(w, L), L.age, rot);
                        else
                            rot = B::add(rot, B::div(K::curve(w, L), L.invLife));   // the integral LUT, times the lifetime
                    }
                }
                const f4 turns = M::fract(B::mul(rot, K::k(0.15915494309189535f)));

                // the sprite frame
                f4 frame01 = B::zero();
                if (rp.storedFrame && rp.frameCount > 0){
                    frame01 = B::div(B::add(K::ld(ctx.ch[ChFrame], i), K::k(0.5f)), K::k((float)rp.frameCount));
                }else if (rp.sheet != SheetKind::None){
                    const float total = (float)rp.frameCount;
                    f4 f;
                    switch (rp.sheetTime){
                        case TextureSheetTimeMode::FPS:
                            f = B::mul(L.age, K::k(rp.fps));
                            break;
                        case TextureSheetTimeMode::Speed:
                            f = B::mul(K::curve(rp.frameOverTime, L), K::k(total));
                            break;
                        case TextureSheetTimeMode::Lifetime:
                        default: {
                            // Cycles repeat the frame curve over the life
                            const f4 cyc = B::mul(L.normAge, K::k(rp.cycles));
                            const f4 t = B::select(B::cmpge(cyc, K::k(rp.cycles)), K::k(1.0f), M::fract(cyc));
                            typename K::Lane C = L;
                            C.normAge = t;
                            f = B::mul(K::curve(rp.frameOverTime, C), K::k(total));
                            break;
                        }
                    }
                    f = B::add(f, K::curve(rp.startFrame, L));
                    if (rp.startFrameListCount > 0){
                        const uint32_t* list = reinterpret_cast<const uint32_t*>(ctx.pool + rp.startFrameList);
                        alignas(16) float r[4], picked[4];
                        B::storeu(r, K::rand01(listKey, L.spawn));
                        for (int l = 0; l < 4; l++){
                            uint32_t k = (uint32_t)(r[l] * (float)rp.startFrameListCount);
                            if (k >= rp.startFrameListCount) k = rp.startFrameListCount - 1;
                            picked[l] = (float)list[k];
                        }
                        f = B::add(f, B::loadu(picked));
                    }
                    // wrap into the frames, keeping the blend fraction
                    f = B::sub(f, B::mul(B::floor(B::div(f, K::k(total))), K::k(total)));
                    if (!rp.frameBlending)
                        f = B::add(B::floor(f), K::k(0.5f));
                    if (rp.sheetSingleRow){
                        const f4 row = rp.sheetRandomRow ? B::floor(B::mul(K::rand01(rowKey, L.spawn), K::k((float)rp.tilesY)))
                                                         : K::k((float)rp.rowIndex);
                        f = B::madd(row, K::k((float)rp.tilesX), f);
                        frame01 = B::div(f, K::k((float)(rp.tilesX * rp.tilesY)));
                    }else{
                        frame01 = B::div(f, K::k(total));
                    }
                }

                // the stretch axis (or the mesh's alignment): the velocity
                const u4 rotationFrame = pair(unorm16(turns), unorm16(frame01));
                const u4 sizeXY = pair(half(sx), half(sy));
                const u4 velXY = pair(half(vx), half(vy));
                const u4 velZW = pair(half(vz), half(rp.mesh ? sz : K::k(rp.emissive)));
                const u4 xb = B::castu(px), yb = B::castu(py), zb = B::castu(pz);

                alignas(16) uint32_t pixelWords[4] = {0, 0, 0, 0};
                if (job.pixel){
                    // x the ramp step, y the age, z the phase, w the flags (R10)
                    alignas(16) uint32_t steps[4] = {255u, 255u, 255u, 255u};
                    if (pix.rampSource)
                        K::rampIndex(pix.ramp, L, steps);
                    alignas(16) uint32_t ages[4], phases[4], rest[4] = {0, 0, 0, 0};
                    B::storeu4(ages, B::cvtt(B::add(B::mul(M::clamp01(L.normAge), K::k(255.0f)), K::k(0.5f))));
                    B::storeu4(phases, B::cvtt(B::mul(K::rand01(phaseKey, L.spawn), K::k(256.0f))));
                    if (ctx.ch[ChSettled])
                        B::storeu4(rest, B::cvtt(K::ld(ctx.ch[ChSettled], i)));
                    for (int l = 0; l < 4; l++)
                        pixelWords[l] = std::min(steps[l], 255u) | (std::min(ages[l], 255u) << 8) | ((phases[l] & 255u) << 16) |
                                        ((rest[l] ? (uint32_t)kPixelFlagRest : 0u) << 24);
                }

                // four whole billboard records: transposed straight into the instances (with their
                // sort keys when the view sorts)
                if (keepBits == 0xfu && !job.mesh && !job.custom && !job.meshIndex){
                    if (job.pixel)
                        std::memcpy(&job.pixel[written], pixelWords, sizeof(pixelWords));
                    if (job.keys){
                        alignas(16) uint32_t keys[4];
                        B::storeu4(keys, sortKeys(job, L, px, py, pz));
                        for (int l = 0; l < 4; l++) job.keys[written + l] = (uint16_t)keys[l];
                    }
                    static_assert(sizeof(ParticleInstance) == 8 * sizeof(uint32_t), "two rows of four words");
                    alignas(16) uint32_t rows[2][16];
                    B::storeTransposed(rows[0], xb, yb, zb, color);
                    B::storeTransposed(rows[1], sizeXY, rotationFrame, velXY, velZW);
                    uint32_t* out = reinterpret_cast<uint32_t*>(&job.instances[written]);
                    for (int l = 0; l < 4; l++){
                        std::memcpy(out + l * 8, rows[0] + l * 4, 16);
                        std::memcpy(out + l * 8 + 4, rows[1] + l * 4, 16);
                    }
                    written += 4;
                    continue;
                }

                alignas(16) uint32_t ax[4], ay[4], az[4], ac[4], as[4], ar[4], av[4], aw[4];
                B::storeu4(ax, xb); B::storeu4(ay, yb); B::storeu4(az, zb); B::storeu4(ac, color);
                B::storeu4(as, sizeXY); B::storeu4(ar, rotationFrame); B::storeu4(av, velXY); B::storeu4(aw, velZW);
                alignas(16) uint32_t qa[4] = {0, 0, 0, 0}, qb[4] = {0, 0, 0, 0};
                if (job.mesh){
                    f4 qx, qy, qz, qw;
                    if (ctx.ch[ChQuatX]){
                        qx = K::ld(ctx.ch[ChQuatX], i); qy = K::ld(ctx.ch[ChQuatY], i);
                        qz = K::ld(ctx.ch[ChQuatZ], i); qw = K::ld(ctx.ch[ChQuatW], i);
                    }else{
                        f4 s, c;
                        M::sincos(B::mul(rot, K::k(0.5f)), s, c);
                        qx = B::zero(); qy = B::zero(); qz = s; qw = c;
                    }
                    alignMesh(job, rp, px, py, pz, qx, qy, qz, qw);
                    B::storeu4(qa, pair(snorm16(qx), snorm16(qy)));
                    B::storeu4(qb, pair(snorm16(qz), snorm16(qw)));
                }
                alignas(16) uint32_t ca[4] = {0, 0, 0, 0}, cb[4] = {0, 0, 0, 0};
                if (job.custom){
                    B::storeu4(ca, pair(half(K::curve(rp.custom[0], L)), half(K::curve(rp.custom[1], L))));
                    B::storeu4(cb, pair(half(K::curve(rp.custom[2], L)), half(K::curve(rp.custom[3], L))));
                }
                alignas(16) uint32_t meshPick[4] = {0, 0, 0, 0};
                if (job.meshIndex){
                    // Random: a hash of the spawn index; By Spawn Index: the index modulo the count
                    alignas(16) uint32_t ids[4];
                    alignas(16) float r[4];
                    B::storeu4(ids, L.spawn);
                    B::storeu(r, K::rand01(meshKey, L.spawn));
                    for (int l = 0; l < 4; l++){
                        meshPick[l] = rp.meshSelection == (uint8_t)MeshSelection::Index ? ids[l] % rp.meshCount
                                                                                         : std::min((uint32_t)(r[l] * (float)rp.meshCount), rp.meshCount - 1);
                    }
                }
                alignas(16) uint32_t keyBits[4] = {0, 0, 0, 0};
                if (job.keys)
                    B::storeu4(keyBits, sortKeys(job, L, px, py, pz));
                for (uint32_t l = 0; l < 4; l++){
                    if (!(keepBits & (1u << l)))
                        continue;
                    ParticleInstance& o = job.instances[written];
                    std::memcpy(&o.position[0], &ax[l], 4);
                    std::memcpy(&o.position[1], &ay[l], 4);
                    std::memcpy(&o.position[2], &az[l], 4);
                    o.color = ac[l];
                    o.size = as[l];
                    o.rotationFrame = ar[l];
                    o.velocityXY = av[l];
                    o.velocityZW = aw[l];
                    if (job.mesh){
                        ParticleMeshInstance& m = job.mesh[written];
                        m.rotation[0] = (int16_t)(qa[l] & 0xffffu);
                        m.rotation[1] = (int16_t)(qa[l] >> 16);
                        m.rotation[2] = (int16_t)(qb[l] & 0xffffu);
                        m.rotation[3] = (int16_t)(qb[l] >> 16);
                    }
                    if (job.custom){
                        ParticleCustomInstance& c = job.custom[written];
                        c.data[0] = (uint16_t)(ca[l] & 0xffffu);
                        c.data[1] = (uint16_t)(ca[l] >> 16);
                        c.data[2] = (uint16_t)(cb[l] & 0xffffu);
                        c.data[3] = (uint16_t)(cb[l] >> 16);
                    }
                    if (job.keys)
                        job.keys[written] = (uint16_t)keyBits[l];
                    if (job.pixel)
                        std::memcpy(&job.pixel[written], &pixelWords[l], sizeof(uint32_t));
                    if (job.meshIndex)
                        job.meshIndex[written] = (uint8_t)meshPick[l];
                    written++;
                }
            }
            return written - outBegin;
        }

        // Mesh particles' Render Alignment (Simulation and Velocity need nothing here: the shader
        // turns Velocity's): View, World and Local put the view's rotation in front of the
        // particle's; Facing turns each to the camera position (+Z towards it, up the camera's)
        static void alignMesh(const PackJob& job, const RenderParams& rp, f4 px, f4 py, f4 pz, f4& qx, f4& qy, f4& qz, f4& qw){
            switch (rp.meshAlignment){
                case MeshAlignment::View:
                case MeshAlignment::World:
                case MeshAlignment::Local:
                    K::quatMulUniform(job.view->alignQuat, qx, qy, qz, qw);
                    break;
                case MeshAlignment::Facing: {
                    const RenderView& v = *job.view;
                    alignas(16) float x[4], y[4], z[4], out[4][4];
                    B::storeu(x, px); B::storeu(y, py); B::storeu(z, pz);
                    for (int l = 0; l < 4; l++){
                        float f[3] = {v.position[0] - x[l], v.position[1] - y[l], v.position[2] - z[l]};
                        float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                        if (fl < 1.0e-6f){ f[0] = -v.forward[0]; f[1] = -v.forward[1]; f[2] = -v.forward[2]; fl = 1.0f; }
                        for (float& c : f) c /= fl;
                        // right = up x forward, up' = forward x right
                        float r[3] = {v.up[1] * f[2] - v.up[2] * f[1], v.up[2] * f[0] - v.up[0] * f[2], v.up[0] * f[1] - v.up[1] * f[0]};
                        float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                        if (rl < 1.0e-6f){
                            const float helper[3] = {std::fabs(f[1]) < 0.99f ? 0.0f : 1.0f, std::fabs(f[1]) < 0.99f ? 1.0f : 0.0f, 0.0f};
                            r[0] = helper[1] * f[2] - helper[2] * f[1]; r[1] = helper[2] * f[0] - helper[0] * f[2]; r[2] = helper[0] * f[1] - helper[1] * f[0];
                            rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                        }
                        for (float& c : r) c /= rl;
                        const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
                        quatFromAxes(r, u, f, out[l]);
                    }
                    const f4 ax = B::set(out[0][0], out[1][0], out[2][0], out[3][0]);
                    const f4 ay = B::set(out[0][1], out[1][1], out[2][1], out[3][1]);
                    const f4 az = B::set(out[0][2], out[1][2], out[2][2], out[3][2]);
                    const f4 aw = B::set(out[0][3], out[1][3], out[2][3], out[3][3]);
                    K::quatMulLanes(ax, ay, az, aw, qx, qy, qz, qw);
                    break;
                }
                case MeshAlignment::Simulation:
                case MeshAlignment::Velocity:
                default:
                    break;
            }
        }

        // the quaternion (x, y, z, w) of the rotation whose columns are the unit axes x, y, z
        static void quatFromAxes(const float x[3], const float y[3], const float z[3], float q[4]){
            const float trace = x[0] + y[1] + z[2];
            if (trace > 0.0f){
                const float s = std::sqrt(trace + 1.0f) * 2.0f;
                q[3] = 0.25f * s;
                q[0] = (y[2] - z[1]) / s;
                q[1] = (z[0] - x[2]) / s;
                q[2] = (x[1] - y[0]) / s;
            }else if (x[0] > y[1] && x[0] > z[2]){
                const float s = std::sqrt(1.0f + x[0] - y[1] - z[2]) * 2.0f;
                q[3] = (y[2] - z[1]) / s;
                q[0] = 0.25f * s;
                q[1] = (y[0] + x[1]) / s;
                q[2] = (z[0] + x[2]) / s;
            }else if (y[1] > z[2]){
                const float s = std::sqrt(1.0f + y[1] - x[0] - z[2]) * 2.0f;
                q[3] = (z[0] - x[2]) / s;
                q[0] = (y[0] + x[1]) / s;
                q[1] = 0.25f * s;
                q[2] = (z[1] + y[2]) / s;
            }else{
                const float s = std::sqrt(1.0f + z[2] - x[0] - y[1]) * 2.0f;
                q[3] = (x[1] - y[0]) / s;
                q[0] = (z[0] + x[2]) / s;
                q[1] = (z[1] + y[2]) / s;
                q[2] = 0.25f * s;
            }
        }

        // the 16-bit back-to-front keys of four particles: far first, the key falls as the metric grows
        static u4 sortKeys(const PackJob& job, const typename K::Lane& L, f4 px, f4 py, f4 pz){
            const RenderParams& rp = job.prog->render;
            f4 metric;
            switch (rp.sort){
                case SortMode::ByDepth:
                    metric = K::dot3(B::sub(px, K::k(job.view->position[0])), B::sub(py, K::k(job.view->position[1])), B::sub(pz, K::k(job.view->position[2])),
                                     K::k(job.view->forward[0]), K::k(job.view->forward[1]), K::k(job.view->forward[2]));
                    break;
                case SortMode::OldestInFront:
                    // the youngest first: the oldest drawn last, on top
                    metric = B::sub(K::k(1.0f), L.normAge);
                    break;
                case SortMode::YoungestInFront:
                    metric = L.normAge;
                    break;
                case SortMode::ByDistance:
                default:
                    metric = K::length3(B::sub(px, K::k(job.view->position[0])), B::sub(py, K::k(job.view->position[1])), B::sub(pz, K::k(job.view->position[2])));
                    break;
            }
            const f4 q = M::clamp01(B::mul(B::sub(metric, K::k(job.sortLo)), K::k(job.sortScale)));
            return B::cvtt(B::mul(B::sub(K::k(1.0f), q), K::k(65535.0f)));
        }

        // the lanes Flicker hides now (pixelfx EMBER): less than flickerBelow of the life left and
        // sin(2 pi rate time + phase) < 0, the phase the particle's own; L's age interpolated
        static uint32_t flickerHidden(const PackJob& job, const typename K::Lane& L, uint32_t phaseKey){
            const PixelParams& pix = job.prog->render.pixel;
            const f4 left = B::sub(K::k(1.0f), L.normAge);
            const float base = (float)std::fmod(job.time * (double)pix.flickerRate, 1.0);
            const f4 cycle = B::madd(K::rand01(phaseKey, L.spawn), K::k(1.0f), K::k(base));
            const f4 s = K::sin(B::mul(cycle, K::k(6.28318530717958647692f)));
            return B::movemask(B::iand(B::cmplt(left, K::k(pix.flickerBelow)), B::cmplt(s, B::zero())));
        }

        // how many of [begin, end) thinning and Flicker keep
        static uint32_t countKept(const PackJob& job, const KernelContext& ctx, uint32_t begin, uint32_t end){
            const PixelParams& pix = job.prog->render.pixel;
            const bool flicker = pix.flickerBelow > 0.0f;
            if (job.keepThreshold >= 1.0f && !flicker)
                return end - begin;
            const uint32_t phaseKey = ctx.seed ^ randomSalt((uint32_t)ModuleId::PixelArt, 1);
            const f4 back = K::k((1.0f - job.alpha) * job.tick);
            uint32_t n = 0;
            for (uint32_t i = begin; i < end; i += 4){
                const uint32_t valid = end - i >= 4 ? 0xfu : ((1u << (end - i)) - 1u);
                uint32_t bits = valid;
                if (job.keepThreshold < 1.0f){
                    const f4 r = K::rand01(job.thinKey, K::ldu(ctx.ch[ChSpawnIndex], i));
                    bits &= B::movemask(B::cmplt(r, K::k(job.keepThreshold)));
                }
                if (flicker && bits){
                    typename K::Lane L(ctx, i);
                    if (job.alpha < 1.0f){
                        L.age = B::max(B::sub(L.age, back), B::zero());
                        L.normAge = B::mul(L.age, L.invLife);
                    }
                    bits &= ~flickerHidden(job, L, phaseKey);
                }
                n += (uint32_t)((bits & 1u) + ((bits >> 1) & 1u) + ((bits >> 2) & 1u) + ((bits >> 3) & 1u));
            }
            return n;
        }
    };

    template<class B>
    PackResult packImpl(const EmitterRuntime& emitter, const RenderView& view, memory::Arena& arena, jobs::JobSystem* jobs){
        PackResult result;
        const ParticleProgram* prog = emitter.program();
        const uint32_t count = emitter.count();
        if (!prog || count == 0 || prog->render.mode == RenderMode::None || emitter.isPrewarming())
            return result;
        const RenderParams& rp = prog->render;
        PackJob job;
        job.emitter = &emitter;
        job.prog = prog;
        job.view = &view;
        // Hold Frames draws the state of the step as it is, without interpolation (S49 R5)
        job.alpha = rp.pixel.holdFrameRate > 0.0f ? 1.0f : emitter.interpolation();
        job.tick = emitter.lastTickSeconds();
        job.time = emitter.time();
        gPackCount.fetch_add(1, std::memory_order_relaxed);
        job.keepThreshold = std::clamp(view.keep, 0.0f, 1.0f);
        job.thinKey = emitter.seedValue() ^ randomSalt((uint32_t)ModuleId::Renderer, 7);
        const bool sort = view.sort && rp.sort != SortMode::None;
        if (sort){
            if (rp.sort == SortMode::ByDistance || rp.sort == SortMode::ByDepth){
                float lo[3], hi[3];
                emitter.bounds(lo, hi);
                // the metric's range over the bounds' corners
                float mn = 3.0e38f, mx = -3.0e38f;
                for (int c = 0; c < 8; c++){
                    const float p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]};
                    const float d[3] = {p[0] - view.position[0], p[1] - view.position[1], p[2] - view.position[2]};
                    const float m = rp.sort == SortMode::ByDepth ? d[0] * view.forward[0] + d[1] * view.forward[1] + d[2] * view.forward[2]
                                                                 : std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    mn = std::min(mn, m);
                    mx = std::max(mx, m);
                }
                if (rp.sort == SortMode::ByDistance){
                    // the nearest: the distance to the box (0 inside it), not to a corner
                    float d2 = 0.0f;
                    for (int k = 0; k < 3; k++){
                        const float d = std::max({lo[k] - view.position[k], 0.0f, view.position[k] - hi[k]});
                        d2 += d * d;
                    }
                    mn = std::sqrt(d2);
                }
                job.sortLo = mn;
                job.sortScale = mx > mn ? 1.0f / (mx - mn) : 0.0f;
            }else{
                job.sortLo = 0.0f;
                job.sortScale = 1.0f;
            }
        }

        KernelContext ctx;
        const ParticleChannels& ch = emitter.particleChannels();
        for (int c = 0; c < ChCount; c++) ctx.ch[c] = ch.get((Channel)c);
        ctx.program = prog;
        ctx.pool = reinterpret_cast<const unsigned char*>(prog->pool.data());
        ctx.seed = emitter.seedValue();
        ctx.emitterTime = prog->duration > 0.0f ? (float)std::clamp(emitter.emitterTime() / prog->duration, 0.0, 1.0) : 0.0f;

        const uint32_t chunk = kChunkSize;
        const uint32_t chunks = (count + chunk - 1) / chunk;
        uint32_t* offsets = arena.allocateArrayForOverwrite<uint32_t>(chunks + 1);
        jobs::JobSystem& js = jobs ? *jobs : jobs::system();
        // thinning keeps a varying number per chunk: count first, then pack at the offsets
        offsets[0] = 0;
        if (job.keepThreshold < 1.0f || rp.pixel.flickerBelow > 0.0f){
            uint32_t* kept = arena.allocateArrayForOverwrite<uint32_t>(chunks);
            js.parallelFor(chunks, [&](size_t c){
                const uint32_t b = (uint32_t)c * chunk;
                kept[c] = Pack<B>::countKept(job, ctx, b, std::min(b + chunk, count));
            });
            for (uint32_t c = 0; c < chunks; c++) offsets[c + 1] = offsets[c] + kept[c];
        }else{
            for (uint32_t c = 0; c < chunks; c++) offsets[c + 1] = std::min((c + 1) * chunk, count);
        }
        const uint32_t total = offsets[chunks];
        if (total == 0)
            return result;
        job.instances = static_cast<ParticleInstance*>(arena.allocate(sizeof(ParticleInstance) * total, 16));
        job.mesh = rp.mesh ? static_cast<ParticleMeshInstance*>(arena.allocate(sizeof(ParticleMeshInstance) * total, 16)) : nullptr;
        job.custom = rp.customData ? static_cast<ParticleCustomInstance*>(arena.allocate(sizeof(ParticleCustomInstance) * total, 16)) : nullptr;
        job.pixel = rp.pixel.needsStream() ? static_cast<ParticlePixelInstance*>(arena.allocate(sizeof(ParticlePixelInstance) * total, 16)) : nullptr;
        job.keys = sort ? arena.allocateArrayForOverwrite<uint16_t>(total) : nullptr;
        job.meshIndex = (rp.mesh && rp.meshCount > 1) ? arena.allocateArrayForOverwrite<uint8_t>(total) : nullptr;
        js.parallelFor(chunks, [&](size_t c){
            const uint32_t b = (uint32_t)c * chunk;
            Pack<B>::run(job, ctx, b, std::min(b + chunk, count), offsets[c]);
        });
        result.count = total;
        result.instances = job.instances;
        result.mesh = job.mesh;
        result.custom = job.custom;
        result.pixel = job.pixel;
        result.meshFirst[0] = 0;
        result.meshCount[0] = total;
        if (!sort && !job.meshIndex)
            return result;
        // the draw order: sorted by key (stable LSD), then grouped by mesh (one draw per mesh)
        uint32_t* order = arena.allocateArrayForOverwrite<uint32_t>(total);
        uint32_t* scratch = arena.allocateArrayForOverwrite<uint32_t>(total);
        if (sort){
            ParticleRenderPrep::radixSort16Sequence(job.keys, order, scratch, arena.allocateArrayForOverwrite<uint16_t>(total), total);
        }else{
            for (uint32_t i = 0; i < total; i++) order[i] = i;
        }
        if (job.meshIndex){
            uint32_t counts[kMaxRendererMeshes] = {};
            for (uint32_t i = 0; i < total; i++) counts[job.meshIndex[i]]++;
            uint32_t first[kMaxRendererMeshes];
            uint32_t sum = 0;
            for (uint32_t m = 0; m < kMaxRendererMeshes; m++){
                first[m] = sum;
                result.meshFirst[m] = sum;
                result.meshCount[m] = counts[m];
                sum += counts[m];
            }
            for (uint32_t i = 0; i < total; i++){
                const uint32_t index = order[i];
                scratch[first[job.meshIndex[index]]++] = index;
            }
            std::memcpy(order, scratch, total * sizeof(uint32_t));
        }
        ParticleInstance* sorted = static_cast<ParticleInstance*>(arena.allocate(sizeof(ParticleInstance) * total, 16));
        // the records in the draw order: a gather, its reads fetched ahead (the order is random)
        constexpr uint32_t kAhead = 16;
        for (uint32_t i = 0; i < total; i++){
            if (i + kAhead < total) prefetchRead(job.instances + order[i + kAhead]);
            sorted[i] = job.instances[order[i]];
        }
        result.instances = sorted;
        if (job.mesh){
            ParticleMeshInstance* m = static_cast<ParticleMeshInstance*>(arena.allocate(sizeof(ParticleMeshInstance) * total, 16));
            for (uint32_t i = 0; i < total; i++) m[i] = job.mesh[order[i]];
            result.mesh = m;
        }
        if (job.custom){
            ParticleCustomInstance* cu = static_cast<ParticleCustomInstance*>(arena.allocate(sizeof(ParticleCustomInstance) * total, 16));
            for (uint32_t i = 0; i < total; i++) cu[i] = job.custom[order[i]];
            result.custom = cu;
        }
        if (job.pixel){
            ParticlePixelInstance* px = static_cast<ParticlePixelInstance*>(arena.allocate(sizeof(ParticlePixelInstance) * total, 16));
            for (uint32_t i = 0; i < total; i++) px[i] = job.pixel[order[i]];
            result.pixel = px;
        }
        return result;
    }

}

PackResult ParticleRenderPrep::packWith(bool scalar, const EmitterRuntime& emitter, const RenderView& view, memory::Arena& arena, jobs::JobSystem* jobs){
    return scalar ? packImpl<simd::Scalar>(emitter, view, arena, jobs) : packImpl<simd::Native>(emitter, view, arena, jobs);
}

PackResult ParticleRenderPrep::pack(const EmitterRuntime& emitter, const RenderView& view, memory::Arena& arena, jobs::JobSystem* jobs){
    return packWith(forceScalarKernels(), emitter, view, arena, jobs);
}

PackResult HeldPack::get(const EmitterRuntime& emitter, const RenderView& view, memory::Arena& arena, jobs::JobSystem* jobs, float rate){
    const double now = std::floor(emitter.time() * (double)rate + 1.0e-9);
    if (now != step || emitter.generation() != generation || emitter.effect() != effect){
        const PackResult fresh = ParticleRenderPrep::pack(emitter, view, arena, jobs);
        step = now;
        generation = emitter.generation();
        effect = emitter.effect();
        instances.assign(fresh.instances, fresh.instances + fresh.count);
        if (fresh.pixel)
            pixels.assign(fresh.pixel, fresh.pixel + fresh.count);
        else
            pixels.clear();
    }
    PackResult held;
    held.count = (uint32_t)instances.size();
    held.instances = instances.data();
    held.pixel = pixels.empty() ? nullptr : pixels.data();
    held.meshCount[0] = held.count;
    return held;
}

HeldPack& HeldPacks::of(uint64_t view){
    uses++;
    for (Entry& e : entries){
        if (e.view == view){
            e.lastUse = uses;
            return e.pack;
        }
    }
    if (entries.size() < kMaxViews){
        entries.push_back(Entry());
        entries.back().view = view;
        entries.back().lastUse = uses;
        return entries.back().pack;
    }
    Entry* oldest = &entries[0];
    for (Entry& e : entries){
        if (e.lastUse < oldest->lastUse) oldest = &e;
    }
    oldest->view = view;
    oldest->lastUse = uses;
    oldest->pack.clear();
    return oldest->pack;
}

uint64_t ParticleRenderPrep::packCount(){
    return gPackCount.load(std::memory_order_relaxed);
}

void ParticleRenderPrep::radixSort16Sequence(const uint16_t* keys, uint32_t* out, uint32_t* scratch, uint16_t* keyScratch, uint32_t count){
    // radixSort16 of the indices 0..count-1: the first pass reads the keys in place
    uint32_t low[256] = {}, high[256] = {};
    for (uint32_t i = 0; i < count; i++){
        const uint16_t k = keys[i];
        low[k & 255u]++;
        high[k >> 8]++;
    }
    uint32_t sumLow = 0, sumHigh = 0;
    for (uint32_t b = 0; b < 256; b++){
        const uint32_t l = low[b], h = high[b];
        low[b] = sumLow;
        high[b] = sumHigh;
        sumLow += l;
        sumHigh += h;
    }
    for (uint32_t i = 0; i < count; i++){
        const uint16_t k = keys[i];
        const uint32_t at = low[k & 255u]++;
        scratch[at] = i;
        keyScratch[at] = k;
    }
    for (uint32_t i = 0; i < count; i++)
        out[high[keyScratch[i] >> 8]++] = scratch[i];
}

void ParticleRenderPrep::radixSort16(const uint16_t* keys, uint32_t* indices, uint32_t* scratch, uint16_t* keyScratch, uint32_t count){
    uint32_t low[256] = {}, high[256] = {};
    for (uint32_t i = 0; i < count; i++){
        const uint16_t k = keys[indices[i]];
        low[k & 255u]++;
        high[k >> 8]++;
    }
    uint32_t sumLow = 0, sumHigh = 0;
    for (uint32_t b = 0; b < 256; b++){
        const uint32_t l = low[b], h = high[b];
        low[b] = sumLow;
        high[b] = sumHigh;
        sumLow += l;
        sumHigh += h;
    }
    for (uint32_t i = 0; i < count; i++){
        const uint32_t index = indices[i];
        const uint16_t k = keys[index];
        const uint32_t at = low[k & 255u]++;
        scratch[at] = index;
        keyScratch[at] = k;
    }
    for (uint32_t i = 0; i < count; i++)
        indices[high[keyScratch[i] >> 8]++] = scratch[i];
}

uint16_t ParticleRenderPrep::toHalf(float value){
    // Pack::half on one value, without the vector round trip
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t x = bits ^ sign;
    uint32_t h;
    if ((int32_t)x > 0x7f800000){
        h = 0x7e00u;
    }else if ((int32_t)x > 0x477fefff){
        h = 0x7c00u;
    }else if ((int32_t)x < 0x38800000){
        float f;
        std::memcpy(&f, &x, 4);
        f += 0.5f;
        uint32_t dn;
        std::memcpy(&dn, &f, 4);
        h = dn - 0x3f000000u;
    }else{
        h = (x + 0xc8000fffu + ((x >> 13) & 1u)) >> 13;
    }
    return (uint16_t)(h | (sign >> 16));
}

float ParticleRenderPrep::fromHalf(uint16_t h){
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exponent = (h >> 10) & 0x1fu;
    const uint32_t mantissa = h & 0x3ffu;
    uint32_t bits;
    if (exponent == 0){
        if (mantissa == 0){
            bits = sign;
        }else{
            // a denormal half: normalize
            float f = (float)mantissa * (1.0f / 16777216.0f);
            std::memcpy(&bits, &f, 4);
            bits |= sign;
        }
    }else if (exponent == 31){
        bits = sign | 0x7f800000u | (mantissa << 13);
    }else{
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint32_t ParticleRenderPrep::ribbonStripPoints(const ParticleProgram& program){
    // with fewer live particles than two per ribbon there are count / 2 ribbons of at most 3
    const uint32_t ribbons = std::max(1u, program.trails.ribbonCount);
    return std::max((program.maxParticles + ribbons - 1) / ribbons, 3u);
}

TrailPack ParticleRenderPrep::packTrails(const EmitterRuntime& emitter, memory::Arena& arena){
    TrailPack pack;
    const ParticleProgram* prog = emitter.program();
    if (!prog || !prog->trails.enabled || emitter.isPrewarming())
        return pack;
    const TrailParams& tp = prog->trails;
    const ParticleChannels& ch = emitter.particleChannels();
    const float* decode = ParticleCompiler::srgbDecodeTable();
    const uint8_t* encode = encodeTable();
    auto encodeColor = [&](const float c[4]){
        uint32_t v = 0;
        for (int k = 0; k < 3; k++){
            const float x = std::clamp(c[k], 0.0f, 1.0f);
            v |= (uint32_t)encode[(uint32_t)(std::sqrt(x) * 4095.0f)] << (k * 8);
        }
        return v | ((uint32_t)(std::clamp(c[3], 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
    };
    ScalarDriver d;
    d.seed = emitter.seedValue();
    const float now = (float)emitter.time();

    // a strip of points, newest first, written as pairs of vertices; unused points repeat
    // the last pair so their quads have no area. Width and colour are worked out per point only
    // when they vary along the trail.
    const bool constantWidth = tp.width.kind == CurveParam::Const;
    const bool constantTrailColor = tp.colorOverTrail.kind == GradientParam::Const;
    const bool constantLifetimeColor = tp.colorOverLifetime.kind == GradientParam::Const;
    auto writeStrip = [&](TrailVertex* out, uint32_t points, uint32_t capacity, const float* xs, const float* ys, const float* zs,
                          const float* ages, uint32_t baseColor, float baseWidth, float lifetime){
        float base[4] = {1, 1, 1, 1};
        if (tp.inheritParticleColor){
            base[0] = decode[baseColor & 255u]; base[1] = decode[(baseColor >> 8) & 255u];
            base[2] = decode[(baseColor >> 16) & 255u]; base[3] = (float)(baseColor >> 24) / 255.0f;
        }
        float m[4];
        // the parts of the colour that do not vary, multiplied in once
        float fixedColor[4] = {base[0], base[1], base[2], base[3]};
        if (constantTrailColor){
            gradientScalar(*prog, tp.colorOverTrail, d, m);
            for (int c = 0; c < 4; c++) fixedColor[c] *= m[c];
        }
        if (constantLifetimeColor){
            gradientScalar(*prog, tp.colorOverLifetime, d, m);
            for (int c = 0; c < 4; c++) fixedColor[c] *= m[c];
        }
        const uint32_t fixedPacked = encodeColor(fixedColor);
        const uint16_t fixedHalfWidth = toHalf(baseWidth * curveScalar(*prog, tp.width, d) * 0.5f);
        float length = 0.0f;
        const uint32_t used = std::min(points, capacity);
        for (uint32_t k = 0; k < used; k++){
            const uint32_t p = k;
            const float u = points > 1 ? (float)p / (float)(points - 1) : 0.0f;
            // the tangent: the neighbours' difference
            const uint32_t a = p > 0 ? p - 1 : p, b = p + 1 < points ? p + 1 : p;
            if (k > 0){
                const float dx = xs[p] - xs[p - 1], dy = ys[p] - ys[p - 1], dz = zs[p] - zs[p - 1];
                length += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            uint16_t halfWidth = fixedHalfWidth;
            if (!constantWidth){
                d.normAge = u;
                halfWidth = toHalf(baseWidth * curveScalar(*prog, tp.width, d) * 0.5f);
            }
            uint32_t packed = fixedPacked;
            if (!constantTrailColor || !constantLifetimeColor){
                float color[4] = {fixedColor[0], fixedColor[1], fixedColor[2], fixedColor[3]};
                if (!constantTrailColor){
                    d.normAge = u;
                    gradientScalar(*prog, tp.colorOverTrail, d, m);
                    for (int c = 0; c < 4; c++) color[c] *= m[c];
                }
                if (!constantLifetimeColor){
                    d.normAge = lifetime > 0.0f ? std::clamp(ages[p] / lifetime, 0.0f, 1.0f) : 0.0f;
                    gradientScalar(*prog, tp.colorOverLifetime, d, m);
                    for (int c = 0; c < 4; c++) color[c] *= m[c];
                }
                packed = encodeColor(color);
            }
            float texU = u;
            if (tp.textureMode == TrailTextureMode::Tile) texU = baseWidth > 0.0f ? length / (baseWidth * 2.0f) : u;
            else if (tp.textureMode == TrailTextureMode::DistributePerSegment) texU = (float)p / (float)std::max(capacity - 1, 1u);
            // Tile u passes 1: it is stored over kTrailTileRange tiles (USHORT2N holds 0..1)
            // and the shader scales it back, so the texture repeats along the trail
            const float stored = tp.textureMode == TrailTextureMode::Tile ? texU / kTrailTileRange : texU;
            TrailVertex v;
            v.center[0] = xs[p]; v.center[1] = ys[p]; v.center[2] = zs[p];
            v.tangentWidth[0] = toHalf(xs[a] - xs[b]);
            v.tangentWidth[1] = toHalf(ys[a] - ys[b]);
            v.tangentWidth[2] = toHalf(zs[a] - zs[b]);
            v.tangentWidth[3] = halfWidth;
            v.color = packed;
            v.uvSide[0] = (uint16_t)(std::clamp(stored, 0.0f, 1.0f) * 65535.0f + 0.5f);
            v.uvSide[1] = 0;
            out[k * 2] = v;
            v.uvSide[1] = 65535;
            out[k * 2 + 1] = v;
        }
        // the unused points: the last pair again
        for (uint32_t k = used; k < capacity; k++){
            out[k * 2] = out[(used - 1) * 2];
            out[k * 2 + 1] = out[(used - 1) * 2 + 1];
        }
    };

    if (tp.mode == TrailMode::PerParticle){
        const ParticleTrails& trails = emitter.trails();
        const uint32_t rings = trails.ringCount();
        uint32_t active = 0;
        for (uint32_t r = 0; r < rings; r++){
            if (trails.active(r) && trails.live(r) >= 1) active++;
        }
        if (active == 0)
            return pack;
        const uint32_t capacity = trails.pointsPerRing() + 1;   // the particle itself heads the trail
        pack.pointsPerStrip = capacity;
        pack.strips = active;
        pack.worldSpace = tp.worldSpace && prog->space == SimulationSpace::Local;
        pack.vertices = static_cast<TrailVertex*>(arena.allocate(sizeof(TrailVertex) * active * capacity * 2, 16));
        // the live particles' interpolated positions head their trails
        const uint32_t* slots = ch.getU(ChTrailSlot);
        float* headX = arena.allocateArrayForOverwrite<float>(rings);
        float* headY = arena.allocateArrayForOverwrite<float>(rings);
        float* headZ = arena.allocateArrayForOverwrite<float>(rings);
        uint8_t* hasHead = arena.allocateArray<uint8_t>(rings);
        const float alpha = emitter.interpolation();
        for (uint32_t i = 0; i < ch.count; i++){
            if (slots[i] == kNoTrail) continue;
            float p[3] = {ch.get(ChPrevX)[i] + (ch.get(ChPosX)[i] - ch.get(ChPrevX)[i]) * alpha,
                          ch.get(ChPrevY)[i] + (ch.get(ChPosY)[i] - ch.get(ChPrevY)[i]) * alpha,
                          ch.get(ChPrevZ)[i] + (ch.get(ChPosZ)[i] - ch.get(ChPrevZ)[i]) * alpha};
            if (pack.worldSpace){
                const float* m = emitter.simToWorld();
                const float s[3] = {p[0], p[1], p[2]};
                p[0] = m[0] * s[0] + m[4] * s[1] + m[8] * s[2] + m[12];
                p[1] = m[1] * s[0] + m[5] * s[1] + m[9] * s[2] + m[13];
                p[2] = m[2] * s[0] + m[6] * s[1] + m[10] * s[2] + m[14];
            }
            headX[slots[i]] = p[0]; headY[slots[i]] = p[1]; headZ[slots[i]] = p[2];
            hasHead[slots[i]] = 1;
        }
        float xs[kMaxTrailPoints + 1], ys[kMaxTrailPoints + 1], zs[kMaxTrailPoints + 1], ages[kMaxTrailPoints + 1];
        uint32_t strip = 0;
        for (uint32_t r = 0; r < rings; r++){
            if (!trails.active(r) || trails.live(r) < 1) continue;
            uint32_t n = 0;
            if (hasHead[r]){
                xs[0] = headX[r]; ys[0] = headY[r]; zs[0] = headZ[r]; ages[0] = 0.0f;
                n = 1;
            }
            for (uint32_t k = 0; k < trails.live(r); k++){
                float t;
                trails.point(r, k, xs[n], ys[n], zs[n], t);
                ages[n] = std::max(0.0f, now - t);
                n++;
            }
            writeStrip(pack.vertices + (size_t)strip * capacity * 2, n, capacity, xs, ys, zs, ages, trails.color(r),
                       tp.sizeAffectsWidth ? trails.width(r) : 1.0f, trails.lifetime(r));
            strip++;
        }
        return pack;
    }

    // ribbons: the live particles in spawn order, dealt round-robin to the ribbons
    const uint32_t count = ch.count;
    if (count < 2)
        return pack;
    const uint32_t ribbons = std::max(1u, std::min(tp.ribbonCount, count / 2));
    uint32_t* order = arena.allocateArrayForOverwrite<uint32_t>(count);
    uint32_t* scratch = arena.allocateArrayForOverwrite<uint32_t>(count);
    const uint32_t* spawn = ch.getU(ChSpawnIndex);
    for (uint32_t i = 0; i < count; i++) order[i] = i;
    // the spawn order (spawn indices only grow, a stable radix over their low 32 bits in 4 passes)
    {
        uint16_t* low = arena.allocateArrayForOverwrite<uint16_t>(count);
        uint16_t* high = arena.allocateArrayForOverwrite<uint16_t>(count);
        for (uint32_t i = 0; i < count; i++){ low[i] = (uint16_t)(spawn[i] & 0xffffu); high[i] = (uint16_t)(spawn[i] >> 16); }
        uint16_t* keyScratch = arena.allocateArrayForOverwrite<uint16_t>(count);
        radixSort16(low, order, scratch, keyScratch, count);
        radixSort16(high, order, scratch, keyScratch, count);
    }
    // one ribbon holds them all; several have the effect's fixed stride (the renderer's
    // triangles), the points past a ribbon's own repeating its last
    const uint32_t perRibbon = ribbons == 1 ? count : ribbonStripPoints(*prog);
    pack.pointsPerStrip = perRibbon;
    pack.strips = ribbons;
    pack.worldSpace = false;
    pack.vertices = static_cast<TrailVertex*>(arena.allocate(sizeof(TrailVertex) * ribbons * perRibbon * 2, 16));
    float* xs = arena.allocateArrayForOverwrite<float>(perRibbon);
    float* ys = arena.allocateArrayForOverwrite<float>(perRibbon);
    float* zs = arena.allocateArrayForOverwrite<float>(perRibbon);
    float* ages = arena.allocateArrayForOverwrite<float>(perRibbon);
    const float alpha = emitter.interpolation();
    for (uint32_t r = 0; r < ribbons; r++){
        uint32_t n = 0;
        // newest first, as trails are
        for (uint32_t k = count; k > 0; k--){
            const uint32_t idx = order[k - 1];
            if (spawn[idx] % ribbons != r) continue;
            xs[n] = ch.get(ChPrevX)[idx] + (ch.get(ChPosX)[idx] - ch.get(ChPrevX)[idx]) * alpha;
            ys[n] = ch.get(ChPrevY)[idx] + (ch.get(ChPosY)[idx] - ch.get(ChPrevY)[idx]) * alpha;
            zs[n] = ch.get(ChPrevZ)[idx] + (ch.get(ChPosZ)[idx] - ch.get(ChPrevZ)[idx]) * alpha;
            ages[n] = ch.get(ChAge)[idx];
            if (++n == perRibbon) break;
        }
        if (n == 0){
            // an empty ribbon: one point repeated (no area)
            xs[0] = ch.get(ChPosX)[0]; ys[0] = ch.get(ChPosY)[0]; zs[0] = ch.get(ChPosZ)[0]; ages[0] = 0.0f;
            n = 1;
        }
        const uint32_t newest = order[count - 1];
        writeStrip(pack.vertices + (size_t)r * perRibbon * 2, n, perRibbon, xs, ys, zs, ages, ch.getU(ChStartColor)[newest],
                   tp.sizeAffectsWidth ? ch.get(ChStartSize)[newest] : 1.0f, 1.0f / ch.get(ChInvLife)[newest]);
    }
    return pack;
}
