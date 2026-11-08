// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The file layer of every export (editor/export/ExportFiles.h, export-pipeline spec 6.1, 6.4):
// one walker whose errors fail the export, copies that report failure, the folders an earlier
// export left, and the engine workspaces' IDE output, never copied and never removed.

#include "export/ExportFiles.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <system_error>

using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

TEST(ExportFiles, OpenErrorsClassify) {
    // a folder that can't be read is left out with a warning; anything else stops the export
    EXPECT_TRUE(exportfiles::isSkippableOpenError(std::make_error_code(std::errc::permission_denied)));
    EXPECT_FALSE(exportfiles::isSkippableOpenError(std::make_error_code(std::errc::io_error)));
    EXPECT_FALSE(exportfiles::isSkippableOpenError(std::make_error_code(std::errc::not_a_directory)));
    EXPECT_FALSE(exportfiles::isSkippableOpenError(std::make_error_code(std::errc::no_such_file_or_directory)));
}

TEST(ExportFiles, WalkFailsWhenTheRootIsNotADirectory) {
    TestTempDir dir("export-walk");
    TestTempDir::write(dir / "file.txt", "x");
    auto visit = [](const fs::directory_entry&, const fs::path&) { return true; };
    const exportfiles::WalkResult onFile = exportfiles::walk(dir / "file.txt", nullptr, visit);
    EXPECT_FALSE(onFile.ok);
    EXPECT_NE(onFile.error.find("file.txt"), std::string::npos) << onFile.error;
    const exportfiles::WalkResult missing = exportfiles::walk(dir / "missing", nullptr, visit);
    EXPECT_FALSE(missing.ok);
}

TEST(ExportFiles, WalkVisitsFilesSkipsFoldersAndStops) {
    TestTempDir dir("export-walk");
    TestTempDir::write(dir / "a/one.txt", "1");
    TestTempDir::write(dir / "a/b/two.txt", "2");
    TestTempDir::write(dir / "skip/three.txt", "3");
    std::vector<std::string> seen;
    const exportfiles::WalkResult walked = exportfiles::walk(dir.path(),
        [](const fs::directory_entry&, const fs::path& rel) {
            return rel == "skip" ? exportfiles::DirAction::Skip : exportfiles::DirAction::Descend;
        },
        [&seen](const fs::directory_entry&, const fs::path& rel) {
            seen.push_back(rel.generic_string());
            return true;
        });
    ASSERT_TRUE(walked.ok) << walked.error;
    // a folder's files, then its folders, each in name order
    EXPECT_EQ(seen, (std::vector<std::string>{"a/one.txt", "a/b/two.txt"}));

    // a visitor that fails stops the walk
    size_t visits = 0;
    const exportfiles::WalkResult stopped = exportfiles::walk(dir.path(), nullptr, [&visits](const fs::directory_entry&, const fs::path&) {
        visits++;
        return false;
    });
    EXPECT_FALSE(stopped.ok);
    EXPECT_EQ(visits, 1u);
}

TEST(ExportFiles, CopyFileFailsOverADirectory) {
    TestTempDir dir("export-copy");
    TestTempDir::write(dir / "src/logo.png", "png");
    fs::create_directories(dir / "dst/ui/logo.png");
    std::string error;
    EXPECT_FALSE(exportfiles::copyFile(dir / "src/logo.png", dir / "dst/ui/logo.png", error));
    EXPECT_NE(error.find("logo.png"), std::string::npos) << error;

    error.clear();
    EXPECT_TRUE(exportfiles::copyFile(dir / "src/logo.png", dir / "dst/new/deep/logo.png", error)) << error;
    EXPECT_EQ(TestTempDir::read(dir / "dst/new/deep/logo.png"), "png");

    EXPECT_FALSE(exportfiles::writeFile(dir / "dst/ui/logo.png", "text", error));
}

TEST(ExportFiles, MirrorNeverCopiesIdeOutput) {
    TestTempDir dir("export-mirror");
    TestTempDir::write(dir / "sdk/workspaces/android/build.gradle", "gradle");
    TestTempDir::write(dir / "sdk/workspaces/android/.idea/workspace.xml", "ide");
    TestTempDir::write(dir / "sdk/workspaces/android/app/app.iml", "ide");
    TestTempDir::write(dir / "sdk/workspaces/xcode/x.xcodeproj/xcuserdata/me.xcuserdatad/state", "ide");
    std::string error;
    ASSERT_TRUE(exportfiles::mirrorTree(dir / "sdk/workspaces", dir / "out/workspaces", true, error)) << error;
    EXPECT_TRUE(fs::exists(dir / "out/workspaces/android/build.gradle"));
    EXPECT_FALSE(fs::exists(dir / "out/workspaces/android/.idea"));
    EXPECT_FALSE(fs::exists(dir / "out/workspaces/android/app/app.iml"));
    EXPECT_FALSE(fs::exists(dir / "out/workspaces/xcode/x.xcodeproj/xcuserdata"));
}

TEST(ExportFiles, MirrorKeepsIdeOutputInTheDestination) {
    TestTempDir dir("export-mirror");
    TestTempDir::write(dir / "sdk/android/build.gradle", "gradle");
    TestTempDir::write(dir / "sdk/android/app/build.gradle", "app");
    TestTempDir::write(dir / "sdk/android/gone.txt", "old");
    std::string error;
    ASSERT_TRUE(exportfiles::mirrorTree(dir / "sdk", dir / "out", true, error)) << error;
    // the user opened the exported workspace in Android Studio
    TestTempDir::write(dir / "out/android/local.properties", "sdk.dir=/opt/android");
    TestTempDir::write(dir / "out/android/.gradle/cache", "cache");
    TestTempDir::write(dir / "out/android/app/build/out.apk", "apk");
    fs::remove(dir / "sdk/android/gone.txt");

    ASSERT_TRUE(exportfiles::mirrorTree(dir / "sdk", dir / "out", true, error)) << error;
    EXPECT_EQ(TestTempDir::read(dir / "out/android/local.properties"), "sdk.dir=/opt/android");
    EXPECT_TRUE(fs::exists(dir / "out/android/.gradle/cache"));
    EXPECT_TRUE(fs::exists(dir / "out/android/app/build/out.apk"));
    // what the SDK no longer has still goes
    EXPECT_FALSE(fs::exists(dir / "out/android/gone.txt"));

    // without the IDE rule (the engine's sources) the mirror is exact
    ASSERT_TRUE(exportfiles::mirrorTree(dir / "sdk", dir / "out", false, error)) << error;
    EXPECT_FALSE(fs::exists(dir / "out/android/local.properties"));
}

TEST(ExportFiles, MirrorKeepsTheTimeOfUnchangedFiles) {
    TestTempDir dir("export-mirror");
    TestTempDir::write(dir / "sdk/core/a.cpp", "int a;");
    std::string error;
    ASSERT_TRUE(exportfiles::mirrorTree(dir / "sdk", dir / "out", false, error)) << error;
    const fs::file_time_type old = fs::last_write_time(dir / "out/core/a.cpp") - std::chrono::hours(1);
    fs::last_write_time(dir / "out/core/a.cpp", old);
    ASSERT_TRUE(exportfiles::mirrorTree(dir / "sdk", dir / "out", false, error)) << error;
    EXPECT_EQ(fs::last_write_time(dir / "out/core/a.cpp").time_since_epoch().count(), old.time_since_epoch().count())
        << "an incremental build recompiles nothing";
}

TEST(ExportFiles, ExportFolderMarkers) {
    TestTempDir dir("export-markers");
    fs::create_directories(dir / "plain");
    TestTempDir::write(dir / "plain/readme.txt", "x");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "plain", "MyGame"), "");

    TestTempDir::write(dir / "source/jasperengine.py", "#");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "source", "MyGame"), "jasperengine.py");
    TestTempDir::write(dir / "player/content-manifest.json", "{}");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "player", "MyGame"), "content-manifest.json");
    TestTempDir::write(dir / "web/MyGame.wasm", "");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "web", "MyGame"), "MyGame.wasm");
    TestTempDir::write(dir / "desktop/MyGame", "ELF");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "desktop", "MyGame"), "MyGame");
    TestTempDir::write(dir / "windows/MyGame.exe", "MZ");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "windows", "MyGame"), "MyGame.exe");
    fs::create_directories(dir / "mac/MyGame.app/Contents");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "mac", "MyGame"), "MyGame.app");
    // a folder named like the game is not an executable
    fs::create_directories(dir / "assets/MyGame");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "assets", "MyGame"), "");
}

TEST(ExportFiles, IdeOutputNames) {
    for (const char* name : {".gradle", ".idea", ".cxx", ".externalNativeBuild", ".kotlin", "build", "captures",
                             "local.properties", "xcuserdata", "DerivedData", ".DS_Store", "app.iml"}) {
        EXPECT_TRUE(exportfiles::isIdeOutput(fs::path("w") / name)) << name;
    }
    for (const char* name : {"app", "gradle", "build.gradle", "Info.plist"}) {
        EXPECT_FALSE(exportfiles::isIdeOutput(fs::path("w") / name)) << name;
    }
}

// A folder listed in part would ship without the rest: a subfolder that can't be listed (here a
// file put in its place after its parent was listed: ENOTDIR) stops the walk, naming it
TEST(ExportFiles, WalkFailsOnASubfolderItCannotList) {
    TestTempDir dir("export-walk-error");
    TestTempDir::write(dir / "root/a/one.txt", "1");
    const exportfiles::WalkResult walked = exportfiles::walk(dir / "root",
        [&dir](const fs::directory_entry&, const fs::path& rel) {
            if (rel == "a") {
                fs::remove_all(dir / "root/a");
                TestTempDir::write(dir / "root/a", "now a file");
            }
            return exportfiles::DirAction::Descend;
        },
        [](const fs::directory_entry&, const fs::path&) { return true; });
    EXPECT_FALSE(walked.ok);
    EXPECT_NE(walked.error.find("root/a"), std::string::npos) << walked.error;
    EXPECT_TRUE(walked.unreadable.empty());
}

TEST(ExportFiles, AndroidPlayerFolderIsAnExport) {
    TestTempDir dir("export-markers-android");
    TestTempDir::write(dir / "bundle/MyGame.aab", "PK");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "bundle", "MyGame"), "MyGame.aab");
    TestTempDir::write(dir / "apks/MyGame-arm64-v8a.apk", "PK");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "apks", "MyGame"), "MyGame-arm64-v8a.apk");
    TestTempDir::write(dir / "universal/MyGame.apk", "PK");
    EXPECT_EQ(exportfiles::exportFolderMarker(dir / "universal", "MyGame"), "MyGame.apk");
}

// The cooks list the project's textures and models from the asset database, not by walking: a
// file inside an earlier export is found by the folders above it
TEST(ExportFiles, ExportFolderFinderFindsFilesOfEarlierExports) {
    TestTempDir dir("export-finder");
    TestTempDir::write(dir / "Builds/Web/MyGame.wasm", "");
    TestTempDir::write(dir / "Builds/Web/assets/tree.png", "png");
    TestTempDir::write(dir / "art/tree.png", "png");
    exportfiles::ExportFolderFinder finder(dir.path(), "MyGame");
    EXPECT_TRUE(finder.contains(dir / "Builds/Web/assets/tree.png"));
    EXPECT_TRUE(finder.contains(fs::path("Builds/Web/assets/tree.png")));
    EXPECT_FALSE(finder.contains(dir / "art/tree.png"));
    EXPECT_FALSE(finder.contains(dir.path().parent_path() / "elsewhere.png"));
}
