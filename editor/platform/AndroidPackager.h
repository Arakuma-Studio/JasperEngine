// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// An APK or Android App Bundle from a prebuilt player (docs/specs/S30-shipping.md R13), with the
// SDK's tools and no NDK, CMake or Gradle: the manifest from the project's Android settings, its
// icons compiled with aapt2, an aapt2 link of the player's compiled resources (stable ids) and
// the game's content as assets/game, the player's dex and the selected ABIs' libraries (stored, or
// compressed in an APK for minSdk < 23 as Gradle packages them),
// zipalign -P 16, apksigner (or bundletool and jarsigner for a bundle). A missing tool fails the
// build with what installs it; nothing falls back to Gradle.

#include "Project.h"
#include "ShippingTypes.h"
#include "util/CommandRunner.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::platform {

    struct AndroidPackage {
        AndroidProjectSettings settings;
        // Player > Resolution and Presentation: the manifest's screenOrientation and keep-screen-on
        PresentationSettings presentation;
        std::string label;
        std::string applicationId;
        unsigned int versionCode = 1;
        std::string versionName = "1.0";
        bool astc = true;                      // supports-gl-texture ASTC (no ETC2 fallback)
        std::filesystem::path runtimeDir;      // the player runtime's android/
        std::filesystem::path contentDir;      // the game's content (assets/game of the package)
        std::filesystem::path projectPath;     // the project's icons are relative to it
        std::filesystem::path workDir;
        std::filesystem::path output;
        // Aab, or an APK (Apk and ApkPerAbi alike: the caller makes one package per ABI, with abis)
        AndroidFormat format = AndroidFormat::Aab;
        // the ABIs it holds; empty: the project's selection (settings)
        std::vector<std::string> abis;
        bool debug = false;
        // empty: the debug keystore (made with keytool when missing)
        std::filesystem::path keystore;
        std::string keystoreAlias;
        std::string keystorePassword;          // else JASPER_ANDROID_KS_PASS
        std::string keyPassword;               // else JASPER_ANDROID_KEY_PASS, else the keystore's
        // a bundle's bundletool jar; empty: the pinned one, downloaded once (AndroidTools::bundletoolJar)
        std::filesystem::path bundletool;
    };

    // What keytool's output says is wrong with a keystore and its key, in words a user can act on
    // (the wrong store or key password, no such alias, not a keystore); "" when it says none of it
    std::string describeKeystoreFailure(const std::string& keytoolOutput, const std::filesystem::path& keystore, const std::string& alias);

    // whether a package compresses its native libraries (an APK for minSdk < 23, as the Android
    // Gradle plugin does; its manifest then says extractNativeLibs="true") or stores them
    bool compressesNativeLibraries(const AndroidPackage& package);

    // the AndroidManifest.xml of a package (the runtime's template with the settings filled in)
    bool writeAndroidManifest(const AndroidPackage& package, const std::string& templateText, std::string& manifest, std::string& error);

    bool buildAndroidPackage(const AndroidPackage& package, CommandRunner& runner, const std::function<bool()>& cancelled,
                             std::vector<BuildWarning>& warnings, std::string& error);

    // the project's selected ABIs (Project Settings > Android > Architectures), in package order
    std::vector<std::string> selectedAndroidAbis(const AndroidProjectSettings& settings);
    // the bytes of each ABI's native libraries in a player runtime's android/ (lib/<abi>/*.so):
    // what an APK of that ABI carries, stored (Build Settings shows the package sizes from it)
    std::vector<std::pair<std::string, uint64_t>> androidLibrarySizes(const std::filesystem::path& runtimeAndroidDir,
                                                                      const std::vector<std::string>& abis);

}
