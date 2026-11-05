// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S53 R4-R6, R12, AC7, AC11 end to end: the loader's fixtures (gltfpack, gltf-transform,
// Draco, WebP, MSFT_lod; engine/tests/core/fixtures/gltf/s41) cooked at each compression, then read
// back as a game reads them (tinygltf, then GltfNormalize): the cooked file needs no editor-only
// decoder, the loader finds no error in it, and it draws the source's triangles within the
// compression's documented error. MSFT_lod's lower levels become the base mesh's baked levels;
// a cooked model loads through the editor's redirect (MeshSystem::getImportedModelPath).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TextureCorpus.h"

#include "model/CookedModels.h"
#include "model/GltfDocument.h"
#include "model/ModelCooker.h"
#include "pool/MeshLodPool.h"
#include "Scene.h"
#include "object/Model.h"
#include "subsystem/GltfNormalize.h"
#include "subsystem/MeshSystem.h"

#include "LogCapture.h"
#include "subsystem/GltfTriangleSoup.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
using namespace jasperengine::test;
using GJson = GltfDocument::Json;

namespace {

    ResolvedModelSettings settingsOf(OptimizeMesh optimize, MeshCompression compression, GenerateLods lods = GenerateLods::Automatic){
        ResolvedModelSettings settings;
        settings.optimizeMesh = optimize;
        settings.meshCompression = compression;
        settings.generateLods = lods;
        return settings;
    }

    bool cookFile(const fs::path& source, const ResolvedModelSettings& settings, ModelFamily family, std::vector<uint8_t>& glb,
                  ModelCookRecord& record, std::string& error){
        ModelCookRequest request;
        request.source = source;
        request.assetPath = source.filename().generic_string();
        request.settings = settings;
        request.family = family;
        return ModelCooker::cookToMemory(request, glb, record, error);
    }

    // a file as a game reads it: tinygltf, then the loader's normalise step
    ::testing::AssertionResult loadAsGame(const std::vector<uint8_t>& bytes, const fs::path& baseDir, tinygltf::Model& model,
                                          GltfNormalizeResult& result){
        tinygltf::TinyGLTF loader;
        loader.SetImagesAsIs(true);
        std::string err, warn;
        if (!loader.LoadBinaryFromMemory(&model, &err, &warn, bytes.data(), (unsigned int)bytes.size(), baseDir.generic_string()) || !err.empty())
            return ::testing::AssertionFailure() << "does not parse: " << err;
        if (!GltfNormalize::normalize(model, "cooked.glb", result))
            return ::testing::AssertionFailure() << "does not load: " << result.firstError();
        return ::testing::AssertionSuccess();
    }

    // the documented bounds (docs/meshes.md) for the fixtures, which are about two units across
    SoupTolerance toleranceOf(MeshCompression compression){
        SoupTolerance tolerance;
        if (compression == MeshCompression::Off || compression == MeshCompression::Lossless){
            tolerance.position = 1e-6;
            tolerance.uv = 1e-6;
            tolerance.normalDot = 0.99999;
            tolerance.color = 1e-6;
        }else{
            // Medium: 14-bit positions (2 * 2^-13), 12-bit uvs over [0, 2], 8-bit normals (1 degree), 8-bit colours
            tolerance.position = 2.5e-4;
            tolerance.uv = 1e-3;
            tolerance.normalDot = 0.9995;
            tolerance.color = 1.0 / 255.0;
        }
        return tolerance;
    }

    // a triangle list's positions, each triangle turned to start at its smallest corner, sorted
    std::vector<std::array<double, 9>> trianglePositions(const std::vector<float>& positions, const std::vector<uint32_t>& list){
        std::vector<std::array<double, 9>> out;
        for (size_t t = 0; t + 2 < list.size(); t += 3){
            std::array<std::array<double, 3>, 3> corner;
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++)
                    corner[c][k] = positions[(size_t)list[t + c] * 3 + k];
            const int first = (int)(std::min_element(corner.begin(), corner.end()) - corner.begin());
            std::array<double, 9> turned;
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++)
                    turned[c * 3 + k] = corner[(first + c) % 3][k];
            out.push_back(turned);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

}

// one test per fixture: what its loader reads, and the cook of it at each compression
class ModelCookLoadFixtures : public ::testing::TestWithParam<const char*> {};

TEST_P(ModelCookLoadFixtures, LoadsAsItsSourceWithoutEditorDecoders){
    const char* const name = GetParam();
    const fs::path source = s41Fixture(name);
    tinygltf::Model reference;
    std::string error;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(source.generic_string(), reference, &error)) << name << ": " << error;
    const std::vector<SoupTriangle> expected = triangleSoup(reference);
    ASSERT_FALSE(expected.empty()) << name;
    for (MeshCompression compression : {MeshCompression::Off, MeshCompression::Lossless, MeshCompression::Medium}){
        for (ModelFamily family : {ModelFamily::Desktop, ModelFamily::Web}){
            std::vector<uint8_t> glb;
            ModelCookRecord record;
            ASSERT_TRUE(cookFile(source, settingsOf(OptimizeMesh::Everything, compression), family, glb, record, error))
                << name << " " << meshCompressionName(compression) << ": " << error;
            tinygltf::Model cooked;
            GltfNormalizeResult result;
            ASSERT_TRUE(loadAsGame(glb, source.parent_path(), cooked, result)) << name << " " << meshCompressionName(compression);
            EXPECT_FALSE(result.needsEditorDecoders) << name << ": a game reads the cook without Draco or WebP";
            EXPECT_FALSE(result.hasErrors()) << name << ": " << result.firstError();
            for (const std::string& extension : cooked.extensionsUsed){
                EXPECT_NE(extension, "KHR_draco_mesh_compression") << name;
                EXPECT_NE(extension, "EXT_texture_webp") << name;
                EXPECT_NE(extension, "EXT_meshopt_compression") << name << ": the cook writes KHR_meshopt_compression";
            }
            EXPECT_EQ(compareSoups(expected, triangleSoup(cooked), toleranceOf(compression)), "")
                << name << " " << meshCompressionName(compression) << " " << modelFamilyName(family);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(S53, ModelCookLoadFixtures,
    ::testing::Values("source.glb", "meshopt_c.glb", "meshopt_cc.glb", "meshopt_khr.glb", "meshopt_noq.glb", "quantized.glb",
                      "gt_meshopt.glb", "meshopt_c.gltf", "instancing.glb", "texture_transform.glb", "msft_lod.glb", "basisu.glb",
                      "draco.glb", "draco_sequential.glb", "webp.glb"),
    [](const ::testing::TestParamInfo<const char*>& info){
        std::string name = info.param;
        std::replace(name.begin(), name.end(), '.', '_');
        return name;
    });

TEST(ModelCookLoad, DracoAndWebpAreTranscodedAndSaySo){
    std::string error;
    std::vector<uint8_t> glb;
    ModelCookRecord record;
    ASSERT_TRUE(cookFile(s41Fixture("draco.glb"), settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless), ModelFamily::Desktop,
                         glb, record, error)) << error;
    EXPECT_EQ(record.transcoded, std::vector<std::string>{"KHR_draco_mesh_compression"});
    EXPECT_TRUE(std::any_of(record.diagnostics.begin(), record.diagnostics.end(), [](const GltfDiagnostic& d){
        return d.extension == "KHR_draco_mesh_compression" && d.cookFixes;
    })) << "the record keeps the loader's finding about the source";
    EXPECT_GT(record.after.triangles, 0);

    ASSERT_TRUE(cookFile(s41Fixture("webp.glb"), settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless), ModelFamily::Mobile,
                         glb, record, error)) << error;
    EXPECT_EQ(record.transcoded, std::vector<std::string>{"EXT_texture_webp"});
    GltfDocument doc;
    ASSERT_TRUE(doc.loadMemory(glb, fs::path(s41Fixture("webp.glb")).parent_path(), "cooked.glb", error)) << error;
    const GJson& root = doc.json();
    ASSERT_EQ(root["textures"].size(), 1u);
    EXPECT_FALSE(root["textures"][0].contains("extensions")) << root["textures"][0].dump();
    const size_t image = root["textures"][0]["source"].get<size_t>();
    EXPECT_EQ(root["images"][image]["mimeType"], "image/png");
    std::vector<uint8_t> png;
    ASSERT_TRUE(doc.imageBytes(image, png, error)) << error;
    tinygltf::Image decoded;
    std::string warn;
    ASSERT_TRUE(tinygltf::LoadImageData(&decoded, 0, &error, &warn, 0, 0, png.data(), (int)png.size(), nullptr)) << error;
    EXPECT_EQ(decoded.width, 16);
    EXPECT_EQ(decoded.height, 16);
}

TEST(ModelCookLoad, MsftLodLevelsBecomeTheBaseMeshesBakedLevels){
    const fs::path source = s41Fixture("msft_lod.glb");
    GltfDocument original;
    std::string error;
    ASSERT_TRUE(original.load(source, error)) << error;
    const GJson& sourceRoot = original.json();
    const GJson& lod = sourceRoot["nodes"][0]["extensions"]["MSFT_lod"];
    ASSERT_EQ(lod["ids"].size(), 2u);
    std::vector<uint8_t> glb;
    ModelCookRecord record;
    ASSERT_TRUE(cookFile(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless), ModelFamily::Desktop, glb, record, error)) << error;
    EXPECT_EQ(record.authoredLodPrimitives, 1);
    GltfDocument cooked;
    ASSERT_TRUE(cooked.loadMemory(glb, source.parent_path(), "cooked.glb", error)) << error;
    const GJson& root = cooked.json();
    const size_t base = root["nodes"][0]["mesh"].get<size_t>();
    const GJson& primitive = root["meshes"][base]["primitives"][0];
    ASSERT_TRUE(primitive.contains("extensions") && primitive["extensions"].contains("JE_mesh_lods")) << primitive.dump();
    const GJson& levels = primitive["extensions"]["JE_mesh_lods"]["levels"];
    ASSERT_EQ(levels.size(), 2u);
    // each baked level draws the authored level's triangles, over the base primitive's vertices
    std::vector<float> positions;
    int components = 0;
    ASSERT_TRUE(cooked.readAccessorFloats(primitive["attributes"]["POSITION"].get<size_t>(), positions, components, error)) << error;
    const float expectedCoverage[2] = {0.2f, 0.01f};
    for (size_t l = 0; l < 2; l++){
        const size_t node = lod["ids"][l].get<size_t>();
        const GJson& authored = sourceRoot["meshes"][sourceRoot["nodes"][node]["mesh"].get<size_t>()]["primitives"][0];
        std::vector<float> authoredPositions;
        std::vector<uint32_t> authoredList, bakedList;
        ASSERT_TRUE(original.readAccessorFloats(authored["attributes"]["POSITION"].get<size_t>(), authoredPositions, components, error)) << error;
        ASSERT_TRUE(original.readIndices(authored["indices"].get<size_t>(), authoredList, error)) << error;
        ASSERT_TRUE(cooked.readIndices(levels[l]["indices"].get<size_t>(), bakedList, error)) << error;
        EXPECT_EQ(trianglePositions(positions, bakedList), trianglePositions(authoredPositions, authoredList)) << "level " << l + 1;
        EXPECT_FLOAT_EQ(levels[l]["coverage"].get<float>(), expectedCoverage[l]);
        EXPECT_GT(levels[l]["error"].get<float>(), 0.0f);
    }
    // the runtime's reader takes them (contract C1), of this builder's recipe
    tinygltf::Model game;
    GltfNormalizeResult normalized;
    ASSERT_TRUE(loadAsGame(glb, source.parent_path(), game, normalized));
    const tinygltf::Primitive& drawn = game.meshes[(size_t)game.nodes[0].mesh].primitives[0];
    const tinygltf::Accessor& positionAccessor = game.accessors[(size_t)drawn.attributes.at("POSITION")];
    const AttributeDataType indexType = game.accessors[(size_t)drawn.indices].componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT
                                            ? AttributeDataType::UNSIGNED_INT : AttributeDataType::UNSIGNED_SHORT;
    int recipe = 0;
    const std::shared_ptr<MeshLodData> baked = MeshLodPool::readGltfLevels(game, drawn, indexType, positionAccessor.count, recipe);
    ASSERT_TRUE(baked);
    EXPECT_EQ(baked->numLevels, 3u);
    EXPECT_EQ(recipe, kMeshLodRecipe);
    // Generate LODs Off: no levels
    ASSERT_TRUE(cookFile(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless, GenerateLods::Off), ModelFamily::Desktop,
                         glb, record, error)) << error;
    EXPECT_EQ(record.authoredLodPrimitives, 0);
    EXPECT_TRUE(record.lods.empty());
}

TEST(ModelCookLoad, TheEditorLoadsTheCookedFileThroughItsRedirect){
    ScratchDir scratch("model-cook-redirect");
    std::string error;
    std::vector<uint8_t> glb;
    ModelCookRecord record;
    ASSERT_TRUE(cookFile(s41Fixture("draco.glb"), settingsOf(OptimizeMesh::Everything, MeshCompression::Medium), ModelFamily::Desktop,
                         glb, record, error)) << error;
    const fs::path cookedFile = scratch.path / "cooked.glb";
    std::ofstream(cookedFile, std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), (std::streamsize)glb.size());
    // the provider answers for an asset path (the editor's answers from its cook cache): the
    // source itself need not be there, the redirect reads the cook
    CookedModels::setProvider([&](const std::string& assetPath, CookedModelEntry& out){
        if (assetPath != "models/draco.glb")
            return false;
        out.file = cookedFile.generic_string();
        out.bytes = glb.size();
        return true;
    });
    const std::string sourcePath = "asset://models/draco.glb";
    EXPECT_EQ(MeshSystem::getImportedModelPath(sourcePath), cookedFile.generic_string());
    {
        Scene scene;
        LogCapture log;
        Model model(&scene);
        EXPECT_TRUE(model.loadModel(sourcePath));
        EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
        // what loaded is the cook: no Draco stream for the loader to decode
        for (const GltfDiagnostic& diagnostic : ModelLoadDiagnostics::get(sourcePath))
            EXPECT_NE(diagnostic.extension, "KHR_draco_mesh_compression") << diagnostic.text();
    }
    CookedModels::setProvider(nullptr);
}
