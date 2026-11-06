// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The materials and lighting features (docs/specs/S11-materials-lighting.md) cost nothing when
// unused: every built-in Lit variant compiled without their defines preprocesses to exactly the
// program it was before them. The list of hashes was generated on the commit the work started
// from (JASPER_ZERO_COST_UPDATE=1 rewrites it; only do so when a change of the built-in shader
// is meant to change existing variants, and say so in its commit).

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    const char* hashListPath(){
        return JASPER_TEST_SOURCE_DIR "/fixtures/shader/zero_cost_mesh_variants.txt";
    }

    struct Case{
        std::function<void(SubmeshShaderInputs&, MeshShaderEnvironment&)> apply;
    };

    // what a submesh can be, one thing at a time (each applied over every lighting setup below)
    const std::vector<Case>& meshCases(){
        static const std::vector<Case> cases = {
            {[](SubmeshShaderInputs&, MeshShaderEnvironment&){}},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.baseColorTexture = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.baseColorTexture = true; i.normalTexture = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.baseColorTexture = true; i.normalTexture = true; i.tangent = false; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.baseColorTexture = true; i.texCoord2 = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.skinning = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.morphTarget = true; i.morphNormal = true; i.morphTangent = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.instanced = true; i.instanceFade = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.vertexColor4 = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.vertexColor3 = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.textureRect = true; i.baseColorTexture = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.normal = false; i.tangent = false; i.texCoord1 = false; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.alphaMode = MaterialAlphaMode::MASK; i.baseColorTexture = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.alphaMode = MaterialAlphaMode::ALPHA_OPAQUE; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.mirror = true; i.baseColorTexture = true; }},
            {[](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.fog = true; }},
            {[](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.ssao = true; }},
            // a scene with Water Excluders and Wakes: only shaders requiring them change (the water's)
            {[](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.waterExclusion = true; e.waterWake = true; }},
            {[](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.postColor = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment& e){ e.postColor = true; e.fog = true; i.baseColorTexture = true; i.normalTexture = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment& e){ i.textureRect = true; i.baseColorTexture = true; e.lights2D = true; e.shadows2D = true; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.terrain = true; i.texCoord1 = false; i.tangent = false; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.terrain = true; i.terrainBlendMaps = true; i.texCoord1 = false; i.tangent = false; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment&){
                i.terrain = true; i.terrainBlendMaps = true; i.terrainSurface = true; i.texCoord1 = false; i.tangent = false; }},
            {[](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                i.terrain = true; i.terrainBlendMaps = true; i.terrainSurface = true; i.texCoord1 = false; i.tangent = false; e.postColor = true; e.fog = true; }},
        };
        return cases;
    }

    // the scene and material lighting each case is drawn under: unlit (no lights), the three lit
    // modes at each light capacity tier, with and without shadows, IBL and SSAO
    std::vector<std::pair<SubmeshShaderInputs, MeshShaderEnvironment>> lightingSetups(){
        std::vector<std::pair<SubmeshShaderInputs, MeshShaderEnvironment>> setups;
        SubmeshShaderInputs base;
        base.texCoord1 = true;
        base.normal = true;
        base.tangent = true;
        setups.push_back({base, MeshShaderEnvironment()});
        for (MaterialLighting lighting : {MaterialLighting::PIXEL, MaterialLighting::VERTEX, MaterialLighting::MIXED}){
            for (int capacity : {8, 4, 16}){
                for (int shadows = 0; shadows < 2; shadows++){
                    for (int ibl = 0; ibl < 2; ibl++){
                        SubmeshShaderInputs inputs = base;
                        inputs.lighting = lighting;
                        MeshShaderEnvironment environment;
                        environment.lights = true;
                        environment.lightCapacity = capacity;
                        environment.shadows = shadows != 0;
                        environment.ibl = ibl != 0;
                        setups.push_back({inputs, environment});
                    }
                }
            }
        }
        // IBL alone (an environment, no lights)
        MeshShaderEnvironment iblOnly;
        iblOnly.ibl = true;
        setups.push_back({base, iblOnly});
        return setups;
    }

    // every built-in Lit (customId 0) mesh variant the cases reach, by property bits
    std::set<uint32_t> enumerateVariants(){
        std::set<uint32_t> variants;
        for (const auto& setup : lightingSetups()){
            for (const Case& meshCase : meshCases()){
                SubmeshShaderInputs inputs = setup.first;
                MeshShaderEnvironment environment = setup.second;
                meshCase.apply(inputs, environment);
                variants.insert(RenderSystem::getSubmeshShaderVariant(environment, inputs).properties);
            }
        }
        return variants;
    }

    std::map<std::string, std::string> readHashList(){
        std::map<std::string, std::string> list;
        std::ifstream file(hashListPath());
        std::string line;
        while (std::getline(file, line)){
            if (line.empty() || line[0] == '#')
                continue;
            const size_t space = line.find(' ');
            if (space != std::string::npos)
                list[line.substr(0, space)] = line.substr(space + 1);
        }
        return list;
    }

}

TEST(ShaderZeroCost, DefaultVariantsUnchanged){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no embedded shader library";

    const std::set<uint32_t> variants = enumerateVariants();
    ASSERT_GT(variants.size(), 100u);

    std::map<std::string, std::string> current;
    for (uint32_t properties : variants){
        const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, properties, 0);
        std::string vertex, fragment, error;
        ASSERT_TRUE(ShaderVariantCompiler::preprocess(key, ShaderBackend::GLCore, nullptr, vertex, fragment, &error))
            << ShaderPool::getShaderStr(ShaderType::MESH, properties) << ": " << error;
        current[ShaderPool::getShaderStr(ShaderType::MESH, properties)] =
            ShaderVariantCompiler::toHex(ShaderVariantCompiler::hash(ShaderVariantCompiler::normalizePreprocessed(vertex))) + " " +
            ShaderVariantCompiler::toHex(ShaderVariantCompiler::hash(ShaderVariantCompiler::normalizePreprocessed(fragment)));
    }

    const char* update = std::getenv("JASPER_ZERO_COST_UPDATE");
    if (update && std::string(update) == "1"){
        std::filesystem::create_directories(std::filesystem::path(hashListPath()).parent_path());
        std::ofstream file(hashListPath(), std::ios::trunc);
        ASSERT_TRUE(file.good()) << "cannot write " << hashListPath();
        file << "# built-in Lit mesh variants: <variant> <mesh.vert hash> <mesh.frag hash> (normalized preprocessed GLSL,\n";
        file << "# FNV-1a 64). Generated by ShaderZeroCostTest with JASPER_ZERO_COST_UPDATE=1.\n";
        for (const auto& [name, hashes] : current)
            file << name << " " << hashes << "\n";
        return;
    }

    const std::map<std::string, std::string> expected = readHashList();
    ASSERT_FALSE(expected.empty()) << "no hash list at " << hashListPath();
    for (const auto& [name, hashes] : current){
        auto found = expected.find(name);
        ASSERT_NE(found, expected.end()) << name << " is not in the hash list: the matrix reaches a variant it did not";
        EXPECT_EQ(found->second, hashes) << name << " preprocesses to another program than before";
    }
    EXPECT_EQ(expected.size(), current.size()) << "the hash list has variants the matrix no longer reaches";
}
