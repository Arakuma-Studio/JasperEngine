// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Apple build templates as the repository holds them (mobile-shipping spec 6.1, 6.6, 4.4):
// deployment targets, the engine frameworks of the CMake iOS app, the privacy manifest in every
// app bundle, no Google Mobile Ads left. Read as text: nothing here needs a Mac.

#include "Exporter.h"
#include "Project.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine::editor;

namespace apple_templates_test {

    const fs::path repo = JASPER_TEST_REPO_DIR;

    std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // the text of the standalone project's `if(<condition>)` block of engine/CMakeLists.txt (at
    // its indentation, not an elseif), to its endif()
    std::string cmakeBlock(const std::string& text, const std::string& condition) {
        const size_t line = text.find("\n    if(" + condition + ")");
        if (line == std::string::npos) return {};
        const size_t start = line + 5;
        int depth = 0;
        size_t pos = start;
        const std::regex token(R"(\b(if|endif)\s*\()");
        for (std::sregex_iterator it(text.begin() + static_cast<long>(start), text.end(), token), end; it != end; ++it) {
            pos = start + static_cast<size_t>(it->position());
            depth += (*it)[1] == "if" ? 1 : -1;
            if (depth == 0) return text.substr(start, pos - start);
        }
        return text.substr(start);
    }

    // the files of every PBXResourcesBuildPhase of a target, by the build files' comments
    std::string resourcesOf(const std::string& pbx, const std::string& target) {
        const size_t name = pbx.find("\t\t\tname = " + (target.find(' ') != std::string::npos ? "\"" + target + "\"" : target) + ";\n");
        if (name == std::string::npos) return {};
        const size_t objectStart = pbx.rfind("\n\t\t", pbx.rfind("isa = PBXNativeTarget;", name));
        const std::string object = pbx.substr(objectStart, name - objectStart);
        std::string files;
        const std::regex phaseId(R"(([0-9A-F]{24}) /\* Resources \*/)");
        for (std::sregex_iterator it(object.begin(), object.end(), phaseId), end; it != end; ++it) {
            const size_t phase = pbx.find("\t\t" + (*it)[1].str() + " /* Resources */ = {");
            if (phase == std::string::npos) continue;
            files += pbx.substr(phase, pbx.find("\t\t};", phase) - phase);
        }
        return files;
    }

}

using namespace apple_templates_test;

// red before mobile-shipping: the zstd targets kept iOS 13.0 and macOS 10.15 (Doriax 9b95b353b)
TEST(AppleTemplates, XcodeDeploymentTargetsAreTheMinimums) {
    for (const char* file : {"engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj",
                             "engine/platform/apple/ios/player-template/JasperEnginePlayer.xcodeproj/project.pbxproj"}) {
        const std::string pbx = readFile(repo / file);
        ASSERT_FALSE(pbx.empty()) << file;
        const std::regex target(R"((IPHONEOS|MACOSX)_DEPLOYMENT_TARGET = ([0-9.]+);)");
        int count = 0;
        for (std::sregex_iterator it(pbx.begin(), pbx.end(), target), end; it != end; ++it, ++count) {
            EXPECT_EQ((*it)[2].str(), (*it)[1] == "IPHONEOS" ? "15.0" : "12.0") << file << ": " << it->str();
        }
        EXPECT_GT(count, 0) << file;
    }
}

// red before mobile-shipping: the CMake iOS app linked GameController only, and miniaudio's
// AudioUnit calls had no AudioToolbox (Doriax 458bad1a3)
TEST(AppleTemplates, CMakeIosLinksTheEngineFrameworks) {
    const std::string cmake = readFile(repo / "engine/CMakeLists.txt");
    const std::string ios = cmakeBlock(cmake, "CMAKE_SYSTEM_NAME STREQUAL \"iOS\"");
    ASSERT_FALSE(ios.empty());
    for (const std::string& framework : Exporter::appleEngineFrameworks(true)) {
        EXPECT_NE(ios.find("\"-framework " + framework + "\""), std::string::npos) << framework;
    }
}

// red before mobile-shipping: no Apple app bundle had a privacy manifest
TEST(AppleTemplates, EveryAppleBundleShipsThePrivacyManifest) {
    const std::string workspace = readFile(repo / "engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj");
    EXPECT_NE(resourcesOf(workspace, "JasperEngine iOS").find("PrivacyInfo.xcprivacy in Resources"), std::string::npos);
    EXPECT_NE(resourcesOf(workspace, "JasperEngine macOS").find("PrivacyInfo.xcprivacy in Resources"), std::string::npos);
    EXPECT_NE(workspace.find("path = ios/PrivacyInfo.xcprivacy;"), std::string::npos);
    EXPECT_NE(workspace.find("path = macos/PrivacyInfo.xcprivacy;"), std::string::npos);
    const std::string player = readFile(repo / "engine/platform/apple/ios/player-template/JasperEnginePlayer.xcodeproj/project.pbxproj");
    EXPECT_NE(resourcesOf(player, "JasperEnginePlayer").find("PrivacyInfo.xcprivacy in Resources"), std::string::npos);

    const std::string cmake = readFile(repo / "engine/CMakeLists.txt");
    const std::string ios = cmakeBlock(cmake, "CMAKE_SYSTEM_NAME STREQUAL \"iOS\"");
    EXPECT_NE(ios.find("workspaces/xcode/ios/PrivacyInfo.xcprivacy"), std::string::npos);
    const std::string mac = cmakeBlock(cmake, "CMAKE_SYSTEM_NAME STREQUAL \"Darwin\"");
    // the Xcode-generator app and the prebuilt player's bundle
    size_t count = 0;
    for (size_t at = mac.find("workspaces/xcode/macos/PrivacyInfo.xcprivacy"); at != std::string::npos;
         at = mac.find("workspaces/xcode/macos/PrivacyInfo.xcprivacy", at + 1)) {
        count++;
    }
    EXPECT_EQ(count, 2u);
    EXPECT_NE(readFile(repo / "tools/players/ios_player.sh").find("include/"), std::string::npos) << "the template gets JasperNative.h";
}

// red before mobile-shipping: the templates linked and declared Google Mobile Ads (owner decision
// 2026-11-04: built-in AdMob retired; ads go through the native bridge). Every source and
// template file below engine/ and editor/ (spec 10.1); only the tests that assert the absence
// name it.
TEST(AppleTemplates, NoAdMobLeft) {
    const std::set<std::string> allowed = {"editor/tests/export/AppleTemplatesTest.cpp", "editor/tests/export/BuildMethodTest.cpp"};
    const std::set<std::string> textExtensions = {".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".m", ".mm", ".swift", ".java", ".kt",
                                                  ".gradle", ".pro", ".properties", ".xml", ".plist", ".pbxproj", ".xcprivacy",
                                                  ".entitlements", ".cmake", ".txt", ".in", ".py", ".sh", ".md", ".yaml", ".yml",
                                                  ".json", ".lua", ".js", ".html"};
    const char* const needles[] = {"admob", "googlemobileads", "gadapplicationidentifier", "play-services-ads"};
    size_t scanned = 0;
    for (const char* top : {"engine", "editor"}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(repo / top, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!textExtensions.count(ext)) continue;
            const std::string relative = it->path().lexically_relative(repo).generic_string();
            if (allowed.count(relative)) continue;
            std::string text = readFile(it->path());
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            scanned++;
            for (const char* needle : needles) EXPECT_EQ(text.find(needle), std::string::npos) << relative << ": " << needle;
        }
        ASSERT_FALSE(ec) << top << ": " << ec.message();
    }
    EXPECT_GT(scanned, 1000u) << "the walk found the sources";
    // the templates' ad network list went with the SDK
    const std::regex adNetworks(R"(SKAdNetworkItems|GADApplicationIdentifier)");
    for (const char* file : {"engine/workspaces/xcode/ios/Info.plist", "engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj",
                             "engine/workspaces/androidstudio/app/src/main/AndroidManifest.xml"}) {
        const std::string text = readFile(repo / file);
        ASSERT_FALSE(text.empty()) << file;
        EXPECT_FALSE(std::regex_search(text, adNetworks)) << file;
    }
    EXPECT_FALSE(fs::exists(repo / "engine/platform/apple/GoogleMobileAdsSdkiOS-10.14.0"));
    EXPECT_FALSE(fs::exists(repo / "engine/platform/android/java-admob"));
}

// Doriax 323fc511e: the engine API sync walks no SDK bundle (none is vendored now; a guard)
TEST(AppleTemplates, EngineApiSyncSkipsSdkBundles) {
    EXPECT_TRUE(Project::isEngineApiSkippedDir("engine/platform/apple/Sdk.xcframework"));
    EXPECT_TRUE(Project::isEngineApiSkippedDir("engine/platform/apple/Sdk.framework"));
    EXPECT_TRUE(Project::isEngineApiSkippedDir("engine/platform/apple/Res.BUNDLE"));
    EXPECT_FALSE(Project::isEngineApiSkippedDir("engine/core"));
    EXPECT_FALSE(Project::isEngineApiSkippedDir("engine/core/framework"));
}
