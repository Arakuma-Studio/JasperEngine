// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_CURLNOISEBATCH_H
#define NOISE_CURLNOISEBATCH_H

// Arrays of curl noise samples through the 4-wide kernels (GradientCurl3DSimd.h), on the
// build's widest SIMD back end or on the scalar one: what the particle Noise module and the
// force fields run, for tests, benchmarks and tools. Built with the particle kernels' flags.

#include "Export.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine { namespace noise {

    enum class CurlBackend { Scalar, Native };

    // the name of the Native back end ("sse2", "neon", "wasm-simd128" or "scalar")
    JASPERENGINE_API const char* curlNativeBackend();

    // potentials 3: the reference field (High); 2: the two-potential one (Medium)
    JASPERENGINE_API void curlNoiseBatch(CurlBackend backend, int potentials, const float* x, const float* y, const float* z,
                                         size_t count, uint32_t seed, float* outX, float* outY, float* outZ);

}}

#endif // NOISE_CURLNOISEBATCH_H
