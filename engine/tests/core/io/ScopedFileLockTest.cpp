// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// io::ScopedFileLock (docs/specs/S03-data-safety.md R10, R11, AC5)

#include "io/ScopedFileLock.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#if !defined(_WIN32)
    #include <csignal>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace jasperengine;
using jasperengine::test::TestTempDir;

TEST(ScopedFileLock, ExclusiveExcludesUntilReleased){
    TestTempDir dir("lock");
    const fs::path lockFile = dir / "user/editor.lock";
    {
        io::ScopedFileLock first(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
        ASSERT_TRUE(first.isLocked()) << first.failureReason();
        EXPECT_TRUE(first.failureReason().empty());

        io::ScopedFileLock second(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
        EXPECT_FALSE(second.isLocked());
        EXPECT_EQ(second.getStatus(), io::ScopedFileLock::Status::Busy);
        EXPECT_NE(second.failureReason().find("could not lock"), std::string::npos);
    }
    io::ScopedFileLock again(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
    EXPECT_TRUE(again.isLocked()) << again.failureReason();
    again.release();
    EXPECT_FALSE(again.isLocked());
    io::ScopedFileLock afterRelease(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
    EXPECT_TRUE(afterRelease.isLocked());
}

TEST(ScopedFileLock, SharedLocksCoexist){
    TestTempDir dir("lock");
    const fs::path lockFile = dir / "pin.lock";
    io::ScopedFileLock a(lockFile, io::ScopedFileLock::Mode::Shared, false);
    io::ScopedFileLock b(lockFile, io::ScopedFileLock::Mode::Shared, false);
    EXPECT_TRUE(a.isLocked());
    EXPECT_TRUE(b.isLocked());
    io::ScopedFileLock exclusive(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
    EXPECT_EQ(exclusive.getStatus(), io::ScopedFileLock::Status::Busy);
}

TEST(ScopedFileLock, OwnerInfoReadableWhileLocked){
    TestTempDir dir("lock");
    const fs::path lockFile = dir / "editor.lock";
    io::ScopedFileLock lock(lockFile);
    ASSERT_TRUE(lock.isLocked());
    ASSERT_TRUE(lock.writeOwnerInfo("{\"pid\": 1, \"host\": \"a long host name\"}"));
    ASSERT_TRUE(lock.writeOwnerInfo("{\"pid\": 2}"));
    EXPECT_EQ(io::ScopedFileLock::readOwnerInfo(lockFile), "{\"pid\": 2}");
}

#if !defined(_WIN32)
// AC5: a child process holds the lock; the parent sees it busy with the owner's text, then
// takes it once the child is killed (the system releases a dead owner's lock)
TEST(ScopedFileLock, CrossProcess){
    TestTempDir dir("lock");
    const fs::path lockFile = dir / ".jasperengine/user/editor.lock";
    int ready[2];
    ASSERT_EQ(::pipe(ready), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0){
        ::close(ready[0]);
        io::ScopedFileLock lock(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
        const bool ok = lock.isLocked() && lock.writeOwnerInfo("{\"pid\": " + std::to_string(::getpid()) + "}");
        const char status = ok ? '1' : '0';
        if (::write(ready[1], &status, 1) != 1)
            ::_exit(2);
        for (;;)
            ::pause();
    }
    ::close(ready[1]);
    char status = 0;
    ASSERT_EQ(::read(ready[0], &status, 1), 1);
    ::close(ready[0]);
    ASSERT_EQ(status, '1');

    {
        io::ScopedFileLock attempt(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
        EXPECT_FALSE(attempt.isLocked());
        EXPECT_EQ(attempt.getStatus(), io::ScopedFileLock::Status::Busy);
        EXPECT_EQ(io::ScopedFileLock::readOwnerInfo(lockFile), "{\"pid\": " + std::to_string(child) + "}");
    }

    ::kill(child, SIGKILL);
    int childStatus = 0;
    ::waitpid(child, &childStatus, 0);

    io::ScopedFileLock after(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
    EXPECT_TRUE(after.isLocked()) << after.failureReason();
}
#endif
