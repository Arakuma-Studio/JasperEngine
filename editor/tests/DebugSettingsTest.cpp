// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Debug (feature debug-overlay, spec 7): the debug: node of project.yaml (none
// for the defaults, read back as written, bad values warned and dropped, older projects get the
// defaults), the mode resolved per build type, the generated init of exports, and the undoable
// command the dialog and MCP use.

#include <gtest/gtest.h>

#include "DebugSettings.h"
#include "Generator.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/type/DebugSettingsCmd.h"

#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    DebugSettings customSettings() {
        DebugSettings debug;
        debug.overlay = DebugSettings::Mode::AllBuilds;
        debug.toggleKey = "f12";
        debug.statsKey = "";
        debug.gesture = DebugOverlaySettings::Gesture::FourFingerTap;
        debug.popup = DebugOverlaySettings::Popup::AllDevices;
        debug.openOnError = true;
        debug.stats = DebugOverlaySettings::Stats::Full;
        debug.statsCorner = DebugOverlaySettings::Corner::BottomRight;
        debug.allowLua = DebugSettings::AllowLua::Never;
        debug.logBufferSize = 2000;
        debug.logStackTraces = LogStackTraces::All;
        return debug;
    }

    size_t count(const std::string& text, const std::string& part) {
        size_t n = 0;
        for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) n++;
        return n;
    }

}

TEST(DebugSettingsTest, DefaultsWriteNoNode) {
    Project project;
    EXPECT_EQ(project.getDebugSettings(), DebugSettings());
    EXPECT_FALSE(Stream::encodeProject(&project)["debug"]);
}

TEST(DebugSettingsTest, TheDebugNodeRoundTrips) {
    Project project;
    project.setDebugSettings(customSettings());
    const YAML::Node node = Stream::encodeProject(&project);
    ASSERT_TRUE(node["debug"]);
    EXPECT_EQ(node["debug"]["overlay"].as<std::string>(), "always");
    EXPECT_EQ(node["debug"]["gesture"].as<std::string>(), "four_finger_tap");
    EXPECT_EQ(node["debug"]["statsKey"].as<std::string>(), "");
    const std::string yaml = YAML::Dump(node);

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_EQ(loaded.getDebugSettings(), customSettings()) << yaml;
}

TEST(DebugSettingsTest, OldProjectsAndBadValues) {
    Project loaded;
    loaded.setDebugSettings(customSettings());
    Stream::decodeProject(&loaded, YAML::Load("name: old\n"), Workspace());
    EXPECT_EQ(loaded.getDebugSettings(), DebugSettings());

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    Stream::decodeProject(&loaded, YAML::Load("name: typo\ndebug:\n  overlay: sometimes\n  toggleKey: notakey\n  stats: fps\n  logBufferSize: 999999999\n"), Workspace());
    fflush(stdout);
    testing::internal::GetCapturedStderr();
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(loaded.getDebugSettings().overlay, DebugSettings::Mode::Development);
    EXPECT_EQ(loaded.getDebugSettings().toggleKey, "backquote");
    EXPECT_EQ(loaded.getDebugSettings().stats, DebugOverlaySettings::Stats::Fps);
    EXPECT_EQ(loaded.getDebugSettings().logBufferSize, DebugSettings::maxLogBufferSize);
}

TEST(DebugSettingsTest, ModesResolvePerBuildType) {
    DebugSettings debug;
    EXPECT_TRUE(debug.toOverlaySettings(true).enabled);
    EXPECT_FALSE(debug.toOverlaySettings(false).enabled);
    EXPECT_TRUE(debug.toOverlaySettings(true).allowLua);
    EXPECT_FALSE(debug.toOverlaySettings(false).allowLua);
    debug.overlay = DebugSettings::Mode::AllBuilds;
    debug.allowLua = DebugSettings::AllowLua::AllBuilds;
    EXPECT_TRUE(debug.toOverlaySettings(false).enabled);
    EXPECT_TRUE(debug.toOverlaySettings(false).allowLua);
    debug.overlay = DebugSettings::Mode::Never;
    debug.allowLua = DebugSettings::AllowLua::Never;
    EXPECT_FALSE(debug.toOverlaySettings(true).enabled);
    EXPECT_FALSE(debug.toOverlaySettings(true).allowLua);
}

TEST(DebugSettingsTest, Names) {
    DebugOverlaySettings::Gesture gesture;
    EXPECT_TRUE(DebugSettingsNames::parse("Three-Finger Hold", gesture));
    EXPECT_EQ(gesture, DebugOverlaySettings::Gesture::ThreeFingerHold);
    LogStackTraces traces;
    EXPECT_TRUE(DebugSettingsNames::parse("errors", traces));
    EXPECT_EQ(traces, LogStackTraces::Errors);
    EXPECT_FALSE(DebugSettingsNames::parse("sometimes", traces));
    EXPECT_EQ(DebugSettingsNames::name(DebugOverlaySettings::Corner::TopRight), "top_right");
    EXPECT_TRUE(DebugSettingsNames::isKeyName("`"));
    EXPECT_TRUE(DebugSettingsNames::isKeyName("F3"));
    EXPECT_TRUE(DebugSettingsNames::isKeyName(""));
    EXPECT_FALSE(DebugSettingsNames::isKeyName("notakey"));
}

// the generated init: the development flag from the build type, the log, DebugOverlay::configure
TEST(DebugSettingsTest, GeneratedInit) {
    const std::string defaults = Generator::debugInitSource(DebugSettings());
    EXPECT_NE(defaults.find("#if defined(JASPERENGINE_DEVELOPMENT_BUILD) || defined(DEBUG)\n    Engine::setDevelopmentBuild(true);\n#endif"), std::string::npos) << defaults;
    EXPECT_NE(defaults.find("debugOverlay.enabled = Engine::isDevelopmentBuild();"), std::string::npos) << defaults;
    EXPECT_NE(defaults.find("debugOverlay.allowLua = Engine::isDevelopmentBuild();"), std::string::npos) << defaults;
    EXPECT_NE(defaults.find("DebugOverlay::configure(debugOverlay);"), std::string::npos);
    EXPECT_EQ(defaults.find("Log::setBufferSize"), std::string::npos);
    EXPECT_EQ(defaults.find("toggleKey"), std::string::npos);

    const std::string custom = Generator::debugInitSource(customSettings());
    EXPECT_NE(custom.find("debugOverlay.enabled = true;"), std::string::npos) << custom;
    EXPECT_NE(custom.find("debugOverlay.allowLua = false;"), std::string::npos);
    EXPECT_NE(custom.find("Log::setBufferSize(2000);"), std::string::npos);
    EXPECT_NE(custom.find("Log::setStackTraces(LogStackTraces::All);"), std::string::npos);
    EXPECT_NE(custom.find("debugOverlay.toggleKey = \"f12\";"), std::string::npos);
    EXPECT_NE(custom.find("debugOverlay.statsKey = \"\";"), std::string::npos);
    EXPECT_NE(custom.find("debugOverlay.gesture = DebugOverlaySettings::Gesture::FourFingerTap;"), std::string::npos);
    EXPECT_NE(custom.find("debugOverlay.popup = DebugOverlaySettings::Popup::AllDevices;"), std::string::npos);
    EXPECT_NE(custom.find("debugOverlay.openOnError = true;"), std::string::npos);
    EXPECT_NE(custom.find("debugOverlay.stats = DebugOverlaySettings::Stats::Full;"), std::string::npos);
    EXPECT_NE(custom.find("debugOverlay.statsCorner = DebugOverlaySettings::Corner::BottomRight;"), std::string::npos);
    EXPECT_EQ(count(custom, "DebugOverlay::configure"), 1u);

    DebugSettings never;
    never.overlay = DebugSettings::Mode::Never;
    EXPECT_NE(Generator::debugInitSource(never).find("debugOverlay.enabled = false;"), std::string::npos);
}

// the dialog's OK and MCP: one undoable step
TEST(DebugSettingsTest, CommandUndoes) {
    Project project;
    DebugSettingsCmd command(&project, project.getDebugSettings(), customSettings());
    EXPECT_TRUE(command.execute());
    EXPECT_EQ(project.getDebugSettings(), customSettings());
    command.undo();
    EXPECT_EQ(project.getDebugSettings(), DebugSettings());
}

// the editor's Play: a development build, the overlay of development mode on
TEST(DebugSettingsTest, PlayApplies) {
    Project project;
    DebugSettings debug;
    debug.logBufferSize = 123;
    project.setDebugSettings(debug);
    project.applyDebugSettingsForPlay();
    EXPECT_TRUE(Engine::isDevelopmentBuild());
    EXPECT_TRUE(DebugOverlay::isEnabled());
    EXPECT_TRUE(DebugOverlay::getSettings().allowLua);
    EXPECT_EQ(Log::getBufferSize(), 123u);
    DebugOverlay::shutdown();
    Log::setBufferSize(500);
    EXPECT_FALSE(DebugOverlay::isEnabled());
}

// changed during the editor's Play (the dialog's OK, its undo): the running game takes them at
// once, the console stays open, a stats mode picked stays unless the setting changed
TEST(DebugSettingsTest, ChangesApplyDuringPlay) {
    Project project;
    project.applyDebugSettingsForPlay();
    DebugOverlay::showConsole();
    DebugOverlay::setStatsMode(DebugOverlaySettings::Stats::Fps);

    DebugSettings changed = project.getDebugSettings();
    changed.logBufferSize = 300;
    changed.logStackTraces = LogStackTraces::All;
    changed.openOnError = true;
    DebugSettingsCmd command(&project, project.getDebugSettings(), changed);
    EXPECT_TRUE(command.execute());
    EXPECT_EQ(Log::getBufferSize(), 300u);
    EXPECT_EQ(Log::getStackTraces(), LogStackTraces::All);
    EXPECT_TRUE(DebugOverlay::getSettings().openOnError);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Fps);

    DebugSettings full = changed;
    full.stats = DebugOverlaySettings::Stats::Full;
    project.setDebugSettings(full);
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Full);
    DebugSettings never = full;
    never.overlay = DebugSettings::Mode::Never;
    project.setDebugSettings(never);
    EXPECT_FALSE(DebugOverlay::isEnabled());

    command.undo();
    EXPECT_EQ(Log::getBufferSize(), 500u);
    EXPECT_TRUE(DebugOverlay::isEnabled());
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "turned off and on: closed";
    DebugOverlay::shutdown();
    Log::setBufferSize(500);
    Log::setStackTraces(LogStackTraces::Warnings);

    // what the game set itself stays when another setting changes
    project.applyDebugSettingsForPlay();
    Log::setBufferSize(42);   // Log.bufferSize = 42 in a script
    DebugSettings popup = project.getDebugSettings();
    popup.popup = DebugOverlaySettings::Popup::AllDevices;
    project.setDebugSettings(popup);
    EXPECT_EQ(Log::getBufferSize(), 42u);
    EXPECT_EQ(DebugOverlay::getSettings().popup, DebugOverlaySettings::Popup::AllDevices);
    DebugOverlay::shutdown();
    Log::setBufferSize(500);

    // not playing: the engine keeps what it has
    Project idle;
    idle.setDebugSettings(changed);
    EXPECT_EQ(Log::getBufferSize(), 500u);
}
