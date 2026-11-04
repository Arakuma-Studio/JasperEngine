// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// What a mesh costs (mesh-memory spec, docs/superpowers/specs/2026-11-04-mesh-memory-design.md):
// a component holds what it uses, not a capacity. Each ceiling is the size measured on Linux
// (libstdc++, x86-64) with some room; a field that brings an inline array back trips it.

#include <gtest/gtest.h>

#include "Engine.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "render/ObjectRender.h"
#include "subsystem/MeshSystem.h"

using namespace jasperengine;

TEST(MeshMemory, SizeofCeilings) {
    EXPECT_LE(sizeof(SokolObject), 1024u);
    EXPECT_LE(sizeof(ObjectRender), 1088u);
    EXPECT_LE(sizeof(Submesh), 7168u);
    EXPECT_LE(sizeof(MeshComponent), 2048u);
    EXPECT_LE(sizeof(SpriteComponent), 512u);
    EXPECT_LE(sizeof(TilemapComponent), 512u);
}

// A sprite's mesh holds one submesh and no palette: about a twentieth of the 204,840 bytes every
// mesh took with 16 inline submeshes, a 128-bone palette and 30 buffer slots (estimated)
TEST(MeshMemory, SpriteCostsATwentiethOfBefore) {
    MeshComponent mesh;
    MeshSystem::prepareSpriteMesh(mesh);
    EXPECT_EQ(mesh.submeshes.stored(), 1u);
    EXPECT_EQ(mesh.bonesMatrix.stored(), 0u);
    EXPECT_EQ(mesh.eBuffers.stored(), 0u);
    EXPECT_LE(sizeof(MeshComponent) + mesh.arrayHeapBytes(), 10240u);
}
