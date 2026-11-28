// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S100's Cavity shaders (docs/specs/S100-cavity.md): the fullscreen draw and the depth programs
// with normals ('Cvn') compile for every backend an export targets, a fork of depth.frag of any
// age gets the normals output (the compiler wraps its main), from the vertex stage when that
// stage writes one, else "none"; an export ships them only with Cavity.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <cstdio>
#include <set>

#include <string>
#include <vector>

namespace CavityVariantsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

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

    // a fork with a comment before #version and an output of its own at location 1, where the
    // normals output goes
    const char* const COMMENTED_FORK_FRAG =
        "// my depth fork\n"
        "/* clips by a texture\n"
        "   later */\n"
        "\n"
        "#version 450\n"
        "out vec4 frag_color;\n"
        "layout(location = 1) out vec4 frag_extra;\n"
        "in vec2 v_projZW;\n"
        "#include \"includes/depth_util.glsl\"\n"
        "void main() {\n"
        "    frag_color = encodeDepth(0.5 * v_projZW[0] / v_projZW[1] + 0.5);\n"
        "    frag_extra = vec4(1.0);\n"
        "}\n";

    // a fork without a #version line (glslang then takes its default version)
    const char* const NO_VERSION_FORK_FRAG =
        "// my depth fork, no version line\n"
        "out vec4 frag_color;\n"
        "in vec2 v_projZW;\n"
        "#include \"includes/depth_util.glsl\"\n"
        "void main() {\n"
        "    frag_color = encodeDepth(0.5 * v_projZW[0] / v_projZW[1] + 0.5);\n"
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
            if (path == "shaders/CommentedDepth.frag"){
                content = COMMENTED_FORK_FRAG;
                return true;
            }
            if (path == "shaders/NoVersionDepth.frag"){
                content = NO_VERSION_FORK_FRAG;
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

namespace {

    class CavityVariantKeys : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
    };

    // what an export of the scene ships of Cavity: its fullscreen variants and its depth
    // programs with normals
    struct CavityKeys {
        std::set<uint64_t> cavity;
        size_t depthWithNormals = 0;
        size_t depth = 0;
    };

    CavityKeys cavityVariantsCollect(Scene& scene, bool scriptsSetPostProcessing, const std::vector<std::string>& scriptFields = {}){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys, {}, false, scriptsSetPostProcessing, scriptFields);
        CavityKeys out;
        for (ShaderKey key : keys){
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            if (ShaderPool::getShaderTypeFromKey(key) == ShaderType::CAVITY)
                out.cavity.insert(properties);
            if (ShaderPool::getShaderTypeFromKey(key) == ShaderType::DEPTH){
                out.depth++;
                if (properties & ShaderPool::getDepthNormalsProperty())
                    out.depthWithNormals++;
            }
        }
        return out;
    }

}

// S100 AC4: an export with Cavity off everywhere and no script naming cavityEnabled ships no
// Cavity shader, though a script sets other post-processing; with it on, its type's variant and
// every depth program with normals; a script naming cavityEnabled ships both depth forms and the
// scene's type, one naming cavityType too every type
TEST_F(CavityVariantKeys, ExportShipsCavityOnlyWhenAScenesOrScriptsMayUseIt){
    Scene scene;
    Primitive cube(&scene);
    cube.createBox(1.0f, 1.0f, 1.0f);

    const CavityKeys off = cavityVariantsCollect(scene, false);
    EXPECT_TRUE(off.cavity.empty());
    EXPECT_EQ(off.depthWithNormals, 0u);

    PostProcessingSettings settings = scene.getPostProcessing();
    settings.cavityEnabled = true;
    settings.cavityType = CavityType::SCREEN;
    scene.setPostProcessing(settings);
    const CavityKeys on = cavityVariantsCollect(scene, false);
    EXPECT_EQ(on.cavity, std::set<uint64_t>{ShaderPool::getCavityProperties(true, false)});
    EXPECT_GT(on.depthWithNormals, 0u);
    EXPECT_EQ(on.depthWithNormals, on.depth) << "a depth program without normals ships though Cavity is always on";

    settings.cavityEnabled = false;
    scene.setPostProcessing(settings);
    for (const std::vector<std::string>& fields : {std::vector<std::string>{}, std::vector<std::string>{"bloomEnabled"}}){
        const CavityKeys other = cavityVariantsCollect(scene, true, fields);
        EXPECT_TRUE(other.cavity.empty()) << "a script setting post-processing without naming Cavity";
        EXPECT_EQ(other.depthWithNormals, 0u);
        EXPECT_EQ(other.depth, off.depth);
    }

    const CavityKeys switched = cavityVariantsCollect(scene, true, {"cavityEnabled"});
    EXPECT_EQ(switched.cavity, std::set<uint64_t>{ShaderPool::getCavityProperties(true, false)});
    EXPECT_GT(switched.depthWithNormals, 0u);
    EXPECT_EQ(switched.depth - switched.depthWithNormals, off.depth) << "both depth forms: Cavity may turn off again";

    const CavityKeys typed = cavityVariantsCollect(scene, true, {"cavityEnabled", "cavityType"});
    std::printf("[S100] depth programs %zu; a script naming cavityEnabled and cavityType adds %zu and %zu fullscreen\n",
                off.depth, typed.depthWithNormals, typed.cavity.size());
    EXPECT_EQ(typed.cavity.size(), 3u);
    EXPECT_EQ(typed.depthWithNormals, switched.depthWithNormals);

    // Cavity's names leave the uber variants as a script naming no switch has them
    EXPECT_EQ(ShaderVariantCollector::plausibleUberFeatures(settings, true, {"cavityEnabled", "cavityType"}),
              ShaderVariantCollector::plausibleUberFeatures(settings, true, {}));
}

TEST(CavityVariants, ForkWithCommentsBeforeVersionAndItsOwnLocation1Compiles){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const uint64_t properties = cavityDepthProperties(meshInputs());
    const std::string shader = "shaders/CommentedDepth.frag";
    const CustomShaderSources sources = forkSources(shader);
    ASSERT_TRUE(sources.valid) << sources.error;
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::DEPTH, properties, ShaderPool::registerCustomShader(shader));
    std::string vertex, fragment, error;
    ASSERT_TRUE(ShaderVariantCompiler::preprocess(key, ShaderBackend::GLCore, &sources, vertex, fragment, &error)) << error;
    EXPECT_NE(fragment.find("frag_depthNormal"), std::string::npos);
    expectCompiles(key, &sources);
}

// a fork without #version builds with Cavity on exactly as with Cavity off: it fails there (glslang's
// default version is ES 100, below SPIR-V's 310) and the mesh falls back to the built-in depth
// program, so the error is the compiler's, not Cavity's
TEST(CavityVariants, ForkWithoutVersionBuildsAsWithoutCavity){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const uint64_t properties = cavityDepthProperties(meshInputs());
    const uint64_t plain = properties & ~ShaderPool::getDepthNormalsProperty();
    const std::string shader = "shaders/NoVersionDepth.frag";
    const CustomShaderSources sources = forkSources(shader);
    ASSERT_TRUE(sources.valid) << sources.error;
    const uint16_t id = ShaderPool::registerCustomShader(shader);
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        ShaderData without, with;
        std::string withoutError, withError;
        const bool off = ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::DEPTH, plain, id), backend, &sources, without, &withoutError);
        const bool on = ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::DEPTH, properties, id), backend, &sources, with, &withError);
        EXPECT_EQ(on, off) << ShaderPool::getShaderLangStr(backend);
        EXPECT_EQ(withError, withoutError) << ShaderPool::getShaderLangStr(backend);
    }
}

}  // namespace CavityVariantsTest_cpp
