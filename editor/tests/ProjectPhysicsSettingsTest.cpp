// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// project.yaml keeps the 3D physics switch (`physics3D`, a bool) and the 3D world capacity
// (`physics3DWorld`, a map) under different keys: a project that turns 3D physics off and changes
// the capacity reads both back (they once shared `physics3D`, one overwriting the other).

#include <gtest/gtest.h>

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"

#include "yaml-cpp/yaml.h"

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ProjectPhysicsSettings, SwitchAndWorldCapacityRoundTripTogether) {
    Project project;
    project.setPhysics3DEnabled(!Project::defaultPhysics3DEnabled);
    Physics3DWorldSettings world;
    world.maxBodies = 1234;
    world.maxBodyPairs = 2345;
    world.maxContactConstraints = 3456;
    project.setPhysics3DWorldSettings(world);

    const YAML::Node node = Stream::encodeProject(&project);
    ASSERT_TRUE(node["physics3D"]);
    EXPECT_TRUE(node["physics3D"].IsScalar());
    ASSERT_TRUE(node["physics3DWorld"]);
    EXPECT_TRUE(node["physics3DWorld"].IsMap());
    const std::string yaml = YAML::Dump(node);

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_EQ(loaded.isPhysics3DEnabled(), !Project::defaultPhysics3DEnabled) << yaml;
    EXPECT_EQ(loaded.getPhysics3DWorldSettings().maxBodies, 1234u) << yaml;
    EXPECT_EQ(loaded.getPhysics3DWorldSettings().maxBodyPairs, 2345u);
    EXPECT_EQ(loaded.getPhysics3DWorldSettings().maxContactConstraints, 3456u);
}

TEST(ProjectPhysicsSettings, DefaultsWriteNeitherKey) {
    Project project;
    const YAML::Node node = Stream::encodeProject(&project);
    EXPECT_FALSE(node["physics3D"]);
    EXPECT_FALSE(node["physics3DWorld"]);
}
