// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The 8-child slab test of MeshBvh's traversal, one function per SIMD path (D3). Adapted from
// OpenEngine's MeshBvh::Raycast (Engine/Modules/MeshPicking/Source/MeshBvh.cpp), Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md). Every path does
// the same arithmetic in the same order (decode: one multiply then one add, no FMA; min/max as
// a < b ? a : b), so the hit bits are bit-identical; the picking sources are compiled without
// floating-point contraction for that (engine/CMakeLists.txt).

#ifndef MESHBVHSIMD_H
#define MESHBVHSIMD_H

#include "picking/MeshBvh.h"

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#define JE_PICK_HAS_AVX2 1
#else
#define JE_PICK_HAS_AVX2 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define JE_PICK_HAS_NEON 1
#else
#define JE_PICK_HAS_NEON 0
#endif

namespace jasperengine::picking {

    // the slab exit is widened by this factor so that rounding never drops a box a hit lies
    // on (Ize, "Robust BVH Ray Traversal", 2013: 1 + 2 gamma(3), rounded up)
    inline constexpr float SLAB_ROBUST = 1.0000008f;

    struct SlabRay {
        float origin[3];
        float invDir[3];
    };

    // Bit i set: child i is valid and its box meets the ray at an entry tEnter[i] <= tBest
    // with an exit >= 0. tEnter is written for all 8 slots.
    using SlabTest8 = uint32_t (*)(const MeshBvh::Node& node, const SlabRay& ray, float tBest, float* tEnter);

    uint32_t slabTest8Scalar(const MeshBvh::Node& node, const SlabRay& ray, float tBest, float* tEnter);
#if JE_PICK_HAS_AVX2
    uint32_t slabTest8Avx2(const MeshBvh::Node& node, const SlabRay& ray, float tBest, float* tEnter);
#endif
#if JE_PICK_HAS_NEON
    uint32_t slabTest8Neon(const MeshBvh::Node& node, const SlabRay& ray, float tBest, float* tEnter);
#endif

    // the path getPickSimdPath() names
    SlabTest8 activeSlabTest();

}

#endif // MESHBVHSIMD_H
