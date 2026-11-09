// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// AssetIOService (S06 AC8). The cases are ported from OpenEngine's
// Tests/Assets/AssetIOServiceTests.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"

#include "JobsTestUtil.h"
#include "TestPack.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::jobs;
namespace fs = std::filesystem;

namespace{

    // ResourcePack opens resources.pak (working directory first) once, at the first packed
    // lookup, which may come from the engine's start in main(): write it before main runs
    [[maybe_unused]] const bool testPackWritten = [](){
        testsupport::writeTestPack("resources.pak");
        return true;
    }();

    class TempDir {
    public:
        TempDir(){
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("je-asset-io-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(path);
        }
        ~TempDir(){
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        std::string write(const std::string& name, const std::string& content) const{
            const fs::path file = path / name;
            std::ofstream out(file, std::ios::binary);
            out << content;
            return file.string();
        }
        std::string file(const std::string& name) const{ return (path / name).string(); }

    private:
        fs::path path;
    };

    std::string asString(const std::vector<uint8_t>& bytes){
        return std::string(bytes.begin(), bytes.end());
    }

    // callbacks: exactly one fires per request
    struct Outcome {
        std::atomic<int> submitted{0};
        std::atomic<int> failed{0};
        std::mutex mutex;
        std::string error;

        void attach(AssetIOService::ReadRequest& request){
            request.onSubmitted = [this](const TaskRef&){ submitted++; };
            request.onFailure = [this](const std::string& message){
                std::lock_guard<std::mutex> lock(mutex);
                error = message;
                failed++;
            };
        }
        int total() const{ return submitted.load() + failed.load(); }
    };

    class AssetIO : public ::testing::TestWithParam<int> {
    protected:
        // GetParam(): reader threads (0 = inline mode, on a single-thread system)
        void SetUp() override{
            system = std::make_unique<JobSystem>(testsupport::configFor(GetParam() == 0 ? 0 : 4));
            service = std::make_unique<AssetIOService>(system.get());
            service->start(GetParam());
        }
        void TearDown() override{
            service->stop();
            service.reset();
            system.reset();
        }

        template<class T>
        void finish(Task<T>& task){
            if (system->isSingleThreaded()){
                for (int i = 0; i < 1000 && !task.isReady(); i++)
                    system->pump(4.0);
            }
            task.wait();
        }

        std::unique_ptr<JobSystem> system;
        std::unique_ptr<AssetIOService> service;
        TempDir dir;
    };
    INSTANTIATE_TEST_SUITE_P(Readers, AssetIO, ::testing::Values(0, 1, 2),
        [](const ::testing::TestParamInfo<int>& info){ return "readers" + std::to_string(info.param); });
}

TEST_P(AssetIO, ReadsFileBytesIntoDecodeJob){
    AssetIOService::ReadRequest request;
    request.path = dir.write("a.txt", "texture bytes");
    Outcome outcome;
    outcome.attach(request);
    Task<std::string> task = service->submitRead(request, [](std::vector<uint8_t>&& bytes){ return asString(bytes); });
    finish(task);
    EXPECT_EQ(task.get(), "texture bytes");
    EXPECT_EQ(outcome.submitted.load(), 1);
    EXPECT_EQ(outcome.failed.load(), 0);
}

TEST_P(AssetIO, ReadsOnlyTheRequestedByteRange){
    AssetIOService::ReadRequest request;
    request.path = dir.write("range.bin", "0123456789abcdef");
    request.offset = 4;
    request.size = 6;
    Task<std::string> task = service->submitRead(request, [](std::vector<uint8_t>&& bytes){ return asString(bytes); });
    finish(task);
    EXPECT_EQ(task.get(), "456789");
}

TEST_P(AssetIO, RangePastTheEndOfTheFileFails){
    AssetIOService::ReadRequest request;
    request.path = dir.write("short.bin", "0123");
    request.offset = 2;
    request.size = 10;
    Outcome outcome;
    outcome.attach(request);
    std::atomic<bool> decoded{false};
    Task<int> task = service->submitRead(request, [&](std::vector<uint8_t>&&){ decoded = true; return 1; });
    finish(task);
    EXPECT_EQ(task.status(), TaskStatus::Failed);
    EXPECT_THROW(task.get(), AssetIOError);
    EXPECT_FALSE(decoded.load());
    EXPECT_EQ(outcome.total(), 1);
    if (GetParam() > 0){
        EXPECT_EQ(outcome.failed.load(), 1);
        EXPECT_NE(outcome.error.find("Short read"), std::string::npos);
    }
}

TEST_P(AssetIO, ReadFailureResolvesOnFailure){
    AssetIOService::ReadRequest request;
    request.path = dir.file("missing.png");
    Outcome outcome;
    outcome.attach(request);
    Task<int> task = service->submitRead(request, [](std::vector<uint8_t>&&){ return 1; });
    finish(task);
    EXPECT_EQ(task.status(), TaskStatus::Failed);
    EXPECT_EQ(outcome.total(), 1) << "exactly one of onSubmitted/onFailure";
}

TEST_P(AssetIO, RetryHalfWrittenFile){
    // the file appears (as a hot-reload rename does) inside the 200 ms retry window
    const std::string path = dir.file("late.txt");
    std::thread writer([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        dir.write("late.tmp", "arrived");
        fs::rename(dir.file("late.tmp"), path);
    });
    AssetIOService::ReadRequest request;
    request.path = path;
    Task<std::string> task = service->submitRead(request, [](std::vector<uint8_t>&& bytes){ return asString(bytes); });
    finish(task);
    writer.join();
    EXPECT_EQ(task.get(), "arrived");
}

TEST_P(AssetIO, EmptyFileIsRetriedThenFails){
    AssetIOService::ReadRequest request;
    request.path = dir.write("empty.txt", "");
    Task<int> task = service->submitRead(request, [](std::vector<uint8_t>&&){ return 1; });
    const auto start = std::chrono::steady_clock::now();
    finish(task);
    EXPECT_EQ(task.status(), TaskStatus::Failed);
    EXPECT_GE(testsupport::elapsedMs(start), 150.0) << "an empty file gets the retry window first";
}

TEST_P(AssetIO, ReadsResourcePack){
    AssetIOService::ReadRequest request;
    request.path = testsupport::testPackPath();   // packed only: no such file on disk
    ASSERT_FALSE(fs::exists(fs::current_path() / testsupport::testPackPath()));
    Task<std::string> task = service->submitRead(request, [](std::vector<uint8_t>&& bytes){ return asString(bytes); });
    finish(task);
    EXPECT_EQ(task.get(), testsupport::testPackContent());
}

TEST_P(AssetIO, SubmitAfterStopFailsInline){
    service->stop();
    AssetIOService::ReadRequest request;
    request.path = dir.write("after.txt", "x");
    Outcome outcome;
    outcome.attach(request);
    Task<int> task = service->submitRead(request, [](std::vector<uint8_t>&&){ return 1; });
    EXPECT_EQ(outcome.failed.load(), 1) << "fails on the calling thread";
    finish(task);
    EXPECT_NE(task.status(), TaskStatus::Completed);
}

// ---- with reader threads only

class AssetIOReaders : public ::testing::Test {
protected:
    void SetUp() override{
        Config config = testsupport::configFor(4);
        config.reserveForFrame = 2;
        system = std::make_unique<JobSystem>(config);
        service = std::make_unique<AssetIOService>(system.get());
    }
    void TearDown() override{
        service->stop();
        service.reset();
        system.reset();
    }
    std::unique_ptr<JobSystem> system;
    std::unique_ptr<AssetIOService> service;
    TempDir dir;
};

TEST_F(AssetIOReaders, HigherPriorityReadsClaimedFirstUnderSaturatedQueue){
    // queue everything before a reader exists, then start one reader on a 1-slot gate
    system->getGate().setMaxOverride(1);
    std::mutex mutex;
    std::vector<int> order;
    std::vector<Task<void>> tasks;
    service->start(1);
    // the reader claims while the gate admits: hold the slot so the queue fills first
    ASSERT_TRUE(system->getGate().tryAcquire(GateClass::Default));
    const IOPriority priorities[] = {IOPriority::Low, IOPriority::Normal, IOPriority::Critical, IOPriority::High};
    for (int i = 0; i < 8; i++){
        AssetIOService::ReadRequest request;
        request.path = dir.write("p" + std::to_string(i) + ".txt", "x");
        request.priority = priorities[i % 4];
        const int level = (int)request.priority;
        tasks.push_back(service->submitRead(request, [&, level](std::vector<uint8_t>&&){
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(level);
        }));
    }
    EXPECT_EQ(service->stats().queuedByPriority[(size_t)IOPriority::Critical], 2u);
    system->getGate().release(GateClass::Default);
    for (auto& task : tasks)
        ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 10000.0));
    ASSERT_EQ(order.size(), 8u);
    EXPECT_TRUE(std::is_sorted(order.begin(), order.end(), std::greater<int>())) << "highest priority first";
}

TEST_F(AssetIOReaders, StopFailsQueuedReadsAndJoinsThreads){
    service->start(1);
    system->getGate().setMaxOverride(1);
    ASSERT_TRUE(system->getGate().tryAcquire(GateClass::Default));
    Outcome outcomes[5];
    std::vector<Task<int>> tasks;
    for (int i = 0; i < 5; i++){
        AssetIOService::ReadRequest request;
        request.path = dir.write("s" + std::to_string(i), "x");
        outcomes[i].attach(request);
        tasks.push_back(service->submitRead(request, [](std::vector<uint8_t>&&){ return 1; }));
    }
    service->stop();
    system->getGate().release(GateClass::Default);
    for (int i = 0; i < 5; i++){
        EXPECT_EQ(outcomes[i].failed.load(), 1);
        EXPECT_EQ(outcomes[i].submitted.load(), 0);
        EXPECT_EQ(tasks[(size_t)i].status(), TaskStatus::Cancelled);
    }
    EXPECT_EQ(service->stats().readers, 0);
}

TEST_F(AssetIOReaders, CancelRemovesQueuedReadByFlagIdentity){
    service->start(1);
    system->getGate().setMaxOverride(1);
    ASSERT_TRUE(system->getGate().tryAcquire(GateClass::Default));
    const std::string path = dir.write("shared.txt", "x");
    AssetIOService::ReadRequest older;
    older.path = path;
    Outcome olderOutcome;
    olderOutcome.attach(older);
    AssetIOService::ReadRequest newer;
    newer.path = path;   // a fresh token: another load generation
    Outcome newerOutcome;
    newerOutcome.attach(newer);
    const CancelToken olderFlag = older.cancel;
    Task<int> olderTask = service->submitRead(older, [](std::vector<uint8_t>&&){ return 1; });
    Task<int> newerTask = service->submitRead(newer, [](std::vector<uint8_t>&&){ return 2; });
    EXPECT_EQ(service->cancel(path, olderFlag), 1u);
    EXPECT_EQ(olderTask.status(), TaskStatus::Cancelled);
    EXPECT_EQ(olderOutcome.failed.load(), 1);
    system->getGate().release(GateClass::Default);
    ASSERT_TRUE(testsupport::spinUntil([&]{ return newerTask.isReady(); }, 10000.0));
    EXPECT_EQ(newerTask.get(), 2) << "the newer generation was not withdrawn";
    EXPECT_EQ(newerOutcome.submitted.load(), 1);
}

TEST_F(AssetIOReaders, CancelledClaimedReadIsDiscardedBeforeDecode){
    service->start(1);
    // a large file so the read is in progress when the token is cancelled
    AssetIOService::ReadRequest request;
    request.path = dir.write("big.bin", std::string(64 * 1024 * 1024, 'z'));
    Outcome outcome;
    outcome.attach(request);
    CancelToken token = request.cancel;
    std::atomic<bool> decoded{false};
    Task<int> task = service->submitRead(request, [&](std::vector<uint8_t>&&){ decoded = true; return 1; });
    ASSERT_TRUE(testsupport::spinUntil([&]{
        for (const auto& read : service->stats().inFlight)
            if (read.claimed) return true;
        return task.isReady();
    }, 10000.0));
    token.cancel();
    ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 10000.0));
    if (outcome.failed.load() == 1){
        EXPECT_EQ(task.status(), TaskStatus::Cancelled);
        EXPECT_FALSE(decoded.load());
    }
    EXPECT_EQ(outcome.total(), 1);
}

TEST_F(AssetIOReaders, CancelRacingAReaderClaimStillFinishes){
    // A cancel that lands between the reader's liveness check and its claim of the slot leaves
    // the reader a slot to give back; the wait for the cancel sweeps across the reader's wake-up
    // so that some rounds land there. A hang here is the reader stuck on its own mutex.
    service->start(2);
    const std::string path = dir.write("race.txt", "x");
    testsupport::runWithWatchdog(30.0, "CancelRacingAReaderClaimStillFinishes", [&]{
        for (int round = 0; round < 400; round++){
            AssetIOService::ReadRequest request;
            request.path = path;
            Task<int> task = service->submitRead(request, [](std::vector<uint8_t>&&){ return 1; });
            testsupport::spinFor((double)(round % 64) * 5.0);
            task.cancel();
            ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 10000.0));
        }
        service->stop();
    });
    EXPECT_TRUE(testsupport::spinUntil([&]{ return system->getGate().taken() == 0; }, 10000.0)) << "every slot came back";
}

TEST_F(AssetIOReaders, AWaitRunsAQueuedReadItself){
    // the gate is full (the waiter may hold the slot itself): the wait reads and decodes inline
    service->start(1);
    system->getGate().setMaxOverride(1);
    ASSERT_TRUE(system->getGate().tryAcquire(GateClass::Default));
    AssetIOService::ReadRequest request;
    request.path = dir.write("wait.txt", "waited");
    Task<std::string> task = service->submitRead(request, [](std::vector<uint8_t>&& bytes){ return asString(bytes); });
    // no reader can claim it (no free slot): only the waiter's own read completes it
    testsupport::runWithWatchdog(10.0, "AWaitRunsAQueuedReadItself", [&]{
        EXPECT_EQ(task.get(), "waited");
    });
    system->getGate().release(GateClass::Default);
}

TEST_F(AssetIOReaders, DecodeHoldsAGateSlot){
    service->start(2);
    system->getGate().setMaxOverride(2);
    std::atomic<int> running{0};
    std::atomic<int> maximum{0};
    std::vector<Task<void>> tasks;
    for (int i = 0; i < 20; i++){
        AssetIOService::ReadRequest request;
        request.path = dir.write("g" + std::to_string(i), "x");
        tasks.push_back(service->submitRead(request, [&](std::vector<uint8_t>&&){
            const int now = ++running;
            int seen = maximum.load();
            while (now > seen && !maximum.compare_exchange_weak(seen, now)){
            }
            testsupport::spinFor(2000.0);
            running--;
        }));
    }
    for (auto& task : tasks)
        ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 20000.0));
    EXPECT_LE(maximum.load(), 2);
    EXPECT_EQ(system->getGate().taken(), 0) << "every slot came back";
}
