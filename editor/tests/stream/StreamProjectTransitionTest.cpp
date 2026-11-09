// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's Screen Transitions (S48 R41, R47): a project saved before them takes the
// defaults and writes no key while it holds them; changed keys round-trip under
// graphics.screenTransitions, the change is undoable through TransitionSettingsCmd and applies to
// the engine (Lua's defaults) at once, and both export forms carry them when they differ.

#include "Generator.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/type/TransitionSettingsCmd.h"
#include "player/PlayerConfig.h"
#include "pixelart/ScreenTransition.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::pixelart::ScreenTransition;
using jasperengine::pixelart::TransitionDefaults;
using jasperengine::pixelart::TransitionKind;
using jasperengine::player::PlayerConfig;

namespace {
    struct RestoreTransitions {
        ~RestoreTransitions(){
            ScreenTransition::setDefaults(TransitionDefaults());
        }
    };

    TransitionDefaults changedDefaults(){
        TransitionDefaults settings;
        settings.kind = TransitionKind::CRT_OFF;
        settings.duration = 1.25f;
        settings.color = Vector4(1.0f, 0.0f, 128.0f / 255.0f, 1.0f);   // whole bytes: the file stores "#rrggbb"
        settings.ramp = "palettes/ui.palette#ramp:gold";
        settings.rim = 3;
        settings.pixelSize = 4;
        settings.coverUI = false;
        return settings;
    }
}

TEST(StreamProjectTransition, MissingKeysGiveTheDefaults) {
    RestoreTransitions restore;
    Project project;
    project.setTransitionDefaults(changedDefaults());
    Stream::decodeProject(&project, YAML::Load("vsync: true\ngraphics:\n  shadowTier:\n    web: low\n"), Workspace());
    EXPECT_TRUE(project.getTransitionDefaults() == TransitionDefaults());
    EXPECT_TRUE(ScreenTransition::getDefaults() == TransitionDefaults());
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["graphics"] && encoded["graphics"]["screenTransitions"]);
}

TEST(StreamProjectTransition, RoundTrips) {
    RestoreTransitions restore;
    Project project;
    const TransitionDefaults settings = changedDefaults();
    project.setTransitionDefaults(settings);
    EXPECT_EQ(ScreenTransition::getDefaults().kind, TransitionKind::CRT_OFF);
    EXPECT_FALSE(ScreenTransition::isCoverUI());
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    const YAML::Node node = encoded["graphics"]["screenTransitions"];
    ASSERT_TRUE(node && node.IsMap());
    EXPECT_EQ(node["kind"].as<std::string>(), "crt_off");
    EXPECT_EQ(node["color"].as<std::string>(), "#ff0080");
    Project loaded;
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_TRUE(loaded.getTransitionDefaults() == settings);
}

TEST(StreamProjectTransition, CommandIsUndoable) {
    RestoreTransitions restore;
    Project project;
    const TransitionDefaults before = project.getTransitionDefaults();
    TransitionSettingsCmd command(&project, changedDefaults());
    ASSERT_TRUE(command.execute());
    EXPECT_TRUE(project.getTransitionDefaults() == changedDefaults());
    EXPECT_EQ(ScreenTransition::getDefaults().pixelSize, 4);
    command.undo();
    EXPECT_TRUE(project.getTransitionDefaults() == before);
    EXPECT_TRUE(ScreenTransition::getDefaults() == TransitionDefaults());
}

TEST(StreamProjectTransition, ExportsCarryThem) {
    for (bool changed : {false, true}){
        jasperengine::test::TempDir dir;
        Generator generator;
        generator.setTransitionDefaults(changed ? changedDefaults() : TransitionDefaults());
        generator.configure({}, "game", {}, {}, dir.path(), dir.path() / ".jasperengine", dir.path() / "assets", dir.path() / "lua", {}, 20,
                            Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, WindowSettings(), LoadingSettings(), TagsAndLayers(), 4,
                            ThreadingProjectSettings(), HtmlUiRenderSettings());
        std::ifstream in(dir.path() / ".jasperengine" / "generated" / "main.cpp");
        ASSERT_TRUE(in.good());
        std::stringstream text;
        text << in.rdbuf();
        EXPECT_EQ(text.str().find("ScreenTransition::setDefaults(transitionDefaults);") != std::string::npos, changed);
        if (changed) {
            EXPECT_NE(text.str().find("transitionDefaults.ramp = \"palettes/ui.palette#ramp:gold\";"), std::string::npos);
        }
    }
    // the prebuilt player's game.json
    PlayerConfig config;
    EXPECT_FALSE(config.toJson().contains("transitions"));
    config.transitions = changedDefaults();
    PlayerConfig read;
    std::string error;
    ASSERT_TRUE(PlayerConfig::fromJson(config.toJson(), read, error)) << error;
    EXPECT_TRUE(read.transitions == changedDefaults());
}
