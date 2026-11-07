// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The build output lines a failed build reports (util/BuildDiagnostics.h, export-pipeline spec
// 6.10): parsed once for the Console, the Build dialog, the CLI and MCP.

#include "util/BuildDiagnostics.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(BuildDiagnostics, GccClangMsvcCmakeLinkerLines) {
    BuildDiagnostic d;
    ASSERT_TRUE(BuildDiagnostics::parse("/src/scripts/Player.cpp:12:5: error: 'jump' was not declared in this scope", d));
    EXPECT_EQ(d.file, "/src/scripts/Player.cpp");
    EXPECT_EQ(d.line, 12);
    EXPECT_EQ(d.column, 5);
    EXPECT_EQ(d.severity, "error");
    EXPECT_EQ(d.message, "'jump' was not declared in this scope");
    EXPECT_EQ(d.summary(), "/src/scripts/Player.cpp:12: 'jump' was not declared in this scope");

    ASSERT_TRUE(BuildDiagnostics::parse("C:\\game\\Player.cpp(7,3): error C2065: 'x': undeclared identifier [C:\\b\\game.vcxproj]", d));
    EXPECT_EQ(d.line, 7);
    EXPECT_EQ(d.code, "C2065");
    EXPECT_EQ(d.severity, "error");
    EXPECT_EQ(d.text.find("vcxproj"), std::string::npos) << "MSBuild's project suffix is dropped";

    ASSERT_TRUE(BuildDiagnostics::parse("CMake Error at CMakeLists.txt:40 (add_library):", d));
    EXPECT_EQ(d.file, "CMakeLists.txt");
    EXPECT_EQ(d.line, 40);
    EXPECT_EQ(d.code, "cmake");

    ASSERT_TRUE(BuildDiagnostics::parse("/usr/bin/ld: main.o: undefined reference to `foo()'", d));
    EXPECT_EQ(d.code, "link");
    EXPECT_EQ(d.severity, "error");
    ASSERT_TRUE(BuildDiagnostics::parse("game.obj : error LNK2019: unresolved external symbol foo", d));
    EXPECT_EQ(d.severity, "error");

    ASSERT_TRUE(BuildDiagnostics::parse("\x1b[1m/a/b.cpp:3:1: \x1b[31mwarning:\x1b[0m unused variable", d));
    EXPECT_EQ(d.severity, "warning");

    EXPECT_FALSE(BuildDiagnostics::parse("[12/40] Building CXX object x.o", d));
    EXPECT_FALSE(BuildDiagnostics::parse("ninja: build stopped: subcommand failed.", d));
}

TEST(BuildDiagnostics, LineLevels) {
    EXPECT_EQ(BuildDiagnostics::lineLevel("a.cpp:1:1: error: x"), LogLevel::Error);
    EXPECT_EQ(BuildDiagnostics::lineLevel("a.cpp(1): error C2065: x"), LogLevel::Error);
    EXPECT_EQ(BuildDiagnostics::lineLevel("x.vcxproj : error MSB3073: the command exited"), LogLevel::Error);
    EXPECT_EQ(BuildDiagnostics::lineLevel("b.obj : error LNK2019: unresolved"), LogLevel::Error);
    EXPECT_EQ(BuildDiagnostics::lineLevel("a.cpp:1:1: warning: x"), LogLevel::Warn);
    EXPECT_EQ(BuildDiagnostics::lineLevel("[3/9] Linking CXX executable game"), LogLevel::Info);
}

TEST(BuildDiagnostics, MsBuildRepeatsCollapse) {
    BuildErrorLog log;
    log.add("C:\\g\\Player.cpp(7,3): error C2065: 'x': undeclared identifier [C:\\b\\game.vcxproj]");
    log.add("  Building Custom Rule C:/g/CMakeLists.txt");
    // MSBuild's summary at the end repeats every error
    log.add("    C:\\g\\Player.cpp(7,3): error C2065: 'x': undeclared identifier [C:\\b\\game.vcxproj]");
    EXPECT_EQ(log.errorCount(), 1u);
    ASSERT_EQ(log.errors().size(), 1u);
    EXPECT_EQ(log.failureDetail(), " with 1 error(s). First: C:\\g\\Player.cpp:7: 'x': undeclared identifier");
}

TEST(BuildDiagnostics, KeepsTwentyCountsAll) {
    BuildErrorLog log;
    for (int i = 0; i < 57; i++) log.add("/g/a.cpp:" + std::to_string(i + 1) + ":1: error: e" + std::to_string(i));
    EXPECT_EQ(log.errorCount(), 57u);
    EXPECT_EQ(log.errors().size(), BuildErrorLog::kMaxKept);
    EXPECT_EQ(log.errors().front().message, "e0");
}

TEST(BuildDiagnostics, LastLineWhenNothingMatches) {
    BuildErrorLog log;
    log.add("[1/2] Building CXX object a.o");
    log.add("Killed");
    log.add("");
    EXPECT_EQ(log.errorCount(), 0u);
    EXPECT_EQ(log.failureDetail(), "; the last output was: Killed");
}

TEST(BuildDiagnostics, RepeatsPastTheKeptOnesCountOnce) {
    BuildErrorLog log;
    for (int pass = 0; pass < 2; pass++) {
        // MSBuild's summary repeats all 25 errors
        for (int i = 0; i < 25; i++) log.add("C:\\g\\a.cpp(" + std::to_string(i + 1) + ",1): error C2065: e [C:\\b\\g.vcxproj]");
    }
    EXPECT_EQ(log.errorCount(), 25u);
    EXPECT_EQ(log.errors().size(), BuildErrorLog::kMaxKept);
}
