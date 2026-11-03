// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A project saved by upstream's editor (origin/main 047eccf9b, before SYNC#4) with edited Project
// Settings > Input: editor/tests/fixtures/upstream-input-project, made by that editor over MCP
// (set_project_settings input.actions, save_project; expected-input-actions.json is what that
// editor read back as its actions). Its project.yaml has `input: {version: 1, actions: [...]}`,
// which the merge base read as S22's `input.actions` path and failed ("Failed to load project
// file!"). It loads, and the actions are the Input Manager map of an actions asset, losslessly
// (docs/specs/S22-gameplay.md "Upstream absorption" 11.4).

#include <gtest/gtest.h>

#include "Input.h"
#include "Project.h"
#include "input/InputActions.h"
#include "input/InputManagerFormat.h"
#include "support/TestTempDir.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string readText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    const fs::path fixture() {
        return fs::path(JASPER_TEST_FIXTURES_DIR) / "upstream-input-project";
    }

    class ProjectInputMigrationTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"upstream-input"};

        void SetUp() override {
            fs::copy(fixture(), dir.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::remove(dir.path() / "expected-input-actions.json");
        }

        void TearDown() override {
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
        }

        std::vector<InputManagerAction> expected() const {
            std::vector<InputManagerAction> actions;
            std::string error;
            EXPECT_TRUE(InputManagerFormat::parseActionsJson(readText(fixture() / "expected-input-actions.json"), actions, &error)) << error;
            return actions;
        }
    };

}

TEST_F(ProjectInputMigrationTest, TheFixtureIsAnUpstreamInputNode) {
    const YAML::Node yaml = YAML::LoadFile((fixture() / "project.yaml").string());
    ASSERT_TRUE(yaml["input"]);
    EXPECT_EQ(yaml["input"]["version"].as<int>(), 1);
    ASSERT_TRUE(yaml["input"]["actions"].IsSequence()) << "the shape the merge base's decodeProject threw on";
    EXPECT_GT(expected().size(), 11u);
}

TEST_F(ProjectInputMigrationTest, AnUpstreamSavedProjectWithEditedInputLoads) {
    Project project;
    project.setSkipProjectLock(true);
    // red on the merge base: input["actions"].as<std::string>() on a sequence threw
    ASSERT_TRUE(project.loadProject(dir.path(), false));

    // the actions are the Input Manager map of an actions asset in the assets directory
    const fs::path asset = project.getInputActionsAsset();
    ASSERT_FALSE(asset.empty());
    EXPECT_EQ(asset.filename().generic_string(), "InputManager.inputactions");
    ASSERT_TRUE(fs::exists(dir.path() / asset));
    EXPECT_FALSE(project.getInputActionsEnginePath().empty()) << "the asset is in the assets directory: builds ship it";

    // losslessly: what upstream's editor read back, action for action, binding for binding
    const std::vector<InputManagerAction> actions = expected();
    EXPECT_EQ(InputManagerFormat::actionsToJson(project.getInputManagerActions()), InputManagerFormat::actionsToJson(actions));
    InputActionAssetDesc desc;
    std::string error;
    ASSERT_TRUE(InputActionAssetDesc::parse(readText(dir.path() / asset), desc, error)) << error;
    ASSERT_NE(desc.findMap(InputManagerFormat::MAP_NAME), nullptr);
    EXPECT_EQ(InputManagerFormat::actionsToJson(InputManagerFormat::fromMap(*desc.findMap(InputManagerFormat::MAP_NAME))),
              InputManagerFormat::actionsToJson(actions));

    // project.yaml keeps the older node until the user saves; then it points to the asset
    EXPECT_EQ(YAML::LoadFile((dir.path() / "project.yaml").string())["input"]["version"].as<int>(), 1);
    ASSERT_TRUE(project.saveProjectFile());
    const YAML::Node saved = YAML::LoadFile((dir.path() / "project.yaml").string());
    EXPECT_EQ(saved["input"]["actions"].as<std::string>(), asset.generic_string());
    EXPECT_FALSE(saved["input"]["version"]);

    // loaded again as S22's node: the same actions, nothing converted twice
    Project again;
    again.setSkipProjectLock(true);
    ASSERT_TRUE(again.loadProject(dir.path(), false));
    EXPECT_FALSE(again.getInputActionsMigration().pending);
    EXPECT_FALSE(again.getInputActionsMigration().reused);
    EXPECT_EQ(again.getInputActionsAsset(), asset);
    EXPECT_EQ(InputManagerFormat::actionsToJson(again.getInputManagerActions()), InputManagerFormat::actionsToJson(actions));
    EXPECT_FALSE(fs::exists(dir.path() / asset.parent_path() / "InputManager 2.inputactions"));
}

TEST_F(ProjectInputMigrationTest, TheGameReadsTheEditedActions) {
    Project project;
    project.setSkipProjectLock(true);
    ASSERT_TRUE(project.loadProject(dir.path(), false));
    ASSERT_TRUE(InputActions::loadFromText(project.getInputActionsEnginePath(), readText(dir.path() / project.getInputActionsAsset())));
    EXPECT_TRUE(InputActions::hasAction("Dash"));
    EXPECT_TRUE(InputActions::hasAction("Look X"));
    EXPECT_FALSE(InputActions::hasAction("Mouse X")) << "renamed in the older editor";
    EXPECT_TRUE(InputActions::hasAction("1"));

    // Dash: left shift at once (gravity and sensitivity 0), a Button
    Input::reset();
    Input::addKeyPressed(D_KEY_LEFT_SHIFT);
    Input::beginFrame(1.0 / 60.0, InputSuppression());
    EXPECT_TRUE(InputActions::getButtonDown("Dash"));
    EXPECT_FLOAT_EQ(InputActions::getAxis("Dash"), 1.0f);
    Input::endFrame();
    Input::releaseKeyPressed(D_KEY_LEFT_SHIFT);
    // Horizontal with its edited sensitivity 6: 0.1 after one frame
    Input::addKeyPressed(D_KEY_D);
    Input::beginFrame(1.0 / 60.0, InputSuppression());
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.1f, 1e-5f);
    Input::endFrame();
    Input::releaseKeyPressed(D_KEY_D);
    Input::beginFrame(1.0 / 60.0, InputSuppression());
    Input::endFrame();
    Input::reset();
}
