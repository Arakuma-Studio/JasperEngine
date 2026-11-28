// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// BF23 F1: RenderSystem loads a sub-emitter child's own Renderer, so an export has to ship its
// shader variants too (ShaderVariantCollector), down the levels EmitterRuntime binds. The
// fogged particle variants' height fog (docs/specs/S101-vapor.md R16) is cached under its own version.

#include "shader/ShaderVariantCollector.h"

#include "App.h"
#include "Scene.h"
#include "object/ParticleSystem.h"
#include "object/Primitive.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "pool/ShaderPool.h"

#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <string>

namespace ParticleShaderVariantsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    void defineEffect(const std::string& path, const std::string& json){
        particles::ParticleEffectDocument document;
        std::vector<particles::Diagnostic> diagnostics;
        ASSERT_TRUE(particles::ParticleEffectCodec::read(json, document, diagnostics)) << particles::ParticleEffectCodec::format(diagnostics);
        particles::ParticleEffectLibrary::set(path, document);
    }

    bool hasParticlesKey(const std::set<ShaderKey>& keys, const ParticlesShaderFlags& flags){
        return keys.count(ShaderPool::getShaderKey(ShaderType::PARTICLES, ShaderPool::getParticlesProperties(flags), 0)) > 0;
    }

}

TEST(ParticleShaderVariants, ChildrenShipTheirOwnVariants){
    editor::test::ensureEngine();
    // only the deepest child has trails, the middle one stretches: neither variant is the root's
    defineEffect("fx/variants-leaf.particles", R"({"version": 1, "modules": [
        {"type": "trails", "mode": "perParticle"}, {"type": "renderer", "renderMode": "none"}]})");
    defineEffect("fx/variants-middle.particles", R"({"version": 1,
        "subEmitters": [{"trigger": "birth", "effect": "fx/variants-leaf.particles"}],
        "modules": [{"type": "subEmitters"}, {"type": "renderer", "renderMode": "stretchedBillboard"}]})");
    Scene scene;
    ParticleSystem system(&scene);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1,
        "subEmitters": [{"trigger": "death", "effect": "fx/variants-middle.particles"}],
        "modules": [{"type": "subEmitters"}, {"type": "renderer", "renderMode": "billboard"}]})"));

    ShaderVariantContent content;
    content.registry = &scene;
    content.entities = scene.getEntityList();
    std::set<ShaderKey> keys;
    ShaderVariantCollector::collectScene(&scene, content, {}, keys);

    ParticlesShaderFlags stretch;
    stretch.stretch = true;
    ParticlesShaderFlags trail;
    trail.trail = true;
    EXPECT_TRUE(hasParticlesKey(keys, ParticlesShaderFlags())) << "the root's";
    EXPECT_TRUE(hasParticlesKey(keys, stretch)) << "the child's";
    EXPECT_TRUE(hasParticlesKey(keys, trail)) << "the child's child's";
    particles::ParticleEffectLibrary::clear();
}

TEST(ParticleShaderVariants, ASoftChildGivesTheMeshesTheirDepthPrograms){
    editor::test::ensureEngine();
    // a child's soft particles read the scene depth as the root's would (every mesh then ships a
    // depth program)
    auto depthKeys = [](const std::string& childRenderer){
        defineEffect("fx/variants-soft.particles", R"({"version": 1, "modules": [{"type": "renderer", )" + childRenderer + R"(}]})");
        Scene scene;
        Primitive plane(&scene);
        plane.createPlane(2.0f, 2.0f);
        ParticleSystem system(&scene);
        EXPECT_TRUE(system.setInlineEffect(R"({"version": 1,
            "subEmitters": [{"trigger": "death", "effect": "fx/variants-soft.particles"}],
            "modules": [{"type": "subEmitters"}, {"type": "renderer", "renderMode": "billboard"}]})"));
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        size_t depth = 0;
        for (const ShaderKey& key : keys){
            if (key.type == (uint16_t)ShaderType::DEPTH) depth++;
        }
        return depth;
    };
    const size_t plain = depthKeys(R"("renderMode": "billboard")");
    const size_t soft = depthKeys(R"("renderMode": "billboard", "shader": "library:particles", "shaderFeatures": ["SOFT"])");
    EXPECT_GT(soft, plain);
    particles::ParticleEffectLibrary::clear();
}

// the fogged particle variants carry the height fog from v75 (S101, after V1's v74) or later: an older
// cache holds 'Fog' particle programs that take a height fog's linear fields instead
TEST(ParticleShaderVariants, ShaderCacheVersionCoversTheHeightFog){
    const std::string name = editor::App::getUserShaderCacheDir().filename().string();
    ASSERT_EQ(name.rfind('v', 0), 0u) << name;
    EXPECT_GE(std::stoi(name.substr(1)), 75) << name;
}

}  // namespace ParticleShaderVariantsTest_cpp
