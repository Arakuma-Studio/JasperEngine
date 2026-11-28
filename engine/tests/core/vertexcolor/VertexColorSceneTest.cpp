// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R1, R2, R9, D12: the targets of a scene's entities
// (subsystem/VertexColorScene.h) on a primitive, a multi-node model with COLOR_0, the same model
// merged and a single-node model, their mesh views and fingerprints. CPU only.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "component/VertexColorsComponent.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VertexColorRenderer.h"
#include "subsystem/VertexColorScene.h"

#include "VertexColorSceneFixture.h"

#include <gtest/gtest.h>

namespace VertexColorSceneTest_cpp {

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_scene_test {
    using namespace jasperengine::vertexcolor;

    TargetKey nodeKey(int node, int primitive) {
        TargetKey key;
        key.node = node;
        key.primitive = primitive;
        return key;
    }

    TargetKey submeshKey(int submesh) {
        TargetKey key;
        key.submesh = submesh;
        return key;
    }

    // node 0 "Rock": one plain 2x2 grid; node 1 "Moss": two grids with COLOR_0, apart in x
    std::vector<vctest::GltfNode> twoNodes() {
        vctest::GltfNode rock{"Rock", {vctest::GridPrimitive{}}, 0.0f};
        vctest::GridPrimitive left;
        left.cells = 3;
        left.x = -2.0f;
        left.colors = true;
        vctest::GridPrimitive right;
        right.cells = 1;
        right.x = 2.0f;
        right.colors = true;
        vctest::GltfNode moss{"Moss", {left, right}, 5.0f};
        return {rock, moss};
    }

TEST(VertexColorScene, PrimitiveHasOneSubmeshTarget) {
    Scene scene;
    Primitive box(&scene);
    box.createBox(1.0f, 1.0f, 1.0f);
    EXPECT_EQ(paintRefusal(&scene, box.getEntity()), "");

    const std::vector<SceneTarget> targets = sceneTargets(&scene, box.getEntity());
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].key, submeshKey(0));
    EXPECT_EQ(targets[0].name, "Submesh 0");
    EXPECT_EQ(targets[0].meshEntity, box.getEntity());
    EXPECT_EQ(targets[0].submesh, 0u);
    EXPECT_EQ(targets[0].vertexCount, 24u) << "a GPU vertex per face corner";
    EXPECT_EQ(targets[0].triangleCount, 12u);
    EXPECT_TRUE(targets[0].hasAssetColors) << "primitives carry white COLOR";

    const std::shared_ptr<const MeshView> view = sceneMeshView(&scene, box.getEntity(), submeshKey(0));
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->built());
    EXPECT_EQ(view->vertexCount(), 24u);
    EXPECT_EQ(view->triangleCount(), 12u);
    EXPECT_EQ(view->logicalCount(), 8u) << "the box's corners";
    ASSERT_EQ(view->assetColors.size(), 24u);
    EXPECT_EQ(view->assetColors[0], Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    EXPECT_EQ(view->normals.size(), 24u);
    EXPECT_EQ(view->uv0.size(), 24u);

    // cached per mesh load: the same view until the geometry changes
    EXPECT_EQ(sceneMeshView(&scene, box.getEntity(), submeshKey(0)).get(), view.get());
    box.createBox(2.0f, 1.0f, 1.0f);
    const std::shared_ptr<const MeshView> rebuilt = sceneMeshView(&scene, box.getEntity(), submeshKey(0));
    ASSERT_TRUE(rebuilt);
    EXPECT_NE(rebuilt.get(), view.get());
    EXPECT_FLOAT_EQ(rebuilt->bounds.getMaximum().x - rebuilt->bounds.getMinimum().x, 2.0f);

    EXPECT_FALSE(sceneMeshView(&scene, box.getEntity(), submeshKey(1))) << "no such target";
}

TEST(VertexColorScene, ModelTargetsAreNodeAndPrimitive) {
    jasperengine::test::TempDir dir;
    const std::string file = vctest::writeGridModel(dir, "nodes", twoNodes());
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(file));
    const ModelComponent& component = model.getComponent<ModelComponent>();
    ASSERT_EQ(component.meshNodesMapping.size(), 2u) << "two mesh nodes: a child entity each";
    const Entity rock = component.meshNodesMapping.at(0);
    const Entity moss = component.meshNodesMapping.at(1);

    const std::vector<SceneTarget> targets = sceneTargets(&scene, model.getEntity());
    ASSERT_EQ(targets.size(), 3u);
    EXPECT_EQ(targets[0].key, nodeKey(0, 0));
    EXPECT_EQ(targets[0].meshEntity, rock);
    EXPECT_EQ(targets[0].name, "Rock (node 0, primitive 0)");
    EXPECT_FALSE(targets[0].hasAssetColors);
    EXPECT_EQ(targets[0].vertexCount, 9u);
    EXPECT_EQ(targets[1].key, nodeKey(1, 0));
    EXPECT_EQ(targets[1].meshEntity, moss);
    // the loader reverses a node's submeshes: primitive 0 of two is submesh 1
    EXPECT_EQ(targets[1].submesh, 1u);
    EXPECT_EQ(targets[1].vertexCount, 16u);
    EXPECT_EQ(targets[1].triangleCount, 18u);
    EXPECT_TRUE(targets[1].hasAssetColors);
    EXPECT_EQ(targets[2].key, nodeKey(1, 1));
    EXPECT_EQ(targets[2].submesh, 0u);
    EXPECT_EQ(targets[2].vertexCount, 4u);

    // the view is the primitive's own geometry in its mesh's space (bind pose), its COLOR_0 linear
    const std::shared_ptr<const MeshView> left = sceneMeshView(&scene, model.getEntity(), nodeKey(1, 0));
    ASSERT_TRUE(left);
    EXPECT_EQ(left->vertexCount(), 16u);
    EXPECT_NEAR(left->bounds.getCenter().x, -2.0f, 1e-5f);
    ASSERT_EQ(left->assetColors.size(), 16u);
    EXPECT_EQ(left->assetColors[0], Vector4(0.0f, 0.0f, 0.25f, 1.0f));
    EXPECT_EQ(left->assetColors[15], Vector4(1.0f, 1.0f, 0.25f, 1.0f));
    const std::shared_ptr<const MeshView> plain = sceneMeshView(&scene, model.getEntity(), nodeKey(0, 0));
    ASSERT_TRUE(plain);
    EXPECT_TRUE(plain->assetColors.empty());

    // D12: the target's local -> world is its mesh entity's
    Transform& mossTransform = scene.getComponent<Transform>(moss);
    mossTransform.modelMatrix = Matrix4::translateMatrix(5.0f, 1.0f, 0.0f);
    EXPECT_EQ(sceneTargetModel(&scene, model.getEntity(), nodeKey(1, 1)), mossTransform.modelMatrix);

    // R1: the mesh nodes are refused, the Model's GameObject is not
    EXPECT_EQ(paintRefusal(&scene, model.getEntity()), "");
    EXPECT_NE(paintRefusal(&scene, moss).find("not on one of its mesh nodes"), std::string::npos);
    EXPECT_TRUE(sceneTargets(&scene, moss).empty());
}

TEST(VertexColorScene, MergedModelTargetsAreSubmeshes) {
    jasperengine::test::TempDir dir;
    const std::string file = vctest::writeGridModel(dir, "merged", twoNodes());
    Scene scene;
    Model model(&scene);
    model.getComponent<ModelComponent>().mergeStaticMeshes = true;
    ASSERT_TRUE(model.loadModel(file));
    ASSERT_TRUE(model.getComponent<ModelComponent>().meshNodesMapping.empty());
    const std::vector<SceneTarget> targets = sceneTargets(&scene, model.getEntity());
    ASSERT_EQ(targets.size(), 3u);
    for (unsigned int s = 0; s < 3; s++) {
        EXPECT_EQ(targets[s].key, submeshKey((int)s));
        EXPECT_EQ(targets[s].submesh, s);
        EXPECT_EQ(targets[s].meshEntity, model.getEntity());
    }
}

TEST(VertexColorScene, SingleNodeModelTargetsAreItsPrimitives) {
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel("gltf/two_primitive_counts.gltf"));
    const std::vector<SceneTarget> targets = sceneTargets(&scene, model.getEntity());
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].key, nodeKey(0, 0));
    EXPECT_EQ(targets[0].name, "TwoCounts (node 0, primitive 0)");
    EXPECT_EQ(targets[0].triangleCount, 1u) << "the file's first primitive: 3 indices";
    EXPECT_EQ(targets[1].key, nodeKey(0, 1));
    EXPECT_EQ(targets[1].triangleCount, 2u);
}

TEST(VertexColorScene, RefusesGeneratedMeshes) {
    Scene scene;
    const Entity empty = scene.createEntity();
    EXPECT_EQ(paintRefusal(&scene, empty), "Vertex Colors go on a GameObject with a Model or a Mesh (add one first).");
    const Entity sprite = scene.createEntity();
    scene.addComponent<Transform>(sprite);
    scene.addComponent<MeshComponent>(sprite);
    scene.addComponent<SpriteComponent>(sprite);
    EXPECT_EQ(paintRefusal(&scene, sprite), "Vertex Colors cannot paint the mesh a Sprite Renderer makes: the Sprite Renderer "
                                             "rebuilds it with its own colors. Paint a Model or a Mesh primitive.");
    EXPECT_TRUE(sceneTargets(&scene, sprite).empty());
}

TEST(VertexColorScene, FingerprintsAndChangeMarks) {
    Scene scene;
    Primitive box(&scene);
    box.createBox(1.0f, 1.0f, 1.0f);
    const Entity entity = box.getEntity();
    EXPECT_FALSE(targetMatchesMesh(&scene, entity, submeshKey(0), true)) << "no component";
    VertexColorsComponent colors;
    colors.loaded = std::make_shared<vertexcolor::Data>();
    scene.addComponent<VertexColorsComponent>(entity, colors);
    EXPECT_FALSE(targetMatchesMesh(&scene, entity, submeshKey(0), true)) << "no stored target";

    const std::shared_ptr<const MeshView> view = sceneMeshView(&scene, entity, submeshKey(0));
    ASSERT_TRUE(view);
    VertexColorsComponent& component = scene.getComponent<VertexColorsComponent>(entity);
    TargetData& target = component.loaded->add(submeshKey(0));
    target.fingerprint = view->fingerprint();
    EXPECT_TRUE(targetMatchesMesh(&scene, entity, submeshKey(0), true));
    EXPECT_TRUE(targetMatchesMesh(&scene, entity, submeshKey(0), false));
    // positions moved, topology kept: stale for the editor only (R9)
    target.fingerprint.positionHash ^= 1;
    EXPECT_TRUE(targetMatchesMesh(&scene, entity, submeshKey(0), true));
    EXPECT_FALSE(targetMatchesMesh(&scene, entity, submeshKey(0), false));
    target.fingerprint.indexCount += 3;
    EXPECT_FALSE(targetMatchesMesh(&scene, entity, submeshKey(0), true));

    const uint64_t revision = component.revision;
    markVertexColorsChanged(&scene, entity);
    EXPECT_EQ(component.revision, revision + 1);
    EXPECT_EQ(boundSplitVertices(&scene, entity, submeshKey(0)), 0u) << "nothing bound without a view";
}

} // namespace vertex_color_scene_test

}  // namespace VertexColorSceneTest_cpp
