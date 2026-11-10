// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// io::writeFileAtomic / publishFile / renameWithRetry (docs/specs/S03-data-safety.md R1-R8,
// AC1-AC4)

#include "io/AtomicFile.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace jasperengine;
using jasperengine::test::TestTempDir;

namespace {

    // Removes the fault injector even when an assertion returns early
    struct FaultInjectorGuard {
        explicit FaultInjectorGuard(io::AtomicFileFaultInjector injector){ io::setAtomicFileFaultInjector(std::move(injector)); }
        ~FaultInjectorGuard(){ io::setAtomicFileFaultInjector(nullptr); }
    };

    // Fails the `nth` (1-based) call of `step`
    io::AtomicFileFaultInjector failNth(std::string step, int nth, std::error_code error, std::shared_ptr<int> calls = std::make_shared<int>(0)){
        return [=](std::string_view current, const fs::path&) -> std::error_code {
            if (current != step)
                return {};
            (*calls)++;
            return *calls == nth ? error : std::error_code();
        };
    }

    std::vector<fs::path> temporarySiblings(const fs::path& dir){
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(dir)){
            if (io::isTemporarySiblingName(entry.path().filename().string()))
                found.push_back(entry.path());
        }
        return found;
    }

    std::string payload(size_t size, char seed){
        std::string bytes(size, '\0');
        for (size_t i = 0; i < size; i++)
            bytes[i] = (char)('a' + (seed + i) % 26);
        return bytes;
    }

}

TEST(AtomicFile, WritesNewFileAndCreatesDirectories){
    TestTempDir dir("atomic");
    const fs::path target = dir / "a/b/c.txt";
    const io::WriteResult result = io::writeFileAtomic(target, "hello");
    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(result.error, io::WriteError::None);
    EXPECT_EQ(result.attempts, 1);
    EXPECT_EQ(TestTempDir::read(target), "hello");
    EXPECT_TRUE(temporarySiblings(target.parent_path()).empty());
}

TEST(AtomicFile, WithoutCreateDirectoriesAMissingFolderFails){
    TestTempDir dir("atomic");
    io::AtomicWriteOptions options;
    options.createDirectories = false;

    const fs::path missing = dir / "gone/c.txt";
    const io::WriteResult refused = io::writeFileAtomic(missing, "hello", options);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.error, io::WriteError::MissingDirectory);
    EXPECT_EQ(refused.code, std::errc::no_such_file_or_directory);
    EXPECT_EQ(io::describeWriteError(refused), "its folder does not exist");
    EXPECT_STREQ(io::writeErrorName(refused.error), "missing_directory");
    EXPECT_FALSE(fs::exists(dir / "gone"));

    // a streamed temp is not published there either, and goes
    const fs::path temp = io::makeTemporarySiblingPath(dir / "c.txt");
    TestTempDir::write(temp, "streamed");
    const io::WriteResult unpublished = io::publishFile(temp, missing, options);
    EXPECT_EQ(unpublished.error, io::WriteError::MissingDirectory);
    EXPECT_FALSE(fs::exists(temp));
    EXPECT_FALSE(fs::exists(dir / "gone"));

    // an existing folder takes the file as usual
    const fs::path present = dir / "c.txt";
    ASSERT_TRUE(io::writeFileAtomic(present, "hello", options).ok());
    EXPECT_EQ(TestTempDir::read(present), "hello");
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
}

TEST(AtomicFile, ReplacesExistingBytes){
    TestTempDir dir("atomic");
    const fs::path target = dir / "doc.scene";
    TestTempDir::write(target, "old content that is longer than the new one");
    ASSERT_TRUE(io::writeFileAtomic(target, "new").ok());
    EXPECT_EQ(TestTempDir::read(target), "new");
    ASSERT_TRUE(io::writeFileAtomic(target, "", {io::Durability::Cache}).ok());
    EXPECT_EQ(TestTempDir::read(target), "");
}

// AC1: every failing step leaves the previous bytes and no temporary sibling
TEST(AtomicFile, FailureKeepsTarget){
    TestTempDir dir("atomic");
    const fs::path target = dir / "level.scene";
    const std::string oldBytes = payload(1000, 1);
    // two write chunks: failing the second one fails the write at 50%
    const std::string newBytes = payload(2 * io::kWriteChunkSize, 2);

    struct Step { const char* name; int nth; io::WriteError expected; };
    const Step steps[] = {
        {"open", 1, io::WriteError::OpenTemp},
        {"write", 2, io::WriteError::WriteTemp},
        {"sync", 1, io::WriteError::SyncTemp},
    };
    for (const Step& step : steps){
        for (const bool existed : {true, false}){
            SCOPED_TRACE(std::string(step.name) + (existed ? " (existing target)" : " (new target)"));
            std::error_code ec;
            fs::remove(target, ec);
            if (existed)
                TestTempDir::write(target, oldBytes);
            FaultInjectorGuard guard(failNth(step.name, step.nth, std::make_error_code(std::errc::no_space_on_device)));
            const io::WriteResult result = io::writeFileAtomic(target, newBytes);
            EXPECT_FALSE(result.ok());
            EXPECT_EQ(result.error, step.expected);
            EXPECT_EQ(result.code, std::errc::no_space_on_device);
            EXPECT_EQ(io::describeWriteError(result), "the disk is full");
            if (existed){
                EXPECT_EQ(TestTempDir::read(target), oldBytes);
            }
            else{
                EXPECT_FALSE(fs::exists(target));
            }
            EXPECT_TRUE(temporarySiblings(dir.path()).empty());
        }
    }

    // the rename fails all five attempts
    TestTempDir::write(target, oldBytes);
    auto calls = std::make_shared<int>(0);
    FaultInjectorGuard guard([calls](std::string_view step, const fs::path&) -> std::error_code {
        if (step != "rename")
            return {};
        (*calls)++;
        return std::make_error_code(std::errc::device_or_resource_busy);
    });
    io::AtomicWriteOptions options;
    options.retryDelay = std::chrono::milliseconds(1);
    const io::WriteResult result = io::writeFileAtomic(target, newBytes, options);
    EXPECT_EQ(result.error, io::WriteError::Replace);
    EXPECT_EQ(result.attempts, 5);
    EXPECT_EQ(*calls, 5);
    EXPECT_EQ(TestTempDir::read(target), oldBytes);
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
}

// AC2: a busy target is retried 200 ms apart until the rename goes through
TEST(AtomicFile, RetryThenSucceeds){
    TestTempDir dir("atomic");
    const fs::path target = dir / "settings.yaml";
    TestTempDir::write(target, "old");
    auto calls = std::make_shared<int>(0);
    FaultInjectorGuard guard([calls](std::string_view step, const fs::path&) -> std::error_code {
        if (step != "rename")
            return {};
        return ++(*calls) <= 2 ? std::make_error_code(std::errc::device_or_resource_busy) : std::error_code();
    });
    const auto start = std::chrono::steady_clock::now();
    const io::WriteResult result = io::writeFileAtomic(target, "new");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(result.attempts, 3);
    EXPECT_GE(elapsed, std::chrono::milliseconds(400));
    EXPECT_EQ(TestTempDir::read(target), "new");
}

#if defined(_WIN32)
// AC2 on Windows: another process holding the target open without FILE_SHARE_DELETE
TEST(AtomicFile, RetryWhileTargetHeldOpen){
    TestTempDir dir("atomic");
    const fs::path target = dir / "held.yaml";
    TestTempDir::write(target, "old");
    HANDLE held = ::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    std::thread closer([held]{
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ::CloseHandle(held);
    });
    const io::WriteResult result = io::writeFileAtomic(target, "new");
    closer.join();
    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_GE(result.attempts, 2);
    EXPECT_EQ(TestTempDir::read(target), "new");
}
#endif

TEST(AtomicFile, NonRetryableErrorFailsAtOnce){
    TestTempDir dir("atomic");
    const fs::path target = dir / "x.txt";
    TestTempDir::write(target, "old");
    FaultInjectorGuard guard(failNth("rename", 1, std::make_error_code(std::errc::cross_device_link)));
    const auto start = std::chrono::steady_clock::now();
    const io::WriteResult result = io::writeFileAtomic(target, "new");
    EXPECT_EQ(result.error, io::WriteError::Replace);
    EXPECT_EQ(result.attempts, 1);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(150));
    EXPECT_EQ(TestTempDir::read(target), "old");
}

TEST(AtomicFile, CancelStopsRetries){
    TestTempDir dir("atomic");
    const fs::path target = dir / "x.txt";
    TestTempDir::write(target, "old");
    auto calls = std::make_shared<int>(0);
    FaultInjectorGuard guard([calls](std::string_view step, const fs::path&) -> std::error_code {
        if (step != "rename")
            return {};
        (*calls)++;
        return std::make_error_code(std::errc::permission_denied);
    });
    io::AtomicWriteOptions options;
    options.retryDelay = std::chrono::milliseconds(1);
    options.shouldCancel = [calls]{ return *calls >= 2; };
    const io::WriteResult result = io::writeFileAtomic(target, "new", options);
    EXPECT_EQ(result.error, io::WriteError::Cancelled);
    EXPECT_EQ(*calls, 2);
    EXPECT_EQ(TestTempDir::read(target), "old");
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
}

// AC3
TEST(AtomicFile, ReadOnlyRefused){
    TestTempDir dir("atomic");
    const fs::path target = dir / "locked.prefab";
    TestTempDir::write(target, "checked in");
    fs::permissions(target, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove);
    const auto mtime = fs::last_write_time(target);
    const io::WriteResult result = io::writeFileAtomic(target, "edited");
    EXPECT_EQ(result.error, io::WriteError::ReadOnlyTarget);
    EXPECT_EQ(result.attempts, 0);
    EXPECT_EQ(TestTempDir::read(target), "checked in");
    // clock ticks: gtest would print time points with C++20 chrono formatting, unavailable
    // below macOS 13.3 (the engine's deployment target is 12.0)
    EXPECT_EQ(fs::last_write_time(target).time_since_epoch().count(), mtime.time_since_epoch().count());
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
    EXPECT_NE(io::describeWriteError(result).find("read-only"), std::string::npos);
}

#if !defined(_WIN32)
TEST(AtomicFile, ModePreserved){
    TestTempDir dir("atomic");
    const fs::path target = dir / "tool.sh";
    TestTempDir::write(target, "#!/bin/sh\n");
    ASSERT_EQ(::chmod(target.c_str(), 0755), 0);
    ASSERT_TRUE(io::writeFileAtomic(target, "#!/bin/sh\necho hi\n").ok());
    struct stat st;
    ASSERT_EQ(::stat(target.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0755u);
}
#endif

TEST(AtomicFile, SymlinkPreserved){
    TestTempDir dir("atomic");
    const fs::path real = dir / "real/scene.scene";
    const fs::path link = dir / "link.scene";
    TestTempDir::write(real, "old");
    std::error_code ec;
    fs::create_symlink(fs::path("real") / "scene.scene", link, ec);
    if (ec)
        GTEST_SKIP() << "symbolic links unavailable: " << ec.message();
    ASSERT_TRUE(io::writeFileAtomic(link, "new").ok());
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
    EXPECT_EQ(TestTempDir::read(real), "new");
    EXPECT_EQ(TestTempDir::read(link), "new");
}

TEST(AtomicFile, UnchangedKeepsMtime){
    TestTempDir dir("atomic");
    const fs::path target = dir / "cache.yaml";
    TestTempDir::write(target, "same");
    const auto past = fs::file_time_type::clock::now() - std::chrono::hours(5);
    fs::last_write_time(target, past);
    io::AtomicWriteOptions options;
    options.skipIfUnchanged = true;
    const io::WriteResult same = io::writeFileAtomic(target, "same", options);
    EXPECT_EQ(same.error, io::WriteError::Unchanged);
    EXPECT_TRUE(same.ok());
    EXPECT_FALSE(same.written());
    EXPECT_EQ(fs::last_write_time(target).time_since_epoch().count(), past.time_since_epoch().count());   // ticks, as above

    const io::WriteResult changed = io::writeFileAtomic(target, "different", options);
    EXPECT_EQ(changed.error, io::WriteError::None);
    EXPECT_EQ(TestTempDir::read(target), "different");
}

// AC4: readers only ever see one complete payload
TEST(AtomicFile, ConcurrentWriters){
    TestTempDir dir("atomic");
    const fs::path target = dir / "shared.yaml";
    constexpr int kThreads = 8;
    constexpr int kWrites = 200;
    constexpr size_t kBody = 3000;

    // "<thread>:<write>:" then a body derived from both, so a mix or a truncation shows
    auto make = [](int thread, int write){
        std::string bytes = std::to_string(thread) + ":" + std::to_string(write) + ":";
        bytes += payload(kBody, (char)(thread * 7 + write));
        return bytes;
    };
    ASSERT_TRUE(io::writeFileAtomic(target, make(0, 0), {io::Durability::Cache}).ok());

    std::atomic<bool> done{false};
    std::atomic<int> badReads{0};
    std::atomic<int> reads{0};
    std::thread reader([&]{
        while (!done.load()){
            const std::string bytes = TestTempDir::read(target);
            if (bytes.empty())
                continue; // Windows: the file can be momentarily unopenable during a replace
            reads++;
            const size_t first = bytes.find(':');
            const size_t second = bytes.find(':', first + 1);
            if (first == std::string::npos || second == std::string::npos){
                badReads++;
                continue;
            }
            const int thread = std::stoi(bytes.substr(0, first));
            const int write = std::stoi(bytes.substr(first + 1, second - first - 1));
            if (bytes != make(thread, write))
                badReads++;
        }
    });

    std::atomic<int> failedWrites{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; t++){
        writers.emplace_back([&, t]{
            io::AtomicWriteOptions options;
            options.durability = io::Durability::Cache;
            options.retryDelay = std::chrono::milliseconds(5);
            options.attempts = 50;
            for (int w = 0; w < kWrites; w++){
                if (!io::writeFileAtomic(target, make(t, w), options).ok())
                    failedWrites++;
            }
        });
    }
    for (std::thread& writer : writers)
        writer.join();
    done = true;
    reader.join();

    EXPECT_EQ(failedWrites.load(), 0);
    EXPECT_EQ(badReads.load(), 0);
    EXPECT_GT(reads.load(), 0);
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
}

TEST(AtomicFile, TemporarySiblingNames){
    const fs::path target = fs::path("scenes") / "Level 1.scene";
    const fs::path temp = io::makeTemporarySiblingPath(target);
    const std::string name = temp.filename().string();
    EXPECT_EQ(temp.parent_path(), target.parent_path());
    EXPECT_EQ(name.rfind(".Level 1.scene.", 0), 0u) << name;
    EXPECT_TRUE(io::isTemporarySiblingName(name));
    EXPECT_EQ(io::temporarySiblingWriter(name), io::currentProcessId());
    EXPECT_EQ(io::temporarySiblingTarget(temp), target);
    EXPECT_NE(io::makeTemporarySiblingPath(target), temp);

    EXPECT_TRUE(io::isTemporarySiblingName(".a.b.c.123-4.je-tmp"));
    EXPECT_EQ(io::temporarySiblingTarget(fs::path(".a.b.c.123-4.je-tmp")), fs::path("a.b.c"));
    for (const char* other : {"Level1.scene", ".Level1.scene", "Level1.scene.12-3.je-tmp", ".x.12.je-tmp",
            ".x.-3.je-tmp", ".x.12-.je-tmp", ".x.a2-3.je-tmp", ".x.12-3.tmp", ".12-3.je-tmp"}){
        EXPECT_FALSE(io::isTemporarySiblingName(other)) << other;
        EXPECT_EQ(io::temporarySiblingWriter(other), -1) << other;
    }
}

TEST(AtomicFile, PublishFileOfStreamedTemp){
    TestTempDir dir("atomic");
    const fs::path target = dir / "pack.bin";
    TestTempDir::write(target, "old");
    const fs::path temp = io::makeTemporarySiblingPath(target);
    TestTempDir::write(temp, "streamed bytes");
    const io::WriteResult result = io::publishFile(temp, target);
    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(TestTempDir::read(target), "streamed bytes");
    EXPECT_FALSE(fs::exists(temp));

    // a failed publish removes the temp and keeps the target
    const fs::path temp2 = io::makeTemporarySiblingPath(target);
    TestTempDir::write(temp2, "never published");
    FaultInjectorGuard guard(failNth("sync", 1, std::make_error_code(std::errc::io_error)));
    EXPECT_EQ(io::publishFile(temp2, target).error, io::WriteError::SyncTemp);
    EXPECT_EQ(TestTempDir::read(target), "streamed bytes");
    EXPECT_FALSE(fs::exists(temp2));
}

// A streamed write of more than one chunk (S03: same rules as writeFileAtomic)
TEST(AtomicFile, WriterPublishesThePiecesAtCommit){
    TestTempDir dir("atomic_writer");
    const fs::path target = dir.path() / "sub" / "out.bin";
    const std::string first = payload(io::kWriteChunkSize + 37, 'a');
    const std::string second = payload(1234, 'b');
    io::AtomicFileWriter writer;
    ASSERT_TRUE(writer.open(target));
    EXPECT_FALSE(fs::exists(target));
    EXPECT_TRUE(fs::exists(writer.temporaryPath()));
    ASSERT_TRUE(writer.write(first.data(), first.size()));
    ASSERT_TRUE(writer.write(second.data(), second.size()));
    EXPECT_TRUE(writer.commit().written());
    EXPECT_EQ(TestTempDir::read(target), first + second);
    EXPECT_TRUE(temporarySiblings(target.parent_path()).empty());
}

TEST(AtomicFile, WriterDiscardedKeepsTheTarget){
    TestTempDir dir("atomic_writer");
    const fs::path target = dir.path() / "out.bin";
    ASSERT_TRUE(io::writeFileAtomic(target, "before").ok());
    {
        io::AtomicFileWriter writer;
        ASSERT_TRUE(writer.open(target));
        ASSERT_TRUE(writer.write("after", 5));
        // no commit: the writer going away must not touch the target
    }
    EXPECT_EQ(TestTempDir::read(target), "before");
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
}

TEST(AtomicFile, WriterFailureDropsLaterPiecesAndKeepsTheTarget){
    TestTempDir dir("atomic_writer");
    const fs::path target = dir.path() / "out.bin";
    ASSERT_TRUE(io::writeFileAtomic(target, "before").ok());
    io::AtomicFileWriter writer;
    ASSERT_TRUE(writer.open(target));
    {
        FaultInjectorGuard guard(failNth("write", 1, std::make_error_code(std::errc::no_space_on_device)));
        EXPECT_FALSE(writer.write("x", 1));
    }
    EXPECT_EQ(writer.result().error, io::WriteError::WriteTemp);
    EXPECT_FALSE(writer.write("y", 1));
    EXPECT_FALSE(writer.commit().ok());
    EXPECT_EQ(TestTempDir::read(target), "before");
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
}

TEST(AtomicFile, WriterKeepsTheFileReadableWhileOpen){
    TestTempDir dir("atomic_writer");
    const fs::path target = dir.path() / "out.bin";
    io::AtomicFileWriter writer;
    ASSERT_TRUE(writer.open(target));
    ASSERT_TRUE(writer.write("12345", 5));
    EXPECT_EQ(TestTempDir::read(writer.temporaryPath()), "12345");
    writer.discard();
    EXPECT_FALSE(fs::exists(target));
    EXPECT_TRUE(temporarySiblings(dir.path()).empty());
}

TEST(AtomicFile, PublishListenerSeesEveryPublish){
    TestTempDir dir("atomic");
    const fs::path target = dir / "watched.png";
    std::vector<std::pair<fs::path, uintmax_t>> published;
    io::setPublishListener([&](const fs::path& path, uintmax_t size, fs::file_time_type){
        published.emplace_back(path, size);
    });
    ASSERT_TRUE(io::writeFileAtomic(target, "12345").ok());
    io::AtomicWriteOptions options;
    options.skipIfUnchanged = true;
    ASSERT_TRUE(io::writeFileAtomic(target, "12345", options).ok());
    io::setPublishListener(nullptr);
    ASSERT_TRUE(io::writeFileAtomic(target, "123").ok());

    ASSERT_EQ(published.size(), 1u);
    EXPECT_EQ(published[0].first, target);
    EXPECT_EQ(published[0].second, 5u);
}

TEST(AtomicFile, RenameWithRetryMovesFiles){
    TestTempDir dir("atomic");
    TestTempDir::write(dir / "a.txt", "a");
    std::error_code ec;
    EXPECT_TRUE(io::renameWithRetry(dir / "a.txt", dir / "b.txt", ec)) << ec.message();
    EXPECT_EQ(TestTempDir::read(dir / "b.txt"), "a");
    EXPECT_FALSE(io::renameWithRetry(dir / "missing.txt", dir / "c.txt", ec));
    EXPECT_EQ(ec, std::errc::no_such_file_or_directory);
}

TEST(AtomicFile, ProcessIds){
    EXPECT_GT(io::currentProcessId(), 0);
    EXPECT_TRUE(io::isProcessRunning(io::currentProcessId()));
    EXPECT_FALSE(io::isProcessRunning(0));
    EXPECT_FALSE(io::isProcessRunning(-5));
}
