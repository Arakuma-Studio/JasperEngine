// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ShaderType::HTMLUI: both variants cross-compile for every backend with the reflection
// RmlRenderer looks up by name, and the shader pool knows the type's names. CPU only.

#include "pool/ShaderPool.h"
#include "registry/EntityRegistry.h"
#include "shader/ShaderVariantCompiler.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    const ShaderStage* findStage(const ShaderData& data, ShaderStageType type){
        for (const ShaderStage& stage : data.stages){
            if (stage.type == type)
                return &stage;
        }
        return nullptr;
    }

    bool hasAttribute(const ShaderStage& stage, const std::string& name){
        for (const ShaderAttr& attr : stage.attributes){
            if (attr.name == name)
                return true;
        }
        return false;
    }

    const ShaderUniformBlock* findBlock(const ShaderData& data, const std::string& name){
        for (const ShaderStage& stage : data.stages){
            for (const ShaderUniformBlock& block : stage.uniformblocks){
                if (block.name == name)
                    return &block;
            }
        }
        return nullptr;
    }

}

TEST(HtmlUIShaderTest, CompilesAllBackends){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader library";

    const ShaderBackend backends[] = {
        ShaderBackend::GLCore, ShaderBackend::GLES3, ShaderBackend::D3D11,
        ShaderBackend::MetalMacOS, ShaderBackend::MetalIOS, ShaderBackend::Vulkan
    };

    for (ShaderBackend backend : backends){
        for (bool textured : {false, true}){
            SCOPED_TRACE(ShaderPool::getShaderBackendName(backend) + (textured ? " textured" : " untextured"));

            const ShaderKey key = ShaderPool::getShaderKey(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(textured));
            ShaderData data;
            std::string error;
            std::string log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(key, backend, nullptr, data, &error, &log)) << error << "\n" << log;

            const ShaderStage* vertex = findStage(data, ShaderStageType::VERTEX);
            const ShaderStage* fragment = findStage(data, ShaderStageType::FRAGMENT);
            ASSERT_NE(vertex, nullptr);
            ASSERT_NE(fragment, nullptr);

            EXPECT_TRUE(hasAttribute(*vertex, "a_position"));
            EXPECT_TRUE(hasAttribute(*vertex, "a_color"));
            EXPECT_EQ(hasAttribute(*vertex, "a_texcoord1"), textured);
            EXPECT_GE(data.getAttrIndex(AttributeType::POSITION), 0);
            EXPECT_GE(data.getAttrIndex(AttributeType::COLOR), 0);
            EXPECT_EQ(data.getAttrIndex(AttributeType::TEXCOORD1) >= 0, textured);

            const ShaderUniformBlock* block = findBlock(data, "u_vs_htmluiParams");
            ASSERT_NE(block, nullptr);
            EXPECT_EQ(block->sizeBytes, 80u);
            EXPECT_GE(data.getUniformBlockIndexByName("u_vs_htmluiParams"), 0);

            const std::pair<int, int> texture = data.getTextureIndexByName("u_htmluiTexture");
            if (textured){
                EXPECT_GE(texture.first, 0);
                EXPECT_GE(texture.second, 0);
                ASSERT_FALSE(fragment->textureSamplerPairs.empty());
                EXPECT_EQ(fragment->textureSamplerPairs[0].textureName, "u_htmluiTexture");
                EXPECT_EQ(fragment->textureSamplerPairs[0].samplerName, "u_htmlui_smp");
            }else{
                EXPECT_EQ(texture.first, -1);
                EXPECT_TRUE(fragment->textures.empty());
            }
        }
    }
}

TEST(HtmlUIShaderTest, ShaderPoolNames){
    ShaderType type = ShaderType::MESH;
    ASSERT_TRUE(ShaderPool::parseShaderTypeToken("htmlui", type));
    EXPECT_EQ(type, ShaderType::HTMLUI);

    EXPECT_EQ(ShaderPool::getShaderTypeName(ShaderType::HTMLUI, true), "htmlui");
    EXPECT_EQ(ShaderPool::getShaderTypeName(ShaderType::HTMLUI, false), "HTML UI");
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::HTMLUI), 1);
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::HTMLUI, 0, true), "Tex");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::HTMLUI, 0, false), "Texture");

    EXPECT_EQ(ShaderPool::getHtmlUIProperties(false), 0u);
    EXPECT_EQ(ShaderPool::getHtmlUIProperties(true), 1u);

    // the .sdat basenames and their way back to the key (editor CLI, missing-shader lists)
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::HTMLUI, 0), "htmlui");
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::HTMLUI, 1), "htmlui_Tex");
    ShaderKey key = 0;
    ASSERT_TRUE(ShaderPool::parseShaderStr("htmlui_Tex", key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::HTMLUI, 1));
    ASSERT_TRUE(ShaderPool::parseShaderSpec("htmlui:Tex", key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::HTMLUI, 1));
    EXPECT_EQ(ShaderPool::getShaderTypeFromKey(key), ShaderType::HTMLUI);

    // appended: the stored enum values of every earlier type are unchanged. Since the merge of
    // origin/main the shader library's BLOOM (published first) comes between POSTPROCESS and
    // the HTML UI's types
    EXPECT_EQ((int)ShaderType::BLOOM, (int)ShaderType::POSTPROCESS + 1);
    EXPECT_EQ((int)ShaderType::HTMLUI, (int)ShaderType::BLOOM + 1);
    EXPECT_EQ((int)ShaderType::UI_EFFECT, (int)ShaderType::HTMLUI_POST + 1);
}

namespace {
    struct NeverRegisteredComponent { int value = 0; };
}

// the editor's HTMLUI shader-key collection asks the registry before it looks the document
// component up (ShaderVariantCollector, Project::collectSceneShaderKeys)
TEST(HtmlUIShaderTest, ComponentRegistrationQuery){
    EntityRegistry registry;
    EXPECT_TRUE(registry.isComponentRegistered<Transform>());
    EXPECT_TRUE(registry.isComponentRegistered<UIComponent>());
    EXPECT_FALSE(registry.isComponentRegistered<NeverRegisteredComponent>());
}
