// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The coalescing rules of S05 R3/R4 with a fake clock and a fake disk (AC3): no I/O.

#include "FileChangeCoalescer.h"

#include <gtest/gtest.h>

#include <map>

using namespace jasperengine;

namespace {

    using Clock = FileChangeCoalescer::Clock;
    using ms = std::chrono::milliseconds;
    using Type = watch::RawEvent::Type;

    class FakeEnvironment : public FileChangeCoalescer::Environment {
    public:
        std::map<std::string, watch::PathStat> index;   // as last delivered
        std::map<std::string, watch::PathStat> disk;    // now
        int stats = 0;

        bool known(const std::filesystem::path& path, watch::PathStat& out) override {
            auto it = index.find(path.generic_string());
            if (it == index.end()) return false;
            out = it->second;
            return true;
        }
        bool stat(const std::filesystem::path& path, watch::PathStat& out) override {
            stats++;
            auto it = disk.find(path.generic_string());
            if (it == disk.end()) return false;
            out = it->second;
            return true;
        }

        static watch::PathStat file(uint64_t size, int64_t mtime, uint64_t inode) {
            watch::PathStat stat;
            stat.size = size;
            stat.mtimeNs = mtime;
            stat.identity.volume = 1;
            stat.identity.index = inode;
            stat.identity.valid = true;
            return stat;
        }
        static watch::PathStat folder(uint64_t inode) {
            watch::PathStat stat = file(0, 1, inode);
            stat.directory = true;
            return stat;
        }
        // a file that is there and was delivered
        void existing(const std::string& path, uint64_t size, int64_t mtime, uint64_t inode) {
            index[path] = disk[path] = file(size, mtime, inode);
        }
    };

    class FileChangeCoalescerTests : public ::testing::Test {
    protected:
        FakeEnvironment env;
        FileChangeCoalescer coalescer{env};
        Clock::time_point now = Clock::time_point() + std::chrono::hours(1);

        void raw(Type type, const std::string& path, const std::string& oldPath = {}, bool directory = false) {
            coalescer.add(watch::RawEvent{type, path, oldPath, directory}, now);
        }
        void advance(int milliseconds) { now += ms(milliseconds); }
        // takes every 10 ms for `span` ms; everything that came out
        std::vector<FileChange> run(int span = 1000) {
            std::vector<FileChange> out;
            for (int t = 0; t <= span; t += 10) {
                std::vector<FileChange> got = coalescer.take(now);
                out.insert(out.end(), got.begin(), got.end());
                advance(10);
            }
            return out;
        }
    };

    std::string kinds(const std::vector<FileChange>& changes) {
        std::string out;
        for (const FileChange& change : changes) {
            if (!out.empty()) out += ",";
            out += std::string(fileChangeKindName(change.kind)) + ":" + change.path.generic_string();
            if (change.kind == FileChangeKind::Moved) out += "<" + change.oldPath.generic_string();
        }
        return out;
    }

}

TEST_F(FileChangeCoalescerTests, AddedThenModifiedIsOneAdded) {
    env.disk["/p/a.png"] = FakeEnvironment::file(10, 100, 7);
    raw(Type::Created, "/p/a.png");
    advance(20);
    raw(Type::Modified, "/p/a.png");
    advance(20);
    raw(Type::Modified, "/p/a.png");
    EXPECT_EQ(kinds(run()), "added:/p/a.png");
}

TEST_F(FileChangeCoalescerTests, ModifiedRepeatedlyIsOneModified) {
    env.existing("/p/a.png", 10, 100, 7);
    for (int i = 0; i < 5; i++) {
        raw(Type::Modified, "/p/a.png");
        advance(30);
    }
    env.disk["/p/a.png"] = FakeEnvironment::file(20, 200, 7);
    EXPECT_EQ(kinds(run()), "modified:/p/a.png");
    EXPECT_EQ(coalescer.coalescedCount(), 4u);
}

TEST_F(FileChangeCoalescerTests, AddedThenRemovedIsNothing) {
    env.disk["/p/a.tmp2"] = FakeEnvironment::file(10, 100, 7);
    raw(Type::Created, "/p/a.tmp2");
    advance(30);
    env.disk.erase("/p/a.tmp2");
    raw(Type::Removed, "/p/a.tmp2");
    EXPECT_EQ(kinds(run()), "");
}

TEST_F(FileChangeCoalescerTests, RemovedThenAddedIsModified) {
    env.existing("/p/a.png", 10, 100, 7);
    env.disk.erase("/p/a.png");
    raw(Type::Removed, "/p/a.png");
    advance(50);
    env.disk["/p/a.png"] = FakeEnvironment::file(12, 300, 9);
    raw(Type::Created, "/p/a.png");
    EXPECT_EQ(kinds(run()), "modified:/p/a.png");
}

TEST_F(FileChangeCoalescerTests, MovedThenModifiedIsMovedAndModified) {
    env.existing("/p/a.png", 10, 100, 7);
    env.disk.erase("/p/a.png");
    env.disk["/p/b.png"] = FakeEnvironment::file(10, 100, 7);
    raw(Type::Moved, "/p/b.png", "/p/a.png");
    advance(20);
    env.disk["/p/b.png"] = FakeEnvironment::file(30, 400, 7);
    raw(Type::Modified, "/p/b.png");
    EXPECT_EQ(kinds(run()), "moved:/p/b.png</p/a.png,modified:/p/b.png");
}

TEST_F(FileChangeCoalescerTests, MovedTwiceIsOneMove) {
    env.existing("/p/a.png", 10, 100, 7);
    env.disk.erase("/p/a.png");
    raw(Type::Moved, "/p/b.png", "/p/a.png");
    advance(20);
    env.disk["/p/c.png"] = FakeEnvironment::file(10, 100, 7);
    raw(Type::Moved, "/p/c.png", "/p/b.png");
    EXPECT_EQ(kinds(run()), "moved:/p/c.png</p/a.png");
}

TEST_F(FileChangeCoalescerTests, MovedBackIsNothing) {
    env.existing("/p/a.png", 10, 100, 7);
    raw(Type::Moved, "/p/b.png", "/p/a.png");
    advance(20);
    raw(Type::Moved, "/p/a.png", "/p/b.png");
    EXPECT_EQ(kinds(run()), "");
}

TEST_F(FileChangeCoalescerTests, TempFileRenamedOverTargetIsModifiedTarget) {
    // R3 (a): write temp + rename over the target (VS Code, Photoshop, os.replace)
    env.existing("/p/a.png", 10, 100, 7);
    env.disk["/p/a.png.tmp"] = FakeEnvironment::file(20, 200, 9);
    raw(Type::Created, "/p/a.png.tmp");
    advance(10);
    raw(Type::Modified, "/p/a.png.tmp");
    advance(10);
    env.disk.erase("/p/a.png.tmp");
    env.disk["/p/a.png"] = FakeEnvironment::file(20, 200, 9);
    raw(Type::Moved, "/p/a.png", "/p/a.png.tmp");
    EXPECT_EQ(kinds(run()), "modified:/p/a.png");
}

TEST_F(FileChangeCoalescerTests, RenameFromAnUnseenTempIsModifiedTarget) {
    // the temp's own events were excluded (.a.png.12-3.je-tmp): only the rename's target is seen
    env.existing("/p/a.png", 10, 100, 7);
    env.disk["/p/a.png"] = FakeEnvironment::file(20, 200, 9);
    raw(Type::Created, "/p/a.png");
    EXPECT_EQ(kinds(run()), "modified:/p/a.png");
}

TEST_F(FileChangeCoalescerTests, VimBackupSequenceIsModified) {
    // R3 (b): rename the target to a backup, write a new target, delete the backup
    env.existing("/p/a.lua", 10, 100, 7);
    env.disk.erase("/p/a.lua");
    env.disk["/p/a.lua~"] = FakeEnvironment::file(10, 100, 7);
    raw(Type::Moved, "/p/a.lua~", "/p/a.lua");
    advance(5);
    env.disk["/p/4913"] = FakeEnvironment::file(0, 150, 11);
    raw(Type::Created, "/p/4913");
    env.disk.erase("/p/4913");
    raw(Type::Removed, "/p/4913");
    advance(5);
    env.disk["/p/a.lua"] = FakeEnvironment::file(12, 300, 12);
    raw(Type::Created, "/p/a.lua");
    raw(Type::Modified, "/p/a.lua");
    advance(5);
    env.disk.erase("/p/a.lua~");
    raw(Type::Removed, "/p/a.lua~");
    EXPECT_EQ(kinds(run()), "modified:/p/a.lua");
}

TEST_F(FileChangeCoalescerTests, TruncateAndWriteIsModified) {
    // R3 (c)
    env.existing("/p/a.css", 10, 100, 7);
    env.disk["/p/a.css"] = FakeEnvironment::file(0, 150, 7);
    raw(Type::Modified, "/p/a.css");
    advance(5);
    env.disk["/p/a.css"] = FakeEnvironment::file(40, 160, 7);
    raw(Type::Modified, "/p/a.css");
    const std::vector<FileChange> out = run();
    ASSERT_EQ(kinds(out), "modified:/p/a.css");
    EXPECT_EQ(out[0].size, 40u);
}

TEST_F(FileChangeCoalescerTests, DeleteAndRecreateWithinTheHoldIsModified) {
    // R3 (d)
    env.existing("/p/a.png", 10, 100, 7);
    env.disk.erase("/p/a.png");
    raw(Type::Removed, "/p/a.png");
    advance(180);   // inside the 200 ms hold
    EXPECT_EQ(kinds(coalescer.take(now)), "");
    env.disk["/p/a.png"] = FakeEnvironment::file(10, 500, 8);
    raw(Type::Created, "/p/a.png");
    EXPECT_EQ(kinds(run()), "modified:/p/a.png");
}

TEST_F(FileChangeCoalescerTests, RemovalWaitsForTheDeleteHold) {
    env.existing("/p/a.png", 10, 100, 7);
    env.disk.erase("/p/a.png");
    raw(Type::Removed, "/p/a.png");
    advance(190);
    EXPECT_EQ(kinds(coalescer.take(now)), "");
    advance(15);
    EXPECT_EQ(kinds(coalescer.take(now)), "removed:/p/a.png");
}

TEST_F(FileChangeCoalescerTests, NothingBeforeTheQuietWindowThenTwoStats) {
    env.existing("/p/a.png", 10, 100, 7);
    env.disk["/p/a.png"] = FakeEnvironment::file(20, 200, 7);
    raw(Type::Modified, "/p/a.png");
    advance(149);
    EXPECT_EQ(kinds(coalescer.take(now)), "");
    advance(1);
    // the first settle stat
    EXPECT_EQ(kinds(coalescer.take(now)), "");
    advance(49);
    EXPECT_EQ(kinds(coalescer.take(now)), "");
    advance(1);
    // the second, 50 ms later, agrees
    EXPECT_EQ(kinds(coalescer.take(now)), "modified:/p/a.png");
}

TEST_F(FileChangeCoalescerTests, StatsThatDisagreeWaitForTheFileToSettle) {
    env.existing("/p/big.fbx", 10, 100, 7);
    raw(Type::Modified, "/p/big.fbx");
    advance(150);
    // growing between every pair of stats for a second
    int64_t size = 100;
    for (int i = 0; i < 20; i++) {
        env.disk["/p/big.fbx"] = FakeEnvironment::file(static_cast<uint64_t>(size += 100), 200 + i, 7);
        EXPECT_EQ(kinds(coalescer.take(now)), "");
        advance(50);
    }
    EXPECT_EQ(kinds(run(200)), "modified:/p/big.fbx");
}

TEST_F(FileChangeCoalescerTests, GrowingFileIsDeliveredAfterTheCap) {
    env.existing("/p/big.fbx", 10, 100, 7);
    raw(Type::Modified, "/p/big.fbx");
    std::vector<FileChange> out;
    // it never stops growing; after 5 s it comes out anyway
    for (int t = 0; t <= 6000; t += 10) {
        env.disk["/p/big.fbx"] = FakeEnvironment::file(static_cast<uint64_t>(1000 + t), 1000 + t, 7);
        std::vector<FileChange> got = coalescer.take(now);
        if (!got.empty() && out.empty()) EXPECT_GE(t, 5000);
        out.insert(out.end(), got.begin(), got.end());
        advance(10);
    }
    EXPECT_EQ(kinds(out), "modified:/p/big.fbx");
}

TEST_F(FileChangeCoalescerTests, RemovalAndAdditionOfOneIdentityAreOneMove) {
    // the polling backend and FSEvents see a rename as two changes
    env.existing("/p/a.prefab", 10, 100, 7);
    env.disk.erase("/p/a.prefab");
    env.disk["/p/b.prefab"] = FakeEnvironment::file(10, 100, 7);
    raw(Type::Removed, "/p/a.prefab");
    raw(Type::Created, "/p/b.prefab");
    EXPECT_EQ(kinds(run()), "moved:/p/b.prefab</p/a.prefab");
}

TEST_F(FileChangeCoalescerTests, TemporaryNamesNeverComeOut) {
    for (const char* name : {"/p/x.tmp", "/p/x~", "/p/x.swp", "/p/x.swx", "/p/4913", "/p/x.crdownload", "/p/x___jb_tmp___",
                             "/p/x___jb_old___", "/p/x.kra~"}) {
        env.disk[name] = FakeEnvironment::file(1, 1, 99);
        raw(Type::Created, name);
    }
    EXPECT_EQ(kinds(run()), "");
}

TEST_F(FileChangeCoalescerTests, RenamedToABackupNameAndNotReplacedIsRemoved) {
    env.existing("/p/a.lua", 10, 100, 7);
    env.disk.erase("/p/a.lua");
    env.disk["/p/a.lua~"] = FakeEnvironment::file(10, 100, 7);
    raw(Type::Moved, "/p/a.lua~", "/p/a.lua");
    EXPECT_EQ(kinds(run()), "removed:/p/a.lua");
}

TEST_F(FileChangeCoalescerTests, ChangedIsDecidedByTheDiskAndTheIndex) {
    // FSEvents: cumulative flags, so each path is "something happened here"
    env.existing("/p/known.png", 10, 100, 7);
    env.disk["/p/known.png"] = FakeEnvironment::file(11, 101, 7);
    env.disk["/p/new.png"] = FakeEnvironment::file(5, 50, 8);
    env.existing("/p/gone.png", 3, 30, 9);
    env.disk.erase("/p/gone.png");
    raw(Type::Changed, "/p/known.png");
    raw(Type::Changed, "/p/new.png");
    raw(Type::Changed, "/p/gone.png");
    raw(Type::Changed, "/p/never.png");   // came and went before the stat
    EXPECT_EQ(kinds(run()), "modified:/p/known.png,added:/p/new.png,removed:/p/gone.png");
}

TEST_F(FileChangeCoalescerTests, ForceTakesEverythingWithoutWindows) {
    env.existing("/p/a.png", 10, 100, 7);
    env.disk["/p/a.png"] = FakeEnvironment::file(20, 200, 7);
    env.disk["/p/b.png"] = FakeEnvironment::file(5, 50, 8);
    env.existing("/p/c.png", 3, 30, 9);
    env.disk.erase("/p/c.png");
    raw(Type::Modified, "/p/a.png");
    raw(Type::Created, "/p/b.png");
    raw(Type::Removed, "/p/c.png");
    EXPECT_EQ(kinds(coalescer.take(now, true)), "modified:/p/a.png,added:/p/b.png,removed:/p/c.png");
    EXPECT_EQ(coalescer.pendingCount(), 0u);
}

TEST_F(FileChangeCoalescerTests, RescanPassesThroughAtOnce) {
    raw(Type::Rescan, "/p");
    raw(Type::Rescan, "/p");
    EXPECT_EQ(coalescer.nextDeadline(), Clock::time_point::min());
    EXPECT_EQ(kinds(coalescer.take(now)), "rescan:/p");
}

TEST_F(FileChangeCoalescerTests, FolderRenameCarriesPendingChangesAlong) {
    env.index["/p/old"] = env.disk["/p/old"] = FakeEnvironment::folder(5);
    env.existing("/p/old/a.png", 10, 100, 7);
    raw(Type::Modified, "/p/old/a.png");
    advance(10);
    env.disk.erase("/p/old");
    env.disk.erase("/p/old/a.png");
    env.disk["/p/new"] = FakeEnvironment::folder(5);
    env.disk["/p/new/a.png"] = FakeEnvironment::file(20, 200, 7);
    raw(Type::Moved, "/p/new", "/p/old", true);
    // the folder first, then what changed in it under its new name
    EXPECT_EQ(kinds(run()), "moved:/p/new</p/old,modified:/p/new/a.png");
}

TEST_F(FileChangeCoalescerTests, IdleHasNoDeadline) {
    EXPECT_EQ(coalescer.nextDeadline(), Clock::time_point::max());
    EXPECT_EQ(env.stats, 0);
}
