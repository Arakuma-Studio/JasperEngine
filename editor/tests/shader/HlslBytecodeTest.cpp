// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// D3D11 exports built on Windows ship bytecode (ShaderBuilder::compileHlslBytecode, export-pipeline
// spec 6.9): the game skips D3DCompile. Other hosts keep the HLSL source.

#include "shader/ShaderBuilder.h"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace hlsl_bytecode_test {

    ShaderData hlslData(const std::string& vertexSource) {
        ShaderData data;
        data.lang = ShaderLang::HLSL;
        data.version = 50;
        ShaderStage vertex;
        vertex.type = ShaderStageType::VERTEX;
        vertex.name = "main";
        vertex.source = vertexSource;
        data.stages.push_back(vertex);
        ShaderStage fragment;
        fragment.type = ShaderStageType::FRAGMENT;
        fragment.name = "main";
        fragment.source = "float4 main() : SV_Target0 { return float4(1, 0, 0, 1); }\n";
        data.stages.push_back(fragment);
        return data;
    }

}

using namespace hlsl_bytecode_test;

#ifdef _WIN32
TEST(HlslBytecode, ExportShipsBytecodeOnWindows) {
    ShaderData data = hlslData("float4 main(float4 position : POSITION) : SV_Position { return position; }\n");
    ASSERT_TRUE(ShaderBuilder::compileHlslBytecode(data)) << "d3dcompiler_47.dll ships with Windows 8.1 and later";
    for (const ShaderStage& stage : data.stages) {
        EXPECT_TRUE(stage.source.empty());
        ASSERT_GT(stage.bytecode.size, 4u);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(stage.bytecode.data), 4), "DXBC");
    }
}

TEST(HlslBytecode, AStageThatDoesNotCompileFailsTheExport) {
    ShaderData data = hlslData("float4 main( : SV_Position { syntax error }\n");
    EXPECT_THROW(ShaderBuilder::compileHlslBytecode(data), std::runtime_error);
}
#else
TEST(HlslBytecode, OtherHostsKeepTheSource) {
    ShaderData data = hlslData("float4 main(float4 position : POSITION) : SV_Position { return position; }\n");
    EXPECT_FALSE(ShaderBuilder::compileHlslBytecode(data)) << "no DXBC compiler off Windows";
    for (const ShaderStage& stage : data.stages) {
        EXPECT_FALSE(stage.source.empty());
        EXPECT_EQ(stage.bytecode.data, nullptr);
    }
}
#endif
