// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Input in project.yaml (input spec §7.4): no node until the actions differ
// from Unity's defaults, the `input` node read back as written (control names that look like
// numbers stay names), an unreadable node kept verbatim, and the project's actions applied to
// the engine's InputActions (what Play reads).

#include <gtest/gtest.h>

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "input/InputActions.h"

#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::vector<InputAction> customActions() {
        std::vector<InputAction> actions = InputActions::defaults();
        InputAction dash;
        dash.name = "1";                      // a name YAML would read as a number
        dash.type = InputActionType::Button;
        dash.description = "true: a description that looks like a bool";
        dash.pressPoint = 0.25f;
        InputBinding keys = InputActions::makeBinding(InputBindingKind::Buttons);
        keys.positive = {{InputDevice::Key, D_KEY_0}, {InputDevice::Key, D_KEY_LEFT_SHIFT}, {InputDevice::GamepadButton, D_GAMEPAD_BUTTON_X}};
        keys.gamepad = 2;
        dash.bindings.push_back(keys);
        InputBinding stick = InputActions::makeBinding(InputBindingKind::GamepadAxis);
        stick.axis = D_GAMEPAD_AXIS_RIGHT_TRIGGER;
        stick.invert = true;
        dash.bindings.push_back(stick);
        actions.push_back(dash);
        return actions;
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
    EXPECT_FALSE(project.hasCustomInputActions());
    EXPECT_EQ(project.getInputActionsJsonForBuild(), "");
    const YAML::Node node = Stream::encodeProject(&project);
    EXPECT_FALSE(node["input"]) << YAML::Dump(node["input"]);

    // set to what they already are: still the defaults
    project.setInputActions(InputActions::defaults());
    EXPECT_FALSE(project.hasCustomInputActions());
}

TEST_F(ProjectInputTest, TheInputNodeRoundTrips) {
    Project project;
    project.setInputActions(customActions());
    EXPECT_TRUE(project.hasCustomInputActions());
    // applied to the engine
    EXPECT_TRUE(InputActions::hasAction("1"));

    const YAML::Node node = Stream::encodeProject(&project);
    ASSERT_TRUE(node["input"]);
    EXPECT_EQ(node["input"]["version"].as<int>(), 1);
    const std::string yaml = YAML::Dump(node);

    InputActions::loadDefaults();
    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_TRUE(loaded.hasCustomInputActions());
    EXPECT_EQ(InputActions::actionsToJson(loaded.getInputActions()), InputActions::actionsToJson(customActions())) << yaml;
    EXPECT_TRUE(InputActions::hasAction("1"));
    EXPECT_EQ(loaded.getInputActionsJsonForBuild(), InputActions::actionsToJson(customActions()));
}

TEST_F(ProjectInputTest, AnOldProjectWithoutTheNodeHasTheDefaults) {
    Project loaded;
    loaded.setInputActions(customActions());
    Stream::decodeProject(&loaded, YAML::Load("name: old\n"), Workspace());
    EXPECT_FALSE(loaded.hasCustomInputActions());
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
    EXPECT_FALSE(loaded.hasCustomInputActions());
    EXPECT_EQ(InputActions::getActionNames().size(), 11u);
    const YAML::Node saved = Stream::encodeProject(&loaded);
    ASSERT_TRUE(saved["input"]);
    EXPECT_EQ(saved["input"]["actions"][0]["bindings"][0]["positive"][0].as<std::string>(), "lft_shift");
    // until the actions are edited
    loaded.setInputActions(customActions());
    EXPECT_EQ(Stream::encodeProject(&loaded)["input"]["actions"][11]["name"].as<std::string>(), "1");
}
