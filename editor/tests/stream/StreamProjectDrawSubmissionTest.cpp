// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's GPU Instancing (spec S55 R11): a project saved before it takes the default (on)
// and writes no key while it holds; off round-trips under `graphics`, the change is undoable
// through DrawSubmissionSettingsCmd and applies to the engine at once, and an export's generated
// main carries it when off. (Shadow LOD Bias is S56's quality.shadow_lod_bias.)

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
        }
    };
}

TEST(StreamProjectDrawSubmission, MissingKeysGiveTheDefaults) {
    RestoreEngine restore;
    Project project;
    DrawSubmissionSettings changed;
    changed.gpuInstancing = false;
    project.setDrawSubmissionSettings(changed);
    // a project.yaml from before S55
    Stream::decodeProject(&project, YAML::Load("vsync: true\ngraphics:\n  shadowTier:\n    web: low\n"), Workspace());
    EXPECT_TRUE(project.getDrawSubmissionSettings() == DrawSubmissionSettings());
    EXPECT_TRUE(Engine::isGpuInstancingEnabled());
    // the default writes nothing
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["graphics"] && encoded["graphics"]["gpuInstancing"]);
}

TEST(StreamProjectDrawSubmission, RoundTrips) {
    RestoreEngine restore;
    Project project;
    DrawSubmissionSettings settings;
    settings.gpuInstancing = false;
    project.setDrawSubmissionSettings(settings);
    EXPECT_FALSE(Engine::isGpuInstancingEnabled());
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["graphics"]["gpuInstancing"].as<bool>());
    Project loaded;
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_TRUE(loaded.getDrawSubmissionSettings() == settings);
}

TEST(StreamProjectDrawSubmission, CommandIsUndoable) {
    RestoreEngine restore;
    Project project;
    const DrawSubmissionSettings before = project.getDrawSubmissionSettings();
    DrawSubmissionSettings after;
    after.gpuInstancing = false;
    DrawSubmissionSettingsCmd command(&project, after);
    ASSERT_TRUE(command.execute());
    EXPECT_TRUE(project.getDrawSubmissionSettings() == after);
    EXPECT_FALSE(Engine::isGpuInstancingEnabled());
    command.undo();
    EXPECT_TRUE(project.getDrawSubmissionSettings() == before);
    EXPECT_TRUE(Engine::isGpuInstancingEnabled());
}

TEST(StreamProjectDrawSubmission, ExportCarriesItWhenOff) {
    for (bool off : {false, true}){
        jasperengine::test::TempDir dir;
        Generator generator;
        DrawSubmissionSettings settings;
        settings.gpuInstancing = !off;
        generator.setDrawSubmission(settings);
        generator.configure({}, "game", {}, {}, dir.path(), dir.path() / ".jasperengine", dir.path() / "assets", dir.path() / "lua", {}, 20,
                            Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, WindowSettings(), LoadingSettings(), TagsAndLayers(), 4,
                            ThreadingProjectSettings(), HtmlUiRenderSettings());
        std::ifstream in(dir.path() / ".jasperengine" / "generated" / "main.cpp");
        ASSERT_TRUE(in.good());
        std::stringstream text;
        text << in.rdbuf();
        EXPECT_EQ(text.str().find("Engine::setGpuInstancingEnabled(false);") != std::string::npos, off);
    }
}
