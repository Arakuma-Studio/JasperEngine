// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a model's load found, for the Inspector, the Project window badges and inspect_models
// (docs/specs/S53-model-import-cook.md R5): ModelLoadDiagnostics keeps each model's findings with
// the extension, the severity and the fix; GltfNormalize reports what it decoded, the MSFT_lod
// chains the cook folds (R4), the extensions the loader reads (contract C1's JE_mesh_lods among
// them), and leaves a file without these extensions as it was. CPU only.

#include "Engine.h"
#include "subsystem/GltfNormalize.h"
#include "subsystem/MeshSystem.h"

#include "GltfTriangleSoup.h"
#include "LogCapture.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    bool parseRaw(const std::string& path, tinygltf::Model& model){
        tinygltf::TinyGLTF loader;
        std::string err, warn;
        const bool binary = path.size() > 4 && path.compare(path.size() - 4, 4, ".glb") == 0;
        loader.SetImagesAsIs(true);
        return binary ? loader.LoadBinaryFromFile(&model, &err, &warn, path) : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    }

    const GltfDiagnostic* find(const std::vector<GltfDiagnostic>& diagnostics, const std::string& extension){
        for (const GltfDiagnostic& diagnostic : diagnostics){
            if (diagnostic.extension == extension)
                return &diagnostic;
        }
        return nullptr;
    }

}

TEST(GltfLoadDiagnostics, AnUnreadRequiredExtensionIsAnErrorWithItsFix){
    ModelLoadDiagnostics::clearAll();
    const uint64_t before = ModelLoadDiagnostics::revision();
    // a copy of its own: the Console reports a model's finding once per process, and other suites
    // load the fixture
    TempDir dir;
    std::ifstream in(JASPER_TEST_ASSET_DIR "/gltf/unsupported_required.gltf");
    const std::string path = dir.write("unsupported_required.gltf", std::string((std::istreambuf_iterator<char>(in)),
                                                                                 std::istreambuf_iterator<char>())).generic_string();
    LogCapture log;
    // (a parse every time: a Model load may take the pooled parse of an earlier test)
    tinygltf::Model parsed;
    std::string parseError;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(path, parsed, &parseError)) << parseError;
    const std::vector<GltfDiagnostic> diagnostics = ModelLoadDiagnostics::get(path);
    const GltfDiagnostic* found = find(diagnostics, "EXT_unknown_required_extension");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->severity, GltfDiagnosticSeverity::Error);
    EXPECT_NE(found->message.find("unsupported_required.gltf"), std::string::npos) << found->message;
    EXPECT_EQ(found->fix, "export the model again without EXT_unknown_required_extension");
    EXPECT_FALSE(found->cookFixes);
    EXPECT_TRUE(ModelLoadDiagnostics::hasErrors(path));
    EXPECT_NE(ModelLoadDiagnostics::revision(), before);
    // the Console has it once, with the fix
    EXPECT_EQ(log.count(D_LOG_ERROR, "export the model again without EXT_unknown_required_extension"), 1u);
    // another spelling of the path names the same model
    EXPECT_EQ(ModelLoadDiagnostics::get(dir.str() + "/./unsupported_required.gltf").size(), diagnostics.size());
    const std::vector<std::string> models = ModelLoadDiagnostics::models();
    EXPECT_EQ(models.size(), 1u);

    // a load that finds the same changes nothing a badge polls
    const uint64_t loaded = ModelLoadDiagnostics::revision();
    tinygltf::Model again;
    std::string error;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(path, again, &error)) << error;
    EXPECT_EQ(ModelLoadDiagnostics::revision(), loaded);
    ModelLoadDiagnostics::clear(path);
    EXPECT_TRUE(ModelLoadDiagnostics::get(path).empty());
    EXPECT_NE(ModelLoadDiagnostics::revision(), loaded);
}

TEST(GltfLoadDiagnostics, DracoWithoutTheEditorSaysTheCookTranscodesIt){
    tinygltf::Model model;
    ASSERT_TRUE(parseRaw(s41Fixture("draco.glb"), model));
    GltfNormalizeResult result;
    EXPECT_FALSE(GltfNormalize::normalize(model, "draco.glb", result));
    EXPECT_FALSE(result.ok);
    const GltfDiagnostic* found = find(result.diagnostics, "KHR_draco_mesh_compression");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->severity, GltfDiagnosticSeverity::Error);
    EXPECT_TRUE(found->cookFixes);
    EXPECT_NE(result.firstError().find("the cook transcodes it"), std::string::npos) << result.firstError();
}

TEST(GltfLoadDiagnostics, ReportsWhatItDecoded){
    tinygltf::Model model;
    ASSERT_TRUE(parseRaw(s41Fixture("meshopt_c.glb"), model));
    GltfNormalizeResult result;
    ASSERT_TRUE(GltfNormalize::normalize(model, "meshopt_c.glb", result)) << result.firstError();
    for (const char* name : {"EXT_meshopt_compression", "KHR_mesh_quantization", "KHR_texture_transform"})
        EXPECT_TRUE(std::count(result.handledExtensions.begin(), result.handledExtensions.end(), name)) << name;
    EXPECT_FALSE(result.needsEditorDecoders);
    EXPECT_FALSE(result.hasErrors());
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(GltfLoadDiagnostics, MsftLodChainForTheCook){
    tinygltf::Model model;
    ASSERT_TRUE(parseRaw(s41Fixture("msft_lod.glb"), model));
    const std::vector<GltfLodChain> before = GltfNormalize::readLodChains(model);
    ASSERT_EQ(before.size(), 1u);
    EXPECT_EQ(before[0].base.node, 0);
    EXPECT_EQ(before[0].base.mesh, 0);
    EXPECT_FLOAT_EQ(before[0].base.screenCoverage, 0.5f);
    ASSERT_EQ(before[0].levels.size(), 2u);
    EXPECT_EQ(before[0].levels[0].node, 1);
    EXPECT_EQ(before[0].levels[0].mesh, 1);
    EXPECT_FLOAT_EQ(before[0].levels[0].screenCoverage, 0.2f);
    EXPECT_EQ(before[0].levels[1].mesh, 2);
    EXPECT_FLOAT_EQ(before[0].levels[1].screenCoverage, 0.01f);

    GltfNormalizeResult result;
    ASSERT_TRUE(GltfNormalize::normalize(model, "msft_lod.glb", result));
    // the chain as the file had it, the lower levels off the scene, the meshes still there
    ASSERT_EQ(result.lodChains.size(), 1u);
    EXPECT_EQ(result.lodChains[0].levels.size(), 2u);
    EXPECT_EQ(result.lodChains[0].levels[1].mesh, 2);
    EXPECT_EQ(model.nodes[0].mesh, 0);
    EXPECT_EQ(model.nodes[1].mesh, -1);
    EXPECT_EQ(model.nodes[2].mesh, -1);
    EXPECT_EQ(model.meshes.size(), 3u);
}

TEST(GltfLoadDiagnostics, SupportedExtensionsListTheCooksLods){
    EXPECT_TRUE(GltfNormalize::isSupportedExtension("JE_mesh_lods"));
    for (const char* name : {"EXT_meshopt_compression", "KHR_meshopt_compression", "KHR_mesh_quantization", "KHR_texture_transform",
                             "KHR_texture_basisu", "EXT_mesh_gpu_instancing", "MSFT_lod", "KHR_draco_mesh_compression", "EXT_texture_webp"})
        EXPECT_TRUE(GltfNormalize::isSupportedExtension(name)) << name;
    EXPECT_FALSE(GltfNormalize::isSupportedExtension("EXT_unknown_required_extension"));
}

TEST(GltfLoadDiagnostics, AFileWithoutTheExtensionsIsLeftAsItWas){
    tinygltf::Model model;
    ASSERT_TRUE(parseRaw(s41Fixture("source.glb"), model));
    const tinygltf::Model original = model;
    GltfNormalizeResult result;
    ASSERT_TRUE(GltfNormalize::normalize(model, "source.glb", result));
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_TRUE(result.handledExtensions.empty());
    EXPECT_TRUE(model == original);
}
