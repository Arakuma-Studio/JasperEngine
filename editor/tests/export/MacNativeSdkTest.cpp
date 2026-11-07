// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A macOS CMake build takes the plugin SDKs of plugins/native/apple and macos as the Xcode
// project's macOS target does (docs/superpowers/specs/2026-11-04-mobile-shipping-design.md,
// "Follow-ups"): frameworks, .xcframeworks (their macOS library) and static libraries linked, the
// dynamic ones carried in Frameworks/ (its rpath) and the resource bundles as resources: beside an
// unbundled executable, in Contents/ of a .app. Only the text and the copies are checked here;
// linking and launching need a Mac.

// (Not on Windows: a framework's Versions/Current is a link, which Windows makes only with a privilege.)
#if !defined(_WIN32)

#include "support/TestTempDir.h"

#include "Exporter.h"
#include "util/NativePlugins.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace mac_native_sdk_test {

    const char* const root = "${PROJECT_ROOT}/plugins/native";

    // a Mach-O's first bytes (not "!<arch>\n": a dynamic library)
    const char* const dynamicBinary = "\xcf\xfa\xed\xfe dynamic";
    const char* const staticBinary = "!<arch>\nstatic";

    std::string xcframeworkPlist(const std::string& libraries) {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
               "<key>AvailableLibraries</key>\n<array>\n" + libraries + "</array>\n"
               "<key>CFBundlePackageType</key><string>XFWK</string>\n</dict>\n</plist>\n";
    }

    std::string plistLibrary(const std::string& identifier, const std::string& library, const std::string& platform,
                                   const std::string& variant = "", const std::string& headers = "") {
        std::string text = "<dict><key>LibraryIdentifier</key><string>" + identifier + "</string>"
                           "<key>LibraryPath</key><string>" + library + "</string>"
                           "<key>SupportedPlatform</key><string>" + platform + "</string>";
        if (!variant.empty()) text += "<key>SupportedPlatformVariant</key><string>" + variant + "</string>";
        if (!headers.empty()) text += "<key>HeadersPath</key><string>" + headers + "</string>";
        return text + "</dict>\n";
    }

    // plugins/native of a project with one SDK of each kind
    void writeSdks(const fs::path& project) {
        const fs::path native = project / "plugins/native";
        TestTempDir::write(native / "macos/Store.m", "@implementation Store\n@end\n");
        // a versioned dynamic framework: Versions/Current and the binary are links
        TestTempDir::write(native / "macos/Dyn.framework/Versions/A/Dyn", dynamicBinary);
        TestTempDir::write(native / "macos/Dyn.framework/Versions/A/Headers/Dyn.h", "void dyn(void);\n");
        fs::create_directory_symlink("A", native / "macos/Dyn.framework/Versions/Current");
        fs::create_symlink("Versions/Current/Dyn", native / "macos/Dyn.framework/Dyn");
        fs::create_directory_symlink("Versions/Current/Headers", native / "macos/Dyn.framework/Headers");
        TestTempDir::write(native / "macos/Dyn.framework/Versions/A/Resources/Info.plist", "<plist/>\n");
        TestTempDir::write(native / "macos/Static.framework/Static", staticBinary);
        TestTempDir::write(native / "macos/libZip.a", staticBinary);
        TestTempDir::write(native / "apple/Res.bundle/Contents/Info.plist", "<plist/>\n");
        // the macOS library is not the first one
        TestTempDir::write(native / "apple/Ads.xcframework/Info.plist",
                           xcframeworkPlist(plistLibrary("ios-arm64", "Ads.framework", "ios") +
                                            plistLibrary("ios-arm64_x86_64-maccatalyst", "Ads.framework", "ios", "maccatalyst") +
                                            plistLibrary("macos-arm64_x86_64", "Ads.framework", "macos")));
        TestTempDir::write(native / "apple/Ads.xcframework/ios-arm64/Ads.framework/Ads", dynamicBinary);
        TestTempDir::write(native / "apple/Ads.xcframework/macos-arm64_x86_64/Ads.framework/Ads", dynamicBinary);
        TestTempDir::write(native / "apple/Zip2.xcframework/Info.plist",
                           xcframeworkPlist(plistLibrary("macos-arm64", "libzip2.a", "macos", "", "Headers")));
        TestTempDir::write(native / "apple/Zip2.xcframework/macos-arm64/libzip2.a", staticBinary);
        TestTempDir::write(native / "apple/Zip2.xcframework/macos-arm64/Headers/zip2.h", "int zip2(void);\n");
        TestTempDir::write(native / "apple/IosOnly.xcframework/Info.plist",
                           xcframeworkPlist(plistLibrary("ios-arm64", "IosOnly.framework", "ios")));
        TestTempDir::write(native / "apple/IosOnly.xcframework/ios-arm64/IosOnly.framework/IosOnly", dynamicBinary);
        // a macOS library that is a lone dylib
        TestTempDir::write(native / "macos/Dylib.xcframework/Info.plist",
                           xcframeworkPlist(plistLibrary("macos-arm64_x86_64", "libdyn.dylib", "macos")));
        TestTempDir::write(native / "macos/Dylib.xcframework/macos-arm64_x86_64/libdyn.dylib", dynamicBinary);
    }

    bool has(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

}

using namespace mac_native_sdk_test;

// red before platform-build: the CMake build compiled the Objective-C sources only, so an SDK was
// neither linked nor carried and the game failed to link or to launch
TEST(MacNativeSdk, TheCMakeBuildLinksAndCarriesTheSdks) {
    TestTempDir dir("mac-native-sdk");
    writeSdks(dir.path());
    const std::string cmake = NativePlugins::macosCMake(NativePlugins::scan(dir.path()), "Game", root, {}, "${JASPERENGINE_ROOT}/platform/apple");

    EXPECT_TRUE(has(cmake, "target_compile_options(Game PRIVATE \"-F${PROJECT_ROOT}/plugins/native/macos\")")) << cmake;
    EXPECT_TRUE(has(cmake, "target_link_options(Game PRIVATE \"-F${PROJECT_ROOT}/plugins/native/apple/Ads.xcframework/macos-arm64_x86_64\")")) << cmake;
    EXPECT_TRUE(has(cmake, "\"-framework Ads\"")) << cmake;
    EXPECT_TRUE(has(cmake, "\"-framework Dyn\"")) << cmake;
    EXPECT_TRUE(has(cmake, "\"-framework Static\"")) << cmake;
    EXPECT_TRUE(has(cmake, "\"${PROJECT_ROOT}/plugins/native/macos/libZip.a\"")) << cmake;
    EXPECT_TRUE(has(cmake, "\"${PROJECT_ROOT}/plugins/native/apple/Zip2.xcframework/macos-arm64/libzip2.a\"")) << cmake;
    EXPECT_TRUE(has(cmake, "target_include_directories(Game PRIVATE \"${PROJECT_ROOT}/plugins/native/apple/Zip2.xcframework/macos-arm64/Headers\")")) << cmake;
    EXPECT_FALSE(has(cmake, "IosOnly")) << "an iOS-only SDK has nothing for macOS\n" << cmake;
    EXPECT_FALSE(has(cmake, "ios-arm64/")) << cmake;

    // carried: the dynamic frameworks in Frameworks/ (the rpath), the bundle as a resource; in a
    // .app (the Xcode generator) at Contents/, else beside the unbundled executable
    EXPECT_TRUE(has(cmake, "get_target_property(JASPERENGINE_NATIVE_APP Game MACOSX_BUNDLE)")) << cmake;
    EXPECT_TRUE(has(cmake, "set(JASPERENGINE_NATIVE_FRAMEWORKS_DIR \"$<TARGET_BUNDLE_CONTENT_DIR:Game>/Frameworks\")")) << cmake;
    EXPECT_TRUE(has(cmake, "set(JASPERENGINE_NATIVE_RESOURCES_DIR \"$<TARGET_BUNDLE_CONTENT_DIR:Game>/Resources\")")) << cmake;
    EXPECT_TRUE(has(cmake, "set(JASPERENGINE_NATIVE_RPATH \"@executable_path/../Frameworks\")")) << cmake;
    EXPECT_TRUE(has(cmake, "set(JASPERENGINE_NATIVE_FRAMEWORKS_DIR \"$<TARGET_FILE_DIR:Game>/Frameworks\")")) << cmake;
    EXPECT_TRUE(has(cmake, "set(JASPERENGINE_NATIVE_RESOURCES_DIR \"$<TARGET_FILE_DIR:Game>\")")) << cmake;
    EXPECT_TRUE(has(cmake, "set(JASPERENGINE_NATIVE_RPATH \"@executable_path/Frameworks\")")) << cmake;
    EXPECT_TRUE(has(cmake, "target_link_options(Game PRIVATE \"-Wl,-rpath,${JASPERENGINE_NATIVE_RPATH}\")")) << cmake;
    EXPECT_TRUE(has(cmake, "add_custom_command(TARGET Game POST_BUILD")) << cmake;
    // embedded as Xcode's CodeSignOnCopy and RemoveHeadersOnCopy do (platform-build review)
    const std::string embed = "COMMAND /bin/sh \"${CMAKE_CURRENT_BINARY_DIR}/jasperengine-embed-framework.sh\" ";
    EXPECT_TRUE(has(cmake, embed + "\"${PROJECT_ROOT}/plugins/native/macos/Dyn.framework\" \"${JASPERENGINE_NATIVE_FRAMEWORKS_DIR}/Dyn.framework\"")) << cmake;
    EXPECT_TRUE(has(cmake, embed + "\"${PROJECT_ROOT}/plugins/native/apple/Ads.xcframework/macos-arm64_x86_64/Ads.framework\" "
                           "\"${JASPERENGINE_NATIVE_FRAMEWORKS_DIR}/Ads.framework\"")) << cmake;
    EXPECT_TRUE(has(cmake, embed + "\"${PROJECT_ROOT}/plugins/native/macos/Dylib.xcframework/macos-arm64_x86_64/libdyn.dylib\" "
                           "\"${JASPERENGINE_NATIVE_FRAMEWORKS_DIR}/libdyn.dylib\"")) << cmake;
    const size_t links = cmake.find("target_link_libraries(Game PRIVATE \"-framework Ads\"");
    ASSERT_NE(links, std::string::npos) << cmake;
    EXPECT_TRUE(has(cmake.substr(links, cmake.find('\n', links) - links), "\"${PROJECT_ROOT}/plugins/native/macos/Dylib.xcframework/macos-arm64_x86_64/libdyn.dylib\""))
        << "linked too\n" << cmake;
    EXPECT_TRUE(has(cmake, "file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/jasperengine-embed-framework.sh\" [==[")) << cmake;
    EXPECT_TRUE(has(cmake, "find \"$2\" \\( -name Headers -o -name PrivateHeaders \\) -prune -exec rm -rf {} +")) << cmake;
    EXPECT_TRUE(has(cmake, "codesign --force --sign \"${EXPANDED_CODE_SIGN_IDENTITY:-${JASPER_MACOS_SIGN_IDENTITY:--}}\"")) << cmake;
    EXPECT_TRUE(has(cmake, "COMMAND ditto \"${PROJECT_ROOT}/plugins/native/apple/Res.bundle\" \"${JASPERENGINE_NATIVE_RESOURCES_DIR}/Res.bundle\"")) << cmake;
    EXPECT_FALSE(has(cmake, "macos/Static.framework\"")) << "a static framework is linked, not carried\n" << cmake;
    EXPECT_FALSE(has(cmake, "libZip.a\" \"${JASPERENGINE_NATIVE")) << cmake;
    // the block stays macOS-only
    EXPECT_LT(cmake.find("if(APPLE AND NOT IOS)"), cmake.find("-framework Ads"));
    EXPECT_EQ(cmake.substr(cmake.size() - 8), "endif()\n");
}

TEST(MacNativeSdk, ThePlayLibraryFindsTheDynamicOnesWhereTheyAre) {
    TestTempDir dir("mac-native-sdk");
    writeSdks(dir.path());
    const NativePlugins::Scan scan = NativePlugins::scan(dir.path());
    const std::vector<NativePlugins::MacosSdk> sdks = NativePlugins::macosSdks(scan);
    auto find = [&sdks](const std::string& path) {
        return std::find_if(sdks.begin(), sdks.end(), [&path](const NativePlugins::MacosSdk& sdk) { return sdk.path == path; });
    };
    ASSERT_NE(find("macos/Dyn.framework"), sdks.end());
    EXPECT_TRUE(find("macos/Dyn.framework")->embedded);
    ASSERT_NE(find("macos/Static.framework"), sdks.end());
    EXPECT_FALSE(find("macos/Static.framework")->embedded);
    ASSERT_NE(find("apple/Res.bundle"), sdks.end());
    EXPECT_TRUE(find("apple/Res.bundle")->resource);
    ASSERT_NE(find("apple/Zip2.xcframework"), sdks.end());
    EXPECT_FALSE(find("apple/Zip2.xcframework")->embedded);
    EXPECT_EQ(find("apple/Zip2.xcframework")->headers, "apple/Zip2.xcframework/macos-arm64/Headers");
    EXPECT_EQ(find("apple/IosOnly.xcframework"), sdks.end());

    const std::string cmake = NativePlugins::macosCMake(scan, "jasperengine-plugin", "${CMAKE_CURRENT_SOURCE_DIR}/plugins/native", {},
                                                        "/engine/platform/apple", false);
    EXPECT_TRUE(has(cmake, "\"-framework Dyn\"")) << cmake;
    EXPECT_FALSE(has(cmake, "POST_BUILD")) << "a library is no app: nothing is copied beside it\n" << cmake;
    EXPECT_FALSE(has(cmake, "@executable_path")) << cmake;
    EXPECT_TRUE(has(cmake, "\"-Wl,-rpath,${CMAKE_CURRENT_SOURCE_DIR}/plugins/native/macos\"")) << cmake;
    EXPECT_TRUE(has(cmake, "\"-Wl,-rpath,${CMAKE_CURRENT_SOURCE_DIR}/plugins/native/apple/Ads.xcframework/macos-arm64_x86_64\"")) << cmake;
    EXPECT_FALSE(has(cmake, "MACOSX_BUNDLE")) << cmake;
}

// red before platform-build for the sidecars: plugin folders were mirrored with the editor's .meta
// files and Finder's .DS_Store, and one inside a framework or bundle breaks its signature's seal;
// red before the review for the headers (Xcode's RemoveHeadersOnCopy) and a removed bundle
TEST(MacNativeSdk, TheExportCarriesThemBesideTheExecutable) {
    TestTempDir dir("mac-native-sdk");
    const fs::path project = dir / "project";
    writeSdks(project);
    TestTempDir::write(project / "plugins/native/macos/Dyn.framework/Versions/A/Resources/Info.plist.meta", "guid: 1\n");
    TestTempDir::write(project / "plugins/native/macos/Dyn.framework/Versions/A/.DS_Store", "Bud1");
    TestTempDir::write(project / "plugins/native/apple/Res.bundle/Contents/.DS_Store", "Bud1");
    const fs::path out = dir / "out";
    TestTempDir::write(out / "Frameworks/Old.framework/Old", dynamicBinary);
    TestTempDir::write(out / "Old.bundle/Contents/Info.plist", "<plist/>\n");

    std::string error;
    ASSERT_TRUE(Exporter::copyMacosNativeSdks(project, out, error)) << error;
    std::error_code ec;
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(out / "Frameworks/Dyn.framework/Versions/Current", ec))) << "the framework's links stay links";
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(out / "Frameworks/Dyn.framework/Dyn", ec)));
    EXPECT_TRUE(fs::is_regular_file(out / "Frameworks/Dyn.framework/Versions/A/Dyn"));
    EXPECT_TRUE(fs::is_regular_file(out / "Frameworks/Dyn.framework/Versions/A/Resources/Info.plist"));
    EXPECT_FALSE(fs::exists(fs::symlink_status(out / "Frameworks/Dyn.framework/Headers", ec))) << "no headers in an app";
    EXPECT_FALSE(fs::exists(out / "Frameworks/Dyn.framework/Versions/A/Headers"));
    EXPECT_FALSE(fs::exists(out / "Frameworks/Dyn.framework/Versions/A/Resources/Info.plist.meta")) << "an editor sidecar is no SDK file";
    EXPECT_FALSE(fs::exists(out / "Frameworks/Dyn.framework/Versions/A/.DS_Store"));
    EXPECT_TRUE(fs::is_regular_file(out / "Frameworks/Ads.framework/Ads"));
    EXPECT_TRUE(fs::is_regular_file(out / "Frameworks/libdyn.dylib")) << "an xcframework's lone dylib";
    EXPECT_TRUE(fs::is_regular_file(out / "Res.bundle/Contents/Info.plist"));
    EXPECT_FALSE(fs::exists(out / "Res.bundle/Contents/.DS_Store"));
    EXPECT_FALSE(fs::exists(out / "Old.bundle")) << "an earlier build's bundle the project no longer has";
    EXPECT_FALSE(fs::exists(out / "Frameworks/Static.framework")) << "linked into the executable";
    EXPECT_FALSE(fs::exists(out / "Frameworks/IosOnly.framework"));
    EXPECT_FALSE(fs::exists(out / "Frameworks/Old.framework")) << "an earlier build's framework the project no longer has";

    // a project without SDKs leaves no Frameworks/ behind
    fs::remove_all(project / "plugins");
    ASSERT_TRUE(Exporter::copyMacosNativeSdks(project, out, error)) << error;
    EXPECT_FALSE(fs::exists(out / "Frameworks"));
}

#endif
