// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SaveGame store (docs/specs/savegame.md R3-R17, R29, R30; AC8-AC10, AC12-AC17): generations,
// recovery, coalescing, flushes and caps.

#include "Engine.h"
#include "Log.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "save/SaveGameStore.h"

#include "LogCapture.h"
#include "TestSystem.h"
#include "support/AllocationCounter.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


using namespace jasperengine;
using jasperengine::test::LogCapture;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace {

    std::string storePayload(const std::string& json){
        std::string payload;
        savegame::SaveError error;
        EXPECT_TRUE(savegame::fromJson(nlohmann::json::parse(json), payload, error)) << error.message;
        return payload;
    }

    std::string storeJson(const std::string& payload){
        nlohmann::json json;
        savegame::SaveError error;
        EXPECT_TRUE(savegame::toJson(payload, json, error)) << error.message;
        return json.dump();
    }

    template<class F>
    bool drainUntil(F condition, std::chrono::milliseconds limit = std::chrono::seconds(10)){
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!condition()){
            if (std::chrono::steady_clock::now() > deadline) return false;
            jobs::drainMainQueue();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    std::vector<std::string> storeFileNames(const fs::path& dir){
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) names.push_back(it->path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }

    // A store on a fresh folder, back to the defaults afterwards
    class SaveGameStoreTest : public ::testing::Test {
    protected:
        TestTempDir dir{"savegame"};
        savegame::SaveGameStore& store = savegame::SaveGameStore::instance();

        void SetUp() override {
            savegame::configure(savegame::SaveSettings());
            savegame::setGameDataVersion(1);
            store.setFolder(dir.path() / "saves");
        }

        void TearDown() override {
            store.setHold(false);
            io::setAtomicFileFaultInjector(nullptr);
            store.flush();
            jobs::drainMainQueue();
            store.setFolder({});
            savegame::configure(savegame::SaveSettings());
            savegame::setGameDataVersion(1);
        }

        savegame::SaveError writeAndFlush(const std::string& slot, const std::string& json, const std::string& nameSpace = "main",
                                      savegame::WriteOptions options = savegame::WriteOptions()){
            if (options.version == 1) options.version = savegame::gameDataVersion();
            const savegame::SaveError error = store.write(slot, nameSpace, storePayload(json), options, nullptr);
            if (!error.ok()) return error;
            return store.flush(slot);
        }

        std::string readJson(const std::string& slot, const std::string& nameSpace = "main", savegame::ReadInfo* infoOut = nullptr){
            std::string payload;
            savegame::ReadInfo info;
            const savegame::SaveError error = store.read(slot, nameSpace, payload, info);
            if (infoOut) *infoOut = info;
            if (!error.ok()) return std::string("error ") + savegame::errorCodeName(error.code) + ": " + error.message;
            return storeJson(payload);
        }
    };

}

TEST_F(SaveGameStoreTest, KeepsBackupsKeptPlusOneGenerations){
    for (int i = 1; i <= 10; i++) { ASSERT_TRUE(writeAndFlush("slot1", "{\"i\":" + std::to_string(i) + "}").ok()); }
    EXPECT_EQ(storeFileNames(dir.path() / "saves"), (std::vector<std::string>{"slot1.g10.jsave", "slot1.g8.jsave", "slot1.g9.jsave"}));

    savegame::SaveSettings none;
    none.backups = 0;
    savegame::configure(none);
    ASSERT_TRUE(writeAndFlush("slot1", R"({"i":11})").ok());
    EXPECT_EQ(storeFileNames(dir.path() / "saves"), (std::vector<std::string>{"slot1.g11.jsave"}));
    store.unload();
    EXPECT_EQ(readJson("slot1"), R"({"i":11})");
}

TEST_F(SaveGameStoreTest, RecoversFromNewestValidGeneration){
    const fs::path saves = dir.path() / "saves";
    for (savegame::CorruptMode mode : {savegame::CorruptMode::Truncate, savegame::CorruptMode::Flip, savegame::CorruptMode::Header}){
        SCOPED_TRACE(static_cast<int>(mode));
        store.remove("slot1");
        store.reset();
        store.setFolder(saves);
        for (int i = 1; i <= 10; i++) { ASSERT_TRUE(writeAndFlush("slot1", "{\"i\":" + std::to_string(i) + "}").ok()); }
        std::string file;
        ASSERT_TRUE(store.corrupt("slot1", mode, file).ok());
        EXPECT_EQ(file, "slot1.g10.jsave");

        LogCapture log;
        savegame::ReadInfo info;
        EXPECT_EQ(readJson("slot1", "main", &info), R"({"i":9})");
        EXPECT_EQ(info.recoveredFrom, 9u);
        ASSERT_EQ(info.problems.size(), 1u);
        EXPECT_EQ(info.problems[0].rfind("slot1.g10.jsave: ", 0), 0u) << info.problems[0];
        EXPECT_EQ(log.count(D_LOG_WARN, "recovered from generation 9"), 1u);
        EXPECT_TRUE(fs::exists(saves / "slot1.g10.jsave.corrupt"));
        EXPECT_FALSE(fs::exists(saves / "slot1.g10.jsave"));
        // read again: no second warning, and list says what happened
        readJson("slot1");
        EXPECT_EQ(log.count(D_LOG_WARN, "recovered from generation"), 1u);
        const std::vector<savegame::SlotInfo> slots = store.list();
        ASSERT_EQ(slots.size(), 1u);
        EXPECT_EQ(slots[0].status, "recovered");
        // the next write continues above the set-aside generation
        ASSERT_TRUE(writeAndFlush("slot1", R"({"i":11})").ok());
        EXPECT_TRUE(fs::exists(saves / "slot1.g11.jsave"));
    }

    // every generation corrupt: corrupt, and every file stays where it is
    store.remove("slot1");
    for (int i = 1; i <= 3; i++) { ASSERT_TRUE(writeAndFlush("slot1", "{\"i\":" + std::to_string(i) + "}").ok()); }
    for (const std::string& name : storeFileNames(saves)){
        std::ofstream(saves / name, std::ios::binary | std::ios::trunc) << "garbage";
    }
    const std::vector<std::string> before = storeFileNames(saves);
    store.unload();
    const std::string result = readJson("slot1");
    EXPECT_EQ(result.rfind("error corrupt: ", 0), 0u) << result;
    EXPECT_EQ(storeFileNames(saves), before);
}

TEST_F(SaveGameStoreTest, CrashAtEveryWriteStepLeavesAReadableSlot){
    const fs::path saves = dir.path() / "saves";
    for (const char* step : {"open", "write", "sync", "rename"}){
        SCOPED_TRACE(step);
        store.remove("slot1");
        ASSERT_TRUE(writeAndFlush("slot1", R"({"v":"old"})").ok());
        io::setAtomicFileFaultInjector([step](std::string_view at, const fs::path&) -> std::error_code {
            if (at == step) return std::make_error_code(std::errc::io_error);
            return {};
        });
        const savegame::SaveError error = writeAndFlush("slot1", R"({"v":"new"})");
        io::setAtomicFileFaultInjector(nullptr);
        EXPECT_EQ(error.code, savegame::SaveErrorCode::Io) << error.message;
        // what the disk holds: the old generation, whole
        store.unload();
        EXPECT_EQ(readJson("slot1"), R"({"v":"old"})");
        for (const std::string& name : storeFileNames(saves)) { EXPECT_EQ(name.find("je-tmp"), std::string::npos) << name; }
    }

    // a crash between publishing a generation and pruning: one file too many, all readable, the
    // next write prunes it
    store.remove("slot1");
    for (int i = 1; i <= 3; i++) { ASSERT_TRUE(writeAndFlush("slot1", "{\"i\":" + std::to_string(i) + "}").ok()); }
    fs::copy_file(saves / "slot1.g3.jsave", saves / "slot1.g4.jsave");
    store.reset();
    store.setFolder(saves);
    EXPECT_EQ(readJson("slot1"), R"({"i":3})");
    ASSERT_TRUE(writeAndFlush("slot1", R"({"i":5})").ok());
    EXPECT_EQ(storeFileNames(saves), (std::vector<std::string>{"slot1.g3.jsave", "slot1.g4.jsave", "slot1.g5.jsave"}));
}

TEST_F(SaveGameStoreTest, CoalescesWrites){
    store.setHold(true);
    const size_t writesBefore = store.fileWrites();
    std::vector<int> order;
    std::vector<bool> results;
    for (int i = 0; i < 100; i++){
        savegame::WriteOptions options;
        const savegame::SaveError error = store.write("slot1", "main", storePayload("{\"i\":" + std::to_string(i) + "}"), options,
            [&order, &results, i](const savegame::SaveError& result){ order.push_back(i); results.push_back(result.ok()); });
        ASSERT_TRUE(error.ok()) << error.message;
    }
    store.setHold(false);
    ASSERT_TRUE(drainUntil([&]{ return order.size() == 100; }));
    EXPECT_EQ(store.fileWrites() - writesBefore, 2u) << "the first snapshot and the last";
    for (int i = 0; i < 100; i++) { EXPECT_EQ(order[i], i); }
    for (bool ok : results) { EXPECT_TRUE(ok); }
    store.unload();
    EXPECT_EQ(readJson("slot1"), R"({"i":99})");
    EXPECT_EQ(storeFileNames(dir.path() / "saves"), (std::vector<std::string>{"slot1.g1.jsave", "slot1.g2.jsave"}));
}

TEST_F(SaveGameStoreTest, NamespaceWriteKeepsOtherNamespaces){
    ASSERT_TRUE(writeAndFlush("slot1", R"({"level":"forest"})").ok());
    ASSERT_TRUE(writeAndFlush("slot1", R"(["sword"])", "inventory").ok());
    ASSERT_TRUE(writeAndFlush("slot1", R"({"level":"cave"})").ok());
    store.unload();
    savegame::ReadInfo info;
    EXPECT_EQ(readJson("slot1", "main", &info), R"({"level":"cave"})");
    EXPECT_EQ(readJson("slot1", "inventory"), R"(["sword"])");
    EXPECT_EQ(info.namespaces, (std::vector<std::string>{"inventory", "main"}));
    const std::string missing = readJson("slot1", "quests");
    EXPECT_NE(missing.find("error not_found"), std::string::npos) << missing;

    savegame::WriteOptions replace;
    replace.replaceAll = true;
    ASSERT_TRUE(writeAndFlush("slot1", R"({"fresh":true})", "main", replace).ok());
    store.unload();
    EXPECT_NE(readJson("slot1", "inventory").find("error not_found"), std::string::npos);

    const savegame::SaveError bad = store.write("Slot 1", "main", storePayload("{}"), savegame::WriteOptions(), nullptr);
    EXPECT_EQ(bad.code, savegame::SaveErrorCode::Name);
    EXPECT_EQ(bad.message, "Slot names use a-z, 0-9, '_' and '-' (got \"Slot 1\"); try \"slot-1\".");
    std::string payload;
    savegame::ReadInfo ignored;
    EXPECT_EQ(store.read("slot2", "main", payload, ignored).message, "No slot \"slot2\". Slots: slot1.");
}

TEST_F(SaveGameStoreTest, ReadSeesPendingWrite){
    store.setHold(true);
    bool landed = false;
    ASSERT_TRUE(store.write("slot1", "main", storePayload(R"({"gold":5})"), savegame::WriteOptions(),
                            [&landed](const savegame::SaveError&){ landed = true; }).ok());
    EXPECT_EQ(readJson("slot1"), R"({"gold":5})");
    EXPECT_FALSE(fs::exists(dir.path() / "saves" / "slot1.g1.jsave"));
    EXPECT_TRUE(store.exists("slot1"));
    store.setHold(false);
    ASSERT_TRUE(drainUntil([&]{ return landed; }));
    EXPECT_TRUE(fs::exists(dir.path() / "saves" / "slot1.g1.jsave"));
}

TEST_F(SaveGameStoreTest, NewerVersionIsNeverOverwritten){
    savegame::setGameDataVersion(4);
    ASSERT_TRUE(writeAndFlush("slot1", R"({"v":4})").ok());
    savegame::setGameDataVersion(5);
    ASSERT_TRUE(writeAndFlush("slot1", R"({"v":5})").ok());
    savegame::setGameDataVersion(4);
    store.unload();
    const std::string read = readJson("slot1");
    EXPECT_EQ(read.rfind("error newer_version", 0), 0u) << read;
    savegame::WriteOptions options;
    options.version = 4;
    const savegame::SaveError refused = store.write("slot1", "main", storePayload(R"({"v":"x"})"), options, nullptr);
    EXPECT_EQ(refused.code, savegame::SaveErrorCode::NewerVersion);
    EXPECT_EQ(store.list()[0].status, "newer_version");
    // the older generation (version 4) is never used instead
    store.unload();
    EXPECT_EQ(readJson("slot1").rfind("error newer_version", 0), 0u);
    options.force = true;
    ASSERT_TRUE(store.write("slot1", "main", storePayload(R"({"v":"forced"})"), options, nullptr).ok());
    ASSERT_TRUE(store.flush("slot1").ok());
    store.unload();
    EXPECT_EQ(readJson("slot1"), R"({"v":"forced"})");

    // a newer format: reported, never written over without force
    std::string bytes;
    {
        std::ifstream in(dir.path() / "saves" / "slot1.g3.jsave", std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    bytes[8] = 2;
    std::ofstream(dir.path() / "saves" / "slot1.g4.jsave", std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    store.unload();
    EXPECT_EQ(readJson("slot1").rfind("error newer_format", 0), 0u);
    options.force = false;
    EXPECT_EQ(store.write("slot1", "main", storePayload("{}"), options, nullptr).code, savegame::SaveErrorCode::NewerFormat);
    EXPECT_EQ(store.list()[0].status, "newer_format");
}

TEST_F(SaveGameStoreTest, FlushOnPauseWritesPending){
    store.setHold(true);
    ASSERT_TRUE(store.write("slot1", "main", storePayload(R"({"a":1})"), savegame::WriteOptions(), nullptr).ok());
    ASSERT_TRUE(store.write("slot1", "main", storePayload(R"({"a":2})"), savegame::WriteOptions(), nullptr).ok());
    Engine::systemPause();
    EXPECT_TRUE(fs::exists(dir.path() / "saves" / "slot1.g2.jsave")) << "the waiting snapshot is on disk when the pause returns";
    Engine::systemResume();
    store.setHold(false);
    store.reset();
    store.setFolder(dir.path() / "saves");
    EXPECT_EQ(readJson("slot1"), R"({"a":2})");
}

TEST_F(SaveGameStoreTest, TooLargeNamesTheNamespaces){
    savegame::SaveSettings small;
    small.maxSlotMiB = 1;
    savegame::configure(small);
    auto blob = [](size_t bytes){ return nlohmann::json(std::string(bytes, 'x')).dump(); };
    ASSERT_TRUE(writeAndFlush("slot1", blob(500 * 1024), "world").ok());
    ASSERT_TRUE(writeAndFlush("slot1", blob(300 * 1024), "quests").ok());
    ASSERT_TRUE(writeAndFlush("slot1", blob(100 * 1024), "main").ok());
    const savegame::SaveError error = store.write("slot1", "inventory", storePayload(blob(200 * 1024)), savegame::WriteOptions(), nullptr);
    EXPECT_EQ(error.code, savegame::SaveErrorCode::TooLarge);
    EXPECT_EQ(error.message, "slot \"slot1\" would be 1.1 MiB, above Max Slot Size (1 MiB); largest namespaces: world 0.5 MiB, "
                             "quests 0.3 MiB, inventory 0.2 MiB. Split the data into more slots or save less.");
    store.unload();
    EXPECT_NE(readJson("slot1", "inventory").find("not_found"), std::string::npos) << "nothing of the refused write stays";
}

TEST_F(SaveGameStoreTest, RestoreWritesAGenerationAsTheNewest){
    for (int i = 1; i <= 3; i++) { ASSERT_TRUE(writeAndFlush("slot1", "{\"i\":" + std::to_string(i) + "}").ok()); }
    std::vector<savegame::BackupInfo> backups;
    ASSERT_TRUE(store.backups("slot1", backups).ok());
    ASSERT_EQ(backups.size(), 3u);
    EXPECT_EQ(backups[0].generation, 3u);
    EXPECT_TRUE(backups[2].valid);
    ASSERT_TRUE(store.restore("slot1", 1, nullptr).ok());
    ASSERT_TRUE(store.flush("slot1").ok());
    store.unload();
    savegame::ReadInfo info;
    EXPECT_EQ(readJson("slot1", "main", &info), R"({"i":1})");
    EXPECT_EQ(info.generation, 4u);
    std::string file;
    ASSERT_TRUE(store.corrupt("slot1", savegame::CorruptMode::Flip, file).ok());
    const savegame::SaveError error = store.restore("slot1", 4, nullptr);
    EXPECT_EQ(error.message, "Couldn't restore \"slot1\" generation 4: the file is corrupt (checksum mismatch).");
}

TEST_F(SaveGameStoreTest, UnusedImagesStayUnderTheMemoryLimit){
    savegame::SaveSettings big;
    big.maxSlotMiB = 16;
    savegame::configure(big);
    const std::string blob = nlohmann::json(std::string(9 * 1024 * 1024, 'm')).dump();
    for (int i = 0; i < 4; i++) { ASSERT_TRUE(writeAndFlush("slot" + std::to_string(i), blob).ok()); }
    jobs::drainMainQueue();
    EXPECT_LE(store.memoryBytes(), savegame::SaveGameStore::MEMORY_LIMIT);
    EXPECT_EQ(readJson("slot0").size(), blob.size()) << "a dropped image reloads";
    store.unload();
    EXPECT_EQ(store.memoryBytes(), 0u);
}

// The child of NothingExistsUntilFirstUse (opt-in: it means something only in a process where
// nothing called SaveGame yet)
TEST(SaveGameStore, DISABLED_NothingExistsUntilFirstUseChild){
    ASSERT_EQ(savegame::SaveGameStore::existing(), nullptr);
    {
        testsupport::AllocationCountScope allocations;
        for (int i = 0; i < 100; i++) savegame::flushForLifecycle("the app paused");
        EXPECT_EQ(allocations.count(), 0u) << "the hooks test one pointer";
    }
    Engine::systemPause();
    Engine::systemResume();
    EXPECT_EQ(savegame::SaveGameStore::existing(), nullptr);
    EXPECT_FALSE(fs::exists(fs::path(test::TestSystem::get().getUserDataPath()) / "saves"));
}

TEST(SaveGameStore, NothingExistsUntilFirstUse){
    // the store is process-wide and other tests make it: the check runs in a process of its own
    const std::string self = ::testing::internal::GetArgvs().front();
    TestTempDir scratch("savegame-child");
    // its own report file: the run's XML is the parent's
    const std::string command = "\"" + self + "\" --gtest_also_run_disabled_tests"
                                " --gtest_filter=SaveGameStore.DISABLED_NothingExistsUntilFirstUseChild"
                                " \"--gtest_output=xml:" + (scratch / "child.xml").string() + "\"";
    const int status = std::system(command.c_str());
    EXPECT_EQ(status, 0) << command;
}
