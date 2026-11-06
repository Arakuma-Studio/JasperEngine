// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene's default shader property values (docs/superpowers/specs/
// 2026-11-04-code-editor-git-design.md 6.6): who inherits them, and a change flags exactly the
// components drawing with the default (no GPU: the flags the render system rewrites blocks from).

#include "LogCapture.h"
#include "Scene.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/PointsComponent.h"
#include "component/SkyboxComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/UIComponent.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    template<typename T>
    Entity withComponent(Scene& scene, const std::string& customShader = std::string()) {
        const Entity entity = scene.createEntity();
        scene.addComponent<T>(entity);
        T& component = scene.getComponent<T>(entity);
        component.customShader = customShader;
        component.needUpdateShaderUniforms = false;
        return entity;
    }

}

TEST(SceneDefaultUniforms, InheritedOnlyWithTheDefault) {
    Scene scene;
    // values without a default shader are kept but not inherited
    scene.setDefaultShaderUniform(ShaderType::MESH, "tint", Vector4(1, 0, 0, 1));
    EXPECT_TRUE(scene.hasDefaultShaderUniform(ShaderType::MESH, "tint"));
    EXPECT_EQ(scene.getInheritedShaderUniforms(ShaderType::MESH, ""), nullptr);

    scene.setDefaultMeshShader("shaders/Toon");
    const ShaderUniformValues* inherited = scene.getInheritedShaderUniforms(ShaderType::MESH, "");
    ASSERT_NE(inherited, nullptr);
    EXPECT_EQ(ShaderUniforms::get(*inherited, "tint"), Vector4(1, 0, 0, 1));
    // an object with its own shader never inherits
    EXPECT_EQ(scene.getInheritedShaderUniforms(ShaderType::MESH, "shaders/Own"), nullptr);
    // a type without values: nothing to look up (the zero-cost path)
    scene.setDefaultUIShader("shaders/UiTint");
    EXPECT_EQ(scene.getInheritedShaderUniforms(ShaderType::UI, ""), nullptr);

    EXPECT_EQ(scene.getDefaultShaderUniform(ShaderType::MESH, "tint"), Vector4(1, 0, 0, 1));
    EXPECT_TRUE(scene.removeDefaultShaderUniform(ShaderType::MESH, "tint"));
    EXPECT_FALSE(scene.removeDefaultShaderUniform(ShaderType::MESH, "tint"));
    EXPECT_EQ(scene.getInheritedShaderUniforms(ShaderType::MESH, ""), nullptr);

    // a type without a scene default refuses, and says so once per call site (an earlier test
    // may have had that once already)
    const test::LogCapture log;
    for (int i = 0; i < 3; i++) {
        scene.setDefaultShaderUniform(ShaderType::DEPTH, "tint", 1.0f);
        scene.removeDefaultShaderUniform(ShaderType::DEPTH, "tint");
    }
    EXPECT_LE(log.count(D_LOG_ERROR, "no scene default shader"), 2u);
    EXPECT_FALSE(scene.hasDefaultShaderUniform(ShaderType::DEPTH, "tint"));
    EXPECT_TRUE(scene.getDefaultShaderUniforms(ShaderType::DEPTH).empty());
    // the reserved members are the engine's
    scene.setDefaultShaderUniform(ShaderType::MESH, "time", 1.0f);
    EXPECT_FALSE(scene.hasDefaultShaderUniform(ShaderType::MESH, "time"));
}

TEST(SceneDefaultUniforms, FlagsOnlyInheritors) {
    Scene scene;
    const Entity inheriting = withComponent<MeshComponent>(scene);
    const Entity own = withComponent<MeshComponent>(scene, "shaders/Own");
    const Entity ui = withComponent<UIComponent>(scene);
    const Entity points = withComponent<PointsComponent>(scene);
    const Entity lines = withComponent<LineRendererComponent>(scene);
    const Entity trail = withComponent<TrailRendererComponent>(scene);
    const Entity trailOwn = withComponent<TrailRendererComponent>(scene, "shaders/OwnTrail");
    const Entity sky = withComponent<SkyboxComponent>(scene);

    scene.setDefaultShaderUniform(ShaderType::MESH, "tint", Vector4(1, 1, 1, 1));
    EXPECT_TRUE(scene.getComponent<MeshComponent>(inheriting).needUpdateShaderUniforms);
    EXPECT_FALSE(scene.getComponent<MeshComponent>(own).needUpdateShaderUniforms);
    EXPECT_FALSE(scene.getComponent<UIComponent>(ui).needUpdateShaderUniforms);

    // the same value again flags nothing
    scene.getComponent<MeshComponent>(inheriting).needUpdateShaderUniforms = false;
    scene.setDefaultShaderUniform(ShaderType::MESH, "tint", Vector4(1, 1, 1, 1));
    EXPECT_FALSE(scene.getComponent<MeshComponent>(inheriting).needUpdateShaderUniforms);

    scene.setDefaultShaderUniform(ShaderType::LINES, "width", 2.0f);
    EXPECT_TRUE(scene.getComponent<LineRendererComponent>(lines).needUpdateShaderUniforms);
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(trail).needUpdateShaderUniforms);
    EXPECT_FALSE(scene.getComponent<TrailRendererComponent>(trailOwn).needUpdateShaderUniforms);
    EXPECT_FALSE(scene.getComponent<PointsComponent>(points).needUpdateShaderUniforms);

    scene.setDefaultShaderUniforms(ShaderType::POINTS, {{"size", Vector4(3, 0, 0, 0)}});
    EXPECT_TRUE(scene.getComponent<PointsComponent>(points).needUpdateShaderUniforms);
    scene.setDefaultShaderUniform(ShaderType::UI, "alpha", 0.5f);
    EXPECT_TRUE(scene.getComponent<UIComponent>(ui).needUpdateShaderUniforms);
    scene.setDefaultShaderUniform(ShaderType::SKYBOX, "horizon", 0.5f);
    EXPECT_TRUE(scene.getComponent<SkyboxComponent>(sky).needUpdateShaderUniforms);

    // a removal flags too
    scene.getComponent<MeshComponent>(inheriting).needUpdateShaderUniforms = false;
    EXPECT_TRUE(scene.removeDefaultShaderUniform(ShaderType::MESH, "tint"));
    EXPECT_TRUE(scene.getComponent<MeshComponent>(inheriting).needUpdateShaderUniforms);
}
