// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A copy a system places (MeshSystem::loadPlacedModel: a foliage or spline chunk) draws instanced
// and never animates, so a textured, skinned model with a blend shape loads at rest as static
// geometry: no skin, the blend shape's default weight baked into its positions and normals.
// Instanced, its blend shapes alone needed 17 vertex inputs (16 is every backend's limit) and the
// chunk never drew. The export's shader variant collector probes foliage the same way, so it ships
// the variant the chunk asks for. The flags Lua can pass to loadGLTF keep a skin.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "buffer/Buffer.h"
#include "buffer/ExternalBuffer.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "pool/ShaderPool.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "json.hpp"
#include "support/EditorTestSupport.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    class PlacedModel : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
    };

    template<typename T>
    void append(std::vector<unsigned char>& data, const std::vector<T>& values){
        const size_t at = data.size();
        data.resize(at + values.size() * sizeof(T));
        std::memcpy(data.data() + at, values.data(), values.size() * sizeof(T));
    }

    // one textured triangle on a one-bone skin with a blend shape weighing 0.5 in the file: it
    // lifts the vertices by 0.5 and turns the normals towards +X
    fs::path writeSkinnedBlendShape(){
        std::vector<unsigned char> data;
        append<float>(data, {0, 0, 0, 1, 0, 0, 0, 1, 0});       // 0: positions
        append<float>(data, {0, 0, 1, 0, 0, 1, 0, 0, 1});       // 36: normals
        append<float>(data, {0, 0, 1, 0, 0, 1});                // 72: uvs
        append<uint16_t>(data, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}); // 96: joints
        append<float>(data, {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}); // 120: weights
        append<float>(data, {0, 0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 0}); // 168: position displacements
        append<float>(data, {2, 0, 0, 2, 0, 0, 2, 0, 0});       // 204: normal displacements
        append<uint16_t>(data, {0, 1, 2, 0});                    // 240: indices (padded)

        const fs::path dir = fs::temp_directory_path() / "jasperengine-editor-tests-placed";
        fs::create_directories(dir);
        {
            std::ofstream bin(dir / "blade.bin", std::ios::binary);
            bin.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        auto view = [](int offset, int length, int target){
            return nlohmann::json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}, {"target", target}};
        };
        const nlohmann::json gltf = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", {{{"nodes", {0, 1}}}}},
            {"nodes", {{{"name", "Blade"}, {"mesh", 0}, {"skin", 0}}, {{"name", "Bone"}}}},
            {"skins", {{{"joints", {1}}}}},
            {"meshes", {{{"weights", {0.5}}, {"primitives", {{
                {"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}, {"JOINTS_0", 3}, {"WEIGHTS_0", 4}}},
                {"targets", {{{"POSITION", 5}, {"NORMAL", 6}}}},
                {"indices", 7}, {"material", 0}}}}}}},
            {"materials", {{{"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}}}}},
            {"textures", {{{"source", 0}}}},
            {"images", {{{"uri", "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNoaGgAAAMEAYFL09IQAAAAAElFTkSuQmCC"}}}},
            {"buffers", {{{"uri", "blade.bin"}, {"byteLength", data.size()}}}},
            {"bufferViews", {view(0, 36, 34962), view(36, 36, 34962), view(72, 24, 34962), view(96, 24, 34962),
                             view(120, 48, 34962), view(168, 36, 34962), view(204, 36, 34962), view(240, 6, 34963)}},
            {"accessors", {
                {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
                {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                {{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
                {{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "VEC4"}},
                {{"bufferView", 4}, {"componentType", 5126}, {"count", 3}, {"type", "VEC4"}},
                {{"bufferView", 5}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {0, 0.5, 0}}, {"max", {0, 0.5, 0}}},
                {{"bufferView", 6}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                {{"bufferView", 7}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}}},
        };
        const fs::path path = dir / "blade.gltf";
        std::ofstream(path) << gltf.dump(2);
        return path;
    }

    Entity chunkEntity(Scene& scene){
        Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<MeshComponent>(entity, {});
        ModelComponent model;
        model.mergeStaticMeshes = true;
        scene.addComponent<ModelComponent>(entity, model);
        return entity;
    }

    // vertex i of the first submesh's attribute
    Vector3 vertex(const MeshComponent& mesh, AttributeType type, size_t i){
        auto attribute = mesh.submeshes[0].attributes.find(type);
        if (attribute == mesh.submeshes[0].attributes.end()){
            ADD_FAILURE() << "no attribute " << static_cast<int>(type);
            return Vector3();
        }
        for (unsigned int b = 0; b < mesh.numExternalBuffers; b++){
            const ExternalBuffer& buffer = mesh.eBuffers[b];
            if (buffer.getName() != attribute->second.getBufferName())
                continue;
            const size_t stride = buffer.getStride() ? buffer.getStride() : 3 * sizeof(float);
            float v[3];
            std::memcpy(v, buffer.getData() + attribute->second.getOffset() + i * stride, sizeof(v));
            return Vector3(v[0], v[1], v[2]);
        }
        ADD_FAILURE() << "no buffer " << attribute->second.getBufferName();
        return Vector3();
    }

    void expectNear(const Vector3& actual, const Vector3& expected, const std::string& what){
        EXPECT_NEAR(actual.x, expected.x, 1e-4f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1e-4f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1e-4f) << what;
    }

    // the geometry part of a mesh variant, whatever the scene's lights: what the vertex inputs follow
    uint64_t geometryBits(uint64_t properties){
        uint64_t bits = 0;
        for (int bit : {1, 11, 13, 14, 15, 16, 18, 26}) // Uv1 Txr Ski Mta Mnr Mtg Ist Ifd
            bits |= properties & shaderPropertyBit(bit);
        return bits;
    }
}

TEST_F(PlacedModel, SkinnedBlendShapeLoadsAtRestAsStaticGeometry){
    const fs::path path = writeSkinnedBlendShape();
    Scene scene;
    const Entity chunk = chunkEntity(scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadPlacedModel(chunk, path.string()));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(chunk);
    ASSERT_GE(mesh.numSubmeshes, 1u);
    const auto& attributes = mesh.submeshes[0].attributes;
    EXPECT_EQ(attributes.count(AttributeType::BONEIDS), 0u) << "no skin";
    EXPECT_EQ(attributes.count(AttributeType::BONEWEIGHTS), 0u) << "no skin";
    EXPECT_EQ(attributes.count(AttributeType::MORPHTARGET0), 0u) << "no blend shape input";
    EXPECT_EQ(attributes.count(AttributeType::MORPHNORMAL0), 0u) << "no blend shape input";
    EXPECT_EQ(attributes.count(AttributeType::TEXCOORD1), 1u) << "the UVs stay";
    // the file's 0.5 weight, baked
    expectNear(vertex(mesh, AttributeType::POSITION, 0), Vector3(0, 0.25f, 0), "position 0");
    expectNear(vertex(mesh, AttributeType::POSITION, 1), Vector3(1, 0.25f, 0), "position 1");
    expectNear(vertex(mesh, AttributeType::POSITION, 2), Vector3(0, 1.25f, 0), "position 2");
    expectNear(vertex(mesh, AttributeType::NORMAL, 0), Vector3(std::sqrt(0.5f), 0, std::sqrt(0.5f)), "normal 0");
}

TEST_F(PlacedModel, TheLoaderFlagsLuaCanPassKeepTheSkin){
    const fs::path path = writeSkinnedBlendShape();
    Scene scene;
    const Entity entity = chunkEntity(scene);
    // skipEntities and bakeSceneRoot, as ECSClassesLua's loadGLTF takes them
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(entity, path.string(), false, true, false, true));
    const auto& attributes = scene.getComponent<MeshComponent>(entity).submeshes[0].attributes;
    EXPECT_EQ(attributes.count(AttributeType::BONEIDS), 1u) << "a script's load keeps the skin";
    EXPECT_EQ(attributes.count(AttributeType::MORPHTARGET0), 1u) << "and the blend shape";
}

TEST_F(PlacedModel, FoliageShipsTheVariantTheChunkAsksFor){
    const fs::path path = writeSkinnedBlendShape();

    // what a foliage chunk of the model asks for (MeshSystem::updateFoliageLayer, RenderSystem::loadMesh)
    Scene chunkScene;
    const Entity chunk = chunkEntity(chunkScene);
    InstancedMeshComponent instmesh;
    instmesh.distanceFade = true;
    chunkScene.addComponent<InstancedMeshComponent>(chunk, instmesh);
    ASSERT_TRUE(chunkScene.getSystem<MeshSystem>()->loadPlacedModel(chunk, path.string()));
    MeshComponent& mesh = chunkScene.getComponent<MeshComponent>(chunk);
    std::vector<const Buffer*> buffers;
    for (unsigned int b = 0; b < mesh.numExternalBuffers; b++)
        buffers.push_back(&mesh.eBuffers[b]);
    Submesh submesh = mesh.submeshes[0];
    if (MeshSystem::usesTextureRect(submesh))
        submesh.hasTextureRect = true;
    RenderSystem::scanSubmeshAttributes(submesh, buffers);
    const SubmeshShaderInputs inputs = RenderSystem::getSubmeshShaderInputs(mesh, submesh, nullptr,
        &chunkScene.getComponent<InstancedMeshComponent>(chunk), false, "");
    const uint64_t asked = geometryBits(RenderSystem::getSubmeshShaderVariant(MeshShaderEnvironment(), inputs).properties);
    EXPECT_FALSE(asked & shaderPropertyBit(13)) << "the chunk draws no skin";
    EXPECT_FALSE(asked & shaderPropertyBit(14)) << "nor blend shapes";

    // what an export of a terrain with that detail layer ships
    Scene scene;
    const Entity ground = scene.createEntity();
    scene.addComponent<Transform>(ground, {});
    TerrainComponent terrain;
    terrain.heightMap = Texture("heightmap.png");
    TerrainFoliageLayer layer;
    layer.meshPath = AssetRef(path.string());
    terrain.foliageLayers.push_back(layer);
    scene.addComponent<TerrainComponent>(ground, terrain);
    ShaderVariantContent content;
    content.registry = &scene;
    content.entities = {ground};
    std::set<ShaderKey> keys;
    ShaderVariantCollector::collectScene(&scene, content, {}, keys);

    std::set<uint64_t> shipped;
    std::string names;
    for (ShaderKey key : keys){
        const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
        if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH || !(properties & shaderPropertyBit(18)))
            continue;
        shipped.insert(geometryBits(properties));
        names += " " + ShaderPool::getShaderStr(ShaderType::MESH, properties, 0);
    }
    EXPECT_EQ(shipped, std::set<uint64_t>{asked}) << "the instanced variants shipped:" << names;
}
