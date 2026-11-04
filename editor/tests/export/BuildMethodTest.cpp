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
#include "Scene.h"
#include "component/MeshComponent.h"

#include <gtest/gtest.h>

#include <algorithm>
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

TEST(BuildMethod, ContentSizeNeverForcesCompile) {
    // components hold what they are given (mesh-memory): no capacity of a player to pass
    Project project;
    SceneProject& scene = addScene(project, 1);
    scene.scene = new Scene();
    const Entity entity = scene.scene->createEntity();
    MeshComponent mesh;
    mesh.numSubmeshes = 24;
    mesh.submeshes[23].material.name = "last";
    mesh.bonesMatrix.resize(200);
    scene.scene->addComponent<MeshComponent>(entity, mesh);
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt) << (choice.reasons.empty() ? "" : choice.reasons.front());
    delete scene.scene;
    scene.scene = nullptr;
}

TEST(BuildMethod, PlayerWithCapacitiesIsRefused) {
    // a player packaged when components had fixed arrays would cut what the scenes hold
    Project project;
    addScene(project, 1);
    PlayerInfo old;
    old.id = "linux-x64-glcore-release";
    old.capacities["MAX_BONES"] = 128;
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), &old);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "fixed array capacities")) << (choice.reasons.empty() ? "" : choice.reasons.front());
}

TEST(BuildMethod, AdMobAndForcedCompile) {
    Project project;
    addScene(project, 1);
    project.getAndroidProjectSettings().admobEnabled = true;
    ExportConfig android = linuxConfig();
    android.platform = BuildPlatform::Android;
    BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, android, nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "AdMob")) << (choice.reasons.empty() ? "" : choice.reasons.front());
    // AdMob is Android's: a Linux build does not need it
    EXPECT_EQ(Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr).method, BuildMethod::Prebuilt);

    ExportConfig forced = linuxConfig();
    forced.method = BuildMethod::Compile;
    choice = Exporter::chooseBuildMethod(&project, forced, nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "Compile chosen"));
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
