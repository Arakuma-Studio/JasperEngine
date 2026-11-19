// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Build Profiles (docs/specs/build-profiles.md): the .buildprofile codec (AC1), the settings snapshot
// of a build (AC2, AC3, AC10), validation (AC4) and the project's list of profiles (AC13).

#include <gtest/gtest.h>

#include "Project.h"
#include "Stream.h"
#include "buildprofile/BuildProfile.h"
#include "render/shadow/ShadowTiers.h"
#include "settings/SettingsRegistry.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    BuildProfile everyField() {
        BuildProfile profile;
        profile.name = "Demo";
        profile.platform = BuildPlatform::Android;
        profile.productName = "My Game: Demo";
        profile.identifier = "com.studio.mygame.demo";
        profile.versionSuffix = "-demo";
        profile.icon = "art/icon-demo.png";
        profile.store = "google-play";
        profile.storeTitle = "My Game: Demo";
        profile.backend = "gles3";
        profile.development = false;
        profile.outputDir = "../builds/android-demo";
        profile.scenes = std::vector<BuildProfileScene>{{"6c1f00000000000000000000000000aa", "scenes/Menu.scene"},
                                                         {"0a9e00000000000000000000000000bb", "scenes/Level1.scene"}};
        profile.startScene = "0a9e00000000000000000000000000bb";
        profile.defines = {"DEMO", "NO_ADS"};
        profile.excludeLabels = {"full-only"};
        profile.excludePaths = {"music/full/", "levels/bonus/"};
        profile.overrides["player.window_title"] = "My Game Demo";
        profile.overrides["android.package_format"] = "apk";
        profile.overrides["player.window_width"] = 1280;
        return profile;
    }

    std::string encoded(Project& project) {
        return YAML::Dump(Stream::encodeProject(&project));
    }

}

// ---------------------------------------------------------------- AC1: the codec

TEST(BuildProfileFile, RoundTripsEveryField) {
    const BuildProfile profile = everyField();
    const std::string text = BuildProfileFile::encode(profile);
    BuildProfile read;
    read.name = "Demo";
    std::string error;
    ASSERT_TRUE(BuildProfileFile::parse(text, read, error)) << error << "\n" << text;
    EXPECT_EQ(read.platform, BuildPlatform::Android);
    EXPECT_EQ(read.productName, profile.productName);
    EXPECT_EQ(read.identifier, profile.identifier);
    EXPECT_EQ(read.versionSuffix, profile.versionSuffix);
    EXPECT_EQ(read.icon, profile.icon);
    EXPECT_EQ(read.store, profile.store);
    EXPECT_EQ(read.storeTitle, profile.storeTitle);
    EXPECT_EQ(read.backend, profile.backend);
    ASSERT_TRUE(read.development.has_value());
    EXPECT_FALSE(*read.development);
    EXPECT_EQ(read.outputDir, profile.outputDir);
    ASSERT_TRUE(read.scenes.has_value());
    EXPECT_EQ(*read.scenes, *profile.scenes);
    EXPECT_EQ(read.startScene, profile.startScene);
    EXPECT_EQ(read.defines, profile.defines);
    EXPECT_EQ(read.excludeLabels, profile.excludeLabels);
    EXPECT_EQ(read.excludePaths, profile.excludePaths);
    EXPECT_EQ(read.overrides, profile.overrides);
    // the same bytes again
    EXPECT_EQ(BuildProfileFile::encode(read), text);

    // an absent scene list is the project's Scenes In Build, an empty one the profile's (refused by validation)
    BuildProfile none;
    none.platform = BuildPlatform::Linux;
    BuildProfile back;
    ASSERT_TRUE(BuildProfileFile::parse(BuildProfileFile::encode(none), back, error)) << error;
    EXPECT_FALSE(back.scenes.has_value());
    EXPECT_FALSE(back.development.has_value());
    EXPECT_EQ(BuildProfileFile::encode(none), "formatVersion: 1\nplatform: linux\n");
}

TEST(BuildProfileFile, KeepsUnknownKeys) {
    const std::string text = "formatVersion: 1\nplatform: web\nproductName: \"Box\"\nfutureKey:\n  nested: [1, 2]\nlater: text\n";
    BuildProfile profile;
    std::string error;
    ASSERT_TRUE(BuildProfileFile::parse(text, profile, error)) << error;
    profile.productName = "Box Demo";
    const std::string saved = BuildProfileFile::encode(profile);
    const YAML::Node node = YAML::Load(saved);
    EXPECT_EQ(node["productName"].as<std::string>(), "Box Demo");
    EXPECT_EQ(node["later"].as<std::string>(), "text");
    ASSERT_TRUE(node["futureKey"]["nested"].IsSequence());
    EXPECT_EQ(node["futureKey"]["nested"][1].as<int>(), 2);
}

TEST(BuildProfileFile, RefusesANewerFormatAsReadOnly) {
    jasperengine::test::TestTempDir dir("build-profile-newer");
    fs::create_directories(BuildProfileFile::directory(dir.path()));
    std::ofstream(BuildProfileFile::pathOf(dir.path(), "Future")) << "formatVersion: 2\nplatform: linux\nproductName: x\n";
    std::ofstream(BuildProfileFile::pathOf(dir.path(), "Broken")) << "formatVersion: 1\nplatform: [\n";
    std::ofstream(BuildProfileFile::pathOf(dir.path(), "Fine")) << "formatVersion: 1\nplatform: linux\n";

    const std::vector<BuildProfileEntry> entries = buildprofiles::list(dir.path());
    ASSERT_EQ(entries.size(), 3u);
    const BuildProfileEntry* future = buildprofiles::find(entries, "future");
    ASSERT_NE(future, nullptr);
    EXPECT_FALSE(future->readable);
    EXPECT_TRUE(future->newer);
    EXPECT_NE(future->error.find("made by a newer editor: update to edit or build it"), std::string::npos) << future->error;
    const BuildProfileEntry* broken = buildprofiles::find(entries, "Broken");
    ASSERT_NE(broken, nullptr);
    EXPECT_FALSE(broken->readable);
    EXPECT_FALSE(broken->newer);
    EXPECT_FALSE(broken->error.empty());
    EXPECT_TRUE(buildprofiles::find(entries, "Fine")->readable);

    BuildProfile loaded;
    std::string error;
    EXPECT_FALSE(buildprofiles::load(dir.path(), "Future", loaded, error));
    EXPECT_NE(error.find("newer editor"), std::string::npos) << error;
}

TEST(BuildProfileFile, NameRules) {
    std::string why;
    EXPECT_TRUE(BuildProfileFile::isValidName("Demo"));
    EXPECT_TRUE(BuildProfileFile::isValidName("Steam Demo_2.0-beta"));
    EXPECT_TRUE(BuildProfileFile::isValidName(std::string(64, 'a')));
    EXPECT_FALSE(BuildProfileFile::isValidName(std::string(65, 'a'), &why));
    EXPECT_NE(why.find("1 to 64"), std::string::npos);
    EXPECT_FALSE(BuildProfileFile::isValidName(""));
    EXPECT_FALSE(BuildProfileFile::isValidName("a/b", &why));
    EXPECT_FALSE(BuildProfileFile::isValidName("Démo"));
    EXPECT_FALSE(BuildProfileFile::isValidName(".hidden"));
    EXPECT_FALSE(BuildProfileFile::isValidName("trailing "));
    // unique without case
    jasperengine::test::TestTempDir dir("build-profile-names");
    fs::create_directories(BuildProfileFile::directory(dir.path()));
    std::ofstream(BuildProfileFile::pathOf(dir.path(), "Demo")) << "formatVersion: 1\nplatform: linux\n";
    const std::vector<BuildProfileEntry> entries = buildprofiles::list(dir.path());
    EXPECT_NE(buildprofiles::find(entries, "DEMO"), nullptr);
}

// ---------------------------------------------------------------- AC2, AC3, AC10: resolution

TEST(BuildProfileResolve, WithoutAProfileTheSnapshotIsTheProject) {
    Project project;
    std::vector<BuildWarning> warnings;
    std::string error;
    const std::shared_ptr<Project> snapshot = resolveBuildSettings(project, nullptr, warnings, error);
    EXPECT_EQ(snapshot.get(), &project);
    EXPECT_TRUE(warnings.empty());
    EXPECT_TRUE(error.empty());
}

TEST(BuildProfileResolve, TypedFieldsWriteThePlatformKeys) {
    struct Row {
        BuildPlatform platform;
        std::string name;        // the settings field Product Name lands in
        std::string identifier;
        std::string icon;
    };
    Project project;
    project.setName("Box");
    auto nameOf = [](const Project& p, BuildPlatform platform) -> std::string {
        switch (platform) {
            case BuildPlatform::Android: return p.getAndroidProjectSettings().applicationName;
            case BuildPlatform::IOS: return p.getIOSProjectSettings().applicationName;
            case BuildPlatform::MacOS: return p.getMacOSProjectSettings().applicationName;
            case BuildPlatform::Windows: return p.getWindowsProjectSettings().productName;
            case BuildPlatform::Linux: return p.getLinuxProjectSettings().applicationName;
            case BuildPlatform::Web: return p.getWebProjectSettings().applicationName;
        }
        return {};
    };
    auto identifierOf = [](const Project& p, BuildPlatform platform) -> std::string {
        switch (platform) {
            case BuildPlatform::Android: return p.getAndroidProjectSettings().packageName;
            case BuildPlatform::IOS: return p.getIOSProjectSettings().bundleIdentifier;
            case BuildPlatform::MacOS: return p.getMacOSProjectSettings().bundleIdentifier;
            default: return p.getApplicationSettings().identifier;
        }
    };
    auto iconOf = [](const Project& p, BuildPlatform platform) -> std::string {
        switch (platform) {
            case BuildPlatform::Android: return p.getAndroidProjectSettings().launcherIcon.generic_string();
            case BuildPlatform::IOS: return p.getIOSProjectSettings().icon.generic_string();
            case BuildPlatform::MacOS: return p.getMacOSProjectSettings().icon.generic_string();
            case BuildPlatform::Web: return p.getWebProjectSettings().favicon.generic_string();
            default: return p.getWindowIcon().generic_string();
        }
    };
    for (BuildPlatform platform : {BuildPlatform::Android, BuildPlatform::IOS, BuildPlatform::MacOS, BuildPlatform::Windows,
                                   BuildPlatform::Linux, BuildPlatform::Web}) {
        SCOPED_TRACE(buildPlatformName(platform));
        BuildProfile profile;
        profile.name = "Demo";
        profile.platform = platform;
        profile.productName = "Box Demo";
        profile.identifier = "com.studio.boxdemo";
        profile.icon = "art/icon.png";
        profile.versionSuffix = "-demo";
        std::vector<BuildWarning> warnings;
        std::string error;
        const std::shared_ptr<Project> snapshot = resolveBuildSettings(project, &profile, warnings, error);
        ASSERT_TRUE(snapshot) << error;
        ASSERT_NE(snapshot.get(), &project);
        EXPECT_EQ(nameOf(*snapshot, platform), "Box Demo");
        EXPECT_EQ(identifierOf(*snapshot, platform), "com.studio.boxdemo");
        EXPECT_EQ(iconOf(*snapshot, platform), "art/icon.png");
        EXPECT_EQ(snapshot->getWindowTitle(), "Box Demo") << "the project has no window title of its own";
        EXPECT_EQ(snapshot->getAndroidProjectSettings().versionName, project.getApplicationVersion() + "-demo");
        // the open project keeps its own values
        EXPECT_EQ(nameOf(project, platform), "");
        EXPECT_EQ(project.getWindowTitle(), "");
    }
    // a project with its own window title keeps it
    project.setWindowTitle("Box: The Game");
    BuildProfile profile;
    profile.name = "Demo";
    profile.productName = "Box Demo";
    std::vector<BuildWarning> warnings;
    std::string error;
    const std::shared_ptr<Project> titled = resolveBuildSettings(project, &profile, warnings, error);
    ASSERT_TRUE(titled) << error;
    EXPECT_EQ(titled->getWindowTitle(), "Box: The Game");
    // the adaptive pair goes with an Android Icon (it would win over the launcher icon)
    project.getAndroidProjectSettings().adaptiveIconForeground = "art/fg.png";
    profile.platform = BuildPlatform::Android;
    profile.icon = "art/icon.png";
    const std::shared_ptr<Project> android = resolveBuildSettings(project, &profile, warnings, error);
    ASSERT_TRUE(android) << error;
    EXPECT_TRUE(android->getAndroidProjectSettings().adaptiveIconForeground.empty());
    EXPECT_EQ(project.getAndroidProjectSettings().adaptiveIconForeground.generic_string(), "art/fg.png");
}

TEST(BuildProfileResolve, SuffixSkipsNumericVersions) {
    Project project;
    project.getApplicationSettings().version = "1.2.0";
    for (BuildPlatform platform : {BuildPlatform::MacOS, BuildPlatform::IOS, BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::Android,
                                   BuildPlatform::Web}) {
        SCOPED_TRACE(buildPlatformName(platform));
        BuildProfile profile;
        profile.name = "Demo";
        profile.platform = platform;
        profile.versionSuffix = "-demo";
        std::vector<BuildWarning> warnings;
        std::string error;
        const std::shared_ptr<Project> snapshot = resolveBuildSettings(project, &profile, warnings, error);
        ASSERT_TRUE(snapshot) << error;
        // the text version (game.json, the Android package)
        EXPECT_EQ(snapshot->getAndroidProjectSettings().versionName, "1.2.0-demo");
        // never the numeric ones
        EXPECT_EQ(snapshot->getMacOSProjectSettings().versionName, "");
        EXPECT_EQ(snapshot->getIOSProjectSettings().versionName, "");
        EXPECT_EQ(snapshot->getWindowsProjectSettings().fileVersion, "");
        EXPECT_EQ(snapshot->getApplicationSettings().version, "1.2.0");
        const bool numeric = platform == BuildPlatform::MacOS || platform == BuildPlatform::IOS || platform == BuildPlatform::Windows;
        size_t said = 0;
        for (const BuildWarning& warning : warnings) said += warning.code == "W-PROFILE-SUFFIX-NUMERIC";
        EXPECT_EQ(said, numeric ? 1u : 0u);
    }
}

TEST(BuildProfileResolve, TheOpenProjectNeverChanges) {
    Project project;
    project.setName("Box");
    project.getApplicationSettings().version = "2.0";
    // a project.yaml of 2,000 lines (the label colours are one line each)
    std::vector<LabelColor> labels;
    for (int i = 0; i < 2000; i++) labels.push_back({"label" + std::to_string(i), "#336699"});
    project.getAssetLabels().setEntries(labels);
    const std::string before = encoded(project);
    ASSERT_GE(static_cast<size_t>(std::count(before.begin(), before.end(), '\n')), 2000u);
    const ShadowTier tierBefore = ShadowTiers::getPlatformTier(0);
    const ShadowTier other = tierBefore == ShadowTier::LOW ? ShadowTier::HIGH : ShadowTier::LOW;

    BuildProfile profile;
    profile.name = "Demo";
    profile.platform = BuildPlatform::Android;
    profile.productName = "Box Demo";
    profile.identifier = "com.studio.box.demo";
    profile.versionSuffix = "-demo";
    profile.icon = "art/icon.png";
    profile.development = true;
    profile.defines = {"DEMO"};
    profile.overrides["player.window_width"] = 640;
    profile.overrides["player.application_name"] = "Shared Demo";
    profile.overrides["graphics.canvas_width"] = 320;
    profile.overrides[std::string("graphics.shadow_tier.") + Project::getShadowTierPlatformName(0)] = other == ShadowTier::LOW ? "low" : "high";
    profile.overrides["graphics.dynamic_batching"] = false;
    profile.overrides["quality.vsync"] = false;
    profile.overrides["android.package_format"] = "apk";

    std::vector<BuildWarning> warnings;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const std::shared_ptr<Project> snapshot = resolveBuildSettings(project, &profile, warnings, error);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(snapshot) << error;
    // every override landed in the snapshot
    EXPECT_EQ(snapshot->getWindowSettings().width, 640u);
    EXPECT_EQ(snapshot->getApplicationSettings().name, "Shared Demo");
    EXPECT_EQ(snapshot->getCanvasWidth(), 320u);
    EXPECT_EQ(snapshot->getShadowTier(0), other);
    EXPECT_FALSE(snapshot->isDynamicBatching());
    EXPECT_FALSE(snapshot->isVSyncEnabled());
    EXPECT_EQ(snapshot->getAndroidProjectSettings().packageFormat, "apk");
    EXPECT_TRUE(snapshot->isDevelopmentBuild());
    EXPECT_EQ(snapshot->getAssetLabels().entries().size(), 2000u);
    // and nothing of the open project, its file or the engine moved
    EXPECT_EQ(encoded(project), before);
    EXPECT_EQ(ShadowTiers::getPlatformTier(0), tierBefore);
    EXPECT_FALSE(project.isDevelopmentBuild());

    std::printf("[ bench ] settings snapshot of a %zu-line project.yaml: %.1f ms (budget 50)\n",
                static_cast<size_t>(std::count(before.begin(), before.end(), '\n')), ms);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(ms, 50.0 * slack);
    }
}

// ---------------------------------------------------------------- AC4: validation

TEST(BuildProfileValidate, RefusesATypedFieldAndItsOverride) {
    Project project;
    BuildProfile profile;
    profile.name = "Demo";
    profile.platform = BuildPlatform::Android;
    profile.productName = "Box Demo";
    profile.overrides["android.application_name"] = "Box";
    const std::vector<std::string> findings = validateProfile(project, profile);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings.front(), "Product Name and the override android.application_name both set the Android label: keep one");
}

TEST(BuildProfileValidate, RefusesKeysOfOtherCategories) {
    Project project;
    BuildProfile profile;
    profile.name = "Demo";
    profile.platform = BuildPlatform::Android;
    profile.overrides["physics.gravity"] = mcp::Json::array({0, -9.8, 0});
    profile.overrides["ios.icon"] = "art/icon.png";
    const std::vector<std::string> findings = validateProfile(project, profile);
    ASSERT_EQ(findings.size(), 2u);
    std::string all;
    for (const std::string& finding : findings) all += finding + "\n";
    EXPECT_NE(all.find("ios.icon is an iOS key; this profile builds for Android: use android.* or player.* keys"), std::string::npos) << all;
    EXPECT_NE(all.find("physics.gravity is a physics setting, which a Build Profile cannot change: overrides take player, graphics, quality, android keys"),
              std::string::npos) << all;
}

TEST(BuildProfileValidate, UsesTheSettingsOwnValidator) {
    Project project;
    BuildProfile profile;
    profile.name = "Demo";
    profile.platform = BuildPlatform::Linux;
    profile.overrides["player.window_width"] = "abc";
    // the setter's own message for the same value
    std::string expected;
    SettingsContext ctx;
    ctx.projectPtr = &project;
    ASSERT_FALSE(SettingsRegistry::get().find("player.window_width")->set(ctx, mcp::Json("abc"), false, expected));
    ASSERT_FALSE(expected.empty());
    const std::vector<std::string> findings = validateProfile(project, profile);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings.front(), "override player.window_width: " + expected);
    // and the identifiers' validators for the typed fields (R3)
    profile.overrides.clear();
    profile.platform = BuildPlatform::Android;
    profile.identifier = "not an id";
    const std::vector<std::string> idFindings = validateProfile(project, profile);
    ASSERT_EQ(idFindings.size(), 1u);
    EXPECT_EQ(idFindings.front().rfind("Identifier (android.package_name): ", 0), 0u) << idFindings.front();
    // defines, labels and paths
    profile.identifier.clear();
    profile.defines = {"DEMO", "1BAD", "DEMO"};
    profile.excludePaths = {"../outside/", "EditorSettings/", "music/full/"};
    const std::vector<std::string> more = validateProfile(project, profile);
    EXPECT_EQ(more.size(), 4u);
}

// ---------------------------------------------------------------- AC13: old projects

TEST(BuildProfileStore, AProjectWithoutTheFolderHasNoProfiles) {
    jasperengine::test::TestTempDir dir("build-profile-none");
    EXPECT_TRUE(buildprofiles::list(dir.path()).empty());
    EXPECT_EQ(buildprofiles::activeName(dir.path()), "");
    EXPECT_EQ(buildprofiles::active(dir.path()), nullptr);
}

TEST(BuildProfileStore, AnUnreadableProfileDoesNotStopTheProject) {
    jasperengine::test::TestTempDir dir("build-profile-unreadable");
    std::ofstream(dir.path() / "project.yaml") << "name: Box\n";
    fs::create_directories(BuildProfileFile::directory(dir.path()));
    std::ofstream(BuildProfileFile::pathOf(dir.path(), "Broken")) << "formatVersion: 1\nplatform: dreamcast\n";
    std::string error;
    ASSERT_TRUE(buildprofiles::setActiveName(dir.path(), "Broken", error)) << error;

    Project project;
    project.setSkipProjectLock(true);
    ASSERT_TRUE(project.loadProject(dir.path(), false));
    const std::vector<BuildProfileEntry> entries = buildprofiles::list(dir.path());
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_FALSE(entries.front().readable);
    EXPECT_NE(entries.front().error.find("dreamcast"), std::string::npos) << entries.front().error;
    // the active one falls back to none, and a platform still builds without a profile
    EXPECT_EQ(buildprofiles::active(dir.path()), nullptr);
    std::vector<BuildWarning> warnings;
    EXPECT_EQ(resolveBuildSettings(project, nullptr, warnings, error).get(), &project);
    editor::test::closeProject(project);
}
