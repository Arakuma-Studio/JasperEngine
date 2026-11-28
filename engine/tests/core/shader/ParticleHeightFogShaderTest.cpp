// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The fogged particle variants take the height fog as the mesh's do (docs/specs/S101-vapor.md R16): the
// 'Fog' bit compiles includes/height_fog.glsl in with the distance fog, u_fs_heightFog choosing at
// run time, in the built-in shader and library:particles, for quads, meshes, trails and pixel
// particles. A fogless variant reads no height fog. The heaviest fogged variants stay within what
// GLES3 and WebGL2 guarantee a fragment stage (224 vectors, 8 blocks, 16 textures and samplers).

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "texture/Material.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace particle_height_fog_shader_test {

using namespace jasperengine;


    // PARTICLES 'Pcl' and 'Pxs': the post colour and the pixel snap, set by the scene, not the effect
    constexpr uint64_t POST_COLOR_BIT = 1u << 10;
    constexpr uint64_t PIXEL_SNAP_BIT = 1u << 11;

    struct Case {
        const char* label;
        ParticlesShaderFlags flags;
        uint64_t sceneBits = 0;
    };

    // a variant of each kind, with as much as its kind combines (Analytic takes no Custom Data:
    // AnalyticParticles' eligibility)
    std::vector<Case> cases(){
        std::vector<Case> out;
        Case quad{"textured six-way quad with shadows"};
        quad.flags.texture = true;
        quad.flags.frameBlend = true;
        quad.flags.sixWay = true;
        quad.flags.customData = true;
        quad.flags.receiveShadows = true;
        quad.sceneBits = POST_COLOR_BIT | PIXEL_SNAP_BIT;
        out.push_back(quad);
        Case lit{"stretched lit quad"};
        lit.flags.texture = true;
        lit.flags.stretch = true;
        lit.flags.lit = true;
        lit.flags.receiveShadows = true;
        lit.sceneBits = POST_COLOR_BIT;
        out.push_back(lit);
        Case mesh{"lit mesh particle"};
        mesh.flags.mesh = true;
        mesh.flags.texture = true;
        mesh.flags.lit = true;
        mesh.flags.receiveShadows = true;
        mesh.sceneBits = POST_COLOR_BIT;
        out.push_back(mesh);
        Case trail{"trail"};
        trail.flags.trail = true;
        trail.flags.texture = true;
        trail.sceneBits = POST_COLOR_BIT;
        out.push_back(trail);
        Case pixel{"analytic pixel particle"};
        pixel.flags.pixel = true;
        pixel.flags.analytic = true;
        pixel.sceneBits = POST_COLOR_BIT | PIXEL_SNAP_BIT;
        out.push_back(pixel);
        return out;
    }

    // the built-in shader (""), and library:particles with every feature in two blends (the
    // additive one routes its fog through fadeByFog, includes/output.glsl)
    std::vector<std::string> references(){
        const std::vector<std::string> features = {"GLOW", "EROSION", "SOFT", "NEAR_FADE", "DISTORTION"};
        return {"",
                ShaderReference::getColorVariantReference("library:particles", features, MaterialBlendMode::ALPHA),
                ShaderReference::getColorVariantReference("library:particles", features, MaterialBlendMode::ADDITIVE)};
    }

    uint64_t propertiesOf(const Case& c, bool fog){
        ParticlesShaderFlags flags = c.flags;
        flags.fog = fog;
        return ShaderPool::getParticlesProperties(flags) | c.sceneBits;
    }

    std::string nameOf(const Case& c, const std::string& reference, uint64_t properties){
        return std::string(c.label) + " " + ShaderPool::getShaderStr(ShaderType::PARTICLES, properties, 0) +
               (reference.empty() ? std::string() : " (" + reference + ")");
    }

    bool preprocessFragment(const std::string& reference, uint64_t properties, std::string& fragment, std::string& error){
        uint16_t customId = 0;
        CustomShaderSources sources;
        if (!reference.empty()){
            customId = ShaderPool::registerCustomShader(reference);
            sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, ShaderVariantCompiler::FileReader(), ShaderType::PARTICLES);
            if (!sources.valid){
                error = sources.error;
                return false;
            }
        }
        std::string vertex;
        return ShaderVariantCompiler::preprocess(ShaderPool::getShaderKey(ShaderType::PARTICLES, properties, customId), ShaderBackend::GLCore,
                                                 reference.empty() ? nullptr : &sources, vertex, fragment, &error);
    }

    bool compileGles3(const std::string& reference, uint64_t properties, ShaderData& data, std::string& error){
        uint16_t customId = 0;
        CustomShaderSources sources;
        if (!reference.empty()){
            customId = ShaderPool::registerCustomShader(reference);
            sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, ShaderVariantCompiler::FileReader(), ShaderType::PARTICLES);
            if (!sources.valid){
                error = sources.error;
                return false;
            }
        }
        std::string log;
        const bool ok = ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::PARTICLES, properties, customId), ShaderBackend::GLES3,
                                                       reference.empty() ? nullptr : &sources, data, &error, &log);
        if (!ok)
            error += "\n" + log;
        return ok;
    }

    size_t fragmentVectors(const ShaderData& data, std::string& blocks){
        size_t vectors = 0;
        for (const ShaderStage& stage : data.stages){
            if (stage.type != ShaderStageType::FRAGMENT)
                continue;
            for (const ShaderUniformBlock& block : stage.uniformblocks){
                vectors += (block.sizeBytes + 15) / 16;
                blocks += (blocks.empty() ? "" : ", ") + block.name + " " + std::to_string((block.sizeBytes + 15) / 16);
            }
        }
        return vectors;
    }

}

namespace particle_height_fog_shader_test {

using namespace particle_height_fog_shader_test;

TEST(ParticleHeightFogShader, FoggedVariantsChooseTheHeightFogAtRunTime){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    size_t checked = 0;
    for (const std::string& reference : references()){
        for (const Case& c : cases()){
            for (const bool fog : {true, false}){
                const uint64_t properties = propertiesOf(c, fog);
                const std::string name = nameOf(c, reference, properties);
                std::string fragment, error;
                ASSERT_TRUE(preprocessFragment(reference, properties, fragment, error)) << name << ": " << error;
                if (fog){
                    EXPECT_NE(fragment.find("u_fs_heightFog"), std::string::npos) << name << ": no height fog block";
                    EXPECT_NE(fragment.find("isHeightFog"), std::string::npos) << name << ": the fog does not choose the height fog";
                }else{
                    EXPECT_EQ(fragment.find("u_fs_heightFog"), std::string::npos) << name << ": a fogless variant reads the height fog";
                }
                checked++;
            }
        }
    }
    EXPECT_EQ(checked, references().size() * cases().size() * 2);
}

TEST(ParticleHeightFogShader, FoggedVariantsFitGles3){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    size_t worst = 0;
    std::string worstName;
    for (const std::string& reference : references()){
        for (const Case& c : cases()){
            const uint64_t properties = propertiesOf(c, true);
            const std::string name = nameOf(c, reference, properties);
            ShaderData data;
            std::string error;
            ASSERT_TRUE(compileGles3(reference, properties, data, error)) << name << ": " << error;
            EXPECT_NE(data.getUniformBlockIndexByName("u_fs_heightFog"), -1) << name;
            std::string blocks;
            const size_t vectors = fragmentVectors(data, blocks);
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name << "; fragment blocks: " << blocks;
            EXPECT_LE(vectors, 224u) << name << "; fragment blocks: " << blocks;
            if (vectors > worst){
                worst = vectors;
                worstName = name + "; " + blocks;
            }
        }
    }
    std::printf("[particles] the most fragment uniform vectors of a fogged variant on GLES3: %zu of 224, %s\n", worst, worstName.c_str());
}

}  // namespace particle_height_fog_shader_test
