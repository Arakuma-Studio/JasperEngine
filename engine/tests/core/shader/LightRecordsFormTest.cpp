// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Which lit variants read their lights from the light records (ShaderVariantCompiler::compile,
// includes/lights.glsl LIGHTS_FS_RECORDS: docs/superpowers/specs/2026-11-04-water-reflections-design.md,
// Follow-ups):
// - per backend, against its own limits: a Lit over GLES3's 224 fragment vectors but within desktop
//   GL's 256 takes the records on GLES3 only, and keeps its blocks, PCSS's raw view and the spot
//   masks on desktop GL, D3D11, Metal and Vulkan;
// - a colour variant and its OIT twin (u_fs_oit) take the same form, so the OIT draws can apply the
//   colour variant's slots;
// - a fork reading the lights' blocks itself keeps them, with a warning, instead of failing.

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;

namespace light_records_form_test {

    struct FormCompiled {
        ShaderData data;
        std::string log;
        std::string name;
        bool ok = false;
    };

    FormCompiled compileForm(uint64_t properties, ShaderBackend backend, const std::string& reference,
                             const ShaderVariantCompiler::FileReader& reader = nullptr){
        FormCompiled out;
        uint16_t customId = 0;
        CustomShaderSources sources;
        if (!reference.empty()){
            customId = ShaderPool::registerCustomShader(reference);
            sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, reader ? reader : ShaderVariantCompiler::FileReader(), ShaderType::MESH);
            EXPECT_TRUE(sources.valid) << sources.error;
        }
        out.name = ShaderPool::getShaderStr(ShaderType::MESH, properties, customId) + " (" + ShaderPool::getShaderLangStr(backend) + ")";
        std::string error;
        out.ok = ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, properties, customId), backend,
                                                reference.empty() ? nullptr : &sources, out.data, &error, &out.log);
        if (!out.ok)
            out.log += "\n" + error;
        return out;
    }

    bool readsRecords(FormCompiled& compiled){
        return compiled.data.getTextureIndexByName("u_lightRecords").first != -1;
    }

    size_t formFragmentVectors(const ShaderData& data){
        size_t vectors = 0;
        for (const ShaderStage& stage : data.stages){
            if (stage.type == ShaderStageType::FRAGMENT){
                for (const ShaderUniformBlock& block : stage.uniformblocks)
                    vectors += (block.sizeBytes + 15) / 16;
            }
        }
        return vectors;
    }

    // a textured, normal-mapped Lit with sixteen lights, shadows at a High-filter tier, IBL, SSAO,
    // fog and the post colour: with a lobe (clearcoat's block) 230 fragment vectors in 7 blocks
    uint64_t desktopHeavyLit(){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.baseColorTexture = true;
        inputs.normalTexture = true;
        MeshShaderEnvironment environment;
        environment.lights = true;
        environment.lightCapacity = 16;
        environment.shadows = true;
        environment.shadowsHQ = true;
        environment.ibl = true;
        environment.ssao = true;
        environment.postColor = true;
        environment.fog = true;
        return RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    }

}

using namespace light_records_form_test;

TEST(LightRecordsForm, EachBackendDecidesByItsOwnLimits){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::string clearcoat = ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, {"PBR_CLEARCOAT"});
    const uint64_t properties = desktopHeavyLit();

    FormCompiled gles = compileForm(properties, ShaderBackend::GLES3, clearcoat);
    ASSERT_TRUE(gles.ok) << gles.name << ": " << gles.log;
    EXPECT_TRUE(readsRecords(gles)) << gles.name << " is over GLES3's 224 fragment vectors in its blocks";
    EXPECT_LE(formFragmentVectors(gles.data), 224u) << gles.name;

    for (ShaderBackend backend : {ShaderBackend::GLCore, ShaderBackend::D3D11, ShaderBackend::MetalMacOS, ShaderBackend::Vulkan}){
        FormCompiled desktop = compileForm(properties, backend, clearcoat);
        ASSERT_TRUE(desktop.ok) << desktop.name << ": " << desktop.log;
        EXPECT_FALSE(readsRecords(desktop)) << desktop.name << " fits its backend in its blocks: GLES3's limit cost it the records";
        EXPECT_NE(desktop.data.getTextureIndex(TextureShaderType::SHADOWATLASRAW).first, -1) << desktop.name << " lost PCSS";
        EXPECT_NE(desktop.data.getTextureIndex(TextureShaderType::SPOTMASKATLAS).first, -1) << desktop.name << " lost the spot masks";
        EXPECT_EQ(desktop.log.find(ShaderVariantCompiler::PCSS_LEFT_OUT), std::string::npos) << desktop.name;
        EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(desktop.data), "") << desktop.name;
    }
}

// a Lit at 8 blocks with contact shadows: its OIT twin's u_fs_oit is a ninth
TEST(LightRecordsForm, OitTwinTakesTheColourVariantsForm){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    SubmeshShaderInputs inputs;
    inputs.texCoord1 = true;
    inputs.normal = true;
    inputs.baseColorTexture = true;
    MeshShaderEnvironment environment;
    environment.lights = true;
    environment.shadows = true;
    environment.contactShadows = true;
    environment.ibl = true;
    environment.reflectionProbes = true;
    environment.fog = true;
    environment.postColor = true;
    const uint64_t color = RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    ASSERT_TRUE(color & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::CONTACT_SHADOWS));
    const uint64_t oit = color | ShaderPool::getLightingProperty(ShaderPool::LightingFeature::OIT);

    for (ShaderBackend backend : {ShaderBackend::GLES3, ShaderBackend::GLCore, ShaderBackend::Vulkan}){
        FormCompiled colorVariant = compileForm(color, backend, std::string());
        FormCompiled oitVariant = compileForm(oit, backend, std::string());
        ASSERT_TRUE(colorVariant.ok) << colorVariant.name << ": " << colorVariant.log;
        ASSERT_TRUE(oitVariant.ok) << oitVariant.name << ": " << oitVariant.log;
        EXPECT_EQ(readsRecords(colorVariant), readsRecords(oitVariant)) << colorVariant.name << " and its OIT twin take different forms";
        // the engine's check (RenderSystem::finishSubmeshOit): every colour block at the same slot
        for (const ShaderStage& stage : colorVariant.data.stages){
            for (const ShaderStage& other : oitVariant.data.stages){
                if (other.type != stage.type)
                    continue;
                for (const ShaderUniformBlock& block : stage.uniformblocks){
                    bool found = false;
                    for (const ShaderUniformBlock& twin : other.uniformblocks)
                        found = found || (twin.name == block.name && twin.slot == block.slot);
                    EXPECT_TRUE(found) << colorVariant.name << ": " << block.name << " is not at its slot in the OIT twin";
                }
            }
        }
    }
}

// a fork of the built-in Lit reading objectLights.position_type itself, in a scene whose blocks
// are over every backend's 8 (capsule and contact shadows, 2D lights)
TEST(LightRecordsForm, ForkReadingTheBlocksKeepsThem){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    auto reader = [](const std::string& path, std::string& content){
        const auto& sources = ShaderVariantCompiler::getLibrarySources();
        const std::string file = path == "shaders/BlockReader.vert" ? "mesh.vert" : (path == "shaders/BlockReader.frag" ? "mesh.frag" : "");
        auto found = sources.find(file);
        if (file.empty() || found == sources.end())
            return false;
        content = found->second;
        if (file == "mesh.frag"){
            const std::string entry = "void main() {";
            const size_t at = content.find(entry);
            if (at == std::string::npos)
                return false;
            content.insert(at + entry.size(), "\n#ifdef USE_PUNCTUAL\n    if (objectLights.position_type[0].w > 1e30) discard;\n#endif\n");
        }
        return true;
    };
    SubmeshShaderInputs inputs;
    inputs.texCoord1 = true;
    inputs.normal = true;
    inputs.baseColorTexture = true;
    inputs.builtinLit = false;
    MeshShaderEnvironment environment;
    environment.lights = true;
    environment.lightCapacity = 16;
    environment.shadows = true;
    environment.capsuleShadows = true;
    environment.contactShadows = true;
    environment.lights2D = true;
    environment.fog = true;
    environment.ibl = true;
    const uint64_t properties = RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    FormCompiled fork = compileForm(properties, ShaderBackend::GLES3, "shaders/BlockReader", reader);
    ASSERT_TRUE(fork.ok) << fork.name << " no longer builds: " << fork.log;
    EXPECT_FALSE(readsRecords(fork)) << fork.name;
    EXPECT_NE(fork.log.find("light-records-failed"), std::string::npos) << fork.name << ": " << fork.log;
}
