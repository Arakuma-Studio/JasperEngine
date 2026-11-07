// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S91 R28: the thresholds of the particle checks are project settings (particles.qa.*), saved in
// project.yaml only when changed, so an old project loads with the defaults.

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "settings/SettingsRegistry.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ParticleQaSettings, EveryThresholdIsASetting){
    Project project;
    SettingsContext ctx;
    ctx.projectPtr = &project;
    for (const ParticleQaField& field : particleQaFields()){
        const SettingRow* row = SettingsRegistry::get().find(field.key);
        ASSERT_NE(row, nullptr) << field.key;
        std::string error;
        const mcp::Json value = field.real ? mcp::Json(field.maximum) : mcp::Json((int)field.maximum);
        ASSERT_TRUE(row->set(ctx, value, true, error)) << field.key << ": " << error;
        EXPECT_EQ(row->get(ctx), value) << field.key;
        EXPECT_FALSE(row->set(ctx, mcp::Json(field.maximum + 1.0), true, error)) << field.key;
    }
    EXPECT_NE(project.getParticleQaSettings(), ParticleQaSettings());
}

TEST(ParticleQaSettings, RoundTripsAndAnOldProjectKeepsTheDefaults){
    Project project;
    EXPECT_FALSE(Stream::encodeProject(&project)["particleQa"]) << "defaults write nothing: old files stay as they were";
    ParticleQaSettings qa;
    qa.overdrawMobile = 6.0f;
    qa.fixRounds = 3;
    project.setParticleQaSettings(qa);
    const std::string yaml = YAML::Dump(Stream::encodeProject(&project));
    EXPECT_NE(yaml.find("overdrawMobile: 6"), std::string::npos) << yaml;
    EXPECT_EQ(yaml.find("overdrawWeb"), std::string::npos) << "only the changed ones: " << yaml;

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_EQ(loaded.getParticleQaSettings(), qa);

    Project old;
    Stream::decodeProject(&old, YAML::Load("name: Old\n"), Workspace());
    EXPECT_EQ(old.getParticleQaSettings(), ParticleQaSettings());
}
