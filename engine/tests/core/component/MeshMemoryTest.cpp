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
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "object/Mesh.h"
#include "render/ObjectRender.h"
#include "subsystem/MeshSystem.h"

using namespace jasperengine;

namespace {

    // MSVC's checked iterators (Debug: _ITERATOR_DEBUG_LEVEL above 0) give every std::string,
    // std::vector and std::map a proxy pointer of their own, which the measured sizes do not hold
#if defined(_MSC_VER) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
    constexpr size_t meshMemoryCeiling(size_t bytes) { return bytes + bytes / 2; }
#else
    constexpr size_t meshMemoryCeiling(size_t bytes) { return bytes; }
#endif

}

TEST(MeshMemory, SizeofCeilings) {
    std::printf("[sizes] SokolObject %zu, ObjectRender %zu, Submesh %zu, MeshComponent %zu, SpriteComponent %zu, TilemapComponent %zu\n",
                sizeof(SokolObject), sizeof(ObjectRender), sizeof(Submesh), sizeof(MeshComponent), sizeof(SpriteComponent),
                sizeof(TilemapComponent));
    EXPECT_LE(sizeof(SokolObject), meshMemoryCeiling(896));
    EXPECT_LE(sizeof(ObjectRender), meshMemoryCeiling(896));
    EXPECT_LE(sizeof(Submesh), meshMemoryCeiling(9472));
    EXPECT_LE(sizeof(MeshComponent), meshMemoryCeiling(2048));
    EXPECT_LE(sizeof(SpriteComponent), meshMemoryCeiling(256));
    EXPECT_LE(sizeof(TilemapComponent), meshMemoryCeiling(384));
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
    EXPECT_LE(sizeof(MeshComponent) + mesh.arrayHeapBytes(), meshMemoryCeiling(11264));
}

// Every Mesh method taking a submesh index refuses one the mesh does not have, with an error, and
// grows nothing: a submesh is 8.5 KB, and Mesh:getMaterial(1000000) from a script made a million
TEST(MeshMemory, SubmeshIndexPastTheMeshGrowsNothing) {
    Scene scene;
    Mesh mesh(&scene);
    MeshComponent& component = mesh.getComponent<MeshComponent>();
    component.numSubmeshes = 2;
    component.submeshes[1].material.name = "second";
    const size_t stored = component.submeshes.stored();
    const Submesh& unbuilt = StableArray<Submesh>::defaultElement();

    Material red;
    red.name = "red";
    mesh.setMaterial(40, red);
    mesh.setPrimitiveType(40, PrimitiveType::LINES);
    mesh.setFaceCulling(40, !unbuilt.faceCulling);
    mesh.setCastShadowsWithTexture(40, !unbuilt.textureShadow);
    mesh.setNormalTexture(40, "normal.png");
    EXPECT_EQ(mesh.getMaterial(40).name, unbuilt.material.name);
    EXPECT_EQ(mesh.getPrimitiveType(40), unbuilt.primitiveType);
    EXPECT_EQ(mesh.isFaceCulling(40), unbuilt.faceCulling);
    EXPECT_EQ(mesh.isCastShadowsWithTexture(40), unbuilt.textureShadow);
    EXPECT_EQ(mesh.getNormalTexture(40), "");
    EXPECT_EQ(component.submeshes.stored(), stored) << "an index past the submeshes grew the array";

    // the submeshes it has still take them
    mesh.setMaterial(1, red);
    EXPECT_EQ(mesh.getMaterial(1).name, "red");
    EXPECT_EQ(component.submeshes.stored(), stored);
}
