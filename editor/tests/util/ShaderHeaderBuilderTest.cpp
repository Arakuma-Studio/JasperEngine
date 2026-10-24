// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generated <lang>.h of a shader build: getBase64Shader() is flat ifs, as MSVC rejects an
// else-if chain nested deeper than 128 (C1061) and a project ships hundreds of variants.

#include <gtest/gtest.h>

#include "util/ShaderHeaderBuilder.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using editor::ShaderHeaderBuilder;
namespace fs = std::filesystem;

namespace {

    std::string buildHeader(const std::vector<ShaderHeaderBuilder::HeaderShader>& shaders) {
        const fs::path path = fs::temp_directory_path() / "jasperengine-editor-tests-shader-header.h";
        std::string err;
        EXPECT_TRUE(ShaderHeaderBuilder::writeShaderHeader(path, "glsl410", shaders, err)) << err;
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        fs::remove(path);
        return text.str();
    }

}

TEST(ShaderHeaderBuilder, ManyShadersAreFlatIfs) {
    std::vector<ShaderHeaderBuilder::HeaderShader> shaders;
    for (int i = 0; i < 300; i++)
        shaders.push_back({"mesh_" + std::to_string(1000 + i) + "_vs", "QUJD"});

    const std::string header = buildHeader(shaders);
    EXPECT_EQ(header.find("else"), std::string::npos);
    EXPECT_NE(header.find("    if (name == \"mesh_1000_vs\") return mesh_1000_vs;\n"), std::string::npos);
    EXPECT_NE(header.find("    if (name == \"mesh_1299_vs\") return mesh_1299_vs;\n    return \"\";\n}\n"), std::string::npos);
}

TEST(ShaderHeaderBuilder, NoShadersHasEmptyLookups) {
    // the same text the else-if chain gave without shaders
    EXPECT_EQ(buildHeader({}),
              "#ifndef SHADER_glsl410_h\n#define SHADER_glsl410_h\n\n#include <string>\n#include <vector>\n\n"
              "std::string getBase64Shader(std::string name) {\n    return \"\";\n}\n\n"
              "std::vector<std::string> getBase64ShaderNames() {\n    return {};\n}\n\n"
              "#endif //SHADER_glsl410_h\n");
}
