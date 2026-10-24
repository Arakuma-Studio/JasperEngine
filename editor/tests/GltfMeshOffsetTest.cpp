// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A glTF whose only mesh node sits under transform nodes: MeshSystem::loadGLTF gives the entity
// the scene root's transform and bakes the nodes between the root and the mesh into the
// vertices (they were lost), and Project::countGltfExternalBuffers counts, from the file, the
// external buffers that load gives the mesh (a command line export loads no model).

#include <gtest/gtest.h>

#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "System.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "buffer/ExternalBuffer.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    // File (the glTF loader's reads) goes through System::instance(), which the editor's
    // Platform provides: a bare one, set once for the test binary
    class FileSystemOnly : public System {
    public:
        int getScreenWidth() override { return 1000; }
        int getScreenHeight() override { return 480; }
    };

    class GltfMeshOffset : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            static FileSystemOnly* system = nullptr;
            if (!system) {
                system = new FileSystemOnly();
                Engine::systemInit(0, nullptr, system);
            }
        }
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
    fs::path writeGltf(const std::string& name, const nlohmann::json& nodes, const nlohmann::json& roots) {
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
        const fs::path path = dir / (name + ".gltf");
        std::ofstream(path) << gltf.dump(2);
        return path;
    }

    void expectVector(const Vector3& actual, const Vector3& expected, const std::string& what) {
        EXPECT_NEAR(actual.x, expected.x, 1e-4f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1e-4f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1e-4f) << what;
    }

    // the vertex positions the mesh's first submesh binds (the AABB also holds the origin)
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
    EXPECT_EQ(mesh.windingOrder, WindingOrder::CCW);

    // the file count covers what the load gave the mesh (index view, baked position and normal)
    EXPECT_EQ(mesh.numExternalBuffers, 3u);
    EXPECT_GE(editor::Project::countGltfExternalBuffers(path, false), mesh.numExternalBuffers);
    EXPECT_GE(editor::Project::countGltfExternalBuffers(path, true), mesh.numExternalBuffers);
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
    EXPECT_EQ(editor::Project::countGltfExternalBuffers(path, false), 3u);
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
    expectVector(mesh.verticesAABB.getMinimum(), Vector3(-6, 0, 0), "AABB minimum");
    // the mirror flips the winding
    EXPECT_EQ(mesh.windingOrder, WindingOrder::CW);
    EXPECT_GE(editor::Project::countGltfExternalBuffers(path, false), mesh.numExternalBuffers);
}

TEST(GltfExternalBuffers, UnreadableFileCountsNothing) {
    EXPECT_EQ(editor::Project::countGltfExternalBuffers(fs::temp_directory_path() / "jasperengine-no-such-model.gltf", false), 0u);
}

