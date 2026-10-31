// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Graphics > Shader Globals in project.yaml and exports (shader globals spec
// 6.5, 9): the shaderGlobals list read back as written for every type (sRGB hex colours when
// exact), an old project without it, entries that cannot be read left out; the project's list
// declared to the engine; the generated main.cpp's ShaderGlobals::declare; the JSON shape MCP
// shares; bindings in scenes and the exported scene code.

#include <gtest/gtest.h>

#include "Factory.h"
#include "Generator.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "shader/ShaderGlobals.h"
#include "util/Color.h"
#include "util/ShaderGlobalsJson.h"
#include "yaml-cpp/yaml.h"

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class ProjectShaderGlobalsStreamTest : public ::testing::Test {
    protected:
        void SetUp() override { ShaderGlobals::clear(); }
        void TearDown() override { ShaderGlobals::clear(); }
    };

    // one of each type, with defaults away from the type's
    std::vector<ShaderGlobalDecl> everyType() {
        std::vector<ShaderGlobalDecl> decls;
        auto add = [&decls](const std::string& name, ShaderGlobalType type, std::vector<Vector4> value, unsigned int size = 0) {
            ShaderGlobalDecl d = ShaderGlobals::makeDecl(name, type, size);
            if (!value.empty()) d.defaultValue = value;
            decls.push_back(d);
        };
        add("_Strength", ShaderGlobalType::Float, {Vector4(0.1f, 0, 0, 0)});
        add("Wind2", ShaderGlobalType::Vector2, {Vector4(1.5f, -2, 0, 0)});
        add("Wind3", ShaderGlobalType::Vector3, {Vector4(1, 2, 3, 0)});
        add("_LightPositionX", ShaderGlobalType::Vector4, {Vector4(-2, 2, 0, 0.25f)});
        // an 8-bit sRGB colour (hex) and one that is not (numbers)
        add("FogTint", ShaderGlobalType::Color, {Color::sRGBToLinear(Vector4(128 / 255.0f, 64 / 255.0f, 1.0f, 1.0f))});
        add("Glow", ShaderGlobalType::Color, {Vector4(0.3f, 0.6f, 0.9f, 0.5f)});
        add("View", ShaderGlobalType::Matrix4, {Vector4(1, 2, 3, 4), Vector4(5, 6, 7, 8), Vector4(9, 10, 11, 12), Vector4(13, 14, 15, 16)});
        add("Weights", ShaderGlobalType::FloatArray, {Vector4(0.5f, 0, 0, 0), Vector4(0.25f, 0, 0, 0), Vector4(0, 0, 0, 0)}, 3);
        add("Points", ShaderGlobalType::Vector4Array, {Vector4(1, 2, 3, 4), Vector4(5, 6, 7, 8)}, 2);
        ShaderGlobalDecl noise = ShaderGlobals::makeDecl("_NoiseTex", ShaderGlobalType::Texture2D);
        noise.defaultTexture = "textures/noise.png";
        decls.push_back(noise);
        decls.push_back(ShaderGlobals::makeDecl("Sky", ShaderGlobalType::TextureCube));
        return decls;
    }

    std::string yamlOf(Project& project) {
        return YAML::Dump(Stream::encodeProject(&project));
    }

}

TEST_F(ProjectShaderGlobalsStreamTest, NoListWritesNoNode) {
    Project project;
    EXPECT_TRUE(project.getShaderGlobals().empty());
    EXPECT_FALSE(Stream::encodeProject(&project)["shaderGlobals"]);
}

TEST_F(ProjectShaderGlobalsStreamTest, TheListRoundTripsForEveryType) {
    Project project;
    project.setShaderGlobals(everyType());
    // declared to the engine at once (the views and Play read it)
    EXPECT_TRUE(ShaderGlobals::isDeclared(ShaderGlobals::findID("FogTint")));

    const YAML::Node node = Stream::encodeProject(&project);
    ASSERT_TRUE(node["shaderGlobals"]) << yamlOf(project);
    ASSERT_EQ(node["shaderGlobals"].size(), everyType().size());
    const std::string yaml = YAML::Dump(node);
    EXPECT_NE(yaml.find("\"#8040FFFF\""), std::string::npos) << "an 8-bit colour as sRGB hex:\n" << yaml;
    EXPECT_NE(yaml.find("arraySize: 3"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("default: [0.1]"), std::string::npos) << "a float in its shortest text:\n" << yaml;

    ShaderGlobals::clear();
    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    const std::vector<ShaderGlobalDecl> expected = everyType();
    ASSERT_EQ(loaded.getShaderGlobals().size(), expected.size());
    for (size_t i = 0; i < expected.size(); i++) {
        const ShaderGlobalDecl& got = loaded.getShaderGlobals()[i];
        EXPECT_EQ(got.name, expected[i].name);
        EXPECT_EQ(got.type, expected[i].type) << got.name;
        EXPECT_EQ(got.arraySize, expected[i].arraySize) << got.name;
        EXPECT_EQ(got.defaultTexture, expected[i].defaultTexture) << got.name;
        ASSERT_EQ(got.defaultValue.size(), expected[i].defaultValue.size()) << got.name;
        for (size_t s = 0; s < got.defaultValue.size(); s++) {
            // exact for the hex colour and the numbers (9 significant digits)
            EXPECT_NEAR(got.defaultValue[s].x, expected[i].defaultValue[s].x, 1e-6f) << got.name << "[" << s << "]";
            EXPECT_NEAR(got.defaultValue[s].y, expected[i].defaultValue[s].y, 1e-6f) << got.name << "[" << s << "]";
            EXPECT_NEAR(got.defaultValue[s].z, expected[i].defaultValue[s].z, 1e-6f) << got.name << "[" << s << "]";
            EXPECT_NEAR(got.defaultValue[s].w, expected[i].defaultValue[s].w, 1e-6f) << got.name << "[" << s << "]";
        }
    }
    EXPECT_EQ(loaded.getShaderGlobals()[4], expected[4]) << "the hex colour reads back bit for bit";
    EXPECT_TRUE(ShaderGlobals::isDeclared(ShaderGlobals::findID("_NoiseTex"))) << "the loaded list is declared";
}

TEST_F(ProjectShaderGlobalsStreamTest, AnOldProjectHasNoGlobals) {
    Project loaded;
    loaded.setShaderGlobals(everyType());
    Stream::decodeProject(&loaded, YAML::Load("name: old\n"), Workspace());
    EXPECT_TRUE(loaded.getShaderGlobals().empty());
    EXPECT_FALSE(ShaderGlobals::exists(ShaderGlobals::findID("FogTint")));
}

TEST_F(ProjectShaderGlobalsStreamTest, AnEntryThatCannotBeReadIsLeftOut) {
    const std::string yaml = "name: typo\n"
                             "shaderGlobals:\n"
                             "  - {name: Good, type: color, default: \"#FF0000\"}\n"
                             "  - {name: u_reserved, type: float, default: 1}\n"
                             "  - {name: Bad, type: colour, default: 1}\n"
                             "  - {name: Good, type: float, default: 2}\n"
                             "  - {name: Arr, type: float_array, default: [1, 2]}\n";
    Project loaded;
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    testing::internal::GetCapturedStdout();
    testing::internal::GetCapturedStderr();
    ASSERT_EQ(loaded.getShaderGlobals().size(), 2u);
    EXPECT_EQ(loaded.getShaderGlobals()[0].name, "Good");
    EXPECT_EQ(loaded.getShaderGlobals()[0].type, ShaderGlobalType::Color);
    EXPECT_EQ(loaded.getShaderGlobals()[1].name, "Arr");
    EXPECT_EQ(loaded.getShaderGlobals()[1].arraySize, 2u) << "an array without a size takes its default's";
}

TEST_F(ProjectShaderGlobalsStreamTest, TheJsonShapeMcpTakes) {
    std::vector<ShaderGlobalDecl> decls;
    std::string error;
    ASSERT_TRUE(ShaderGlobalsJson::listFromJson(nlohmann::json::parse(R"([
        {"name": "FogTint", "type": "color", "default": "#808080"},
        {"name": "Wind", "type": "vector4", "default": [1, 0, 0]},
        {"name": "W", "type": "float_array", "array_size": 4, "default": [0.5]},
        {"name": "Tex", "type": "texture2d", "default": "textures/t.png"}
    ])"), decls, error)) << error;
    ASSERT_EQ(decls.size(), 4u);
    EXPECT_NEAR(decls[0].defaultValue[0].x, Color::sRGBToLinear(Vector4(128 / 255.0f, 0, 0, 1)).x, 1e-7f);
    EXPECT_EQ(decls[0].defaultValue[0].w, 1.0f) << "#RRGGBB is opaque";
    EXPECT_EQ(decls[2].defaultValue.size(), 4u);
    EXPECT_EQ(decls[2].defaultValue[0].x, 0.5f);
    EXPECT_EQ(ShaderGlobalsJson::declToJson(decls[0])["default"], "#808080FF");
    EXPECT_EQ(ShaderGlobalsJson::declToJson(decls[2])["array_size"], 4);

    // what is wrong is named
    for (const char* bad : {R"([{"name": "A", "type": "vec4"}])", R"([{"name": "u_x", "type": "float"}])",
                            R"([{"name": "A", "type": "float"}, {"name": "A", "type": "float"}])",
                            R"([{"name": "A", "type": "color", "default": "red"}])",
                            R"([{"name": "A", "type": "matrix4", "default": [1, 2]}])",
                            R"([{"name": "A", "type": "float_array", "array_size": 99}])"}) {
        std::vector<ShaderGlobalDecl> none;
        error.clear();
        EXPECT_FALSE(ShaderGlobalsJson::listFromJson(nlohmann::json::parse(bad), none, error)) << bad;
        EXPECT_FALSE(error.empty()) << bad;
    }

    // an upsert keeps what it does not give
    ShaderGlobalDecl updated;
    ASSERT_TRUE(ShaderGlobalsJson::declFromJson(nlohmann::json::parse(R"({"default": [0, 0, 1, 0]})"), updated, error, "Wind", &decls[1])) << error;
    EXPECT_EQ(updated.type, ShaderGlobalType::Vector4);
    EXPECT_EQ(updated.defaultValue[0], Vector4(0, 0, 1, 0));
    ASSERT_TRUE(ShaderGlobalsJson::declFromJson(nlohmann::json::parse(R"({"type": "vector4"})"), updated, error, "Wind", &decls[1])) << error;
    EXPECT_EQ(updated.defaultValue[0], Vector4(1, 0, 0, 0)) << "the same type keeps the default";
}

TEST_F(ProjectShaderGlobalsStreamTest, ExportsDeclareTheListBeforeAnySceneLoads) {
    EXPECT_EQ(Generator::shaderGlobalsInitSource({}), "") << "nothing without globals";
    const std::string source = Generator::shaderGlobalsInitSource(everyType());
    EXPECT_EQ(source.rfind("    ShaderGlobals::declare({\n", 0), 0u) << source;
    EXPECT_NE(source.find("ShaderGlobalDecl{\"FogTint\", ShaderGlobalType::Color, 0u, {Vector4("), std::string::npos) << source;
    EXPECT_NE(source.find("ShaderGlobalDecl{\"Weights\", ShaderGlobalType::FloatArray, 3u, {"), std::string::npos) << source;
    EXPECT_NE(source.find("ShaderGlobalDecl{\"_NoiseTex\", ShaderGlobalType::Texture2D, 0u, {}, \"textures/noise.png\"}"), std::string::npos) << source;
    EXPECT_NE(source.find("ShaderGlobalType::TextureCube"), std::string::npos) << source;
    EXPECT_EQ(source.substr(source.size() - 8), "    });\n") << source;
    // one entry per global, every one a literal the engine's types take
    size_t entries = 0;
    for (size_t at = 0; (at = source.find("ShaderGlobalDecl{", at)) != std::string::npos; at++) entries++;
    EXPECT_EQ(entries, everyType().size());
}

TEST_F(ProjectShaderGlobalsStreamTest, BindingsAreWrittenOnlyWhenSet) {
    Material material;
    EXPECT_FALSE(Stream::encodeMaterial(material)["shaderGlobalBindings"]);
    material.shader = "library:toon";
    material.shaderGlobalBindings = {{"tint", "FogTint"}, {"rimColor", "_Rim"}};
    const YAML::Node node = Stream::encodeMaterial(material);
    ASSERT_TRUE(node["shaderGlobalBindings"]);
    EXPECT_EQ(node["shaderGlobalBindings"]["tint"].as<std::string>(), "FogTint");
    const Material decoded = Stream::decodeMaterial(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(decoded.shaderGlobalBindings, material.shaderGlobalBindings) << "in order";
    EXPECT_EQ(Factory::formatShaderGlobalBindings(material.shaderGlobalBindings), "{{\"tint\", \"FogTint\"}, {\"rimColor\", \"_Rim\"}}");
    EXPECT_EQ(Stream::decodeShaderGlobalBindings(YAML::Load("{a: B, c: ''}")), (ShaderGlobalBindings{{"a", "B"}}));
}
