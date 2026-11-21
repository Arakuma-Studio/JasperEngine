// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92's Cavity shaders (docs/specs/S92-cavity.md): the fullscreen draw and the depth programs
// with normals ('Cvn') compile for every backend an export targets, a fork of depth.frag of any
// age gets the normals output (the compiler wraps its main), from the vertex stage when that
// stage writes one, else "none"; an export ships them only with Cavity.

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    void expectCompiles(ShaderKey key, const CustomShaderSources* sources = nullptr){
        ASSERT_GE(ShaderPool::getShaderBackends().size(), 6u);
        for (ShaderBackend backend : ShaderPool::getShaderBackends()){
            ShaderData data;
            std::string error, log;
            EXPECT_TRUE(ShaderVariantCompiler::compile(key, backend, sources, data, &error, &log))
                << ShaderPool::getShaderKeyText(key) << " on " << ShaderPool::getShaderLangStr(backend) << ": " << error << "\n" << log;
        }
    }

    // the built-in depth program of a submesh in a scene with Cavity on
    uint64_t cavityDepthProperties(SubmeshShaderInputs inputs){
        MeshShaderEnvironment environment;
        environment.cavityNormals = true;
        return RenderSystem::getSubmeshShaderVariant(environment, inputs).depthProperties;
    }

    SubmeshShaderInputs meshInputs(){
        SubmeshShaderInputs inputs;
        inputs.normal = true;
        inputs.texCoord1 = true;
        return inputs;
    }

    // a fork as a project holds it, made before Cavity: depth.frag's shape, and a depth.vert
    // without v_depthNormal
    const char* const OLD_FORK_FRAG =
        "#version 450\n"
        "out vec4 frag_color;\n"
        "in vec2 v_projZW;\n"
        "#include \"includes/depth_util.glsl\"\n"
        "void main() {\n"
        "    if (v_projZW.x > 1e6) discard;\n"
        "    frag_color = encodeDepth(0.5 * v_projZW[0] / v_projZW[1] + 0.5);\n"
        "}\n";
    const char* const OLD_FORK_VERT =
        "#version 450\n"
        "#include \"includes/object_depth_params.glsl\"\n"
        "in vec3 a_position;\n"
        "out vec2 v_projZW;\n"
        "void main() {\n"
        "    gl_Position = depthParams.mvpMatrix * vec4(a_position, 1.0);\n"
        "    v_projZW = gl_Position.zw;\n"
        "}\n";

    CustomShaderSources forkSources(const std::string& shader){
        return ShaderVariantCompiler::resolveCustomShaderSources(shader, [](const std::string& path, std::string& content){
            if (path == "shaders/OldDepth.frag"){
                content = OLD_FORK_FRAG;
                return true;
            }
            if (path == "shaders/OldDepth.vert"){
                content = OLD_FORK_VERT;
                return true;
            }
            return false;
        }, ShaderType::DEPTH);
    }

}

TEST(CavityVariants, FullscreenVariantsCompileOnEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    for (int type = 0; type < 3; type++){
        const uint64_t properties = ShaderPool::getCavityProperties(type != 1, type != 0);
        expectCompiles(ShaderPool::getShaderKey(ShaderType::CAVITY, properties));
    }
}

TEST(CavityVariants, DepthProgramsWithNormalsCompileOnEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    std::vector<SubmeshShaderInputs> cases;
    cases.push_back(meshInputs());
    {
        SubmeshShaderInputs inputs = meshInputs();
        inputs.skinning = true;
        cases.push_back(inputs);
    }
    {
        SubmeshShaderInputs inputs = meshInputs();
        inputs.instanced = true;
        inputs.instanceFade = true;
        cases.push_back(inputs);
    }
    {
        SubmeshShaderInputs inputs = meshInputs();
        inputs.alphaMode = MaterialAlphaMode::MASK;
        inputs.baseColorTexture = true;
        cases.push_back(inputs);
    }
    {
        SubmeshShaderInputs inputs = meshInputs();
        inputs.terrain = true;
        inputs.texCoord1 = false;
        cases.push_back(inputs);
    }
    {
        // a mesh without normals writes "none"
        SubmeshShaderInputs inputs = meshInputs();
        inputs.normal = false;
        cases.push_back(inputs);
    }
    for (const SubmeshShaderInputs& inputs : cases){
        const uint64_t properties = cavityDepthProperties(inputs);
        ASSERT_NE(properties & ShaderPool::getDepthNormalsProperty(), 0u);
        expectCompiles(ShaderPool::getShaderKey(ShaderType::DEPTH, properties));
    }
    // and an automatically instanced batch's (S55)
    const uint64_t batched = cavityDepthProperties(meshInputs()) | ShaderPool::getAutoInstancingProperty(ShaderType::DEPTH);
    expectCompiles(ShaderPool::getShaderKey(ShaderType::DEPTH, batched));
}

TEST(CavityVariants, LibraryDepthProgramsWriteNormals){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const uint64_t properties = cavityDepthProperties(meshInputs());
    for (const char* shader : {"library:simple_lit", "library:dissolve#WORLD_NOISE", "library:spin_bob"}){
        const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(shader,
            [](const std::string&, std::string&){ return false; }, ShaderType::DEPTH);
        ASSERT_TRUE(sources.valid) << shader << ": " << sources.error;
        const ShaderKey key = ShaderPool::getShaderKey(ShaderType::DEPTH, properties, ShaderPool::registerCustomShader(shader));
        std::string vertex, fragment, error;
        ASSERT_TRUE(ShaderVariantCompiler::preprocess(key, ShaderBackend::GLCore, &sources, vertex, fragment, &error)) << shader << ": " << error;
        EXPECT_NE(fragment.find("frag_depthNormal"), std::string::npos) << shader << " has no normals output";
        EXPECT_NE(fragment.find("v_depthNormal"), std::string::npos) << shader << "'s vertex stage writes the normal";
        expectCompiles(key, &sources);
    }
}

TEST(CavityVariants, OldDepthForksWriteNoNormalsAndCompile){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const uint64_t properties = cavityDepthProperties(meshInputs());

    // a fragment-only fork on the engine's depth.vert: its normal comes from the vertex stage
    {
        const std::string shader = "shaders/OldDepth.frag";
        const CustomShaderSources sources = forkSources(shader);
        ASSERT_TRUE(sources.valid) << sources.error;
        const ShaderKey key = ShaderPool::getShaderKey(ShaderType::DEPTH, properties, ShaderPool::registerCustomShader(shader));
        std::string vertex, fragment, error;
        ASSERT_TRUE(ShaderVariantCompiler::preprocess(key, ShaderBackend::GLCore, &sources, vertex, fragment, &error)) << error;
        EXPECT_NE(fragment.find("frag_depthNormal"), std::string::npos);
        EXPECT_NE(fragment.find("v_depthNormal"), std::string::npos) << "the engine's depth.vert writes the normal";
        expectCompiles(key, &sources);
    }
    // both stages forked before Cavity: no normal from its vertex stage, "none" everywhere
    {
        const std::string shader = "shaders/OldDepth";
        const CustomShaderSources sources = forkSources(shader);
        ASSERT_TRUE(sources.valid) << sources.error;
        const ShaderKey key = ShaderPool::getShaderKey(ShaderType::DEPTH, properties, ShaderPool::registerCustomShader(shader));
        std::string vertex, fragment, error;
        ASSERT_TRUE(ShaderVariantCompiler::preprocess(key, ShaderBackend::GLCore, &sources, vertex, fragment, &error)) << error;
        EXPECT_NE(fragment.find("frag_depthNormal"), std::string::npos);
        EXPECT_EQ(fragment.find("v_depthNormal"), std::string::npos) << "a vertex stage without the normal leaves the fragment none to read";
        expectCompiles(key, &sources);
    }
}
