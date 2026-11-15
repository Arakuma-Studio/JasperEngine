// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vertex Animation shader variants in exports (ShaderVariantCollector, vertex animation spec
// 3.5/3.9): a model with a Vertex Animator ships its 'Vat' (Vertices) or 'Vab' (Bones) variants
// in the colour and depth passes next to the ones it draws at rest with (a .vat.glb has no
// skin, so neither has 'Ski'); an animator whose asset cannot be read ships both modes; a model
// without an animator ships none.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "animation/VertexAnimation.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/ShaderPool.h"
#include "Engine.h"
#include "System.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include "support/PosixCompat.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class VatShaderVariants : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            // File goes through System::instance(): TestMain's TestSystem (S02)
            editor::test::ensureEngine();
        }
    };

    std::string base64(const std::vector<uint8_t>& bytes){
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3){
            const uint32_t b0 = bytes[i];
            const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
            const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
            const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += i + 1 < bytes.size() ? table[(v >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? table[v & 63] : '=';
        }
        return out;
    }

    // a one-triangle .vat.glb-like glTF (one node, no skin, the JASPER_vertex_animation
    // extension): _VAT_INDEX, or JOINTS_0 / WEIGHTS_0 for Bones mode
    std::string writeVatGltf(const std::string& name, bool bones){
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        const float vatIndex[3] = {0, 1, 2};
        const uint8_t joints[12] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
        const float weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        const uint16_t indices[4] = {0, 1, 2, 0};
        std::vector<uint8_t> data;
        auto put = [&data](const void* bytes, size_t size){
            const size_t offset = data.size();
            data.insert(data.end(), (const uint8_t*)bytes, (const uint8_t*)bytes + size);
            return offset;
        };
        const size_t p = put(positions, 36), n = put(normals, 36), v = put(vatIndex, 12), j = put(joints, 12), w = put(weights, 48), i = put(indices, 6);
        std::ostringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"" << vat::VAT_GLTF_EXTENSION << "\"],"
             << "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
             << "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
             << (bones ? "\"JOINTS_0\":3,\"WEIGHTS_0\":4" : std::string("\"") + vat::VAT_INDEX_ATTRIBUTE + "\":2") << "},\"indices\":5}]}],"
             << "\"buffers\":[{\"byteLength\":" << data.size() << ",\"uri\":\"data:application/octet-stream;base64," << base64(data) << "\"}],"
             << "\"bufferViews\":["
             << "{\"buffer\":0,\"byteOffset\":" << p << ",\"byteLength\":36},{\"buffer\":0,\"byteOffset\":" << n << ",\"byteLength\":36},"
             << "{\"buffer\":0,\"byteOffset\":" << v << ",\"byteLength\":12},{\"buffer\":0,\"byteOffset\":" << j << ",\"byteLength\":12},"
             << "{\"buffer\":0,\"byteOffset\":" << w << ",\"byteLength\":48},{\"buffer\":0,\"byteOffset\":" << i << ",\"byteLength\":6}],"
             << "\"accessors\":["
             << "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
             << "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
             << "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\"},"
             << "{\"bufferView\":3,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
             << "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
             << "{\"bufferView\":5,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}";
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("jasper-vat-variants-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        const std::filesystem::path path = dir / (name + ".gltf");
        std::ofstream(path) << json.str();
        return path.string();
    }

    std::shared_ptr<const vat::VertexAnimation> makeAsset(vat::VatMode mode){
        vat::VatData data;
        data.mode = mode;
        data.mesh = "test.vat.glb";
        vat::VatClip clip;
        clip.name = "Idle";
        clip.frames = 1;
        data.clips.push_back(clip);
        data.layout.precision = vat::VatPrecision::Float;
        data.layout.width = 8;
        data.layout.height = 1;
        data.layout.samples = 1;
        data.layout.normals = false;
        if (mode == vat::VatMode::Bones)
            data.layout.bones = 2;
        else
            data.layout.vertices = 3;
        data.layout.layoutHash = vat::computeLayoutHash(data);
        vat::VatPayload payload;
        payload.precision = vat::VatPrecision::Float;
        payload.width = 8;
        payload.height = 1;
        payload.bytes.assign(8 * 16, 0);
        std::string error;
        auto asset = vat::VertexAnimation::create(data, payload, vat::VatSocketTracks(), &error);
        EXPECT_TRUE(asset) << error;
        return asset;
    }

    struct Keys {
        bool vertices = false;      // MESH 'Vat'
        bool bones = false;         // MESH 'Vab'
        bool rest = false;          // MESH without either
        bool skinning = false;      // any MESH or DEPTH key with 'Ski'
        bool depthVertices = false; // DEPTH 'Vat'
        bool depthBones = false;    // DEPTH 'Vab'
    };

    Keys collect(Scene& scene){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        Keys out;
        for (ShaderKey key : keys){
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            const ShaderType type = ShaderPool::getShaderTypeFromKey(key);
            if (type == ShaderType::MESH){
                out.vertices |= (properties & ShaderPool::getVatProperty(ShaderType::MESH, false)) != 0;
                out.bones |= (properties & ShaderPool::getVatProperty(ShaderType::MESH, true)) != 0;
                out.rest |= (properties & ShaderPool::getVatProperties(ShaderType::MESH)) == 0;
                out.skinning |= (properties & shaderPropertyBit(13)) != 0;
            }else if (type == ShaderType::DEPTH){
                out.depthVertices |= (properties & shaderPropertyBit(16)) != 0;
                out.depthBones |= (properties & shaderPropertyBit(17)) != 0;
                out.skinning |= (properties & shaderPropertyBit(1)) != 0;
            }
        }
        return out;
    }

    // a model GameObject (a shadow-casting light in the scene, so depth variants are collected)
    Entity addModel(Scene& scene, const std::string& file){
        if (!scene.isComponentRegistered<VertexAnimatorComponent>())
            scene.registerComponent<VertexAnimatorComponent>();
        const Entity light = scene.createEntity();
        scene.addComponent<Transform>(light, {});
        LightComponent lightComponent;
        lightComponent.shadows = true;
        scene.addComponent<LightComponent>(light, lightComponent);

        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<MeshComponent>(entity, {});
        ModelComponent model;
        model.filename = file;
        model.needUpdateModel = false;
        scene.addComponent<ModelComponent>(entity, model);
        return entity;
    }

    void addAnimator(Scene& scene, Entity entity, std::shared_ptr<const vat::VertexAnimation> asset, const std::string& path = ""){
        VertexAnimatorComponent animator;
        animator.animation = path;
        animator.asset = asset;
        scene.addComponent<VertexAnimatorComponent>(entity, animator);
    }

}

TEST_F(VatShaderVariants, VerticesModeShipsVatNextToRest){
    auto scene = std::make_unique<Scene>();
    const Entity entity = addModel(*scene, writeVatGltf("vertices", false));
    addAnimator(*scene, entity, makeAsset(vat::VatMode::Vertices));
    const Keys keys = collect(*scene);
    EXPECT_TRUE(keys.vertices);
    EXPECT_TRUE(keys.depthVertices);
    EXPECT_TRUE(keys.rest) << "drawn at rest until its animation is on the GPU";
    EXPECT_FALSE(keys.bones);
    EXPECT_FALSE(keys.depthBones);
}

TEST_F(VatShaderVariants, BonesModeShipsVabWithoutSkinning){
    auto scene = std::make_unique<Scene>();
    const Entity entity = addModel(*scene, writeVatGltf("bones", true));
    addAnimator(*scene, entity, makeAsset(vat::VatMode::Bones));
    const Keys keys = collect(*scene);
    EXPECT_TRUE(keys.bones);
    EXPECT_TRUE(keys.depthBones);
    EXPECT_TRUE(keys.rest);
    EXPECT_FALSE(keys.vertices);
    EXPECT_FALSE(keys.skinning) << "a .vat.glb's JOINTS_0 have no skin: no 'Ski' at rest either";
}

TEST_F(VatShaderVariants, UnreadableAssetShipsBothModes){
    auto scene = std::make_unique<Scene>();
    const Entity entity = addModel(*scene, writeVatGltf("unreadable", false));
    addAnimator(*scene, entity, nullptr, "assets/vat/missing.vat");
    const Keys keys = collect(*scene);
    EXPECT_TRUE(keys.vertices);
    EXPECT_TRUE(keys.bones);
    EXPECT_TRUE(keys.rest);
}

TEST_F(VatShaderVariants, NoAnimatorShipsNone){
    auto scene = std::make_unique<Scene>();
    addModel(*scene, writeVatGltf("plain", true));
    const Keys keys = collect(*scene);
    EXPECT_TRUE(keys.rest);
    EXPECT_FALSE(keys.vertices);
    EXPECT_FALSE(keys.bones);
    EXPECT_FALSE(keys.skinning) << "the .vat.glb draws at rest without a skin";
}
