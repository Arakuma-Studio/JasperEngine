// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CommandRunner::capture, the argv probe the kit detection uses: the program runs directly (no
// shell), so a tool path holding spaces and shell metacharacters is one literal argument and
// nothing in it is executed. POSIX here.

#include <gtest/gtest.h>

#include "Generator.h"
#include "util/CommandRunner.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

TEST(CommandRunner, CaptureRunsAToolWhosePathHoldsSpacesAndMetacharacters) {
    const fs::path base = fs::temp_directory_path() / ("jasper-capture-" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path dir = base / "my tools $(touch pwned) ; `touch pwned2` & 'q'";
    fs::create_directories(dir);
    const fs::path tool = dir / "g++ (fake)";
    std::ofstream(tool, std::ios::binary) << "#!/bin/sh\nprintf 'arg:%s\\n' \"$1\"\nexit 0\n";
    fs::permissions(tool, fs::perms::owner_all, fs::perm_options::replace);

    EXPECT_EQ(CommandRunner::capture({tool.string(), "-dumpmachine; touch pwned3"}), "arg:-dumpmachine; touch pwned3");
    EXPECT_FALSE(fs::exists(base / "pwned"));
    EXPECT_FALSE(fs::exists("pwned"));
    EXPECT_FALSE(fs::exists("pwned2"));
    EXPECT_FALSE(fs::exists("pwned3"));

    // a failing tool yields "" (what the kit detection treats as "not usable"), so does a missing one
    std::ofstream(tool, std::ios::binary | std::ios::trunc) << "#!/bin/sh\necho broken\nexit 3\n";
    EXPECT_EQ(CommandRunner::capture({tool.string(), "--version"}), "");
    EXPECT_EQ(CommandRunner::capture({(dir / "missing").string()}), "");

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(CommandRunner, KitDetectionFindsTheCompilersOnPath) {
    // this host builds the editor, so a C++ compiler is on PATH: the argv probes still find it
    const std::string gxx = CommandRunner::capture({"which", "g++"});
    const std::string clangxx = CommandRunner::capture({"which", "clang++"});
    if (gxx.empty() && clangxx.empty()) GTEST_SKIP() << "no g++ or clang++ on PATH";
    const auto kits = Generator::detectAvailableKits();
    ASSERT_FALSE(kits.empty());
    bool found = false;
    for (const auto& kit : kits) {
        if (kit.cxxCompiler == gxx || kit.cxxCompiler == clangxx) found = true;
        EXPECT_FALSE(kit.displayName.empty());
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(Generator::checkBuildTools(false, nullptr).find("C++ compiler"), std::string::npos)
        << Generator::checkBuildTools(false, nullptr);
}

TEST(CommandRunner, CaptureTimesOutAndTheCMakeVersionProbeTakesAnArgv) {
    const fs::path base = fs::temp_directory_path() / ("jasper-capture-cmake-" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path dir = base / "CMake $(touch pwned) & co";
    fs::create_directories(dir);

    // a "cmake" that answers --version (only when it is the one literal argument)
    const fs::path cmake = dir / "cmake";
    std::ofstream(cmake, std::ios::binary) << "#!/bin/sh\n[ \"$#\" = 1 ] && [ \"$1\" = --version ] || exit 2\n"
                                              "echo 'cmake version 3.99.1'\necho\necho 'CMake suite maintained'\n";
    fs::permissions(cmake, fs::perms::owner_all, fs::perm_options::replace);
    EXPECT_EQ(Generator::probeCMakeVersion(cmake.string()), "3.99.1");
    EXPECT_FALSE(fs::exists(base / "pwned"));
    EXPECT_FALSE(fs::exists("pwned"));

    // a program that never answers is stopped after the timeout, and yields ""
    const fs::path hang = dir / "hang";
    std::ofstream(hang, std::ios::binary) << "#!/bin/sh\necho 'cmake version 1.0'\nexec sleep 30\n";
    fs::permissions(hang, fs::perms::owner_all, fs::perm_options::replace);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(CommandRunner::capture({hang.string(), "--version"}, 300), "");
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));

    std::error_code ec;
    fs::remove_all(base, ec);
}

namespace {

    // `text` (ASCII / UTF-8 BMP + one astral char via \U) as UTF-16LE bytes, the way cmd.exe /u writes
    std::string utf16le(const std::u16string& text, bool bom) {
        std::string out;
        if (bom) out += "\xFF\xFE";
        for (char16_t c : text) {
            out.push_back(static_cast<char>(c & 0xFF));
            out.push_back(static_cast<char>((c >> 8) & 0xFF));
        }
        return out;
    }

    const std::string* lookup(const CommandRunner::Environment& env, const std::string& name) {
        for (const auto& entry : env) {
            if (entry.first == name) return &entry.second;
        }
        return nullptr;
    }

}

TEST(CommandRunner, ParsesTheEnvironmentSetPrintsInUtf16) {
    const std::u16string text =
        u"=C:=C:\\Users\\dev\r\n"                                  // drive entry: skipped
        u"=ExitCode=00000000\r\n"
        u"ALLUSERSPROFILE=C:\\ProgramData\r\n"
        u"Path=C:\\Program Files\\Microsoft Visual Studio\\VC\\bin;C:\\Windows\\system32;;\"C:\\quoted;dir\"\r\n"
        u"INCLUDE=C:\\SDK\\include;C:\\VC\\include\r\n"
        u"\r\n"
        u"**********************************************************************\r\n"   // banner: no '='
        u"  indented=not a variable\r\n"
        u"VSCMD_ARG_TGT_ARCH=x64\r\n"
        u"WEIRD=a=b=c\r\n"
        u"UNICODE_DIR=C:\\Users\\J\u00f6rg\\\u65e5\u672c \U0001F600\r\n"
        u"EMPTY=\r\n"
        u"PATH=C:\\later\\wins\n"                                  // LF only, same name in other case
        u"LAST=no newline at the end";
    for (bool bom : {true, false}) {
        const auto env = CommandRunner::parseEnvironmentOutput(utf16le(text, bom));
        for (const auto& entry : env) {
            EXPECT_FALSE(entry.first.empty());
            EXPECT_NE(entry.first[0], '=');
            EXPECT_NE(entry.first[0], ' ');
        }
        // one PATH, the last value, under the first spelling, with its semicolons kept
        ASSERT_NE(lookup(env, "Path"), nullptr);
        EXPECT_EQ(*lookup(env, "Path"), "C:\\later\\wins");
        EXPECT_EQ(lookup(env, "PATH"), nullptr);
        ASSERT_NE(lookup(env, "INCLUDE"), nullptr);
        EXPECT_EQ(*lookup(env, "INCLUDE"), "C:\\SDK\\include;C:\\VC\\include");
        ASSERT_NE(lookup(env, "WEIRD"), nullptr);
        EXPECT_EQ(*lookup(env, "WEIRD"), "a=b=c");
        ASSERT_NE(lookup(env, "UNICODE_DIR"), nullptr);
        EXPECT_EQ(*lookup(env, "UNICODE_DIR"), "C:\\Users\\J\xC3\xB6rg\\\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80");
        ASSERT_NE(lookup(env, "EMPTY"), nullptr);
        EXPECT_EQ(*lookup(env, "EMPTY"), "");
        ASSERT_NE(lookup(env, "LAST"), nullptr);
        EXPECT_EQ(*lookup(env, "LAST"), "no newline at the end");
        EXPECT_EQ(lookup(env, "  indented"), nullptr);
        EXPECT_EQ(env.size(), 8u);   // ALLUSERSPROFILE EMPTY INCLUDE LAST Path UNICODE_DIR VSCMD_ARG_TGT_ARCH WEIRD
    }
}

TEST(CommandRunner, ParsesOddEnvironmentOutput) {
    // 8-bit text (no UTF-16), CRLF, sorted case-insensitively for the CreateProcess block
    const auto env = CommandRunner::parseEnvironmentOutput("zeta=1\r\nAlpha=2\r\n=D:=D:\\\r\nbeta=3\r\nnot a line\r\n");
    ASSERT_EQ(env.size(), 3u);
    EXPECT_EQ(env[0].first, "Alpha");
    EXPECT_EQ(env[1].first, "beta");
    EXPECT_EQ(env[2].first, "zeta");

    // an odd byte count (a truncated read) drops only the last half code unit; a lone surrogate is U+FFFD
    std::string bytes = utf16le(u"A=1\r\nB=x", false);
    bytes += "\x3D\xD8";            // high surrogate with nothing after it
    bytes += "\x41";                // and a stray last byte
    const auto odd = CommandRunner::parseEnvironmentOutput(bytes);
    ASSERT_EQ(odd.size(), 2u);
    EXPECT_EQ(odd[1].second, "x\xEF\xBF\xBD");

    // empty input, and a UTF-8 BOM
    EXPECT_TRUE(CommandRunner::parseEnvironmentOutput("").empty());
    const auto bom = CommandRunner::parseEnvironmentOutput("\xEF\xBB\xBFONE=1\n");
    ASSERT_EQ(bom.size(), 1u);
    EXPECT_EQ(bom[0].first, "ONE");

    // off Windows nothing is loaded: no environment, no error
    CommandRunner::Environment msvc;
    std::string error;
    EXPECT_TRUE(CommandRunner::msvcEnvironment("Ninja", msvc, error));
#ifndef _WIN32
    EXPECT_TRUE(msvc.empty());
#endif
    EXPECT_TRUE(error.empty());
}

TEST(CommandRunner, RunPassesTheGivenEnvironmentOnly) {
    // the child sees exactly the given environment (what the MSVC one is on Windows)
    CommandRunner runner;
    const CommandRunner::Environment env = {{"JASPER_A", "one;two=three"}, {"PATH", "/nonexistent-dir"}};
    std::vector<std::string> lines;
    ASSERT_TRUE(runner.run({"/usr/bin/env"}, fs::path(), [&lines](const std::string& line) { lines.push_back(line); }, &env));
    std::sort(lines.begin(), lines.end());
    EXPECT_EQ(lines, (std::vector<std::string>{"JASPER_A=one;two=three", "PATH=/nonexistent-dir"}));
    // without one, the editor's environment
    lines.clear();
    ASSERT_TRUE(runner.run({"/usr/bin/env"}, fs::path(), [&lines](const std::string& line) { lines.push_back(line); }));
    EXPECT_GT(lines.size(), 2u);
}

TEST(CommandRunner, WindowsProgramResolutionNeverUsesTheCurrentFolderOrBatchFiles) {
    const fs::path base = fs::temp_directory_path() / ("jasper-resolve-" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path a = base / "a", b = base / "b", cwd = base / "cwd";
    for (const fs::path& dir : {a, b, cwd}) fs::create_directories(dir);
    auto touch = [](const fs::path& path) { std::ofstream(path, std::ios::binary) << "x"; };
    touch(b / "tool.exe");
    touch(a / "emcmake.bat");
    touch(a / "script.cmd");
    touch(b / "other.com");
    touch(cwd / "planted.exe");
    const fs::path previous = fs::current_path();
    fs::current_path(cwd);

    fs::path resolved;
    // a bare name: the search folders in order, .exe then .com added
    EXPECT_EQ(CommandRunner::resolveWindowsProgram("tool", {a, b}, resolved), "");
    EXPECT_EQ(resolved, b / "tool.exe");
    EXPECT_EQ(CommandRunner::resolveWindowsProgram("other", {a, b}, resolved), "");
    EXPECT_EQ(resolved, b / "other.com");
    // never the current folder
    EXPECT_NE(CommandRunner::resolveWindowsProgram("planted", {a, b}, resolved), "");
    EXPECT_NE(CommandRunner::resolveWindowsProgram("planted.exe", {a, b}, resolved), "");
    // a batch file is never picked by a bare name, and refused when named
    EXPECT_NE(CommandRunner::resolveWindowsProgram("emcmake", {a, b}, resolved).find("not found"), std::string::npos);
    EXPECT_NE(CommandRunner::resolveWindowsProgram("emcmake.bat", {a, b}, resolved).find("batch file"), std::string::npos);
    EXPECT_NE(CommandRunner::resolveWindowsProgram("SCRIPT.CMD", {a, b}, resolved).find("batch file"), std::string::npos);
    EXPECT_NE(CommandRunner::resolveWindowsProgram(a / "emcmake.bat", {}, resolved).find("batch file"), std::string::npos);
    // a dot in a bare name is no extension of a program: .exe / .com still added, never .bat / .cmd
    touch(b / "tool-1.2.exe");
    touch(a / "run-1.2.bat");
    EXPECT_EQ(CommandRunner::resolveWindowsProgram("tool-1.2", {a, b}, resolved), "");
    EXPECT_EQ(resolved, b / "tool-1.2.exe");
    EXPECT_NE(CommandRunner::resolveWindowsProgram("run-1.2", {a, b}, resolved), "");
    touch(b / "tool-1.2");
    EXPECT_EQ(CommandRunner::resolveWindowsProgram(b / "tool-1.2", {}, resolved), "");
    EXPECT_EQ(resolved, b / "tool-1.2.exe");   // the .exe first, even with a file spelled so beside it
    touch(b / "only-1.2");
    EXPECT_EQ(CommandRunner::resolveWindowsProgram(b / "only-1.2", {}, resolved), "");
    EXPECT_EQ(resolved, b / "only-1.2");       // then as spelled
    // a path is used as given (extension added when missing)
    EXPECT_EQ(CommandRunner::resolveWindowsProgram(b / "tool.exe", {}, resolved), "");
    EXPECT_EQ(resolved, b / "tool.exe");
    EXPECT_EQ(CommandRunner::resolveWindowsProgram(b / "tool", {}, resolved), "");
    EXPECT_EQ(resolved, b / "tool.exe");
    EXPECT_NE(CommandRunner::resolveWindowsProgram(b / "missing", {}, resolved), "");
    EXPECT_TRUE(resolved.empty());

    fs::current_path(previous);
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(CommandRunner, LaunchDetachedReportsExecFailureLeavesNoChildAndInheritsNoDescriptor) {
    const fs::path base = fs::temp_directory_path() / ("jasper-detached-" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path dir = base / "player dir $(x) & co";
    fs::create_directories(dir);
    // a descriptor the editor holds open WITHOUT close-on-exec
    const fs::path probe = base / "inherited-probe";
    const int held = ::open(probe.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(held, 0);

    const fs::path out = base / "fds.txt";
    const fs::path player = dir / "player";
#if defined(__APPLE__)
    // no /proc, and /dev/fd's entries name no file: the descriptors looked for by their numbers
    std::ofstream(player, std::ios::binary) << "#!/bin/sh\npwd > '" << (base / "pwd.txt").string() << "'\n"
                                            << "for n in 0 1 2 " << held << "; do [ -e /dev/fd/$n ] && echo \"fd $n\"; done > '" << out.string() << "'\n";
#else
    std::ofstream(player, std::ios::binary) << "#!/bin/sh\npwd > '" << (base / "pwd.txt").string() << "'\n"
                                            << "for f in /proc/$$/fd/*; do readlink \"$f\"; done > '" << out.string() << "'\n";
#endif
    fs::permissions(player, fs::perms::owner_all, fs::perm_options::replace);

    std::string error;
    ASSERT_TRUE(CommandRunner::launchDetached(player, dir, error)) << error;
    // the intermediate child is reaped already: nothing to collect
    int status = 0;
    EXPECT_LE(::waitpid(-1, &status, WNOHANG), 0);
    for (int i = 0; i < 100 && !fs::exists(out); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::ifstream fds(out);
    const std::string listed((std::istreambuf_iterator<char>(fds)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(listed.empty());
#if defined(__APPLE__)
    EXPECT_EQ(listed.find("fd " + std::to_string(held) + "\n"), std::string::npos) << listed;
#else
    EXPECT_EQ(listed.find("inherited-probe"), std::string::npos) << listed;
#endif
    std::ifstream pwd(base / "pwd.txt");
    std::string where;
    std::getline(pwd, where);
    EXPECT_EQ(fs::path(where), fs::canonical(dir));

    // exec failures are reported, not hidden behind "Running"
    EXPECT_FALSE(CommandRunner::launchDetached(dir / "missing", dir, error));
    EXPECT_FALSE(error.empty());
    const fs::path notExecutable = dir / "data";
    std::ofstream(notExecutable, std::ios::binary) << "data";
    fs::permissions(notExecutable, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    EXPECT_FALSE(CommandRunner::launchDetached(notExecutable, dir, error));
    EXPECT_NE(error.find("ermission"), std::string::npos) << error;

    ::close(held);
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(CommandRunner, CloexecPipesAreNotInheritedBySpawnedPrograms) {
    int fds[2];
    ASSERT_TRUE(CommandRunner::makeCloexecPipe(fds));
    EXPECT_TRUE(::fcntl(fds[0], F_GETFD) & FD_CLOEXEC);
    EXPECT_TRUE(::fcntl(fds[1], F_GETFD) & FD_CLOEXEC);
    // a program run meanwhile doesn't hold the write end: closing ours gives end-of-file at once
    CommandRunner runner;
    std::vector<std::string> lines;
#if defined(__APPLE__)
    // no /proc: the pipe's two descriptors looked for by their numbers
    const std::string listing = "for n in " + std::to_string(fds[0]) + " " + std::to_string(fds[1]) + "; do [ -e /dev/fd/$n ] && echo $n; done; true";
#else
    const std::string listing = "ls /proc/$$/fd";
#endif
    ASSERT_TRUE(runner.run({"/bin/sh", "-c", listing}, fs::path(), [&lines](const std::string& line) { lines.push_back(line); }));
    for (const std::string& line : lines) {
        EXPECT_NE(line, std::to_string(fds[0]));
        EXPECT_NE(line, std::to_string(fds[1]));
    }
    ::close(fds[1]);
    char byte = 0;
    EXPECT_EQ(::read(fds[0], &byte, 1), 0);
    ::close(fds[0]);
}

// A tool's output reaches the Build window, the log and MCP as UTF-8: a line that is not (a
// tool writing another code page) is repaired, one U+FFFD per bad byte; UTF-8 passes untouched
TEST(CommandRunner, InvalidUtf8OutputIsRepaired) {
    EXPECT_EQ(CommandRunner::capture({"/usr/bin/printf", "a\\377b\\n"}), "a\xEF\xBF\xBD" "b");
    EXPECT_EQ(CommandRunner::capture({"/usr/bin/printf", "caf\xC3\xA9 \xF0\x9F\x98\x80\\n"}), "caf\xC3\xA9 \xF0\x9F\x98\x80");
    std::vector<std::string> lines;
    CommandRunner runner;
    ASSERT_TRUE(runner.run({"/usr/bin/printf", "ok\\n\\351t\\351\\n"}, fs::path(), [&lines](const std::string& line) {
        lines.push_back(line);
    }));
    EXPECT_EQ(lines, (std::vector<std::string>{"ok", "\xEF\xBF\xBDt\xEF\xBF\xBD"}));
}
