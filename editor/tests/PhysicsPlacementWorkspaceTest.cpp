// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Physics Placement panel's settings in the per-user workspace (.jasperengine/user/
// workspace.yaml): every field survives a save and a load, and a file with missing, unknown or
// unreadable keys still loads, each such key keeping its default.

#include <gtest/gtest.h>

#include "Stream.h"
#include "Workspace.h"
#include "physics/PlacementSettingsLimits.h"

#include "yaml-cpp/yaml.h"

#include "support/PosixCompat.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // Every field away from its default
    PhysicsPlacementSettings changedSettings() {
        PhysicsPlacementSettings s;
        s.feel = PlacementFeel::Precise;
        s.settleOnRelease = false;
        s.keepFormation = false;
        s.pushNeighbours = true;
        s.gravity = Vector3(1.5f, -4.25f, 0.125f);
        s.gravityTowardSurface = true;
        s.colliders = PlacementColliderPolicy::BOTH;
        s.decomposition = PlacementDecomposition::ALWAYS;
        s.quality = DecompositionQuality::HIGH;
        s.alignToSurface = false;
        s.snapOffset = 0.05f;
        s.rotatePivot = PlacementRotatePivot::EachObject;
        s.regionMargin = 3.5f;
        s.ignoreLayerMask = 0x80000005u;
        s.maxMoverSize = 12.0f;
        s.maxSpeed = 6.0f;
        s.contactSpeed = 0.5f;
        s.maxAngularSpeed = 4.0f;
        s.substeps = 3;
        s.velocityIterations = 12;
        s.positionIterations = 5;
        s.teleportDistance = 9.0f;
        s.sleepLinear = 0.03f;
        s.sleepAngularDeg = 3.0f;
        s.sleepSeconds = 0.5f;
        s.maxSettleSeconds = 6.0f;
        s.killYDepth = 80.0f;
        s.showDistanceIndicators = true;
        s.showProxies = true;
        return s;
    }

    void expectSame(const PhysicsPlacementSettings& a, const PhysicsPlacementSettings& b) {
        EXPECT_EQ(a.feel, b.feel);
        EXPECT_EQ(a.settleOnRelease, b.settleOnRelease);
        EXPECT_EQ(a.keepFormation, b.keepFormation);
        EXPECT_EQ(a.pushNeighbours, b.pushNeighbours);
        EXPECT_EQ(a.gravity.x, b.gravity.x);
        EXPECT_EQ(a.gravity.y, b.gravity.y);
        EXPECT_EQ(a.gravity.z, b.gravity.z);
        EXPECT_EQ(a.gravityTowardSurface, b.gravityTowardSurface);
        EXPECT_EQ(a.colliders, b.colliders);
        EXPECT_EQ(a.decomposition, b.decomposition);
        EXPECT_EQ(a.quality, b.quality);
        EXPECT_EQ(a.alignToSurface, b.alignToSurface);
        EXPECT_EQ(a.snapOffset, b.snapOffset);
        EXPECT_EQ(a.rotatePivot, b.rotatePivot);
        EXPECT_EQ(a.regionMargin, b.regionMargin);
        EXPECT_EQ(a.ignoreLayerMask, b.ignoreLayerMask);
        EXPECT_EQ(a.maxMoverSize, b.maxMoverSize);
        EXPECT_EQ(a.maxSpeed, b.maxSpeed);
        EXPECT_EQ(a.contactSpeed, b.contactSpeed);
        EXPECT_EQ(a.maxAngularSpeed, b.maxAngularSpeed);
        EXPECT_EQ(a.substeps, b.substeps);
        EXPECT_EQ(a.velocityIterations, b.velocityIterations);
        EXPECT_EQ(a.positionIterations, b.positionIterations);
        EXPECT_EQ(a.teleportDistance, b.teleportDistance);
        EXPECT_EQ(a.sleepLinear, b.sleepLinear);
        EXPECT_EQ(a.sleepAngularDeg, b.sleepAngularDeg);
        EXPECT_EQ(a.sleepSeconds, b.sleepSeconds);
        EXPECT_EQ(a.maxSettleSeconds, b.maxSettleSeconds);
        EXPECT_EQ(a.killYDepth, b.killYDepth);
        EXPECT_EQ(a.showDistanceIndicators, b.showDistanceIndicators);
        EXPECT_EQ(a.showProxies, b.showProxies);
    }

    // Through text, as the workspace file holds it
    PhysicsPlacementSettings throughText(const PhysicsPlacementSettings& settings) {
        YAML::Emitter out;
        out << Stream::encodePhysicsPlacementSettings(settings);
        return Stream::decodePhysicsPlacementSettings(YAML::Load(out.c_str()));
    }

    fs::path tempProject(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-physics-placement-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(Workspace::getUserDirectory(dir));
        return dir;
    }

}

TEST(PhysicsPlacementWorkspace, EveryEnumValueRoundTrips) {
    for (PlacementFeel feel : {PlacementFeel::Precise, PlacementFeel::Balanced, PlacementFeel::Fast}) {
        PhysicsPlacementSettings s;
        s.feel = feel;
        EXPECT_EQ(throughText(s).feel, feel);
    }
    for (PlacementColliderPolicy policy : {PlacementColliderPolicy::AUTO, PlacementColliderPolicy::AUTHORED,
                                           PlacementColliderPolicy::GENERATED, PlacementColliderPolicy::BOTH}) {
        PhysicsPlacementSettings s;
        s.colliders = policy;
        EXPECT_EQ(throughText(s).colliders, policy);
    }
    for (PlacementDecomposition mode : {PlacementDecomposition::NEVER, PlacementDecomposition::CACHED, PlacementDecomposition::ALWAYS}) {
        PhysicsPlacementSettings s;
        s.decomposition = mode;
        EXPECT_EQ(throughText(s).decomposition, mode);
    }
    for (DecompositionQuality quality : {DecompositionQuality::LOW, DecompositionQuality::MEDIUM, DecompositionQuality::HIGH}) {
        PhysicsPlacementSettings s;
        s.quality = quality;
        EXPECT_EQ(throughText(s).quality, quality);
    }
    for (PlacementRotatePivot pivot : {PlacementRotatePivot::GroupCenter, PlacementRotatePivot::EachObject}) {
        PhysicsPlacementSettings s;
        s.rotatePivot = pivot;
        EXPECT_EQ(throughText(s).rotatePivot, pivot);
    }
}

TEST(PhysicsPlacementWorkspace, MissingKeysKeepDefaults) {
    expectSame(Stream::decodePhysicsPlacementSettings(YAML::Load("{}")), PhysicsPlacementSettings());
    expectSame(Stream::decodePhysicsPlacementSettings(YAML::Node()), PhysicsPlacementSettings());
    expectSame(Stream::decodePhysicsPlacementSettings(YAML::Load("[1, 2]")), PhysicsPlacementSettings());

    PhysicsPlacementSettings expected;
    expected.pushNeighbours = true;
    expected.regionMargin = 4.0f;
    expectSame(Stream::decodePhysicsPlacementSettings(YAML::Load("{pushNeighbours: true, regionMargin: 4}")), expected);
}

TEST(PhysicsPlacementWorkspace, UnknownAndUnreadableKeysKeepDefaults) {
    const YAML::Node node = YAML::Load(
        "feel: sluggish\n"              // unknown name
        "colliders: [auto]\n"           // not a scalar
        "quality: ultra\n"
        "rotatePivot: 7\n"
        "settleOnRelease: maybe\n"      // not a bool
        "substeps: lots\n"              // not an int
        "maxSpeed: .nan\n"              // not finite
        "gravity: [0, 1]\n"             // too short
        "ignoreLayerMask: everything\n"
        "someFutureKey: 12\n"           // a key this editor does not know
        "keepFormation: false\n"        // still read
        "teleportDistance: 7.5\n");
    PhysicsPlacementSettings expected;
    expected.keepFormation = false;
    expected.teleportDistance = 7.5f;
    expectSame(Stream::decodePhysicsPlacementSettings(node), expected);
}

TEST(PhysicsPlacementWorkspace, WorkspaceFileLoadsTheSettings) {
    const fs::path project = tempProject("load");

    YAML::Node root;
    root["selectedScene"] = 1;
    root["physicsPlacement"] = Stream::encodePhysicsPlacementSettings(changedSettings());
    {
        YAML::Emitter out;
        out << root;
        std::ofstream file(Workspace::getUserDirectory(project) / "workspace.yaml");
        file << out.c_str();
    }

    Workspace workspace;
    workspace.load(project);
    expectSame(workspace.getPhysicsPlacementSettings(), changedSettings());

    // A workspace without the section (written before the panel existed) loads the defaults,
    // also after one that had it
    {
        std::ofstream file(Workspace::getUserDirectory(project) / "workspace.yaml");
        file << "selectedScene: 1\n";
    }
    workspace.load(project);
    expectSame(workspace.getPhysicsPlacementSettings(), PhysicsPlacementSettings());

    fs::remove_all(project);
}

TEST(PhysicsPlacementWorkspace, HandEditedValuesAreClampedToThePanelRanges) {
    const YAML::Node node = YAML::Load(
        "substeps: 1000\n"
        "velocityIterations: 0\n"
        "positionIterations: -3\n"           // negative: from the Feel preset
        "maxSettleSeconds: 1000000\n"
        "maxMoverSize: 0\n"
        "regionMargin: -5\n"
        "teleportDistance: 0\n"
        "maxSpeed: 5000\n"
        "contactSpeed: -7\n"
        "maxAngularSpeed: 1000\n"            // rad/s
        "sleepLinear: 3\n"
        "sleepAngularDeg: -1\n"
        "sleepSeconds: 60\n"
        "killYDepth: -10\n"
        "snapOffset: 9\n"
        "gravity: [0, -500, 1000]\n");
    const PhysicsPlacementSettings s = Stream::decodePhysicsPlacementSettings(node);
    EXPECT_EQ(s.substeps, 16);
    EXPECT_EQ(s.velocityIterations, 1);
    EXPECT_EQ(s.positionIterations, -1);
    EXPECT_EQ(s.maxSettleSeconds, 60.0f);
    EXPECT_EQ(s.maxMoverSize, 0.1f);
    EXPECT_EQ(s.regionMargin, 0.0f);
    EXPECT_EQ(s.teleportDistance, 0.1f);
    EXPECT_EQ(s.maxSpeed, 100.0f);
    EXPECT_EQ(s.contactSpeed, -1.0f);
    EXPECT_NEAR(s.maxAngularSpeed, 5000.0f * 3.14159265f / 180.0f, 1e-3f);
    EXPECT_EQ(s.sleepLinear, 1.0f);
    EXPECT_EQ(s.sleepAngularDeg, 0.0f);
    EXPECT_EQ(s.sleepSeconds, 5.0f);
    EXPECT_EQ(s.killYDepth, 0.0f);
    EXPECT_EQ(s.snapOffset, 1.0f);
    EXPECT_EQ(s.gravity.x, 0.0f);
    EXPECT_EQ(s.gravity.y, -100.0f);
    EXPECT_EQ(s.gravity.z, 100.0f);

    // every default is inside its range, so clamping changes nothing
    PhysicsPlacementSettings defaults;
    clampPhysicsPlacementSettings(defaults);
    expectSame(defaults, PhysicsPlacementSettings());
}
