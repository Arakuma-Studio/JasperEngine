// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The NEON 8-child slab test (two halves of 4 children), adapted from OpenEngine's
// MeshBvh::Raycast (Engine/Modules/MeshPicking/Source/MeshBvh.cpp), Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md). arm64 always has NEON.
// The decode is vmul then vadd (no vfma), as the scalar and AVX2 paths do; vminq/vmaxq differ
// from a < b ? a : b only in the sign of a zero, which no comparison sees.

#include "MeshBvhSimd.h"

#if JE_PICK_HAS_NEON

#include <arm_neon.h>

using namespace jasperengine;

uint32_t picking::slabTest8Neon(const MeshBvh::Node& node, const SlabRay& ray, float tBest, float* tEnter){
    const float32x4_t scaleX = vdupq_n_f32(node.scale[0]);
    const float32x4_t scaleY = vdupq_n_f32(node.scale[1]);
    const float32x4_t scaleZ = vdupq_n_f32(node.scale[2]);
    const float32x4_t minX = vdupq_n_f32(node.boxMin[0]);
    const float32x4_t minY = vdupq_n_f32(node.boxMin[1]);
    const float32x4_t minZ = vdupq_n_f32(node.boxMin[2]);

    auto decode = [](const uint8_t* q, float32x4_t scale, float32x4_t boxMin, float32x4_t& low, float32x4_t& high){
        const uint16x8_t wide = vmovl_u8(vld1_u8(q));
        const float32x4_t lowValues = vcvtq_f32_u32(vmovl_u16(vget_low_u16(wide)));
        const float32x4_t highValues = vcvtq_f32_u32(vmovl_u16(vget_high_u16(wide)));
        low = vaddq_f32(vmulq_f32(lowValues, scale), boxMin);
        high = vaddq_f32(vmulq_f32(highValues, scale), boxMin);
    };

    float32x4_t loX[2], loY[2], loZ[2], hiX[2], hiY[2], hiZ[2];
    decode(node.qMinX, scaleX, minX, loX[0], loX[1]);
    decode(node.qMinY, scaleY, minY, loY[0], loY[1]);
    decode(node.qMinZ, scaleZ, minZ, loZ[0], loZ[1]);
    decode(node.qMaxX, scaleX, minX, hiX[0], hiX[1]);
    decode(node.qMaxY, scaleY, minY, hiY[0], hiY[1]);
    decode(node.qMaxZ, scaleZ, minZ, hiZ[0], hiZ[1]);

    const float32x4_t originX = vdupq_n_f32(ray.origin[0]);
    const float32x4_t originY = vdupq_n_f32(ray.origin[1]);
    const float32x4_t originZ = vdupq_n_f32(ray.origin[2]);
    const float32x4_t invX = vdupq_n_f32(ray.invDir[0]);
    const float32x4_t invY = vdupq_n_f32(ray.invDir[1]);
    const float32x4_t invZ = vdupq_n_f32(ray.invDir[2]);
    const float32x4_t robust = vdupq_n_f32(SLAB_ROBUST);
    const float32x4_t best = vdupq_n_f32(tBest);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    static const uint32_t weights[4] = {1u, 2u, 4u, 8u};
    const uint32x4_t weight = vld1q_u32(weights);

    uint32_t bits = 0;
    for (int half = 0; half < 2; half++){
        const float32x4_t t1x = vmulq_f32(vsubq_f32(loX[half], originX), invX);
        const float32x4_t t2x = vmulq_f32(vsubq_f32(hiX[half], originX), invX);
        const float32x4_t t1y = vmulq_f32(vsubq_f32(loY[half], originY), invY);
        const float32x4_t t2y = vmulq_f32(vsubq_f32(hiY[half], originY), invY);
        const float32x4_t t1z = vmulq_f32(vsubq_f32(loZ[half], originZ), invZ);
        const float32x4_t t2z = vmulq_f32(vsubq_f32(hiZ[half], originZ), invZ);

        const float32x4_t nearXY = vmaxq_f32(vminq_f32(t1x, t2x), vminq_f32(t1y, t2y));
        const float32x4_t tMin = vmaxq_f32(nearXY, vminq_f32(t1z, t2z));
        const float32x4_t farXY = vminq_f32(vmaxq_f32(t1x, t2x), vmaxq_f32(t1y, t2y));
        const float32x4_t tMax = vmulq_f32(vminq_f32(farXY, vmaxq_f32(t1z, t2z)), robust);

        const uint32x4_t hit = vandq_u32(vandq_u32(vcleq_f32(tMin, tMax), vcgeq_f32(tMax, zero)), vcleq_f32(tMin, best));
        bits |= vaddvq_u32(vandq_u32(hit, weight)) << (half * 4);
        vst1q_f32(tEnter + half * 4, tMin);
    }
    return bits & node.valid;
}

#endif
