// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "noise/NoiseSwitch.h"

#if JASPERENGINE_NOISE_ENABLED

#include "noise/NoiseKernelImpl.h"

#include <cfloat>
#include <cstring>
#include <limits>
#include <type_traits>

// R40: a 32-bit x86 build on x87 would round through 80-bit registers, and no table could hold
static_assert(FLT_EVAL_METHOD == 0, "the noise kernels need float arithmetic in float precision");
static_assert(std::numeric_limits<float>::is_iec559, "the noise kernels need IEEE 754 floats");

namespace jasperengine { namespace noise {

    namespace {

        constexpr int kGridBatch = 256;

        // Whether the second instantiation exists: only when asked (editor, tests) and only when Native is not already it
#if defined(JASPERENGINE_NOISE_ALL_BACKENDS)
        constexpr bool kScalarBuilt = !std::is_same_v<Native, simd::Scalar>;
#else
        constexpr bool kScalarBuilt = false;
#endif

        template<class B>
        void basisRun(const BasisParams& params, const float* x, const float* y, const float* z, size_t count, float* out){
            forEachLaneGroup<B>(x, y, z, count, out, [&params](typename B::f4 px, typename B::f4 py, typename B::f4 pz){
                return NoiseKernel<B>::basis(params, px, py, pz);
            });
        }

        template<class B>
        void fbmRun(const FractalParams& params, const float* x, const float* y, const float* z, size_t count, float* out){
            forEachLaneGroup<B>(x, y, z, count, out, [&params](typename B::f4 px, typename B::f4 py, typename B::f4 pz){
                return NoiseKernel<B>::fbm(params, px, py, pz);
            });
        }

        template<class B>
        void ridgedRun(const FractalParams& params, const float* x, const float* y, const float* z, size_t count, float* out){
            forEachLaneGroup<B>(x, y, z, count, out, [&params](typename B::f4 px, typename B::f4 py, typename B::f4 pz){
                return NoiseKernel<B>::ridged(params, px, py, pz);
            });
        }

        FractalParams clampedFractal(const FractalParams& params){
            FractalParams out = params;
            out.octaves = params.octaves < 1 ? 1 : (params.octaves > 12 ? 12 : params.octaves);
            return out;
        }

    }

    void basisBatch(const BasisParams& params, const float* x, const float* y, const float* z, size_t count, float* out, Backend backend){
#if defined(JASPERENGINE_NOISE_ALL_BACKENDS)
        if constexpr (kScalarBuilt){
            if (backend == Backend::Scalar){ basisRun<simd::Scalar>(params, x, y, z, count, out); return; }
        }
#else
        (void)backend;
#endif
        basisRun<Native>(params, x, y, z, count, out);
    }

    void fbmBatch(const FractalParams& params, const float* x, const float* y, const float* z, size_t count, float* out, Backend backend){
        const FractalParams clamped = clampedFractal(params);
#if defined(JASPERENGINE_NOISE_ALL_BACKENDS)
        if constexpr (kScalarBuilt){
            if (backend == Backend::Scalar){ fbmRun<simd::Scalar>(clamped, x, y, z, count, out); return; }
        }
#else
        (void)backend;
#endif
        fbmRun<Native>(clamped, x, y, z, count, out);
    }

    void ridgedBatch(const FractalParams& params, const float* x, const float* y, const float* z, size_t count, float* out, Backend backend){
        const FractalParams clamped = clampedFractal(params);
#if defined(JASPERENGINE_NOISE_ALL_BACKENDS)
        if constexpr (kScalarBuilt){
            if (backend == Backend::Scalar){ ridgedRun<simd::Scalar>(clamped, x, y, z, count, out); return; }
        }
#else
        (void)backend;
#endif
        ridgedRun<Native>(clamped, x, y, z, count, out);
    }

    void basisGrid2D(const BasisParams& params, int64_t startX, int64_t startZ, int width, int depth, double unitsPerTexel, float* out, Backend backend){
        float x[kGridBatch], z[kGridBatch], zero[kGridBatch];
        std::memset(zero, 0, sizeof(zero));
        for (int j = 0; j < depth; j++){
            const float pz = static_cast<float>((static_cast<double>(startZ + j) + 0.5) * unitsPerTexel);
            for (int i = 0; i < width; i += kGridBatch){
                const int n = width - i < kGridBatch ? width - i : kGridBatch;
                for (int k = 0; k < n; k++){
                    x[k] = static_cast<float>((static_cast<double>(startX + i + k) + 0.5) * unitsPerTexel);
                    z[k] = pz;
                }
                basisBatch(params, x, zero, z, static_cast<size_t>(n), out + static_cast<size_t>(j) * static_cast<size_t>(width) + static_cast<size_t>(i), backend);
            }
        }
    }

    void basisGrid3D(const BasisParams& params, int64_t startX, int64_t startY, int64_t startZ, int width, int height, int depth, double unitsPerVoxel, float* out, Backend backend){
        float x[kGridBatch], y[kGridBatch], z[kGridBatch];
        size_t offset = 0;
        for (int l = 0; l < depth; l++){
            const float pz = static_cast<float>(static_cast<double>(startZ + l) * unitsPerVoxel);
            for (int j = 0; j < height; j++){
                const float py = static_cast<float>(static_cast<double>(startY + j) * unitsPerVoxel);
                for (int i = 0; i < width; i += kGridBatch){
                    const int n = width - i < kGridBatch ? width - i : kGridBatch;
                    for (int k = 0; k < n; k++){
                        x[k] = static_cast<float>(static_cast<double>(startX + i + k) * unitsPerVoxel);
                        y[k] = py;
                        z[k] = pz;
                    }
                    basisBatch(params, x, y, z, static_cast<size_t>(n), out + offset, backend);
                    offset += static_cast<size_t>(n);
                }
            }
        }
    }

    float perlin2(float x, float z, uint32_t seed){ return NoiseKernel<Lane1>::perlin2(x, z, seed); }
    float perlin3(float x, float y, float z, uint32_t seed){ return NoiseKernel<Lane1>::perlin3(x, y, z, seed); }
    float simplex2(float x, float z, uint32_t seed){ return NoiseKernel<Lane1>::simplex2(x, z, seed); }
    float simplex3(float x, float y, float z, uint32_t seed){ return NoiseKernel<Lane1>::simplex3(x, y, z, seed); }
    float value2(float x, float z, uint32_t seed){ return NoiseKernel<Lane1>::value2(x, z, seed); }
    float value3(float x, float y, float z, uint32_t seed){ return NoiseKernel<Lane1>::value3(x, y, z, seed); }

    float cellular2(float x, float z, uint32_t seed, CellularReturn cellReturn, CellularMetric metric, float jitter){
        BasisParams params;
        params.basis = Basis::Cellular;
        params.seed = seed;
        params.cellReturn = cellReturn;
        params.metric = metric;
        params.jitter = jitter;
        return NoiseKernel<Lane1>::cellular2(x, z, params);
    }

    float cellular3(float x, float y, float z, uint32_t seed, CellularReturn cellReturn, CellularMetric metric, float jitter){
        BasisParams params;
        params.basis = Basis::Cellular;
        params.dimensions = 3;
        params.seed = seed;
        params.cellReturn = cellReturn;
        params.metric = metric;
        params.jitter = jitter;
        return NoiseKernel<Lane1>::cellular3(x, y, z, params);
    }

    float basisPoint(const BasisParams& params, float x, float y, float z){
        return NoiseKernel<Lane1>::basis(params, x, y, z);
    }

    float fbmPoint(const FractalParams& params, float x, float y, float z){
        return NoiseKernel<Lane1>::fbm(clampedFractal(params), x, y, z);
    }

    float ridgedPoint(const FractalParams& params, float x, float y, float z){
        return NoiseKernel<Lane1>::ridged(clampedFractal(params), x, y, z);
    }

    Interval basisRange(const BasisParams& params){
        if (params.basis != Basis::Cellular || params.cellReturn == CellularReturn::CellValue) return Interval{-1.0f, 1.0f};
        // A feature point lies in its own cell, so F1 is bounded by that cell's far corner and F2 by
        // the far corner of the adjacent cell: distances, in float, with a margin for rounding
        const bool threeD = params.dimensions == 3;
        const bool nearestOnly = params.cellReturn == CellularReturn::F1;
        float top = 0.0f;
        switch (params.metric){
            case CellularMetric::Euclidean: top = nearestOnly ? (threeD ? 1.7320511f : 1.4142137f) : (threeD ? 2.4494899f : 2.2360681f); break;
            case CellularMetric::Manhattan: top = nearestOnly ? (threeD ? 3.0f : 2.0f) : (threeD ? 4.0f : 3.0f); break;
            default: top = nearestOnly ? 1.0f : 2.0f; break;
        }
        return Interval{0.0f, top * 1.000001f};
    }

    const char* nativeBackendName(){
        return Native::name;
    }

    bool hasScalarBackend(){
        return kScalarBuilt || std::is_same_v<Native, simd::Scalar>;
    }

    uint64_t hashFloats(const float* values, size_t count){
        uint64_t h = 14695981039346656037ull;
        for (size_t i = 0; i < count; i++){
            uint32_t bits;
            std::memcpy(&bits, values + i, sizeof(bits));
            for (int b = 0; b < 4; b++){
                h ^= (bits >> (b * 8)) & 0xffu;
                h *= 1099511628211ull;
            }
        }
        return h;
    }

}}

#endif // JASPERENGINE_NOISE_ENABLED
