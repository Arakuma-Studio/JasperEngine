// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's own glTF decoders (docs/specs/S53-model-import-cook.md R6, AC3; util/GltfEditorDecoders):
// KHR_draco_mesh_compression (gltf-transform's edgebreaker and sequential Draco of the S53 source
// model, engine/tests/core/fixtures/gltf/s41) parses to the source's triangles within Draco's
// quantization, and loads; EXT_texture_webp (gltfpack -tw) decodes to the source's pixels within
// WebP's loss. Both say the cook must transcode them (games never carry these decoders: the
// engine's GltfExtensionsTest has the game's side).

#include <gtest/gtest.h>

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"
#include "subsystem/GltfNormalize.h"
#include "subsystem/MeshSystem.h"

#include "LogCapture.h"
#include "subsystem/GltfTriangleSoup.h"


using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    bool parseWithImages(const std::string& path, tinygltf::Model& model, GltfNormalizeResult& result){
        tinygltf::TinyGLTF loader;
        loader.SetImagesAsIs(true);
        std::string err, warn;
        if (!loader.LoadBinaryFromFile(&model, &err, &warn, path))
            return false;
        return GltfNormalize::normalize(model, path, result);
    }

}

TEST(GltfExtensionsTest, DracoDecodesInTheEditor){
    ASSERT_TRUE(GltfNormalize::hasDracoDecoder());
    tinygltf::Model source;
    std::string error;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(s41Fixture("source.glb"), source, &error)) << error;
    for (const char* file : {"draco.glb", "draco_sequential.glb"}){
        tinygltf::Model draco;
        GltfNormalizeResult result;
        ASSERT_TRUE(parseWithImages(s41Fixture(file), draco, result)) << file << ": " << result.firstError();
        EXPECT_TRUE(result.needsEditorDecoders) << file;
        bool cookNote = false;
        for (const GltfDiagnostic& diagnostic : result.diagnostics)
            cookNote = cookNote || (diagnostic.extension == "KHR_draco_mesh_compression" && diagnostic.cookFixes);
        EXPECT_TRUE(cookNote) << file;
        // gltf-transform's Draco defaults: 14-bit positions, 10-bit normals, 12-bit UVs (over a
        // range of 2), 8-bit colours
        SoupTolerance tolerance;
        tolerance.position = 2e-3;
        tolerance.uv = 2e-3;
        tolerance.normalDot = 0.99;
        tolerance.color = 3.0 / 255.0;
        EXPECT_EQ(compareSoups(triangleSoup(source), triangleSoup(draco), tolerance), "") << file;
    }

    Scene scene;
    LogCapture log;
    Model model(&scene);
    EXPECT_TRUE(model.loadModel(s41Fixture("draco.glb")));
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

TEST(GltfExtensionsTest, WebpDecodesInTheEditor){
    ASSERT_TRUE(GltfNormalize::hasWebpDecoder());
    tinygltf::Model webp;
    GltfNormalizeResult result;
    ASSERT_TRUE(parseWithImages(s41Fixture("webp.glb"), webp, result)) << result.firstError();
    EXPECT_TRUE(result.needsEditorDecoders);
    ASSERT_EQ(webp.textures.size(), 1u);
    const int image = webp.textures[0].source;
    ASSERT_GE(image, 0) << "the WebP image is not the texture's source";
    const tinygltf::Image& encoded = webp.images[image];
    ASSERT_TRUE(GltfNormalize::isWebp(encoded.image.data(), encoded.image.size()));
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    std::string error;
    ASSERT_TRUE(GltfNormalize::decodeImage(encoded.image.data(), encoded.image.size(), rgba, width, height, error)) << error;
    ASSERT_EQ(width, 16);
    ASSERT_EQ(height, 16);
    // the source's checker (orange 230 120 40, blue 40 90 200 in 2 x 2 cells) through lossy WebP,
    // whose 4:2:0 chroma blends the cells' colours: every 4 x 4 block averages the checker's
    // 135 105 120, and the cells still differ in brightness
    for (int by = 0; by < height; by += 4){
        for (int bx = 0; bx < width; bx += 4){
            double mean[3] = {0, 0, 0};
            for (int y = by; y < by + 4; y++)
                for (int x = bx; x < bx + 4; x++)
                    for (int c = 0; c < 3; c++)
                        mean[c] += rgba[((size_t)y * width + x) * 4 + c] / 16.0;
            const double expected[3] = {135, 105, 120};
            for (int c = 0; c < 3; c++)
                EXPECT_NEAR(mean[c], expected[c], 20) << "block " << bx << ", " << by << " channel " << c;
        }
    }
    auto luma = [&](int x, int y){
        const uint8_t* p = &rgba[((size_t)y * width + x) * 4];
        return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    };
    EXPECT_GT(luma(0, 0), luma(2, 0)) << "the checker's cells are gone";

    Scene scene;
    LogCapture log;
    Model model(&scene);
    EXPECT_TRUE(model.loadModel(s41Fixture("webp.glb")));
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}
