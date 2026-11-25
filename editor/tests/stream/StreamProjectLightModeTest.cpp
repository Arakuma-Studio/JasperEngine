// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's graphics.lightMode (S57 R1, Project Settings > Graphics > Light Mode): Automatic
// when a project saved before it loads, written only away from Automatic, round-tripped, applied
// to the editor's rendering at once, and carried into an export's generated main.cpp.

#include "Generator.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "render/LightingTiers.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    // the process-wide mode the tests change, put back
    struct LightModeRestore {
        LightMode saved = LightingTiers::getLightMode();
        ~LightModeRestore() { LightingTiers::setLightMode(saved); }
    };
}

TEST(StreamProjectLightMode, MissingKeyIsAutomatic) {
    LightModeRestore restore;
    Project project;
    project.setLightMode(LightMode::TILED);
    // a project.yaml from before S57
    Stream::decodeProject(&project, YAML::Load("vsync: true\n"), Workspace());
    EXPECT_EQ(project.getLightMode(), LightMode::AUTOMATIC);
    EXPECT_EQ(LightingTiers::getLightMode(), LightMode::AUTOMATIC);
    EXPECT_FALSE(Stream::encodeProject(&project)["graphics"]);
}

TEST(StreamProjectLightMode, RoundTripsAndAppliesAtOnce) {
    LightModeRestore restore;
    Project project;
    project.setLightMode(LightMode::PER_OBJECT);
    EXPECT_EQ(LightingTiers::getLightMode(), LightMode::PER_OBJECT);
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_EQ(encoded["graphics"]["lightMode"].as<std::string>(), "per_object");

    Project loaded;
    loaded.setLightMode(LightMode::TILED);
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_EQ(loaded.getLightMode(), LightMode::PER_OBJECT);
    EXPECT_EQ(LightingTiers::getLightMode(), LightMode::PER_OBJECT);

    // an unknown name loads as Automatic
    Stream::decodeProject(&loaded, YAML::Load("graphics:\n  lightMode: clustered\n"), Workspace());
    EXPECT_EQ(loaded.getLightMode(), LightMode::AUTOMATIC);
}

