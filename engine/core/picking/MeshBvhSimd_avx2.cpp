// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The AVX2 8-child slab test, adapted from OpenEngine's MeshBvh::Raycast (Engine/Modules/
// MeshPicking/Source/MeshBvh.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License (NOTICE.md). Built for the baseline x86-64 target: only this
// function carries the avx2 target (no -mavx2 for the file), and it runs only once CPUID says
// the CPU and the OS support it (MeshBvh.cpp). The decode is a multiply then an add, not an
// FMA, as the scalar and NEON paths do.

#include "MeshBvhSimd.h"

#if JE_PICK_HAS_AVX2

#include <immintrin.h>

#if defined(__GNUC__) || defined(__clang__)
#define JE_PICK_AVX2_TARGET __attribute__((target("avx2")))
#else
#define JE_PICK_AVX2_TARGET
#endif

using namespace jasperengine;

namespace {

    // boxMin + q * scale for 8 children (a function, not a lambda: a lambda would not carry
    // the avx2 target)
    JE_PICK_AVX2_TARGET
    inline __m256 decode8(const uint8_t* q, float scale, float boxMin){
        const __m128i bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(q));
        const __m256 values = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(bytes));
        return _mm256_add_ps(_mm256_mul_ps(values, _mm256_set1_ps(scale)), _mm256_set1_ps(boxMin));
    }

}

JE_PICK_AVX2_TARGET
uint32_t picking::slabTest8Avx2(const MeshBvh::Node& node, const SlabRay& ray, float tBest, float* tEnter){
    const __m256 loX = decode8(node.qMinX, node.scale[0], node.boxMin[0]);
    const __m256 loY = decode8(node.qMinY, node.scale[1], node.boxMin[1]);
    const __m256 loZ = decode8(node.qMinZ, node.scale[2], node.boxMin[2]);
    const __m256 hiX = decode8(node.qMaxX, node.scale[0], node.boxMin[0]);
    const __m256 hiY = decode8(node.qMaxY, node.scale[1], node.boxMin[1]);
    const __m256 hiZ = decode8(node.qMaxZ, node.scale[2], node.boxMin[2]);

    const __m256 originX = _mm256_set1_ps(ray.origin[0]);
    const __m256 originY = _mm256_set1_ps(ray.origin[1]);
    const __m256 originZ = _mm256_set1_ps(ray.origin[2]);
    const __m256 invX = _mm256_set1_ps(ray.invDir[0]);
    const __m256 invY = _mm256_set1_ps(ray.invDir[1]);
    const __m256 invZ = _mm256_set1_ps(ray.invDir[2]);

    const __m256 t1x = _mm256_mul_ps(_mm256_sub_ps(loX, originX), invX);
    const __m256 t2x = _mm256_mul_ps(_mm256_sub_ps(hiX, originX), invX);
    const __m256 t1y = _mm256_mul_ps(_mm256_sub_ps(loY, originY), invY);
    const __m256 t2y = _mm256_mul_ps(_mm256_sub_ps(hiY, originY), invY);
    const __m256 t1z = _mm256_mul_ps(_mm256_sub_ps(loZ, originZ), invZ);
    const __m256 t2z = _mm256_mul_ps(_mm256_sub_ps(hiZ, originZ), invZ);

    // _mm256_min_ps(a, b) is a < b ? a : b, _mm256_max_ps(a, b) a > b ? a : b: the scalar rules
    const __m256 nearXY = _mm256_max_ps(_mm256_min_ps(t1x, t2x), _mm256_min_ps(t1y, t2y));
    const __m256 tMin = _mm256_max_ps(nearXY, _mm256_min_ps(t1z, t2z));
    const __m256 farXY = _mm256_min_ps(_mm256_max_ps(t1x, t2x), _mm256_max_ps(t1y, t2y));
    const __m256 tMax = _mm256_mul_ps(_mm256_min_ps(farXY, _mm256_max_ps(t1z, t2z)), _mm256_set1_ps(SLAB_ROBUST));

    const __m256 hit = _mm256_and_ps(_mm256_and_ps(_mm256_cmp_ps(tMin, tMax, _CMP_LE_OQ),
                                                   _mm256_cmp_ps(tMax, _mm256_setzero_ps(), _CMP_GE_OQ)),
                                     _mm256_cmp_ps(tMin, _mm256_set1_ps(tBest), _CMP_LE_OQ));
    _mm256_storeu_ps(tEnter, tMin);
    return (uint32_t)_mm256_movemask_ps(hit) & node.valid;
}

#endif
