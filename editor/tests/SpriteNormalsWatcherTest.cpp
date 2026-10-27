// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The staleness index and the opt-in auto-regenerate of generated normal maps
// (spritenormals/SpriteNormalsWatcher, sprite normals Phase 4 spec 7 and 9.2): the index finds the
// project's generated PNGs and ignores plain ones, a changed source makes its outputs stale after a
// scan, a deleted one missing, an unchanged source is never hashed again; auto-regenerate off writes
// nothing, on rewrites the map within two ticks without a project undo step and logs it; a playing
// scene pauses it; a source changed less than a second ago waits. A headless project, as
// SpriteNormalsApplyTest's.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "texsynth/SynthImage.h"

#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;

namespace jasperengine::editor {

    struct SpriteNormalsWatcherTestAccess {
        using W = SpriteNormalsWatcher;
        // a whole scan now: started, finished, collected
        static void scan(W& w) {
            w.lastScan = -1.0;
            w.tick();
            if (w.scan.valid()) w.scan.wait();
            w.tick();
        }
        static bool scanning(W& w) { return w.scan.valid(); }
        static int hashes(W& w) { return w.hashCount->load(); }
        static void settle(W& w, double seconds) { w.settleSeconds = seconds; }
        static std::shared_ptr<sn::SpriteNormalsRun> regeneration(W& w) { return w.regen; }
        static size_t queued(W& w) { return w.queue.size(); }
        static const std::string& lastLog(W& w) { return w.lastLog; }
    };

}

namespace {

    using Access = SpriteNormalsWatcherTestAccess;

    struct Headless {
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-spritenormals-watcher-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: spritenormalswatcher\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            static char arg0[] = "jasperengine-editor-tests";
            static char* argv[] = {arg0, nullptr};
            Engine::systemInit(1, argv, out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        static char arg0[] = "jasperengine-editor-tests";
        static char* argv[] = {arg0, nullptr};
        if (&System::instance() != h.platform.get()) Engine::systemInit(1, argv, h.platform.get());
        return h;
    }

    Project* project() { return headless().project.get(); }
    const fs::path& root() { return headless().dir; }

    struct TempProjectCleanup : public ::testing::Environment {
        void TearDown() override {
            std::error_code ec;
            fs::remove_all(fs::temp_directory_path() / ("jasper-spritenormals-watcher-test-" + std::to_string(::getpid())), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const cleanup = ::testing::AddGlobalTestEnvironment(new TempProjectCleanup);

    ts::Image disk(int size, int shade) {
        ts::Image image = ts::Image::make(size, size, 4, 8);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                uint8_t* p = image.pixel(x, y);
                const float dx = x + 0.5f - size * 0.5f, dy = y + 0.5f - size * 0.5f;
                if (dx * dx + dy * dy > size * size * 0.18f) continue;
                p[0] = static_cast<uint8_t>(std::clamp(shade + x * 3, 0, 255));
                p[1] = static_cast<uint8_t>(std::clamp(shade + y * 2, 0, 255));
                p[2] = 60;
                p[3] = 255;
            }
        }
        return image;
    }

    void writePng(const std::string& rel, const ts::Image& image) {
        std::vector<uint8_t> png;
        ASSERT_TRUE(ts::encodePng(image, png));
        const fs::path path = root() / fs::u8path(rel);
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // a generated map of `source` (written without history, as the CLI does)
    void generate(const std::string& source) {
        sn::Spec spec;
        spec.source = source;
        spec.settings = sn::presetSettings(sn::Preset::Default);
        spec.overwrite = true;
        sn::Prepared prepared;
        std::vector<sn::Problem> problems;
        ASSERT_TRUE(sn::prepare(root(), spec, nullptr, prepared, problems)) << (problems.empty() ? "" : problems[0].message);
        const sn::Result result = sn::generate(prepared.request);
        ASSERT_EQ(result.status, sn::Status::Ok);
        std::vector<sn::OutputFile> files;
        std::string error, failed;
        ASSERT_TRUE(sn::encodeOutputs(prepared, result, files, error)) << error;
        ASSERT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;
    }

    // a file's mtime moved into the past (a source that has rested)
    void age(const std::string& rel, int seconds) {
        const fs::path path = root() / fs::u8path(rel);
        fs::last_write_time(path, fs::last_write_time(path) - std::chrono::seconds(seconds));
    }

    // the test's own folder, every other test's outputs gone (a watcher indexes the whole project)
    std::string fresh(const std::string& name) {
        fs::remove_all(root() / "assets");
        fs::create_directories(root() / "assets" / name);
        return "assets/" + name;
    }

}

TEST(SpriteNormalsWatcher, IndexStaleMissingAndHashing) {
    const std::string dir = fresh("index");
    writePng(dir + "/hero.png", disk(32, 80));
    writePng(dir + "/plain.png", disk(16, 40));
    generate(dir + "/hero.png");
    age(dir + "/hero.png", 10);
    project()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(project());
    const fs::path map = root() / dir / "hero_normal.png";
    const fs::path source = root() / dir / "hero.png";
    Access::scan(watcher);
    std::string recorded, role;
    ASSERT_TRUE(watcher.generatedInfo(map, recorded, role));
    EXPECT_EQ(recorded, dir + "/hero.png");
    EXPECT_EQ(role, "normal");
    EXPECT_FALSE(watcher.generatedInfo(root() / dir / "plain.png", recorded, role)) << "a plain PNG is not an output";
    EXPECT_FALSE(watcher.isStale(map));
    EXPECT_FALSE(watcher.sourceMissing(map));
    EXPECT_EQ(watcher.outputsOf(source), std::vector<fs::path>{map.lexically_normal()});
    const int hashed = Access::hashes(watcher);
    EXPECT_GE(hashed, 1);
    const uint64_t revision = watcher.revision();

    // an unchanged source is not hashed again
    Access::scan(watcher);
    EXPECT_EQ(Access::hashes(watcher), hashed);
    EXPECT_FALSE(watcher.isStale(map));

    // modified: stale after the scan (and the badge revision grows)
    writePng(dir + "/hero.png", disk(32, 120));
    EXPECT_FALSE(watcher.isStale(map)) << "not before the scan";
    Access::scan(watcher);
    EXPECT_TRUE(watcher.isStale(map));
    EXPECT_GT(watcher.revision(), revision);
    EXPECT_EQ(Access::hashes(watcher), hashed + 1);

    // deleted: missing
    fs::remove(source);
    Access::scan(watcher);
    EXPECT_TRUE(watcher.sourceMissing(map));
    EXPECT_FALSE(watcher.isStale(map));
}

TEST(SpriteNormalsWatcher, AutoRegenerateOffWritesNothing) {
    const std::string dir = fresh("off");
    writePng(dir + "/hero.png", disk(32, 80));
    generate(dir + "/hero.png");
    project()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(project());
    Access::scan(watcher);
    const std::string before = readAll(root() / dir / "hero_normal.png");
    writePng(dir + "/hero.png", disk(32, 140));
    age(dir + "/hero.png", 10);
    Access::scan(watcher);
    EXPECT_TRUE(watcher.isStale(root() / dir / "hero_normal.png"));
    for (int i = 0; i < 20; i++) {
        watcher.tick();
        usleep(5000);
    }
    EXPECT_EQ(Access::regeneration(watcher), nullptr);
    EXPECT_EQ(readAll(root() / dir / "hero_normal.png"), before);
}

TEST(SpriteNormalsWatcher, AutoRegenerateRewritesWithoutUndo) {
    const std::string dir = fresh("on");
    writePng(dir + "/hero.png", disk(32, 80));
    generate(dir + "/hero.png");
    project()->getSpriteNormalsSettings().autoRegenerate = true;
    SpriteNormalsWatcher watcher(project());
    Access::scan(watcher);
    const fs::path map = root() / dir / "hero_normal.png";
    const std::string before = readAll(map);
    const size_t history = project()->getProjectCommandHistory()->position();
    writePng(dir + "/hero.png", disk(32, 160));
    age(dir + "/hero.png", 10);
    // tick 1: the scan finds it stale and starts the regeneration; tick 2: written
    Access::scan(watcher);
    ASSERT_NE(Access::regeneration(watcher), nullptr) << "regenerating after the scan";
    const std::shared_ptr<sn::SpriteNormalsRun> run = Access::regeneration(watcher);
    for (int i = 0; i < 4000 && !run->finished(); i++) usleep(1000);
    watcher.tick();
    EXPECT_NE(readAll(map), before) << "rewritten";
    EXPECT_EQ(sn::checkStaleness(root(), map).state, sn::Staleness::State::Fresh);
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), history) << "not an undo step";
    EXPECT_NE(Access::lastLog(watcher).find("Regenerated " + dir + "/hero_normal.png (hero.png changed)"), std::string::npos) << Access::lastLog(watcher);
    // the next scan sees it fresh
    Access::scan(watcher);
    EXPECT_FALSE(watcher.isStale(map));
    project()->getSpriteNormalsSettings().autoRegenerate = false;
}

TEST(SpriteNormalsWatcher, PausedWhileAScenePlays) {
    const std::string dir = fresh("play");
    writePng(dir + "/hero.png", disk(32, 80));
    generate(dir + "/hero.png");
    project()->getSpriteNormalsSettings().autoRegenerate = true;
    SpriteNormalsWatcher watcher(project());
    Access::scan(watcher);
    writePng(dir + "/hero.png", disk(32, 200));
    age(dir + "/hero.png", 10);
    project()->createNewScene("Playing", SceneType::SCENE_2D);
    SceneProject* sp = project()->getScene(project()->getSelectedSceneId());
    ASSERT_NE(sp, nullptr);
    sp->playState = ScenePlayState::PLAYING;
    Access::scan(watcher);
    EXPECT_FALSE(Access::scanning(watcher)) << "no scan while playing";
    EXPECT_FALSE(watcher.isStale(root() / dir / "hero_normal.png")) << "nothing learned while playing";
    EXPECT_EQ(Access::regeneration(watcher), nullptr);
    sp->playState = ScenePlayState::STOPPED;
    Access::scan(watcher);
    EXPECT_NE(Access::regeneration(watcher), nullptr) << "resumed once stopped";
    const std::shared_ptr<sn::SpriteNormalsRun> run = Access::regeneration(watcher);
    for (int i = 0; i < 4000 && run && !run->finished(); i++) usleep(1000);
    watcher.tick();
    project()->getSpriteNormalsSettings().autoRegenerate = false;
}

TEST(SpriteNormalsWatcher, ARecentChangeWaits) {
    const std::string dir = fresh("settle");
    writePng(dir + "/hero.png", disk(32, 80));
    generate(dir + "/hero.png");
    project()->getSpriteNormalsSettings().autoRegenerate = true;
    SpriteNormalsWatcher watcher(project());
    Access::scan(watcher);
    // changed just now: stale, but it waits (an editor writing in several steps)
    writePng(dir + "/hero.png", disk(32, 30));
    Access::scan(watcher);
    EXPECT_TRUE(watcher.isStale(root() / dir / "hero_normal.png"));
    EXPECT_EQ(Access::regeneration(watcher), nullptr);
    EXPECT_EQ(Access::queued(watcher), 0u);
    // rested: regenerated
    age(dir + "/hero.png", 5);
    Access::scan(watcher);
    const std::shared_ptr<sn::SpriteNormalsRun> run = Access::regeneration(watcher);
    ASSERT_NE(run, nullptr);
    for (int i = 0; i < 4000 && !run->finished(); i++) usleep(1000);
    watcher.tick();
    EXPECT_EQ(sn::checkStaleness(root(), root() / dir / "hero_normal.png").state, sn::Staleness::State::Fresh);
    project()->getSpriteNormalsSettings().autoRegenerate = false;
}
