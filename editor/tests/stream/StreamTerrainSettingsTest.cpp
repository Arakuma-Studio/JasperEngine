// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Generate section's two settings (T2 R60, R70, AC46): a project without them loads with the defaults,
// and a project that never generated writes no new key; they round-trip when set.

#include "Project.h"
#include "Stream.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

using namespace jasperengine::editor;

TEST(StreamTerrainSettings, GenerateKeysDefault) {
    const TerrainEditorSettings fresh = Stream::decodeTerrainEditorSettings(YAML::Load("{brushSize: 8.0}"));
    EXPECT_EQ(fresh.brushSize, 8.0f);
    EXPECT_TRUE(fresh.generateGraph.empty());
    EXPECT_EQ(fresh.generateSeed, 1u);

    const YAML::Node untouched = Stream::encodeTerrainEditorSettings(TerrainEditorSettings());
    EXPECT_FALSE(untouched["generateGraph"].IsDefined());
    EXPECT_FALSE(untouched["generateSeed"].IsDefined());

    TerrainEditorSettings set;
    set.generateGraph = "preset:mountains";
    set.generateSeed = 7;
    const TerrainEditorSettings back = Stream::decodeTerrainEditorSettings(Stream::encodeTerrainEditorSettings(set));
    EXPECT_EQ(back.generateGraph, "preset:mountains");
    EXPECT_EQ(back.generateSeed, 7u);
}

// T1 R37: Pressure Strength defaults on and Pressure Size off; an old file has neither key and an
// untouched one writes neither
TEST(StreamTerrainSettings, PressureKeysDefault) {
    const TerrainEditorSettings old = Stream::decodeTerrainEditorSettings(YAML::Load("{brushSize: 8.0}"));
    EXPECT_TRUE(old.pressureStrength);
    EXPECT_FALSE(old.pressureSize);

    const YAML::Node untouched = Stream::encodeTerrainEditorSettings(TerrainEditorSettings());
    EXPECT_FALSE(untouched["pressureStrength"].IsDefined());
    EXPECT_FALSE(untouched["pressureSize"].IsDefined());

    TerrainEditorSettings set;
    set.pressureStrength = false;
    set.pressureSize = true;
    const TerrainEditorSettings back = Stream::decodeTerrainEditorSettings(Stream::encodeTerrainEditorSettings(set));
    EXPECT_FALSE(back.pressureStrength);
    EXPECT_TRUE(back.pressureSize);
}
