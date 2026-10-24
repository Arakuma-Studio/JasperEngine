// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// FileWatchService delivery (S05 AC6, AC7): the flush barrier, RAII subscriptions, filters,
// priorities, the main-thread budget and the journal.

#include "FileWatchService.h"
#include "WatchTestSupport.h"

#include <algorithm>
#include <atomic>
#include <mutex>

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
            ASSERT_TRUE(waitFor([this]() { return service.stats().indexReady; }, ms(5000)));
        }
    };

    std::string pathsOf(const std::vector<FileChange>& list) {
        std::string out;
        for (const FileChange& c : list) out += c.path.filename().string() + " ";
        return out;
    }

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
        ASSERT_TRUE(service.flush(ms(250)));
        const bool found = std::any_of(got.begin(), got.end(), [&](const FileChange& c) { return c.path == file; });
        if (!found) misses++;
    }
    EXPECT_EQ(misses, 0);
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
    ASSERT_TRUE(waitFor([&]() { return started.load(); }, ms(3000)));
    subscription.reset();
    // returned only after the callback on the dispatcher thread ended
    EXPECT_TRUE(finished);
    // and it is never called again
    started = false;
    writeText(dir.path / "after.txt", "y");
    service.flush(ms(500));
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
    ASSERT_TRUE(waitFor([&]() { return calls.load() == 1; }, ms(3000)));
    writeText(dir.path / "two.txt", "2");
    service.flush(ms(500));
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
    ASSERT_TRUE(service.flush(ms(1000)));
    // a folder created with files in it settles in a second pass
    std::this_thread::sleep_for(ms(400));
    service.flush(ms(1000));

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
    ASSERT_TRUE(service.flush(ms(1000)));
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

    int frames = 0;
    double longest = 0.0;
    while (delivered < 10000 && frames < 100) {
        const auto start = std::chrono::steady_clock::now();
        service.pump(1.0);
        longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        frames++;
    }
    EXPECT_EQ(delivered, 10000u);
    EXPECT_LE(frames, 100);
    EXPECT_LE(longest, 2.0);
}

TEST_P(FileWatchServiceTests, JournalCursorsAndPause) {
    startWatching();
    uint64_t cursor = service.journalHead();
    std::vector<FileChange> delivered;
    FileWatchSubscription subscription = service.subscribe({}, [&](std::span<const FileChange> b) { delivered.insert(delivered.end(), b.begin(), b.end()); });

    service.setPaused(true);
    writeText(dir.path / "while-paused.txt", "x");
    ASSERT_TRUE(service.flush(ms(1000)));
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
    ASSERT_TRUE(service.flush(ms(1000)));
    EXPECT_EQ(pathsOf(delivered), "after.txt ");
}

TEST_P(FileWatchServiceTests, StoppedConsumersStartOver) {
    startWatching();
    uint64_t cursor = service.journalHead();
    writeText(dir.path / "x.txt", "x");
    ASSERT_TRUE(service.flush(ms(1000)));
    service.stop();
    startWatching();
    // the old cursor points before this journal: one Rescan of the root
    std::vector<FileChange> journal = service.journalSince(cursor);
    ASSERT_EQ(journal.size(), 1u);
    EXPECT_EQ(journal[0].kind, FileChangeKind::Rescan);
    EXPECT_EQ(journal[0].path, dir.path);
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
    EXPECT_LT(spent, 5.0);
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
    EXPECT_FALSE(service.flush(ms(100)));
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
