// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The FBX import queue (spec S98 D10, REQ-32, REQ-33): what is queued is pending until converted,
// a file is converted once however often it is asked for, files already current count nothing, a
// batch counts done/total and says "Importing N models (first open)" once, a failed file waits for
// a change (or a forced reimport), and the Background job converts the queue as drain() does, once
// a call of start() finds nothing new queued (the requests of a scene's load make one batch).

#include <gtest/gtest.h>

#include "LogCapture.h"
#include "TempDir.h"

#include "model/FbxImportQueue.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // A fake importer: each file needs what `needs` says (Create by default) and converts as `fails` says
    struct FakeImporter {
        std::mutex mutex;
        std::map<fs::path, FbxImportQueue::Need> needs;
        std::map<fs::path, bool> fails;
        std::map<fs::path, int> conversions;
        std::map<fs::path, bool> forced;

        FbxImportQueue::Hooks hooks() {
            FbxImportQueue::Hooks hooks;
            hooks.need = [this](const fs::path& fbx) {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = needs.find(fbx);
                return it == needs.end() ? FbxImportQueue::Need::Create : it->second;
            };
            hooks.convert = [this](const fs::path& fbx, bool force) {
                std::lock_guard<std::mutex> lock(mutex);
                conversions[fbx]++;
                forced[fbx] = force;
                if (!fails[fbx]) needs[fbx] = FbxImportQueue::Need::None;
                return !fails[fbx];
            };
            return hooks;
        }
        int count(const fs::path& fbx) {
            std::lock_guard<std::mutex> lock(mutex);
            return conversions[fbx];
        }
    };

    // lines of any level containing `text` (the editor's info lines)
    size_t importLines(const jasperengine::test::LogCapture& log, const std::string& text) {
        size_t count = 0;
        for (const auto& entry : log.entries()) {
            if (entry.message.find(text) != std::string::npos) count++;
        }
        return count;
    }

}

TEST(FbxImportQueue, AQueuedFileIsPendingUntilConverted) {
    jasperengine::test::TempDir dir;
    const fs::path a = dir.write("a.fbx", "a");
    FakeImporter importer;
    FbxImportQueue queue(importer.hooks());
    EXPECT_FALSE(queue.isPending(a));
    queue.request(a);
    EXPECT_TRUE(queue.isPending(a));
    EXPECT_TRUE(queue.progress().active);
    queue.drain();
    EXPECT_FALSE(queue.isPending(a));
    EXPECT_FALSE(queue.progress().active);
    EXPECT_EQ(importer.count(a), 1);
}

TEST(FbxImportQueue, RepeatedRequestsConvertOnce) {
    jasperengine::test::TempDir dir;
    const fs::path a = dir.write("a.fbx", "a");
    FakeImporter importer;
    FbxImportQueue queue(importer.hooks());
    queue.request(a);
    queue.request(a);
    queue.request(dir.path() / "." / "a.fbx");
    queue.drain();
    EXPECT_EQ(importer.count(a), 1);
    // a check once it is current converts nothing
    queue.request(a);
    queue.drain();
    EXPECT_EQ(importer.count(a), 1);
}

TEST(FbxImportQueue, ABatchCountsItsConversionsAndSaysSoOnce) {
    jasperengine::test::TempDir dir;
    const fs::path a = dir.write("a.fbx", "a");
    const fs::path b = dir.write("b.fbx", "b");
    const fs::path c = dir.write("c.fbx", "c");
    const fs::path current = dir.write("current.fbx", "d");
    FakeImporter importer;
    importer.needs[current] = FbxImportQueue::Need::None;
    importer.needs[c] = FbxImportQueue::Need::Update;
    FbxImportQueue queue(importer.hooks());
    jasperengine::test::LogCapture log;
    for (const fs::path& fbx : {a, b, c, current}) queue.request(fbx);
    queue.drain();
    const FbxImportQueue::Progress progress = queue.progress();
    EXPECT_EQ(progress.total, 3u) << "a current file counts no conversion";
    EXPECT_EQ(progress.done, 3u);
    EXPECT_EQ(progress.failed, 0u);
    EXPECT_EQ(importer.count(current), 0);
    // one line, counting the models not imported before (an update replaces a model shown already)
    EXPECT_EQ(importLines(log, "(first open)"), 1u);
    EXPECT_EQ(importLines(log, "Importing 2 models (first open): they appear when ready"), 1u);

    // the next batch starts its own counts
    const uint64_t batch = progress.batch;
    const fs::path e = dir.write("e.fbx", "e");
    queue.request(e);
    EXPECT_GT(queue.progress().batch, batch);
    queue.drain();
    EXPECT_EQ(queue.progress().total, 1u);
    EXPECT_EQ(importLines(log, "Importing 1 model (first open): it appears when ready"), 1u);
}

TEST(FbxImportQueue, AFailedFileWaitsForAChange) {
    jasperengine::test::TempDir dir;
    const fs::path a = dir.write("a.fbx", "a");
    FakeImporter importer;
    importer.fails[a] = true;
    FbxImportQueue queue(importer.hooks());
    queue.request(a);
    queue.drain();
    EXPECT_EQ(importer.count(a), 1);
    EXPECT_EQ(queue.progress().failed, 1u);
    EXPECT_TRUE(queue.hasFailed(a));

    queue.request(a);
    queue.drain();
    EXPECT_EQ(importer.count(a), 1) << "not tried again while the file is the same";

    queue.request(a, true);
    queue.drain();
    EXPECT_EQ(importer.count(a), 2) << "a forced reimport tries it";

    dir.write("a.fbx", "a changed");
    EXPECT_FALSE(queue.hasFailed(a));
    queue.request(a);
    queue.drain();
    EXPECT_EQ(importer.count(a), 3) << "tried again once it changed";
}

TEST(FbxImportQueue, ConvertNowRunsTheFileOnTheCallingThread) {
    jasperengine::test::TempDir dir;
    const fs::path a = dir.write("a.fbx", "a");
    const fs::path b = dir.write("b.fbx", "b");
    FakeImporter importer;
    FbxImportQueue queue(importer.hooks());
    queue.request(a);
    queue.request(b);
    EXPECT_TRUE(queue.convertNow(a, true));
    EXPECT_EQ(importer.count(a), 1);
    EXPECT_TRUE(importer.forced[a]);
    EXPECT_FALSE(queue.isPending(a));
    EXPECT_TRUE(queue.isPending(b)) << "the rest stays queued";
    importer.fails[b] = true;
    EXPECT_FALSE(queue.convertNow(b));
}

TEST(FbxImportQueue, TheJobConvertsTheQueue) {
    jasperengine::test::TempDir dir;
    const fs::path a = dir.write("a.fbx", "a");
    const fs::path b = dir.write("b.fbx", "b");
    FakeImporter importer;
    std::atomic<int> changes{0};
    FbxImportQueue::Hooks hooks = importer.hooks();
    hooks.changed = [&changes]() { changes++; };
    FbxImportQueue queue(std::move(hooks));
    queue.start();   // nothing queued: no job
    queue.request(a);
    queue.request(b);
    queue.start();   // files queued since the last call: the next one starts them
    EXPECT_TRUE(queue.isPending(a));
    queue.wait();
    EXPECT_EQ(importer.count(a), 0);
    queue.start();
    queue.wait();
    EXPECT_EQ(importer.count(a), 1);
    EXPECT_EQ(importer.count(b), 1);
    EXPECT_FALSE(queue.progress().active);
    EXPECT_EQ(queue.progress().done, 2u);
    EXPECT_GE(changes.load(), 2) << "the main thread is woken after each conversion";
}
