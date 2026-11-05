// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The glTF extensions of optimised files beyond geometry compression (docs/specs/S53-model-import-cook.md
// R1, R3, R4, R6; AC3), on the fixtures of fixtures/gltf/s41 (README.md there):
// EXT_mesh_gpu_instancing draws through an InstancedMeshComponent, MSFT_lod loads its highest
// level only, KHR_texture_basisu's KTX2 image is the texture, KHR_texture_transform is baked
// into the UVs, and the cook's JE_mesh_lods passes through. Games have no Draco or WebP
// decoder: those files are errors that name the extension and say the cook transcodes it (the
// editor decodes them: editor/tests/util/GltfEditorDecodersTest.cpp). CPU only.

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include "GltfTriangleSoup.h"
#include "LogCapture.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::vector<Entity> meshEntities(Scene& scene, const Model& model){
        std::vector<Entity> entities = {model.getEntity()};
        for (const auto& [node, entity] : scene.getComponent<ModelComponent>(model.getEntity()).meshNodesMapping)
            entities.push_back(entity);
        return entities;
    }

    struct Box {
        double min[3] = {1e30, 1e30, 1e30};
        double max[3] = {-1e30, -1e30, -1e30};
    };

    // the box around `positions` (xyz triples) through m (column-major)
    Box transformedBox(const std::vector<double>& positions, const SoupMatrix& m){
        Box box;
        for (size_t v = 0; v + 2 < positions.size(); v += 3){
            for (int r = 0; r < 3; r++){
                const double p = m[r] * positions[v] + m[4 + r] * positions[v + 1] + m[8 + r] * positions[v + 2] + m[12 + r];
                box.min[r] = std::min(box.min[r], p);
                box.max[r] = std::max(box.max[r], p);
            }
        }
        return box;
    }

    bool sameBox(const Box& a, const Box& b, double tolerance){
        for (int r = 0; r < 3; r++){
            if (std::fabs(a.min[r] - b.min[r]) > tolerance || std::fabs(a.max[r] - b.max[r]) > tolerance)
                return false;
        }
        return true;
    }

    // the pixels of a loaded texture (a decode on a job, polled as a draw would)
    bool texturePixels(Texture texture, std::vector<uint8_t>& rgba, unsigned int& width, unsigned int& height){
        TextureLoadResult result = texture.load();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (result.state == ResourceLoadState::Loading && std::chrono::steady_clock::now() < until){
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            result = texture.load();
        }
        if (result.state != ResourceLoadState::Finished || !texture.hasData() || !texture.getData().getData())
            return false;
        width = texture.getWidth();
        height = texture.getHeight();
        const uint8_t* data = static_cast<const uint8_t*>(texture.getData().getData());
        rgba.assign(data, data + (size_t)width * height * 4);
        return true;
    }

}

TEST(GltfExtensionsTest, InstancingDrawsEveryInstance){
    // gltfpack -mi made one node with six instances of the box the source has under six nodes
    tinygltf::Model source;
    std::string error;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(s41Fixture("instances_source.glb"), source, &error)) << error;
    std::vector<double> sourcePositions;
    int components = 0;
    ASSERT_TRUE(soupRead(source, source.meshes[0].primitives[0].attributes.at("POSITION"), sourcePositions, components));
    std::vector<Box> expected;
    for (const tinygltf::Node& node : source.nodes)
        expected.push_back(transformedBox(sourcePositions, soupNodeMatrix(node)));

    Scene scene;
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(s41Fixture("instancing.glb")));
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
    const InstancedMeshComponent* instanced = nullptr;
    Entity instancedEntity = NULL_ENTITY;
    for (Entity entity : meshEntities(scene, model)){
        if (const InstancedMeshComponent* found = scene.findComponent<InstancedMeshComponent>(entity)){
            instanced = found;
            instancedEntity = entity;
        }
    }
    ASSERT_NE(instanced, nullptr) << "the instanced node has no InstancedMeshComponent";
    ASSERT_EQ(instanced->instances.size(), expected.size());
    EXPECT_GE(instanced->maxInstances, expected.size());

    // each instance places the (quantized) box where a source node placed it
    const ModelComponent& component = scene.getComponent<ModelComponent>(model.getEntity());
    const tinygltf::Model& packed = *component.gltfModel;
    std::vector<double> packedPositions;
    ASSERT_TRUE(soupRead(packed, packed.meshes[0].primitives[0].attributes.at("POSITION"), packedPositions, components));
    const Transform& transform = scene.getComponent<Transform>(instancedEntity);
    EXPECT_EQ(transform.position, Vector3(0, 0, 0)); // gltfpack's instanced node has no transform
    std::vector<bool> matched(expected.size(), false);
    for (const InstanceData& instance : instanced->instances){
        tinygltf::Node node;
        node.translation = {instance.position.x, instance.position.y, instance.position.z};
        node.rotation = {instance.rotation.x, instance.rotation.y, instance.rotation.z, instance.rotation.w};
        node.scale = {instance.scale.x, instance.scale.y, instance.scale.z};
        const Box box = transformedBox(packedPositions, soupNodeMatrix(node));
        for (size_t e = 0; e < expected.size(); e++){
            if (!matched[e] && sameBox(box, expected[e], 2e-3)){
                matched[e] = true;
                break;
            }
        }
    }
    for (size_t e = 0; e < expected.size(); e++)
        EXPECT_TRUE(matched[e]) << "no instance where source node " << e << " is";
}

TEST(GltfExtensionsTest, MsftLodLoadsTheHighestLevel){
    // three levels of a sphere: 16 x 10 (960 indices) with MSFT_lod naming 10 x 6 and 5 x 3
    Scene scene;
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(s41Fixture("msft_lod.glb")));
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
    EXPECT_EQ(log.count(D_LOG_WARN, "MSFT_lod"), 0u);
    unsigned int submeshes = 0;
    unsigned int indices = 0;
    for (Entity entity : meshEntities(scene, model)){
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            submeshes++;
            indices += mesh.submeshes[s].vertexCount;
        }
    }
    EXPECT_EQ(submeshes, 1u) << "the lower levels draw too";
    EXPECT_EQ(indices, 960u);
}

TEST(GltfExtensionsTest, BasisTextureIsTheSource){
    // gltfpack -tc: the texture's only source is KHR_texture_basisu's KTX2 (ETC1S) image of the
    // 16 x 16 checker (orange 230 120 40, blue 40 90 200 in 2 x 2 cells)
    Scene scene;
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(s41Fixture("basisu.glb")));
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
    const Texture* texture = nullptr;
    for (Entity entity : meshEntities(scene, model)){
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            if (!mesh.submeshes[s].material.baseColorTexture.getId().empty())
                texture = &mesh.submeshes[s].material.baseColorTexture;
        }
    }
    ASSERT_NE(texture, nullptr) << "the ball draws untextured";
    std::vector<uint8_t> rgba;
    unsigned int width = 0, height = 0;
    ASSERT_TRUE(texturePixels(*texture, rgba, width, height));
    ASSERT_EQ(width, 16u);
    ASSERT_EQ(height, 16u);
    // ETC1S codes a 4 x 4 block with one colour line, so the 2 x 2 cells of a block blend: each
    // block's mean is the checker's (135 105 120) within the codec's error, never white or black
    for (unsigned int by = 0; by < height; by += 4){
        for (unsigned int bx = 0; bx < width; bx += 4){
            double mean[3] = {0, 0, 0};
            for (unsigned int y = by; y < by + 4; y++)
                for (unsigned int x = bx; x < bx + 4; x++)
                    for (int c = 0; c < 3; c++)
                        mean[c] += rgba[((size_t)y * width + x) * 4 + c] / 16.0;
            const double expected[3] = {135, 105, 120};
            for (int c = 0; c < 3; c++)
                EXPECT_NEAR(mean[c], expected[c], 30) << "block " << bx << ", " << by << " channel " << c;
        }
    }
}

namespace {

    // the glTF transform of KHR_texture_transform's README: translation * rotation * scale
    void transformUv(double offsetU, double offsetV, double rotation, double scaleU, double scaleV, double& u, double& v){
        const double su = scaleU * u, sv = scaleV * v;
        const double c = std::cos(rotation), s = std::sin(rotation);
        u = c * su - s * sv + offsetU;
        v = s * su + c * sv + offsetV;
    }

    const tinygltf::Primitive& primitiveOf(const tinygltf::Model& model, const char* nodeName){
        for (const tinygltf::Node& node : model.nodes){
            if (node.name == nodeName)
                return model.meshes[node.mesh].primitives[0];
        }
        static const tinygltf::Primitive none;
        return none;
    }

    // a quad's UV set `set` against its authored UVs (0,0 1,0 1,1 0,1) through a transform
    void expectUvs(const tinygltf::Model& model, const tinygltf::Primitive& primitive, int set,
                   double ou, double ov, double rotation, double su, double sv){
        std::vector<double> uvs;
        int components = 0;
        ASSERT_TRUE(primitive.attributes.count("TEXCOORD_" + std::to_string(set)));
        ASSERT_TRUE(soupRead(model, primitive.attributes.at("TEXCOORD_" + std::to_string(set)), uvs, components));
        const double authored[8] = {0, 0, 1, 0, 1, 1, 0, 1};
        ASSERT_EQ(uvs.size(), 8u);
        for (int i = 0; i < 4; i++){
            double u = authored[i * 2], v = authored[i * 2 + 1];
            transformUv(ou, ov, rotation, su, sv, u, v);
            EXPECT_NEAR(uvs[i * 2], u, 1e-5) << "set " << set << " vertex " << i;
            EXPECT_NEAR(uvs[i * 2 + 1], v, 1e-5) << "set " << set << " vertex " << i;
        }
    }

}

TEST(GltfExtensionsTest, TextureTransformIsBakedIntoTheUvs){
    tinygltf::Model model;
    std::string error;
    LogCapture log;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(s41Fixture("texture_transform.glb"), model, &error)) << error;
    const double a[5] = {0.25, 0.5, 0.3, 2.0, 3.0};

    // one transform on set 0 (base colour and normal map): the set is moved
    expectUvs(model, primitiveOf(model, "One transform"), 0, a[0], a[1], a[2], a[3], a[4]);
    const tinygltf::Material& one = model.materials[0];
    EXPECT_EQ(one.pbrMetallicRoughness.baseColorTexture.texCoord, 0);
    EXPECT_EQ(one.normalTexture.texCoord, 0);
    EXPECT_FALSE(one.pbrMetallicRoughness.baseColorTexture.extensions.count("KHR_texture_transform"));
    EXPECT_FALSE(one.normalTexture.extensions.count("KHR_texture_transform"));

    // two on set 0 and set 1 free: the emissive map reads a new set 1 with its own
    const tinygltf::Primitive& two = primitiveOf(model, "Two transforms");
    expectUvs(model, two, 0, a[0], a[1], a[2], a[3], a[4]);
    expectUvs(model, two, 1, 0.0, 0.0, 0.0, 4.0, 4.0);
    EXPECT_EQ(model.materials[1].pbrMetallicRoughness.baseColorTexture.texCoord, 0);
    EXPECT_EQ(model.materials[1].emissiveTexture.texCoord, 1);

    // two on set 0 with set 1 taken: the first transform for both, and a warning naming the material
    const tinygltf::Primitive& taken = primitiveOf(model, "No free set");
    expectUvs(model, taken, 0, a[0], a[1], a[2], a[3], a[4]);
    expectUvs(model, taken, 1, 0.0, 0.0, 0.0, 0.5, 0.5); // authored set 1 (half the UVs), untouched
    EXPECT_EQ(model.materials[2].emissiveTexture.texCoord, 0);
    EXPECT_EQ(log.count(D_LOG_WARN, "KHR_texture_transform"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "Taken"), 1u);

    // the transform's own texCoord (0) wins over the slot's (1)
    expectUvs(model, primitiveOf(model, "Set override"), 0, a[0], a[1], a[2], a[3], a[4]);
    EXPECT_EQ(model.materials[3].pbrMetallicRoughness.baseColorTexture.texCoord, 0);
}

TEST(GltfExtensionsTest, TextureTransformLoadsWithoutAWarningPerTexture){
    // a model whose transforms all bake is read in full: no "does not read" warning
    Scene scene;
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(s41Fixture("meshopt_c.glb")));
    EXPECT_EQ(log.count(D_LOG_WARN, "KHR_texture_transform"), 0u);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

TEST(GltfExtensionsTest, MeshLodsOfTheCookPassThrough){
    // contract C1: the cook's JE_mesh_lods on a primitive is read later (the LOD pool), never
    // reported or stripped by the loader
    TempDir dir;
    std::ifstream in(JASPER_TEST_ASSET_DIR "/gltf/plain.gltf");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string primitive = "\"attributes\"";
    const size_t at = text.find(primitive);
    ASSERT_NE(at, std::string::npos);
    text.insert(at, "\"extensions\": {\"JE_mesh_lods\": {\"recipe\": 1, \"levels\": []}}, ");
    const size_t asset = text.find("\"asset\"");
    ASSERT_NE(asset, std::string::npos);
    text.insert(asset, "\"extensionsUsed\": [\"JE_mesh_lods\"], ");
    const std::string path = dir.write("lods.gltf", text).generic_string();

    LogCapture log;
    tinygltf::Model model;
    std::string error;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(path, model, &error)) << error;
    ASSERT_FALSE(model.meshes.empty());
    EXPECT_TRUE(model.meshes[0].primitives[0].extensions.count("JE_mesh_lods"));
    Scene scene;
    Model loaded(&scene);
    EXPECT_TRUE(loaded.loadModel(path));
    EXPECT_EQ(log.count(D_LOG_WARN, "JE_mesh_lods"), 0u);
    EXPECT_EQ(log.count(D_LOG_ERROR, "JE_mesh_lods"), 0u);
}

TEST(GltfExtensionsTest, DracoInAGameNamesTheCook){
    // games carry no Draco decoder: the load fails, saying so and what fixes it
    Scene scene;
    LogCapture log;
    Model model(&scene);
    EXPECT_FALSE(model.loadModel(s41Fixture("draco.glb")));
    EXPECT_EQ(log.count(D_LOG_ERROR, "KHR_draco_mesh_compression"), 1u) << log.describeErrors();
    EXPECT_EQ(log.count(D_LOG_ERROR, "the cook transcodes it"), 1u) << log.describeErrors();
}

TEST(GltfExtensionsTest, WebpInAGameNamesTheCook){
    // the geometry loads; the texture whose only source is WebP is an error naming the fix
    Scene scene;
    LogCapture log;
    Model model(&scene);
    EXPECT_TRUE(model.loadModel(s41Fixture("webp.glb")));
    EXPECT_EQ(log.count(D_LOG_ERROR, "EXT_texture_webp"), 1u) << log.describeErrors();
    EXPECT_EQ(log.count(D_LOG_ERROR, "the cook transcodes it"), 1u) << log.describeErrors();
}
