// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A source export's Android Studio project (docs/specs/S98-editor-tooling-gaps.md REQ-13 to REQ-17,
// F2; AC12, AC13, AC15): the Gradle template as the export writes it from Project Settings >
// Android. Read as text: Gradle itself runs in tools/build.sh android.

#include "support/ExporterTestAccess.h"
#include "support/TestTempDir.h"

#include "Exporter.h"
#include "export/ExportFiles.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/type/AndroidSettingsCmd.h"
#include "platform/AndroidPackager.h"
#include "settings/SettingsRegistry.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <regex>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace android_template_test {

    const fs::path androidTemplateRepo = JASPER_TEST_REPO_DIR;
    const fs::path workspace = androidTemplateRepo / "engine" / "workspaces" / "androidstudio";

    std::unique_ptr<Project> androidTemplateProjectIn(const fs::path& root) {
        fs::create_directories(root / "assets");
        std::ofstream(root / "project.yaml") << "name: AndroidGame\nassetsDir: assets\nluaDir: assets\n";
        auto project = std::make_unique<Project>();
        project->setSkipProjectLock(true);
        EXPECT_TRUE(project->loadProject(root, false));
        return project;
    }

    // the export's Android Studio project: the template and MainActivity copied as copyEngine does,
    // then written from the project's settings; app/build.gradle's text
    std::string exportGradle(Project& project, const fs::path& target, std::string& error) {
        if (!exportfiles::mirrorTree(workspace, target / "workspaces" / "androidstudio", true, error)) return {};
        const fs::path java = fs::path("platform") / "android" / "java" / "com" / "jaspergameengine" / "jasperengine" / "MainActivity.java";
        fs::create_directories((target / java).parent_path());
        fs::copy_file(androidTemplateRepo / "engine" / java, target / java);
        Exporter exporter;
        if (!ExporterTestAccess::writeAndroidProjectSettings(exporter, &project, target, error)) return {};
        return TestTempDir::read(target / "workspaces" / "androidstudio" / "app" / "build.gradle");
    }

    // the text of a Gradle block opened by `header` (its braces counted)
    std::string block(const std::string& text, const std::string& header) {
        const size_t start = text.find(header);
        if (start == std::string::npos) return {};
        int depth = 0;
        for (size_t i = text.find('{', start); i < text.size(); i++) {
            if (text[i] == '{') depth++;
            else if (text[i] == '}' && --depth == 0) return text.substr(start, i + 1 - start);
        }
        return text.substr(start);
    }

}

using namespace android_template_test;

// red before REQ-13: no pattern, so AGP's default left out every folder starting with "_"
TEST(AndroidTemplate, IgnoreAssetsPatternKeepsUnderscoreDirs) {
    TestTempDir dir("android-template-pattern");
    auto project = androidTemplateProjectIn(dir / "game");
    std::string error;
    const std::string gradle = exportGradle(*project, dir / "export", error);
    ASSERT_FALSE(gradle.empty()) << error;
    EXPECT_NE(gradle.find(std::string("def jasperIgnoreAssets = '") + platform::kAndroidIgnoreAssetsPattern + "'"), std::string::npos) << gradle;
    EXPECT_NE(gradle.find("ignoreAssetsPattern = jasperIgnoreAssets"), std::string::npos) << gradle;
    EXPECT_EQ(std::string(platform::kAndroidIgnoreAssetsPattern).find("<dir>_*"), std::string::npos);
    // the prebuilt package leaves out the same names
    EXPECT_FALSE(platform::isIgnoredAndroidAsset("_Onboarding"));
    EXPECT_FALSE(platform::isIgnoredAndroidAsset("_glow.png"));
    EXPECT_TRUE(platform::isIgnoredAndroidAsset(".gitkeep"));
    EXPECT_TRUE(platform::isIgnoredAndroidAsset("Thumbs.db"));
    EXPECT_TRUE(platform::isIgnoredAndroidAsset("level.lua~"));
    EXPECT_TRUE(platform::isIgnoredAndroidAsset("CVS"));
    EXPECT_FALSE(platform::isIgnoredAndroidAsset("level.lua"));
    // and the build fails naming a file the package lost (the Gradle side of REQ-13)
    EXPECT_NE(gradle.find("jasperengineVerifyContent"), std::string::npos);
}

// red before REQ-14: the template had only a release build type, so AGP compiled debug at -O0
TEST(AndroidTemplate, DebugTypeBuildsOptimisedNative) {
    TestTempDir dir("android-template-debug");
    auto project = androidTemplateProjectIn(dir / "game");
    project->getAndroidProjectSettings().debugPackageSuffix = ".debug";
    std::string error;
    const std::string gradle = exportGradle(*project, dir / "export", error);
    ASSERT_FALSE(gradle.empty()) << error;
    const std::string debug = block(gradle, "        debug {");
    ASSERT_FALSE(debug.empty()) << gradle;
    EXPECT_NE(debug.find("applicationIdSuffix \".debug\""), std::string::npos) << debug;
    EXPECT_NE(debug.find("-DCMAKE_BUILD_TYPE=RelWithDebInfo"), std::string::npos) << debug;
    EXPECT_NE(debug.find("-DJASPERENGINE_DEVELOPMENT_BUILD=ON"), std::string::npos) << debug;

    // no suffix: the same package
    TestTempDir other("android-template-debug-plain");
    auto plain = androidTemplateProjectIn(other / "game");
    const std::string plainGradle = exportGradle(*plain, other / "export", error);
    EXPECT_EQ(plainGradle.find("applicationIdSuffix"), std::string::npos) << plainGradle;
}

// red before REQ-15: the release build was never signed with the project's keystore
TEST(AndroidTemplate, ReleaseSigningFromKeystore) {
    TestTempDir dir("android-template-signing");
    auto project = androidTemplateProjectIn(dir / "game");
    TestTempDir::write(dir / "game" / "keys" / "upload.jks", "not a real keystore");
    AndroidProjectSettings& android = project->getAndroidProjectSettings();
    android.keystorePath = "keys/upload.jks";
    android.keystoreAlias = "upload";
    std::string error;
    const std::string gradle = exportGradle(*project, dir / "export", error);
    ASSERT_FALSE(gradle.empty()) << error;
    const std::string signing = block(gradle, "    signingConfigs {");
    ASSERT_FALSE(signing.empty()) << gradle;
    EXPECT_NE(signing.find("storeFile file(\"" + (dir / "game" / "keys" / "upload.jks").generic_string() + "\")"), std::string::npos) << signing;
    EXPECT_NE(signing.find("storePassword System.getenv(\"JASPER_ANDROID_KS_PASS\")"), std::string::npos) << signing;
    EXPECT_NE(signing.find("keyAlias \"upload\""), std::string::npos) << signing;
    EXPECT_NE(signing.find("System.getenv(\"JASPER_ANDROID_KEY_PASS\")"), std::string::npos) << signing;
    const std::string release = block(gradle, "        release {\n            signingConfig");
    EXPECT_NE(release.find("signingConfig signingConfigs.release"), std::string::npos) << gradle;
    // the passwords are never written: only the variables' names
    EXPECT_EQ(std::regex_search(gradle, std::regex(R"((store|key)Password\s+["'])")), false) << gradle;

    // no keystore: unsigned (Android Studio signs it)
    TestTempDir other("android-template-unsigned");
    auto plain = androidTemplateProjectIn(other / "game");
    const std::string plainGradle = exportGradle(*plain, other / "export", error);
    EXPECT_EQ(plainGradle.find("signingConfig"), std::string::npos) << plainGradle;
}

// red before REQ-17: CMake was not pinned and the image installed no NDK or CMake
TEST(AndroidTemplate, PinsTheImagesNdkAndCMake) {
    const std::string gradle = TestTempDir::read(workspace / "app" / "build.gradle");
    const std::string docker = TestTempDir::read(androidTemplateRepo / "docker" / "android.Dockerfile");
    std::smatch ndk, cmake;
    ASSERT_TRUE(std::regex_search(gradle, ndk, std::regex(R"re(ndkVersion = "([0-9.]+)")re"))) << gradle;
    ASSERT_TRUE(std::regex_search(gradle, cmake, std::regex(R"re(\n\s+version = "([0-9.]+)")re"))) << gradle;
    EXPECT_NE(docker.find("ARG ANDROID_NDK=" + ndk[1].str()), std::string::npos) << docker;
    EXPECT_NE(docker.find("ARG ANDROID_CMAKE=" + cmake[1].str()), std::string::npos) << docker;
    EXPECT_NE(docker.find("\"ndk;${ANDROID_NDK}\" \"cmake;${ANDROID_CMAKE}\""), std::string::npos) << docker;
}

// F2 (brought into scope): an App Bundle's content is the install-time asset pack game_content
TEST(AndroidTemplate, BundlesCarryTheInstallTimeAssetPack) {
    const std::string settings = TestTempDir::read(workspace / "settings.gradle");
    EXPECT_NE(settings.find("include ':game_content'"), std::string::npos) << settings;
    const std::string pack = TestTempDir::read(workspace / "game_content" / "build.gradle");
    EXPECT_NE(pack.find("apply plugin: 'com.android.asset-pack'"), std::string::npos) << pack;
    EXPECT_NE(pack.find(std::string("packName = \"") + platform::kAndroidContentPack + "\""), std::string::npos) << pack;
    EXPECT_NE(pack.find("deliveryType = \"install-time\""), std::string::npos) << pack;
    const std::string gradle = TestTempDir::read(workspace / "app" / "build.gradle");
    EXPECT_NE(gradle.find("assetPacks = [\":game_content\"]"), std::string::npos) << gradle;
    EXPECT_NE(gradle.find("jasperengineSyncAssetPack"), std::string::npos) << gradle;
}

// android.debugPackageSuffix (REQ-14, REQ-44, REQ-50): absent in old projects, a dot and one
// identifier through set_project_settings, kept by project.yaml, one undo step from the window
TEST(AndroidTemplate, DebugPackageSuffixSetting) {
    Project old;
    Stream::decodeProject(&old, YAML::Load("name: Old\n"), Workspace());
    EXPECT_EQ(old.getAndroidProjectSettings().debugPackageSuffix, "");
    EXPECT_FALSE(Stream::encodeProject(&old)["android"]) << "the default writes nothing";

    TestTempDir dir("android-suffix-setting");
    auto project = androidTemplateProjectIn(dir / "game");
    SettingsContext ctx;
    ctx.projectPtr = project.get();
    const SettingRow* row = SettingsRegistry::get().find("android.debug_package_suffix");
    ASSERT_NE(row, nullptr);
    std::string error;
    for (const char* bad : {"dbg", ".1dbg", ".d-b", "."}) EXPECT_FALSE(row->set(ctx, mcp::Json(bad), false, error)) << bad;
    for (const char* good : {".dbg", ".debug", ".dev.local", ""}) EXPECT_TRUE(row->set(ctx, mcp::Json(good), false, error)) << good << ": " << error;
    ASSERT_TRUE(row->set(ctx, mcp::Json(".dbg"), true, error)) << error;

    const std::string yaml = YAML::Dump(Stream::encodeProject(project.get()));
    EXPECT_NE(yaml.find("debugPackageSuffix: .dbg"), std::string::npos) << yaml;
    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_EQ(loaded.getAndroidProjectSettings().debugPackageSuffix, ".dbg");

    AndroidProjectSettings changed = project->getAndroidProjectSettings();
    changed.debugPackageSuffix = ".debug";
    AndroidSettingsCmd command(project.get(), changed);
    ASSERT_TRUE(command.execute());
    EXPECT_EQ(project->getAndroidProjectSettings().debugPackageSuffix, ".debug");
    command.undo();
    EXPECT_EQ(project->getAndroidProjectSettings().debugPackageSuffix, ".dbg");
}
