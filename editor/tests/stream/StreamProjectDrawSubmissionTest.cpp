// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's GPU Instancing and Shadow LOD Bias (spec S55 R11): a project saved before them
// takes the defaults (on, 1) and writes no key while they hold; they round-trip under
// `graphics`, the bias is clamped, the change is undoable through DrawSubmissionSettingsCmd and
// applies to the engine at once, and an export's generated main carries what is not the default.

#include "Engine.h"
#include "Generator.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/type/DrawSubmissionSettingsCmd.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    struct RestoreEngine {
        ~RestoreEngine(){
            Engine::setGpuInstancingEnabled(true);
            Engine::setShadowLodBias(1);
        }
    };
}

TEST(StreamProjectDrawSubmission, MissingKeysGiveTheDefaults) {
    RestoreEngine restore;
    Project project;
    DrawSubmissionSettings changed;
    changed.gpuInstancing = false;
    changed.shadowLodBias = 3;
    project.setDrawSubmissionSettings(changed);
    // a project.yaml from before S55
    Stream::decodeProject(&project, YAML::Load("vsync: true\ngraphics:\n  shadowTier:\n    web: low\n"), Workspace());
    EXPECT_TRUE(project.getDrawSubmissionSettings() == DrawSubmissionSettings());
    EXPECT_TRUE(Engine::isGpuInstancingEnabled());
    EXPECT_EQ(Engine::getShadowLodBias(), 1);
    // the defaults write nothing
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["graphics"] && encoded["graphics"]["gpuInstancing"]);
    EXPECT_FALSE(encoded["graphics"] && encoded["graphics"]["shadowLodBias"]);
}

TEST(StreamProjectDrawSubmission, RoundTripsAndClamps) {
    RestoreEngine restore;
    Project project;
    DrawSubmissionSettings settings;
    settings.gpuInstancing = false;
    settings.shadowLodBias = 2;
    project.setDrawSubmissionSettings(settings);
    EXPECT_FALSE(Engine::isGpuInstancingEnabled());
    EXPECT_EQ(Engine::getShadowLodBias(), 2);
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["graphics"]["gpuInstancing"].as<bool>());
    EXPECT_EQ(encoded["graphics"]["shadowLodBias"].as<int>(), 2);
    Project loaded;
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_TRUE(loaded.getDrawSubmissionSettings() == settings);

    Stream::decodeProject(&loaded, YAML::Load("graphics:\n  shadowLodBias: 99\n"), Workspace());
    EXPECT_EQ(loaded.getDrawSubmissionSettings().shadowLodBias, MAX_SHADOW_LOD_BIAS);
    Stream::decodeProject(&loaded, YAML::Load("graphics:\n  shadowLodBias: -4\n"), Workspace());
    EXPECT_EQ(loaded.getDrawSubmissionSettings().shadowLodBias, 0);
}

TEST(StreamProjectDrawSubmission, CommandIsUndoable) {
    RestoreEngine restore;
    Project project;
    const DrawSubmissionSettings before = project.getDrawSubmissionSettings();
    DrawSubmissionSettings after;
    after.gpuInstancing = false;
    after.shadowLodBias = 0;
    DrawSubmissionSettingsCmd command(&project, after);
    ASSERT_TRUE(command.execute());
    EXPECT_TRUE(project.getDrawSubmissionSettings() == after);
    EXPECT_FALSE(Engine::isGpuInstancingEnabled());
    command.undo();
    EXPECT_TRUE(project.getDrawSubmissionSettings() == before);
    EXPECT_TRUE(Engine::isGpuInstancingEnabled());
    EXPECT_EQ(Engine::getShadowLodBias(), 1);
}

TEST(StreamProjectDrawSubmission, ExportCarriesWhatIsNotTheDefault) {
    for (bool changed : {false, true}){
        jasperengine::test::TempDir dir;
        Generator generator;
        DrawSubmissionSettings settings;
        if (changed){
            settings.gpuInstancing = false;
            settings.shadowLodBias = 3;
        }
        generator.setDrawSubmission(settings);
        generator.configure({}, "game", {}, {}, dir.path(), dir.path() / ".jasperengine", dir.path() / "assets", dir.path() / "lua", {}, 20,
                            Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, WindowSettings(), LoadingSettings(), TagsAndLayers(), 4,
                            ThreadingProjectSettings(), HtmlUiRenderSettings());
        std::ifstream in(dir.path() / ".jasperengine" / "generated" / "main.cpp");
        ASSERT_TRUE(in.good());
        std::stringstream text;
        text << in.rdbuf();
        EXPECT_EQ(text.str().find("Engine::setGpuInstancingEnabled(false);") != std::string::npos, changed);
        EXPECT_EQ(text.str().find("Engine::setShadowLodBias(3);") != std::string::npos, changed);
    }
}
