// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CurlNoiseBatch.h"

#include "math/noise/GradientCurl3DSimd.h"

using namespace jasperengine;
using namespace jasperengine::noise;

namespace {

    template<class B>
    void run(int potentials, const float* x, const float* y, const float* z, size_t count, uint32_t seed, float* ox, float* oy, float* oz){
        typedef GradientCurlSimd<B> Curl;
        for (size_t i = 0; i < count; i += 4){
            // the tail block reads and writes through a padded copy
            alignas(16) float bx[4] = {0, 0, 0, 0}, by[4] = {0, 0, 0, 0}, bz[4] = {0, 0, 0, 0};
            const size_t n = count - i < 4 ? count - i : 4;
            for (size_t l = 0; l < n; l++){ bx[l] = x[i + l]; by[l] = y[i + l]; bz[l] = z[i + l]; }
            const typename Curl::V3 c = potentials == 2 ? Curl::curl2(B::loadu(bx), B::loadu(by), B::loadu(bz), seed)
                                                        : Curl::curl3(B::loadu(bx), B::loadu(by), B::loadu(bz), seed);
            alignas(16) float rx[4], ry[4], rz[4];
            B::storeu(rx, c.x);
            B::storeu(ry, c.y);
            B::storeu(rz, c.z);
            for (size_t l = 0; l < n; l++){ ox[i + l] = rx[l]; oy[i + l] = ry[l]; oz[i + l] = rz[l]; }
        }
    }

}

const char* noise::curlNativeBackend(){
    return simd::Native::name;
}

void noise::curlNoiseBatch(CurlBackend backend, int potentials, const float* x, const float* y, const float* z,
                           size_t count, uint32_t seed, float* outX, float* outY, float* outZ){
    if (backend == CurlBackend::Native)
        run<simd::Native>(potentials, x, y, z, count, seed, outX, outY, outZ);
    else
        run<simd::Scalar>(potentials, x, y, z, count, seed, outX, outY, outZ);
}
