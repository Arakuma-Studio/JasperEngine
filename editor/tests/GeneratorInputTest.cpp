// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Input in exported builds (docs/specs/S22-gameplay.md R39 and "Upstream
// absorption"; the upstream input feature's GeneratorInputTest, adapted): the generated main.cpp
// loads the project's actions asset (InputActions::loadDefault) before the first scene, only
// when the project has one (else the engine keeps Unity's Input Manager defaults), with the path
// as a C++ string literal whatever it holds; an older editor's Input Manager actions reach the
// build as that asset, which the engine reads back as the same actions. The older inline JSON
// (InputActions::loadJson in raw string literals) is gone: the asset ships with the content.

#include <gtest/gtest.h>

#include "Generator.h"
#include "input/InputActions.h"
#include "input/InputManagerFormat.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    fs::path tempProject(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-generator-input-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / ".jasperengine" / "generated");
        return dir;
    }

    std::string readFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // what the C++ compiler reads from a "..." literal Factory::formatString wrote
    std::string cStringValue(const std::string& literal) {
        std::string value;
        for (size_t i = 1; i + 1 < literal.size(); i++) {
            if (literal[i] == '\\' && i + 2 < literal.size()) {
                const char next = literal[++i];
                if (next == 'n') value += '\n';
                else if (next == 't') value += '\t';
                else if (next == 'r') value += '\r';
                else value += next;
            } else {
                value += literal[i];
            }
        }
        return value;
    }

    class GeneratorInputTest : public ::testing::Test {
    protected:
        void TearDown() override {
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
        }
    };

}

TEST_F(GeneratorInputTest, LoadsTheProjectAsset) {
    EXPECT_EQ(Generator::inputSettingsInitSource("input/game.inputactions", 0.15f),
              "    InputActions::loadDefault(\"input/game.inputactions\");\n");
    // the deadzone first: the asset's actions read it as they load
    EXPECT_EQ(Generator::inputSettingsInitSource("game.inputactions", 0.3f),
              "    InputActions::setDefaultDeadzone(0.300000f);\n    InputActions::loadDefault(\"game.inputactions\");\n");
}

TEST_F(GeneratorInputTest, ThePathSurvivesAsALiteral) {
    // quotes, backslashes and UTF-8 in a path: the literal reads back as the path
    const std::string path = "input/\"quoted\" \\ acci\xc3\xb3n \xf0\x9f\x98\x80.inputactions";
    const std::string line = Generator::inputSettingsInitSource(path, 0.15f);
    const size_t open = line.find('(');
    const size_t close = line.rfind(')');
    ASSERT_NE(open, std::string::npos);
    EXPECT_EQ(cStringValue(line.substr(open + 1, close - open - 1)), path) << line;
}

TEST_F(GeneratorInputTest, MainCppHasTheLineOnlyWhenTheProjectHasAnAsset) {
    const fs::path project = tempProject("main");
    const fs::path internal = project / ".jasperengine";
    const std::vector<SceneBuildInfo> scenes = {SceneBuildInfo{1, "Level", {1}, {1}, true}};

    Generator generator;
    generator.configure(scenes, "test", {}, {}, project, internal, project / "assets", project / "lua", {}, 17);
    EXPECT_EQ(readFile(internal / "generated" / "main.cpp").find("InputActions::loadDefault"), std::string::npos);

    generator.setInputSettings("InputManager.inputactions", 0.15f);
    generator.configure(scenes, "test", {}, {}, project, internal, project / "assets", project / "lua", {}, 17);
    const std::string mainCpp = readFile(internal / "generated" / "main.cpp");
    const size_t at = mainCpp.find("InputActions::loadDefault(\"InputManager.inputactions\");");
    ASSERT_NE(at, std::string::npos) << mainCpp;
    // before the first scene loads
    EXPECT_LT(at, mainCpp.find("SceneManager::loadScene"));
    // main.cpp includes only JasperEngine.h: InputActions (and the Input Manager's names) must be declared through it
    EXPECT_NE(mainCpp.find("#include \"JasperEngine.h\""), std::string::npos);
    EXPECT_EQ(mainCpp.find("InputActions::loadJson"), std::string::npos);
    const std::string umbrella = readFile(fs::path(JASPER_TEST_REPO_DIR) / "engine" / "core" / "JasperEngine.h");
    EXPECT_NE(umbrella.find("#include \"input/InputActions.h\""), std::string::npos);
    EXPECT_NE(umbrella.find("#include \"input/InputControlNames.h\""), std::string::npos);

    fs::remove_all(project);
}

TEST_F(GeneratorInputTest, WhatTheBuildReadsBackIsTheProjectsDefinition) {
    // an older editor's actions with one of its own: the asset the build ships
    std::vector<InputManagerAction> actions = InputManagerFormat::defaults();
    InputManagerAction dash;
    dash.name = "Dash";
    dash.description = "breaks a naive literal )jinput\"";
    InputManagerBinding shift = InputManagerFormat::makeBinding(InputManagerBindingKind::Buttons);
    shift.positive.push_back({InputDevice::Key, D_KEY_LEFT_SHIFT});
    dash.bindings.push_back(shift);
    actions.push_back(dash);
    InputActionAssetDesc asset;
    asset.maps.push_back(InputManagerFormat::toMap(actions));

    // the build's engine loads the asset and has the same actions
    ASSERT_TRUE(InputActions::loadFromText("InputManager.inputactions", asset.save()));
    EXPECT_EQ(InputActions::toJson(), InputManagerFormat::actionsToJson(actions));
    EXPECT_TRUE(InputActions::hasAction("Dash"));
    EXPECT_EQ(InputActions::getManagerActions().back().description, dash.description);
}

TEST_F(GeneratorInputTest, ALongDefinitionIsOneAssetLine) {
    // hundreds of actions (the inline JSON needed pieces MSVC takes): the line stays one call
    std::vector<InputManagerAction> actions;
    for (int i = 0; i < 200; i++) {
        InputManagerAction action;
        action.name = "Acci\xc3\xb3n " + std::to_string(i);
        for (int k = 0; k < 20; k++) action.description += "\xc3\xa9\xf0\x9f\x98\x80";
        action.bindings.push_back(InputManagerFormat::makeBinding(InputManagerBindingKind::Buttons));
        actions.push_back(action);
    }
    InputActionAssetDesc asset;
    asset.maps.push_back(InputManagerFormat::toMap(actions));
    const std::string text = asset.save();
    ASSERT_GT(text.size(), 16000u);
    ASSERT_TRUE(InputActions::loadFromText("big.inputactions", text));
    EXPECT_EQ(InputActions::getManagerActions().size(), 200u);
    EXPECT_EQ(InputActions::getManagerActions()[7].description, actions[7].description);
    const std::string line = Generator::inputSettingsInitSource("big.inputactions", 0.15f);
    EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1);
}
