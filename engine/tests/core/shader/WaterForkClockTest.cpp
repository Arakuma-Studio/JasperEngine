// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The clock of library:water's waves in a fork (docs/superpowers/specs/2026-11-04-water-reflections-design.md,
// Deviations): a fork copied before the scene clock keeps its .frag on frame.time and includes the
// engine's water_vs.glsl, whose waves must then run on frame.time too; the library (and a fork
// copied now, which carries water.vert's WATER_SCENE_CLOCK) runs them on the scene clock.

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace jasperengine;

namespace {

    const char* const OLD_FORK = "shaders/OldWater";

    // the fork's files, as a project holds them: library:water's entry points before the scene clock
    bool readOldFork(const std::string& path, std::string& content){
        std::string file;
        if (path == "shaders/OldWater.vert")
            file = "OldWater.vert";
        else if (path == "shaders/OldWater.frag")
            file = "OldWater.frag";
        else
            return false;
        std::ifstream in(std::string(JASPER_TEST_SOURCE_DIR "/fixtures/shader/old_water_fork/") + file);
        if (!in)
            return false;
        std::stringstream text;
        text << in.rdbuf();
        content = text.str();
        return true;
    }

    // a lit mesh variant with normals and UVs, with the shader's WAVES feature
    ShaderKey waterKey(const std::string& shader){
        const uint16_t id = ShaderPool::registerCustomShader(shader);
        return ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(1) | shaderPropertyBit(6), id);
    }

}

TEST(WaterForkClock, OldForkKeepsTheFrameClock){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::string shader = std::string(OLD_FORK) + "#WAVES";
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(shader, readOldFork, ShaderType::MESH);
    ASSERT_TRUE(sources.valid) << sources.error;
    const ShaderKey key = waterKey(shader);

    std::string vertex, fragment, error;
    ASSERT_TRUE(ShaderVariantCompiler::preprocess(key, ShaderBackend::GLCore, &sources, vertex, fragment, &error)) << error;
    EXPECT_NE(fragment.find("frame.time.x"), std::string::npos) << "the old .frag runs on frame.time";
    EXPECT_NE(vertex.find("frame.time.x"), std::string::npos) << "its waves do not run on the clock of its .frag";
    EXPECT_EQ(vertex.find("customParams.sceneTime"), std::string::npos) << "its waves run on the scene clock, its .frag does not";

    ShaderData data;
    std::string log;
    EXPECT_TRUE(ShaderVariantCompiler::compile(key, ShaderPool::getShaderBackend(), &sources, data, &error, &log))
        << "the old fork no longer compiles: " << error << "\n" << log;
}

TEST(WaterForkClock, LibraryWaterRunsOnTheSceneClock){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::string shader = "library:water#WAVES";
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(shader,
        [](const std::string&, std::string&){ return false; }, ShaderType::MESH);
    ASSERT_TRUE(sources.valid) << sources.error;
    std::string vertex, fragment, error;
    ASSERT_TRUE(ShaderVariantCompiler::preprocess(waterKey(shader), ShaderBackend::GLCore, &sources, vertex, fragment, &error)) << error;
    EXPECT_NE(vertex.find("customParams.sceneTime"), std::string::npos);
    EXPECT_NE(fragment.find("customParams.sceneTime"), std::string::npos);
}
