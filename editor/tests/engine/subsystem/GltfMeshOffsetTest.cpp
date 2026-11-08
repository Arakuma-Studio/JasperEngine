// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A glTF whose only mesh node sits under transform nodes: MeshSystem::loadGLTF gives the entity
// the scene root's transform and bakes the nodes between the root and the mesh into the
// vertices (they were lost). The baked instances, reloads and off-thread thumbnails preserve that
// geometry, while foliage chunks bake the node's whole transform.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "System.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "buffer/ExternalBuffer.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include "json.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    // File (the glTF loader's reads) goes through System::instance(): S02's TestMain started
    // the engine with its TestSystem for the whole binary
    class GltfMeshOffset : public ::testing::Test {
    };

    // one triangle: positions (0,0,0) (1,0,0) (0,1,0), normals +Z, ushort indices
    std::vector<unsigned char> triangleBuffer() {
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        const uint16_t indices[4] = {0, 1, 2, 0}; // padded to 4 bytes
        std::vector<unsigned char> data(sizeof(positions) + sizeof(normals) + sizeof(indices));
        std::memcpy(data.data(), positions, sizeof(positions));
        std::memcpy(data.data() + sizeof(positions), normals, sizeof(normals));
        std::memcpy(data.data() + sizeof(positions) + sizeof(normals), indices, sizeof(indices));
        return data;
    }

    // a .gltf (+ .bin) with the triangle on the given nodes and scene roots
    // morph: the primitive also has a morph target (the normals' view as its POSITION
    // displacements), which a placed copy bakes in (an ordinary load rejects the bake)
    fs::path writeGltf(const std::string& name, const nlohmann::json& nodes, const nlohmann::json& roots, bool morph = false) {
        const fs::path dir = fs::temp_directory_path() / "jasperengine-editor-tests-gltf";
        fs::create_directories(dir);
        const std::vector<unsigned char> buffer = triangleBuffer();
        {
            std::ofstream bin(dir / (name + ".bin"), std::ios::binary);
            bin.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        }
        nlohmann::json gltf = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", {{{"nodes", roots}}}},
            {"nodes", nodes},
            {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"indices", 2}}}}}}},
            {"buffers", {{{"uri", name + ".bin"}, {"byteLength", buffer.size()}}}},
            {"bufferViews", {
                {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 6}, {"target", 34963}}}},
            {"accessors", {
                {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
                 {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
                {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                {{"bufferView", 2}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}}},
        };
        if (morph) {
            gltf["meshes"][0]["primitives"][0]["targets"] = {{{"POSITION", 1}}};
        }
        const fs::path path = dir / (name + ".gltf");
        std::ofstream(path) << gltf.dump(2);
        return path;
    }

    void expectVector(const Vector3& actual, const Vector3& expected, const std::string& what) {
        EXPECT_NEAR(actual.x, expected.x, 1e-4f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1e-4f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1e-4f) << what;
    }

    // the vertex positions the mesh's first submesh binds
    void expectPositions(const MeshComponent& mesh, const std::vector<Vector3>& expected) {
        ASSERT_GE(mesh.numSubmeshes, 1u);
        auto attribute = mesh.submeshes[0].attributes.find(AttributeType::POSITION);
        ASSERT_NE(attribute, mesh.submeshes[0].attributes.end());
        const ExternalBuffer* buffer = nullptr;
        for (unsigned int b = 0; b < mesh.numExternalBuffers; b++) {
            if (mesh.eBuffers[b].getName() == attribute->second.getBufferName())
                buffer = &mesh.eBuffers[b];
        }
        ASSERT_NE(buffer, nullptr) << attribute->second.getBufferName();
        const size_t stride = buffer->getStride() ? buffer->getStride() : 3 * sizeof(float);
        const size_t offset = attribute->second.getOffset();
        ASSERT_GE(buffer->getSize(), offset + (expected.size() - 1) * stride + 3 * sizeof(float));
        for (size_t i = 0; i < expected.size(); i++) {
            float v[3];
            std::memcpy(v, buffer->getData() + offset + i * stride, sizeof(v));
            expectVector(Vector3(v[0], v[1], v[2]), expected[i], "vertex " + std::to_string(i));
        }
    }

    const nlohmann::json nestedNodes = {
        {{"name", "Root"}, {"translation", {1, 0, 0}}, {"children", {1}}},
        {{"name", "Offset"}, {"translation", {0, 2, 0}}, {"scale", {2, 2, 2}}, {"children", {2}}},
        {{"name", "Mesh"}, {"translation", {0, 0, 3}}, {"mesh", 0}},
    };

}

TEST_F(GltfMeshOffset, NestedMeshNodeKeepsItsTransforms) {
    // Root (1,0,0) > Offset (0,2,0) scale 2 > Mesh (0,0,3)
    const nlohmann::json nodes = {
        {{"name", "Root"}, {"translation", {1, 0, 0}}, {"children", {1}}},
        {{"name", "Offset"}, {"translation", {0, 2, 0}}, {"scale", {2, 2, 2}}, {"children", {2}}},
        {{"name", "Mesh"}, {"translation", {0, 0, 3}}, {"mesh", 0}},
    };
    const fs::path path = writeGltf("nested", nodes, {0});

    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string()));

    // the entity is the scene root
    expectVector(scene.getComponent<Transform>(model.getEntity()).position, Vector3(1, 0, 0), "entity position");

    // Offset * Mesh baked: (2x, 2y + 2, 2z + 6)
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    expectPositions(mesh, {Vector3(0, 2, 6), Vector3(2, 2, 6), Vector3(0, 4, 6)});
    expectVector(mesh.verticesAABB.getMaximum(), Vector3(2, 4, 6), "AABB maximum");
    // the bounds hold the vertices only, not the mesh's origin
    expectVector(mesh.verticesAABB.getMinimum(), Vector3(0, 2, 6), "AABB minimum");
    EXPECT_EQ(mesh.windingOrder, WindingOrder::CCW);

    // the index view, the baked position and normal
    EXPECT_EQ(mesh.numExternalBuffers, 3u);
}

TEST_F(GltfMeshOffset, MeshAtTheRootIsNotBaked) {
    const nlohmann::json nodes = {
        {{"name", "Mesh"}, {"translation", {0, 0, 3}}, {"mesh", 0}},
    };
    const fs::path path = writeGltf("root", nodes, {0});

    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string()));

    expectVector(scene.getComponent<Transform>(model.getEntity()).position, Vector3(0, 0, 3), "entity position");
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    expectPositions(mesh, {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0)});

    // the file's three views
    EXPECT_EQ(mesh.numExternalBuffers, 3u);
}

TEST_F(GltfMeshOffset, MeshUnderAnotherSceneRoot) {
    // the entity takes the first root (an empty node); the mesh is the second root, mirrored
    const nlohmann::json nodes = {
        {{"name", "Empty"}, {"translation", {5, 0, 0}}},
        {{"name", "Mesh"}, {"translation", {0, 0, 3}}, {"scale", {-1, 1, 1}}, {"mesh", 0}},
    };
    const fs::path path = writeGltf("roots", nodes, {0, 1});

    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string()));

    expectVector(scene.getComponent<Transform>(model.getEntity()).position, Vector3(5, 0, 0), "entity position");
    // Empty^-1 * Mesh: (-x - 5, y, z + 3), so the world places it where the file does
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    expectPositions(mesh, {Vector3(-5, 0, 3), Vector3(-6, 0, 3), Vector3(-5, 1, 3)});
    expectVector(mesh.verticesAABB.getMinimum(), Vector3(-6, 0, 3), "AABB minimum");
    // the mirror flips the winding
    EXPECT_EQ(mesh.windingOrder, WindingOrder::CW);
    // it is in the vertices: the entity's transform does not mirror, nor does the draw
    EXPECT_FALSE(mesh.mirroredWinding);
    const Transform& transform = scene.getComponent<Transform>(model.getEntity());
    EXPECT_FALSE(RenderSystem::isMirroredDraw(mesh, Matrix4::translateMatrix(transform.position) *
        transform.rotation.getRotationMatrix() * Matrix4::scaleMatrix(transform.scale)));
}

TEST_F(GltfMeshOffset, MirroredRootOnTheEntityIsCompensated) {
    // the mesh at the root, mirrored: the mirror goes on the entity's transform,
    // the winding turns to CW for it, and the draw must not reverse it again
    const nlohmann::json nodes = {
        {{"name", "Mesh"}, {"translation", {0, 0, 3}}, {"scale", {-1, 1, 1}}, {"mesh", 0}},
    };
    const fs::path path = writeGltf("mirrored-root", nodes, {0});

    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string()));

    Transform& transform = scene.getComponent<Transform>(model.getEntity());
    // (decomposed as a turn and a scale whose product is negative)
    EXPECT_LT(transform.scale.x * transform.scale.y * transform.scale.z, 0.0f) << "the entity's scale mirrors";
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    expectPositions(mesh, {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0)});
    EXPECT_EQ(mesh.windingOrder, WindingOrder::CW);
    EXPECT_TRUE(mesh.mirroredWinding);
    Matrix4 placed = Matrix4::translateMatrix(transform.position) * transform.rotation.getRotationMatrix() * Matrix4::scaleMatrix(transform.scale);
    EXPECT_LT(placed.determinant(), 0.0f);
    EXPECT_FALSE(RenderSystem::isMirroredDraw(mesh, placed)) << "compensated by its winding";
    // mirrored back at runtime (scale +1): now the draw reverses the winding
    EXPECT_TRUE(RenderSystem::isMirroredDraw(mesh, Matrix4::translateMatrix(transform.position)));
}

TEST_F(GltfMeshOffset, PreviewLoadPreservesGeometryOfTheOriginal) {
    const fs::path path = writeGltf("offthread", nestedNodes, {0});
    Scene scene;
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();

    Model pooled(&scene);
    ASSERT_TRUE(meshSystem->loadGLTF(pooled.getEntity(), path.string()));

    // a thumbnail load (images decoded downscaled) parses its own copy, which it bakes
    Scene preview;
    Model thumbnail(&preview);
    // on a thread flagged async, as ProjectWindow's thumbnail worker (no AsyncThreadScope: its
    // end commits the render queue, which waits for a render thread this binary has not)
    bool loaded = false;
    std::thread worker([&]() {
        Engine::startAsyncThread();
        MeshSystem::setImageDecodeMaxDimension(256);
        loaded = preview.getSystem<MeshSystem>()->loadGLTF(thumbnail.getEntity(), path.string());
        MeshSystem::setImageDecodeMaxDimension(0);
    });
    worker.join();
    ASSERT_TRUE(loaded);
    expectPositions(scene.getComponent<MeshComponent>(pooled.getEntity()), {Vector3(0, 2, 6), Vector3(2, 2, 6), Vector3(0, 4, 6)});
    expectPositions(preview.getComponent<MeshComponent>(thumbnail.getEntity()), {Vector3(0, 2, 6), Vector3(2, 2, 6), Vector3(0, 4, 6)});
}

TEST_F(GltfMeshOffset, LoadingThreadPreservesBothGeometryModes) {
    // a game's loading thread (Engine.startAsyncThread, full-resolution images) and the main
    // thread load instances of one file at once: they share the pooled model, the main thread
    // its first load's bake, the loading thread (foliage chunks) one it appends meanwhile
    const fs::path path = writeGltf("loadingthread", nestedNodes, {0});

    Scene first;
    Model pooled(&first);
    ASSERT_TRUE(first.getSystem<MeshSystem>()->loadGLTF(pooled.getEntity(), path.string()));

    constexpr int count = 8;
    Scene workerScene;
    Scene mainScene;
    std::vector<std::unique_ptr<Model>> workerModels, mainModels;
    for (int i = 0; i < count; i++) {
        workerModels.push_back(std::make_unique<Model>(&workerScene));
        mainModels.push_back(std::make_unique<Model>(&mainScene));
    }
    bool workerLoaded = true;
    std::thread worker([&]() {
        Engine::startAsyncThread();
        for (auto& model : workerModels)
            workerLoaded = workerScene.getSystem<MeshSystem>()->loadGLTF(model->getEntity(), path.string(), false, true, false, true) && workerLoaded;
    });
    bool mainLoaded = true;
    for (auto& model : mainModels)
        mainLoaded = mainScene.getSystem<MeshSystem>()->loadGLTF(model->getEntity(), path.string()) && mainLoaded;
    worker.join();
    ASSERT_TRUE(workerLoaded);
    ASSERT_TRUE(mainLoaded);

    for (int i = 0; i < count; i++) {
        const MeshComponent& mesh = mainScene.getComponent<MeshComponent>(mainModels[i]->getEntity());
        expectPositions(mesh, {Vector3(0, 2, 6), Vector3(2, 2, 6), Vector3(0, 4, 6)});

        const MeshComponent& chunk = workerScene.getComponent<MeshComponent>(workerModels[i]->getEntity());
        expectPositions(chunk, {Vector3(1, 2, 6), Vector3(3, 2, 6), Vector3(1, 4, 6)});
    }
}

TEST_F(GltfMeshOffset, SceneLoadKeepsTheSavedRootTransform) {
    // a scene load (changeRootTransform off): the saved entity transform is the root's, so only
    // the nodes between the root and the mesh are baked, as on the first load
    const fs::path path = writeGltf("sceneload", nestedNodes, {0});
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string(), false, false, false));
    expectVector(scene.getComponent<Transform>(model.getEntity()).position, Vector3(0, 0, 0), "entity position");
    expectPositions(scene.getComponent<MeshComponent>(model.getEntity()), {Vector3(0, 2, 6), Vector3(2, 2, 6), Vector3(0, 4, 6)});
}

TEST_F(GltfMeshOffset, FoliageBakesTheWholeNodeTransform) {
    Scene scene;
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();

    // loadPlacedModel's load. Nested: Root * Offset * Mesh, (2x + 1, 2y + 2, 2z + 6), as merged nodes bake
    const fs::path nested = writeGltf("foliage-nested", nestedNodes, {0});
    Model chunk(&scene);
    ASSERT_TRUE(meshSystem->loadPlacedModel(chunk.getEntity(), nested.string()));
    expectVector(scene.getComponent<Transform>(chunk.getEntity()).position, Vector3(0, 0, 0), "chunk position");
    const MeshComponent& nestedMesh = scene.getComponent<MeshComponent>(chunk.getEntity());
    expectPositions(nestedMesh, {Vector3(1, 2, 6), Vector3(3, 2, 6), Vector3(1, 4, 6)});

    // the mesh at the root, mirrored: its own transform, and the winding it flips
    const nlohmann::json rootNodes = {
        {{"name", "Mesh"}, {"translation", {0, 0, 3}}, {"scale", {-1, 1, 1}}, {"mesh", 0}},
    };
    const fs::path root = writeGltf("foliage-root", rootNodes, {0});
    Model rootChunk(&scene);
    ASSERT_TRUE(meshSystem->loadPlacedModel(rootChunk.getEntity(), root.string()));
    const MeshComponent& rootMesh = scene.getComponent<MeshComponent>(rootChunk.getEntity());
    expectPositions(rootMesh, {Vector3(0, 0, 3), Vector3(-1, 0, 3), Vector3(0, 1, 3)});
    EXPECT_EQ(rootMesh.windingOrder, WindingOrder::CW);
    EXPECT_FALSE(rootMesh.mirroredWinding) << "the mirror is baked into the vertices";

    // a mesh with a blend shape bakes too: the copy draws it at rest, its default weights (none
    // here) in the vertices with the node's transform and its winding
    const fs::path morph = writeGltf("foliage-morph", rootNodes, {0}, true);
    Model morphChunk(&scene);
    ASSERT_TRUE(meshSystem->loadPlacedModel(morphChunk.getEntity(), morph.string()));
    const MeshComponent& morphMesh = scene.getComponent<MeshComponent>(morphChunk.getEntity());
    expectPositions(morphMesh, {Vector3(0, 0, 3), Vector3(-1, 0, 3), Vector3(0, 1, 3)});
    EXPECT_EQ(morphMesh.windingOrder, WindingOrder::CW);

    // a file without node transforms is not baked (and renders as before)
    const nlohmann::json plainNodes = {{{"name", "Mesh"}, {"mesh", 0}}};
    const fs::path plain = writeGltf("foliage-plain", plainNodes, {0});
    Model plainChunk(&scene);
    ASSERT_TRUE(meshSystem->loadPlacedModel(plainChunk.getEntity(), plain.string()));
    const MeshComponent& plainMesh = scene.getComponent<MeshComponent>(plainChunk.getEntity());
    expectPositions(plainMesh, {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0)});
    EXPECT_EQ(plainMesh.windingOrder, WindingOrder::CCW);
}

