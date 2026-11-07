// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A model made of parts (its glTF's mesh nodes loaded into child entities, Doriax cc550567a): its
// Mesh and Model API reaches the parts. Mesh-wide setters write every part (getters read the
// first), "submesh 0" calls act on each part's first submesh, a submesh index counts across the
// parts, morph weights resolve by each part's own target names. Before, every call changed the
// root's empty mesh, which draws nothing.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/Transform.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Color.h"

#include "support/TwoPartGltf.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    struct PartsModel {
        Scene scene;
        Model model{&scene};
        Entity body = NULL_ENTITY;
        Entity eyes = NULL_ENTITY;

        PartsModel() {
            const fs::path path = editor::test::writeTwoPartGltf(fs::temp_directory_path() / "jasperengine-editor-tests-parts", "parts");
            EXPECT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string()));
            const ModelComponent& component = scene.getComponent<ModelComponent>(model.getEntity());
            for (const auto& node : component.meshNodesMapping) {
                if (node.first == 0) body = node.second;
                if (node.first == 1) eyes = node.second;
            }
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                const Entity entity = transforms->getEntity(i);
                scene.getSystem<RenderSystem>()->updateTransform(scene.getComponent<Transform>(entity), entity);
            }
        }

        MeshComponent& mesh(Entity entity) { return scene.getComponent<MeshComponent>(entity); }
    };

}

TEST(ModelParts, LoadsAsAModelMadeOfParts) {
    PartsModel m;
    ASSERT_NE(m.body, NULL_ENTITY);
    ASSERT_NE(m.eyes, NULL_ENTITY);
    EXPECT_EQ(m.mesh(m.model.getEntity()).numSubmeshes, 0u);
    EXPECT_EQ(m.mesh(m.body).numSubmeshes, 2u);
    EXPECT_EQ(m.mesh(m.eyes).numSubmeshes, 1u);
    EXPECT_EQ(m.model.getModelParts(), (std::vector<Entity>{m.body, m.eyes}));
}

TEST(ModelParts, MeshLevelSettersReachEveryPart) {
    PartsModel m;
    m.model.setCastShadows(false);
    m.model.setReceiveLights(false);
    EXPECT_FALSE(m.mesh(m.body).castShadows);
    EXPECT_FALSE(m.mesh(m.eyes).castShadows);
    EXPECT_FALSE(m.mesh(m.eyes).receiveLights);
    EXPECT_FALSE(m.model.isCastShadows());

    m.mesh(m.body).lodBias = 3.0f;
    EXPECT_EQ(m.model.getLodBias(), 3.0f);  // the first part
}

TEST(ModelParts, UnindexedSubmeshCallsActOnEachPartsFirstSubmesh) {
    PartsModel m;
    const Vector4 before = m.mesh(m.body).submeshes[1].material.baseColorFactor;
    m.model.setColor(1, 0, 0, 1);
    const Vector4 red = Color::sRGBToLinear(Vector4(1, 0, 0, 1));
    EXPECT_EQ(m.mesh(m.body).submeshes[0].material.baseColorFactor, red);
    EXPECT_EQ(m.mesh(m.eyes).submeshes[0].material.baseColorFactor, red);
    EXPECT_EQ(m.mesh(m.body).submeshes[1].material.baseColorFactor, before);
    EXPECT_EQ(m.model.getColor(), Vector4(1, 0, 0, 1));
}

TEST(ModelParts, SubmeshIndexSpansParts) {
    PartsModel m;
    EXPECT_EQ(m.model.getNumSubmeshes(), 3u);
    Material material = m.model.getMaterial(2);
    material.metallicFactor = 0.25f;
    m.model.setMaterial(2, material);
    EXPECT_EQ(m.mesh(m.eyes).submeshes[0].material.metallicFactor, 0.25f);
    EXPECT_NE(m.mesh(m.body).submeshes[0].material.metallicFactor, 0.25f);
    EXPECT_EQ(m.model.getMaterial(2).metallicFactor, 0.25f);

    // past the last one: logged, nothing changes
    material.metallicFactor = 0.75f;
    m.model.setMaterial(3, material);
    EXPECT_NE(m.mesh(m.eyes).submeshes[0].material.metallicFactor, 0.75f);
    EXPECT_NE(m.mesh(m.body).submeshes[1].material.metallicFactor, 0.75f);
}

TEST(ModelParts, MorphWeightsResolvePerPart) {
    PartsModel m;
    m.model.setMorphWeight("Blink", 1.0f);
    EXPECT_EQ(m.mesh(m.body).morphWeights[1], 1.0f);  // Blink is the body's second target
    EXPECT_EQ(m.mesh(m.eyes).morphWeights[0], 1.0f);  // and the eyes' first
    EXPECT_EQ(m.model.getMorphWeight("Blink"), 1.0f);

    m.model.setMorphWeight("Smile", 0.5f);
    EXPECT_EQ(m.mesh(m.body).morphWeights[0], 0.5f);
    EXPECT_EQ(m.mesh(m.eyes).morphWeights[0], 1.0f);  // the eyes have no Smile
    EXPECT_TRUE(m.mesh(m.body).morphWeightsSet);
}

TEST(ModelParts, BoundsAndVertexCountMergeParts) {
    PartsModel m;
    const AABB aabb = m.model.getAABB();
    EXPECT_TRUE(aabb.contains(Vector3(0.1f, 0.1f, 0.0f)));
    EXPECT_TRUE(aabb.contains(Vector3(2.9f, 0.05f, 0.0f)));  // the eyes, moved by 2 on x
    EXPECT_EQ(m.model.getVertexCount(), m.mesh(m.body).vertexCount + m.mesh(m.eyes).vertexCount);
}

TEST(ModelParts, InstancingRefusedOnPartsModel) {
    PartsModel m;
    // each refuses without throwing: they made the instanced mesh first, then read it
    EXPECT_NO_THROW(m.model.createInstancedMesh());
    EXPECT_NO_THROW(m.model.setInstancedBillboard(true));
    EXPECT_NO_THROW(m.model.setInstancedBillboard(true, true));
    EXPECT_NO_THROW(m.model.setInstancedCylindricalBillboard(true));
    EXPECT_NO_THROW(m.model.addInstance(Vector3(1, 2, 3)));
    EXPECT_NO_THROW(m.model.updateInstance(0, InstanceData()));
    EXPECT_NO_THROW(m.model.setAsMirror());
    EXPECT_EQ(m.scene.findComponent<InstancedMeshComponent>(m.model.getEntity()), nullptr);
    // logged once per model, not per call
    EXPECT_TRUE(m.scene.getComponent<ModelComponent>(m.model.getEntity()).refusedPartsCallLogged);
}

TEST(ModelParts, SingleMeshModelUnchanged) {
    Scene scene;
    Model model(&scene);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    mesh.numSubmeshes = 2;
    model.setCastShadows(false);
    model.setColor(0, 1, 0, 1);
    EXPECT_FALSE(mesh.castShadows);
    EXPECT_EQ(mesh.submeshes[0].material.baseColorFactor, Color::sRGBToLinear(Vector4(0, 1, 0, 1)));
    EXPECT_EQ(model.getNumSubmeshes(), 2u);
    EXPECT_TRUE(model.getModelParts().empty());
    model.setMorphWeight(1, 0.5f);
    EXPECT_EQ(mesh.morphWeights[1], 0.5f);
}
