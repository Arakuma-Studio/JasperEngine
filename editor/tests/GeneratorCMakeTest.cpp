// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project CMakeLists.txt the editor generates (Play's plugin build and a standalone build of
// the project against the editor's engine library; docs/superpowers/specs/
// 2026-11-04-build-toolchain-design.md 6.2, 6.5, 6.7): the window title reaches the compiler as
// escaped bytes, and the engine library's ABI macros hold whatever JASPERENGINE_LIB_DIR says.

#include <gtest/gtest.h>

#include "Generator.h"
#include "support/TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace {

    std::string generatorCMakeRead(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // the CMakeLists.txt a configure writes for a project with this window title
    std::string generatedCMake(const std::string& name, const std::string& title) {
        TestTempDir dir("generator-cmake-" + name);
        const fs::path project = dir.path();
        fs::create_directories(project / ".jasperengine" / "generated");
        WindowSettings window;
        window.title = title;
        const std::vector<SceneBuildInfo> scenes = {SceneBuildInfo{1, "Level", {1}, {1}, true}};
        Generator generator;
        generator.configure(scenes, "test", {}, {}, project, project / ".jasperengine", project / "assets", project / "lua", {}, 20,
                            Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, window);
        return generatorCMakeRead(project / "CMakeLists.txt");
    }

    // the text of the CMake block that starts at `opening` (an if(...) line), to its endif()
    std::string generatorCMakeBlockAt(const std::string& text, size_t opening) {
        int depth = 0;
        size_t pos = opening;
        while (pos < text.size()) {
            const size_t end = text.find('\n', pos);
            const std::string line = text.substr(pos, end - pos);
            const size_t first = line.find_first_not_of(' ');
            if (first != std::string::npos && line.compare(first, 3, "if(") == 0) depth++;
            if (first != std::string::npos && line.compare(first, 6, "endif(") == 0 && --depth == 0) {
                return text.substr(opening, end - opening);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        return text.substr(opening);
    }

}

TEST(GeneratorCMake, TitleIsCarriedAsEscapedBytes) {
    const std::string cmake = generatedCMake("title", "A; B #2");
    // ; and # never reach a command line: CMake splits a definition at ; and drops one holding #
    EXPECT_NE(cmake.find("-DJASPERENGINE_WINDOW_TITLE=\\\"A\\\\073 B \\\\0432\\\""), std::string::npos) << cmake;
    EXPECT_EQ(cmake.find("A; B"), std::string::npos) << cmake;
}

TEST(GeneratorCMake, PlainTitleStaysReadable) {
    const std::string cmake = generatedCMake("plain", "My Game 2");
    EXPECT_NE(cmake.find("-DJASPERENGINE_WINDOW_TITLE=\\\"My Game 2\\\""), std::string::npos) << cmake;
}

TEST(GeneratorCMake, AbiMacrosWhateverTheLibraryDirectory) {
    const std::string cmake = generatedCMake("abi", "Game");
    const size_t macros = cmake.find("add_compile_definitions(JASPERENGINE_SHARED JASPERENGINE_EDITOR)");
    ASSERT_NE(macros, std::string::npos) << cmake;
    // not inside the plugin-only block nor the default-library-directory block
    for (const char* opening : {"if(JASPERENGINE_EDITOR_PLUGIN)\n", "if(NOT DEFINED JASPERENGINE_LIB_DIR"}) {
        const size_t at = cmake.find(opening);
        ASSERT_NE(at, std::string::npos) << opening;
        EXPECT_EQ(generatorCMakeBlockAt(cmake, at).find("JASPERENGINE_SHARED"), std::string::npos) << generatorCMakeBlockAt(cmake, at);
    }
    // a static engine library (other layouts) is refused at configure; MinGW's import library is not static
    EXPECT_NE(cmake.find("is not an editor engine library"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("\\\\.dll\\\\.a$"), std::string::npos) << cmake;
}

TEST(GeneratorCMake, PhysicsLibrariesImportedFromTheEngineLibrary) {
    const std::string cmake = generatedCMake("physics", "Game");
    const size_t at = cmake.find("add_compile_definitions(JASPERENGINE_PHYSICS_2D JASPERENGINE_PHYSICS_3D");
    ASSERT_NE(at, std::string::npos) << cmake;
    const std::string line = cmake.substr(at, cmake.find('\n', at) - at);
    // the editor's engine library carries and exports Jolt (and Box2D: BOX2D_DLL on MSVC builds)
    EXPECT_NE(line.find("JPH_SHARED_LIBRARY"), std::string::npos) << line;
#if defined(_MSC_VER)
    EXPECT_NE(line.find("BOX2D_DLL"), std::string::npos) << line;
#endif
}

// clangd reads the scripts with the macros they build with: Jolt's layouts and linkage follow them
TEST(GeneratorCMake, ClangdFlagsCarryThePhysicsMacros) {
    TestTempDir dir("generator-clangd");
    const fs::path project = dir.path();
    TestTempDir::write(project / "scripts" / "Mover.cpp", "int mover() { return 0; }\n");
    Generator::writeEditorTooling(project, project / ".jasperengine", 20, {});
    const std::string database = generatorCMakeRead(project / ".jasperengine" / "clangd" / "compile_commands.json");
    ASSERT_NE(database.find("Mover.cpp"), std::string::npos) << database;
    EXPECT_NE(database.find("-DJPH_SHARED_LIBRARY"), std::string::npos) << database;
    EXPECT_NE(database.find("-DJPH_OBJECT_LAYER_BITS="), std::string::npos) << database;
}

// red before platform-build: Visual Studio builds ran MSBuild's /m:<jobs> over projects whose cl
// each took /MP<jobs>, up to jobs x jobs compilers (build-toolchain spec, deviation 9)
TEST(GeneratorCMake, VisualStudioBuildsOneProjectAtATime) {
    TestTempDir dir("generator-cmake-jobs");
    EXPECT_EQ(Generator::buildStepParallelJobs(dir.path(), 12), 12u) << "not configured yet: CMake's own choice";
    TestTempDir::write(dir / "CMakeCache.txt", "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n");
    EXPECT_EQ(Generator::buildStepParallelJobs(dir.path(), 12), 1u);
    for (const char* generator : {"Ninja", "Ninja Multi-Config", "Unix Makefiles", "MinGW Makefiles", "Xcode"}) {
        TestTempDir::write(dir / "CMakeCache.txt", std::string("CMAKE_GENERATOR:INTERNAL=") + generator + "\n");
        EXPECT_EQ(Generator::buildStepParallelJobs(dir.path(), 12), 12u) << generator;
    }
}
