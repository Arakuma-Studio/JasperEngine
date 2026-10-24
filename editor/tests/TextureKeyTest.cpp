// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Texture identity and cube faces: the TexturePool key carries a non-default sampler (two users
// of one file with different filter/wrap get their own GPU texture), and the six-face setters
// (Skybox, ReflectionProbe, shader cube uniforms, the MCP "cube" value) agree on the face order.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/ReflectionProbeComponent.h"
#include "component/SkyboxComponent.h"
#include "mcp/McpValues.h"
#include "object/environment/ReflectionProbe.h"
#include "object/environment/Skybox.h"
#include "pool/TexturePool.h"
#include "shader/ShaderManifest.h"
#include "texture/Texture.h"

#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {
    // engine face index order: 0 +X (right), 1 -X (left), 2 +Y (up), 3 -Y (down), 4 +Z (front), 5 -Z (back)
    const char* const faceFiles[6] = {"px.png", "nx.png", "py.png", "ny.png", "pz.png", "nz.png"};

    void expectFaces(const Texture& texture) {
        EXPECT_TRUE(texture.isCubeMap());
        for (size_t face = 0; face < 6; face++)
            EXPECT_EQ(texture.getPath(face), faceFiles[face]) << "face " << face;
    }
}

TEST(TextureRenderId, DefaultSamplerKeepsTheId) {
    Texture texture("sprites/rock.png");
    EXPECT_EQ(texture.getRenderId(), texture.getId());
    EXPECT_EQ(TexturePool::getRenderId("a.png", TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::REPEAT), "a.png");
}

TEST(TextureRenderId, SamplerIsPartOfTheKey) {
    Texture repeat("sprites/rock.png");
    Texture clamped("sprites/rock.png");
    clamped.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    clamped.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    Texture nearest("sprites/rock.png");
    nearest.setMagFilter(TextureFilter::NEAREST);

    // one file (one data pool entry), three GPU textures
    EXPECT_EQ(clamped.getId(), repeat.getId());
    EXPECT_EQ(nearest.getId(), repeat.getId());
    EXPECT_NE(clamped.getRenderId(), repeat.getRenderId());
    EXPECT_NE(nearest.getRenderId(), repeat.getRenderId());
    EXPECT_NE(nearest.getRenderId(), clamped.getRenderId());
    // the variants share the id's prefix, which TexturePool::invalidate() also drops
    EXPECT_EQ(clamped.getRenderId().rfind(repeat.getId() + "|sampler=", 0), 0u);

    // each field on its own counts
    std::vector<std::string> keys;
    for (int field = 0; field < 4; field++) {
        Texture texture("sprites/rock.png");
        if (field == 0) texture.setMinFilter(TextureFilter::NEAREST);
        if (field == 1) texture.setMagFilter(TextureFilter::NEAREST);
        if (field == 2) texture.setWrapU(TextureWrap::MIRRORED_REPEAT);
        if (field == 3) texture.setWrapV(TextureWrap::MIRRORED_REPEAT);
        for (const std::string& key : keys)
            EXPECT_NE(texture.getRenderId(), key) << "field " << field;
        EXPECT_NE(texture.getRenderId(), repeat.getRenderId()) << "field " << field;
        keys.push_back(texture.getRenderId());
    }

    // back to the default sampler: the plain id again
    clamped.setWrapU(TextureWrap::REPEAT);
    clamped.setWrapV(TextureWrap::REPEAT);
    EXPECT_EQ(clamped.getRenderId(), repeat.getRenderId());
}

TEST(TextureRenderId, NoIdNoKey) {
    Texture texture;
    texture.setMinFilter(TextureFilter::NEAREST);
    EXPECT_TRUE(texture.getRenderId().empty());
}

TEST(CubeFaces, SetCubePathsTakesFrontBackLeftRightUpDown) {
    Texture texture;
    texture.setCubePaths("pz.png", "nz.png", "nx.png", "px.png", "py.png", "ny.png");
    expectFaces(texture);
}

TEST(CubeFaces, SkyboxSixPathsAndSingleFacesAgree) {
    Scene scene;
    Skybox sky(&scene);
    sky.setTextures(faceFiles[0], faceFiles[1], faceFiles[2], faceFiles[3], faceFiles[4], faceFiles[5]);
    const Texture& six = scene.getComponent<SkyboxComponent>(sky.getEntity()).texture;
    expectFaces(six);

    Skybox faces(&scene);
    faces.setTexturePositiveX(faceFiles[0]);
    faces.setTextureNegativeX(faceFiles[1]);
    faces.setTexturePositiveY(faceFiles[2]);
    faces.setTextureNegativeY(faceFiles[3]);
    faces.setTexturePositiveZ(faceFiles[4]);
    faces.setTextureNegativeZ(faceFiles[5]);
    const Texture& single = scene.getComponent<SkyboxComponent>(faces.getEntity()).texture;
    expectFaces(single);
    EXPECT_EQ(single.getId(), six.getId());
}

TEST(CubeFaces, ReflectionProbeSixPaths) {
    Scene scene;
    ReflectionProbe probe(&scene);
    probe.setTextures(faceFiles[0], faceFiles[1], faceFiles[2], faceFiles[3], faceFiles[4], faceFiles[5]);
    expectFaces(scene.getComponent<ReflectionProbeComponent>(probe.getEntity()).texture);
}

TEST(CubeFaces, ShaderCubeUniformFaces) {
    std::vector<std::pair<std::string, Texture>> textures;
    EXPECT_TRUE(ShaderLibraryValues::setCubeTextureFaces(textures, "u_cube", "pz.png", "nz.png", "nx.png", "px.png", "py.png", "ny.png"));
    ASSERT_EQ(textures.size(), 1u);
    expectFaces(textures[0].second);
    // the same faces again change nothing
    EXPECT_FALSE(ShaderLibraryValues::setCubeTextureFaces(textures, "u_cube", "pz.png", "nz.png", "nx.png", "px.png", "py.png", "ny.png"));
    // the front face, as getShaderTexture documents
    EXPECT_EQ(ShaderLibraryValues::getTexture(textures, "u_cube"), "pz.png");
    EXPECT_TRUE(ShaderLibraryValues::setCubeTextureFaces(textures, "u_cube", "pz.png", "nz.png", "px.png", "nx.png", "py.png", "ny.png"));
}

TEST(CubeFaces, McpCubeReadsInTheOrderItWrites) {
    Texture texture;
    texture.setCubePaths("pz.png", "nz.png", "nx.png", "px.png", "py.png", "ny.png");
    const editor::mcp::Json json = editor::mcp::McpValues::textureToJson(nullptr, nullptr, texture);
    ASSERT_TRUE(json.is_object()) << json.dump();
    ASSERT_TRUE(json.contains("cube")) << json.dump();
    const editor::mcp::Json expected = {"pz.png", "nz.png", "nx.png", "px.png", "py.png", "ny.png"};
    EXPECT_EQ(json["cube"], expected) << json.dump();
}
