// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "noise/NoiseSwitch.h"

#if JASPERENGINE_NOISE_ENABLED

#include "noise/NoiseDetMath.h"
#include "noise/NoiseKernelImpl.h"
#include "noise/NoiseParallel.h"
#include "noise/NoisePlan.h"
#include "terrain/HeightfieldErosion.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

static_assert(FLT_EVAL_METHOD == 0, "the noise plan needs float arithmetic in float precision");
static_assert(std::numeric_limits<float>::is_iec559, "the noise plan needs IEEE 754 floats");

namespace jasperengine { namespace noise {

    namespace {

        // Applies fn over lane groups of up to three input arrays; a tail is padded and never stored past n
        template<class B, class Fn>
        void mapLanes(const float* a, const float* b, const float* c, float* out, size_t n, Fn&& fn){
            constexpr size_t lanes = laneCount<B>;
            const size_t whole = n - n % lanes;
            for (size_t i = 0; i < whole; i += lanes){
                const typename B::f4 va = a ? B::loadu(a + i) : B::zero();
                const typename B::f4 vb = b ? B::loadu(b + i) : B::zero();
                const typename B::f4 vc = c ? B::loadu(c + i) : B::zero();
                B::storeu(out + i, fn(va, vb, vc));
            }
            if (whole < n){
                float ta[4] = {0.0f, 0.0f, 0.0f, 0.0f}, tb[4] = {0.0f, 0.0f, 0.0f, 0.0f}, tc[4] = {0.0f, 0.0f, 0.0f, 0.0f}, to[4];
                const size_t rest = n - whole;
                for (size_t i = 0; i < rest; i++){
                    if (a) ta[i] = a[whole + i];
                    if (b) tb[i] = b[whole + i];
                    if (c) tc[i] = c[whole + i];
                }
                B::storeu(to, fn(B::loadu(ta), B::loadu(tb), B::loadu(tc)));
                for (size_t i = 0; i < rest; i++) out[whole + i] = to[i];
            }
        }

        template<class B>
        struct PointOps {
            typedef typename B::f4 f4;
            typedef typename B::u4 u4;

            static f4 clamp01(f4 v){
                return B::min(B::max(v, B::zero()), B::set1(1.0f));
            }

            static f4 smoothstep(f4 lo, f4 hi, f4 v){
                const f4 t = clamp01(B::div(B::sub(v, lo), B::sub(hi, lo)));
                return B::mul(B::mul(t, t), B::sub(B::set1(3.0f), B::mul(B::set1(2.0f), t)));
            }

            static f4 stepUp(float edge, f4 v){
                return B::select(B::cmpge(v, B::set1(edge)), B::set1(1.0f), B::zero());
            }

            static f4 sanitized(f4 v){
                return B::select(B::cmplt(B::abs(v), B::set1(HUGE_VALF)), v, B::zero());
            }

            static f4 mathOp(MathKind kind, f4 a, f4 b){
                switch (kind){
                    case MathKind::Add: return B::add(a, b);
                    case MathKind::Subtract: return B::sub(a, b);
                    case MathKind::Multiply: return B::mul(a, b);
                    case MathKind::Divide: return B::select(B::cmpeq(b, B::zero()), B::zero(), B::div(a, b));
                    case MathKind::Min: return B::min(a, b);
                    case MathKind::Max: return B::max(a, b);
                    case MathKind::Power: return detmath::DetMath<B>::pow(a, b);
                    case MathKind::Modulo: {
                        // Clamped: a huge quotient rounds, and the result must stay between 0 and b (R19)
                        const f4 r = B::sub(a, B::mul(B::floor(B::div(a, b)), b));
                        const f4 clamped = B::min(B::max(r, B::min(B::zero(), b)), B::max(B::zero(), b));
                        return B::select(B::cmpeq(b, B::zero()), B::zero(), clamped);
                    }
                    case MathKind::Abs: return B::abs(a);
                    case MathKind::Negate: return B::neg(a);
                    case MathKind::OneMinus: return B::sub(B::set1(1.0f), a);
                    case MathKind::Sqrt: return B::select(B::cmplt(a, B::zero()), B::zero(), B::sqrt(a));
                    case MathKind::Floor: return B::floor(a);
                    case MathKind::Frac: return B::sub(a, B::floor(a));
                    case MathKind::Sin: return detmath::DetMath<B>::sin(a);
                    default: return detmath::DetMath<B>::cos(a);
                }
            }

            static void warp(const PlanOp& op, const BasisParams& base, float* const* buf, size_t n){
                constexpr size_t lanes = laneCount<B>;
                const float* x = buf[op.in[0]];
                const float* y = buf[op.in[1]];
                const float* z = buf[op.in[2]];
                const float* strength = op.in[3] >= 0 ? buf[op.in[3]] : nullptr;
                const bool threeD = op.i[0] == 3;
                FractalParams fx = op.fractal, fy = op.fractal, fz = op.fractal;
                fx.basis.seed = base.seed;
                fz.basis.seed = base.seed + 1u;
                fy.basis.seed = base.seed + 2u;
                const f4 amplitude = B::set1(op.f[0]);
                const auto group = [&](f4 px, f4 py, f4 pz, f4 s, f4& ox, f4& oy, f4& oz){
                    const f4 reach = strength ? B::mul(amplitude, s) : amplitude;
                    ox = B::add(px, B::mul(NoiseKernel<B>::fbm(fx, px, py, pz), reach));
                    oz = B::add(pz, B::mul(NoiseKernel<B>::fbm(fz, px, py, pz), reach));
                    if (threeD) oy = B::add(py, B::mul(NoiseKernel<B>::fbm(fy, px, py, pz), reach));
                };
                const size_t whole = n - n % lanes;
                for (size_t i = 0; i < whole; i += lanes){
                    f4 ox, oy = B::zero(), oz;
                    group(B::loadu(x + i), B::loadu(y + i), B::loadu(z + i), strength ? B::loadu(strength + i) : B::zero(), ox, oy, oz);
                    B::storeu(buf[op.out[0]] + i, ox);
                    B::storeu(buf[op.out[2]] + i, oz);
                    if (threeD) B::storeu(buf[op.out[1]] + i, oy);
                }
                if (whole < n){
                    float tx[4] = {0.0f, 0.0f, 0.0f, 0.0f}, ty[4] = {0.0f, 0.0f, 0.0f, 0.0f}, tz[4] = {0.0f, 0.0f, 0.0f, 0.0f}, ts[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    const size_t rest = n - whole;
                    for (size_t i = 0; i < rest; i++){
                        tx[i] = x[whole + i];
                        ty[i] = y[whole + i];
                        tz[i] = z[whole + i];
                        if (strength) ts[i] = strength[whole + i];
                    }
                    f4 ox, oy = B::zero(), oz;
                    group(B::loadu(tx), B::loadu(ty), B::loadu(tz), B::loadu(ts), ox, oy, oz);
                    float rx[4], ry[4], rz[4];
                    B::storeu(rx, ox);
                    B::storeu(ry, oy);
                    B::storeu(rz, oz);
                    for (size_t i = 0; i < rest; i++){
                        buf[op.out[0]][whole + i] = rx[i];
                        buf[op.out[2]][whole + i] = rz[i];
                        if (threeD) buf[op.out[1]][whole + i] = ry[i];
                    }
                }
            }

            static void transform(const PlanOp& op, float* const* buf, size_t n){
                constexpr size_t lanes = laneCount<B>;
                const f4 sx = B::set1(op.f[0]), sy = B::set1(op.f[1]), sz = B::set1(op.f[2]);
                const f4 ox = B::set1(op.f[3]), oy = B::set1(op.f[4]), oz = B::set1(op.f[5]);
                const f4 c = B::set1(op.f[6]), s = B::set1(op.f[7]);
                const auto group = [&](f4 px, f4 py, f4 pz, f4& rx, f4& ry, f4& rz){
                    const f4 x = B::mul(px, sx), z = B::mul(pz, sz);
                    rx = B::add(B::add(B::mul(x, c), B::mul(z, s)), ox);
                    ry = B::add(B::mul(py, sy), oy);
                    rz = B::add(B::sub(B::mul(z, c), B::mul(x, s)), oz);
                };
                const float* x = buf[op.in[0]];
                const float* y = buf[op.in[1]];
                const float* z = buf[op.in[2]];
                const size_t whole = n - n % lanes;
                for (size_t i = 0; i < whole; i += lanes){
                    f4 rx, ry, rz;
                    group(B::loadu(x + i), B::loadu(y + i), B::loadu(z + i), rx, ry, rz);
                    B::storeu(buf[op.out[0]] + i, rx);
                    B::storeu(buf[op.out[1]] + i, ry);
                    B::storeu(buf[op.out[2]] + i, rz);
                }
                if (whole < n){
                    float tx[4] = {0.0f, 0.0f, 0.0f, 0.0f}, ty[4] = {0.0f, 0.0f, 0.0f, 0.0f}, tz[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    const size_t rest = n - whole;
                    for (size_t i = 0; i < rest; i++){ tx[i] = x[whole + i]; ty[i] = y[whole + i]; tz[i] = z[whole + i]; }
                    f4 rx, ry, rz;
                    group(B::loadu(tx), B::loadu(ty), B::loadu(tz), rx, ry, rz);
                    float ox4[4], oy4[4], oz4[4];
                    B::storeu(ox4, rx);
                    B::storeu(oy4, ry);
                    B::storeu(oz4, rz);
                    for (size_t i = 0; i < rest; i++){
                        buf[op.out[0]][whole + i] = ox4[i];
                        buf[op.out[1]][whole + i] = oy4[i];
                        buf[op.out[2]][whole + i] = oz4[i];
                    }
                }
            }

            static void curve(const PlanOp& op, const PlanData& plan, float* const* buf, size_t n){
                constexpr size_t lanes = laneCount<B>;
                const float* lut = plan.luts[static_cast<size_t>(op.lut)].data();
                const f4 t0 = B::set1(op.f[0]), scale = B::set1(op.f[1]);
                mapLanes<B>(buf[op.in[0]], nullptr, nullptr, buf[op.out[0]], n, [&](f4 v, f4, f4){
                    const f4 place = B::min(B::max(B::mul(B::sub(v, t0), scale), B::zero()), B::set1(255.0f));
                    const u4 index = B::cvtt(place);
                    const f4 t = B::sub(place, B::cvt(index));
                    uint32_t cells[4];
                    B::storeu4(cells, index);
                    float low[4], high[4];
                    for (size_t l = 0; l < lanes; l++){
                        low[l] = lut[cells[l]];
                        high[l] = lut[cells[l] < 255u ? cells[l] + 1u : 255u];
                    }
                    const f4 a = B::loadu(low);
                    return B::add(a, B::mul(B::sub(B::loadu(high), a), t));
                });
            }

            // One op over n points of this chunk's buffers
            static void run(const PlanOp& op, const PlanData& plan, float* const* buf, size_t n, const EvalContext& context){
                switch (op.kind){
                    case OpKind::Fill:
                        std::fill(buf[op.out[0]], buf[op.out[0]] + n, op.f[0]);
                        break;
                    case OpKind::Variable: {
                        const PlanVariable& variable = plan.variables[static_cast<size_t>(op.i[0])];
                        float value = variable.value;
                        for (const auto& [name, set] : context.variables){
                            if (name == variable.name) value = set;
                        }
                        value = std::isfinite(value) ? value : 0.0f;
                        value = value < variable.min ? variable.min : (value > variable.max ? variable.max : value);
                        std::fill(buf[op.out[0]], buf[op.out[0]] + n, value);
                        return;
                    }
                    case OpKind::PositionField:
                        std::memcpy(buf[op.out[0]], buf[op.in[0]], n * sizeof(float));
                        break;
                    case OpKind::Noise: {
                        BasisParams params = op.fractal.basis;
                        params.seed = context.seed + op.seedOffset;
                        forEachLaneGroup<B>(buf[op.in[0]], buf[op.in[1]], buf[op.in[2]], n, buf[op.out[0]], [&params](f4 x, f4 y, f4 z){
                            return NoiseKernel<B>::basis(params, x, y, z);
                        });
                        break;
                    }
                    case OpKind::Fbm: {
                        FractalParams params = op.fractal;
                        params.basis.seed = context.seed + op.seedOffset;
                        forEachLaneGroup<B>(buf[op.in[0]], buf[op.in[1]], buf[op.in[2]], n, buf[op.out[0]], [&params](f4 x, f4 y, f4 z){
                            return NoiseKernel<B>::fbm(params, x, y, z);
                        });
                        break;
                    }
                    case OpKind::Ridged: {
                        FractalParams params = op.fractal;
                        params.basis.seed = context.seed + op.seedOffset;
                        forEachLaneGroup<B>(buf[op.in[0]], buf[op.in[1]], buf[op.in[2]], n, buf[op.out[0]], [&params](f4 x, f4 y, f4 z){
                            return NoiseKernel<B>::ridged(params, x, y, z);
                        });
                        break;
                    }
                    case OpKind::DomainWarp: {
                        BasisParams base = op.fractal.basis;
                        base.seed = context.seed + op.seedOffset;
                        warp(op, base, buf, n);
                        break;
                    }
                    case OpKind::DomainTransform:
                        transform(op, buf, n);
                        break;
                    case OpKind::ScaleOffset: {
                        const f4 scale = B::set1(op.f[0]), offset = B::set1(op.f[1]);
                        mapLanes<B>(buf[op.in[0]], nullptr, nullptr, buf[op.out[0]], n, [&](f4 v, f4, f4){ return B::add(B::mul(v, scale), offset); });
                        break;
                    }
                    case OpKind::MathOp: {
                        const MathKind kind = op.math;
                        mapLanes<B>(buf[op.in[0]], op.in[1] >= 0 ? buf[op.in[1]] : nullptr, nullptr, buf[op.out[0]], n, [kind](f4 a, f4 b, f4){ return mathOp(kind, a, b); });
                        break;
                    }
                    case OpKind::Clamp: {
                        const f4 lo = B::set1(op.f[0]), hi = B::set1(op.f[1]);
                        mapLanes<B>(buf[op.in[0]], nullptr, nullptr, buf[op.out[0]], n, [&](f4 v, f4, f4){ return B::min(B::max(v, lo), hi); });
                        break;
                    }
                    case OpKind::Remap: {
                        const f4 fromMin = B::set1(op.f[0]), inverse = B::set1(op.f[1]), toMin = B::set1(op.f[2]), toSpan = B::set1(op.f[3]);
                        const bool clamped = op.i[0] != 0;
                        mapLanes<B>(buf[op.in[0]], nullptr, nullptr, buf[op.out[0]], n, [&](f4 v, f4, f4){
                            f4 t = B::mul(B::sub(v, fromMin), inverse);
                            if (clamped) t = clamp01(t);
                            return B::add(B::mul(t, toSpan), toMin);
                        });
                        break;
                    }
                    case OpKind::Curve:
                        curve(op, plan, buf, n);
                        break;
                    case OpKind::Terrace: {
                        const f4 steps = B::set1(op.f[0]);
                        const float smoothness = op.f[1];
                        mapLanes<B>(buf[op.in[0]], nullptr, nullptr, buf[op.out[0]], n, [&](f4 v, f4, f4){
                            const f4 x = B::mul(v, steps);
                            const f4 whole = B::floor(x);
                            f4 rise = B::zero();
                            if (smoothness > 0.0f){
                                const f4 t = clamp01(B::div(B::sub(B::sub(x, whole), B::set1(1.0f - smoothness)), B::set1(smoothness)));
                                rise = B::mul(B::mul(t, t), B::sub(B::set1(3.0f), B::mul(B::set1(2.0f), t)));
                            }
                            return B::div(B::add(whole, rise), steps);
                        });
                        break;
                    }
                    case OpKind::Select: {
                        const float threshold = op.f[0], falloff = op.f[1];
                        mapLanes<B>(buf[op.in[0]], buf[op.in[1]], buf[op.in[2]], buf[op.out[0]], n, [&](f4 a, f4 b, f4 mask){
                            const f4 t = falloff > 0.0f ? smoothstep(B::set1(threshold - falloff), B::set1(threshold + falloff), mask) : stepUp(threshold, mask);
                            return B::add(a, B::mul(B::sub(b, a), t));
                        });
                        break;
                    }
                    case OpKind::Blend:
                        mapLanes<B>(buf[op.in[0]], buf[op.in[1]], buf[op.in[2]], buf[op.out[0]], n, [](f4 a, f4 b, f4 weight){
                            return B::add(a, B::mul(B::sub(b, a), clamp01(weight)));
                        });
                        break;
                    case OpKind::HeightMask: {
                        const float lo = op.f[0], hi = op.f[1], falloff = op.f[2];
                        mapLanes<B>(buf[op.in[0]], nullptr, nullptr, buf[op.out[0]], n, [&](f4 v, f4, f4){
                            if (falloff > 0.0f){
                                const f4 lower = smoothstep(B::set1(lo - falloff), B::set1(lo), v);
                                const f4 upper = B::sub(B::set1(1.0f), smoothstep(B::set1(hi), B::set1(hi + falloff), v));
                                return B::mul(lower, upper);
                            }
                            return B::mul(stepUp(lo, v), B::select(B::cmple(v, B::set1(hi)), B::set1(1.0f), B::zero()));
                        });
                        break;
                    }
                    default:
                        return;
                }
                if (op.sanitize){
                    for (int k = 0; k < 3; k++){
                        if (op.out[k] < 0) continue;
                        float* target = buf[op.out[k]];
                        mapLanes<B>(target, nullptr, nullptr, target, n, [](f4 v, f4, f4){ return sanitized(v); });
                    }
                }
            }
        };

        // What every chunk of one evaluation shares
        struct Run {
            const PlanData& plan;
            const EvalContext& context;
            std::span<float* const> outputs;    // by output index; null when not asked for
            std::vector<float*>& grids;
            // the region's shape: a Grid2 with its padding, or a Grid3
            bool threeD = false;
            GridDomain region;
            int pad = 0;
            Grid3Domain volume;
            std::atomic<uint64_t>* opNs = nullptr;
        };

        uint64_t steadyNs(){
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        void fillGrid2Domain(const Run& run, size_t start, size_t n, float* const* buf){
            const GridDomain& domain = run.region;
            const size_t width = static_cast<size_t>(domain.width);
            const double units = domain.unitsPerTexel;
            const float y = static_cast<float>(domain.y);
            float* outX = buf[kBufX];
            float* outY = buf[kBufY];
            float* outZ = buf[kBufZ];
            if (run.plan.readsPosition){
                for (size_t k = 0; k < n; k++){
                    const size_t index = start + k;
                    const int64_t i = static_cast<int64_t>(index % width);
                    const int64_t j = static_cast<int64_t>(index / width);
                    outX[k] = static_cast<float>((static_cast<double>(domain.startX + i) + 0.5) * units);
                    outY[k] = y;
                    outZ[k] = static_cast<float>((static_cast<double>(domain.startZ + j) + 0.5) * units);
                }
            }
            if (run.plan.readsUv){
                const double perUnit = run.context.uvPerUnit;
                const double inner = static_cast<double>(domain.width - 2 * run.pad);
                const double innerDepth = static_cast<double>(domain.depth - 2 * run.pad);
                for (size_t k = 0; k < n; k++){
                    const size_t index = start + k;
                    const int64_t i = static_cast<int64_t>(index % width);
                    const int64_t j = static_cast<int64_t>(index / width);
                    float u, v;
                    if (perUnit > 0.0){
                        u = static_cast<float>((static_cast<double>(domain.startX + i) + 0.5) * units * perUnit);
                        v = static_cast<float>((static_cast<double>(domain.startZ + j) + 0.5) * units * perUnit);
                    } else {
                        u = static_cast<float>((static_cast<double>(i - run.pad) + 0.5) / inner);
                        v = static_cast<float>((static_cast<double>(j - run.pad) + 0.5) / innerDepth);
                    }
                    buf[kBufU][k] = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
                    buf[kBufV][k] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                }
            }
        }

        void fillGrid3Domain(const Run& run, size_t start, size_t n, float* const* buf){
            const Grid3Domain& domain = run.volume;
            const size_t width = static_cast<size_t>(domain.width);
            const size_t height = static_cast<size_t>(domain.height);
            const double units = domain.unitsPerVoxel;
            for (size_t k = 0; k < n; k++){
                const size_t index = start + k;
                const int64_t i = static_cast<int64_t>(index % width);
                const int64_t j = static_cast<int64_t>((index / width) % height);
                const int64_t l = static_cast<int64_t>(index / (width * height));
                if (run.plan.readsPosition){
                    buf[kBufX][k] = static_cast<float>(static_cast<double>(domain.startX + i) * units);
                    buf[kBufY][k] = static_cast<float>(static_cast<double>(domain.startY + j) * units);
                    buf[kBufZ][k] = static_cast<float>(static_cast<double>(domain.startZ + l) * units);
                }
                if (run.plan.readsUv){
                    buf[kBufU][k] = domain.width > 1 ? static_cast<float>(static_cast<double>(i) / static_cast<double>(domain.width - 1)) : 0.0f;
                    buf[kBufV][k] = domain.depth > 1 ? static_cast<float>(static_cast<double>(l) / static_cast<double>(domain.depth - 1)) : 0.0f;
                }
            }
        }

        void readTerrainHeight(const Run& run, size_t start, size_t n, float* out){
            const EvalContext& context = run.context;
            if (run.threeD || !context.terrainHeight || context.terrainHeightWidth <= 0 || context.terrainHeightDepth <= 0){
                std::fill(out, out + n, 0.0f);
                return;
            }
            const size_t width = static_cast<size_t>(run.region.width);
            for (size_t k = 0; k < n; k++){
                const size_t index = start + k;
                const int64_t x = run.region.startX + static_cast<int64_t>(index % width) - context.terrainHeightStartX;
                const int64_t z = run.region.startZ + static_cast<int64_t>(index / width) - context.terrainHeightStartZ;
                const int64_t cx = x < 0 ? 0 : (x >= context.terrainHeightWidth ? context.terrainHeightWidth - 1 : x);
                const int64_t cz = z < 0 ? 0 : (z >= context.terrainHeightDepth ? context.terrainHeightDepth - 1 : z);
                out[k] = context.terrainHeight[cz * context.terrainHeightWidth + cx];
            }
        }

        // The buffers of a chunk: the domain arrays and the slots in the scratch, the grids at this chunk's offset
        void chunkBuffers(const PlanData& plan, const std::vector<float*>& grids, float* scratch, size_t start, float** buf){
            for (int32_t i = 0; i < plan.firstGrid; i++) buf[i] = scratch + static_cast<size_t>(i) * kChunkPoints;
            for (int g = 0; g < plan.gridCount; g++){
                buf[plan.firstGrid + g] = grids[static_cast<size_t>(g)] ? grids[static_cast<size_t>(g)] + start : nullptr;
            }
        }

        template<class B>
        void runChunk(const Run& run, const uint32_t* ops, size_t opCount, size_t start, size_t n, float** buf){
            const bool profiling = run.opNs != nullptr;
            const auto now = run.context.clock ? run.context.clock : &steadyNs;
            for (size_t o = 0; o < opCount; o++){
                const uint32_t index = ops[o];
                const PlanOp& op = run.plan.ops[index];
                const uint64_t before = profiling ? now() : 0;
                if (op.kind == OpKind::TerrainHeight){
                    readTerrainHeight(run, start, n, buf[op.out[0]]);
                } else if (op.kind == OpKind::Output){
                    float* target = run.outputs[static_cast<size_t>(op.i[0])];
                    if (target){
                        const float* from = buf[op.in[0]];
                        if (op.f[0] > 0.0f){
                            // Normalized from metres at the evaluation's height (R9); a height of 0 keeps its values
                            const float top = run.context.maxHeight > 0.0f ? run.context.maxHeight : 1.0f;
                            for (size_t k = 0; k < n; k++) target[start + k] = from[k] / top;
                        } else {
                            std::memcpy(target + start, from, n * sizeof(float));
                        }
                    }
                } else {
                    PointOps<B>::run(op, run.plan, buf, n, run.context);
                }
                if (profiling) run.opNs[index].fetch_add(now() - before, std::memory_order_relaxed);
            }
        }

        // Per-worker scratches, claimed without a lock (R7): at most this many bodies run at once in
        // practice; past it a body makes its own
        constexpr int kScratchPool = 32;

        struct ScratchPool {
            std::atomic<uint32_t> claimed{0};
            std::unique_ptr<float[]> scratches[kScratchPool];
            std::unique_ptr<float*[]> tables[kScratchPool];
            size_t floats = 0;
            size_t tableSize = 0;

            ScratchPool(size_t bytes, size_t buffers): floats(bytes / sizeof(float)), tableSize(buffers) {}

            int claim(){
                uint32_t mask = claimed.load(std::memory_order_relaxed);
                for (;;){
                    int slot = -1;
                    for (int i = 0; i < kScratchPool; i++){
                        if (!(mask & (1u << i))){ slot = i; break; }
                    }
                    if (slot < 0) return -1;
                    if (claimed.compare_exchange_weak(mask, mask | (1u << slot), std::memory_order_acquire, std::memory_order_relaxed)){
                        if (!scratches[slot]){
                            scratches[slot].reset(new float[floats]);
                            tables[slot].reset(new float*[tableSize]);
                        }
                        return slot;
                    }
                }
            }

            void release(int slot){
                claimed.fetch_and(~(1u << slot), std::memory_order_release);
            }
        };

        void runSlopeMask(const PlanOp& op, const Run& run, const float* height, float* out){
            const int width = run.region.width;
            const int depth = run.region.depth;
            const EvalContext& context = run.context;
            const float scale = context.metresPerTexel > 0.0 ? context.maxHeight * 0.5f / static_cast<float>(context.metresPerTexel) : 0.0f;
            const float t0 = op.f[0], t1 = op.f[1], t2 = op.f[2], t3 = op.f[3];
            const bool openLow = op.i[0] != 0, openHigh = op.i[1] != 0;
            detail::parallelChunks(static_cast<size_t>(depth), context.parallel && !context.scratch, context.cancel, [&](size_t row){
                const int z = static_cast<int>(row);
                const float* here = height + static_cast<size_t>(z) * static_cast<size_t>(width);
                const float* up = z > 0 ? here - width : here;
                const float* down = z + 1 < depth ? here + width : here;
                float* result = out + static_cast<size_t>(z) * static_cast<size_t>(width);
                const auto smooth = [](float a, float b, float v){
                    if (b <= a) return v >= b ? 1.0f : 0.0f;
                    float t = (v - a) / (b - a);
                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                    return t * t * (3.0f - 2.0f * t);
                };
                for (int x = 0; x < width; x++){
                    const float gx = (here[x + 1 < width ? x + 1 : x] - here[x > 0 ? x - 1 : x]) * scale;
                    const float gz = (down[x] - up[x]) * scale;
                    const float gradient = std::sqrt(gx * gx + gz * gz);
                    const float lower = openLow ? 1.0f : smooth(t0, t1, gradient);
                    const float upper = openHigh ? 1.0f : 1.0f - smooth(t2, t3, gradient);
                    result[x] = lower * upper;
                }
            });
        }

        bool runStage(const PlanOp& op, const Run& run, float* const* grids, EvalReport& report){
            const auto gridOf = [&](int32_t buffer) { return grids[buffer - run.plan.firstGrid]; };
            const size_t texels = static_cast<size_t>(run.region.width) * static_cast<size_t>(run.region.depth);
            const EvalContext& context = run.context;
            terraincore::ErosionRegion region;
            region.startX = run.region.startX;
            region.startZ = run.region.startZ;
            region.width = run.region.width;
            region.depth = run.region.depth;
            region.maxHeight = context.maxHeight;
            region.metresPerTexel = context.metresPerTexel;
            terraincore::ErosionControl control;
            control.cancel = context.cancel;
            control.parallel = context.parallel && !context.scratch;
            if (op.kind == OpKind::SlopeMask){
                runSlopeMask(op, run, gridOf(op.in[0]), gridOf(op.out[0]));
                return !detail::cancelled(context.cancel);
            }
            const uint64_t before = context.report ? steadyNs() : 0;
            bool finished = true;
            if (op.kind == OpKind::Hydraulic){
                terraincore::HydraulicParams params;
                params.dropletsPerTexel = op.f[0];
                params.inertia = op.f[1];
                params.capacity = op.f[2];
                params.minSlope = op.f[3];
                params.erodeRate = op.f[4];
                params.depositRate = op.f[5];
                params.evaporation = op.f[6];
                params.gravity = op.f[7];
                params.lifetime = op.i[0];
                params.radius = op.i[1];
                params.seed = context.seed + op.seedOffset;
                float* height = gridOf(op.out[0]);
                std::memcpy(height, gridOf(op.in[0]), texels * sizeof(float));
                uint64_t steps = 0;
                finished = terraincore::HeightfieldErosion::hydraulic(params, region, height, op.in[1] >= 0 ? gridOf(op.in[1]) : nullptr,
                                                                  op.out[1] >= 0 ? gridOf(op.out[1]) : nullptr, op.out[2] >= 0 ? gridOf(op.out[2]) : nullptr, control, &steps);
                report.dropletSteps += steps;
                for (size_t i = 0; i < texels; i++){
                    height[i] = height[i] < op.f[8] ? op.f[8] : (height[i] > op.f[9] ? op.f[9] : height[i]);
                }
            } else {
                terraincore::ThermalParams params;
                params.iterations = op.i[0];
                params.talusAngle = op.f[0];
                params.rate = op.f[1];
                float* height = gridOf(op.out[0]);
                std::memcpy(height, gridOf(op.in[0]), texels * sizeof(float));
                finished = terraincore::HeightfieldErosion::thermal(params, region, height, control);
            }
            if (context.report) report.erosionMs += static_cast<double>(steadyNs() - before) / 1e6;
            return finished;
        }

        void finishProfile(const Run& run, size_t points, std::atomic<uint64_t>* opNs){
            NodeProfile& profile = *run.context.profile;
            const PlanData& plan = run.plan;
            std::vector<uint64_t> perNode(plan.nodes.size(), 0);
            for (size_t i = 0; i < plan.ops.size(); i++) perNode[static_cast<size_t>(plan.ops[i].node)] += opNs[i].load(std::memory_order_relaxed);
            uint64_t total = 0;
            for (uint64_t ns : perNode) total += ns;
            profile.nodes.clear();
            profile.totalMs = static_cast<double>(total) / 1e6;
            profile.points = static_cast<uint64_t>(points);
            for (size_t i = 0; i < perNode.size(); i++){
                if (plan.nodes[i].dead && perNode[i] == 0) continue;
                NodeProfileEntry entry;
                entry.nodeId = plan.nodes[i].id;
                entry.ms = static_cast<double>(perNode[i]) / 1e6;
                entry.nsPerPoint = points > 0 ? static_cast<double>(perNode[i]) / static_cast<double>(points) : 0.0;
                entry.share = total > 0 ? static_cast<double>(perNode[i]) / static_cast<double>(total) : 0.0;
                profile.nodes.push_back(std::move(entry));
            }
        }

        template<class Fn>
        void withBackend(Backend backend, Fn&& fn){
#if defined(JASPERENGINE_NOISE_ALL_BACKENDS)
            if constexpr (!std::is_same_v<Native, simd::Scalar>){
                if (backend == Backend::Scalar){
                    fn(simd::Scalar{});
                    return;
                }
            }
#else
            (void)backend;
#endif
            fn(Native{});
        }

        // The ops of the outputs asked for, in plan order
        std::vector<uint32_t> mergedOps(const PlanData& plan, std::span<const int> outputs){
            std::vector<char> needed(plan.ops.size(), 0);
            for (int output : outputs){
                for (uint32_t op : plan.outputs[static_cast<size_t>(output)].ops) needed[op] = 1;
            }
            std::vector<uint32_t> list;
            for (size_t i = 0; i < needed.size(); i++){
                if (needed[i]) list.push_back(static_cast<uint32_t>(i));
            }
            return list;
        }

        // Runs a list of ops over a region of points: chunks for point ops, whole-region for stages
        bool runRegion(Run& run, size_t points, const uint32_t* ops, size_t opCount, EvalReport& report, size_t& liveBytes){
            const PlanData& plan = run.plan;
            const EvalContext& context = run.context;
            const size_t chunks = (points + kChunkPoints - 1) / kChunkPoints;
            std::vector<std::unique_ptr<float[]>> gridStorage(static_cast<size_t>(plan.gridCount));
            const size_t gridBytes = points * sizeof(float);
            const auto ensureGrid = [&](int32_t buffer){
                if (buffer < plan.firstGrid) return;
                const size_t g = static_cast<size_t>(buffer - plan.firstGrid);
                if (!gridStorage[g]){
                    gridStorage[g].reset(new float[points]);
                    run.grids[g] = gridStorage[g].get();
                    liveBytes += gridBytes;
                    report.peakBytes = std::max(report.peakBytes, liveBytes);
                }
            };
            const bool ownScratch = context.scratch == nullptr;
            const size_t scratchBytes = (static_cast<size_t>(plan.slotCount) + kFirstSlot) * kChunkPoints * sizeof(float);
            std::unique_ptr<ScratchPool> pool;
            if (ownScratch && chunks > 0) pool = std::make_unique<ScratchPool>(scratchBytes, static_cast<size_t>(plan.bufferTotal));

            size_t o = 0;
            while (o < opCount){
                const int segment = plan.opSegment[ops[o]];
                size_t end = o;
                while (end < opCount && plan.opSegment[ops[end]] == segment) end++;
                for (size_t k = o; k < end; k++){
                    const PlanOp& op = plan.ops[ops[k]];
                    for (int out = 0; out < 3; out++){
                        if (op.out[out] >= 0) ensureGrid(op.out[out]);
                    }
                }
                if (plan.segments[static_cast<size_t>(segment)].stage){
                    const PlanOp& op = plan.ops[ops[o]];
                    const uint64_t before = run.opNs ? (context.clock ? context.clock() : steadyNs()) : 0;
                    const bool finished = runStage(op, run, run.grids.data(), report);
                    if (run.opNs) run.opNs[ops[o]].fetch_add((context.clock ? context.clock() : steadyNs()) - before, std::memory_order_relaxed);
                    if (!finished) return false;
                } else {
                    const uint32_t* segmentOps = ops + o;
                    const size_t segmentCount = end - o;
                    bool finished = true;
                    if (ownScratch){
                        finished = detail::parallelChunks(chunks, context.parallel, context.cancel, [&](size_t chunk){
                            const size_t start = chunk * kChunkPoints;
                            const size_t n = std::min(kChunkPoints, points - start);
                            const int slot = pool->claim();
                            std::unique_ptr<float[]> own;
                            std::unique_ptr<float*[]> ownTable;
                            if (slot < 0){
                                own.reset(new float[scratchBytes / sizeof(float)]);
                                ownTable.reset(new float*[static_cast<size_t>(plan.bufferTotal)]);
                            }
                            float* scratch = slot >= 0 ? pool->scratches[slot].get() : own.get();
                            float** buf = slot >= 0 ? pool->tables[slot].get() : ownTable.get();
                            chunkBuffers(plan, run.grids, scratch, start, buf);
                            if (run.threeD) fillGrid3Domain(run, start, n, buf);
                            else fillGrid2Domain(run, start, n, buf);
                            withBackend(context.backend, [&](auto backend){
                                using B = decltype(backend);
                                runChunk<B>(run, segmentOps, segmentCount, start, n, buf);
                            });
                            if (slot >= 0) pool->release(slot);
                        });
                    } else {
                        context.scratch->reserve(scratchBytes);
                        float** buf = context.scratch->table(static_cast<size_t>(plan.bufferTotal));
                        for (size_t chunk = 0; chunk < chunks; chunk++){
                            if (detail::cancelled(context.cancel)){ finished = false; break; }
                            const size_t start = chunk * kChunkPoints;
                            const size_t n = std::min(kChunkPoints, points - start);
                            chunkBuffers(plan, run.grids, context.scratch->data(), start, buf);
                            if (run.threeD) fillGrid3Domain(run, start, n, buf);
                            else fillGrid2Domain(run, start, n, buf);
                            withBackend(context.backend, [&](auto backend){
                                using B = decltype(backend);
                                runChunk<B>(run, segmentOps, segmentCount, start, n, buf);
                            });
                        }
                    }
                    if (!finished) return false;
                }
                // Done with what nothing later reads (R52)
                for (size_t g = 0; g < gridStorage.size(); g++){
                    if (gridStorage[g] && plan.gridLastSegment[g] <= segment){
                        gridStorage[g].reset();
                        run.grids[g] = nullptr;
                        liveBytes -= gridBytes;
                    }
                }
                o = end;
            }
            return true;
        }

    }

    float foldOp(const PlanOp& op, const PlanData& plan, const float* constants){
        // Buffers 0..3 are the inputs, 4 the output: the same code as at run time, one point
        float in[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float out = 0.0f;
        float* buf[5] = {&in[0], &in[1], &in[2], &in[3], &out};
        PlanOp local = op;
        for (int k = 0; k < 4; k++){
            if (op.inValue[k] >= 0){
                in[k] = constants[op.inValue[k]];
                local.in[k] = k;
            } else {
                local.in[k] = -1;
            }
        }
        local.out[0] = 4;
        EvalContext context;
        PointOps<Lane1>::run(local, plan, buf, 1, context);
        return std::isfinite(out) ? out : 0.0f;
    }

    CompiledGraph::CompiledGraph(std::unique_ptr<const PlanData> planData): plan(std::move(planData)) {}
    CompiledGraph::~CompiledGraph() = default;

    bool CompiledGraph::isGridOnly() const { return plan->gridOnly; }
    bool CompiledGraph::isBakeOnly() const { return plan->bakeOnly; }
    bool CompiledGraph::readsTerrainHeight() const { return plan->readsHeight; }
    const std::vector<OutputInfo>& CompiledGraph::outputs() const { return plan->outputInfos; }
    const std::vector<CompiledNodeInfo>& CompiledGraph::nodes() const { return plan->nodes; }
    size_t CompiledGraph::bufferCount() const { return static_cast<size_t>(plan->slotCount + kFirstSlot); }
    size_t CompiledGraph::opCount() const { return plan->ops.size(); }
    size_t CompiledGraph::scratchBytes() const { return static_cast<size_t>(plan->bufferTotal) * kChunkPoints * sizeof(float); }

    int CompiledGraph::outputIndex(std::string_view name) const {
        for (size_t i = 0; i < plan->outputs.size(); i++){
            if (plan->outputs[i].info.name == name) return static_cast<int>(i);
        }
        return -1;
    }

    int CompiledGraph::paddingOf(int output) const {
        return plan->outputs[static_cast<size_t>(output)].usesStage ? plan->globalPad : 0;
    }

    Interval CompiledGraph::rangeOf(int output) const {
        return plan->ranges[static_cast<size_t>(plan->outputs[static_cast<size_t>(output)].value)];
    }

    Interval CompiledGraph::rangeOver(int output, const float boxMin[3], const float boxMax[3]) const {
        std::vector<Interval> ranges = plan->ranges;
        ranges[kBufX] = Interval{boxMin[0], boxMax[0]};
        ranges[kBufY] = Interval{boxMin[1], boxMax[1]};
        ranges[kBufZ] = Interval{boxMin[2], boxMax[2]};
        const PlanOutput& target = plan->outputs[static_cast<size_t>(output)];
        for (uint32_t index : target.ops) rangeOfOp(plan->ops[index], *plan, ranges);
        return ranges[static_cast<size_t>(target.value)];
    }

    bool CompiledGraph::evaluateGrid(const GridDomain& domain, const EvalContext& context, int output, float* out) const {
        const int outputs[1] = {output};
        float* const targets[1] = {out};
        return evaluateGridOutputs(domain, context, outputs, targets);
    }

    bool CompiledGraph::evaluateGridOutputs(const GridDomain& domain, const EvalContext& context, std::span<const int> outputs, std::span<float* const> out) const {
        if (domain.width <= 0 || domain.depth <= 0) return true;
        const PlanData& data = *plan;
        bool usesStage = false;
        for (int output : outputs) usesStage = usesStage || data.outputs[static_cast<size_t>(output)].usesStage;
        const int pad = usesStage ? data.globalPad : 0;

        GridDomain region = domain;
        region.startX -= pad;
        region.startZ -= pad;
        region.width += 2 * pad;
        region.depth += 2 * pad;
        const size_t points = static_cast<size_t>(region.width) * static_cast<size_t>(region.depth);

        // Without padding the op writes straight into the caller's array; with it, to a region it crops from (R22)
        std::vector<float*> targets(data.outputs.size(), nullptr);
        std::vector<std::unique_ptr<float[]>> padded;
        EvalReport localReport;
        EvalReport& report = context.report ? *context.report : localReport;
        report.peakBytes = 0;
        size_t liveBytes = 0;
        for (size_t i = 0; i < outputs.size(); i++){
            if (pad > 0){
                padded.emplace_back(new float[points]);
                targets[static_cast<size_t>(outputs[i])] = padded.back().get();
                liveBytes += points * sizeof(float);
            } else {
                targets[static_cast<size_t>(outputs[i])] = out[i];
            }
        }
        report.peakBytes = liveBytes;

        std::vector<float*> grids(static_cast<size_t>(data.gridCount), nullptr);
        Run run{data, context, targets, grids};
        run.region = region;
        run.pad = pad;
        std::unique_ptr<std::atomic<uint64_t>[]> opNs;
        if (context.profile){
            opNs.reset(new std::atomic<uint64_t>[data.ops.size()]);
            for (size_t i = 0; i < data.ops.size(); i++) opNs[i].store(0, std::memory_order_relaxed);
            run.opNs = opNs.get();
        }

        bool finished;
        if (outputs.size() == 1){
            const std::vector<uint32_t>& list = data.outputs[static_cast<size_t>(outputs[0])].ops;
            finished = runRegion(run, points, list.data(), list.size(), report, liveBytes);
        } else {
            const std::vector<uint32_t> list = mergedOps(data, outputs);
            finished = runRegion(run, points, list.data(), list.size(), report, liveBytes);
        }
        report.cancelled = !finished;
        if (context.profile) finishProfile(run, static_cast<size_t>(domain.width) * static_cast<size_t>(domain.depth), opNs.get());
        if (finished && pad > 0){
            for (size_t i = 0; i < outputs.size(); i++){
                const float* from = padded[i].get();
                for (int z = 0; z < domain.depth; z++){
                    std::memcpy(out[i] + static_cast<size_t>(z) * static_cast<size_t>(domain.width),
                                from + static_cast<size_t>(z + pad) * static_cast<size_t>(region.width) + static_cast<size_t>(pad),
                                static_cast<size_t>(domain.width) * sizeof(float));
                }
            }
        }
        return finished;
    }

    bool CompiledGraph::evaluateGrid3D(const Grid3Domain& domain, const EvalContext& context, int output, float* out) const {
        if (domain.width <= 0 || domain.height <= 0 || domain.depth <= 0) return true;
        const PlanData& data = *plan;
        const size_t points = static_cast<size_t>(domain.width) * static_cast<size_t>(domain.height) * static_cast<size_t>(domain.depth);
        const PlanOutput& target = data.outputs[static_cast<size_t>(output)];
        if (target.usesStage){
            std::fill(out, out + points, 0.0f);
            return false;
        }
        std::vector<float*> targets(data.outputs.size(), nullptr);
        targets[static_cast<size_t>(output)] = out;
        std::vector<float*> grids(static_cast<size_t>(data.gridCount), nullptr);
        Run run{data, context, targets, grids};
        run.threeD = true;
        run.volume = domain;
        EvalReport localReport;
        EvalReport& report = context.report ? *context.report : localReport;
        size_t liveBytes = 0;
        std::unique_ptr<std::atomic<uint64_t>[]> opNs;
        if (context.profile){
            opNs.reset(new std::atomic<uint64_t>[data.ops.size()]);
            for (size_t i = 0; i < data.ops.size(); i++) opNs[i].store(0, std::memory_order_relaxed);
            run.opNs = opNs.get();
        }
        const bool finished = runRegion(run, points, target.ops.data(), target.ops.size(), report, liveBytes);
        if (context.profile) finishProfile(run, points, opNs.get());
        return finished;
    }

    void CompiledGraph::evaluatePoints(const float* x, const float* y, const float* z, size_t count, const EvalContext& context,
                                       int output, float* out, EvalScratch& scratch) const {
        const PlanData& data = *plan;
        const PlanOutput& target = data.outputs[static_cast<size_t>(output)];
        if (target.usesStage){
            std::fill(out, out + count, 0.0f);
            return;
        }
        scratch.reserve(scratchBytes());
        float* base = scratch.data();
        float** buf = scratch.table(static_cast<size_t>(data.bufferTotal));
        for (size_t start = 0; start < count; start += kChunkPoints){
            const size_t n = std::min(kChunkPoints, count - start);
            // Values a stage elsewhere in the plan reads are grids in a region; here they are one more buffer
            for (int32_t i = kFirstSlot; i < data.bufferTotal; i++) buf[i] = base + static_cast<size_t>(i) * kChunkPoints;
            // x, y and z are read in place, never written: the kernels take them as inputs only
            buf[kBufX] = const_cast<float*>(x + start);
            buf[kBufZ] = const_cast<float*>(z + start);
            if (y){
                buf[kBufY] = const_cast<float*>(y + start);
            } else {
                std::memset(base + kBufY * kChunkPoints, 0, n * sizeof(float));
                buf[kBufY] = base + kBufY * kChunkPoints;
            }
            buf[kBufU] = base + kBufU * kChunkPoints;
            buf[kBufV] = base + kBufV * kChunkPoints;
            if (data.readsUv){
                const double perUnit = context.uvPerUnit > 0.0 ? context.uvPerUnit : 1.0;
                for (size_t k = 0; k < n; k++){
                    const float u = static_cast<float>(static_cast<double>(x[start + k]) * perUnit);
                    const float v = static_cast<float>(static_cast<double>(z[start + k]) * perUnit);
                    buf[kBufU][k] = u > 0.0f ? (u < 1.0f ? u : 1.0f) : 0.0f;
                    buf[kBufV][k] = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
                }
            }
            withBackend(context.backend, [&](auto backend){
                using B = decltype(backend);
                for (uint32_t index : target.ops){
                    const PlanOp& op = data.ops[index];
                    if (op.kind == OpKind::Output){
                        const float* from = buf[op.in[0]];
                        if (op.f[0] > 0.0f){
                            const float top = context.maxHeight > 0.0f ? context.maxHeight : 1.0f;
                            for (size_t k = 0; k < n; k++) out[start + k] = from[k] / top;
                        } else {
                            std::memcpy(out + start, from, n * sizeof(float));
                        }
                    } else if (op.kind == OpKind::TerrainHeight){
                        std::fill(buf[op.out[0]], buf[op.out[0]] + n, 0.0f);
                    } else {
                        PointOps<B>::run(op, data, buf, n, context);
                    }
                }
            });
        }
    }

}}

#endif // JASPERENGINE_NOISE_ENABLED
