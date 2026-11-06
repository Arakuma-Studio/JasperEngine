// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// What a mesh costs (mesh-memory spec, docs/superpowers/specs/2026-11-04-mesh-memory-design.md):
// a component holds what it uses, not a capacity. Each ceiling is the size measured on Linux
// (libstdc++, x86-64: 816, 832, 8,528, 1,744, 72 and 152 bytes; 2,528, 2,544, 11,776, 204,928,
// 6,192 and 125,992 before; a Submesh holds S55's two batch render objects since) with some
// room; a field that brings an inline array back trips it.

#include <gtest/gtest.h>

#include <cstdio>

#include "Engine.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "render/ObjectRender.h"
#include "subsystem/MeshSystem.h"

using namespace jasperengine;

TEST(MeshMemory, SizeofCeilings) {
    std::printf("[sizes] SokolObject %zu, ObjectRender %zu, Submesh %zu, MeshComponent %zu, SpriteComponent %zu, TilemapComponent %zu\n",
                sizeof(SokolObject), sizeof(ObjectRender), sizeof(Submesh), sizeof(MeshComponent), sizeof(SpriteComponent),
                sizeof(TilemapComponent));
    EXPECT_LE(sizeof(SokolObject), 896u);
    EXPECT_LE(sizeof(ObjectRender), 896u);
    EXPECT_LE(sizeof(Submesh), 9472u);
    EXPECT_LE(sizeof(MeshComponent), 2048u);
    EXPECT_LE(sizeof(SpriteComponent), 256u);
    EXPECT_LE(sizeof(TilemapComponent), 384u);
}

// A sprite's mesh holds one submesh and no palette: about a twentieth of the 204,928 bytes every
// mesh took with 16 inline submeshes, a 128-bone palette and 30 buffer slots
TEST(MeshMemory, SpriteCostsAnOrderOfMagnitudeLess) {
    MeshComponent mesh;
    MeshSystem::prepareSpriteMesh(mesh);
    EXPECT_EQ(mesh.submeshes.stored(), 1u);
    EXPECT_EQ(mesh.bonesMatrix.stored(), 0u);
    EXPECT_EQ(mesh.eBuffers.stored(), 0u);
    std::printf("[sizes] a sprite's mesh: %zu bytes with its arrays\n", sizeof(MeshComponent) + mesh.arrayHeapBytes());
    EXPECT_LE(sizeof(MeshComponent) + mesh.arrayHeapBytes(), 11264u);
}
