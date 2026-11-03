// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A submesh's vertexCount, which a scene file saves, is right as soon as a glTF is parsed: a reload
// reverses the submeshes (MeshSystem::loadGLTF), and until RenderSystem::loadMesh recomputed the
// count (a new shader variant takes seconds on a GPU's compile threads) a save wrote the count of
// another primitive (test_pose_window.py on the RTX). CPU only: no draw runs here.

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    const char* const MODEL = "gltf/two_primitive_counts.gltf";

}

TEST(GltfSubmeshVertexCount, RightAfterTheParse){
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(MODEL));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    ASSERT_EQ(mesh.numSubmeshes, 2u);
    // reversed: the file's second primitive first
    EXPECT_EQ(mesh.submeshes[0].vertexCount, 6u);
    EXPECT_EQ(mesh.submeshes[1].vertexCount, 3u);
}

TEST(GltfSubmeshVertexCount, ARestoredCountDoesNotSwapOnAReload){
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(MODEL));
    // what RenderSystem::loadMesh or a scene file left in the slots
    MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    mesh.submeshes[0].vertexCount = 6;
    mesh.submeshes[1].vertexCount = 3;
    ASSERT_TRUE(model.loadModel(MODEL));
    const MeshComponent& reloaded = scene.getComponent<MeshComponent>(model.getEntity());
    ASSERT_EQ(reloaded.numSubmeshes, 2u);
    EXPECT_EQ(reloaded.submeshes[0].vertexCount, 6u);
    EXPECT_EQ(reloaded.submeshes[1].vertexCount, 3u);
}
