// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The LOD cross-fade variant (MESH bit 40 'Lfd', spec S54 R6, R10, AC6):
// - it compiles for every backend an export targets (GLSL 300 es included) within the resource
//   budget, with exactly its base variant's bindings, so a fading draw keeps the base's uniform
//   and texture slots (lod-crossfade.md §7, the slot risk);
// - an export collects it only for meshes that can fade: with Cross Fade, Mesh LOD or the mesh's
//   Fade Mode off, or on a fork, no fade variant ships (the zero-cost variant list).

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(LodFadeVariants, CompileForEveryBackendWithTheBaseBindings){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const uint64_t lit = shaderPropertyBit(1) | shaderPropertyBit(3) | shaderPropertyBit(4) | shaderPropertyBit(6) |
                         shaderPropertyBit(12) | shaderPropertyBit(19) | shaderPropertyBit(21);
    const std::vector<uint64_t> bases = {
        shaderPropertyBit(0) | shaderPropertyBit(1),     // unlit, textured
        lit,                                             // lit: lights, shadows, fog, IBL, SSAO
        lit | shaderPropertyBit(18),                     // instanced (the fade from the instance matrix)
        lit | shaderPropertyBit(13),                     // skinned
        lit | shaderPropertyBit(24),                     // alpha mask (a discard already)
    };
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        for (uint64_t base : bases){
            const uint64_t fade = base | ShaderPool::getLodFadeProperty();
            const std::string name = ShaderPool::getShaderStr(ShaderType::MESH, fade, 0) + " (" + ShaderPool::getShaderLangStr(backend) + ")";
            ShaderData baseData, fadeData;
            std::string error, log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, base), backend, nullptr, baseData, &error, &log))
                << name << " base: " << error << "\n" << log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, fade), backend, nullptr, fadeData, &error, &log))
                << name << ": " << error << "\n" << log;
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(fadeData), "") << name;
            EXPECT_TRUE(RenderSystem::sameLodFadeBindings(baseData, fadeData)) << name << " binds other slots than its base";
        }
    }
}

namespace {

    class LodFadeVariantKeys : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
    };

    // the MESH keys of a scene's export, with and without the fade bit
    struct MeshKeys {
        size_t plain = 0;
        size_t fade = 0;
        size_t instancedFade = 0;
    };

    MeshKeys collect(Scene& scene){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        MeshKeys out;
        for (ShaderKey key : keys){
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH)
                continue;
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            if (properties & ShaderPool::getLodFadeProperty()){
                out.fade++;
                if (properties & shaderPropertyBit(18))
                    out.instancedFade++;
            }else{
                out.plain++;
            }
        }
        return out;
    }
}

TEST_F(LodFadeVariantKeys, OnlyMeshesThatCanFadeShipTheFadeVariant){
    Scene scene;
    scene.setMeshLodEnabled(true);
    scene.setMeshLodCrossFade(true);
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 24, 12);

    const MeshKeys on = collect(scene);
    EXPECT_GT(on.plain, 0u);
    EXPECT_EQ(on.fade, on.plain) << "each variant the sphere draws with has its fade sibling";

    // red control of the rule: every switch off ships none, and the same plain variants
    scene.setMeshLodCrossFade(false);
    MeshKeys off = collect(scene);
    EXPECT_EQ(off.fade, 0u) << "Cross Fade off";
    EXPECT_EQ(off.plain, on.plain);
    scene.setMeshLodCrossFade(true);

    scene.setMeshLodEnabled(false);
    EXPECT_EQ(collect(scene).fade, 0u) << "Mesh LOD off";
    scene.setMeshLodEnabled(true);

    sphere.setLodFadeMode(LodFadeMode::NONE);
    EXPECT_EQ(collect(scene).fade, 0u) << "the mesh's Fade Mode None";
    sphere.setLodFadeMode(LodFadeMode::CROSS_FADE);

    sphere.setLodEnabled(false);
    EXPECT_EQ(collect(scene).fade, 0u) << "the mesh's LOD off";
    sphere.setLodEnabled(true);

    sphere.setCustomShader("shaders/myMesh");
    EXPECT_EQ(collect(scene).fade, 0u) << "a fork has no fade variant";
}

TEST_F(LodFadeVariantKeys, AnInstancedMeshShipsItsInstancedFadeVariant){
    Scene scene;
    scene.setMeshLodEnabled(true);
    scene.setMeshLodCrossFade(true);
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 24, 12);
    scene.addComponent<InstancedMeshComponent>(sphere.getEntity(), {});
    EXPECT_GT(collect(scene).instancedFade, 0u);
}
