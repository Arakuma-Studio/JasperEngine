// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sprite normal session without the engine (the generate-sprite-normals CLI without
// --sprite installs no System): checkSpec and prepare with a projectDir read the assets directory
// from the project's project.yaml instead of System (which would abort), and editorProject() is
// absent. SystemTestAccess detaches the instance for the test and restores it.

#include <gtest/gtest.h>

#include "System.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "texsynth/SynthImage.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace jasperengine {
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

// The System of the test binary (other tests started the engine) is detached while one lives
struct NoSystem {
    jasperengine::System* previous = nullptr;
    NoSystem() : previous(jasperengine::SystemTestAccess::swap(nullptr)) {}
    ~NoSystem() { jasperengine::SystemTestAccess::swap(previous); }
};

fs::path makeProject() {
    const fs::path root = fs::temp_directory_path() / ("jasper-sn-noengine-" + std::to_string(static_cast<long>(getpid())));
    fs::remove_all(root);
    fs::create_directories(root / "assets" / "art");
    std::ofstream(root / "project.yaml") << "name: NoEngine\nassetsDir: assets\n";
    ts::Image image = ts::Image::make(16, 16, 4, 8);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint8_t* p = image.pixel(x, y);
            const bool inside = (x - 8) * (x - 8) + (y - 8) * (y - 8) < 36;
            p[0] = 200; p[1] = 120; p[2] = 60; p[3] = inside ? 255 : 0;
        }
    }
    std::vector<uint8_t> png;
    EXPECT_TRUE(ts::encodePng(image, png));
    std::ofstream(root / "assets" / "art" / "ball.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return root;
}

}

TEST(SpriteNormalsEngineFree, CheckSpecAndPrepareWithAProjectAndNoSystem) {
    const fs::path root = makeProject();
    NoSystem detached;
    ASSERT_FALSE(jasperengine::System::hasInstance());

    sn::Spec spec;
    spec.source = "assets/art/ball.png";
    EXPECT_TRUE(sn::checkSpec(root, spec, nullptr).empty()) << "the default output beside the source is inside the assets directory";

    // outside the assets directory project.yaml names: refused (read without System)
    spec.output.normal = "outside/ball_normal.png";
    const std::vector<sn::Problem> outside = sn::checkSpec(root, spec, nullptr);
    ASSERT_EQ(outside.size(), 1u);
    EXPECT_EQ(outside[0].kind, "output_invalid");

    spec.output.normal = "assets/out/ball_normal.png";
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    ASSERT_TRUE(sn::prepare(root, spec, nullptr, prepared, problems)) << (problems.empty() ? "" : problems[0].message);
    EXPECT_EQ(prepared.sourcePath, "assets/art/ball.png");
    EXPECT_EQ(prepared.source.color.width, 16);
    const sn::Result result = sn::generate(prepared.request);
    ASSERT_EQ(result.status, sn::Status::Ok);
    std::vector<sn::OutputFile> files;
    std::string error;
    ASSERT_TRUE(sn::encodeOutputs(prepared, result, files, error)) << error;
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].path, "assets/out/ball_normal.png");
    EXPECT_EQ(sn::checkStaleness(root, root / "assets" / "art" / "ball.png").state, sn::Staleness::State::NoRecipe);
    fs::remove_all(root);
}
