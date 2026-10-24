// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// io::quarantineOrphanedTemporaryFiles / pruneQuarantine (docs/specs/S03-data-safety.md R12, AC8)

#include "io/AtomicFile.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#if !defined(_WIN32)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace jasperengine;
using jasperengine::test::TestTempDir;

namespace {

    // A pid no process has: a child that already exited (POSIX), or a value Windows never
    // hands out (its pids are multiples of 4)
    int64_t deadPid(){
#if defined(_WIN32)
        return 4294966;
#else
        const pid_t child = ::fork();
        if (child == 0)
            ::_exit(0);
        int status = 0;
        ::waitpid(child, &status, 0);
        return (int64_t)child;
#endif
    }

    fs::path tempOf(const fs::path& target, int64_t pid, int counter){
        return target.parent_path() / ("." + target.filename().string() + "." + std::to_string(pid) + "-" + std::to_string(counter) + ".je-tmp");
    }

}

// AC8
TEST(OrphanSweep, QuarantinesDeadPid){
    TestTempDir dir("orphans");
    const fs::path project = dir.path();
    const fs::path quarantine = project / ".jasperengine/backup/orphans";
    const int64_t dead = deadPid();
    ASSERT_FALSE(io::isProcessRunning(dead));

    const fs::path deadTemp = tempOf(project / "scenes/Level1.scene", dead, 3);
    const fs::path liveTemp = tempOf(project / "scenes/Level2.scene", io::currentProcessId(), 4);
    const fs::path emptyTemp = tempOf(project / "materials/Rock.material", dead, 5);
    const fs::path plain = project / "scenes/.hidden-but-not-a-temp";
    TestTempDir::write(deadTemp, "the newest complete copy");
    TestTempDir::write(liveTemp, "being written right now");
    TestTempDir::write(emptyTemp, "");
    TestTempDir::write(plain, "x");

    const io::OrphanSweep sweep = io::quarantineOrphanedTemporaryFiles({deadTemp, liveTemp, emptyTemp, plain}, quarantine, project);

    ASSERT_EQ(sweep.quarantined.size(), 1u);
    const fs::path moved = sweep.quarantined[0];
    EXPECT_EQ(moved.parent_path(), quarantine / "scenes");
    EXPECT_EQ(moved.filename().string().rfind("Level1.scene.", 0), 0u) << moved;
    EXPECT_EQ(TestTempDir::read(moved), "the newest complete copy");
    EXPECT_FALSE(fs::exists(deadTemp));

    ASSERT_EQ(sweep.removed.size(), 1u);
    EXPECT_EQ(sweep.removed[0], emptyTemp);
    EXPECT_FALSE(fs::exists(emptyTemp));

    EXPECT_TRUE(fs::exists(liveTemp));
    EXPECT_TRUE(fs::exists(plain));

    // a second orphan of the same file in the same second keeps both copies
    const fs::path deadTemp2 = tempOf(project / "scenes/Level1.scene", dead, 6);
    TestTempDir::write(deadTemp2, "an older copy");
    const io::OrphanSweep second = io::quarantineOrphanedTemporaryFiles({deadTemp2}, quarantine, project);
    ASSERT_EQ(second.quarantined.size(), 1u);
    EXPECT_NE(second.quarantined[0], moved);
    EXPECT_EQ(TestTempDir::read(moved), "the newest complete copy");
}

TEST(OrphanSweep, PruneRemovesOldQuarantine){
    TestTempDir dir("orphans");
    const fs::path quarantine = dir / "orphans";
    TestTempDir::write(quarantine / "scenes/Old.scene.20260101-000000", "old");
    TestTempDir::write(quarantine / "Recent.scene.20261020-000000", "recent");
    fs::last_write_time(quarantine / "scenes/Old.scene.20260101-000000", fs::file_time_type::clock::now() - std::chrono::hours(24 * 8));

    EXPECT_EQ(io::pruneQuarantine(quarantine), 1u);
    EXPECT_FALSE(fs::exists(quarantine / "scenes/Old.scene.20260101-000000"));
    EXPECT_TRUE(fs::exists(quarantine / "Recent.scene.20261020-000000"));
    EXPECT_EQ(io::pruneQuarantine(dir / "missing"), 0u);
}
