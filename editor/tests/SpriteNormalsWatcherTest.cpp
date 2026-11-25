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
#include "mcp/tools/McpTools.h"

#include "support/PosixCompat.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "SymlinkSupport.h"
#include "ScriptDataTestSupport.h"
#include "support/EditorTestSupport.h"

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
            if (std::shared_ptr<W::ScanJob> job = w.scan) {
                std::unique_lock<std::mutex> lock(job->mutex);
                job->finished.wait(lock, [&job]() { return job->done.load(); });
            }
            w.tick();
        }
        static bool scanning(W& w) { return w.scan != nullptr; }
        static std::shared_ptr<W::ScanJob> job(W& w) { return w.scan; }
        static void startScan(W& w) {
            w.lastScan = -1.0;
            w.tick();
        }
        static int idleScans(W& w) { return w.idleScans; }
        static void failNextScan(W& w) { w.failNextScan = true; }
        static double interval(W& w) { return w.interval(); }
        static int hashes(W& w) { return w.hashCount->load(); }
        static void settle(W& w, double seconds) { w.settleSeconds = seconds; }
        static std::shared_ptr<sn::SpriteNormalsRun> regeneration(W& w) { return w.regen; }
        static size_t queued(W& w) { return w.queue.size(); }
        static const std::string& lastLog(W& w) { return w.lastLog; }
    };

}

namespace {

    using WatcherAccess = SpriteNormalsWatcherTestAccess;

    struct SpriteNormalsWatcherHeadless {
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    SpriteNormalsWatcherHeadless& spriteNormalsWatcherHeadless() {
        static SpriteNormalsWatcherHeadless h = []() {
            SpriteNormalsWatcherHeadless out;
            const fs::path spriteNormalsWatcherRoot = fs::temp_directory_path() / ("jasper-spritenormals-watcher-test-" + std::to_string(::getpid()));
            fs::remove_all(spriteNormalsWatcherRoot);
            fs::create_directories(spriteNormalsWatcherRoot / "config");
            setenv("XDG_CONFIG_HOME", (spriteNormalsWatcherRoot / "config").c_str(), 1);
            out.dir = spriteNormalsWatcherRoot / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: spritenormalswatcher\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::releaseAtExit(out.project.get());
            out.platform.reset(new editor::Platform(out.project.get()));
            // the editor's Platform as System::instance() for this suite's tests (the binary's engine
            // is TestMain's: a second Engine::systemInit would restart its jobs and logger)
            headlesssystem::use(out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        headlesssystem::use(h.platform.get());
        return h;
    }

    Project* spriteNormalsWatcherProject() { return spriteNormalsWatcherHeadless().project.get(); }
    const fs::path& spriteNormalsWatcherRoot() { return spriteNormalsWatcherHeadless().dir; }

    struct SpriteNormalsWatcherTempProjectCleanup : public ::testing::Environment {
        void TearDown() override {
            std::error_code ec;
            fs::remove_all(fs::temp_directory_path() / ("jasper-spritenormals-watcher-test-" + std::to_string(::getpid())), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const spriteNormalsWatcherCleanup = ::testing::AddGlobalTestEnvironment(new SpriteNormalsWatcherTempProjectCleanup);

    ts::Image spriteNormalsWatcherDisk(int size, int shade) {
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

    void spriteNormalsWatcherWritePng(const std::string& rel, const ts::Image& image) {
        std::vector<uint8_t> png;
        ASSERT_TRUE(ts::encodePng(image, png));
        const fs::path path = spriteNormalsWatcherRoot() / fs::u8path(rel);
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }

    std::string spriteNormalsWatcherReadAll(const fs::path& path) {
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
        ASSERT_TRUE(sn::prepare(spriteNormalsWatcherRoot(), spec, nullptr, prepared, problems)) << (problems.empty() ? "" : problems[0].message);
        const sn::Result result = sn::generate(prepared.request);
        ASSERT_EQ(result.status, sn::Status::Ok);
        std::vector<sn::OutputFile> files;
        std::string error, failed;
        ASSERT_TRUE(sn::encodeOutputs(prepared, result, files, error)) << error;
        ASSERT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;
    }

    // a file's mtime moved into the past (a source that has rested)
    void age(const std::string& rel, int seconds) {
        const fs::path path = spriteNormalsWatcherRoot() / fs::u8path(rel);
        fs::last_write_time(path, fs::last_write_time(path) - std::chrono::seconds(seconds));
    }

    // the test's own folder, every other test's outputs gone (a watcher indexes the whole project)
    std::string fresh(const std::string& name) {
        fs::remove_all(spriteNormalsWatcherRoot() / "assets");
        fs::create_directories(spriteNormalsWatcherRoot() / "assets" / name);
        return "assets/" + name;
    }

}

TEST(SpriteNormalsWatcher, IndexStaleMissingAndHashing) {
    const std::string dir = fresh("index");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    spriteNormalsWatcherWritePng(dir + "/plain.png", spriteNormalsWatcherDisk(16, 40));
    generate(dir + "/hero.png");
    age(dir + "/hero.png", 10);
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    const fs::path map = spriteNormalsWatcherRoot() / dir / "hero_normal.png";
    const fs::path source = spriteNormalsWatcherRoot() / dir / "hero.png";
    WatcherAccess::scan(watcher);
    std::string recorded, role;
    ASSERT_TRUE(watcher.generatedInfo(map, recorded, role));
    EXPECT_EQ(recorded, dir + "/hero.png");
    EXPECT_EQ(role, "normal");
    EXPECT_FALSE(watcher.generatedInfo(spriteNormalsWatcherRoot() / dir / "plain.png", recorded, role)) << "a plain PNG is not an output";
    EXPECT_FALSE(watcher.isStale(map));
    EXPECT_FALSE(watcher.sourceMissing(map));
    EXPECT_EQ(watcher.outputsOf(source), std::vector<fs::path>{map.lexically_normal()});
    const int hashed = WatcherAccess::hashes(watcher);
    EXPECT_GE(hashed, 1);
    const uint64_t revision = watcher.revision();

    // an unchanged source is not hashed again
    WatcherAccess::scan(watcher);
    EXPECT_EQ(WatcherAccess::hashes(watcher), hashed);
    EXPECT_FALSE(watcher.isStale(map));

    // modified: stale after the scan (and the badge revision grows)
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 120));
    EXPECT_FALSE(watcher.isStale(map)) << "not before the scan";
    WatcherAccess::scan(watcher);
    EXPECT_TRUE(watcher.isStale(map));
    EXPECT_GT(watcher.revision(), revision);
    EXPECT_EQ(WatcherAccess::hashes(watcher), hashed + 1);

    // deleted: missing
    fs::remove(source);
    WatcherAccess::scan(watcher);
    EXPECT_TRUE(watcher.sourceMissing(map));
    EXPECT_FALSE(watcher.isStale(map));
}

// The MCP file-sync hook (mcp::spriteNormalsFileChanged, Phase 3 spec 3.9) asks the editor's watcher
// first: a map under a name the default-name probe doesn't try (and not in the check cache) is reported
// once the watcher has indexed it, and only then
TEST(SpriteNormalsWatcher, FileSyncAsksTheWatcherFirst) {
    const std::string dir = fresh("sync");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    generate(dir + "/hero.png");
    fs::create_directories(spriteNormalsWatcherRoot() / dir / "lit");
    fs::rename(spriteNormalsWatcherRoot() / dir / "hero_normal.png", spriteNormalsWatcherRoot() / dir / "lit" / "hero_n.png");
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 120));
    const fs::path source = spriteNormalsWatcherRoot() / dir / "hero.png";

    // (an editor made by an earlier test of this process may have left its watcher active)
    SpriteNormalsWatcher* const before = SpriteNormalsWatcher::active();
    SpriteNormalsWatcher::setActive(nullptr);
    mcp::Json problems = mcp::Json::array();
    mcp::spriteNormalsFileChanged(spriteNormalsWatcherProject(), source, problems);
    EXPECT_TRUE(problems.empty()) << "no watcher: only the default names and the check cache, which don't know it";

    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    SpriteNormalsWatcher::setActive(&watcher);
    mcp::spriteNormalsFileChanged(spriteNormalsWatcherProject(), source, problems);
    SpriteNormalsWatcher::setActive(nullptr);
    if (problems.size() != 1u) SpriteNormalsWatcher::setActive(before);
    ASSERT_EQ(problems.size(), 1u) << problems.dump();
    EXPECT_EQ(problems[0].value("path", std::string()), dir + "/lit/hero_n.png");
    EXPECT_NE(problems[0].value("message", std::string()).find("made from " + dir + "/hero.png, which changed"), std::string::npos)
        << problems.dump();

    // the default-name map is still found beside the watcher's (once)
    generate(dir + "/hero.png");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 160));
    WatcherAccess::scan(watcher);
    SpriteNormalsWatcher::setActive(&watcher);
    problems = mcp::Json::array();
    mcp::spriteNormalsFileChanged(spriteNormalsWatcherProject(), source, problems);
    SpriteNormalsWatcher::setActive(before);
    EXPECT_EQ(problems.size(), 2u) << problems.dump();
}

TEST(SpriteNormalsWatcher, AutoRegenerateOffWritesNothing) {
    const std::string dir = fresh("off");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    generate(dir + "/hero.png");
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    const std::string before = spriteNormalsWatcherReadAll(spriteNormalsWatcherRoot() / dir / "hero_normal.png");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 140));
    age(dir + "/hero.png", 10);
    WatcherAccess::scan(watcher);
    EXPECT_TRUE(watcher.isStale(spriteNormalsWatcherRoot() / dir / "hero_normal.png"));
    for (int i = 0; i < 20; i++) {
        watcher.tick();
        usleep(5000);
    }
    EXPECT_EQ(WatcherAccess::regeneration(watcher), nullptr);
    EXPECT_EQ(spriteNormalsWatcherReadAll(spriteNormalsWatcherRoot() / dir / "hero_normal.png"), before);
}

TEST(SpriteNormalsWatcher, AutoRegenerateRewritesWithoutUndo) {
    const std::string dir = fresh("on");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    generate(dir + "/hero.png");
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = true;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    const fs::path map = spriteNormalsWatcherRoot() / dir / "hero_normal.png";
    const std::string before = spriteNormalsWatcherReadAll(map);
    const size_t history = spriteNormalsWatcherProject()->getProjectCommandHistory()->position();
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 160));
    age(dir + "/hero.png", 10);
    // tick 1: the scan finds it stale and starts the regeneration; tick 2: written
    WatcherAccess::scan(watcher);
    ASSERT_NE(WatcherAccess::regeneration(watcher), nullptr) << "regenerating after the scan";
    const std::shared_ptr<sn::SpriteNormalsRun> run = WatcherAccess::regeneration(watcher);
    for (int i = 0; i < 4000 && !run->finished(); i++) usleep(1000);
    watcher.tick();
    EXPECT_NE(spriteNormalsWatcherReadAll(map), before) << "rewritten";
    EXPECT_EQ(sn::checkStaleness(spriteNormalsWatcherRoot(), map).state, sn::Staleness::State::Fresh);
    EXPECT_EQ(spriteNormalsWatcherProject()->getProjectCommandHistory()->position(), history) << "not an undo step";
    EXPECT_NE(WatcherAccess::lastLog(watcher).find("Regenerated " + dir + "/hero_normal.png (hero.png changed)"), std::string::npos) << WatcherAccess::lastLog(watcher);
    // the next scan sees it fresh
    WatcherAccess::scan(watcher);
    EXPECT_FALSE(watcher.isStale(map));
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
}

TEST(SpriteNormalsWatcher, RegenerationNeverWritesThroughALinkOutOfTheAssets) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = fresh("linked");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    generate(dir + "/hero.png");
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = true;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    const fs::path map = spriteNormalsWatcherRoot() / dir / "hero_normal.png";
    const fs::path outside = spriteNormalsWatcherRoot().parent_path() / "outside-watcher";
    fs::remove_all(outside);
    fs::create_directories(outside);
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 160));
    age(dir + "/hero.png", 10);
    WatcherAccess::scan(watcher);
    ASSERT_NE(WatcherAccess::regeneration(watcher), nullptr) << "regenerating after the scan";
    const std::shared_ptr<sn::SpriteNormalsRun> run = WatcherAccess::regeneration(watcher);
    for (int i = 0; i < 4000 && !run->finished(); i++) usleep(1000);
    // while it ran, the map became a (dangling) link out of the project: checked again right
    // before the write, nothing is written through it
    fs::remove(map);
    fs::create_symlink(outside / "hero_normal.png", map);
    watcher.tick();
    EXPECT_FALSE(fs::exists(outside / "hero_normal.png")) << "nothing written outside";
    EXPECT_TRUE(fs::is_symlink(map));
    EXPECT_NE(WatcherAccess::lastLog(watcher).find("Could not regenerate hero_normal.png"), std::string::npos) << WatcherAccess::lastLog(watcher);
    EXPECT_NE(WatcherAccess::lastLog(watcher).find("symbolic link"), std::string::npos) << WatcherAccess::lastLog(watcher);
    fs::remove(map);
    fs::remove_all(outside);
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
}

TEST(SpriteNormalsWatcher, PausedWhileAScenePlays) {
    const std::string dir = fresh("play");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    generate(dir + "/hero.png");
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = true;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 200));
    age(dir + "/hero.png", 10);
    spriteNormalsWatcherProject()->createNewScene("Playing", SceneType::SCENE_2D);
    SceneProject* sp = spriteNormalsWatcherProject()->getScene(spriteNormalsWatcherProject()->getSelectedSceneId());
    ASSERT_NE(sp, nullptr);
    sp->playState = ScenePlayState::PLAYING;
    WatcherAccess::scan(watcher);
    EXPECT_FALSE(WatcherAccess::scanning(watcher)) << "no scan while playing";
    EXPECT_FALSE(watcher.isStale(spriteNormalsWatcherRoot() / dir / "hero_normal.png")) << "nothing learned while playing";
    EXPECT_EQ(WatcherAccess::regeneration(watcher), nullptr);
    sp->playState = ScenePlayState::STOPPED;
    WatcherAccess::scan(watcher);
    EXPECT_NE(WatcherAccess::regeneration(watcher), nullptr) << "resumed once stopped";
    const std::shared_ptr<sn::SpriteNormalsRun> run = WatcherAccess::regeneration(watcher);
    for (int i = 0; i < 4000 && run && !run->finished(); i++) usleep(1000);
    watcher.tick();
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
}

TEST(SpriteNormalsWatcher, ARecentChangeWaits) {
    const std::string dir = fresh("settle");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    generate(dir + "/hero.png");
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = true;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    // changed just now: stale, but it waits (an editor writing in several steps)
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 30));
    WatcherAccess::scan(watcher);
    EXPECT_TRUE(watcher.isStale(spriteNormalsWatcherRoot() / dir / "hero_normal.png"));
    EXPECT_EQ(WatcherAccess::regeneration(watcher), nullptr);
    EXPECT_EQ(WatcherAccess::queued(watcher), 0u);
    // rested: regenerated
    age(dir + "/hero.png", 5);
    WatcherAccess::scan(watcher);
    const std::shared_ptr<sn::SpriteNormalsRun> run = WatcherAccess::regeneration(watcher);
    ASSERT_NE(run, nullptr);
    for (int i = 0; i < 4000 && !run->finished(); i++) usleep(1000);
    watcher.tick();
    EXPECT_EQ(sn::checkStaleness(spriteNormalsWatcherRoot(), spriteNormalsWatcherRoot() / dir / "hero_normal.png").state, sn::Staleness::State::Fresh);
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
}

TEST(SpriteNormalsWatcher, ResetDoesNotWaitAndIdleProjectsBackOff) {
    fresh("idle");
    spriteNormalsWatcherWritePng("assets/idle/plain.png", spriteNormalsWatcherDisk(16, 40));
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    // no generated map: each scan waits longer than the last
    const double first = WatcherAccess::interval(watcher);
    WatcherAccess::scan(watcher);
    WatcherAccess::scan(watcher);
    EXPECT_EQ(WatcherAccess::idleScans(watcher), 2);
    EXPECT_GT(WatcherAccess::interval(watcher), first);
    // dormant: not walked again until something says a map may have appeared (or 10 minutes)
    EXPECT_TRUE(watcher.dormant());
    EXPECT_EQ(WatcherAccess::interval(watcher), 600.0);
    // a write by the tool: back to the short interval
    watcher.invalidate(spriteNormalsWatcherRoot() / "assets/idle/plain.png");
    EXPECT_EQ(WatcherAccess::interval(watcher), first);
    EXPECT_FALSE(watcher.dormant());
    // a scan in flight is dropped, never waited for
    for (int i = 0; i < 300; i++) spriteNormalsWatcherWritePng("assets/idle/many" + std::to_string(i) + ".png", spriteNormalsWatcherDisk(8, 10));
    WatcherAccess::startScan(watcher);
    EXPECT_TRUE(WatcherAccess::scanning(watcher));
    const auto start = std::chrono::steady_clock::now();
    const auto job = WatcherAccess::job(watcher);
    watcher.reset();
    // never waited for: a wall-clock budget, enforced where perfSlack() asks for it (the benchmark run)
    if (const double slack = jasperengine::editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 0.05 * slack);
    }
    EXPECT_FALSE(WatcherAccess::scanning(watcher));
    // its thread ends on its own (waited here so no thread is left for the death tests)
    std::unique_lock<std::mutex> lock(job->mutex);
    job->finished.wait(lock, [&job]() { return job->done.load(); });
}

TEST(SpriteNormalsWatcher, ShutdownStopsTheScanThread) {
    fresh("shutdown");
    for (int i = 0; i < 400; i++) spriteNormalsWatcherWritePng("assets/shutdown/f" + std::to_string(i) + ".png", spriteNormalsWatcherDisk(8, 10));
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::startScan(watcher);
    const auto job = WatcherAccess::job(watcher);
    ASSERT_NE(job, nullptr);
    watcher.shutdown(2.0);
    EXPECT_TRUE(job->done) << "cancelled at its next file and finished before shutdown returned";
    EXPECT_FALSE(WatcherAccess::scanning(watcher));
}

// The scan thread is detached: an exception on it keeps the index as it was (the editor goes on),
// and the next scan works; a source over the generator's input limit is not read into memory (no
// map can have been made from it: stale)
TEST(SpriteNormalsWatcher, AFailedScanKeepsTheIndexAndHugeSourcesAreNotRead) {
    const std::string dir = fresh("barrier");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    generate(dir + "/hero.png");
    age(dir + "/hero.png", 10);
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    const fs::path map = (spriteNormalsWatcherRoot() / dir / "hero_normal.png").lexically_normal();
    const fs::path source = spriteNormalsWatcherRoot() / dir / "hero.png";
    WatcherAccess::scan(watcher);
    ASSERT_EQ(watcher.outputsOf(source), std::vector<fs::path>{map});
    const int hashed = WatcherAccess::hashes(watcher);

    WatcherAccess::failNextScan(watcher);
    WatcherAccess::scan(watcher);
    EXPECT_EQ(watcher.outputsOf(source), std::vector<fs::path>{map}) << "the index as it was";
    EXPECT_FALSE(watcher.isStale(map));
    WatcherAccess::scan(watcher);
    EXPECT_EQ(watcher.outputsOf(source), std::vector<fs::path>{map}) << "the next scan works";

    // the source grows past 64 MiB (sparse: nothing is written to disk)
    fs::resize_file(source, static_cast<uintmax_t>(sn::kMaxInputBytes) + 1024 * 1024);
    age(dir + "/hero.png", 10);
    WatcherAccess::scan(watcher);
    EXPECT_TRUE(watcher.isStale(map));
    EXPECT_EQ(WatcherAccess::hashes(watcher), hashed) << "not hashed (not read)";
}

// A wake while a scan runs is not lost: the scan (it may have read the project before the write)
// does not make the watcher dormant, and the next walk comes soon; the Project window's notice of a
// generated PNG wakes a dormant watcher once per path, and only for paths a walk reads
TEST(SpriteNormalsWatcher, AWakeDuringAScanIsKeptAndNoticesWakeOncePerPath) {
    const std::string dir = fresh("wake");
    spriteNormalsWatcherWritePng(dir + "/plain.png", spriteNormalsWatcherDisk(16, 40));
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    ASSERT_TRUE(watcher.dormant()) << "no generated map";

    // a scan in flight (started, not collected), then a write
    WatcherAccess::startScan(watcher);
    ASSERT_TRUE(WatcherAccess::scanning(watcher));
    watcher.invalidate(spriteNormalsWatcherRoot() / dir / "hero_normal.png");
    {
        const auto job = WatcherAccess::job(watcher);
        std::unique_lock<std::mutex> lock(job->mutex);
        job->finished.wait(lock, [&job]() { return job->done.load(); });
    }
    watcher.tick();   // collects it
    EXPECT_FALSE(WatcherAccess::scanning(watcher));
    EXPECT_FALSE(watcher.dormant()) << "the wake during the scan holds";
    EXPECT_EQ(WatcherAccess::interval(watcher), 5.0);
    WatcherAccess::scan(watcher);
    EXPECT_TRUE(watcher.dormant()) << "the next walk found no generated map either";

    // notices: only paths a walk reads, each once
    EXPECT_TRUE(watcher.wouldIndex(spriteNormalsWatcherRoot() / dir / "a_normal.png"));
    EXPECT_TRUE(watcher.wouldIndex(spriteNormalsWatcherRoot() / dir / "sub/../b_normal.PNG"));
    EXPECT_FALSE(watcher.wouldIndex(spriteNormalsWatcherRoot() / ".hidden" / "a_normal.png"));
    EXPECT_FALSE(watcher.wouldIndex(spriteNormalsWatcherRoot() / "build" / "a_normal.png"));
    EXPECT_FALSE(watcher.wouldIndex(spriteNormalsWatcherRoot() / dir / "a_normal.jpg"));
    EXPECT_FALSE(watcher.wouldIndex(spriteNormalsWatcherRoot().parent_path() / "elsewhere" / "a_normal.png"));
    watcher.noticeGenerated(spriteNormalsWatcherRoot() / ".hidden" / "a_normal.png");
    EXPECT_TRUE(watcher.dormant()) << "a path the walk never reads does not wake it";
    watcher.noticeGenerated(spriteNormalsWatcherRoot() / dir / "copied_normal.png");
    EXPECT_FALSE(watcher.dormant()) << "a generated PNG the index doesn't know";
    WatcherAccess::scan(watcher);
    ASSERT_TRUE(watcher.dormant());
    watcher.noticeGenerated(spriteNormalsWatcherRoot() / dir / "copied_normal.png");
    EXPECT_TRUE(watcher.dormant()) << "the same path does not wake it again";
}

// "Once per path" lasts only until a walk indexes the path: a map noticed, indexed, deleted (the
// watcher dormant again) and restored from outside the editor wakes it again
TEST(SpriteNormalsWatcher, ANoticedPathWakesAgainOnceItWasIndexed) {
    const std::string dir = fresh("renotice");
    spriteNormalsWatcherWritePng(dir + "/hero.png", spriteNormalsWatcherDisk(32, 80));
    spriteNormalsWatcherProject()->getSpriteNormalsSettings().autoRegenerate = false;
    SpriteNormalsWatcher watcher(spriteNormalsWatcherProject());
    WatcherAccess::scan(watcher);
    ASSERT_TRUE(watcher.dormant());
    // copied in from outside: noticed, and the walk indexes it
    generate(dir + "/hero.png");
    const fs::path map = (spriteNormalsWatcherRoot() / dir / "hero_normal.png").lexically_normal();
    const std::string saved = spriteNormalsWatcherReadAll(map);
    watcher.noticeGenerated(map);
    EXPECT_FALSE(watcher.dormant());
    WatcherAccess::scan(watcher);
    std::string source, role;
    ASSERT_TRUE(watcher.generatedInfo(map, source, role));
    // deleted: the next walk ends dormant
    fs::remove(map);
    WatcherAccess::scan(watcher);
    ASSERT_TRUE(watcher.dormant());
    // restored from outside the editor: the notice wakes it again
    std::ofstream(map, std::ios::binary) << saved;
    watcher.noticeGenerated(map);
    EXPECT_FALSE(watcher.dormant()) << "an indexed path is no longer suppressed";
    WatcherAccess::scan(watcher);
    EXPECT_TRUE(watcher.generatedInfo(map, source, role));
}
