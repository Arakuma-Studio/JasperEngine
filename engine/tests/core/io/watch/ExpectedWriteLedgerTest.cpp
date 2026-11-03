// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT)
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from OpenEngine's Engine/Tests/ExpectedWriteLedgerTests.cpp (NOTICE.md), plus the
// removals and folders this ledger adds and ScopedExpectedWrite on the service (S05 AC8).

#include "ExpectedWriteLedger.h"
#include "FileWatchService.h"
#include "WatchTestSupport.h"

#include <mutex>

using namespace jasperengine;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    // long enough that nothing expires by accident; the expiry test has its own ledger
    constexpr std::chrono::milliseconds TEST_ECHO_WINDOW{5000};

    struct LedgerFixture {
        WatchTempDir dir;
        ExpectedWriteLedger ledger{TEST_ECHO_WINDOW};
    };

}

// Anything the ledger was not told about is somebody else's edit
TEST(ExpectedWriteLedgerTests, AnUnannouncedChangeIsNotAnEcho) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "outside.bin";
    writeText(file, "external");
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(file));
}

// A watcher can report a file appearing before the writer reports it
TEST(ExpectedWriteLedgerTests, AChangeToAnAnnouncedPathIsAnEchoBeforeItIsReported) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "in_flight.bin";
    const auto id = f.ledger.expect(file, false);
    ASSERT_NE(id, ExpectedWriteLedger::noRegistration);
    writeText(file, "just landed");
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(file));
}

// Retiring must not end the echo window
TEST(ExpectedWriteLedgerTests, AReportedWriteIsStillAnEchoAfterItsAnnouncementRetires) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "saved.bin";
    const auto id = f.ledger.expect(file, false);
    writeText(file, "saved bytes");
    f.ledger.reported(file);
    f.ledger.retire(id);
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(file));
}

// A file this process wrote can be edited by something else a moment later
TEST(ExpectedWriteLedgerTests, AnEditAfterTheReportIsNotAnEcho) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "edited.bin";
    const auto id = f.ledger.expect(file, false);
    writeText(file, "ours");
    f.ledger.reported(file);
    f.ledger.retire(id);
    writeText(file, "somebody else's, and longer");
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(file));
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(file)) << "the record the edit retired came back";
}

// The write time is part of the identity
TEST(ExpectedWriteLedgerTests, ASameSizeRewriteIsNotAnEcho) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "same_size.bin";
    const auto id = f.ledger.expect(file, false);
    writeText(file, "aaaa");
    f.ledger.reported(file);
    f.ledger.retire(id);
    const fs::file_time_type before = fs::last_write_time(file);
    writeText(file, "bbbb");
    fs::last_write_time(file, before + std::chrono::seconds(1));
    // clock ticks: gtest would print time points with C++20 chrono formatting, unavailable
    // below macOS 13.3 (the engine's deployment target is 12.0)
    ASSERT_NE(fs::last_write_time(file).time_since_epoch().count(), before.time_since_epoch().count());
    ASSERT_EQ(fs::file_size(file), 4u);
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(file));
}

TEST(ExpectedWriteLedgerTests, TheEchoWindowExpires) {
    LedgerFixture f;
    ExpectedWriteLedger shortWindow{std::chrono::milliseconds(1)};
    const fs::path file = f.dir.path / "expiring.bin";
    const auto id = shortWindow.expect(file, false);
    writeText(file, "reported once");
    shortWindow.reported(file);
    shortWindow.retire(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(shortWindow.isEchoOfOurWrite(file));
}

TEST(ExpectedWriteLedgerTests, RetiringAWriteThatNeverLandedStopsExpectingIt) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "failed.bin";
    const auto id = f.ledger.expect(file, false);
    f.ledger.retire(id);
    writeText(file, "written by something else");
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(file));
}

// The writer's spelling and the watcher's reach one entry
TEST(ExpectedWriteLedgerTests, SpellingsOfTheSamePathMatch) {
    LedgerFixture f;
    const fs::path announced = f.dir.path / "Scenes" / "MixedCase.bin";
    const auto id = f.ledger.expect(announced, false);
#if defined(_WIN32) || defined(__APPLE__)
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(f.dir.path / "scenes" / "mixedcase.bin")) << "case";
#endif
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(f.dir.path / "Scenes" / "." / "MixedCase.bin")) << "dot segment";
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(fs::path(f.dir.path.string() + "/Scenes/Nested/../MixedCase.bin"))) << "parent segment";
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(f.dir.path / "Scenes" / "Other.bin"));
    f.ledger.retire(id);
}

TEST(ExpectedWriteLedgerTests, ASubtreeAnnouncementCoversTheFilesUnderIt) {
    LedgerFixture f;
    const fs::path destination = f.dir.path / "Pasted";
    const fs::path nested = destination / "Nested" / "leaf.bin";
    const auto id = f.ledger.expect(destination, true);
    writeText(nested, "copied");
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(nested));
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(destination));
    f.ledger.retire(id);
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(nested));
}

TEST(ExpectedWriteLedgerTests, ASubtreeAnnouncementStopsAtItsOwnDirectory) {
    LedgerFixture f;
    const fs::path destination = f.dir.path / "Mats";
    const fs::path sibling = f.dir.path / "Materials" / "unrelated.bin";
    const auto id = f.ledger.expect(destination, true);
    writeText(sibling, "not part of the paste");
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(sibling));
    f.ledger.retire(id);
}

TEST(ExpectedWriteLedgerTests, AnEditToAReportedFileUnderALiveSubtreeIsNotAnEcho) {
    LedgerFixture f;
    const fs::path destination = f.dir.path / "Pasted";
    const fs::path copied = destination / "first.bin";
    const auto id = f.ledger.expect(destination, true);
    writeText(copied, "copied");
    f.ledger.reported(copied);
    ASSERT_TRUE(f.ledger.isEchoOfOurWrite(copied));
    writeText(copied, "edited while the paste was still running");
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(copied));
    f.ledger.retire(id);
}

TEST(ExpectedWriteLedgerTests, AnEmptyPathIsNotARegistration) {
    LedgerFixture f;
    EXPECT_EQ(f.ledger.expect({}, false), ExpectedWriteLedger::noRegistration);
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite({}));
}

// The first write's identity must not decide the second write's echo
TEST(ExpectedWriteLedgerTests, ASecondWriteOfTheSamePathIsNotJudgedByTheFirstsIdentity) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "written_twice.bin";
    const auto first = f.ledger.expect(file, false);
    writeText(file, "first");
    f.ledger.reported(file);
    f.ledger.retire(first);
    const auto second = f.ledger.expect(file, false);
    writeText(file, "second, and a different length");
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(file)) << "the first write's identity outvoted the announcement of the second";
    f.ledger.reported(file);
    f.ledger.retire(second);
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(file));
}

// Added here: a reported removal is an echo while the path stays gone
TEST(ExpectedWriteLedgerTests, AReportedRemovalIsAnEchoWhileThePathStaysGone) {
    LedgerFixture f;
    const fs::path file = f.dir.path / "deleted.bin";
    writeText(file, "x");
    fs::remove(file);
    f.ledger.reportedRemoved(file);
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(file));
    writeText(file, "back, by somebody else");
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(file));
}

TEST(ExpectedWriteLedgerTests, AReportedFolderIsAnEchoWhileItIsThere) {
    LedgerFixture f;
    const fs::path folder = f.dir.path / "Renamed";
    fs::create_directories(folder);
    f.ledger.reported(folder);
    EXPECT_TRUE(f.ledger.isEchoOfOurWrite(folder));
    fs::remove(folder);
    EXPECT_FALSE(f.ledger.isEchoOfOurWrite(folder));
}

// ------------------------------------------------------------------ ScopedExpectedWrite

namespace {

    class ScopedExpectedWriteTests : public ::testing::Test {
    protected:
        WatchTempDir dir;
        FileWatchService& service = FileWatchService::get();
        std::mutex mutex;
        std::vector<FileChange> external, editor;
        FileWatchSubscription optIn, plain;

        void SetUp() override {
            if (!FileWatcher::hasNative()) GTEST_SKIP() << "no native backend here";
            service.setWarningHandler([](const std::string&) {});
            ASSERT_TRUE(service.start(dir.path, WatchBackendKind::Native));
            ASSERT_TRUE(waitFor([this]() { return service.stats().indexReady; }, std::chrono::milliseconds(5000)));
            WatchFilter withEditor;
            withEditor.deliverEditorWrites = true;
            optIn = service.subscribe(withEditor, [this](std::span<const FileChange> batch) {
                std::lock_guard<std::mutex> lock(mutex);
                for (const FileChange& c : batch) (c.origin == ChangeOrigin::Editor ? editor : external).push_back(c);
            });
            plain = service.subscribe({}, [this](std::span<const FileChange> batch) {
                std::lock_guard<std::mutex> lock(mutex);
                for (const FileChange& c : batch) {
                    EXPECT_EQ(c.origin, ChangeOrigin::External) << "an editor write reached a subscriber that did not ask";
                    external.push_back(c);
                }
            });
        }
        void TearDown() override {
            optIn.reset();
            plain.reset();
            service.stop();
        }
        // what comes in for `span`, pumping like the editor's frames
        void settle(std::chrono::milliseconds span) {
            const auto end = std::chrono::steady_clock::now() + span;
            while (std::chrono::steady_clock::now() < end) {
                service.pump(1.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            service.flush(std::chrono::milliseconds(500));
        }
    };

}

TEST_F(ScopedExpectedWriteTests, EchoDroppedEditorEventDelivered) {
    const uint64_t echoesBefore = service.stats().echoesSuppressed;
    const fs::path file = dir.path / "mat.material";
    {
        ScopedExpectedWrite write(file);
        writeText(file, "baseColor: red");
        write.reported();
    }
    settle(std::chrono::milliseconds(800));
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(editor.size(), 1u);
    EXPECT_EQ(editor[0].kind, FileChangeKind::Added);
    EXPECT_EQ(editor[0].path, file);
    EXPECT_EQ(external.size(), 0u);
    EXPECT_EQ(service.stats().echoesSuppressed, echoesBefore + 1);
}

TEST_F(ScopedExpectedWriteTests, EditAfterReportIsDelivered) {
    const fs::path file = dir.path / "mat.material";
    {
        ScopedExpectedWrite write(file);
        writeText(file, "baseColor: red");
        write.reported();
    }
    settle(std::chrono::milliseconds(400));
    // somebody else edits it inside the echo window: a change of its own
    writeText(file, "baseColor: green, and longer");
    settle(std::chrono::milliseconds(800));
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(editor.size(), 1u);
    ASSERT_GE(external.size(), 2u);   // the opt-in subscriber and the plain one
    EXPECT_EQ(external[0].kind, FileChangeKind::Modified);
}

TEST_F(ScopedExpectedWriteTests, RenameAndDeleteAreTheEditors) {
    const fs::path from = dir.path / "a.prefab";
    writeText(from, "prefab");
    settle(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lock(mutex);
        external.clear();
    }
    const fs::path to = dir.path / "b.prefab";
    {
        ScopedExpectedWrite write(to);
        fs::rename(from, to);
        write.reportedMoved(from, to);
    }
    {
        ScopedExpectedWrite write(to);
        fs::remove(to);
        write.reportedRemoved(to);
    }
    settle(std::chrono::milliseconds(800));
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(editor.size(), 2u);
    EXPECT_EQ(editor[0].kind, FileChangeKind::Moved);
    EXPECT_EQ(editor[0].oldPath, from);
    EXPECT_EQ(editor[1].kind, FileChangeKind::Removed);
    EXPECT_TRUE(external.empty());
}
