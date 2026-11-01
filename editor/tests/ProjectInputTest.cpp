// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Input in project.yaml (docs/specs/S22-gameplay.md R39 and "Upstream
// absorption"): S22 writes `input: {actions: <asset>, deadzone}`, nothing for a project without
// an asset (Unity's Input Manager defaults run); an older editor's Input Manager node (`input:
// {version: 1, actions: [...]}`) becomes the "Input Manager" map of an .inputactions asset with
// the same actions (control names that look like numbers stay names), written once in the assets
// directory and found again by later loads; a node that cannot be read is kept verbatim. The
// upstream input feature's ProjectInputTest, adapted to that.

#include <gtest/gtest.h>

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "input/InputActions.h"
#include "input/InputManagerFormat.h"
#include "json.hpp"
#include "support/TestTempDir.h"

#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::vector<InputManagerAction> customActions() {
        std::vector<InputManagerAction> actions = InputManagerFormat::defaults();
        InputManagerAction dash;
        dash.name = "1";                      // a name YAML would read as a number
        dash.type = InputManagerActionType::Button;
        dash.description = "true: a description that looks like a bool";
        dash.pressPoint = 0.25f;
        InputManagerBinding keys = InputManagerFormat::makeBinding(InputManagerBindingKind::Buttons);
        keys.positive = {{InputDevice::Key, D_KEY_0}, {InputDevice::Key, D_KEY_LEFT_SHIFT}, {InputDevice::GamepadButton, D_GAMEPAD_BUTTON_X}};
        keys.gamepad = 2;
        dash.bindings.push_back(keys);
        InputManagerBinding stick = InputManagerFormat::makeBinding(InputManagerBindingKind::GamepadAxis);
        stick.axis = D_GAMEPAD_AXIS_RIGHT_TRIGGER;
        stick.invert = true;
        dash.bindings.push_back(stick);
        actions.push_back(dash);
        return actions;
    }

    // project.yaml's input node as the older editor wrote it (Stream's encodeInputActions:
    // flow maps per binding, numbers in the shortest form)
    std::string olderEditorInputNode(const std::vector<InputManagerAction>& actions) {
        YAML::Node input;
        input["version"] = 1;
        const nlohmann::json json = nlohmann::json::parse(InputManagerFormat::actionsToJson(actions));
        YAML::Node list(YAML::NodeType::Sequence);
        for (const nlohmann::json& action : json["actions"]) {
            YAML::Node node;
            node["name"] = action["name"].get<std::string>();
            node["type"] = action["type"].get<std::string>();
            if (action.contains("description")) node["description"] = action["description"].get<std::string>();
            if (action.contains("press_point")) node["press_point"] = action["press_point"].get<double>();
            YAML::Node bindings(YAML::NodeType::Sequence);
            for (const nlohmann::json& binding : action["bindings"]) {
                YAML::Node b;
                for (auto it = binding.begin(); it != binding.end(); ++it) {
                    if (it.value().is_array()) {
                        YAML::Node controls(YAML::NodeType::Sequence);
                        for (const nlohmann::json& name : it.value()) controls.push_back(name.get<std::string>());
                        controls.SetStyle(YAML::EmitterStyle::Flow);
                        b[it.key()] = controls;
                    } else if (it.value().is_string()) {
                        b[it.key()] = it.value().get<std::string>();
                    } else if (it.value().is_boolean()) {
                        b[it.key()] = it.value().get<bool>();
                    } else if (it.value().is_number_integer()) {
                        b[it.key()] = it.value().get<int>();
                    } else {
                        char text[32];
                        std::snprintf(text, sizeof(text), "%.6g", it.value().get<double>());
                        b[it.key()] = std::string(text);
                    }
                }
                b.SetStyle(YAML::EmitterStyle::Flow);
                bindings.push_back(b);
            }
            node["bindings"] = bindings;
            list.push_back(node);
        }
        input["actions"] = list;
        YAML::Node root;
        root["name"] = "older";
        root["input"] = input;
        return YAML::Dump(root);
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::vector<InputManagerAction> managerActionsOf(const std::string& assetText) {
        InputActionAssetDesc asset;
        std::string error;
        EXPECT_TRUE(InputActionAssetDesc::parse(assetText, asset, error)) << error;
        const InputActionMapDesc* map = asset.findMap(InputManagerFormat::MAP_NAME);
        EXPECT_NE(map, nullptr);
        return map ? InputManagerFormat::fromMap(*map) : std::vector<InputManagerAction>();
    }

    class ProjectInputTest : public ::testing::Test {
    protected:
        void TearDown() override {
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
        }
    };

}

TEST_F(ProjectInputTest, DefaultsWriteNoNode) {
    Project project;
    EXPECT_TRUE(project.getInputActionsAsset().empty());
    EXPECT_EQ(InputManagerFormat::actionsToJson(project.getInputManagerActions()),
              InputManagerFormat::actionsToJson(InputManagerFormat::defaults()));
    const YAML::Node node = Stream::encodeProject(&project);
    EXPECT_FALSE(node["input"]) << YAML::Dump(node["input"]);
}

TEST_F(ProjectInputTest, AnOlderEditorsInputNodeBecomesTheInputManagerMap) {
    const std::string yaml = olderEditorInputNode(customActions());
    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    const InputActionsMigration& migration = loaded.getInputActionsMigration();
    ASSERT_TRUE(migration.pending) << yaml;
    EXPECT_EQ(migration.asset.filename().generic_string(), "InputManager.inputactions");
    EXPECT_EQ(loaded.getInputActionsAsset(), migration.asset);
    EXPECT_EQ(migration.actions, customActions().size());
    // the same actions, every value kept
    EXPECT_EQ(InputManagerFormat::actionsToJson(managerActionsOf(migration.text)), InputManagerFormat::actionsToJson(customActions())) << yaml;
    EXPECT_EQ(InputManagerFormat::actionsToJson(loaded.getInputManagerActions()), InputManagerFormat::actionsToJson(customActions()));

    // project.yaml then points to the asset (S22's node)
    const YAML::Node saved = Stream::encodeProject(&loaded);
    ASSERT_TRUE(saved["input"]);
    EXPECT_FALSE(saved["input"]["version"]);
    EXPECT_EQ(saved["input"]["actions"].as<std::string>(), migration.asset.generic_string());

    // what Play and the game load: the asset, read by the engine's Input Actions
    ASSERT_TRUE(InputActions::loadFromText("InputManager.inputactions", migration.text));
    EXPECT_TRUE(InputActions::hasAction("1"));
    EXPECT_EQ(InputActions::getActionNames().size(), 12u);
    EXPECT_EQ(InputActions::toJson(), InputManagerFormat::actionsToJson(customActions()));
}

TEST_F(ProjectInputTest, AnOldProjectWithoutTheNodeHasTheDefaults) {
    Project loaded;
    loaded.setInputActionsAsset("assets/old.inputactions");
    Stream::decodeProject(&loaded, YAML::Load("name: old\n"), Workspace());
    EXPECT_TRUE(loaded.getInputActionsAsset().empty());
    EXPECT_FALSE(loaded.getInputActionsMigration().pending);
    EXPECT_EQ(loaded.getInputManagerActions().size(), 11u);
    InputActions::loadDefaults();
    EXPECT_EQ(InputActions::getActionNames().size(), 11u);
}

TEST_F(ProjectInputTest, AnUnreadableNodeIsKeptAsItWas) {
    const std::string yaml = "name: typo\n"
                             "input:\n"
                             "  version: 1\n"
                             "  actions:\n"
                             "    - name: Dash\n"
                             "      bindings:\n"
                             "        - {kind: buttons, positive: [lft_shift]}\n";
    Project loaded;
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    fflush(stdout);
    testing::internal::GetCapturedStderr();
    testing::internal::GetCapturedStdout();
    // the defaults run, the node is written back untouched
    EXPECT_TRUE(loaded.getInputActionsAsset().empty());
    EXPECT_FALSE(loaded.getInputActionsMigration().pending);
    EXPECT_EQ(loaded.getInputManagerActions().size(), 11u);
    const YAML::Node saved = Stream::encodeProject(&loaded);
    ASSERT_TRUE(saved["input"]);
    EXPECT_EQ(saved["input"]["actions"][0]["bindings"][0]["positive"][0].as<std::string>(), "lft_shift");
    // until an actions asset is set
    loaded.setInputActionsAsset("assets/game.inputactions");
    EXPECT_EQ(Stream::encodeProject(&loaded)["input"]["actions"].as<std::string>(), "assets/game.inputactions");
}

TEST_F(ProjectInputTest, TheConvertedAssetIsWrittenOnceAndFoundAgain) {
    jasperengine::test::TestTempDir dir("project-input");
    const std::filesystem::path root = dir.path();
    std::filesystem::create_directories(root / "assets");
    std::ofstream(root / "project.yaml") << "name: older\nassetsDir: assets\n" << olderEditorInputNode(customActions()).substr(std::string("name: older\n").size());

    std::string first;
    {
        Project project;
        project.setSkipProjectLock(true);
        ASSERT_TRUE(project.loadProject(root, false));
        EXPECT_EQ(project.getInputActionsAsset().generic_string(), "assets/InputManager.inputactions");
        EXPECT_FALSE(project.getInputActionsMigration().pending) << "written when the project loads";
        first = readText(root / "assets" / "InputManager.inputactions");
        EXPECT_EQ(InputManagerFormat::actionsToJson(managerActionsOf(first)), InputManagerFormat::actionsToJson(customActions()));
        // opening a project never rewrites project.yaml: it keeps the older node until saved
        EXPECT_NE(readText(root / "project.yaml").find("version: 1"), std::string::npos);
    }
    // the same node again (not saved meanwhile): the asset as it is now, edits made to it included
    {
        std::string edited = first;
        const size_t at = edited.find("\"Horizontal\"");
        ASSERT_NE(at, std::string::npos);
        edited.replace(at, std::string("\"Horizontal\"").size(), "\"Sideways\"");
        std::ofstream(root / "assets" / "InputManager.inputactions", std::ios::binary) << edited;
        Project project;
        project.setSkipProjectLock(true);
        ASSERT_TRUE(project.loadProject(root, false));
        EXPECT_TRUE(project.getInputActionsMigration().reused);
        EXPECT_EQ(project.getInputActionsAsset().generic_string(), "assets/InputManager.inputactions");
        EXPECT_EQ(project.getInputManagerActions()[0].name, "Sideways");
        EXPECT_FALSE(std::filesystem::exists(root / "assets" / "InputManager 2.inputactions"));
        // saved: project.yaml points to the asset, the next load reads it as S22's
        ASSERT_TRUE(project.saveProjectFile());
        const YAML::Node yaml = YAML::LoadFile((root / "project.yaml").string());
        EXPECT_EQ(yaml["input"]["actions"].as<std::string>(), "assets/InputManager.inputactions");
    }
    // another file of that name that is not this conversion stays: the next free name
    {
        std::ofstream(root / "project.yaml") << "name: older\nassetsDir: assets\n" << olderEditorInputNode(customActions()).substr(std::string("name: older\n").size());
        std::ofstream(root / "assets" / "InputManager.inputactions", std::ios::binary) << "{\"version\":1,\"maps\":[]}\n";
        Project project;
        project.setSkipProjectLock(true);
        ASSERT_TRUE(project.loadProject(root, false));
        EXPECT_EQ(project.getInputActionsAsset().generic_string(), "assets/InputManager 2.inputactions");
        EXPECT_EQ(readText(root / "assets" / "InputManager.inputactions"), "{\"version\":1,\"maps\":[]}\n");
        EXPECT_TRUE(std::filesystem::exists(root / "assets" / "InputManager 2.inputactions"));
    }
}
