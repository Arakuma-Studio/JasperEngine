// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Which method a build uses (docs/specs/S30-shipping.md R5, AC3): a prebuilt player unless the
// project needs native code, each reason named.

#include "support/TestTempDir.h"

#include "Exporter.h"
#include "Generator.h"
#include "Project.h"
#include "platform/AndroidPackager.h"
#include "platform/IosProject.h"
#include "players/PlayerRuntimes.h"
#include "util/ZipArchive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
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

// ---------------------------------------------------------------- Android backends (S47)

// platform/android makes an EGL context and nothing else, so no Vulkan player can be built: Build
// Settings, get_build_info and --backend must not offer one
TEST(AndroidPackage, OffersOnlyTheBackendItsPlatformLayerHas) {
    EXPECT_EQ(platformBackends(BuildPlatform::Android), (std::vector<std::string>{"gles3"}));
}

// ---------------------------------------------------------------- Android signing (S47 R3)

TEST(AndroidSigning, KeytoolsAnswersInWordsAUserCanActOn) {
    const fs::path keystore = "release.keystore";
    const auto describe = [&keystore](const std::string& output) { return platform::describeKeystoreFailure(output, keystore, "game"); };
    for (const char* wrongStore : {"keytool error: java.io.IOException: keystore password was incorrect",
                                   "keytool error: java.io.IOException: Keystore was tampered with, or password was incorrect"}) {
        EXPECT_NE(describe(wrongStore).find("password of the keystore release.keystore is wrong"), std::string::npos) << wrongStore;
        EXPECT_NE(describe(wrongStore).find("JASPER_ANDROID_KS_PASS"), std::string::npos) << wrongStore;
    }
    EXPECT_NE(describe("keytool error: java.security.UnrecoverableKeyException: Cannot recover key").find("password of the key game"), std::string::npos);
    EXPECT_NE(describe("keytool error: java.lang.Exception: Alias <gmae> does not exist").find("has no key named game"), std::string::npos);
    EXPECT_NE(describe("keytool error: java.io.IOException: Invalid keystore format").find("is not a keystore"), std::string::npos);
    EXPECT_EQ(describe("keytool error: java.lang.OutOfMemoryError"), "");
}

// A prebuilt package with stand-ins for the SDK's and the JDK's tools: what each is given, in a log
// (the real tools: tools/ci/android_packaging.sh)
#ifndef _WIN32
namespace build_method_test {

    class ScopedVariable {
    public:
        ScopedVariable(const char* name, const std::string& value): name(name) {
            const char* old = std::getenv(name);
            had = old != nullptr;
            if (old) previous = old;
            setenv(name, value.c_str(), 1);
        }
        ~ScopedVariable() {
            if (had) setenv(name, previous.c_str(), 1);
            else unsetenv(name);
        }
    private:
        const char* name;
        bool had = false;
        std::string previous;
    };

    void writeTool(const fs::path& path, const std::string& body) {
        TestTempDir::write(path, "#!/bin/sh\necho \"$(basename \"$0\") $*\" >> \"$FAKE_TOOLS_LOG\"\n" + body);
        fs::permissions(path, fs::perms::owner_all);
    }

    // copies the package every tool "makes" to the path given after `flag`
    std::string makesAfter(const std::string& flag) {
        return "while [ $# -gt 1 ]; do [ \"$1\" = \"" + flag + "\" ] && cp \"$FAKE_TOOLS_ZIP\" \"$2\"; shift; done\n";
    }

    struct FakeAndroid {
        TestTempDir dir{"android-tools"};
        fs::path log = dir / "tools.log";
        ScopedVariable sdk{"ANDROID_HOME", (dir / "sdk").string()};
        ScopedVariable jdk{"JAVA_HOME", (dir / "jdk").string()};
        ScopedVariable home{"HOME", (dir / "home").string()};
        ScopedVariable logVariable{"FAKE_TOOLS_LOG", log.string()};
        ScopedVariable zipVariable{"FAKE_TOOLS_ZIP", (dir / "package.zip").string()};
        platform::AndroidPackage package;
        CommandRunner runner;
        std::vector<BuildWarning> warnings;
        std::string error;

        FakeAndroid() {
            package.settings.abiArm64V8a = false;
            package.settings.abiArmeabiV7a = false;
            package.settings.abiX86 = false;
            package.settings.abiX86_64 = true;
            const fs::path buildTools = dir / "sdk" / "build-tools" / "36.0.0";
            writeTool(buildTools / "aapt2", makesAfter("-o"));
            writeTool(buildTools / "zipalign", "for last; do :; done\ncp \"$FAKE_TOOLS_ZIP\" \"$last\"\n");
            writeTool(buildTools / "apksigner", makesAfter("--out"));
            TestTempDir::write(dir / "sdk" / "platforms" / ("android-" + std::to_string(package.settings.targetSdk)) / "android.jar", "jar");
            writeTool(dir / "jdk" / "bin" / "java", "for arg; do case \"$arg\" in --output=*) cp \"$FAKE_TOOLS_ZIP\" \"${arg#--output=}\";; esac; done\n");
            writeTool(dir / "jdk" / "bin" / "jarsigner", makesAfter("-signedjar"));
            // the keystore holds the alias FAKE_ALIAS, with the passwords FAKE_STORE and FAKE_KEY
            writeTool(dir / "jdk" / "bin" / "keytool", R"SH(store=; key=; alias=; made=; made_at=
while [ $# -gt 0 ]; do
    case "$1" in
        -genkeypair) made=1 ;;
        -keystore) made_at="$2"; shift ;;
        -alias) alias="$2"; shift ;;
        -storepass:file) store="$(cat "$2")"; shift ;;
        -keypass:file) key="$(cat "$2")"; shift ;;
    esac
    shift
done
if [ -n "$made" ]; then echo made > "$made_at"; exit 0; fi
if [ "$store" != "$FAKE_STORE" ]; then echo "keytool error: java.io.IOException: keystore password was incorrect"; exit 1; fi
if [ "$alias" != "$FAKE_ALIAS" ]; then echo "keytool error: java.lang.Exception: Alias <$alias> does not exist"; exit 1; fi
if [ "$key" != "$FAKE_KEY" ]; then echo "keytool error: java.security.UnrecoverableKeyException: Cannot recover key"; exit 1; fi
)SH");
            std::string zipError;
            ZipWriter zip;
            EXPECT_TRUE(zip.open(dir / "package.zip", zipError)) << zipError;
            EXPECT_TRUE(zip.add("AndroidManifest.xml", "x", 1, true, zipError)) << zipError;
            EXPECT_TRUE(zip.finish(zipError)) << zipError;

            const fs::path runtime = dir / "runtime" / "android";
            std::ifstream in(fs::path(JASPER_TEST_REPO_DIR) / "engine/platform/android/player/AndroidManifest.template.xml");
            TestTempDir::write(runtime / "AndroidManifest.template.xml", std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            TestTempDir::write(runtime / "res-flat.zip", "zip");
            TestTempDir::write(runtime / "stable-ids.txt", "");
            TestTempDir::write(runtime / "classes.dex", "dex");
            TestTempDir::write(runtime / "lib" / "x86_64" / "libjasperengine-android.so", "so");
            TestTempDir::write(dir / "content" / "project.bin", "content");
            package.label = "Game";
            package.applicationId = "com.example.game";
            package.runtimeDir = runtime;
            package.contentDir = dir / "content";
            package.projectPath = dir.path();
            package.workDir = dir / "work";
            package.output = dir / "out" / "Game.apk";
            package.format = AndroidFormat::Apk;
            package.bundletool = dir / "bundletool.jar";
            fs::create_directories(dir / "out");
            fs::create_directories(dir / "home");
        }

        bool build() {
            warnings.clear();
            error.clear();
            return platform::buildAndroidPackage(package, runner, []() { return false; }, warnings, error);
        }

        std::string tools() const {
            std::ifstream in(log);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        void useKeystore(const std::string& alias, const std::string& storePassword, const std::string& keyPassword) {
            TestTempDir::write(dir / "release.keystore", "keystore");
            package.keystore = dir / "release.keystore";
            package.keystoreAlias = alias;
            package.keystorePassword = storePassword;
            package.keyPassword = keyPassword;
        }
    };

}

TEST(AndroidSigning, TheDebugKeyByDefaultMadeWhenMissing) {
    FakeAndroid android;
    ScopedVariable store("FAKE_STORE", "android"), key("FAKE_KEY", "android"), alias("FAKE_ALIAS", "androiddebugkey");
    ASSERT_TRUE(android.build()) << android.error;
    const std::string tools = android.tools();
    EXPECT_NE(tools.find("-genkeypair"), std::string::npos) << tools;
    EXPECT_NE(tools.find("--ks " + (android.dir / "home/.android/debug.keystore").string() + " --ks-key-alias androiddebugkey"), std::string::npos) << tools;
    ASSERT_EQ(android.warnings.size(), 1u);
    EXPECT_EQ(android.warnings.front().code, "W-ANDROID-DEBUG-KEY") << "a release build signed for testing says so";

    android.package.debug = true;
    fs::remove(android.log);
    ASSERT_TRUE(android.build()) << android.error;
    EXPECT_EQ(android.tools().find("-genkeypair"), std::string::npos) << "the key is kept";
    EXPECT_TRUE(android.warnings.empty());
}

namespace build_method_test {

    // a release keystore signs the package, its passwords in files that do not outlive the build
    void expectReleaseKeystoreSigns(AndroidFormat format) {
        FakeAndroid android;
        ScopedVariable store("FAKE_STORE", "s3cret-store"), key("FAKE_KEY", "s3cret-key"), alias("FAKE_ALIAS", "game");
        android.useKeystore("game", "s3cret-store", "s3cret-key");
        android.package.format = format;
        ASSERT_TRUE(android.build()) << android.error;
        EXPECT_TRUE(android.warnings.empty());
        EXPECT_TRUE(fs::exists(android.package.output));
        const std::string tools = android.tools();
        const std::string keystore = (android.dir / "release.keystore").string();
        if (format == AndroidFormat::Apk) {
            EXPECT_NE(tools.find("--ks " + keystore + " --ks-key-alias game"), std::string::npos) << tools;
        } else {
            EXPECT_NE(tools.find("jarsigner -keystore " + keystore), std::string::npos) << tools;
            // an EC key has another signature algorithm: jarsigner picks the one that fits
            EXPECT_EQ(tools.find("-sigalg"), std::string::npos) << tools;
        }
        EXPECT_EQ(tools.find("s3cret"), std::string::npos) << "on a command line: " << tools;
        EXPECT_FALSE(fs::exists(android.package.workDir / ".ks-pass"));
        EXPECT_FALSE(fs::exists(android.package.workDir / ".key-pass"));
    }

}

TEST(AndroidSigning, AReleaseKeystoreSignsAnApk) {
    expectReleaseKeystoreSigns(AndroidFormat::Apk);
}

TEST(AndroidSigning, AReleaseKeystoreSignsABundleWithAnyKeyType) {
    expectReleaseKeystoreSigns(AndroidFormat::Aab);
}

TEST(AndroidSigning, TheEnvironmentGivesThePasswords) {
    FakeAndroid android;
    ScopedVariable store("FAKE_STORE", "from-env"), key("FAKE_KEY", "key-from-env"), alias("FAKE_ALIAS", "game");
    ScopedVariable storePassword("JASPER_ANDROID_KS_PASS", "from-env"), keyPassword("JASPER_ANDROID_KEY_PASS", "key-from-env");
    android.useKeystore("game", "", "");
    EXPECT_TRUE(android.build()) << android.error;
}

TEST(AndroidSigning, AWrongPasswordOrAliasFailsBeforeAnyWorkAndClearly) {
    FakeAndroid android;
    ScopedVariable store("FAKE_STORE", "right-store"), key("FAKE_KEY", "right-key"), alias("FAKE_ALIAS", "game");

    android.useKeystore("game", "wr0ng-store", "right-key");
    EXPECT_FALSE(android.build());
    EXPECT_NE(android.error.find("password of the keystore"), std::string::npos) << android.error;
    EXPECT_EQ(android.error.find("wr0ng-store"), std::string::npos) << android.error;
    std::string tools = android.tools();
    EXPECT_EQ(tools.find("aapt2"), std::string::npos) << "no packaging before the key is known good: " << tools;
    EXPECT_EQ(tools.find("wr0ng-store"), std::string::npos) << tools;
    EXPECT_FALSE(fs::exists(android.package.workDir / ".ks-pass"));
    EXPECT_FALSE(fs::exists(android.package.output));

    android.useKeystore("game", "right-store", "wr0ng-key");
    EXPECT_FALSE(android.build());
    EXPECT_NE(android.error.find("password of the key game"), std::string::npos) << android.error;
    EXPECT_NE(android.error.find("JASPER_ANDROID_KEY_PASS"), std::string::npos) << android.error;
    EXPECT_EQ(android.tools().find("wr0ng-key"), std::string::npos);

    android.useKeystore("gmae", "right-store", "right-key");
    EXPECT_FALSE(android.build());
    EXPECT_NE(android.error.find("has no key named gmae"), std::string::npos) << android.error;

    android.useKeystore("game", "", "");
    EXPECT_FALSE(android.build());
    EXPECT_NE(android.error.find("needs its password"), std::string::npos) << android.error;

    android.useKeystore("game", "right-store", "right-key");
    fs::remove(android.package.keystore);
    EXPECT_FALSE(android.build());
    EXPECT_NE(android.error.find("does not exist"), std::string::npos) << android.error;
}
#endif

// ---------------------------------------------------------------- iOS (S47 R4)

TEST(IosBuild, ChoosesAFullXcodeAndLeavesTheMacsSettingAlone) {
    const std::string tools = "/Library/Developer/CommandLineTools";
    const std::string active = "/Applications/Xcode.app/Contents/Developer";
    const std::vector<fs::path> none;
    const std::vector<fs::path> installed = {"/Applications/Xcode-16.app", "/Applications/Xcode.app", "/Applications/Xcode_26.app"};

    platform::XcodeChoice choice = platform::chooseXcode("", active, none);
    EXPECT_TRUE(choice.found());
    EXPECT_EQ(choice.developerDir.string(), active);
    EXPECT_FALSE(choice.throughEnvironment);

    choice = platform::chooseXcode("/Elsewhere/Xcode.app/Contents/Developer", tools, installed);
    EXPECT_EQ(choice.developerDir.string(), "/Elsewhere/Xcode.app/Contents/Developer") << "DEVELOPER_DIR wins";
    EXPECT_FALSE(choice.throughEnvironment) << "it is in the environment already";

    // xcode-select on the Command Line Tools, with an Xcode installed (the case that failed)
    choice = platform::chooseXcode("", tools, installed);
    EXPECT_TRUE(choice.found());
    EXPECT_EQ(choice.developerDir.string(), "/Applications/Xcode.app/Contents/Developer");
    EXPECT_TRUE(choice.throughEnvironment);

    choice = platform::chooseXcode("", tools, {"/Applications/Xcode-16.app", "/Applications/Xcode_26.app"});
    EXPECT_EQ(choice.developerDir.string(), "/Applications/Xcode_26.app/Contents/Developer");

    choice = platform::chooseXcode("", tools, none);
    EXPECT_FALSE(choice.found());
    EXPECT_NE(choice.missing.find("Command Line Tools"), std::string::npos) << choice.missing;
    EXPECT_FALSE(platform::chooseXcode("", "", none).found());
}

TEST(IosBuild, XcodeFailuresInWordsAUserCanActOn) {
    const auto describe = [](const std::string& log) { return platform::describeXcodeFailure("noise\n" + log + "\nmore noise", "ABCDE12345"); };
    EXPECT_NE(describe("error: Signing for \"Game\" requires a development team. Select a development team").find("Team ID"), std::string::npos);
    EXPECT_NE(describe("error: No Account for Team \"ABCDE12345\". Add a new account").find("Xcode > Settings > Accounts"), std::string::npos);
    EXPECT_NE(describe("error: No Accounts: Add a new account in Accounts settings.").find("ABCDE12345"), std::string::npos);
    EXPECT_NE(describe("error: No profiles for 'com.example.game' were found").find("provisioning profile"), std::string::npos);
    EXPECT_NE(describe("error: No signing certificate \"iOS Development\" found").find("certificate"), std::string::npos);
    EXPECT_NE(describe("You have not agreed to the Xcode license agreements.").find("licence"), std::string::npos);
    EXPECT_NE(describe("error: iOS 26.5 is not installed. Please download and install the platform").find("Components"), std::string::npos);
    EXPECT_NE(describe("xcode-select: error: tool 'xcodebuild' requires Xcode, but active developer directory").find("DEVELOPER_DIR"), std::string::npos);
    EXPECT_EQ(describe("error: something else"), "");
}

// The template links the player's library with -all_load, so it must name every framework the
// engine's code needs (a prebuilt game failed to link on CoreMedia), and no SDK it does not ship
TEST(IosBuild, ThePlayerTemplateLinksWhatTheEngineNeeds) {
    const auto read = [](const std::string& relative) {
        std::ifstream in(fs::path(JASPER_TEST_REPO_DIR) / relative, std::ios::binary);
        EXPECT_TRUE(in.good()) << relative;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string template_ = read("engine/platform/apple/ios/player-template/JasperEnginePlayer.xcodeproj/project.pbxproj");
    const std::string cmake = read("engine/CMakeLists.txt");
    const std::string marker = "target_link_libraries(jasperengine PRIVATE \"-framework ";
    size_t checked = 0;
    for (size_t at = cmake.find(marker); at != std::string::npos; at = cmake.find(marker, at + 1)) {
        const std::string line = cmake.substr(at, cmake.find('\n', at) - at);
        for (size_t word = line.find("-framework "); word != std::string::npos; word = line.find("-framework ", word + 1)) {
            const size_t start = word + 11;
            const std::string name = line.substr(start, line.find('"', start) - start);
            EXPECT_NE(template_.find(name + ".framework in Frameworks"), std::string::npos) << name;
            checked++;
        }
    }
    EXPECT_GE(checked, 4u) << "the engine's framework list moved: " << marker;
    EXPECT_EQ(template_.find("GoogleMobileAds"), std::string::npos);
    EXPECT_NE(read("tools/players/ios_player.sh").find("-DJASPERENGINE_ADMOB=OFF"), std::string::npos)
        << "the template links no ads SDK, so the player must not reference it";
}

// no Xcode here: the Xcode project is the result, its settings filled in, and it says so
#ifndef __APPLE__
TEST(IosBuild, WithoutAMacTheXcodeProjectIsTheResult) {
    TestTempDir dir("ios-project");
    const fs::path runtime = dir / "runtime" / "ios";
    TestTempDir::write(runtime / "template" / "Info.plist", "<string>{{DISPLAY_NAME}}</string>");
    TestTempDir::write(runtime / "template" / "JasperEnginePlayer.xcodeproj" / "project.pbxproj",
                       "PRODUCT_BUNDLE_IDENTIFIER = {{BUNDLE_ID}}; DEVELOPMENT_TEAM = \"{{TEAM_ID}}\";");
    TestTempDir::write(runtime / "template" / "JasperEnginePlayer.xcodeproj" / "xcshareddata" / "xcschemes" / "JasperEnginePlayer.xcscheme",
                       "{{PRODUCT_NAME}}");
    TestTempDir::write(runtime / "JasperEnginePlayer.xcframework" / "Info.plist", "x");
    platform::IosProjectSettings settings;
    settings.appName = "Game";
    settings.displayName = "A & B";
    settings.bundleIdentifier = "com.example.game";
    settings.teamId = "ABCDE12345";
    settings.runtimeDir = runtime;
    settings.projectDir = dir / "project";
    CommandRunner runner;
    std::vector<fs::path> artifacts;
    std::vector<std::string> notes;
    std::vector<BuildWarning> warnings;
    std::string error;
    ASSERT_TRUE(platform::buildIosProject(settings, runner, []() { return false; }, artifacts, notes, warnings, error)) << error;
    ASSERT_EQ(artifacts.size(), 1u);
    EXPECT_EQ(artifacts.front().string(), (settings.projectDir / "JasperEnginePlayer.xcodeproj").string());
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_NE(notes.front().find("a Mac with Xcode"), std::string::npos) << notes.front();
    std::ifstream plist(settings.projectDir / "Info.plist");
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(plist)), std::istreambuf_iterator<char>()), "<string>A &amp; B</string>");
    std::ifstream pbxproj(settings.projectDir / "JasperEnginePlayer.xcodeproj" / "project.pbxproj");
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(pbxproj)), std::istreambuf_iterator<char>()),
              "PRODUCT_BUNDLE_IDENTIFIER = com.example.game; DEVELOPMENT_TEAM = \"ABCDE12345\";");
}
#endif
