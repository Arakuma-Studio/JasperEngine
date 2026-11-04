// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Finding Aseprite (docs/specs/S44-aseprite-import.md R32, §6.8, AC16): fake install trees per OS
// layout in a temp directory, read through an injected environment. No process is started.

#include "pixelart/aseprite/AsepriteLocator.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define ASEPRITE_TEST_GETPID _getpid
#else
#include <unistd.h>
#define ASEPRITE_TEST_GETPID getpid
#endif

namespace fs = std::filesystem;
using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::aseprite;

namespace {

    class AsepriteLocatorTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            root = fs::temp_directory_path() / ("jasper-aseprite-locator-" + std::to_string(ASEPRITE_TEST_GETPID()) + "-" + info->name());
            std::error_code ec;
            fs::remove_all(root, ec);
            fs::create_directories(root);
        }

        void TearDown() override {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        // A file under the temp root, its directories made
        fs::path touch(const fs::path& relative, const std::string& content = "#!/bin/sh\n"){
            const fs::path path = root / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << content;
            return path;
        }

        // Files under the temp root count when they exist; `fixed` adds absolute locations outside
        // it (/Applications, /opt) that a test cannot create
        LocatorEnvironment environment(const std::string& os){
            LocatorEnvironment env;
            env.os = os;
            env.home = root / "home";
            env.env = [this](const std::string& name) {
                auto it = vars.find(name);
                return it == vars.end() ? std::string() : it->second;
            };
            env.isExecutable = [this](const fs::path& path) {
                if (fixed.count(path.lexically_normal().generic_string())) return true;
                if (notExecutable.count(path.lexically_normal().generic_string())) return false;
                std::error_code ec;
                return fs::is_regular_file(path, ec);
            };
            env.readFile = [](const fs::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            };
            return env;
        }

        // A path inside a .vdf string: Steam escapes its backslashes
        static std::string vdfString(const fs::path& path){
            std::string out;
            for (char c : path.string()) {
                if (c == '\\' || c == '"') out += '\\';
                out += c;
            }
            return out;
        }

        static std::vector<std::string> sources(const std::vector<AsepriteInstall>& installs){
            std::vector<std::string> out;
            for (const AsepriteInstall& install : installs) out.push_back(install.source);
            return out;
        }

        fs::path root;
        std::map<std::string, std::string> vars;
        std::set<std::string> fixed;
        std::set<std::string> notExecutable;
    };

}

// The Linux and macOS layouts name POSIX absolute paths (/opt, /Applications) and split PATH on
// ':', which a Windows host's drive letters break; the OS-independent rules run on every host.
#ifdef _WIN32
#define SKIP_ON_WINDOWS_HOST() GTEST_SKIP() << "POSIX install locations do not exist on a Windows host"
#else
#define SKIP_ON_WINDOWS_HOST() (void)0
#endif

TEST_F(AsepriteLocatorTest, LinuxLocationsInOrder) {
    SKIP_ON_WINDOWS_HOST();
    const fs::path onPath = touch("bin/aseprite");
    const fs::path steam = touch("home/.steam/steam/steamapps/common/Aseprite/aseprite");
    const fs::path localSteam = touch("home/.local/share/Steam/steamapps/common/Aseprite/aseprite");
    fixed.insert("/opt/aseprite/aseprite");
    vars["PATH"] = (root / "bin").string() + ":" + (root / "empty").string();

    const std::vector<AsepriteInstall> found = asepriteCandidates(environment("linux"));
    ASSERT_EQ(found.size(), 4u);
    EXPECT_EQ(sources(found), (std::vector<std::string>{"path", "steam", "steam", "opt"}));
    EXPECT_EQ(found[0].executable, onPath);
    EXPECT_EQ(found[1].executable, steam);
    EXPECT_EQ(found[2].executable, localSteam);
    EXPECT_EQ(found[3].executable, fs::path("/opt/aseprite/aseprite"));
    for (const AsepriteInstall& install : found) EXPECT_TRUE(install.version.empty());   // no --version in the pure search
}

TEST_F(AsepriteLocatorTest, PreferenceBeatsEnvironmentBeatsOsLocations) {
    const fs::path preference = touch("pref/aseprite");
    const fs::path variable = touch("var/aseprite");
    touch("bin/aseprite.exe");
    vars["PATH"] = (root / "bin").string();
    vars["JASPER_ASEPRITE"] = variable.string();
    LocatorEnvironment env = environment("windows");
    env.preferencePath = preference.string();

    EXPECT_EQ(sources(asepriteCandidates(env)), (std::vector<std::string>{"preference", "JASPER_ASEPRITE", "path"}));
    PixelProblem problem;
    const std::optional<AsepriteInstall> first = findAseprite(env, &problem);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->executable, preference);
    EXPECT_EQ(first->source, "preference");
    EXPECT_TRUE(problem.empty());

    env.preferencePath.clear();
    ASSERT_TRUE(findAseprite(env, nullptr).has_value());
    EXPECT_EQ(findAseprite(env, nullptr)->source, "JASPER_ASEPRITE");
}

TEST_F(AsepriteLocatorTest, NonExecutablePreferenceIsSkipped) {
    const fs::path plain = touch("pref/aseprite.txt", "not a program");
    notExecutable.insert(plain.lexically_normal().generic_string());
    const fs::path variable = touch("var/aseprite");
    vars["JASPER_ASEPRITE"] = variable.string();

    LocatorEnvironment env = environment("linux");
    env.preferencePath = plain.string();
    ASSERT_TRUE(findAseprite(env, nullptr).has_value());
    EXPECT_EQ(findAseprite(env, nullptr)->source, "JASPER_ASEPRITE");

    // a directory, a missing file and a relative path are not executables either
    for (const std::string& path : {(root / "pref").string(), (root / "missing").string(), std::string("pref/aseprite.txt")}) {
        env.preferencePath = path;
        EXPECT_EQ(sources(asepriteCandidates(env)), (std::vector<std::string>{"JASPER_ASEPRITE"})) << path;
    }
}

TEST_F(AsepriteLocatorTest, WindowsLocationsInOrderWithSteamLibraries) {
    const fs::path programFiles = touch("pf/Aseprite/Aseprite.exe");
    const fs::path programFilesX86 = touch("pf86/Aseprite/Aseprite.exe");
    const fs::path localAppData = touch("local/Programs/Aseprite/Aseprite.exe");
    const fs::path steamRoot = touch("pf86/Steam/steamapps/common/Aseprite/Aseprite.exe");
    const fs::path library = touch("library/steamapps/common/Aseprite/Aseprite.exe");
    const fs::path onPath = touch("tools/aseprite.exe");
    // library 0 (the root) is listed too: found once
    touch("pf86/Steam/steamapps/libraryfolders.vdf",
          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + (root / "pf86/Steam").string() + "\"\n\t}\n"
          "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + (root / "library").string() + "\"\n\t\t\"label\"\t\t\"\"\n\t}\n}\n");
    vars["ProgramFiles"] = (root / "pf").string();
    vars["ProgramFiles(x86)"] = (root / "pf86").string();
    vars["LOCALAPPDATA"] = (root / "local").string();
    vars["PATH"] = (root / "nothing").string() + ";" + (root / "tools").string();

    const std::vector<AsepriteInstall> found = asepriteCandidates(environment("windows"));
    ASSERT_EQ(found.size(), 6u);
    EXPECT_EQ(sources(found), (std::vector<std::string>{"program_files", "program_files_x86", "local_app_data", "steam", "steam", "path"}));
    EXPECT_EQ(found[0].executable, programFiles);
    EXPECT_EQ(found[1].executable, programFilesX86);
    EXPECT_EQ(found[2].executable, localAppData);
    EXPECT_EQ(found[3].executable, steamRoot);
    EXPECT_EQ(found[4].executable, library);
    EXPECT_EQ(found[5].executable, onPath);
}

TEST_F(AsepriteLocatorTest, SteamLibraryOnlyFromVdf) {
    const fs::path library = touch("games/steamapps/common/Aseprite/Aseprite.exe");
    touch("pf86/Steam/steamapps/libraryfolders.vdf",
          "\"libraryfolders\" { \"1\" { \"path\" \"" + (root / "games").string() + "\" } }");
    vars["ProgramFiles(x86)"] = (root / "pf86").string();

    const std::optional<AsepriteInstall> found = findAseprite(environment("windows"), nullptr);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, "steam");
    EXPECT_EQ(found->executable, library);
}

TEST_F(AsepriteLocatorTest, VdfPathsAreUnescapedInBothLayouts) {
    const std::string current =
        "// written by Steam\n\"libraryfolders\"\n{\n\t\"contentstatsid\"\t\t\"123\"\n"
        "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"431730\"\t\t\"0\"\n\t\t}\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\Steam \\\"Games\\\"\"\n\t}\n}\n";
    EXPECT_EQ(steamLibraryFolders(current),
              (std::vector<fs::path>{fs::path("C:\\Program Files (x86)\\Steam"), fs::path("D:\\Steam \"Games\"")}));

    const std::string old = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1\"\n\t\"1\"\t\t\"E:\\\\SteamLibrary\"\n}\n";
    EXPECT_EQ(steamLibraryFolders(old), (std::vector<fs::path>{fs::path("E:\\SteamLibrary")}));

    EXPECT_TRUE(steamLibraryFolders("").empty());
    EXPECT_TRUE(steamLibraryFolders("\"path\" { unterminated").empty());
}

TEST_F(AsepriteLocatorTest, MacosLocationsInOrder) {
    SKIP_ON_WINDOWS_HOST();
    fixed.insert("/Applications/Aseprite.app/Contents/MacOS/aseprite");
    const fs::path user = touch("home/Applications/Aseprite.app/Contents/MacOS/aseprite");
    const fs::path steam = touch("home/Library/Application Support/Steam/steamapps/common/Aseprite/Aseprite.app/Contents/MacOS/aseprite");
    touch("bin/aseprite");
    vars["PATH"] = (root / "bin").string();   // not a macOS location (§6.8)

    const std::vector<AsepriteInstall> found = asepriteCandidates(environment("macos"));
    ASSERT_EQ(found.size(), 3u);
    EXPECT_EQ(sources(found), (std::vector<std::string>{"applications", "user_applications", "steam"}));
    EXPECT_EQ(found[0].executable, fs::path("/Applications/Aseprite.app/Contents/MacOS/aseprite"));
    EXPECT_EQ(found[1].executable, user);
    EXPECT_EQ(found[2].executable, steam);
}

TEST_F(AsepriteLocatorTest, MacosPreferenceMayNameTheBundle) {
    const fs::path program = touch("Tools/Aseprite.app/Contents/MacOS/aseprite");
    LocatorEnvironment env = environment("macos");
    env.preferencePath = (root / "Tools/Aseprite.app").string();
    const std::optional<AsepriteInstall> found = findAseprite(env, nullptr);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, "preference");
    EXPECT_EQ(found->executable, program);
}

TEST_F(AsepriteLocatorTest, PathSkipsRelativeEntriesAndRepeats) {
    touch("bin/aseprite.exe");
    vars["PATH"] = std::string(".;bin;") + (root / "bin").string() + ";" + (root / "bin").string() + "/";
    LocatorEnvironment env = environment("windows");
    env.preferencePath = (root / "bin/aseprite.exe").string();   // the same file as PATH's: once
    EXPECT_EQ(sources(asepriteCandidates(env)), (std::vector<std::string>{"preference"}));
}

TEST_F(AsepriteLocatorTest, NoInstallIsAsepriteNotFound) {
    vars["PATH"] = (root / "bin").string();
    PixelProblem problem;
    EXPECT_FALSE(findAseprite(environment("linux"), &problem).has_value());
    EXPECT_EQ(problem.kind, "aseprite_not_found");
    EXPECT_TRUE(problem.field.empty());
    EXPECT_NE(problem.fix.find("Preferences > External Tools > Aseprite"), std::string::npos) << problem.fix;
    EXPECT_NE(problem.fix.find("https://www.aseprite.org"), std::string::npos) << problem.fix;

    for (const char* os : {"windows", "macos"}) {
        PixelProblem other;
        EXPECT_FALSE(findAseprite(environment(os), &other).has_value()) << os;
        EXPECT_EQ(other.kind, "aseprite_not_found") << os;
    }

    // a preference that does not run is named, with the key at fault
    LocatorEnvironment env = environment("linux");
    env.preferencePath = (root / "gone/aseprite").string();
    PixelProblem bad;
    EXPECT_FALSE(findAseprite(env, &bad).has_value());
    EXPECT_EQ(bad.kind, "aseprite_not_found");
    EXPECT_EQ(bad.field, "externalTools.aseprite.path");
    EXPECT_NE(bad.message.find(env.preferencePath), std::string::npos) << bad.message;
}

TEST_F(AsepriteLocatorTest, OpenRejectsRelativePathsBeforeLaunching) {
    PixelProblem problem;
    EXPECT_FALSE(openInAseprite({"aseprite", "path", ""}, {root / "hero.aseprite"}, &problem));
    EXPECT_EQ(problem.kind, "aseprite_not_found");

    problem = {};
    EXPECT_FALSE(openInAseprite({root / "bin/aseprite", "path", ""}, {fs::path("hero.aseprite")}, &problem));
    EXPECT_EQ(problem.kind, "invalid_settings");
    EXPECT_EQ(problem.field, "files");
}
