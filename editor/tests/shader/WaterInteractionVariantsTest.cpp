// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Water Excluder and Water Wake variants (MESH bits 'Wex', 'Wwk';
// docs/superpowers/specs/2026-11-04-water-boats-design.md):
// - a shader takes them only when its manifest requires waterexclusion / waterwake and its scene
//   has the components; library:water does, the built-in Lit and other library shaders never;
// - the missing-variant fallback gives up the wake, then the exclusion, before anything else;
// - an export collects them only for scenes with the components;
// - the worst water variant with both compiles for every backend within the resource budget.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterWakeComponent.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <cstdio>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace water_interaction_variants_test {

    SubmeshShaderInputs waterInputs(){
        SubmeshShaderInputs inputs;
        inputs.normal = true;
        inputs.texCoord1 = true;
        inputs.manifest = ShaderManifests::get("library:water");
        return inputs;
    }

    class WaterInteractionVariantKeys : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
    };

    // the water material's MESH keys a scene's export collects: with 'Wex', with 'Wwk'
    std::pair<size_t, size_t> collectWaterBits(Scene& scene){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        std::pair<size_t, size_t> found{0, 0};
        for (ShaderKey key : keys){
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH)
                continue;
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            if (properties & ShaderPool::getWaterExclusionProperty())
                found.first++;
            if (properties & ShaderPool::getWaterWakeProperty())
                found.second++;
        }
        return found;
    }

}

using namespace water_interaction_variants_test;

TEST(WaterInteractionVariants, BitsOnlyWithRequireAndComponent){
    const uint64_t wex = ShaderPool::getWaterExclusionProperty();
    const uint64_t wwk = ShaderPool::getWaterWakeProperty();
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 42, true), "Wex");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 43, true), "Wwk");

    const SubmeshShaderInputs water = waterInputs();
    ASSERT_TRUE(water.manifest);
    EXPECT_TRUE(water.manifest->getPossibleRequire() & ShaderRequire_WaterExclusion);
    EXPECT_TRUE(water.manifest->getPossibleRequire() & ShaderRequire_WaterWake);

    MeshShaderEnvironment none;
    MeshShaderEnvironment excluders;
    excluders.waterExclusion = true;
    MeshShaderEnvironment wakes;
    wakes.waterWake = true;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(none, water).properties & (wex | wwk), 0u);
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(excluders, water).properties & (wex | wwk), wex);
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(wakes, water).properties & (wex | wwk), wwk);

    // a library shader that does not ask, and the built-in Lit: never
    SubmeshShaderInputs unlit = water;
    unlit.manifest = ShaderManifests::get("library:unlit");
    ASSERT_TRUE(unlit.manifest);
    MeshShaderEnvironment both = excluders;
    both.waterWake = true;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(both, unlit).properties & (wex | wwk), 0u);
    SubmeshShaderInputs lit = water;
    lit.manifest.reset();
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(both, lit).properties & (wex | wwk), 0u);
}

TEST(WaterInteractionVariants, FallbackGivesUpWakeThenExclusion){
    const uint64_t base = shaderPropertyBit(6) | shaderPropertyBit(21);   // normals, SSAO
    const uint64_t key = base | ShaderPool::getWaterExclusionProperty() | ShaderPool::getWaterWakeProperty();
    const std::vector<uint64_t> fallbacks = ShaderPool::getFallbackProperties(ShaderType::MESH, key);
    ASSERT_GE(fallbacks.size(), 2u);
    EXPECT_EQ(fallbacks[0], key & ~ShaderPool::getWaterWakeProperty());
    EXPECT_EQ(fallbacks[1], key & ~ShaderPool::getWaterExclusionProperty());
}

TEST(WaterInteractionVariants, ManifestRequire){
    ShaderManifest mesh;
    mesh.parse("// @shader mesh\n// @name \"Pond\"\n// @require waterexclusion waterwake\nvoid main(){}\n", "pond.frag");
    EXPECT_FALSE(mesh.hasErrors());
    EXPECT_TRUE(mesh.require & ShaderRequire_WaterExclusion);
    EXPECT_TRUE(mesh.require & ShaderRequire_WaterWake);
    // they come with the world position, and add no SURFACE_* define of their own
    EXPECT_TRUE(mesh.require & ShaderRequire_Position);
    EXPECT_EQ(ShaderManifest::getRequireDefines(ShaderRequire_WaterExclusion | ShaderRequire_WaterWake).size(), 0u);
    EXPECT_EQ(ShaderManifest::getRequireNames(ShaderRequire_WaterExclusion | ShaderRequire_WaterWake),
              (std::vector<std::string>{"waterexclusion", "waterwake"}));

    ShaderManifest post;
    post.parse("// @shader postprocess\n// @name \"Post\"\n// @require waterwake\nvoid main(){}\n", "post.frag");
    post.checkComplete("post.frag", false);
    EXPECT_TRUE(post.hasErrors()) << "a post-process shader has no water draws to bind them to";
}

// library:water with every feature and both bits: lit per pixel with shadows (the most textures),
// and with fog and the post colour (the most blocks); each within the per-stage budget, the bits'
// values in the water's own custom block (no uniform block of their own)
TEST(WaterInteractionVariants, WorstWaterVariantsCompileForEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    SubmeshShaderInputs inputs = waterInputs();
    inputs.lighting = MaterialLighting::PIXEL;
    std::vector<std::string> all;
    for (const ShaderManifestFeature& feature : inputs.manifest->features)
        all.push_back(feature.define);
    inputs.features = inputs.manifest->resolveFeatures(all);
    MeshShaderEnvironment shadows;
    shadows.lights = true;
    shadows.shadows = true;
    // the most lights a pass may give (the fragment's lighting block at its largest)
    MeshShaderEnvironment wide = shadows;
    wide.lightCapacity = 16;
    MeshShaderEnvironment fog;
    fog.fog = true;
    fog.postColor = true;
    const std::string reference = ShaderReference::withFeatures("library:water", inputs.features);
    const uint16_t customId = ShaderPool::registerCustomShader(reference);
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, nullptr, ShaderType::MESH);
    for (MeshShaderEnvironment environment : {shadows, fog, wide}){
        const SubmeshShaderVariant plain = RenderSystem::getSubmeshShaderVariant(environment, inputs);
        environment.waterExclusion = true;
        environment.waterWake = true;
        const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
        ASSERT_TRUE(variant.properties & ShaderPool::getWaterExclusionProperty());
        for (ShaderBackend backend : ShaderPool::getShaderBackends()){
            // as the library's compile matrix builds them (ShaderLibraryMatrix::compileVariants)
            ShaderData plainData, data;
            std::string error, log;
            const std::string name = ShaderPool::getShaderStr(ShaderType::MESH, variant.properties, customId) + " (" +
                                     ShaderPool::getShaderLangStr(backend) + ")";
            ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, plain.properties, customId), backend,
                                                       &sources, plainData, &error, &log)) << name << " without the bits: " << error << "\n" << log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, customId), backend,
                                                       &sources, data, &error, &log)) << name << ": " << error << "\n" << log;
            // the bits add two textures and no uniform block
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), ShaderVariantCompiler::checkResourceBudget(plainData)) << name;
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
            // GLES3 / WebGL2 guarantee 224 fragment uniform vectors, and sokol's GL backends
            // flatten the blocks into them
            size_t vectors = 0;
            for (const ShaderStage& stage : data.stages){
                if (stage.type == ShaderStageType::FRAGMENT){
                    for (const ShaderUniformBlock& block : stage.uniformblocks)
                        vectors += (block.sizeBytes + 15) / 16;
                }
            }
            if (backend == ShaderBackend::GLES3){
                std::printf("[water] %s: %zu fragment uniform vectors (limit 224)\n", name.c_str(), vectors);
                EXPECT_LE(vectors, 224u) << name;
            }
            EXPECT_NE(data.getTextureIndexByName("u_waterExclusionMaps").first, -1) << name;
            EXPECT_NE(data.getTextureIndexByName("u_waterWakeTexture").first, -1) << name;
            CustomUniformBlock custom;
            ASSERT_TRUE(custom.resolve(data, "u_fs_customParams")) << name;
            EXPECT_GE(custom.waterInteractionOffset, 0) << name;
            EXPECT_EQ(custom.waterWakeParamsOffset, custom.waterInteractionOffset + 34 * 16) << name;
        }
    }
}

TEST_F(WaterInteractionVariantKeys, ExportsCollectThemOnlyWithTheComponents){
    Scene scene;
    Primitive lake(&scene);
    lake.createGrid(20.0f, 20.0f, 8);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(lake.getEntity());
    mesh.submeshes[0].material.shader = "library:water";

    // red control: without the components no water key has the bits
    EXPECT_EQ(collectWaterBits(scene), std::make_pair(size_t(0), size_t(0)));

    Primitive boat(&scene);
    boat.createBox(1.0f, 1.0f, 3.0f);
    scene.addComponent<WaterExcluderComponent>(boat.getEntity(), {});
    const auto excluders = collectWaterBits(scene);
    EXPECT_GT(excluders.first, 0u);
    EXPECT_EQ(excluders.second, 0u);

    scene.addComponent<WaterWakeComponent>(boat.getEntity(), {});
    const auto both = collectWaterBits(scene);
    EXPECT_GT(both.first, 0u);
    EXPECT_GT(both.second, 0u);
}
