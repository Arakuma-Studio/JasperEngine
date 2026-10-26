// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The fluid simulation's shader variants in exports (ShaderVariantCollector): a scene with a
// FluidSimulationComponent ships the three FLUID programs, and a Sprite that shows its output
// (MATERIAL, lent at draw time) the textured mesh variant even without a texture of its own;
// a scene without one ships neither. (Scripts naming FluidSimulation: Project adds the same
// keys through addFluidKeys.)

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "component/FluidSimulationComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "pool/ShaderPool.h"

#include <gtest/gtest.h>

#include <memory>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    bool hasFluidKeys(const std::set<ShaderKey>& keys){
        return keys.count(ShaderPool::getShaderKey(ShaderType::FLUID, 0)) && keys.count(ShaderPool::getShaderKey(ShaderType::FLUID, 1u)) &&
               keys.count(ShaderPool::getShaderKey(ShaderType::FLUID, 2u)) && keys.count(ShaderPool::getShaderKey(ShaderType::FLUID, 3u));
    }

    size_t fluidKeys(const std::set<ShaderKey>& keys){
        size_t n = 0;
        for (ShaderKey key : keys)
            n += ShaderPool::getShaderTypeFromKey(key) == ShaderType::FLUID ? 1 : 0;
        return n;
    }

    // a MESH variant sampling texture coordinates ('Uv1')
    bool hasTexturedMesh(const std::set<ShaderKey>& keys){
        for (ShaderKey key : keys){
            if (ShaderPool::getShaderTypeFromKey(key) == ShaderType::MESH && (ShaderPool::getPropertiesFromKey(key) & (1u << 1)))
                return true;
        }
        return false;
    }

    std::set<ShaderKey> collect(Scene& scene){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        return keys;
    }

    Entity untexturedSprite(Scene& scene){
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<MeshComponent>(entity, {});
        SpriteComponent sprite;
        sprite.width = 64;
        sprite.height = 64;
        scene.addComponent<SpriteComponent>(entity, sprite);
        return entity;
    }

}

TEST(FluidShaderVariants, AddFluidKeys){
    std::set<ShaderKey> keys;
    ShaderVariantCollector::addFluidKeys(keys);
    EXPECT_TRUE(hasFluidKeys(keys));
    EXPECT_EQ(keys.size(), 4u);
}

TEST(FluidShaderVariants, SceneWithoutAFluid){
    auto scene = std::make_unique<Scene>();
    untexturedSprite(*scene);
    const std::set<ShaderKey> keys = collect(*scene);
    EXPECT_EQ(fluidKeys(keys), 0u);
    EXPECT_FALSE(hasTexturedMesh(keys)) << "an untextured sprite samples no texture";
}

TEST(FluidShaderVariants, SceneWithAMaterialFluid){
    auto scene = std::make_unique<Scene>();
    const Entity entity = untexturedSprite(*scene);
    FluidSimulationComponent fluid;
    fluid.output = FluidOutput::MATERIAL;
    scene->addComponent<FluidSimulationComponent>(entity, fluid);
    const std::set<ShaderKey> keys = collect(*scene);
    EXPECT_TRUE(hasFluidKeys(keys));
    EXPECT_TRUE(hasTexturedMesh(keys)) << "the sprite shows the fluid's output through its texture coordinates";
}

TEST(FluidShaderVariants, SceneWithAScreenFluid){
    auto scene = std::make_unique<Scene>();
    const Entity entity = scene->createEntity();
    FluidSimulationComponent fluid;
    fluid.output = FluidOutput::SCREEN;
    scene->addComponent<FluidSimulationComponent>(entity, fluid);
    EXPECT_TRUE(hasFluidKeys(collect(*scene)));
}
