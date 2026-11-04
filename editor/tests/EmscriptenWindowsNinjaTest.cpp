// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Windows Web build's generator when PATH has neither mingw32-make nor Ninja: the Ninja of
// Visual Studio's C++ CMake tools (CommandRunner::findVisualStudioNinja), which Visual Studio
// keeps off PATH, named to CMake as the make program. Platform-independent, as the generator choice is.

#include <gtest/gtest.h>

#include "Exporter.h"
#include "support/TestTempDir.h"
#include "util/CommandRunner.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

TEST(EmscriptenSdk, WindowsGeneratorFallsBackToVisualStudiosNinja) {
    jasperengine::test::TestTempDir dir("emvsninja");
    const fs::path base = dir.path();
    const fs::path ninjaDir = base / "ninja dir", empty = base / "empty";
    fs::create_directories(ninjaDir);
    fs::create_directories(empty);
    std::ofstream(ninjaDir / "ninja.exe") << "x";
    const fs::path vsNinja = base / "VS" / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja" / "ninja.exe";
    using Env = CommandRunner::Environment;
    std::string error;

    // nothing on PATH: Visual Studio's Ninja, named as the make program
    EXPECT_EQ(Exporter::emscriptenWindowsGeneratorArgs(Env{{"PATH", empty.string()}}, false, error, vsNinja),
              (std::vector<std::string>{"-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + vsNinja.generic_string()}));
    EXPECT_TRUE(error.empty()) << error;
    // PATH's Ninja still wins
    EXPECT_EQ(Exporter::emscriptenWindowsGeneratorArgs(Env{{"PATH", ninjaDir.string()}}, false, error, vsNinja),
              (std::vector<std::string>{"-G", "Ninja"}));
    // no Visual Studio either: the error says what to install
    EXPECT_TRUE(Exporter::emscriptenWindowsGeneratorArgs(Env{{"PATH", empty.string()}}, false, error).empty());
    EXPECT_NE(error.find("Visual Studio"), std::string::npos) << error;
}
