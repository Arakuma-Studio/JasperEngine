// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shadow blocks the engine uploads (fs_shadow_data_t, fs_point_shadow_data_t in
// RenderSystem.h) against the blocks the compiled Lit shader declares (includes/shadow_data.glsl,
// spec S10): the same size and every member at the same offset. And what the High filters'
// variant bit ('Shq') adds: the raw-depth view of the atlas, on GLES3 too where it fits
// (ShadowPcssGles holds where it does not).

#include "subsystem/RenderSystem.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using namespace jasperengine;

namespace {

    // Lit receiving shadows: Puc, Shw, Nor (and Shq)
    ShaderKey litKey(bool highFilters){
        uint64_t properties = (1u << 3) | (1u << 4) | (1u << 6);
        if (highFilters)
            properties |= ShaderPool::getShadowsHQProperty();
        return ShaderPool::getShaderKey(ShaderType::MESH, properties);
    }

    const ShaderStage* fragmentOf(const ShaderData& data){
        for (const ShaderStage& stage : data.stages){
            if (stage.type == ShaderStageType::FRAGMENT)
                return &stage;
        }
        return nullptr;
    }

    const ShaderUniformBlock* blockOf(const ShaderStage& stage, const std::string& name){
        for (const ShaderUniformBlock& block : stage.uniformblocks){
            if (block.name == name)
                return &block;
        }
        return nullptr;
    }

    // the offset of a member ("shadowData.lightVP"), -1 when the block has none of that name
    int memberOffset(const ShaderUniformBlock& block, const std::string& name){
        for (const ShaderUniform& uniform : block.uniforms){
            if (uniform.name == name)
                return (int)uniform.offset;
        }
        return -1;
    }

    bool hasTexture(const ShaderStage& stage, const std::string& name){
        for (const ShaderTexture& texture : stage.textures){
            if (texture.name == name)
                return true;
        }
        return false;
    }

    ShaderData compiled(ShaderKey key, ShaderBackend backend){
        ShaderData data;
        std::string error, log;
        EXPECT_TRUE(ShaderVariantCompiler::compile(key, backend, nullptr, data, &error, &log)) << error << "\n" << log;
        return data;
    }

}

TEST(ShadowDataLayout, BlocksMatchTheShader){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
    const ShaderData data = compiled(litKey(true), ShaderBackend::Vulkan);
    const ShaderStage* fragment = fragmentOf(data);
    ASSERT_NE(fragment, nullptr);

    const ShaderUniformBlock* shadow = blockOf(*fragment, "u_fs_shadow_data");
    ASSERT_NE(shadow, nullptr);
    EXPECT_EQ(shadow->sizeBytes, sizeof(fs_shadow_data_t));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.header"), (int)offsetof(fs_shadow_data_t, header));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.fitCameraPosition"), (int)offsetof(fs_shadow_data_t, fitCameraPosition));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.fitCameraForward"), (int)offsetof(fs_shadow_data_t, fitCameraForward));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.lightVP"), (int)offsetof(fs_shadow_data_t, lightVP));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.atlasRect"), (int)offsetof(fs_shadow_data_t, atlasRect));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.bias"), (int)offsetof(fs_shadow_data_t, bias));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.split"), (int)offsetof(fs_shadow_data_t, split));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.filtering"), (int)offsetof(fs_shadow_data_t, filtering));
    EXPECT_EQ(memberOffset(*shadow, "shadowData.fade"), (int)offsetof(fs_shadow_data_t, fade));

    const ShaderUniformBlock* point = blockOf(*fragment, "u_fs_point_shadow_data");
    ASSERT_NE(point, nullptr);
    EXPECT_EQ(point->sizeBytes, sizeof(fs_point_shadow_data_t));
    EXPECT_EQ(memberOffset(*point, "pointShadowData.faceRect"), (int)offsetof(fs_point_shadow_data_t, faceRect));
    EXPECT_EQ(memberOffset(*point, "pointShadowData.projection"), (int)offsetof(fs_point_shadow_data_t, projection));
    EXPECT_EQ(memberOffset(*point, "pointShadowData.bias"), (int)offsetof(fs_point_shadow_data_t, bias));
    EXPECT_EQ(memberOffset(*point, "pointShadowData.filtering"), (int)offsetof(fs_point_shadow_data_t, filtering));
    EXPECT_EQ(memberOffset(*point, "pointShadowData.state"), (int)offsetof(fs_point_shadow_data_t, state));

    // fragment-stage coordinates: no shadow varyings, no vertex shadow block
    for (const ShaderStage& stage : data.stages){
        EXPECT_EQ(blockOf(stage, "u_vs_shadows"), nullptr);
        EXPECT_EQ(blockOf(stage, "u_fs_shadows"), nullptr);
    }
}

TEST(ShadowDataLayout, HighFiltersAddTheRawDepthView){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
    const ShaderData high = compiled(litKey(true), ShaderBackend::GLCore);
    const ShaderData medium = compiled(litKey(false), ShaderBackend::GLCore);
    const ShaderData gles = compiled(litKey(true), ShaderBackend::GLES3);
    ASSERT_NE(fragmentOf(high), nullptr);
    ASSERT_NE(fragmentOf(medium), nullptr);
    ASSERT_NE(fragmentOf(gles), nullptr);
    EXPECT_TRUE(hasTexture(*fragmentOf(high), "u_shadowAtlasRaw"));
    EXPECT_FALSE(hasTexture(*fragmentOf(medium), "u_shadowAtlasRaw"));
    EXPECT_TRUE(hasTexture(*fragmentOf(gles), "u_shadowAtlasRaw")) << "GLES3 has the texture units for it";
    EXPECT_TRUE(hasTexture(*fragmentOf(medium), "u_shadowAtlas"));
    EXPECT_TRUE(hasTexture(*fragmentOf(medium), "u_shadowPointAtlas"));
}
