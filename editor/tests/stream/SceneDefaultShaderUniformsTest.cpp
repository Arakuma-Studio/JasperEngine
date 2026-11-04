// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene's default shader property values in every form a scene takes (docs/superpowers/specs/
// 2026-11-04-code-editor-git-design.md 6.7, AC11): the .scene file (written only when set, so older
// scenes save unchanged), the generated factory of a Compile build, the cooked scene of a prebuilt
// player, and the undoable edit of the Inspector and MCP.

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Catalog.h"
#include "Factory.h"
#include "JasperEngine.h"
#include "Project.h"
#include "StreamMigrations.h"
#include "command/CommandHandle.h"
#include "command/type/ScenePropertyCmd.h"
#include "cook/SceneCooker.h"
#include "serialization/CookedScene.h"

#include <gtest/gtest.h>

#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;

namespace scene_default_uniforms_test {

    std::string sceneText(const std::string& settings) {
        return "formatVersion: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Scene)) +
               "\nid: 4343\nname: Defaults\nscene:\n  backgroundColor: [0, 0, 0, 1]\n" + settings +
               "sceneType: scene_3d\nmainCamera: 0\nentities: []\n";
    }

    const char* kWithValues =
        "  defaultMeshShader: library:unlit\n"
        "  defaultMeshShaderUniforms:\n"
        "    baseColor: [1, 0.5, 0, 1]\n"
        "  defaultLinesShaderUniforms:\n"
        "    width: [2, 0, 0, 0]\n";

    struct Headless {
        std::unique_ptr<Project> project;
    };

    Project* project() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-default-uniforms-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            const fs::path dir = root / "project";
            fs::create_directories(dir / ".jasperengine");
            std::ofstream(dir / "project.yaml") << "name: defaults\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            if (!out.project->loadProject(dir, false)) throw std::runtime_error("the test project did not load");
            return out;
        }();
        if (h.project) useProjectPaths(h.project.get());
        return h.project.get();
    }

}

using namespace scene_default_uniforms_test;

TEST(SceneDefaultShaderUniforms, StreamRoundTrip) {
    DecodedScene decoded = decodeSceneText(sceneText(kWithValues), "Defaults.scene");
    Scene* scene = decoded.scene->scene;
    EXPECT_EQ(scene->getDefaultShaderUniform(ShaderType::MESH, "baseColor"), Vector4(1, 0.5f, 0, 1));
    EXPECT_EQ(scene->getDefaultShaderUniform(ShaderType::LINES, "width"), Vector4(2, 0, 0, 0));
    EXPECT_TRUE(scene->getDefaultShaderUniforms(ShaderType::UI).empty());
    // the keys are the schema's: nothing reported as unknown
    for (const LoadIssue& issue : decoded.report.issues) ADD_FAILURE() << issue.describe();

    const YAML::Node encoded = encodeScene(decoded);
    ASSERT_TRUE(encoded["scene"]["defaultMeshShaderUniforms"]);
    EXPECT_FLOAT_EQ(encoded["scene"]["defaultMeshShaderUniforms"]["baseColor"][1].as<float>(), 0.5f);
    ASSERT_TRUE(encoded["scene"]["defaultLinesShaderUniforms"]);
    EXPECT_FALSE(encoded["scene"]["defaultUIShaderUniforms"]);

    // a scene from before them: nothing set, nothing written
    DecodedScene old = decodeSceneText(sceneText(""), "Old.scene");
    for (ShaderType type : {ShaderType::MESH, ShaderType::UI, ShaderType::SKYBOX, ShaderType::POINTS, ShaderType::LINES})
        EXPECT_TRUE(old.scene->scene->getDefaultShaderUniforms(type).empty());
    const std::string text = encodeSceneText(old);
    EXPECT_EQ(text.find("ShaderUniforms"), std::string::npos) << text;
}

TEST(SceneDefaultShaderUniforms, GeneratedFactory) {
    DecodedScene decoded = decodeSceneText(sceneText(kWithValues), "Defaults.scene");
    const std::string code = Factory::createScene(0, decoded.scene->scene, 4343, "Defaults", {}, NULL_ENTITY, fs::path("/project"),
                                                  fs::path("/project/.jasperengine/generated"));
    EXPECT_NE(code.find("scene->setDefaultShaderUniforms(ShaderType::MESH, {{\"baseColor\", Vector4("), std::string::npos) << code;
    EXPECT_NE(code.find("scene->setDefaultShaderUniforms(ShaderType::LINES, {{\"width\", Vector4("), std::string::npos);
    EXPECT_EQ(code.find("ShaderType::UI, {"), std::string::npos);

    DecodedScene old = decodeSceneText(sceneText(""), "Old.scene");
    const std::string oldCode = Factory::createScene(0, old.scene->scene, 4343, "Old", {}, NULL_ENTITY, fs::path("/project"),
                                                     fs::path("/project/.jasperengine/generated"));
    EXPECT_EQ(oldCode.find("setDefaultShaderUniforms"), std::string::npos);
}

TEST(SceneDefaultShaderUniforms, CookedRoundTrip) {
    DecodedScene decoded = decodeSceneText(sceneText(kWithValues), "Defaults.scene");
    const std::vector<uint8_t> bytes = editor::cook::SceneCooker::cookScene(decoded.scene->scene, 4343, {}, NULL_ENTITY, SceneResources{});
    DecodedScene loaded = decodeSceneText(sceneText(""), "Loaded.scene");
    std::string error;
    ASSERT_TRUE(CookedSceneLoader::loadScene(loaded.scene->scene, std::span<const uint8_t>(bytes.data(), bytes.size()), &error)) << error;
    EXPECT_EQ(loaded.scene->scene->getDefaultShaderUniform(ShaderType::MESH, "baseColor"), Vector4(1, 0.5f, 0, 1));
    EXPECT_EQ(loaded.scene->scene->getDefaultShaderUniform(ShaderType::LINES, "width"), Vector4(2, 0, 0, 0));
    EXPECT_TRUE(loaded.scene->scene->getDefaultShaderUniforms(ShaderType::POINTS).empty());
}

TEST(SceneDefaultShaderUniforms, UndoRedo) {
    Project* p = project();
    if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
    p->createNewScene("Defaults", SceneType::SCENE_3D);
    const uint32_t sceneId = p->getSelectedSceneId();
    Scene* scene = p->getScene(sceneId)->scene;

    const char* property = Catalog::getDefaultShaderUniformsProperty(ShaderType::MESH);
    EXPECT_STREQ(property, "default_mesh_shader_uniforms");
    ShaderType type;
    ASSERT_TRUE(Catalog::getDefaultShaderUniformsType("default_sky_shader_uniforms", type));
    EXPECT_EQ(type, ShaderType::SKYBOX);
    EXPECT_FALSE(Catalog::getDefaultShaderUniformsType("default_mesh_shader", type));

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new ScenePropertyCmd<ShaderUniformValues>(p, sceneId, property, ShaderUniformValues{{"tint", Vector4(1, 0, 0, 1)}}));
    EXPECT_EQ(scene->getDefaultShaderUniform(ShaderType::MESH, "tint"), Vector4(1, 0, 0, 1));
    EXPECT_TRUE(p->getScene(sceneId)->isModified);
    CommandHandle::get(sceneId)->undo();
    EXPECT_FALSE(scene->hasDefaultShaderUniform(ShaderType::MESH, "tint"));
    CommandHandle::get(sceneId)->redo();
    EXPECT_EQ(scene->getDefaultShaderUniform(ShaderType::MESH, "tint"), Vector4(1, 0, 0, 1));
    EXPECT_TRUE(Catalog::getScenePropertyDefault<ShaderUniformValues>(property).empty());
}
