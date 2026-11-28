// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Build Profiles (docs/specs/build-profiles.md): the .buildprofile codec (AC1), the settings snapshot
// of a build (AC2, AC3, AC10), validation (AC4) and the project's list of profiles (AC13).

#include <gtest/gtest.h>

#include "Project.h"
#include "Stream.h"
#include "buildprofile/BuildProfile.h"
#include "command/type/EditBuildProfileCmd.h"
#include "cook/GameConfig.h"
#include "render/LightingTiers.h"
#include "save/SaveGameStore.h"
#include "render/shadow/ShadowTiers.h"
#include "settings/SettingsRegistry.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace BuildProfileTest_cpp {

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

    // a project on disk with the icon the profiles name (path settings check that the file exists)
    void openProjectWithIcon(const jasperengine::test::TestTempDir& dir, Project& project) {
        std::ofstream(dir.path() / "project.yaml") << "name: Box\n";
        fs::create_directories(dir.path() / "art");
        std::ofstream(dir.path() / "art" / "icon.png", std::ios::binary) << "\x89PNG\r\n\x1a\n";
        project.setSkipProjectLock(true);
        ASSERT_TRUE(project.loadProject(dir.path(), false));
    }

    std::string encoded(Project& project) {
        return YAML::Dump(Stream::encodeProject(&project));
    }

    // every file under a folder and its bytes: what a build must leave as it found it
    std::map<std::string, std::string> folderContents(const fs::path& root) {
        std::map<std::string, std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            // the editor's lock is the open project's own, not a build's write
            if (entry.path().filename() == "editor.lock") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            files[fs::relative(entry.path(), root).generic_string()] =
                std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        return files;
    }

    std::string changedFiles(const std::map<std::string, std::string>& before, const std::map<std::string, std::string>& after) {
        std::string changed;
        for (const auto& [path, bytes] : after) {
            const auto old = before.find(path);
            if (old == before.end() || old->second != bytes) changed += (changed.empty() ? "" : ", ") + path;
        }
        for (const auto& [path, bytes] : before)
            if (!after.count(path)) changed += (changed.empty() ? "" : ", ") + path + " (removed)";
        return changed;
    }

    // values of a settings row's type that differ from `current`, for an override to try in turn
    std::vector<mcp::Json> otherValues(const SettingRow& row, const mcp::Json& current) {
        std::vector<mcp::Json> values;
        auto changedNumber = [](const mcp::Json& value, bool integer, std::vector<mcp::Json>& out) {
            const double base = value.is_number() ? value.get<double>() : 0.0;
            // up and down: a tier's caps only lower
            for (double candidate : {base + 1.0, base - 1.0, base * 2.0, base / 2.0, base + 0.5, base * 0.5, 1.0, 2.0, 0.0, 16.0, 64.0, 100.0,
                                     256.0, 1024.0, 0.5, 0.25}) {
                if (integer && candidate != static_cast<double>(static_cast<long long>(candidate))) continue;
                const mcp::Json json = integer ? mcp::Json(static_cast<long long>(candidate)) : mcp::Json(candidate);
                if (json != value) out.push_back(json);
            }
        };
        if (row.type == "bool") {
            values.push_back(current.is_boolean() ? mcp::Json(!current.get<bool>()) : mcp::Json(true));
        } else if (row.type == "enum") {
            for (const std::string& value : row.values)
                if (current != mcp::Json(value)) values.push_back(value);
        } else if (row.type == "int" || row.type == "number" || row.type == "float") {
            changedNumber(current, row.type == "int", values);
        } else if (row.type == "string" || row.type == "path") {
            for (const char* value : {"art/icon.png", "Profile Value", "1.2.3", "com.studio.profile", "#ff0000", ".profile", ""})
                if (current != mcp::Json(value)) values.push_back(value);
            if (current.is_string()) values.push_back(current.get<std::string>() + "1");
        } else if (row.type == "list") {
            if (current.is_array() && !current.empty()) {
                values.push_back(mcp::Json::array({current.front()}));
                values.push_back(mcp::Json::array());
                // a vector: its components turned (the Transparency Sort axis)
                mcp::Json turned = mcp::Json::array();
                for (size_t i = 0; i < current.size(); i++) turned.push_back(current[(i + 1) % current.size()]);
                if (turned != current) values.push_back(turned);
            }
            // one entry in each list format the settings take: names, URLs, lines, records
            for (const mcp::Json& entry : {mcp::Json("GameKit"), mcp::Json("vibrate"), mcp::Json("com.example:lib:1.0"),
                                           mcp::Json("https://repo.example.com/maven"), mcp::Json("org.gradle.jvmargs=-Xmx2g"),
                                           mcp::Json("android.permission.VIBRATE"), mcp::Json("https://cdn.example.com/sdk.js"),
                                           mcp::Json{{"name", "Firebase/Analytics"}},
                                           mcp::Json{{"url", "https://github.com/example/pkg"}, {"version", "1.0.0"}, {"products", {"Pkg"}}},
                                           mcp::Json{{"platform", "all"}, {"category", "total"}, {"limit_mb", 100}},
                                           mcp::Json{{"name", "_ProfileTint"}, {"type", "float"}, {"default", 1.0}}})
                values.push_back(mcp::Json::array({entry}));
        } else if (row.type == "object" && current.is_object()) {
            // each member changed in turn, through the member's own type
            for (const auto& [member, value] : current.items()) {
                std::vector<mcp::Json> memberValues;
                if (value.is_boolean()) memberValues.push_back(!value.get<bool>());
                else if (value.is_number()) changedNumber(value, value.is_number_integer(), memberValues);
                // an inherited cell (a Device Tier's null): a value of its own
                else if (value.is_null()) memberValues = {mcp::Json(0.5), mcp::Json(false), mcp::Json(1), mcp::Json("low")};
                for (const mcp::Json& changed : memberValues) {
                    mcp::Json object = current;
                    object[member] = changed;
                    values.push_back(object);
                }
            }
            if (current.empty()) values.push_back(mcp::Json{{"NSCameraUsageDescription", "The camera takes photos"}});
        }
        return values;
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
    jasperengine::test::TestTempDir dir("build-profile-typed");
    Project project;
    openProjectWithIcon(dir, project);
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
    editor::test::closeProject(project);
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
    jasperengine::test::TestTempDir dir("build-profile-isolation");
    Project project;
    openProjectWithIcon(dir, project);
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
    // where the time goes: the encode, a whole snapshot (encode, a Project made, decode), one of a
    // project with nothing in it (the Project made), and the validation
    auto timed = [](auto&& work) {
        const auto t0 = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    Project empty;
    std::printf("[ bench ] of which: encodeProject %.1f ms, makeSettingsSnapshot %.1f ms (of an empty project %.1f ms), "
                "validateProfile %.1f ms\n",
                timed([&] { (void)Stream::encodeProject(&project); }), timed([&] { (void)Project::makeSettingsSnapshot(project); }),
                timed([&] { (void)Project::makeSettingsSnapshot(empty); }), timed([&] { (void)validateProfile(project, profile); }));
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(ms, 50.0 * slack);
    }
    editor::test::closeProject(project);
}

// Every key a Build Profile may override, one profile each: the build applies it to a snapshot, and
// neither the open project's files, its undo history nor its settings move (a settings command's
// execute() saved project.yaml through the snapshot, which keeps the project's path)
TEST(BuildProfileResolve, EveryOverridableKeyLeavesTheProjectFilesAlone) {
    jasperengine::test::TestTempDir dir("build-profile-every-key");
    Project project;
    openProjectWithIcon(dir, project);
    ASSERT_TRUE(project.saveProjectFile());
    const std::map<std::string, std::string> filesBefore = folderContents(dir.path());
    const std::string before = encoded(project);
    const size_t historyBefore = project.getProjectCommandHistory()->position();
    // what the editor's own engine runs with (Saved Data: the store the game view's Play writes)
    const savegame::SaveSettings saveSettingsBefore = savegame::settings();
    static const std::set<std::string> owned = {"player.product_name", "player.start_scene", "player.target_platforms",
                                                "player.development_build"};

    size_t overridable = 0, applied = 0;
    std::vector<std::string> notChanged;
    for (const SettingRow& row : SettingsRegistry::get().rows()) {
        if (!row.mcp || row.scope != SettingsScope::Project || !row.set || owned.count(row.key)) continue;
        const std::string category = row.key.substr(0, row.key.find('.'));
        BuildPlatform platform = BuildPlatform::Linux;
        if (category != "player" && category != "graphics" && category != "quality" && !parseBuildPlatform(category, platform)) continue;
        overridable++;
        SettingsContext projectCtx;
        projectCtx.projectPtr = &project;
        const mcp::Json current = row.get ? row.get(projectCtx) : mcp::Json();
        bool changed = false;
        for (const mcp::Json& value : otherValues(row, current)) {
            BuildProfile profile;
            profile.name = "Every";
            profile.platform = platform;
            profile.overrides[row.key] = value;
            std::vector<BuildWarning> warnings;
            std::string error;
            const std::shared_ptr<Project> snapshot = resolveBuildSettings(project, &profile, warnings, error);
            // the files are checked after every attempt, a refused one too
            const std::string moved = changedFiles(filesBefore, folderContents(dir.path()));
            EXPECT_TRUE(moved.empty()) << row.key << " = " << value.dump() << " wrote " << moved;
            if (!snapshot) continue;
            SettingsContext snapshotCtx;
            snapshotCtx.projectPtr = snapshot.get();
            if (row.get && row.get(snapshotCtx) == current) continue;
            changed = true;
            applied++;
            break;
        }
        if (!changed) notChanged.push_back(row.key);
        EXPECT_EQ(encoded(project), before) << row.key << " changed the open project";
        EXPECT_EQ(project.getProjectCommandHistory()->position(), historyBefore) << row.key << " added an undo step to the open project";
        EXPECT_TRUE(savegame::settings() == saveSettingsBefore) << row.key << " changed the editor engine's Saved Data settings";
    }
    std::string skipped;
    for (const std::string& key : notChanged) skipped += (skipped.empty() ? "" : ", ") + key;
    std::printf("[ audit ] %zu overridable keys, %zu applied with a changed value; not changed by any candidate: %s\n", overridable,
                applied, skipped.empty() ? "none" : skipped.c_str());
    // the keys this fixture cannot give a changed value, each for its reason; any other key must
    // change in the snapshot, so a new key is audited too (or named here)
    static const std::map<std::string, const char*> unchangeable = {
        {"player.loading_scene", "names a saved scene; the fixture has none"},
        {"graphics.shipped_terrain_tiers", "follows from the terrain settings; it refuses a value"},
        {"graphics.screen_transitions.ramp", "names a .palette ramp; the fixture has none"},
    };
    EXPECT_GE(overridable, 150u);
    for (const std::string& key : notChanged)
        EXPECT_TRUE(unchangeable.count(key)) << key << ": no candidate changed it in the snapshot (the override was refused or ignored)";
    editor::test::closeProject(project);
}

// A rename whose new name is the old one in other letter case ("demo" -> "Demo") names one file twice
// on a case-insensitive filesystem (Windows, macOS). Here two spellings of one path stand in for them:
// undo must put the file back, not write it and then remove it under its other name
TEST(BuildProfileFile, UndoOfARenameToTheSameFileKeepsIt) {
    jasperengine::test::TestTempDir dir("build-profile-case-rename");
    Project project;
    std::ofstream(dir.path() / "project.yaml") << "name: Box\n";
    project.setSkipProjectLock(true);
    ASSERT_TRUE(project.loadProject(dir.path(), false));
    const fs::path folder = BuildProfileFile::directory(dir.path());
    fs::create_directories(folder / "alias");
    const fs::path oldName = folder / "demo.buildprofile";
    const fs::path newName = folder / "alias" / ".." / "demo.buildprofile";   // the same file, spelled otherwise
    const std::string text = "formatVersion: 1\nplatform: linux\n";
    std::ofstream(oldName, std::ios::binary) << text;

    // what the rename writes: the old name removed, the new one created
    std::vector<EditBuildProfileCmd::FileChange> changes = {{oldName, text, std::nullopt}, {newName, std::nullopt, text}};
    auto* command = new EditBuildProfileCmd("Rename Build Profile 'demo'", std::move(changes));
    command->setNoMerge();
    ASSERT_TRUE(project.getProjectCommandHistory()->addCommand(command));
    ASSERT_TRUE(fs::is_regular_file(newName));
    EXPECT_EQ(editor::test::readText(newName), text);

    ASSERT_TRUE(project.getProjectCommandHistory()->undo());
    ASSERT_TRUE(fs::is_regular_file(oldName)) << "undo removed the profile it put back";
    EXPECT_EQ(editor::test::readText(oldName), text);
    ASSERT_TRUE(project.getProjectCommandHistory()->redo());
    ASSERT_TRUE(fs::is_regular_file(newName)) << "redo removed the profile it wrote";
    editor::test::closeProject(project);
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
    profile.overrides["physics.max_bodies"] = 1000;
    profile.overrides["ios.icon"] = "art/icon.png";
    const std::vector<std::string> findings = validateProfile(project, profile);
    ASSERT_EQ(findings.size(), 2u);
    std::string all;
    for (const std::string& finding : findings) all += finding + "\n";
    EXPECT_NE(all.find("ios.icon is an iOS key; this profile builds for Android: use android.* or player.* keys"), std::string::npos) << all;
    EXPECT_NE(all.find("physics.max_bodies is a physics setting, which a Build Profile cannot change: overrides take player, graphics, quality, android keys"),
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

// ---------------------------------------------------------------- Compile builds

// A Compile build's game.json of a profile (every build reads it, player::runGame): its Scene List
// (and the scenes they involve), its start scene, and its settings snapshot, not the open project's
TEST(BuildProfileCompile, GameConfigHasTheSceneListAndTheProfilesSettings) {
    editor::test::ensureEngine();
    jasperengine::test::TestTempDir dir("build-profile-compile");
    Project project;
    openProjectWithIcon(dir, project);
    const uint32_t menu = project.createSceneNow("Menu", SceneType::SCENE_2D, {}, false, UnsavedChoice::Discard);
    const uint32_t level = project.createSceneNow("Level1", SceneType::SCENE_2D, {}, false, UnsavedChoice::Discard);
    const uint32_t hud = project.createSceneNow("Hud", SceneType::SCENE_2D, {}, false, UnsavedChoice::Discard);
    const uint32_t bonus = project.createSceneNow("Bonus", SceneType::SCENE_2D, {}, false, UnsavedChoice::Discard);
    ASSERT_NE(project.getScene(level), nullptr);
    project.getScene(level)->childScenes.push_back({hud, true});

    const std::shared_ptr<Project> snapshot = Project::makeSettingsSnapshot(project);
    snapshot->setCanvasSize(320, 200);
    snapshot->setLightMode(LightMode::TILED);
    const std::set<uint32_t> shipped = {level, hud};
    editor::cook::GameConfigTarget target;
    target.startScene = level;
    target.settings = snapshot.get();
    target.scenes = &shipped;

    const player::PlayerConfig profile = editor::cook::makeGameConfig(project, target);
    EXPECT_NE(profile.findScene(level), nullptr);
    EXPECT_NE(profile.findScene(hud), nullptr) << "the scene Level1 involves ships with it";
    EXPECT_EQ(profile.findScene(menu), nullptr);
    EXPECT_EQ(profile.findScene(bonus), nullptr);
    EXPECT_EQ(profile.startScene, level);
    EXPECT_EQ(profile.canvasWidth, 320u);
    EXPECT_EQ(profile.canvasHeight, 200u);
    EXPECT_EQ(profile.lightMode, LightMode::TILED);

    // without a profile: every scene, the project's own settings
    editor::cook::GameConfigTarget own;
    own.startScene = menu;
    const player::PlayerConfig game = editor::cook::makeGameConfig(project, own);
    for (uint32_t id : {menu, level, hud, bonus}) EXPECT_NE(game.findScene(id), nullptr) << id;
    EXPECT_EQ(game.startScene, menu);
    EXPECT_NE(game.canvasWidth, 320u);
    EXPECT_NE(game.lightMode, LightMode::TILED);
    editor::test::closeProject(project);
}

}  // namespace BuildProfileTest_cpp
