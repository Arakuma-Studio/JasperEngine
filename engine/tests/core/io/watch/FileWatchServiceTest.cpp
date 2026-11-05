// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// FileWatchService delivery (S05 AC6, AC7): the flush barrier, RAII subscriptions, filters,
// priorities, the main-thread budget and the journal.

#include "FileWatchService.h"
#include "WatchTestSupport.h"
#include "AllocationCounter.h"
#include "LinkedFolders.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    using ms = std::chrono::milliseconds;

    class FileWatchServiceTests : public ::testing::TestWithParam<WatchBackendKind> {
    protected:
        WatchTempDir dir;
        FileWatchService service;

        void SetUp() override {
            // no System in this binary: the warnings are the test's
            service.setWarningHandler([](const std::string&) {});
            if (GetParam() == WatchBackendKind::Native && !FileWatcher::hasNative()) GTEST_SKIP() << "no native backend here";
        }
        void startWatching() {
            ASSERT_TRUE(service.start(dir.path, GetParam(), ms(250)));
            ASSERT_TRUE(waitFor([this]() { return service.stats().indexReady; }, timeLimit(ms(5000))));
        }
    };

    std::string pathsOf(const std::vector<FileChange>& list) {
        std::string out;
        for (const FileChange& c : list) out += c.path.filename().string() + " ";
        return out;
    }

}

// S68 R8: a folder linked in from out of the root is watched as one of its folders, under the link's
// name; the link to itself inside it ends the walk; a link made while watching starts being watched
TEST_P(FileWatchServiceTests, SeesChangesThroughLinkedFolders) {
    WatchTempDir elsewhere;
    writeText(elsewhere.path / "lib" / "a.txt", "a");
    fs::create_directories(elsewhere.path / "lib" / "sub");
    std::error_code ec;
    fs::create_directory_symlink(elsewhere.path / "lib", elsewhere.path / "lib" / "loop", ec);
    fs::create_directory_symlink(elsewhere.path / "lib", dir.path / "shared", ec);
    if (ec) GTEST_SKIP() << "no symbolic links here";
    startWatching();
    EXPECT_TRUE(service.indexMatchesDiskForTesting());

    std::vector<FileChange> got;
    FileWatchSubscription subscription = service.subscribe({}, [&](std::span<const FileChange> batch) {
        got.insert(got.end(), batch.begin(), batch.end());
    });
    auto sawAt = [&](const fs::path& path, FileChangeKind kind) {
        return std::any_of(got.begin(), got.end(), [&](const FileChange& c) { return c.path == path && c.kind == kind; });
    };
    auto settled = [&](const fs::path& path, FileChangeKind kind) {
        return waitFor([&]() {
            service.flush(timeLimit(ms(1000)));
            return sawAt(path, kind);
        }, timeLimit(ms(10000)));
    };

    writeText(elsewhere.path / "lib" / "a.txt", "changed");
    EXPECT_TRUE(settled(dir.path / "shared" / "a.txt", FileChangeKind::Modified)) << pathsOf(got);
    writeText(elsewhere.path / "lib" / "sub" / "new.txt", "new");
    EXPECT_TRUE(settled(dir.path / "shared" / "sub" / "new.txt", FileChangeKind::Added)) << pathsOf(got);
    fs::remove(elsewhere.path / "lib" / "a.txt", ec);
    EXPECT_TRUE(settled(dir.path / "shared" / "a.txt", FileChangeKind::Removed)) << pathsOf(got);
    EXPECT_FALSE(std::any_of(got.begin(), got.end(), [&](const FileChange& c) { return c.path.generic_string().find("/loop/") != std::string::npos; }))
        << "the link to itself is not followed";

    // made while watching
    WatchTempDir later;
    writeText(later.path / "b.txt", "b");
    fs::create_directory_symlink(later.path, dir.path / "later", ec);
    ASSERT_FALSE(ec);
    EXPECT_TRUE(settled(dir.path / "later" / "b.txt", FileChangeKind::Added)) << pathsOf(got);
    got.clear();
    EXPECT_TRUE(waitFor([&]() {
        writeText(later.path / "b.txt", "changed " + std::to_string(got.size()));
        service.flush(timeLimit(ms(1000)));
        return sawAt(dir.path / "later" / "b.txt", FileChangeKind::Modified);
    }, timeLimit(ms(10000)))) << pathsOf(got);

    // the link removed: what it held is gone, the folder itself is not touched
    got.clear();
    fs::remove(dir.path / "later", ec);
    EXPECT_TRUE(settled(dir.path / "later" / "b.txt", FileChangeKind::Removed)) << pathsOf(got);
    EXPECT_TRUE(fs::exists(later.path / "b.txt"));
    service.stop();
}

// A link inside the root, or to a folder holding it, adds nothing to watch: the walk ends
TEST(LinkedFolders, FollowerEntersLinksOutOfTheRootOnce) {
    WatchTempDir base;
    const fs::path root = base.path / "root";
    fs::create_directories(root / "real");
    fs::create_directories(base.path / "out");
    std::error_code ec;
    fs::create_directory_symlink(root / "real", root / "inside", ec);
    fs::create_directory_symlink(base.path, root / "up", ec);
    fs::create_directory_symlink(base.path / "out", root / "first", ec);
    fs::create_directory_symlink(base.path / "out", root / "second", ec);
    fs::create_symlink(base.path / "nowhere", root / "dangling", ec);
    if (ec) GTEST_SKIP() << "no symbolic links here";
    watch::LinkFollower follower(root);
    EXPECT_TRUE(follower.enter(root / "inside").empty());
    EXPECT_TRUE(follower.enter(root / "up").empty());
    EXPECT_TRUE(follower.enter(root / "dangling").empty());
    EXPECT_EQ(follower.enter(root / "first"), base.path / "out");
    EXPECT_TRUE(follower.enter(root / "second").empty());

    // the iterator: every entry of the root once, what the entered link holds under its name
    writeText(base.path / "out" / "held.txt", "x");
    std::set<std::string> seen;
    for (watch::LinkedDirectoryIterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        seen.insert(it->path().lexically_relative(root).generic_string());
    }
    EXPECT_TRUE(seen.count("real") && seen.count("inside") && seen.count("up") && seen.count("dangling"));
    EXPECT_EQ(seen.count("first/held.txt") + seen.count("second/held.txt"), 1u) << "one link enters it";
    EXPECT_FALSE(std::any_of(seen.begin(), seen.end(), [](const std::string& path) { return path.rfind("up/", 0) == 0; }));
}

TEST_P(FileWatchServiceTests, FlushBarrier) {
    startWatching();
    std::vector<FileChange> got;
    // the test thread started the service: it is the main thread, and flush delivers here
    FileWatchSubscription subscription = service.subscribe({}, [&](std::span<const FileChange> batch) {
        got.insert(got.end(), batch.begin(), batch.end());
    });
    int misses = 0;
    for (int i = 0; i < 1000; i++) {
        const fs::path file = dir.path / ("f" + std::to_string(i) + ".txt");
        writeText(file, std::to_string(i));
        got.clear();
        ASSERT_TRUE(service.flush(timeLimit(ms(250))));
        const bool found = std::any_of(got.begin(), got.end(), [&](const FileChange& c) { return c.path == file; });
        if (!found) misses++;
    }
    EXPECT_EQ(misses, 0);
}

TEST_P(FileWatchServiceTests, FlushJournalLeavesDeliveryToThePump) {
    startWatching();
    std::vector<FileChange> got;
    FileWatchSubscription subscription = service.subscribe({}, [&](std::span<const FileChange> batch) {
        got.insert(got.end(), batch.begin(), batch.end());
    });
    const fs::path file = dir.path / "journal.txt";
    uint64_t cursor = service.journalHead();
    writeText(file, "x");
    ASSERT_TRUE(service.flushJournal(timeLimit(ms(250))));
    const std::vector<FileChange> journal = service.journalSince(cursor);
    EXPECT_TRUE(std::any_of(journal.begin(), journal.end(), [&](const FileChange& c) { return c.path == file; }));
    EXPECT_TRUE(got.empty());
    service.pump(1000.0);
    EXPECT_TRUE(std::any_of(got.begin(), got.end(), [&](const FileChange& c) { return c.path == file; }));
}

TEST_P(FileWatchServiceTests, UnsubscribeWaitsForInFlight) {
    startWatching();
    std::atomic<bool> started{false}, finished{false};
    FileWatchSubscription subscription = service.subscribe({}, [&](std::span<const FileChange>) {
        started = true;
        std::this_thread::sleep_for(ms(300));
        finished = true;
    }, DeliverOn::Worker);
    writeText(dir.path / "slow.txt", "x");
    ASSERT_TRUE(waitFor([&]() { return started.load(); }, timeLimit(ms(3000))));
    subscription.reset();
    // returned only after the callback on the dispatcher thread ended
    EXPECT_TRUE(finished);
    // and it is never called again
    started = false;
    writeText(dir.path / "after.txt", "y");
    service.flush(timeLimit(ms(500)));
    std::this_thread::sleep_for(ms(300));
    EXPECT_FALSE(started);
}

TEST_P(FileWatchServiceTests, SelfUnsubscribe) {
    startWatching();
    std::atomic<int> calls{0};
    auto holder = std::make_shared<FileWatchSubscription>();
    // (the holder is written here and read on the worker: the file write between them is no
    // happens-before edge for ThreadSanitizer, the mutex is)
    auto holderMutex = std::make_shared<std::mutex>();
    {
        std::lock_guard<std::mutex> assigning(*holderMutex);
        *holder = service.subscribe({}, [&, holder, holderMutex](std::span<const FileChange>) {
            calls++;
            std::lock_guard<std::mutex> lock(*holderMutex);
            holder->reset();   // a callback may destroy its own subscription
        }, DeliverOn::Worker);
    }
    writeText(dir.path / "one.txt", "1");
    ASSERT_TRUE(waitFor([&]() { return calls.load() == 1; }, timeLimit(ms(3000))));
    writeText(dir.path / "two.txt", "2");
    service.flush(timeLimit(ms(500)));
    std::this_thread::sleep_for(ms(300));
    EXPECT_EQ(calls.load(), 1);
}

TEST_P(FileWatchServiceTests, FilterExtensionsGlobs) {
    startWatching();
    std::vector<FileChange> images, scripts, editorOnly;
    WatchFilter imageFilter;
    imageFilter.extensions = {"png", ".SVG"};
    WatchFilter scriptFilter;
    scriptFilter.globs = {"lua/**/*.lua"};
    WatchFilter nested;
    nested.root = dir.path / "lua";
    nested.recursive = false;
    std::vector<FileChange> direct;
    FileWatchSubscription a = service.subscribe(imageFilter, [&](std::span<const FileChange> b) { images.insert(images.end(), b.begin(), b.end()); });
    FileWatchSubscription b = service.subscribe(scriptFilter, [&](std::span<const FileChange> b) { scripts.insert(scripts.end(), b.begin(), b.end()); });
    FileWatchSubscription c = service.subscribe(nested, [&](std::span<const FileChange> b) { direct.insert(direct.end(), b.begin(), b.end()); });

    writeText(dir.path / "a.png", "p");
    writeText(dir.path / "b.svg", "<svg/>");
    writeText(dir.path / "c.jpg", "j");
    writeText(dir.path / "lua" / "top.lua", "return 1");
    writeText(dir.path / "lua" / "deep" / "x.lua", "return 2");
    writeText(dir.path / "other" / "y.lua", "return 3");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    // a folder created with files in it settles in a second pass
    std::this_thread::sleep_for(ms(400));
    service.flush(timeLimit(ms(1000)));

    EXPECT_EQ(pathsOf(images), "a.png b.svg ");
    std::sort(scripts.begin(), scripts.end(), [](const FileChange& l, const FileChange& r) { return l.path < r.path; });
    EXPECT_EQ(pathsOf(scripts), "x.lua top.lua ");
    EXPECT_EQ(pathsOf(direct), "top.lua ");
}

TEST_P(FileWatchServiceTests, PriorityOrder) {
    startWatching();
    std::vector<int> order;
    FileWatchSubscription low = service.subscribe({}, [&](std::span<const FileChange>) { order.push_back(0); }, DeliverOn::MainThread, 0);
    FileWatchSubscription high = service.subscribe({}, [&](std::span<const FileChange>) { order.push_back(10); }, DeliverOn::MainThread, 10);
    FileWatchSubscription mid = service.subscribe({}, [&](std::span<const FileChange>) { order.push_back(5); }, DeliverOn::MainThread, 5);
    writeText(dir.path / "p.txt", "x");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    EXPECT_EQ(order, (std::vector<int>{10, 5, 0}));
}

TEST_P(FileWatchServiceTests, PumpBudget) {
    // 10,000 files the editor reports writing: 10,000 queued main-thread changes
    for (int i = 0; i < 10000; i++) writeText(dir.path / "many" / ("m" + std::to_string(i) + ".txt"), "x");
    startWatching();
    size_t delivered = 0;
    WatchFilter filter;
    filter.deliverEditorWrites = true;
    FileWatchSubscription subscription = service.subscribe(filter, [&](std::span<const FileChange> batch) {
        delivered += batch.size();
    });
    for (int i = 0; i < 10000; i++) service.reportEditorWrite(dir.path / "many" / ("m" + std::to_string(i) + ".txt"), true);

    // the behaviour: every change is delivered, over frames (each pump stops at its budget).
    // R22's figures (no call over 2 ms, all within 100 frames) are wall-clock ones: a sanitizer
    // or a loaded host makes each delivery slower and the same work takes more frames, so the
    // benchmark run (JASPER_TEST_PERF_SLACK, server E) enforces them
    int frames = 0;
    double longest = 0.0;
    while (delivered < 10000 && frames < 100000) {
        const auto start = std::chrono::steady_clock::now();
        service.pump(1.0);
        longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        frames++;
    }
    EXPECT_EQ(delivered, 10000u);
    std::printf("[bench] FileWatchService pump of 10,000 changes: %d frames (budget 100), longest call %.2f ms (budget 2)\n",
                frames, longest);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(frames, static_cast<int>(100.0 * slack));
        EXPECT_LE(longest, 2.0 * slack);
    }
}

TEST_P(FileWatchServiceTests, JournalCursorsAndPause) {
    startWatching();
    uint64_t cursor = service.journalHead();
    std::vector<FileChange> delivered;
    FileWatchSubscription subscription = service.subscribe({}, [&](std::span<const FileChange> b) { delivered.insert(delivered.end(), b.begin(), b.end()); });

    service.setPaused(true);
    writeText(dir.path / "while-paused.txt", "x");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    // Auto Refresh off: in the journal, not delivered
    EXPECT_TRUE(delivered.empty());
    EXPECT_EQ(service.externalChangesSince(cursor), 1u);
    std::vector<FileChange> journal = service.journalSince(cursor);
    ASSERT_EQ(journal.size(), 1u);
    EXPECT_EQ(journal[0].path.filename(), "while-paused.txt");
    EXPECT_EQ(cursor, service.journalHead());

    // the editor's own writes still go out (to the subscribers that want them)
    WatchFilter editorWrites;
    editorWrites.deliverEditorWrites = true;
    std::vector<FileChange> own;
    FileWatchSubscription mine = service.subscribe(editorWrites, [&](std::span<const FileChange> b) { own.insert(own.end(), b.begin(), b.end()); });
    writeText(dir.path / "saved.txt", "s");
    service.reportEditorWrite(dir.path / "saved.txt", false);
    service.pump(10.0);
    ASSERT_EQ(own.size(), 1u);
    EXPECT_EQ(own[0].origin, ChangeOrigin::Editor);
    EXPECT_EQ(own[0].kind, FileChangeKind::Added);
    EXPECT_TRUE(delivered.empty());

    service.setPaused(false);
    writeText(dir.path / "after.txt", "x");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    EXPECT_EQ(pathsOf(delivered), "after.txt ");
}

TEST_P(FileWatchServiceTests, AnUndeliveredEditIsNotLostToTheEditorsRewrite) {
    startWatching();
    const fs::path file = dir.path / "clip.anim";
    writeText(file, "first");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    uint64_t cursor = service.journalHead();
    // another program's edit, which the editor rewrites (a GUID line) before the watcher
    // delivered it: the editor's bytes would make its events look like the echo of that write
    std::this_thread::sleep_for(ms(20));
    writeText(file, "second, from another program");
    service.reportEditorReplacing(file);
    writeText(file, "second, from another program, with the editor's line");
    service.reportEditorWrite(file, true);
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));

    std::vector<FileChange> journal = service.journalSince(cursor);
    std::vector<ChangeOrigin> origins;
    for (const FileChange& change : journal) {
        if (change.path.filename() == "clip.anim") origins.push_back(change.origin);
    }
    ASSERT_EQ(origins.size(), 2u) << pathsOf(journal);
    EXPECT_EQ(origins[0], ChangeOrigin::External) << "the edit goes out first";
    EXPECT_EQ(origins[1], ChangeOrigin::Editor);

    // what the watcher delivered already is not sent again
    cursor = service.journalHead();
    service.reportEditorReplacing(file);
    EXPECT_TRUE(service.journalSince(cursor).empty());
}

TEST_P(FileWatchServiceTests, StoppedConsumersStartOver) {
    startWatching();
    uint64_t cursor = service.journalHead();
    writeText(dir.path / "x.txt", "x");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    service.stop();
    startWatching();
    // the old cursor points before this journal: one Rescan of the root
    std::vector<FileChange> journal = service.journalSince(cursor);
    ASSERT_EQ(journal.size(), 1u);
    EXPECT_EQ(journal[0].kind, FileChangeKind::Rescan);
    EXPECT_EQ(journal[0].path, dir.path);
}

TEST_P(FileWatchServiceTests, JournalAfterARestartReadsItsOwnChanges) {
    startWatching();
    writeText(dir.path / "before.txt", "x");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    service.stop();
    startWatching();
    // the sequence goes on past the restart: the new changes are read where they were written
    const uint64_t start = service.journalHead();
    uint64_t cursor = start;
    ASSERT_GT(cursor, 1u);
    writeText(dir.path / "after.txt", "x");
    ASSERT_TRUE(service.flush(timeLimit(ms(1000))));
    std::vector<FileChange> journal = service.journalSince(cursor);
    EXPECT_NE(pathsOf(journal).find("after.txt"), std::string::npos) << pathsOf(journal);
    EXPECT_EQ(pathsOf(journal).find("before.txt"), std::string::npos) << pathsOf(journal);
    EXPECT_GE(service.externalChangesSince(start), 1u);
}

TEST_P(FileWatchServiceTests, IdleCostsNothing) {
    startWatching();
    // let the self-test and the first pass finish
    std::this_thread::sleep_for(ms(GetParam() == WatchBackendKind::Native ? 2500 : 600));
    const FileWatchService::Stats before = service.stats();
    std::this_thread::sleep_for(ms(1000));
    const FileWatchService::Stats after = service.stats();
    EXPECT_EQ(after.events, before.events);
    EXPECT_EQ(after.delivered, before.delivered);
    EXPECT_EQ(after.pending, 0u);
    // the main thread's pump does nothing when nothing waits
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; i++) service.pump(1.0);
    const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(spent, timeLimit(5.0));
}

INSTANTIATE_TEST_SUITE_P(Backends, FileWatchServiceTests, ::testing::Values(WatchBackendKind::Native, WatchBackendKind::Polling),
                         [](const ::testing::TestParamInfo<WatchBackendKind>& info) {
                             return std::string(info.param == WatchBackendKind::Native ? "Native" : "Polling");
                         });

TEST(FileWatchServiceOff, OffAttachesForTheEditorsWritesOnly) {
    WatchTempDir dir;
    FileWatchService service;
    service.setWarningHandler([](const std::string&) {});
    EXPECT_FALSE(service.start(dir.path, WatchBackendKind::Off));
    EXPECT_TRUE(service.isAttached());
    EXPECT_FALSE(service.flush(timeLimit(ms(100))));
    EXPECT_STREQ(FileWatchService::stateName(service.stats().state), "off");
    WatchFilter filter;
    filter.deliverEditorWrites = true;
    std::vector<FileChange> got;
    FileWatchSubscription subscription = service.subscribe(filter, [&](std::span<const FileChange> b) { got.insert(got.end(), b.begin(), b.end()); });
    writeText(dir.path / "a.material", "x");
    service.reportEditorWrite(dir.path / "a.material", false);
    service.pump(5.0);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].origin, ChangeOrigin::Editor);
}
