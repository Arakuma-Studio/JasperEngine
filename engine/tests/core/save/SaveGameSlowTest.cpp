// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SaveGame tests that take seconds (docs/specs/savegame.md R13, R15; AC11, AC15's limit): a
// write stuck past the 2 s flush limit, and a process killed while it writes a 4 MiB slot.

#include "Engine.h"
#include "Log.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "save/SaveGameStore.h"

#include "LogCapture.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace SaveGameSlowTest_cpp {

using namespace jasperengine;
using jasperengine::test::LogCapture;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

TEST(SaveGameStoreSlow, FlushPastTheLimitLogsAbandoned){
    TestTempDir dir("savegame-slow");
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    store.setFolder(dir.path() / "saves");
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    // a failed assertion returns early: the injector (it refers to this frame) and the held write go
    // with it, or the next flush of the process spins in it for ever (the next test hung so)
    struct Cleanup {
        std::atomic<bool>& release;
        ~Cleanup(){
            release = true;
            io::setAtomicFileFaultInjector(nullptr);
        }
    } cleanup{release};
    io::setAtomicFileFaultInjector([&](std::string_view step, const fs::path& path) -> std::error_code {
        if (step == "write" && path.string().find(".jsave") != std::string::npos){
            entered = true;
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {};
    });
    std::string payload;
    savegame::SaveError error;
    ASSERT_TRUE(savegame::fromJson(nlohmann::json::parse(R"({"a":1})"), payload, error));
    ASSERT_TRUE(store.write("slot1", "main", payload, savegame::WriteOptions(), nullptr).ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(entered.load()) << "the worker started the write";

    LogCapture log;
    const auto started = std::chrono::steady_clock::now();
    Engine::systemPause();
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    Engine::systemResume();
    EXPECT_EQ(log.count(D_LOG_WARN, "SaveGame: \"slot1\" was not written before the app paused (2 s)"), 1u);
    EXPECT_GE(waited, 1.9);

    release = true;
    while (!fs::exists(dir.path() / "saves" / "slot1.g1.jsave") && std::chrono::steady_clock::now() < deadline + std::chrono::seconds(10)){
        jobs::drainMainQueue();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(store.flush("slot1").ok());
    io::setAtomicFileFaultInjector(nullptr);
    jobs::drainMainQueue();
    store.setFolder({});
}

#if !defined(_WIN32)
namespace {

    pid_t spawnSaveLoop(const fs::path& out){
        const std::string binary = JASPER_TEST_CRASH_TEST_BINARY;
        const std::string outText = out.string();
        char* argv[] = {const_cast<char*>(binary.c_str()), const_cast<char*>("--mode"), const_cast<char*>("savegame_loop"),
                        const_cast<char*>("--out"), const_cast<char*>(outText.c_str()), nullptr};
        pid_t pid = 0;
        if (posix_spawn(&pid, binary.c_str(), nullptr, nullptr, argv, environ) != 0) return 0;
        return pid;
    }

    uint64_t newestSaveGeneration(const fs::path& saves){
        uint64_t newest = 0;
        std::error_code ec;
        for (fs::directory_iterator it(saves, ec), end; !ec && it != end; it.increment(ec)){
            std::string slot;
            uint64_t generation = 0;
            bool corrupt = false;
            if (savegame::parseSlotFileName(it->path().filename().string(), slot, generation, corrupt) && !corrupt)
                newest = std::max(newest, generation);
        }
        return newest;
    }

}

TEST(SaveGameKillSlow, KillDuringWritesLeavesAReadableSlot){
    TestTempDir dir("savegame-kill");
    const fs::path saves = dir.path() / "saves";
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    std::mt19937 random(12345);
    std::uniform_int_distribution<int> delay(0, 120);
    for (int round = 0; round < 20; round++){
        SCOPED_TRACE("kill " + std::to_string(round + 1));
        const uint64_t before = newestSaveGeneration(saves);
        const pid_t pid = spawnSaveLoop(dir.path());
        ASSERT_GT(pid, 0);
        // writing: a generation newer than the last round's
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (newestSaveGeneration(saves) <= before && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay(random)));
        ::kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        ASSERT_TRUE(WIFSIGNALED(status)) << "the loop ran until it was killed";

        store.setFolder(saves);
        savegame::SlotData data;
        const savegame::SaveError error = store.readSlot("slot1", data);
        ASSERT_TRUE(error.ok()) << error.message;
        ASSERT_EQ(data.sections.size(), 1u);
        nlohmann::json json;
        savegame::SaveError decode;
        ASSERT_TRUE(savegame::toJson(*data.sections[0].payload, json, decode)) << decode.message;
        const int64_t i = json["i"].get<int64_t>();
        const std::string& blob = json["blob"].get_ref<const std::string&>();
        ASSERT_EQ(blob.size(), 4u * 1024u * 1024u);
        EXPECT_EQ(blob.find_first_not_of(static_cast<char>('a' + i % 26)), std::string::npos) << "a whole snapshot";
    }
    store.setFolder({});
}
#endif

}  // namespace SaveGameSlowTest_cpp
