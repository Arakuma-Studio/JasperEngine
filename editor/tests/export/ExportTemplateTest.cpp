// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What every exported project builds with (engine/CMakeLists.txt, the Android Studio template):
// the project's sources globbed again at each build (S98 REQ-6, AC7), and game.json with the cooked
// scenes and prefabs (the export's project/content) at each platform's content root, beside
// assets/ and lua/ (S98 D1).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

    std::string repoText(const char* relative) {
        std::ifstream in(fs::path(JASPER_TEST_REPO_DIR) / relative, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // the standalone block of the engine's CMakeLists.txt: what an exported project compiles
    std::string standaloneBlock() {
        const std::string text = repoText("engine/CMakeLists.txt");
        const size_t at = text.find("if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)");
        return at == std::string::npos ? std::string() : text.substr(at);
    }

}

TEST(ExportTemplate, ProjectGlobConfigureDepends) {
    const std::string block = standaloneBlock();
    ASSERT_FALSE(block.empty());
    // an incremental Gradle .cxx or CMake tree compiles a C++ script added since its configure
    EXPECT_NE(block.find("file(GLOB_RECURSE PROJECT_SOURCE CONFIGURE_DEPENDS ${PROJECT_ROOT_GLOB}/*.cpp)"), std::string::npos);
}

TEST(ExportTemplate, CookedContentReachesEveryContentRoot) {
    const std::string cmake = repoText("engine/CMakeLists.txt");
    // the web's preload bundle mounts it at the root, as it mounts assets/ and lua/
    EXPECT_NE(cmake.find("--preload-file \\\"${EM_PRELOAD_ROOT}/content@/\\\""), std::string::npos);
    EXPECT_NE(cmake.find("foreach(preloadDir assets lua content)"), std::string::npos) << "an edited cooked scene relinks the page";
    // desktop builds copy it beside the executable (and install it), Apple bundles into their resources
    const std::string block = standaloneBlock();
    EXPECT_NE(block.find("COMMAND \"${CMAKE_COMMAND}\" -E copy_directory ${PROJECT_ROOT}/content ${CONTENT_DEST_DIR}"), std::string::npos);
    EXPECT_NE(block.find("install(DIRECTORY ${PROJECT_ROOT}/content/ DESTINATION bin)"), std::string::npos);
    EXPECT_NE(block.find("COMMAND \"${CMAKE_COMMAND}\" -E copy_directory ${PROJECT_ROOT}/content ${BUNDLE_RESOURCES_DIR}"), std::string::npos);
    // Android merges it into the APK's assets root
    const std::string gradle = repoText("engine/workspaces/androidstudio/app/build.gradle");
    EXPECT_NE(gradle.find("from(projectSourceRootAbs + '/content')"), std::string::npos);
}
