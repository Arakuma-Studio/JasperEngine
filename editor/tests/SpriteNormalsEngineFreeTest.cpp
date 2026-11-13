// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sprite normal session without the engine (the generate-sprite-normals CLI without
// --sprite installs no System): checkSpec and prepare with a projectDir read the assets directory
// from the project's project.yaml instead of System (which would abort), and editorProject() is
// absent. The System of the test binary (other tests started the engine, and their worker
// threads may still log) is detached only in a forked child: fork() copies the calling thread
// alone, so no other thread can call System::instance() while it is null.
//
// Also: a recipe read decides "garbled" on the error's origin (RecipeReadError), never on its
// text, and checkStaleness tells Unreadable from Invalid.

#include <gtest/gtest.h>

#include "System.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "texsynth/SynthImage.h"

#ifdef _WIN32
#include <process.h>    // getpid
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

namespace {

namespace fs = std::filesystem;
namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;

fs::path testRoot(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / ("jasper-sn-" + name + "-" + std::to_string(static_cast<long>(getpid())));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> ballPng() {
    ts::Image image = ts::Image::make(16, 16, 4, 8);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint8_t* p = image.pixel(x, y);
            const bool inside = (x - 8) * (x - 8) + (y - 8) * (y - 8) < 36;
            p[0] = 200; p[1] = 120; p[2] = 60; p[3] = inside ? 255 : 0;
        }
    }
    std::vector<uint8_t> png;
    ts::encodePng(image, png);
    return png;
}

#ifndef _WIN32
// The engine-free run, in the child (fork(): POSIX only): "" when every check holds, else what failed
std::string engineFreeRun(const fs::path& root) {
    jasperengine::SystemTestAccess::swap(nullptr);
    if (jasperengine::System::hasInstance()) return "the System is still set";
    sn::Spec spec;
    spec.source = "assets/art/ball.png";
    if (!sn::checkSpec(root, spec, nullptr).empty()) return "the default output beside the source should be inside the assets directory";
    // outside the assets directory project.yaml names: refused (read without System)
    spec.output.normal = "outside/ball_normal.png";
    const std::vector<sn::Problem> outside = sn::checkSpec(root, spec, nullptr);
    if (outside.size() != 1 || outside[0].kind != "output_invalid") return "an output outside the assets directory should be output_invalid";
    spec.output.normal = "assets/out/ball_normal.png";
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    if (!sn::prepare(root, spec, nullptr, prepared, problems)) return "prepare failed: " + (problems.empty() ? std::string() : problems[0].message);
    if (prepared.sourcePath != "assets/art/ball.png" || prepared.source.color.width != 16) return "prepare resolved the wrong source";
    const sn::Result result = sn::generate(prepared.request);
    if (result.status != sn::Status::Ok) return "generate failed";
    std::vector<sn::OutputFile> files;
    std::string error;
    if (!sn::encodeOutputs(prepared, result, files, error)) return "encodeOutputs failed: " + error;
    if (files.size() != 1 || files[0].path != "assets/out/ball_normal.png") return "encodeOutputs planned the wrong file";
    if (sn::checkStaleness(root, root / "assets" / "art" / "ball.png").state != sn::Staleness::State::NoRecipe) return "a plain PNG has no recipe";
    return std::string();
}
#endif

}

#ifndef _WIN32
TEST(SpriteNormalsEngineFree, CheckSpecAndPrepareWithAProjectAndNoSystem) {
    const fs::path root = testRoot("noengine");
    fs::create_directories(root / "assets" / "art");
    std::ofstream(root / "project.yaml") << "name: NoEngine\nassetsDir: assets\n";
    writeBytes(root / "assets" / "art" / "ball.png", ballPng());
    std::fflush(nullptr);
    const pid_t child = fork();
    ASSERT_GE(child, 0) << "fork failed";
    if (child == 0) {
        const std::string failure = engineFreeRun(root);
        if (!failure.empty()) std::fprintf(stderr, "engine-free run: %s\n", failure.c_str());
        std::fflush(stderr);
        _exit(failure.empty() ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status)) << "the engine-free run crashed (signal " << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ")";
    EXPECT_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0) << "see the child's message above";
    fs::remove_all(root);
}
#endif

TEST(SpriteNormalsEngineFree, GarbledIsDecidedByTheErrorsOriginNotItsText) {
    const fs::path root = testRoot("origin");
    // a missing file whose path holds the keyword: its error names it, but it is a file error
    const fs::path missing = root / sn::kRecipeKeyword / "missing.png";
    std::string error;
    sn::RecipeReadError origin = sn::RecipeReadError::None;
    EXPECT_FALSE(sn::readRecipeHead(missing, &error, &origin));
    EXPECT_NE(error.find(sn::kRecipeKeyword), std::string::npos) << "the message holds the keyword (a text match would be fooled)";
    EXPECT_EQ(origin, sn::RecipeReadError::File);
    EXPECT_EQ(sn::checkStaleness(root, missing).state, sn::Staleness::State::Unreadable);

    // not a PNG (a JPEG's first bytes), a truncated PNG
    const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00};
    writeBytes(root / sn::kRecipeKeyword / "photo.png", jpeg);
    EXPECT_FALSE(sn::readRecipeHead(root / sn::kRecipeKeyword / "photo.png", &error, &origin));
    EXPECT_EQ(origin, sn::RecipeReadError::File);
    EXPECT_EQ(sn::checkStaleness(root, root / sn::kRecipeKeyword / "photo.png").state, sn::Staleness::State::Unreadable);
    std::vector<uint8_t> truncated = ballPng();
    truncated.resize(20);
    writeBytes(root / "trunc.png", truncated);
    sn::readRecipeHead(root / "trunc.png", &error, &origin);
    EXPECT_NE(origin, sn::RecipeReadError::Chunk);
    EXPECT_NE(sn::checkStaleness(root, root / "trunc.png").state, sn::Staleness::State::Invalid);

    // a recipe chunk with a broken CRC: garbled
    nlohmann::json recipe = {{"format", sn::kRecipeFormat}, {"version", 1}};
    std::vector<uint8_t> png;
    std::string text = recipe.dump();
    ASSERT_TRUE(ts::insertRecipeChunk(ballPng(), text, png, error, sn::kRecipeKeyword)) << error;
    const std::string key = sn::kRecipeKeyword;
    for (size_t i = 8; i + 8 < png.size(); i++) {
        if (std::string(png.begin() + static_cast<long>(i), png.begin() + static_cast<long>(i + key.size())) == key) {
            png[i + key.size() + 6] ^= 0x01;   // a byte of the chunk's text: its CRC no longer matches
            break;
        }
    }
    writeBytes(root / "garbled.png", png);
    EXPECT_FALSE(sn::readRecipeHead(root / "garbled.png", &error, &origin));
    EXPECT_EQ(origin, sn::RecipeReadError::Chunk) << error;
    EXPECT_EQ(sn::checkStaleness(root, root / "garbled.png").state, sn::Staleness::State::Invalid);

    // a plain PNG: no recipe, no error
    writeBytes(root / "plain.png", ballPng());
    EXPECT_FALSE(sn::readRecipeHead(root / "plain.png", &error, &origin));
    EXPECT_EQ(origin, sn::RecipeReadError::None);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(sn::checkStaleness(root, root / "plain.png").state, sn::Staleness::State::NoRecipe);
    fs::remove_all(root);
}
