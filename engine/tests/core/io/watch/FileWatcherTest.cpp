// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The watcher end to end on a real temp folder, for the native backend and the polling one
// (S05 AC1, AC2, AC4, AC5): what comes out of FileWatchService for each kind of edit.

#include "FileWatchService.h"
#include "ProjectFileIndex.h"
#include "WatchTestSupport.h"
#include "backends/Backends.h"
#include "jobs/detail/TimedWait.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    using ms = std::chrono::milliseconds;

    // The polling backend runs at 250 ms in these tests (the shortest interval allowed)
    constexpr ms POLL_INTERVAL{250};

    class FileWatcherTests : public ::testing::TestWithParam<WatchBackendKind> {
    protected:
        WatchTempDir dir;
        std::mutex mutex;
        std::condition_variable changesReady;
        std::vector<FileChange> changes;
        FileWatchService service;
        FileWatchSubscription subscription;

        void SetUp() override {
            // no System in this binary: the warnings are the test's
            service.setWarningHandler([](const std::string&) {});
            if (GetParam() == WatchBackendKind::Native && !FileWatcher::hasNative()) GTEST_SKIP() << "no native backend here";
        }

        void startWatching(bool directories = false) {
            ASSERT_TRUE(service.start(dir.path, GetParam(), POLL_INTERVAL));
            WatchFilter filter;
            filter.directories = directories;
            subscription = service.subscribe(filter, [this](std::span<const FileChange> batch) {
                std::lock_guard<std::mutex> lock(mutex);
                changes.insert(changes.end(), batch.begin(), batch.end());
                changesReady.notify_all();
            }, DeliverOn::Worker);
            // the index is built and the backend armed before the first edit
            ASSERT_TRUE(waitFor([this]() { return service.stats().indexReady; }, timeLimit(ms(5000))));
            if (GetParam() == WatchBackendKind::Polling) std::this_thread::sleep_for(POLL_INTERVAL * 2);
        }

        // within 1 s (native) or interval + 1 s (polling) (AC1)
        ms limit() const { return timeLimit(GetParam() == WatchBackendKind::Native ? ms(1000) : POLL_INTERVAL + ms(1000)); }

        std::vector<FileChange> take() {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<FileChange> out;
            out.swap(changes);
            return out;
        }
        // Wait for the backend's own notification, then drain its receipt to include duplicates.
        std::vector<FileChange> collect(size_t expected) {
            std::unique_lock<std::mutex> lock(mutex);
            EXPECT_TRUE(jobs::detail::timedWait(changesReady, lock, limit(), [&]() { return changes.size() >= expected; }));
            lock.unlock();
            EXPECT_TRUE(service.flush(limit()));
            return take();
        }
        static size_t countKind(const std::vector<FileChange>& list, FileChangeKind kind, bool directory) {
            return static_cast<size_t>(std::count_if(list.begin(), list.end(), [&](const FileChange& c) {
                return c.kind == kind && c.directory == directory;
            }));
        }
        // The file names only: the temp folder's name is "je-watch-" and a random 64-bit number,
        // which holds "4913" (vim's probe file) about one time in 600
        static bool names(const std::vector<FileChange>& list, const std::string& text) {
            return std::any_of(list.begin(), list.end(), [&](const FileChange& c) {
                return c.path.filename().generic_string().find(text) != std::string::npos ||
                       c.oldPath.filename().generic_string().find(text) != std::string::npos;
            });
        }
    };

    std::string describe(const std::vector<FileChange>& list) {
        std::string out;
        for (const FileChange& c : list) {
            out += std::string(fileChangeKindName(c.kind)) + " " + c.path.generic_string();
            if (c.kind == FileChangeKind::Moved) out += " < " + c.oldPath.generic_string();
            out += "\n";
        }
        return out;
    }

}

TEST_P(FileWatcherTests, CreateModifyDeleteRename) {
    startWatching();
    const fs::path file = dir.path / "assets" / "a.png";
    fs::create_directories(file.parent_path());
    ASSERT_TRUE(service.flush(timeLimit(ms(5000))));
    take();

    writeText(file, "one");
    std::vector<FileChange> got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Added);
    EXPECT_EQ(got[0].path, file);

    // (another size, so the stamp differs even on a coarse clock)
    writeText(file, "two, longer");
    got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Modified);
    EXPECT_EQ(got[0].path, file);
    EXPECT_EQ(got[0].size, 11u);

    const fs::path renamed = dir.path / "assets" / "b.png";
    fs::rename(file, renamed);
    got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Moved);
    EXPECT_EQ(got[0].path, renamed);
    EXPECT_EQ(got[0].oldPath, file);

    fs::remove(renamed);
    got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Removed);
    EXPECT_EQ(got[0].path, renamed);
}

TEST_P(FileWatcherTests, SafeSaveTempRename) {
    const fs::path target = dir.path / "tex.png";
    writeText(target, "original");
    startWatching();

    const fs::path temp = dir.path / "tex.png.tmp";
    writeText(temp, "replacement bytes");
    fs::rename(temp, target);
    const std::vector<FileChange> got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Modified);
    EXPECT_EQ(got[0].path, target);
    EXPECT_FALSE(names(got, ".tmp"));
}

TEST_P(FileWatcherTests, SafeSaveVimBackup) {
    const fs::path target = dir.path / "player.lua";
    writeText(target, "return {}");
    startWatching();

    const fs::path backup = dir.path / "player.lua~";
    fs::rename(target, backup);
    writeText(dir.path / "4913", "");
    fs::remove(dir.path / "4913");
    writeText(target, "return { speed = 2 }");
    fs::remove(backup);
    const std::vector<FileChange> got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Modified);
    EXPECT_EQ(got[0].path, target);
    EXPECT_FALSE(names(got, "~"));
    EXPECT_FALSE(names(got, "4913"));
}

TEST_P(FileWatcherTests, TruncateWrite) {
    const fs::path target = dir.path / "ui.css";
    writeText(target, "body {}");
    startWatching();

    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.flush();
        std::this_thread::sleep_for(ms(5));
        out << "body { color: red; }";
    }
    const std::vector<FileChange> got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Modified);
    EXPECT_EQ(got[0].size, 20u);
}

TEST_P(FileWatcherTests, DeleteRecreate) {
    const fs::path target = dir.path / "hero.png";
    writeText(target, "v1");
    startWatching();

    fs::remove(target);
    std::this_thread::sleep_for(ms(20));
    writeText(target, "v2 bytes");
    const std::vector<FileChange> got = collect(1);
    ASSERT_EQ(got.size(), 1u) << describe(got);
    EXPECT_EQ(got[0].kind, FileChangeKind::Modified);
    EXPECT_EQ(got[0].path, target);
}

TEST_P(FileWatcherTests, DirectoryMoveExpands) {
    const fs::path folder = dir.path / "textures";
    for (int i = 0; i < 50; i++) writeText(folder / ("t" + std::to_string(i) + ".png"), "x" + std::to_string(i));
    startWatching(true);

    const fs::path moved = dir.path / "art" ;
    fs::create_directories(moved);
    // the new folder is known before the move (a loaded host's poll may take longer than 400 ms)
    const std::vector<FileChange> made = collect(1);
    ASSERT_TRUE(names(made, "art")) << describe(made);
    fs::rename(folder, moved / "textures");
    const std::vector<FileChange> got = collect(51);
    EXPECT_EQ(countKind(got, FileChangeKind::Moved, true), 1u) << describe(got);
    EXPECT_EQ(countKind(got, FileChangeKind::Moved, false), 50u) << describe(got);
    EXPECT_EQ(got.size(), 51u) << describe(got);
}

TEST_P(FileWatcherTests, NewDirPopulatedBeforeWatch) {
    startWatching();
    // the files land before a watch on the new folder can exist
    const fs::path folder = dir.path / "fresh";
    fs::create_directories(folder);
    for (int i = 0; i < 20; i++) writeText(folder / ("f" + std::to_string(i) + ".lua"), "return " + std::to_string(i));
    const std::vector<FileChange> got = collect(20);
    EXPECT_EQ(countKind(got, FileChangeKind::Added, false), 20u) << describe(got);
    EXPECT_EQ(got.size(), 20u) << describe(got);
}

TEST_P(FileWatcherTests, OverflowEmitsRescan) {
    startWatching(true);
    // 5,000 writes whose events are lost (the backend drops them, as an overflow would)
    service.setSuppressDeliveryForTesting(true);
    for (int i = 0; i < 5000; i++) writeText(dir.path / ("bulk" + std::to_string(i / 500)) / ("f" + std::to_string(i) + ".txt"), std::to_string(i));
    service.flush(ms(2000));
    service.injectOverflowForTesting();
    service.setSuppressDeliveryForTesting(false);
    ASSERT_TRUE(waitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::any_of(changes.begin(), changes.end(), [](const FileChange& c) { return c.kind == FileChangeKind::Rescan; });
    }, ms(5000)));
    EXPECT_GE(service.stats().overflows, 1u);
    // the rescan turned the lost writes into ordinary changes, and the index is the disk
    EXPECT_TRUE(service.indexMatchesDiskForTesting());
    ASSERT_TRUE(service.flush(timeLimit(ms(5000))));
    const std::vector<FileChange> got = take();
    EXPECT_EQ(countKind(got, FileChangeKind::Added, false), 5000u);
}

TEST_P(FileWatcherTests, ExcludedFoldersCostNothing) {
    startWatching(true);
    const size_t watchesBefore = service.stats().watches;
    for (const char* name : {".git", "build", "node_modules", "Trash", ".jasperengine"}) {
        writeText(dir.path / name / "deep" / "x.png", "ignored");
    }
    ASSERT_TRUE(service.flush(timeLimit(ms(5000))));
    const std::vector<FileChange> got = take();
    EXPECT_TRUE(got.empty()) << describe(got);
    // no watch was added under them (inotify: one per folder)
    EXPECT_EQ(service.stats().watches, watchesBefore);
}

INSTANTIATE_TEST_SUITE_P(Backends, FileWatcherTests, ::testing::Values(WatchBackendKind::Native, WatchBackendKind::Polling),
                         [](const ::testing::TestParamInfo<WatchBackendKind>& info) {
                             return std::string(info.param == WatchBackendKind::Native ? "Native" : "Polling");
                         });

TEST(FileWatcherRules, Exclusions) {
    const fs::path root = "/project";
    EXPECT_TRUE(watch::isExcludedPath(root, "/project/.git/HEAD"));
    EXPECT_TRUE(watch::isExcludedPath(root, "/project/assets/Build/x.png"));
    EXPECT_TRUE(watch::isExcludedPath(root, "/project/node_modules/a.js"));
    EXPECT_TRUE(watch::isExcludedPath(root, "/project/TRASH/a.png"));
    EXPECT_TRUE(watch::isExcludedPath(root, "/project/assets/.a.png.12-1.je-tmp"));
    EXPECT_TRUE(watch::isExcludedPath(root, "/elsewhere/a.png"));
    EXPECT_TRUE(watch::isExcludedPath(root, "/projectile/a.png"));
    EXPECT_FALSE(watch::isExcludedPath(root, "/project/assets/builder/a.png"));
    EXPECT_FALSE(watch::isExcludedPath(root, "/project/assets/a.png"));
    EXPECT_FALSE(watch::isExcludedPath(root, "/project"));
}

TEST(FileWatcherRules, TemporaryNames) {
    for (const char* name : {"a.tmp", "a.TMP", "a~", "a.kra~", ".#a.lua", "a.swp", "a.swx", "4913", "a.crdownload", "a.png___jb_tmp___",
                             "a.png___jb_old___", ".a.png.123-4.je-tmp"}) {
        EXPECT_TRUE(watch::isTemporaryName(name)) << name;
    }
    for (const char* name : {"a.png", "tmp", "a.tmpl", "49130", "swap.lua"}) {
        EXPECT_FALSE(watch::isTemporaryName(name)) << name;
    }
}

TEST(FileWatcherRules, Globs) {
    EXPECT_TRUE(watch::globMatch("*.png", "a.png"));
    EXPECT_FALSE(watch::globMatch("*.png", "dir/a.png"));
    EXPECT_TRUE(watch::globMatch("**/*.png", "dir/sub/a.PNG"));
    EXPECT_TRUE(watch::globMatch("**/*.png", "a.png"));
    EXPECT_TRUE(watch::globMatch("assets/?.lua", "assets/a.lua"));
    EXPECT_FALSE(watch::globMatch("assets/?.lua", "assets/ab.lua"));
    EXPECT_TRUE(watch::globMatch("ui/**", "ui/menu/main.html"));
}

TEST(FileWatcherRules, BackendNames) {
    WatchBackendKind kind = WatchBackendKind::Auto;
    EXPECT_TRUE(parseWatchBackendKind("Polling", kind));
    EXPECT_EQ(kind, WatchBackendKind::Polling);
    EXPECT_TRUE(parseWatchBackendKind("off", kind));
    EXPECT_EQ(kind, WatchBackendKind::Off);
    EXPECT_FALSE(parseWatchBackendKind("inotify", kind));
    EXPECT_STREQ(watchBackendKindName(WatchBackendKind::Native), "native");
}

#if defined(JASPERENGINE_WATCH_FSEVENTS)
// On macOS 26 some new streams are never served: start() replaces them (S05, macOS native run)
namespace {

    // restores the seam when a test ends early
    struct DeadStreams {
        explicit DeadStreams(int count) { watch::setFsEventsDeadStreamsForTesting(count); }
        ~DeadStreams() { watch::setFsEventsDeadStreamsForTesting(0); }
    };

    void expectChangesReported(const WatchTempDir& dir, FileWatchService& service, FileWatchService::State state) {
        std::mutex mutex;
        std::vector<FileChange> changes;
        FileWatchSubscription subscription = service.subscribe({}, [&](std::span<const FileChange> batch) {
            std::lock_guard<std::mutex> lock(mutex);
            changes.insert(changes.end(), batch.begin(), batch.end());
        }, DeliverOn::Worker);
        ASSERT_TRUE(waitFor([&]() { return service.stats().indexReady; }, timeLimit(ms(10000))));
        EXPECT_EQ(service.stats().state, state);
        writeText(dir.path / "new.lua", "return 1");
        EXPECT_TRUE(waitFor([&]() {
            std::lock_guard<std::mutex> lock(mutex);
            return !changes.empty();
        }, timeLimit(ms(3000))));
    }

}

TEST(FsEventsStart, AStreamThatIsNotServedIsReplaced) {
    WatchTempDir dir;
    FileWatchService service;
    service.setWarningHandler([](const std::string&) {});
    DeadStreams dead(1);
    ASSERT_TRUE(service.start(dir.path, WatchBackendKind::Native, POLL_INTERVAL));
    expectChangesReported(dir, service, FileWatchService::State::Watching);
}

TEST(FsEventsStart, WithNoStreamServedTheServicePolls) {
    WatchTempDir dir;
    FileWatchService service;
    service.setWarningHandler([](const std::string&) {});
    DeadStreams dead(3);
    ASSERT_TRUE(service.start(dir.path, WatchBackendKind::Native, POLL_INTERVAL));
    expectChangesReported(dir, service, FileWatchService::State::Degraded);
}
#endif
