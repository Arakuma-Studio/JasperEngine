// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The directional-only Lit variant (MESH bit 44 'Dlo', draw-perf spec 6.5):
// - it compiles for every backend an export targets within the resource budget, without the spot
//   mask atlas, the point shadow atlas and their block;
// - an export of a scene whose lights are all directional ships each 'Dlo' variant with its
//   general twin (a script may add a point light: the scene then draws the general one), and a
//   scene with a point or spot light, or its built-in Lit forked, ships none.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/LightingTiers.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // MESH 'Dlo', by its number
    constexpr uint64_t DIRECTIONAL_ONLY_BIT = 1ull << 44;

}

TEST(DirectionalOnlyVariants, CompileForEveryBackendWithoutPointAndSpotResources){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    // Uv1, Puc, Shw, Nor, Fog, Ibl, SSAO; then vertex lights, terrain PBR (its 16 textures)
    const uint64_t lit = shaderPropertyBit(1) | shaderPropertyBit(3) | shaderPropertyBit(4) | shaderPropertyBit(6) |
                         shaderPropertyBit(12) | shaderPropertyBit(19) | shaderPropertyBit(21);
    const std::vector<uint64_t> bases = {
        lit,
        lit | shaderPropertyBit(28),
        lit | shaderPropertyBit(17) | shaderPropertyBit(27),
    };
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        for (uint64_t base : bases){
            const uint64_t directional = base | DIRECTIONAL_ONLY_BIT;
            const std::string name = ShaderPool::getShaderStr(ShaderType::MESH, directional, 0) + " (" + ShaderPool::getShaderLangStr(backend) + ")";
            ShaderData data;
            std::string error, log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, directional), backend, nullptr, data, &error, &log))
                << name << ": " << error << "\n" << log;
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
            EXPECT_EQ(data.getTextureIndex(TextureShaderType::SPOTMASKATLAS).first, -1) << name << " binds the spot mask atlas";
            EXPECT_EQ(data.getTextureIndex(TextureShaderType::SHADOWPOINTATLAS).first, -1) << name << " binds the point shadow atlas";
            EXPECT_EQ(data.getUniformBlockIndexByName("u_fs_point_shadow_data"), -1) << name << " has the point shadow block";
            EXPECT_NE(data.getTextureIndex(TextureShaderType::SHADOWATLAS).first, -1) << name << " lost the sun's shadow atlas";
        }
    }
}

namespace {

    class DirectionalOnlyVariantKeys : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
    };

    // the scene's export MESH keys with the bit, and whether each has its twin
    struct Keys {
        size_t directional = 0;
        size_t withoutTwin = 0;
        size_t lit = 0;
        std::string names;
    };

    Keys collect(Scene& scene){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        Keys out;
        for (ShaderKey key : keys){
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH)
                continue;
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            out.names += " " + ShaderPool::getShaderStr(ShaderType::MESH, properties, 0);
            if (properties & shaderPropertyBit(3))
                out.lit++;
            if (!(properties & DIRECTIONAL_ONLY_BIT))
                continue;
            out.directional++;
            ShaderKey twin = key;
            twin.properties &= ~DIRECTIONAL_ONLY_BIT;
            if (!keys.count(twin))
                out.withoutTwin++;
        }
        return out;
    }

}

TEST_F(DirectionalOnlyVariantKeys, SunOnlySceneShipsBothVariants){
    Scene scene;
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setShadows(true);
    Primitive box(&scene);
    box.createBox(1.0f, 1.0f, 1.0f);

    const Keys keys = collect(scene);
    EXPECT_GT(keys.directional, 0u) << "no 'Dlo' variant for a sun-only scene:" << keys.names;
    EXPECT_EQ(keys.withoutTwin, 0u) << "a 'Dlo' variant without its general twin:" << keys.names;
}

TEST_F(DirectionalOnlyVariantKeys, PointOrSpotLightShipsNone){
    for (LightType type : {LightType::POINT, LightType::SPOT}){
        Scene scene;
        Light sun(&scene);
        sun.setType(LightType::DIRECTIONAL);
        Light local(&scene);
        local.setType(type);
        Primitive box(&scene);
        box.createBox(1.0f, 1.0f, 1.0f);
        const Keys keys = collect(scene);
        EXPECT_GT(keys.lit, 0u);
        EXPECT_EQ(keys.directional, 0u) << "a scene with a point or spot light ships 'Dlo':" << keys.names;
    }
}

TEST_F(DirectionalOnlyVariantKeys, AForkNeverGetsIt){
    Scene scene;
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    Primitive box(&scene);
    box.createBox(1.0f, 1.0f, 1.0f);
    box.setCustomShader("shaders/myMesh");
    const Keys keys = collect(scene);
    EXPECT_EQ(keys.directional, 0u) << "a fork of the Lit got 'Dlo':" << keys.names;
}

TEST_F(DirectionalOnlyVariantKeys, TwinsCoverLightModeTiled){
    // a point light arriving under Automatic or Tiled draws the general variant's tiled form
    // ('Til'): each 'Dlo' variant ships it too; Per Object only the plain twin
    const uint64_t tiledBit = ShaderPool::getLightingProperty(ShaderPool::LightingFeature::TILED_LIGHTS);
    for (LightMode mode : {LightMode::AUTOMATIC, LightMode::TILED, LightMode::PER_OBJECT}){
        LightingTiers::setLightMode(mode);
        Scene scene;
        Light sun(&scene);
        sun.setType(LightType::DIRECTIONAL);
        Primitive box(&scene);
        box.createBox(1.0f, 1.0f, 1.0f);
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        size_t directional = 0, withoutTiledTwin = 0;
        for (ShaderKey key : keys){
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH || !(key.properties & DIRECTIONAL_ONLY_BIT))
                continue;
            directional++;
            ShaderKey twin = key;
            twin.properties = (twin.properties & ~DIRECTIONAL_ONLY_BIT) | tiledBit;
            if (!keys.count(twin))
                withoutTiledTwin++;
        }
        EXPECT_GT(directional, 0u) << LightingTiers::getModeName(mode);
        if (mode == LightMode::PER_OBJECT)
            EXPECT_EQ(withoutTiledTwin, directional) << "Per Object ships no tiled twin";
        else
            EXPECT_EQ(withoutTiledTwin, 0u) << LightingTiers::getModeName(mode) << ": a 'Dlo' variant without its tiled twin";
    }
    LightingTiers::setLightMode(LightMode::AUTOMATIC);
}
