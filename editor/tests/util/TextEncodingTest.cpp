// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Text and paths in every language (docs/core-utilities.md, "Text encoding"): the Windows code
// page check, child output, vswhere's arguments, and project walks over names outside any ANSI
// code page. The walks pass on Linux and macOS either way; on Windows they throw without the
// UTF-8 code page of the manifest (the test binaries get it too).

#include "Project.h"
#include "command/CommandHandle.h"
#include "texture/Material.h"
#include "util/CommandRunner.h"
#include "util/FileUtils.h"
#include "util/TextEncoding.h"
#include "util/Util.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

TEST(TextEncoding, CodePageProblem) {
    EXPECT_FALSE(TextEncoding::codePageProblem(65001).has_value());
    const auto problem = TextEncoding::codePageProblem(1252);
    ASSERT_TRUE(problem.has_value());
    EXPECT_NE(problem->find("Windows 10 version 1903"), std::string::npos) << *problem;
    EXPECT_NE(problem->find("1252"), std::string::npos) << *problem;
}

TEST(TextEncoding, ConsoleToUtf8KeepsValidAndRepairsInvalid) {
    EXPECT_EQ(TextEncoding::consoleToUtf8("C:\\Users\\Jo\xC3\xA3o\\Player.cpp(12): error C2065"),
              "C:\\Users\\Jo\xC3\xA3o\\Player.cpp(12): error C2065");
    EXPECT_EQ(TextEncoding::consoleToUtf8(""), "");
#ifndef _WIN32
    // a Latin-1 byte: on Windows it is read in the console code page instead
    EXPECT_EQ(TextEncoding::consoleToUtf8("Jo\xE3o"), "Jo\xEF\xBF\xBDo");
#endif
}

TEST(TextEncoding, VswhereArgumentsEndWithUtf8) {
    EXPECT_EQ(CommandRunner::vswhereArguments("vswhere.exe", {"-latest", "-property", "productPath"}),
              (std::vector<std::string>{"vswhere.exe", "-latest", "-property", "productPath", "-utf8"}));
}

#ifdef _WIN32
TEST(TextEncoding, ProcessRunsInTheUtf8CodePage) {
    EXPECT_EQ(GetACP(), static_cast<UINT>(CP_UTF8)) << "the test binary has no UTF-8 manifest (jasperengine_add_windows_manifest)";
}
#endif

// FileUtils' path conversions never throw, so a name that is not UTF-8 is reported where it is
// checked: POSIX passes its bytes through; Windows (UTF-16 names, whose standard conversions
// throw) carries a stray byte as U+DC80 + byte and a lone surrogate in its three-byte form
TEST(TextEncoding, PathConversionsKeepNamesThatAreNotUtf8) {
    for (const std::string& text : {std::string("assets/caf\xC3\xA9 \xF0\x9F\x99\x82.png"), std::string("assets/caf\xE9.png"),
                                   std::string("\xC0\xAF\xE2\x82"), std::string("a\xED\xA0\x80z")}) {
        std::string back;
        EXPECT_NO_THROW(back = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(text))) << text;
        EXPECT_EQ(back, text);
    }
    EXPECT_EQ(FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8("assets/caf\xE9.png")), "assets/caf\xE9.png");
#ifdef _WIN32
    EXPECT_EQ(FileUtils::pathFromUtf8("caf\xE9").native(), std::wstring(L"caf\xDCE9"));
    EXPECT_EQ(FileUtils::pathFromUtf8("\xC3\xA9").native(), std::wstring(L"\x00E9"));
    // a lone surrogate an NTFS name can hold, outside the escapes' range: three bytes, and back
    const fs::path lone(std::wstring(L"x\xD800y"));
    std::string text;
    EXPECT_NO_THROW(text = FileUtils::pathToUtf8(lone));
    EXPECT_EQ(text, "x\xED\xA0\x80y");
    EXPECT_EQ(FileUtils::pathFromUtf8(text), lone);
    // such a file is found and named by a walk
    TestTempDir dir("lone-surrogate");
    std::ofstream(dir.path() / lone) << "x";
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir.path())) names.push_back(FileUtils::pathToUtf8(entry.path().filename()));
    EXPECT_EQ(names, std::vector<std::string>{"x\xED\xA0\x80y"});
#endif
}

// The asset type checks read a name's extension from its text: a name that is not UTF-8 (a stray
// byte, a lone surrogate) is typed as any other, never thrown (on Windows its fs::path threw)
TEST(TextEncoding, AssetTypeChecksTakeNamesThatAreNotUtf8) {
    EXPECT_NO_THROW(EXPECT_TRUE(Util::isImageFile("assets/caf\xE9.PNG")));
    EXPECT_NO_THROW(EXPECT_TRUE(Util::isImageFile("assets/\xC0\xAF\xE2\x82/x.png")));
    EXPECT_NO_THROW(EXPECT_TRUE(Util::isSceneFile("a\xED\xA0\x80z.Scene")));
    EXPECT_NO_THROW(EXPECT_FALSE(Util::isImageFile("caf\xE9.png.txt")));
    // and fs::path::extension's rule: a dot file has none, a bare extension is itself
    EXPECT_FALSE(Util::isImageFile("assets/.png"));
    EXPECT_FALSE(Util::isImageFile("assets.png/image"));
    EXPECT_TRUE(Util::isImageFile(".png"));
    EXPECT_TRUE(Util::isImageFile("C:\\art\\a.b\\x.PNG"));
}

// A Chinese and emoji name in a project folder named in Portuguese and Japanese: the asset
// database's scan, a material that references it and the assets root change all walk it
TEST(TextEncoding, ProjectWalksTakeNamesInAnyLanguage) {
    TestTempDir dir("unicode-names");
    const fs::path root = dir.path() / FileUtils::pathFromUtf8("Jogos \xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0");
    Project project;
    project.setSkipProjectLock(true);
    std::string error;
    ASSERT_TRUE(project.createProjectAt(root, "Game", &error)) << error;
    const fs::path folder = project.getAssetsPath() / FileUtils::pathFromUtf8("\xE5\x85\xB3\xE5\x8D\xA1");   // 关卡
    fs::create_directories(folder);
    const std::string imageName = "\xE7\xAC\xAC\xE4\xB8\x80\xE5\x85\xB3\xF0\x9F\x98\x80.png";             // 第一关😀.png
    std::ofstream(folder / FileUtils::pathFromUtf8(imageName), std::ios::binary) << "not decoded here";

    Material material;
    material.baseColorTexture = Texture("\xE5\x85\xB3\xE5\x8D\xA1/" + imageName);
    ASSERT_EQ(project.writeMaterialFile(FileUtils::pathFromUtf8("\xE7\x9F\xB3.material"), material), SaveResult::Saved);

    // the change walks the whole project (the material files among others) before it moves anything
    ASSERT_NO_THROW(project.changeAssetRoots("art", project.getLuaDir()));
    EXPECT_EQ(project.getAssetsDir(), fs::path("art"));

    for (SceneProject& scene : project.getScenes()) CommandHandle::remove(scene.id);
    project.releaseScenes();
}
