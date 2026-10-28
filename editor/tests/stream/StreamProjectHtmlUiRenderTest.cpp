// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's HTML UI rendering fields (S16 R25, R30, AC19): htmlUiGlyphAtlasMiB,
// htmlUiSdfPrimitives and htmlUiTransformAwareText take their defaults when a project saved before
// them loads, round-trip, are clamped into their range, are undoable through
// HtmlUiRenderSettingsCmd and reach an exported player through the generated main.cpp.

#include "Generator.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/type/HtmlUiRenderSettingsCmd.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(StreamProjectHtmlUiRender, MissingKeysGiveTheDefaults) {
    Project project;
    HtmlUiRenderSettings changed;
    changed.glyphAtlasMiB = 64;
    changed.sdfPrimitives = false;
    changed.transformAwareText = false;
    project.setHtmlUiRenderSettings(changed);
    // a project.yaml from before S16
    YAML::Node node = YAML::Load("vsync: true\nhtmlUiMsaaSamples: 4\n");
    Stream::decodeProject(&project, node, Workspace());
    EXPECT_TRUE(project.getHtmlUiRenderSettings() == HtmlUiRenderSettings());
    EXPECT_EQ(project.getHtmlUiRenderSettings().glyphAtlasMiB, 0);
    EXPECT_TRUE(project.getHtmlUiRenderSettings().sdfPrimitives);
    EXPECT_TRUE(project.getHtmlUiRenderSettings().transformAwareText);
}

TEST(StreamProjectHtmlUiRender, RoundTripsAndClamps) {
    Project project;
    HtmlUiRenderSettings settings;
    settings.glyphAtlasMiB = 48;
    settings.sdfPrimitives = false;
    settings.transformAwareText = true;
    project.setHtmlUiRenderSettings(settings);
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_EQ(encoded["htmlUiGlyphAtlasMiB"].as<int>(), 48);
    EXPECT_FALSE(encoded["htmlUiSdfPrimitives"].as<bool>());
    EXPECT_TRUE(encoded["htmlUiTransformAwareText"].as<bool>());
    Project loaded;
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_TRUE(loaded.getHtmlUiRenderSettings() == settings);

    // out of range: clamped to 4-256 (0 stays the platform's default)
    Stream::decodeProject(&loaded, YAML::Load("htmlUiGlyphAtlasMiB: 2\n"), Workspace());
    EXPECT_EQ(loaded.getHtmlUiRenderSettings().glyphAtlasMiB, Project::minHtmlUiGlyphAtlasMiB);
    Stream::decodeProject(&loaded, YAML::Load("htmlUiGlyphAtlasMiB: 9000\n"), Workspace());
    EXPECT_EQ(loaded.getHtmlUiRenderSettings().glyphAtlasMiB, Project::maxHtmlUiGlyphAtlasMiB);
    EXPECT_TRUE(Project::isValidHtmlUiGlyphAtlasMiB(0));
    EXPECT_FALSE(Project::isValidHtmlUiGlyphAtlasMiB(3));
}

TEST(StreamProjectHtmlUiRender, CommandIsUndoable) {
    // (a project without a directory: project.yaml is not written, the settings are)
    Project project;
    const HtmlUiRenderSettings before = project.getHtmlUiRenderSettings();
    HtmlUiRenderSettings after;
    after.glyphAtlasMiB = 32;
    after.sdfPrimitives = false;
    HtmlUiRenderSettingsCmd command(&project, after);
    ASSERT_TRUE(command.execute());
    EXPECT_TRUE(project.getHtmlUiRenderSettings() == after);
    EXPECT_EQ(command.describe(), "Change HTML UI Rendering Settings");
    command.undo();
    EXPECT_TRUE(project.getHtmlUiRenderSettings() == before);
}

TEST(StreamProjectHtmlUiRender, ExportCarriesThem) {
    jasperengine::test::TempDir dir;
    Generator generator;
    HtmlUiRenderSettings settings;
    settings.glyphAtlasMiB = 24;
    settings.sdfPrimitives = false;
    settings.transformAwareText = true;
    generator.configure({}, "game", {}, {}, dir.path(), dir.path() / ".jasperengine", dir.path() / "assets", dir.path() / "lua", {}, 20,
                        Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, WindowSettings(), LoadingSettings(), TagsAndLayers(), 4,
                        ThreadingProjectSettings(), settings);
    std::ifstream in(dir.path() / ".jasperengine" / "generated" / "main.cpp");
    ASSERT_TRUE(in.good());
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_NE(text.str().find("UIDocumentSystem::setHtmlUiRenderOptions(24, false, true);"), std::string::npos);
}
