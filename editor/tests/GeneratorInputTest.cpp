// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Input in builds (docs/specs/S22-gameplay.md R39 and "Upstream absorption"):
// an older editor's Input Manager actions reach the build as the project's actions asset (game.json
// names it, player::applySettings loads it), which the engine reads back as the same actions.

#include <gtest/gtest.h>

#include "input/InputActions.h"
#include "input/InputManagerFormat.h"

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class GeneratorInputTest : public ::testing::Test {
    protected:
        void TearDown() override {
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
        }
    };

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

