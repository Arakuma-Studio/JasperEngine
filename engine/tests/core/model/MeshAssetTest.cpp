// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .mesh asset (docs/specs/S98-editor-tooling-gaps.md F1): what a file holds reads back as the
// geometry it was written from, MeshSystem reads the file a Mesh Renderer names, and a generated
// primitive lets go of it.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "model/MeshAsset.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "subsystem/MeshSystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    std::span<const uint8_t> view(const std::vector<uint8_t>& bytes) {
        return std::span<const uint8_t>(bytes.data(), bytes.size());
    }

    bool sameBytes(const Buffer& a, const Buffer& b) {
        return a.getSize() == b.getSize() && a.getCount() == b.getCount() && a.getStride() == b.getStride() &&
               (a.getSize() == 0 || std::memcmp(a.getData(), b.getData(), a.getSize()) == 0);
    }

    struct TempDir {
        fs::path path;
        TempDir() {
            static std::atomic<int> counter{0};
            path = fs::temp_directory_path() / ("je-meshasset-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                                std::to_string(counter++));
            fs::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

}

TEST(MeshAsset, RoundTripsTheGeometry) {
    Scene scene;
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 12, 8);
    const MeshComponent& source = sphere.getComponent<MeshComponent>();
    ASSERT_GT(MeshAsset::geometryBytes(source), 0u);

    const std::vector<uint8_t> bytes = MeshAsset::encode(source);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(std::memcmp(bytes.data(), "JMSH", 4), 0);

    MeshComponent read;
    std::string error;
    ASSERT_TRUE(MeshAsset::decode(view(bytes), read, &error)) << error;
    EXPECT_TRUE(sameBytes(read.buffer, source.buffer));
    EXPECT_TRUE(sameBytes(read.indices, source.indices));
    EXPECT_EQ(read.buffer.getVertexSize(), source.buffer.getVertexSize());
    EXPECT_EQ(read.buffer.getAttributes().size(), source.buffer.getAttributes().size());
    EXPECT_EQ(read.numSubmeshes, source.numSubmeshes);
    EXPECT_EQ(read.submeshes[0].primitiveType, source.submeshes[0].primitiveType);
    EXPECT_EQ(read.verticesAABB.getMinimum(), source.verticesAABB.getMinimum());
    EXPECT_EQ(read.verticesAABB.getMaximum(), source.verticesAABB.getMaximum());
    // the same geometry gives the same bytes (a scene save finds the file it wrote before)
    EXPECT_EQ(MeshAsset::encode(read), bytes);

    MeshAsset::Info info;
    ASSERT_TRUE(MeshAsset::inspect(view(bytes), info, &error)) << error;
    EXPECT_EQ(info.major, MeshAsset::FORMAT_MAJOR);
    EXPECT_EQ(info.indexCount, source.indices.getCount());
    EXPECT_EQ(info.geometryBytes, MeshAsset::geometryBytes(source));
    ASSERT_EQ(info.submeshes.size(), 1u);
    EXPECT_FALSE(info.attributes.empty());
}

TEST(MeshAsset, RefusesWhatIsNotOne) {
    Scene scene;
    Primitive box(&scene);
    box.createBox(1, 1, 1);
    const std::vector<uint8_t> bytes = MeshAsset::encode(box.getComponent<MeshComponent>());

    MeshComponent kept;
    std::string error;
    std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
    EXPECT_FALSE(MeshAsset::decode(view(cut), kept, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(kept.buffer.getSize(), 0u) << "a refused file leaves the mesh as it was";

    std::vector<uint8_t> magic = bytes;
    magic[0] = 'X';
    EXPECT_FALSE(MeshAsset::decode(view(magic), kept, &error));

    std::vector<uint8_t> major = bytes;
    major[4] = static_cast<uint8_t>(MeshAsset::FORMAT_MAJOR + 1);
    EXPECT_FALSE(MeshAsset::decode(view(major), kept, &error));
    EXPECT_NE(error.find(".mesh format"), std::string::npos) << error;
}

TEST(MeshAsset, MeshSystemReadsTheNamedFile) {
    TempDir dir;
    Scene scene;
    Primitive source(&scene);
    source.createTorus(1.0f, 0.25f, 16, 12);
    const fs::path file = dir.path / "ring.mesh";
    std::string error;
    ASSERT_TRUE(MeshAsset::save(file, source.getComponent<MeshComponent>(), &error)) << error;

    Mesh target(&scene);
    MeshComponent& mesh = target.getComponent<MeshComponent>();
    mesh.meshAsset = file.generic_string();
    ASSERT_EQ(mesh.buffer.getSize(), 0u);
    scene.getSystem<MeshSystem>()->update(0);
    EXPECT_TRUE(sameBytes(mesh.buffer, source.getComponent<MeshComponent>().buffer));
    EXPECT_TRUE(sameBytes(mesh.indices, source.getComponent<MeshComponent>().indices));
    EXPECT_EQ(mesh.loadedMeshAsset, file.generic_string());
    EXPECT_NE(mesh.aabb, AABB::ZERO) << "the bounds come with the geometry";

    // a file another program changed: read again once invalidated
    Primitive other(&scene);
    other.createBox(2, 2, 2);
    ASSERT_TRUE(MeshAsset::save(file, other.getComponent<MeshComponent>(), &error)) << error;
    scene.getSystem<MeshSystem>()->update(0);
    EXPECT_FALSE(sameBytes(mesh.buffer, other.getComponent<MeshComponent>().buffer)) << "read once per path";
    scene.getSystem<MeshSystem>()->invalidateMeshAsset(file.generic_string());
    scene.getSystem<MeshSystem>()->update(0);
    EXPECT_TRUE(sameBytes(mesh.buffer, other.getComponent<MeshComponent>().buffer));

    // a generated primitive replaces the geometry: the mesh no longer shows the file
    Primitive primitive(&scene, target.getEntity());
    primitive.createPlane(1, 1);
    EXPECT_TRUE(mesh.meshAsset.empty());
    EXPECT_TRUE(mesh.loadedMeshAsset.empty());
}

TEST(MeshAsset, ScriptsAssignOneAndReadItAtOnce) {
    TempDir dir;
    Scene scene;
    Primitive source(&scene);
    source.createCylinder(0.5f, 1.0f, 12, 2);
    const fs::path file = dir.path / "pillar.mesh";
    std::string error;
    ASSERT_TRUE(MeshAsset::save(file, source.getComponent<MeshComponent>(), &error)) << error;

    // the Lua property's setter (Mesh.meshAsset)
    Mesh mesh(&scene);
    mesh.setMeshAsset(file.generic_string());
    EXPECT_EQ(mesh.getMeshAsset(), file.generic_string());
    EXPECT_TRUE(sameBytes(mesh.getComponent<MeshComponent>().buffer, source.getComponent<MeshComponent>().buffer));

    // a file that is not there: logged, the geometry kept
    mesh.setMeshAsset((dir.path / "missing.mesh").generic_string());
    EXPECT_TRUE(sameBytes(mesh.getComponent<MeshComponent>().buffer, source.getComponent<MeshComponent>().buffer));
}
