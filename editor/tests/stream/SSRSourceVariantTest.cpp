// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SSR reflection source of a submesh and the G-buffer bits that read its occlusion and UV
// sets, as RenderSystem::getSubmeshShaderVariant decides them (also the export collector's)
// (docs/superpowers/specs/2026-11-04-ssr-probes-design.md 6.1, 6.2).

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"

#include <memory>

using namespace jasperengine;

namespace {

    const uint64_t gbufferOcc = shaderPropertyBit(16);
    const uint64_t gbufferUv2 = shaderPropertyBit(17);
    const uint64_t gbufferSto = shaderPropertyBit(10);

    // a lit textured mesh with two UV sets and an occlusion map
    SubmeshShaderInputs litInputs() {
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.texCoord2 = true;
        inputs.normal = true;
        inputs.baseColorTexture = true;
        inputs.occlusionTexture = true;
        return inputs;
    }

    MeshShaderEnvironment environment(bool lights, bool ibl) {
        MeshShaderEnvironment e;
        e.lights = lights;
        e.ibl = ibl;
        e.ssr = true;
        return e;
    }

    SSRSource sourceOf(const MeshShaderEnvironment& e, const SubmeshShaderInputs& inputs) {
        return RenderSystem::getSubmeshShaderVariant(e, inputs).ssrSource;
    }

}

TEST(SSRSourceVariant, UnlitAndMirrorSkip) {
    SubmeshShaderInputs inputs = litInputs();
    inputs.lighting = MaterialLighting::NONE;
    EXPECT_EQ(sourceOf(environment(true, true), inputs), SSRSource::SKIP);

    inputs = litInputs();
    inputs.receiveLights = false;
    EXPECT_EQ(sourceOf(environment(true, true), inputs), SSRSource::SKIP);

    inputs = litInputs();
    inputs.mirror = true;
    EXPECT_EQ(sourceOf(environment(true, true), inputs), SSRSource::SKIP);
}

TEST(SSRSourceVariant, LitWithoutLightsAdds) {
    // the scene has neither lights nor IBL: Lit draws unlit, there is no specular to swap
    EXPECT_EQ(sourceOf(environment(false, false), litInputs()), SSRSource::ADD);
}

TEST(SSRSourceVariant, ManifestLitAdds) {
    SubmeshShaderInputs inputs = litInputs();
    auto manifest = std::make_shared<ShaderManifest>();
    manifest->lighting = {ShaderLightingMode::PIXEL};
    inputs.manifest = manifest;
    inputs.lighting = MaterialLighting::PIXEL;
    EXPECT_EQ(sourceOf(environment(true, true), inputs), SSRSource::ADD);

    // an unlit library shader takes no SSR
    manifest->lighting.clear();
    inputs.lighting = MaterialLighting::NONE;
    EXPECT_EQ(sourceOf(environment(true, true), inputs), SSRSource::SKIP);
}

TEST(SSRSourceVariant, LitWithoutIBLIsAmbient) {
    EXPECT_EQ(sourceOf(environment(true, false), litInputs()), SSRSource::AMBIENT);
    // a mesh that does not receive IBL in a scene with it
    SubmeshShaderInputs inputs = litInputs();
    inputs.receiveIBL = false;
    EXPECT_EQ(sourceOf(environment(true, true), inputs), SSRSource::AMBIENT);
}

TEST(SSRSourceVariant, LitWithIBLIsEnvironment) {
    EXPECT_EQ(sourceOf(environment(true, true), litInputs()), SSRSource::ENVIRONMENT);
    // IBL alone (no lights) lights Lit too
    EXPECT_EQ(sourceOf(environment(false, true), litInputs()), SSRSource::ENVIRONMENT);
    // reflection probes without a sky
    MeshShaderEnvironment probes = environment(true, false);
    probes.reflectionProbes = true;
    EXPECT_EQ(sourceOf(probes, litInputs()), SSRSource::ENVIRONMENT);
}

TEST(SSRSourceVariant, OcclusionMapTakesOccOnlyWhenSwapped) {
    // swapped (Ambient, Environment): the G-buffer reads the occlusion the colour pass applies
    EXPECT_NE(RenderSystem::getSubmeshShaderVariant(environment(true, true), litInputs()).gbufferProperties & gbufferOcc, 0u);
    EXPECT_NE(RenderSystem::getSubmeshShaderVariant(environment(true, false), litInputs()).gbufferProperties & gbufferOcc, 0u);

    // added or skipped: nothing to weigh
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment(false, false), litInputs()).gbufferProperties & gbufferOcc, 0u);
    SubmeshShaderInputs unlit = litInputs();
    unlit.lighting = MaterialLighting::NONE;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment(true, true), unlit).gbufferProperties & gbufferOcc, 0u);

    // no map, or no UV for the colour pass to read it with
    SubmeshShaderInputs noMap = litInputs();
    noMap.occlusionTexture = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment(true, true), noMap).gbufferProperties & gbufferOcc, 0u);
    SubmeshShaderInputs noUV = litInputs();
    noUV.texCoord1 = false;
    noUV.texCoord2 = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment(true, true), noUV).gbufferProperties & gbufferOcc, 0u);

    // only built with SSR on
    MeshShaderEnvironment off = environment(true, true);
    off.ssr = false;
    const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(off, litInputs());
    EXPECT_FALSE(variant.gbuffer);
    EXPECT_EQ(variant.gbufferProperties, 0u);
}

TEST(SSRSourceVariant, MapsOnUV2TakeUv2) {
    // every map on UV1: no second set in the G-buffer
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment(true, true), litInputs()).gbufferProperties & gbufferUv2, 0u);

    SubmeshShaderInputs base = litInputs();
    base.baseColorTexCoord = 1;
    EXPECT_NE(RenderSystem::getSubmeshShaderVariant(environment(true, true), base).gbufferProperties & gbufferUv2, 0u);

    SubmeshShaderInputs occlusion = litInputs();
    occlusion.occlusionTexCoord = 1;
    EXPECT_NE(RenderSystem::getSubmeshShaderVariant(environment(true, true), occlusion).gbufferProperties & gbufferUv2, 0u);
    // the occlusion map is not read where nothing is swapped: no second set for it
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment(false, false), occlusion).gbufferProperties & gbufferUv2, 0u);

    SubmeshShaderInputs metallicRoughness = litInputs();
    metallicRoughness.metallicRoughnessTexture = true;
    metallicRoughness.metallicRoughnessTexCoord = 1;
    EXPECT_NE(RenderSystem::getSubmeshShaderVariant(environment(true, true), metallicRoughness).gbufferProperties & gbufferUv2, 0u);

    // a set the mesh does not have
    SubmeshShaderInputs oneSet = base;
    oneSet.texCoord2 = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment(true, true), oneSet).gbufferProperties & gbufferUv2, 0u);
}

TEST(SSRSourceVariant, StochasticTilingCoversTheOcclusionMap) {
    // the occlusion map alone still reads through Lit's lattice
    SubmeshShaderInputs inputs = litInputs();
    inputs.baseColorTexture = false;
    inputs.stochasticTiling = true;
    const uint64_t properties = RenderSystem::getSubmeshShaderVariant(environment(true, true), inputs).gbufferProperties;
    EXPECT_NE(properties & gbufferOcc, 0u);
    EXPECT_NE(properties & gbufferSto, 0u);
}

TEST(SSRSourceVariant, TerrainNeverTakesUv2) {
    SubmeshShaderInputs inputs = litInputs();
    inputs.terrain = true;
    inputs.texCoord1 = false;
    inputs.texCoord2 = false;
    inputs.baseColorTexCoord = 1;
    inputs.occlusionTexCoord = 1;
    const uint64_t properties = RenderSystem::getSubmeshShaderVariant(environment(true, true), inputs).gbufferProperties;
    EXPECT_EQ(properties & gbufferUv2, 0u);
    // a terrain reads its material's occlusion on its tiled UV, as Lit
    EXPECT_NE(properties & gbufferOcc, 0u);
}
