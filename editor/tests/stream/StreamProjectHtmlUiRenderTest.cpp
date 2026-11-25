// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's HTML UI rendering fields (S16 R25, R30, AC19): htmlUiGlyphAtlasMiB,
// htmlUiSdfPrimitives and htmlUiTransformAwareText take their defaults when a project saved before
// them loads, round-trip, are clamped into their range, are undoable through
// HtmlUiRenderSettingsCmd and reach an exported player through game.json (S98 D1). S92 (R16,
// AC15): htmlUiMsaaSamples takes 0 (Auto, new projects), a project without it has 4, and game.json
// carries the value. The scene's HTML UI settings are undoable through ScenePropertyCmd
// (the scene Inspector's and set_scene_settings' edit).

#include "cook/GameConfig.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/type/HtmlUiRenderSettingsCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "player/PlayerConfig.h"

#include <gtest/gtest.h>

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

TEST(StreamProjectHtmlUiRender, MsaaAutoForNewProjectsFourWithoutTheKey) {
    Project project;
    EXPECT_EQ(project.getHtmlUiMsaaSamples(), 0) << "a new project is Auto";
    EXPECT_EQ(Project::defaultHtmlUiMsaaSamples, 0);
    // a project.yaml from before S92 without the key: 4, its meaning then
    Stream::decodeProject(&project, YAML::Load("vsync: true\n"), Workspace());
    EXPECT_EQ(project.getHtmlUiMsaaSamples(), 4);
    // 1, 2, 4 and 8 keep their meaning
    for (int samples : {1, 2, 4, 8}) {
        Stream::decodeProject(&project, YAML::Load("htmlUiMsaaSamples: " + std::to_string(samples) + "\n"), Workspace());
        EXPECT_EQ(project.getHtmlUiMsaaSamples(), samples);
    }
    // 0 round-trips
    project.setHtmlUiMsaaSamples(0);
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_EQ(encoded["htmlUiMsaaSamples"].as<int>(), 0);
    Project loaded;
    loaded.setHtmlUiMsaaSamples(8);
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_EQ(loaded.getHtmlUiMsaaSamples(), 0);
    EXPECT_TRUE(Project::isValidHtmlUiMsaaSamples(0));
    EXPECT_FALSE(Project::isValidHtmlUiMsaaSamples(3));
}

TEST(StreamProjectHtmlUiRender, ExportsCarryMsaaAuto) {
    // every build reads game.json (S98 D1): a new project's Auto (0) is what it carries
    Project project;
    const jasperengine::player::PlayerConfig game = editor::cook::makeGameConfig(project, editor::cook::GameConfigTarget());
    EXPECT_EQ(game.htmlUiMsaa, 0);

    // 0 written and read; a game.json without it is Auto too
    using jasperengine::player::PlayerConfig;
    PlayerConfig config;
    EXPECT_EQ(config.htmlUiMsaa, 0);
    PlayerConfig read;
    read.htmlUiMsaa = 4;
    std::string error;
    ASSERT_TRUE(PlayerConfig::fromJson(config.toJson(), read, error)) << error;
    EXPECT_EQ(read.htmlUiMsaa, 0);
}

TEST(StreamProjectHtmlUiRender, SceneSettingsAreUndoable) {
    // (a scene of a project without a directory: the commands edit it and mark it modified)
    Project project;
    Scene scene;
    SceneProject sceneProject;
    sceneProject.id = 9201;
    sceneProject.name = "HtmlUi";
    sceneProject.scene = &scene;
    project.getScenes().push_back(std::move(sceneProject));
    const uint32_t sceneId = 9201;

    ScenePropertyCmd<int> samples(&project, sceneId, "html_ui_msaa_samples", 1);
    ASSERT_TRUE(samples.execute());
    EXPECT_EQ(scene.getHtmlUiMsaaSamples(), 1);
    ScenePropertyCmd<UIRenderMode> mode(&project, sceneId, "html_ui_render_mode", UIRenderMode::CAMERA);
    ASSERT_TRUE(mode.execute());
    EXPECT_EQ(scene.getHtmlUiRenderMode(), UIRenderMode::CAMERA);
    EXPECT_TRUE(project.getScene(sceneId)->isModified);
    mode.undo();
    EXPECT_EQ(scene.getHtmlUiRenderMode(), UIRenderMode::OVERLAY);
    samples.undo();
    EXPECT_EQ(scene.getHtmlUiMsaaSamples(), 0);
    EXPECT_FALSE(project.getScene(sceneId)->isModified);
    project.getScene(sceneId)->scene = nullptr;
    EXPECT_EQ(Catalog::getScenePropertyDefault<int>("html_ui_msaa_samples"), 0);
    EXPECT_EQ(Catalog::getScenePropertyDefault<UIRenderMode>("html_ui_render_mode"), UIRenderMode::OVERLAY);
}
