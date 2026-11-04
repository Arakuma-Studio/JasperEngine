// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Which method a build uses (docs/specs/S30-shipping.md R5, AC3): a prebuilt player unless the
// project needs native code, each reason named.

#include "support/TestTempDir.h"

#include "Exporter.h"
#include "Generator.h"
#include "Project.h"
#include "platform/AndroidPackager.h"
#include "players/PlayerRuntimes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace build_method_test {

    bool hasReason(const BuildMethodChoice& choice, const std::string& part) {
        return std::any_of(choice.reasons.begin(), choice.reasons.end(),
                           [&part](const std::string& reason) { return reason.find(part) != std::string::npos; });
    }

    SceneProject& addScene(Project& project, uint32_t id) {
        SceneProject scene;
        scene.id = id;
        scene.name = "Scene" + std::to_string(id);
        project.getScenes().push_back(scene);
        return project.getScenes().back();
    }

    ExportConfig linuxConfig() {
        ExportConfig config;
        config.mode = ExportMode::Player;
        config.platform = BuildPlatform::Linux;
        return config;
    }

    ExportConfig configFor(BuildPlatform platform) {
        ExportConfig config = linuxConfig();
        config.platform = platform;
        return config;
    }

    bool hasCarried(const BuildMethodChoice& choice, const std::string& part) {
        return std::any_of(choice.carried.begin(), choice.carried.end(),
                           [&part](const std::string& item) { return item.find(part) != std::string::npos; });
    }

    // a project on disk (plugins/native is read from its folder), one scene
    std::unique_ptr<Project> projectIn(const fs::path& root) {
        fs::create_directories(root / "assets");
        fs::create_directories(root / "lua");
        std::ofstream(root / "project.yaml") << "name: native\nassetsDir: assets\nluaDir: lua\n";
        auto project = std::make_unique<Project>();
        project->setSkipProjectLock(true);
        EXPECT_TRUE(project->loadProject(root, false));
        addScene(*project, 1);
        return project;
    }

}

using namespace build_method_test;

TEST(BuildMethod, APlainProjectUsesThePrebuiltPlayer) {
    Project project;
    addScene(project, 1);
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt);
    EXPECT_TRUE(choice.reasons.empty());
}

TEST(BuildMethod, CppScriptsCompile) {
    TestTempDir dir("build-method");
    TestTempDir::write(dir / "scripts/Player.cpp", "int x;\n");
    TestTempDir::write(dir / "scripts/Player.h", "#pragma once\n");
    Project project;
    addScene(project, 1);
    project.setScriptDirs({dir / "scripts"});
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "C++ scripts (")) << (choice.reasons.empty() ? "" : choice.reasons.front());
    EXPECT_TRUE(hasReason(choice, "Player.cpp"));
    EXPECT_FALSE(hasReason(choice, "Player.h")) << "a header alone compiles nothing";
}

TEST(BuildMethod, CppScriptsOfTheScenesCompile) {
    // a C++ script a scene uses, outside any script directory (tools/fixtures/prefab-runtime)
    TestTempDir dir("build-method");
    TestTempDir::write(dir / "cpp/Spawner.cpp", "int spawner;\n");
    Project project;
    SceneProject& scene = addScene(project, 1);
    SceneScriptSource script;
    script.path = dir / "cpp/Spawner.cpp";
    scene.cppScripts.push_back(script);
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "Spawner.cpp")) << (choice.reasons.empty() ? "" : choice.reasons.front());
}

TEST(BuildMethod, CapacitiesAboveThePlayersCompile) {
    Project project;
    addScene(project, 1).maxValues.maxBones = 200;
    BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "MAX_BONES 200 (the player holds 128)")) << (choice.reasons.empty() ? "" : choice.reasons.front());

    // a player that holds more takes it
    PlayerInfo bigger;
    bigger.capacities["MAX_BONES"] = 256;
    choice = Exporter::chooseBuildMethod(&project, linuxConfig(), &bigger);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt);
}

TEST(BuildMethod, ForcedCompile) {
    Project project;
    addScene(project, 1);
    ExportConfig forced = linuxConfig();
    forced.method = BuildMethod::Compile;
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, forced, nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "Compile chosen"));
}

// mobile-shipping: a prebuilt build never drops plugins/native silently; each input is carried
// or a reason to compile, named

TEST(BuildMethod, NativeCppPluginsCompile) {
    // red before mobile-shipping: plugins/native/cpp was outside the C++ scan, the build went prebuilt
    TestTempDir dir("build-method-native");
    TestTempDir::write(dir / "plugins/native/cpp/SteamPlugin.cpp", "int steam;\n");
    auto project = projectIn(dir.path());
    for (BuildPlatform platform : {BuildPlatform::Linux, BuildPlatform::Windows, BuildPlatform::Web, BuildPlatform::Android, BuildPlatform::IOS}) {
        const BuildMethodChoice choice = Exporter::chooseBuildMethod(project.get(), configFor(platform), nullptr);
        EXPECT_EQ(choice.method, BuildMethod::Compile) << buildPlatformName(platform);
        EXPECT_TRUE(hasReason(choice, "C++ native plugins (plugins/native/cpp/SteamPlugin.cpp)")) << buildPlatformName(platform);
    }
}

TEST(BuildMethod, AndroidCodePluginsCompile) {
    // red before mobile-shipping: the prebuilt APK left the Java plugin and the Gradle dependency out
    TestTempDir dir("build-method-native");
    TestTempDir::write(dir / "plugins/native/android/java/com/me/Ads.java", "package com.me; public class Ads {}\n");
    auto project = projectIn(dir.path());
    BuildMethodChoice choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::Android), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "Java/Kotlin plugins (plugins/native/android/java/com/me/Ads.java)"));
    // the other platforms have nothing of it to build
    EXPECT_EQ(Exporter::chooseBuildMethod(project.get(), linuxConfig(), nullptr).method, BuildMethod::Prebuilt);

    fs::remove_all(dir / "plugins");
    project->getAndroidProjectSettings().gradleDependencies = {"com.example:sdk:1.0"};
    choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::Android), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "Gradle dependencies (android.gradle_dependencies) (com.example:sdk:1.0)"));
}

TEST(BuildMethod, AndroidManifestSettingsAreCarried) {
    TestTempDir dir("build-method-native");
    auto project = projectIn(dir.path());
    AndroidProjectSettings& android = project->getAndroidProjectSettings();
    android.extraPermissions = {"com.google.android.gms.permission.AD_ID"};
    android.manifestApplicationXml = "<meta-data android:name=\"k\" android:value=\"v\"/>";
    BuildMethodChoice choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::Android), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt) << (choice.reasons.empty() ? "" : choice.reasons.front());
    EXPECT_TRUE(hasCarried(choice, "com.google.android.gms.permission.AD_ID"));
    EXPECT_TRUE(hasCarried(choice, "android.manifest_application_xml"));
    // merge rules need Gradle's manifest merger
    android.manifestXml = "<uses-permission android:name=\"android.permission.CAMERA\" tools:node=\"remove\"/>";
    choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::Android), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "android.manifest_xml uses tools: merge rules"));
}

TEST(BuildMethod, MacObjectiveCPluginsCompile) {
    // red before mobile-shipping: the prebuilt macOS bundle left the plugin out
    TestTempDir dir("build-method-native");
    TestTempDir::write(dir / "plugins/native/macos/Store.m", "@implementation Store\n@end\n");
    auto project = projectIn(dir.path());
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::MacOS), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "Objective-C/Swift plugins for macOS (plugins/native/macos/Store.m)"));
    EXPECT_EQ(Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::IOS), nullptr).method, BuildMethod::Prebuilt)
        << "a macOS plugin is not iOS's";
}

TEST(BuildMethod, IosPluginsAreCarried) {
    TestTempDir dir("build-method-native");
    TestTempDir::write(dir / "plugins/native/ios/Device.m", "@implementation Device\n@end\n");
    auto project = projectIn(dir.path());
    project->getIOSProjectSettings().swiftPackages.push_back({"https://github.com/example/sdk", "1.0.0", {"Sdk"}});
    BuildMethodChoice choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::IOS), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt) << (choice.reasons.empty() ? "" : choice.reasons.front());
    EXPECT_TRUE(hasCarried(choice, "iOS plugins (plugins/native/ios/Device.m)"));
    EXPECT_TRUE(hasCarried(choice, "Swift packages"));

    // a runtime from before native plugins (no include/JasperNative.h in its template)
    PlayerInfo old;
    old.id = "ios-arm64-metal-release";
    choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::IOS), &old);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "ios-arm64-metal-release player runtime cannot take native plugins"));
    old.features.push_back("nativeplugins");
    EXPECT_EQ(Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::IOS), &old).method, BuildMethod::Prebuilt);

    // CocoaPods need pod install and the workspace of the source export
    project->getIOSProjectSettings().cocoapods.push_back({"Google-Mobile-Ads-SDK", "", ""});
    choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::IOS), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "CocoaPods (ios.cocoapods)"));
}

TEST(BuildMethod, WebPluginsAreCarried) {
    TestTempDir dir("build-method-native");
    TestTempDir::write(dir / "plugins/native/web/poki.js", "JasperNative.register('Poki', {});\n");
    auto project = projectIn(dir.path());
    project->getWebProjectSettings().scriptUrls = {"https://game-cdn.poki.com/scripts/v2/poki-sdk.js"};
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(project.get(), configFor(BuildPlatform::Web), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt);
    EXPECT_TRUE(hasCarried(choice, "Web plugins (plugins/native/web/poki.js)"));
    EXPECT_TRUE(hasCarried(choice, "web.script_urls"));
}

TEST(BuildMethod, MockOnlyPluginsWarn) {
    TestTempDir dir("build-method-native");
    TestTempDir::write(dir / "plugins/native/android/java/com/me/Ads.java", "package com.me; public class Ads {}\n");
    auto project = projectIn(dir.path());
    EXPECT_TRUE(Exporter::chooseBuildMethod(project.get(), linuxConfig(), nullptr).nativeWarnings.empty()) << "no mock: no warning";
    TestTempDir::write(dir / "lua/native_mocks/Ads.lua", "return {}\n");
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(project.get(), linuxConfig(), nullptr);
    ASSERT_EQ(choice.nativeWarnings.size(), 1u);
    EXPECT_NE(choice.nativeWarnings[0].find("\"Ads\" has no code for linux"), std::string::npos) << choice.nativeWarnings[0];
    EXPECT_NE(choice.nativeWarnings[0].find("native_mocks/Ads.lua"), std::string::npos);
}

TEST(BuildMethod, PlayerIds) {
    ExportConfig config = linuxConfig();
    EXPECT_EQ(Exporter::playerIdFor(config, false), "linux-x64-glcore-release");
    config.graphicBackend = "vulkan";
    EXPECT_EQ(Exporter::playerIdFor(config, true), "linux-x64-vulkan-development");
    config.platform = BuildPlatform::Web;
    config.graphicBackend = "";
    EXPECT_EQ(Exporter::playerIdFor(config, false), "web-wasm32-gles3-release");
    config.platform = BuildPlatform::MacOS;
    EXPECT_EQ(Exporter::playerIdFor(config, false), "macos-universal-metal-release");
    config.platform = BuildPlatform::Android;
    EXPECT_EQ(Exporter::playerIdFor(config, false), "android-universal-gles3-release");
}

// Android packages (docs/specs/S30-shipping.md R13): an App Bundle by default, so Google Play
// serves each device one ABI; per-ABI APKs and the universal APK are explicit choices
TEST(AndroidPackage, AppBundleByDefaultAndTheFormatNames) {
    Project project;
    EXPECT_EQ(project.getAndroidProjectSettings().packageFormat, "aab");
    EXPECT_EQ(ExportConfig().androidFormat, AndroidFormat::Aab);
    for (AndroidFormat format : {AndroidFormat::Aab, AndroidFormat::ApkPerAbi, AndroidFormat::Apk}) {
        AndroidFormat parsed = AndroidFormat::Aab;
        EXPECT_TRUE(parseAndroidFormat(androidFormatName(format), parsed)) << androidFormatName(format);
        EXPECT_EQ(parsed, format);
    }
    EXPECT_STREQ(androidFormatName(AndroidFormat::Apk), "apk");
    AndroidFormat parsed = AndroidFormat::ApkPerAbi;
    EXPECT_FALSE(parseAndroidFormat("universal", parsed));
    EXPECT_EQ(parsed, AndroidFormat::ApkPerAbi) << "an unknown word leaves the format as it was";
}

TEST(AndroidPackage, SelectedAbisAndTheirLibrarySizes) {
    AndroidProjectSettings settings;
    settings.abiArm64V8a = true;
    settings.abiArmeabiV7a = true;
    settings.abiX86 = false;
    settings.abiX86_64 = true;
    EXPECT_EQ(platform::selectedAndroidAbis(settings), (std::vector<std::string>{"arm64-v8a", "armeabi-v7a", "x86_64"}));

    TestTempDir dir("android-sizes");
    TestTempDir::write(dir / "lib/arm64-v8a/libjasperengine-android.so", std::string(100, 'a'));
    TestTempDir::write(dir / "lib/arm64-v8a/libc++_shared.so", std::string(20, 'c'));
    TestTempDir::write(dir / "lib/arm64-v8a/README.txt", "not a library");
    TestTempDir::write(dir / "lib/armeabi-v7a/libjasperengine-android.so", std::string(70, 'a'));
    const auto sizes = platform::androidLibrarySizes(dir.path(), {"arm64-v8a", "armeabi-v7a", "x86_64"});
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_EQ(sizes[0], (std::pair<std::string, uint64_t>{"arm64-v8a", 120}));
    EXPECT_EQ(sizes[1], (std::pair<std::string, uint64_t>{"armeabi-v7a", 70}));
    EXPECT_EQ(sizes[2], (std::pair<std::string, uint64_t>{"x86_64", 0})) << "an ABI the runtime lacks weighs nothing";
}

// mobile-shipping: the code-free native settings reach a prebuilt package's manifest (red before:
// the prebuilt package read only the Player permissions)
TEST(AndroidPackage, CarriesNativeManifestSettings) {
    std::ifstream in(fs::path(JASPER_TEST_REPO_DIR) / "engine/platform/android/player/AndroidManifest.template.xml");
    ASSERT_TRUE(in);
    const std::string templateText((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    platform::AndroidPackage package;
    package.label = "Game";
    package.applicationId = "com.example.game";
    package.settings.extraPermissions = {"com.google.android.gms.permission.AD_ID"};
    package.settings.manifestXml = "<queries>\n    <package android:name=\"com.example.other\"/>\n</queries>";
    package.settings.manifestApplicationXml = "<meta-data android:name=\"com.example.APP_ID\" android:value=\"42\"/>";
    std::string manifest, error;
    ASSERT_TRUE(platform::writeAndroidManifest(package, templateText, manifest, error)) << error;
    const size_t permission = manifest.find("<uses-permission android:name=\"com.google.android.gms.permission.AD_ID\" />");
    const size_t queries = manifest.find("    <queries>\n        <package android:name=\"com.example.other\"/>\n    </queries>");
    const size_t application = manifest.find("<application");
    const size_t metaData = manifest.find("        <meta-data android:name=\"com.example.APP_ID\" android:value=\"42\"/>");
    const size_t applicationEnd = manifest.find("</application>");
    ASSERT_NE(permission, std::string::npos) << manifest;
    ASSERT_NE(queries, std::string::npos) << manifest;
    ASSERT_NE(metaData, std::string::npos) << manifest;
    EXPECT_LT(permission, application);
    EXPECT_LT(queries, application);
    EXPECT_GT(metaData, application);
    EXPECT_LT(metaData, applicationEnd);
    EXPECT_EQ(manifest.find("{{"), std::string::npos) << "every marker filled";

    package.settings.manifestXml = "<queries>";
    EXPECT_FALSE(platform::writeAndroidManifest(package, templateText, manifest, error));
    EXPECT_NE(error.find("android.manifest_xml"), std::string::npos) << error;
}

// native libraries as the Android Gradle plugin packages them: compressed in an APK for devices
// before Android 6 (extracted at install), stored from minSdk 23 and in a bundle
TEST(AndroidPackage, NativeLibrariesCompressedBelowApi23) {
    std::ifstream in(fs::path(JASPER_TEST_REPO_DIR) / "engine/platform/android/player/AndroidManifest.template.xml");
    ASSERT_TRUE(in);
    const std::string templateText((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    platform::AndroidPackage package;
    package.label = "Game";
    package.applicationId = "com.example.game";
    auto manifestOf = [&](AndroidFormat format, unsigned int minSdk) {
        package.format = format;
        package.settings.minSdk = minSdk;
        std::string manifest, error;
        EXPECT_TRUE(platform::writeAndroidManifest(package, templateText, manifest, error)) << error;
        return manifest;
    };
    EXPECT_NE(manifestOf(AndroidFormat::Apk, 21).find("android:extractNativeLibs=\"true\""), std::string::npos);
    EXPECT_NE(manifestOf(AndroidFormat::ApkPerAbi, 22).find("android:extractNativeLibs=\"true\""), std::string::npos);
    EXPECT_NE(manifestOf(AndroidFormat::Apk, 23).find("android:extractNativeLibs=\"false\""), std::string::npos);
    EXPECT_NE(manifestOf(AndroidFormat::Aab, 21).find("android:extractNativeLibs=\"false\""), std::string::npos)
        << "a bundle keeps them stored: bundletool makes each device's APK";
    package.format = AndroidFormat::Apk;
    package.settings.minSdk = 21;
    EXPECT_TRUE(platform::compressesNativeLibraries(package));
    package.settings.minSdk = 26;
    EXPECT_FALSE(platform::compressesNativeLibraries(package));
}
