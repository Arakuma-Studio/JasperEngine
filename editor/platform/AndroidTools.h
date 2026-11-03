// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Android tools a prebuilt APK or bundle is made with (docs/specs/S30-shipping.md R13): a JDK
// 17+ (java, keytool, jarsigner), the SDK's build-tools 35.0.0+ (aapt2, zipalign, apksigner) and
// its platform android-<targetSdk>, and bundletool 1.17.2 (downloaded once, its sha256 pinned).
// Found through Preferences > External Tools (androidSdk, jdk), then ANDROID_HOME /
// ANDROID_SDK_ROOT / JAVA_HOME, then PATH. No NDK, CMake or Gradle.

#include "platform/ExternalTools.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    struct AndroidToolchain {
        std::filesystem::path sdk;
        std::filesystem::path buildTools;      // build-tools/<version>
        std::string buildToolsVersion;
        ToolLocation aapt2, zipalign, apksigner;
        std::filesystem::path androidJar;
        ToolLocation java, keytool, jarsigner;
        std::filesystem::path bundletool;      // the jar (only for an aab)
        // each missing piece, with the SDK Manager package or download that provides it
        std::vector<std::string> missing;
        bool ok() const { return missing.empty(); }
    };

    class AndroidTools {
    public:
        static constexpr const char* BUNDLETOOL_VERSION = "1.17.2";
        static constexpr const char* BUNDLETOOL_SHA256 = "2d4ad908faea64047c1cc9cb747e6aa667c6ab192e09607bd16b67246a8cd6ae";
        static constexpr const char* MIN_BUILD_TOOLS = "35.0.0";

        static std::filesystem::path sdkRoot();
        static std::filesystem::path jdkHome();
        // needBundletool: an aab build (the jar is downloaded when `download` and missing)
        static AndroidToolchain discover(unsigned int targetSdk, bool needBundletool, bool download);
        // the cached bundletool jar (downloaded and checked against its pinned sha256)
        static std::filesystem::path bundletoolJar(bool download, std::string& error);
        // `apksigner verify --verbose`: true when it accepts; its output in `output`
        static bool verifyApk(const std::filesystem::path& apk, std::string& output, std::string& error);
    };

}
