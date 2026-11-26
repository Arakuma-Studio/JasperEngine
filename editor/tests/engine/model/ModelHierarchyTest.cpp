// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Preserve Hierarchy (spec S94 F44, R4/R52): a model whose file holds one static mesh node loads
// it into its own GameObject (the root's Mesh stays empty) when ModelComponent::preserveHierarchy
// is on, as one with clips or several skins already does; off, the loader bakes the lone mesh
// into the root as before.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include "support/ModelFixtures.h"

#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;
using GJson = nlohmann::ordered_json;

namespace {

    // Root > Body: one triangle mesh, the body a metre up
    fs::path writeSingleMeshModel(const std::string& name) {
        GltfBuild build;
        const std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        GJson primitive = {{"attributes", {{"POSITION", build.addFloats(positions, 3, 34962, true)}}},
                           {"indices", build.addIndices({0, 1, 2}, 5123)}};
        build.json["meshes"].push_back({{"name", "Body"}, {"primitives", {primitive}}});
        build.json["nodes"] = GJson::array({{{"name", "Root"}, {"children", {1}}},
                                            {{"name", "Body"}, {"mesh", 0}, {"translation", {0, 1, 0}}}});
        build.json["scenes"] = GJson::array({{{"nodes", {0}}}});
        build.json["scene"] = 0;
        const fs::path dir = fs::temp_directory_path() / "jasperengine-editor-tests-hierarchy";
        fs::create_directories(dir);
        const fs::path file = dir / (name + ".glb");
        const std::vector<uint8_t> bytes = build.glb();
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return file;
    }

}

TEST(ModelHierarchyTest, PreserveHierarchySingleMesh) {
    const fs::path file = writeSingleMeshModel("single");
    {
        // the loader's own rule: the lone static mesh is baked into the root
        Scene scene;
        Model model(&scene);
        ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), file.string()));
        EXPECT_EQ(scene.getComponent<MeshComponent>(model.getEntity()).numSubmeshes, 1u);
        EXPECT_TRUE(scene.getComponent<ModelComponent>(model.getEntity()).meshNodesMapping.empty());
    }
    Scene scene;
    Model model(&scene);
    model.setPreserveHierarchy(true);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), file.string()));
    const ModelComponent& component = scene.getComponent<ModelComponent>(model.getEntity());
    EXPECT_EQ(scene.getComponent<MeshComponent>(model.getEntity()).numSubmeshes, 0u) << "the root draws nothing";
    EXPECT_EQ(component.nodesIdMapping.size(), 2u) << "every glTF node has an entity";
    const auto body = component.meshNodesMapping.find(1);
    ASSERT_NE(body, component.meshNodesMapping.end());
    const MeshComponent* mesh = scene.findComponent<MeshComponent>(body->second);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->numSubmeshes, 1u) << "the Body node's own GameObject draws it";
    EXPECT_EQ(scene.getEntityName(body->second), "Body");
    EXPECT_NEAR(scene.getComponent<Transform>(body->second).position.y, 1.0f, 1e-5f) << "the node keeps its transform";
}
