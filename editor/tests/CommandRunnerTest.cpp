// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CommandRunner::capture, the argv probe the kit detection uses: the program runs directly (no
// shell), so a tool path holding spaces and shell metacharacters is one literal argument and
// nothing in it is executed. POSIX here.

#include <gtest/gtest.h>

#include "Generator.h"
#include "util/CommandRunner.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

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
