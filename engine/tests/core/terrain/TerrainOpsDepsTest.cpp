// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the terrain folder may depend on, and that it is the one query (T1 R1, R8, R14; AC1, AC12), by
// reading the sources as SokolPatchMarkers does

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

    namespace fs = std::filesystem;

    // engine/tests/core/terrain -> the repository's root
    fs::path repositoryRoot() {
        return fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
    }

    std::string readFile(const fs::path& path) {
        std::ifstream file(path);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    bool isSource(const fs::path& path) {
        const std::string extension = path.extension().string();
        return extension == ".h" || extension == ".cpp" || extension == ".inl" || extension == ".mm";
    }

}

TEST(TerrainOpsDeps, NoEcsOrRenderIncludes) {
    const fs::path folder = repositoryRoot() / "engine" / "core" / "terrain";
    ASSERT_TRUE(fs::is_directory(folder)) << folder;

    // R1: math, jobs and noise, this folder, the export macro, and the FNV hash mapHash names
    const std::vector<std::string> allowedPrefixes = {"math/", "jobs/", "noise/", "terrain/"};
    const std::set<std::string> allowedFiles = {"Export.h", "util/Fnv.h"};

    const std::regex include("#\\s*include\\s*\"([^\"]+)\"");
    int files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file() || !isSource(entry.path())) continue;
        files++;
        const std::string text = readFile(entry.path());
        for (auto it = std::sregex_iterator(text.begin(), text.end(), include); it != std::sregex_iterator(); ++it) {
            const std::string name = (*it)[1].str();
            bool allowed = allowedFiles.count(name) > 0;
            for (const std::string& prefix : allowedPrefixes) {
                allowed = allowed || name.rfind(prefix, 0) == 0;
            }
            EXPECT_TRUE(allowed) << entry.path().filename() << " includes \"" << name << "\"";
        }
    }
    EXPECT_GE(files, 10) << "the folder was found empty: the test would pass on nothing";
}

TEST(TerrainOpsDeps, OneQuery) {
    // The private copies are gone: nothing outside the folder decodes height texels on its own
    const std::vector<std::string> gone = {"bilinearHeightSample", "decodeHeightTexel"};
    int scanned = 0;
    for (const char* top : {"engine/core", "editor"}) {
        const fs::path root = repositoryRoot() / top;
        ASSERT_TRUE(fs::is_directory(root)) << root;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || !isSource(entry.path())) continue;
            const std::string generic = entry.path().generic_string();
            if (generic.find("engine/core/terrain/") != std::string::npos) continue;
            scanned++;
            const std::string text = readFile(entry.path());
            for (const std::string& name : gone) {
                EXPECT_EQ(text.find(name), std::string::npos) << generic << " still has " << name;
            }
        }
    }
    EXPECT_GT(scanned, 100);
}

// AC17 (T1 R22): the writers the engine replaced have no hit left in the editor
TEST(TerrainOpsDeps, OldWritersAreGone) {
    const std::vector<std::string> gone = {"TerrainMapPatchCmd", "scriptedStampSeconds", "workingPixels", "beforeSnapshot", "heightReference"};
    int scanned = 0;
    const fs::path root = repositoryRoot() / "editor";
    ASSERT_TRUE(fs::is_directory(root)) << root;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || !isSource(entry.path())) continue;
        scanned++;
        const std::string text = readFile(entry.path());
        for (const std::string& name : gone) {
            EXPECT_EQ(text.find(name), std::string::npos) << entry.path().generic_string() << " still has " << name;
        }
    }
    EXPECT_GT(scanned, 100);
}

TEST(TerrainOpsDeps, NoNondeterminismInTheFolder) {
    // R8: no wall clock, no global RNG, no standard distributions, no libm that varies by platform
    const fs::path folder = repositoryRoot() / "engine" / "core" / "terrain";
    const std::vector<std::string> banned = {
        "std::mt19937", "random_device", "uniform_real_distribution", "uniform_int_distribution", "std::rand(", "std::chrono",
        "std::sin(", "std::cos(", "std::tan(", "std::pow(", "std::exp(", "std::log(", "std::acos(", "std::atan"};
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file() || !isSource(entry.path())) continue;
        const std::string text = readFile(entry.path());
        for (const std::string& name : banned) {
            EXPECT_EQ(text.find(name), std::string::npos) << entry.path().filename() << " uses " << name;
        }
    }
}

TEST(TerrainOpsDeps, TheFolderBuildsWithoutContraction) {
    const std::string cmake = readFile(repositoryRoot() / "engine" / "CMakeLists.txt");
    EXPECT_NE(cmake.find("include(core/terrain/TerrainSources.cmake)"), std::string::npos);
    EXPECT_NE(cmake.find("JASPERENGINE_TERRAIN_SRCS} PROPERTIES COMPILE_OPTIONS \"-ffp-contract=off\""), std::string::npos);

    // Every source of the folder is in the list that gets the flag
    const std::string sources = readFile(repositoryRoot() / "engine" / "core" / "terrain" / "TerrainSources.cmake");
    for (const auto& entry : fs::directory_iterator(repositoryRoot() / "engine" / "core" / "terrain")) {
        if (entry.path().extension() == ".cpp") {
            EXPECT_NE(sources.find(entry.path().filename().string()), std::string::npos) << entry.path().filename();
        }
    }
}
