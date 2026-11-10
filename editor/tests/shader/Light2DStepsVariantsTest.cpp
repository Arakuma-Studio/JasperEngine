// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S78 (docs/specs/S78-sprite-normals-improvements.md) in the variants a build ships:
// - the Light Steps variant ('Lst', MESH bit 54) of the 2D light path compiles for every backend an
//   export targets within the resource budget: the built-in sprite (plain, normal-mapped, 2D
//   shadows), library:sprite, and the built-in Lit lit by 2D lights with the per-object lights at
//   their heaviest (the light records form on GL) (R5, AC4);
// - an export ships 'Lst' only for a scene whose Light Steps are on (R3, AC3), the unlit 'L2d'
//   variants of a scene without a Light 2D only while its 2D ambient tints (R6, AC5), and both
//   states of each when the project's scripts set them.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Light2D.h"
#include "object/Sprite.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const uint64_t kUnlitBit = shaderPropertyBit(0);
    const uint64_t kLight2DBit = shaderPropertyBit(22);

    class Light2DStepsVariantKeys : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
    };

    struct Keys {
        size_t light2D = 0;      // MESH keys with 'L2d'
        size_t stepped = 0;      // ... and 'Lst'
        size_t steppedAlone = 0; // 'Lst' without 'L2d'
        std::string names;
    };

    Keys collect(Scene& scene, bool scriptsSetLight2D = false){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys, {}, false, false, {}, false, false, PbrTier::FULL,
                                             terraincore::TERRAIN_ALL_TIERS, scriptsSetLight2D);
        Keys out;
        const uint64_t lst = ShaderPool::getLight2DStepsProperty();
        for (ShaderKey key : keys){
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH)
                continue;
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            out.names += " " + ShaderPool::getShaderStr(ShaderType::MESH, properties, 0);
            if (properties & kLight2DBit){
                out.light2D++;
                if (properties & lst)
                    out.stepped++;
            }else if (properties & lst){
                out.steppedAlone++;
            }
        }
        return out;
    }

}

TEST(Light2DStepsVariants, CompileForEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const uint64_t lst = ShaderPool::getLight2DStepsProperty();
    // a sprite: unlit, uv1, vertex colour 4, texture rect, under 2D lights
    const uint64_t sprite = kUnlitBit | shaderPropertyBit(1) | shaderPropertyBit(10) | shaderPropertyBit(11) | kLight2DBit;
    // its normal map (normals, tangents, the map) and 2D shadows
    const uint64_t normalMapped = sprite | shaderPropertyBit(6) | shaderPropertyBit(7) | shaderPropertyBit(8);
    const uint64_t shadowed = normalMapped | shaderPropertyBit(23);
    // the built-in Lit under a shadowed sun with S57's capsule and contact shadows and the sixteen-light
    // tier, lit by 2D lights too: over GL's fragment uniforms, its 2D lights are light records
    const uint64_t heavyLit = shaderPropertyBit(1) | shaderPropertyBit(3) | shaderPropertyBit(4) | shaderPropertyBit(6) |
                              shaderPropertyBit(30) | kLight2DBit |
                              ShaderPool::getLightingProperty(ShaderPool::LightingFeature::CAPSULE_SHADOWS) |
                              ShaderPool::getLightingProperty(ShaderPool::LightingFeature::CONTACT_SHADOWS);
    const std::string library = "library:sprite";
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(library,
        [](const std::string&, std::string&){ return false; }, ShaderType::MESH);
    ASSERT_TRUE(sources.valid) << sources.error;
    const uint16_t libraryId = ShaderPool::registerCustomShader(library);
    struct Variant { uint64_t properties; uint16_t id; const CustomShaderSources* sources; };
    const Variant variants[] = {
        {sprite | lst, 0, nullptr},
        {normalMapped | lst, 0, nullptr},
        {shadowed | lst, 0, nullptr},
        {heavyLit | lst, 0, nullptr},
        {normalMapped | lst, libraryId, &sources},
        {shadowed | lst, libraryId, &sources},
    };
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        for (const Variant& variant : variants){
            const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, variant.id);
            const std::string name = ShaderPool::getShaderStr(ShaderType::MESH, variant.properties, 0) + (variant.id ? " " + library : std::string()) +
                                     " (" + ShaderPool::getShaderLangStr(backend) + ")";
            ShaderData data;
            std::string error, log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(key, backend, variant.sources, data, &error, &log)) << name << ": " << error << "\n" << log;
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
        }
    }
}

TEST_F(Light2DStepsVariantKeys, StepsOffShipNoLst){
    Scene scene;
    Sprite sprite(&scene);
    sprite.setSize(16, 16);
    Light2D light(&scene);
    const Keys keys = collect(scene);
    EXPECT_GT(keys.light2D, 0u) << keys.names;
    EXPECT_EQ(keys.stepped + keys.steppedAlone, 0u) << "'Lst' with the steps off:" << keys.names;
}

TEST_F(Light2DStepsVariantKeys, StepsOnShipLstWithL2dOnly){
    Scene scene;
    scene.setLight2DSteps(6);
    Sprite sprite(&scene);
    sprite.setSize(16, 16);
    Light2D light(&scene);
    const Keys keys = collect(scene);
    EXPECT_GT(keys.light2D, 0u) << keys.names;
    EXPECT_EQ(keys.stepped, keys.light2D) << "every 2D-lit key is stepped:" << keys.names;
    EXPECT_EQ(keys.steppedAlone, 0u) << keys.names;

    // a script switching them: both states ship
    const Keys scripted = collect(scene, true);
    EXPECT_GT(scripted.stepped, 0u) << scripted.names;
    EXPECT_LT(scripted.stepped, scripted.light2D) << "the smooth variants ship too:" << scripted.names;
}

TEST_F(Light2DStepsVariantKeys, AmbientWithoutLightsShipsL2dOnlyWhileItTints){
    {
        Scene neutral;
        Sprite sprite(&neutral);
        sprite.setSize(16, 16);
        const Keys keys = collect(neutral);
        EXPECT_EQ(keys.light2D, 0u) << "a neutral ambient and no Light 2D:" << keys.names;
        const Keys scripted = collect(neutral, true);
        EXPECT_GT(scripted.light2D, 0u) << "a script may darken the ambient:" << scripted.names;
    }
    {
        Scene dark;
        dark.setAmbientLight2D(1.0f, Vector3(0.3f, 0.3f, 0.4f));
        Sprite sprite(&dark);
        sprite.setSize(16, 16);
        const Keys keys = collect(dark);
        EXPECT_GT(keys.light2D, 0u) << "the dark ambient tints the sprite:" << keys.names;
    }
}
