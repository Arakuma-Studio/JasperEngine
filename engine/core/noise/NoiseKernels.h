// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISEKERNELS_H
#define NOISE_NOISEKERNELS_H

// The noise library's public face (T2 R1-R8): Perlin, Simplex, Value and Cellular in 2D and 3D, with
// FBm and Ridged over them. A sample is a pure function of its position, seed and parameters, so
// where it falls in a batch cannot change its bits, and every target gives the same bits (R4). The
// library owns no thread and no mutable global (R7). The kernels themselves stay behind this header:
// they must only compile with the noise flags (R38).

#include "Export.h"
#include "noise/NoiseTypes.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine { namespace noise {

    enum class Basis : uint8_t { Perlin, Simplex, Value, Cellular };
    enum class CellularMetric : uint8_t { Euclidean, Manhattan, Chebyshev };
    enum class CellularReturn : uint8_t { F1, F2, F2MinusF1, CellValue };

    struct BasisParams {
        Basis basis = Basis::Perlin;
        int dimensions = 2;                 // 2: samples x and z. 3: x, y and z.
        float frequency = 1.0f;
        uint32_t seed = 0;
        CellularMetric metric = CellularMetric::Euclidean;
        CellularReturn cellReturn = CellularReturn::F1;
        float jitter = 1.0f;                // 0..1
    };

    struct FractalParams {
        BasisParams basis;
        int octaves = 4;                    // 1..12
        float lacunarity = 2.0f;
        float gain = 0.5f;
    };

    // Position arrays, any count; y is ignored (and may be null) in 2D (R2)
    JASPERENGINE_API void basisBatch(const BasisParams& params, const float* x, const float* y, const float* z, size_t count, float* out, Backend backend = Backend::Native);
    JASPERENGINE_API void fbmBatch(const FractalParams& params, const float* x, const float* y, const float* z, size_t count, float* out, Backend backend = Backend::Native);
    JASPERENGINE_API void ridgedBatch(const FractalParams& params, const float* x, const float* y, const float* z, size_t count, float* out, Backend backend = Backend::Native);

    // A uniform grid, width * depth values, row by row: the position of (i, j) is R23's formula
    JASPERENGINE_API void basisGrid2D(const BasisParams& params, int64_t startX, int64_t startZ, int width, int depth, double unitsPerTexel, float* out, Backend backend = Backend::Native);
    // width * height * depth values, x fastest, then y, then z, on the lattice (no half step, R21)
    JASPERENGINE_API void basisGrid3D(const BasisParams& params, int64_t startX, int64_t startY, int64_t startZ, int width, int height, int depth, double unitsPerVoxel, float* out, Backend backend = Backend::Native);

    // Single points on the one-lane back end, bit-identical to the batches (R3)
    JASPERENGINE_API float perlin2(float x, float z, uint32_t seed = 0);
    JASPERENGINE_API float perlin3(float x, float y, float z, uint32_t seed = 0);
    JASPERENGINE_API float simplex2(float x, float z, uint32_t seed = 0);
    JASPERENGINE_API float simplex3(float x, float y, float z, uint32_t seed = 0);
    JASPERENGINE_API float value2(float x, float z, uint32_t seed = 0);
    JASPERENGINE_API float value3(float x, float y, float z, uint32_t seed = 0);
    JASPERENGINE_API float cellular2(float x, float z, uint32_t seed = 0, CellularReturn cellReturn = CellularReturn::F1, CellularMetric metric = CellularMetric::Euclidean, float jitter = 1.0f);
    JASPERENGINE_API float cellular3(float x, float y, float z, uint32_t seed = 0, CellularReturn cellReturn = CellularReturn::F1, CellularMetric metric = CellularMetric::Euclidean, float jitter = 1.0f);
    JASPERENGINE_API float basisPoint(const BasisParams& params, float x, float y, float z);
    JASPERENGINE_API float fbmPoint(const FractalParams& params, float x, float y, float z);
    JASPERENGINE_API float ridgedPoint(const FractalParams& params, float x, float y, float z);

    // A basis' analytic range with these parameters (R19)
    JASPERENGINE_API Interval basisRange(const BasisParams& params);

    // The back end a batch takes for Backend::Native: "sse2", "neon", "wasm" or "scalar"
    JASPERENGINE_API const char* nativeBackendName();
    // Whether Backend::Scalar is its own instantiation in this build (R51)
    JASPERENGINE_API bool hasScalarBackend();

    // FNV-1a 64 over the raw bytes of the values (R41)
    JASPERENGINE_API uint64_t hashFloats(const float* values, size_t count);

}}

#endif // NOISE_NOISEKERNELS_H
