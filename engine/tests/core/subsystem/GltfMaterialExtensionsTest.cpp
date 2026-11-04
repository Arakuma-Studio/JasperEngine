// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// glTF KHR_materials_* (docs/specs/S11-materials-lighting.md R4, AC4): hand-written fixtures
// (fixtures/gltf, one triangle and a material each) load to the exact factors, maps and UV
// sets; missing values are the defaults, out-of-range ones clamped, a non-finite one warns
// once; an unsupported extension warns (an error when required, with what to do) and the model
// still loads.
// The Khronos sample models (engine/tests/render/assets/khronos) load without an error. CPU only.

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"
#include "pool/TextureDataPool.h"
#include "subsystem/MeshSystem.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class GltfMaterialExtensionsTest : public ::testing::Test {
    protected:
        Scene scene;

        // the first submesh's material of a fixture
        const Material& load(const std::string& name){
            model = std::make_unique<Model>(&scene);
            EXPECT_TRUE(model->loadModel("gltf/" + name + ".gltf")) << name;
            const MeshComponent& mesh = scene.getComponent<MeshComponent>(model->getEntity());
            EXPECT_GE(mesh.numSubmeshes, 1u);
            return mesh.submeshes[0].material;
        }

        // the pixels of a packed map, made on a job (polled as a draw would)
        std::shared_ptr<std::array<TextureData, 6>> generated(const std::string& id){
            for (int attempt = 0; attempt < 500; attempt++){
                TextureLoadResult result = TextureDataPool::loadGenerated(id);
                if (result.state == ResourceLoadState::Finished)
                    return result.data;
                if (result.state != ResourceLoadState::Loading)
                    return nullptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return nullptr;
        }

        std::unique_ptr<Model> model;
    };

    bool isPacked(const Texture& texture){
        return texture.getId().rfind("pack|", 0) == 0;
    }

}

TEST_F(GltfMaterialExtensionsTest, Clearcoat){
    const Material& material = load("clearcoat");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_TRUE(pbr.clearcoat.enabled);
    EXPECT_FLOAT_EQ(pbr.clearcoat.factor, 0.8f);
    EXPECT_FLOAT_EQ(pbr.clearcoat.roughness, 0.3f);
    // two textures: one map packed from them, R the factor's red, G the roughness's green
    ASSERT_TRUE(isPacked(pbr.clearcoat.map));
    EXPECT_EQ(pbr.clearcoat.mapTexCoord, 0);
    auto pixels = generated(pbr.clearcoat.map.getId());
    ASSERT_TRUE(pixels);
    const unsigned char* texel = static_cast<const unsigned char*>(pixels->at(0).getData());
    EXPECT_EQ(texel[0], 200);
    EXPECT_EQ(texel[1], 150);
    EXPECT_EQ(texel[2], 255);
    EXPECT_EQ(texel[3], 255);
    // the coat normal, its own map on UV set 1 with its scale
    EXPECT_FALSE(pbr.clearcoat.normalMap.empty());
    EXPECT_FALSE(isPacked(pbr.clearcoat.normalMap));
    EXPECT_EQ(pbr.clearcoat.normalTexCoord, 1);
    EXPECT_FLOAT_EQ(pbr.clearcoat.normalScale, 0.5f);
    EXPECT_FALSE(pbr.sheen.enabled);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(GltfMaterialExtensionsTest, ClearcoatSharedTexture){
    // one texture for both: used as it is (glTF's own packing)
    const Material& material = load("clearcoat_shared");
    EXPECT_FALSE(material.pbr->clearcoat.map.empty());
    EXPECT_FALSE(isPacked(material.pbr->clearcoat.map));
}

TEST_F(GltfMaterialExtensionsTest, Sheen){
    const Material& material = load("sheen");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_TRUE(pbr.sheen.enabled);
    EXPECT_EQ(pbr.sheen.color, Vector3(0.5f, 0.2f, 0.1f));
    EXPECT_FLOAT_EQ(pbr.sheen.roughness, 0.4f);
    EXPECT_FALSE(pbr.sheen.map.empty());
    EXPECT_FALSE(isPacked(pbr.sheen.map));
}

TEST_F(GltfMaterialExtensionsTest, Anisotropy){
    const Material& material = load("anisotropy");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_TRUE(pbr.anisotropy.enabled);
    EXPECT_FLOAT_EQ(pbr.anisotropy.strength, 0.6f);
    EXPECT_FLOAT_EQ(pbr.anisotropy.rotation, 1.0f);
    EXPECT_FALSE(pbr.anisotropy.map.empty());
    EXPECT_EQ(pbr.anisotropy.mapTexCoord, 1);
}

TEST_F(GltfMaterialExtensionsTest, Iridescence){
    const Material& material = load("iridescence");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_TRUE(pbr.iridescence.enabled);
    EXPECT_FLOAT_EQ(pbr.iridescence.factor, 1.0f);
    EXPECT_FLOAT_EQ(pbr.iridescence.ior, 1.8f);
    EXPECT_EQ(pbr.iridescence.thicknessNm, Vector2(200.0f, 600.0f));
    ASSERT_TRUE(isPacked(pbr.iridescence.map));
    auto pixels = generated(pbr.iridescence.map.getId());
    ASSERT_TRUE(pixels);
    const unsigned char* texel = static_cast<const unsigned char*>(pixels->at(0).getData());
    EXPECT_EQ(texel[0], 200);   // the factor map's red
    EXPECT_EQ(texel[1], 150);   // the thickness map's green
}

TEST_F(GltfMaterialExtensionsTest, Transmission){
    const Material& material = load("transmission");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_TRUE(pbr.transmission.enabled);
    EXPECT_FLOAT_EQ(pbr.transmission.factor, 0.9f);
    // its factor (R) and the volume's thickness (G) in one map
    ASSERT_TRUE(isPacked(pbr.transmission.map));
    EXPECT_TRUE(pbr.volume.enabled);
    EXPECT_FLOAT_EQ(pbr.volume.thickness, 0.5f);
    EXPECT_FLOAT_EQ(pbr.volume.attenuationDistance, 2.0f);
    EXPECT_EQ(pbr.volume.attenuationColor, Vector3(0.5f, 1.0f, 1.0f));
}

TEST_F(GltfMaterialExtensionsTest, Volume){
    // a volume without transmission is kept, and draws nothing of it
    const Material& material = load("volume_alone");
    EXPECT_TRUE(material.pbr->volume.enabled);
    EXPECT_FLOAT_EQ(material.pbr->volume.thickness, 0.25f);
    EXPECT_TRUE(std::isinf(material.pbr->volume.attenuationDistance));
    EXPECT_TRUE(MaterialPbr::getPbrVariant(material, MaterialLighting::PIXEL, false, PbrTier::FULL).empty());
}

TEST_F(GltfMaterialExtensionsTest, Specular){
    const Material& material = load("specular");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_TRUE(pbr.specular.enabled);
    EXPECT_FLOAT_EQ(pbr.specular.factor, 0.7f);
    EXPECT_EQ(pbr.specular.color, Vector3(1.0f, 0.5f, 0.5f));
    EXPECT_FLOAT_EQ(pbr.specular.ior, 1.5f);
    ASSERT_TRUE(isPacked(pbr.specular.map));
    auto pixels = generated(pbr.specular.map.getId());
    ASSERT_TRUE(pixels);
    const unsigned char* texel = static_cast<const unsigned char*>(pixels->at(0).getData());
    EXPECT_EQ(texel[0], 200);  // the colour map's RGB
    EXPECT_EQ(texel[1], 10);
    EXPECT_EQ(texel[2], 20);
    EXPECT_EQ(texel[3], 60);   // the strength map's alpha
}

TEST_F(GltfMaterialExtensionsTest, Ior){
    // KHR_materials_ior alone: the Specular & IOR lobe with the default reflectance
    const Material& material = load("ior");
    EXPECT_TRUE(material.pbr->specular.enabled);
    EXPECT_FLOAT_EQ(material.pbr->specular.ior, 1.33f);
    EXPECT_FLOAT_EQ(material.pbr->specular.factor, 1.0f);
    EXPECT_EQ(material.pbr->specular.color, Vector3(1.0f, 1.0f, 1.0f));
}

TEST_F(GltfMaterialExtensionsTest, EmissiveStrength){
    const Material& material = load("emissive_strength");
    EXPECT_FLOAT_EQ(material.emissiveStrength, 5.0f);
    EXPECT_EQ(material.emissiveFactor, Vector3(1.0f, 0.5f, 0.0f));
    EXPECT_TRUE(material.pbr == MaterialPbrExtensions());
}

TEST_F(GltfMaterialExtensionsTest, Unlit){
    // the library Unlit shader, Lighting None, with the base colour, map and alpha clip
    const Material& material = load("unlit");
    EXPECT_EQ(material.shader, "library:unlit");
    EXPECT_EQ(material.lighting, MaterialLighting::NONE);
    EXPECT_EQ(ShaderUniforms::get(material.shaderParams, "baseColor"), Vector4(0.2f, 0.4f, 0.6f, 1.0f));
    ASSERT_EQ(material.shaderTextures.size(), 1u);
    EXPECT_EQ(material.shaderTextures[0].first, "u_baseTexture");
    EXPECT_EQ(material.shaderFeatures, (std::vector<std::string>{"ALPHA_CLIP"}));
    EXPECT_FLOAT_EQ(ShaderUniforms::get(material.shaderParams, "alphaCutoff").x, 0.25f);
}

TEST_F(GltfMaterialExtensionsTest, MissingValuesAreDefaults){
    // every extension present but empty: enabled, with the specifications' defaults
    const Material& material = load("defaults");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_TRUE(pbr.clearcoat.enabled);
    EXPECT_FLOAT_EQ(pbr.clearcoat.factor, 0.0f);
    EXPECT_TRUE(pbr.sheen.enabled);
    EXPECT_EQ(pbr.sheen.color, Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(pbr.specular.enabled);
    EXPECT_FLOAT_EQ(pbr.specular.factor, 1.0f);
    EXPECT_FLOAT_EQ(pbr.specular.ior, 1.5f);
    EXPECT_TRUE(pbr.iridescence.enabled);
    EXPECT_FLOAT_EQ(pbr.iridescence.ior, 1.3f);
    EXPECT_EQ(pbr.iridescence.thicknessNm, Vector2(100.0f, 400.0f));
    EXPECT_TRUE(pbr.clearcoat.map.empty());
    // no extension at all: nothing set, no reference
    const Material& plain = load("plain");
    EXPECT_FALSE(plain.pbr.isSet());
    EXPECT_FLOAT_EQ(plain.emissiveStrength, 1.0f);
    EXPECT_TRUE(MaterialPbr::getPbrVariant(plain, MaterialLighting::PIXEL, false, PbrTier::FULL).empty());
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(GltfMaterialExtensionsTest, OutOfRangeIsClamped){
    LogCapture log;
    const Material& material = load("out_of_range");
    const MaterialPbrExtensions& pbr = material.pbr;
    EXPECT_FLOAT_EQ(pbr.clearcoat.factor, 1.0f);
    EXPECT_FLOAT_EQ(pbr.clearcoat.roughness, 0.0f);
    EXPECT_EQ(pbr.sheen.color, Vector3(1.0f, 0.5f, 0.0f));
    EXPECT_FLOAT_EQ(pbr.specular.ior, 1.0f);
    EXPECT_FLOAT_EQ(pbr.anisotropy.strength, 1.0f);
    EXPECT_EQ(log.count(D_LOG_WARN, "out of its range"), 1u);
}

TEST_F(GltfMaterialExtensionsTest, NonFiniteWarnsOnce){
    LogCapture log;
    const Material& material = load("not_finite");
    EXPECT_FLOAT_EQ(material.pbr->clearcoat.factor, 0.0f);
    EXPECT_FLOAT_EQ(material.pbr->clearcoat.roughness, 0.0f);
    // two values, one warning; and none when the model loads again
    EXPECT_EQ(log.count(D_LOG_WARN, "not a finite number"), 1u);
    load("not_finite");
    EXPECT_EQ(log.count(D_LOG_WARN, "not a finite number"), 1u);
}

TEST_F(GltfMaterialExtensionsTest, UnsupportedWarns){
    LogCapture log;
    const Material& material = load("unsupported");
    // the model loads, its clear coat with it
    EXPECT_TRUE(material.pbr->clearcoat.enabled);
    // one warning per extension, however many textures use it (the fixtures name extensions
    // nobody defines: the loader reads the ones they used to name since S53, KHR_texture_transform
    // and EXT_meshopt_compression)
    EXPECT_EQ(log.count(D_LOG_WARN, "EXT_unknown_texture_extension"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "KHR_lights_punctual"), 1u);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();

    // a required one is an error that says what to do, and the model still loads
    LogCapture required;
    load("unsupported_required");
    EXPECT_EQ(required.count(D_LOG_ERROR, "EXT_unknown_required_extension"), 1u);
    EXPECT_EQ(required.count(D_LOG_ERROR, "export the model again without EXT_unknown_required_extension"), 1u);
    EXPECT_EQ(required.count(D_LOG_WARN, "EXT_unknown_required_extension"), 0u);
}

TEST_F(GltfMaterialExtensionsTest, KhronosSampleModelsLoad){
    // every vendored sample (docs/specs/S11-materials-lighting.md, section 6.7) loads with no error
    const std::filesystem::path root = std::filesystem::path(JASPER_TEST_SOURCE_DIR) / ".." / "render" / "assets" / "khronos";
    ASSERT_TRUE(std::filesystem::is_directory(root)) << root;
    int models = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)){
        const std::string ext = entry.path().extension().string();
        if (ext != ".glb" && ext != ".gltf")
            continue;
        LogCapture log;
        Model sample(&scene);
        EXPECT_TRUE(sample.loadModel(std::filesystem::absolute(entry.path()).lexically_normal().string())) << entry.path();
        EXPECT_TRUE(log.errors().empty()) << entry.path() << ":\n" << log.describeErrors();
        models++;
    }
    EXPECT_EQ(models, 13);
}

namespace {

    class SubmeshOverrideTest : public ::testing::Test {
    protected:
        Scene scene;
    };

}

TEST_F(SubmeshOverrideTest, PbrSurvivesReload){
    // an edited lobe (Submesh::overrideFields: SubmeshOverride_PbrExtensions) is put back over
    // what a reload of the model reads; a map that is still the file's keeps the loaded one
    Model model(&scene);
    ASSERT_TRUE(model.loadModel("gltf/clearcoat.gltf"));
    MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    const std::string fileMap = mesh.submeshes[0].material.pbr->clearcoat.map.getId();
    MaterialPbrExtensions& pbr = mesh.submeshes[0].material.pbr.edit();
    pbr.clearcoat.roughness = 0.9f;
    pbr.sheen.enabled = true;
    pbr.sheen.color = Vector3(0.3f, 0.2f, 0.1f);
    mesh.submeshes[0].material.emissiveStrength = 3.0f;
    mesh.submeshes[0].overrideFields |= SubmeshOverride_PbrExtensions | SubmeshOverride_EmissiveStrength;

    ASSERT_TRUE(model.loadModel("gltf/clearcoat.gltf"));
    const Material& reloaded = scene.getComponent<MeshComponent>(model.getEntity()).submeshes[0].material;
    EXPECT_FLOAT_EQ(reloaded.pbr->clearcoat.roughness, 0.9f);
    EXPECT_FLOAT_EQ(reloaded.pbr->clearcoat.factor, 0.8f);
    EXPECT_TRUE(reloaded.pbr->sheen.enabled);
    EXPECT_EQ(reloaded.pbr->sheen.color, Vector3(0.3f, 0.2f, 0.1f));
    EXPECT_EQ(reloaded.pbr->clearcoat.map.getId(), fileMap);
    EXPECT_FLOAT_EQ(reloaded.emissiveStrength, 3.0f);

    // without the override bits the file's values come back
    scene.getComponent<MeshComponent>(model.getEntity()).submeshes[0].overrideFields = 0;
    ASSERT_TRUE(model.loadModel("gltf/clearcoat.gltf"));
    const Material& file = scene.getComponent<MeshComponent>(model.getEntity()).submeshes[0].material;
    EXPECT_FLOAT_EQ(file.pbr->clearcoat.roughness, 0.3f);
    EXPECT_FALSE(file.pbr->sheen.enabled);
    EXPECT_FLOAT_EQ(file.emissiveStrength, 1.0f);
}
