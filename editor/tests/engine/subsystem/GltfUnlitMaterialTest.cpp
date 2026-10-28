// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// glTF materials MeshSystem::loadGLTF reads beyond the core slots, for every glTF it loads:
// KHR_materials_unlit loads as the library Unlit with Lighting None (S11 R4; the mode the
// Inspector's "Lighting: None (unlit)" sets, which the built-in Lit lists too), keeping the base
// colour and its texture; a material without it keeps Auto. The foliage transmission extra the VAT baker writes reads back the
// same way, and the built-in Lit accepts every lighting mode the editor offers for it.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "Catalog.h"
#include "Engine.h"
#include "Scene.h"
#include "System.h"
#include "component/MeshComponent.h"
#include "object/Model.h"
#include "shader/ShaderManifest.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include "json.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    class GltfUnlitMaterial : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            // File goes through System::instance(): TestMain's TestSystem (S02)
            editor::test::ensureEngine();
        }
    };

    // a 1x1 PNG (RGBA 255, 255, 255, 255), as a data: URI
    const char* whitePng =
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII=";

    // two triangles, each its own primitive with material 0 and 1 (`materials`), with UVs
    fs::path writeGltf(const std::string& name, const nlohmann::json& materials, const nlohmann::json& extensionsUsed) {
        const fs::path dir = fs::temp_directory_path() / "jasperengine-editor-tests-gltf-unlit";
        fs::create_directories(dir);
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        const float uvs[6] = {0, 0, 1, 0, 0, 1};
        const uint16_t indices[4] = {0, 1, 2, 0}; // padded to 4 bytes
        std::vector<unsigned char> buffer(sizeof(positions) + sizeof(normals) + sizeof(uvs) + sizeof(indices));
        size_t at = 0;
        for (const auto& part : {std::make_pair((const void*)positions, sizeof(positions)), std::make_pair((const void*)normals, sizeof(normals)),
                                 std::make_pair((const void*)uvs, sizeof(uvs)), std::make_pair((const void*)indices, sizeof(indices))}) {
            std::memcpy(buffer.data() + at, part.first, part.second);
            at += part.second;
        }
        {
            std::ofstream bin(dir / (name + ".bin"), std::ios::binary);
            bin.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        }
        const nlohmann::json attributes = {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}};
        nlohmann::json gltf = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", {{{"nodes", {0}}}}},
            {"nodes", {{{"name", "Mesh"}, {"mesh", 0}}}},
            {"meshes", {{{"primitives", {{{"attributes", attributes}, {"indices", 3}, {"material", 0}},
                                         {{"attributes", attributes}, {"indices", 3}, {"material", 1}}}}}}},
            {"materials", materials},
            {"images", {{{"uri", whitePng}}}},
            {"textures", {{{"source", 0}}}},
            {"buffers", {{{"uri", name + ".bin"}, {"byteLength", buffer.size()}}}},
            {"bufferViews", {
                {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 24}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 6}, {"target", 34963}}}},
            {"accessors", {
                {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
                 {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
                {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                {{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
                {{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}}},
        };
        if (!extensionsUsed.is_null()) gltf["extensionsUsed"] = extensionsUsed;
        const fs::path path = dir / (name + ".gltf");
        std::ofstream(path) << gltf.dump(2);
        return path;
    }

    // the submesh whose material has this red base colour (the load may order them otherwise)
    const Material& materialWithRed(const MeshComponent& mesh, float red) {
        for (unsigned int i = 0; i < mesh.numSubmeshes; i++) {
            if (std::fabs(mesh.submeshes[i].material.baseColorFactor.x - red) < 1e-6f) return mesh.submeshes[i].material;
        }
        ADD_FAILURE() << "no material with red " << red;
        static const Material none{};
        return none;
    }

}

TEST_F(GltfUnlitMaterial, KhrMaterialsUnlitLoadsAsLightingNone) {
    const nlohmann::json materials = {
        {{"name", "Glow"},
         {"pbrMetallicRoughness", {{"baseColorFactor", {0.9, 0.4, 0.2, 1.0}}, {"baseColorTexture", {{"index", 0}}}}},
         {"extensions", {{"KHR_materials_unlit", nlohmann::json::object()}}}},
        {{"name", "Plain"},
         {"pbrMetallicRoughness", {{"baseColorFactor", {0.1, 0.2, 0.3, 1.0}}}}}};
    const fs::path path = writeGltf("unlit", materials, {"KHR_materials_unlit"});

    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string()));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    ASSERT_EQ(mesh.numSubmeshes, 2u);

    const Material& glow = materialWithRed(mesh, 0.9f);
    EXPECT_EQ(glow.lighting, MaterialLighting::NONE);
    EXPECT_FLOAT_EQ(glow.baseColorFactor.x, 0.9f);
    EXPECT_FLOAT_EQ(glow.baseColorFactor.y, 0.4f);
    EXPECT_FLOAT_EQ(glow.baseColorFactor.z, 0.2f);
    EXPECT_FALSE(glow.baseColorTexture.empty()) << "the base colour texture is kept";
    EXPECT_EQ(glow.shader, "library:unlit") << "drawn by the library Unlit (S11)";

    const Material& plain = materialWithRed(mesh, 0.1f);
    EXPECT_EQ(plain.lighting, MaterialLighting::AUTO);
    EXPECT_FLOAT_EQ(plain.baseColorFactor.z, 0.3f);

    // the built-in Lit draws it unlit: it lists None, so None is what it resolves to
    EXPECT_TRUE(RenderSystem::supportsLighting(MaterialLighting::NONE, nullptr));
    EXPECT_EQ(RenderSystem::resolveLighting(glow.lighting, nullptr), MaterialLighting::NONE);
    EXPECT_EQ(RenderSystem::resolveLighting(plain.lighting, nullptr), MaterialLighting::PIXEL);
}

// without extensionsUsed listing it (a lenient exporter) the material's extension still counts;
// the foliage transmission extra reads back clamped
TEST_F(GltfUnlitMaterial, UnlitWithoutExtensionsUsedAndFoliageExtra) {
    const nlohmann::json materials = {
        {{"pbrMetallicRoughness", {{"baseColorFactor", {1, 1, 1, 1}}}},
         {"extensions", {{"KHR_materials_unlit", nlohmann::json::object()}}},
         {"extras", {{"jasperengine_foliage_transmission", 0.35}}}},
        {{"pbrMetallicRoughness", {{"baseColorFactor", {0.5, 1, 1, 1}}}},
         {"extras", {{"jasperengine_foliage_transmission", 4.0}}}}};
    const fs::path path = writeGltf("unlit_lenient", materials, nullptr);

    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), path.string()));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    ASSERT_EQ(mesh.numSubmeshes, 2u);
    EXPECT_EQ(materialWithRed(mesh, 1.0f).lighting, MaterialLighting::NONE);
    EXPECT_FLOAT_EQ(materialWithRed(mesh, 1.0f).foliageTransmission, 0.35f);
    EXPECT_EQ(materialWithRed(mesh, 0.5f).lighting, MaterialLighting::AUTO);
    EXPECT_FLOAT_EQ(materialWithRed(mesh, 0.5f).foliageTransmission, 1.0f);
}

// the editor's Lighting row (Catalog::getShaderLightingModes) offers None for the built-in Lit,
// after its default; a library shader keeps its own list
TEST_F(GltfUnlitMaterial, BuiltInLitListsNoneAfterItsModes) {
    EXPECT_EQ(editor::Catalog::getShaderLightingModes(nullptr),
              (std::vector<MaterialLighting>{MaterialLighting::PIXEL, MaterialLighting::VERTEX, MaterialLighting::MIXED, MaterialLighting::NONE}));
    EXPECT_TRUE(editor::Catalog::supportsLighting(MaterialLighting::NONE, nullptr));
    const std::shared_ptr<const ShaderManifest> toon = ShaderManifests::get("library:toon");
    ASSERT_TRUE(toon);
    EXPECT_FALSE(editor::Catalog::supportsLighting(MaterialLighting::NONE, toon.get())) << "toon lists pixel, mixed, vertex";
    EXPECT_EQ(RenderSystem::resolveLighting(MaterialLighting::NONE, toon.get()), MaterialLighting::PIXEL);
}
