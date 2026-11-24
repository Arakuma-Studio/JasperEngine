// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using jasperengine::test::TestTempDir;

namespace {

    fs::perms modeOf(const fs::path& path){
        return fs::status(path).permissions() & fs::perms::mask;
    }

}

// The cleanup makes read-only entries writable so it can remove them, and must never do it through a
// symlink a test planted: its target is outside the test's directory (PublishingTest's link to
// /etc/passwd hung a macOS job in fchmodat; on a developer's machine it would change their files)
TEST(TestTempDir, CleanupLeavesWhatItsSymlinksPointAtAlone){
    TestTempDir outside("tempdir-outside");
    const fs::path file = outside / "read-only.txt";
    const fs::path folder = outside / "read-only-folder";
    TestTempDir::write(file, "theirs");
    TestTempDir::write(folder / "inner.txt", "theirs");
    fs::permissions(file, fs::perms::owner_read, fs::perm_options::replace);
    fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    const fs::perms fileBefore = modeOf(file), folderBefore = modeOf(folder), innerBefore = modeOf(folder / "inner.txt");

    fs::path removed;
    {
        TestTempDir dir("tempdir-links");
        removed = dir.path();
        fs::create_directories(dir / "sub");
        std::error_code ec;
        fs::create_symlink(file, dir / "file-link", ec);
        if (!ec)
            fs::create_directory_symlink(folder, dir / "sub" / "folder-link", ec);
        if (ec){
            fs::permissions(folder, fs::perms::owner_all, fs::perm_options::add);
            GTEST_SKIP() << "symbolic links unavailable: " << ec.message();
        }
        // what the cleanup is for: a read-only file of the test's own still goes
        TestTempDir::write(dir / "sub" / "own.txt", "mine");
        fs::permissions(dir / "sub" / "own.txt", fs::perms::owner_read, fs::perm_options::replace);
    }

    EXPECT_FALSE(fs::exists(removed));
    EXPECT_EQ(modeOf(file), fileBefore);
    EXPECT_EQ(modeOf(folder), folderBefore);
    EXPECT_EQ(modeOf(folder / "inner.txt"), innerBefore);
    EXPECT_EQ(TestTempDir::read(file), "theirs");
    EXPECT_EQ(TestTempDir::read(folder / "inner.txt"), "theirs");
    // (outside's own cleanup needs to enter it)
    fs::permissions(folder, fs::perms::owner_all, fs::perm_options::add);
}
