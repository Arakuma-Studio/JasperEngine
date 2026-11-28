// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SaveGame store (docs/specs/savegame.md R3-R17, R29, R30; AC8-AC10, AC12-AC17): generations,
// recovery, coalescing, flushes and caps.

#include "Engine.h"
#include "Log.h"
#include "io/AssetIOService.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "save/SaveGameStore.h"
#include "util/ContentHash.h"

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


namespace SaveGameStoreTest_cpp {

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

    // The footer checksum of patched save bytes made to match again (a newer engine's file)
    void resealStoreBytes(std::string& bytes){
        const size_t body = bytes.size() - savegame::FOOTER_BYTES;
        const uint64_t hash = hashContent64(bytes.data(), body, 0);
        for (int i = 0; i < 8; i++) bytes[body + i] = static_cast<char>((hash >> (8 * i)) & 0xFF);
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
            // a Lua test of the same process registered SaveGame's migrator
            savegame::setMigrator(nullptr);
            savegame::configure(savegame::SaveSettings());
            savegame::setGameDataVersion(1);
            store.setFolder(dir.path() / "saves");
        }

        void TearDown() override {
            store.setHold(false);
            io::setAtomicFileFaultInjector(nullptr);
            savegame::setFileFaultInjector(nullptr);
            savegame::setMigrator(nullptr);
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
    resealStoreBytes(bytes);
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

// a new engine run in the same process (a recreated Android activity) restarts the job system: the
// store's writes go to the new one, not to the shut-down system that cancels them (they then waited
// for a flush; the full core run showed it as SaveGameStoreSlow's write never starting)
TEST_F(SaveGameStoreTest, WritesLandAfterTheJobSystemRestarts){
    ASSERT_TRUE(writeAndFlush("slot1", R"({"a":1})").ok());
    // the asset readers keep the job system they started with: restarted with it (FoliageSystemCore)
    AssetIOService::instance().stop();
    jobs::shutdown();
    jobs::initialize(jobs::Config{});
    AssetIOService::instance().start();
    bool landed = false;
    ASSERT_TRUE(store.write("slot1", "main", storePayload(R"({"a":2})"), savegame::WriteOptions(),
                            [&](const savegame::SaveError& error){ landed = error.ok(); }).ok());
    EXPECT_TRUE(drainUntil([&]{ return landed; })) << "the write ran on a worker of the new job system";
    EXPECT_TRUE(fs::exists(dir.path() / "saves" / "slot1.g2.jsave"));
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

// A write at a newer data version never labels the slot's other namespaces with it unmigrated:
// the game's migrations run first, and without them the write is refused (every caller: the
// dialogue save hooks and user_settings set too, not only SaveGame.write)
TEST_F(SaveGameStoreTest, NewerVersionWriteMigratesTheOtherNamespaces){
    ASSERT_TRUE(writeAndFlush("slot1", R"({"level":"forest"})").ok());
    ASSERT_TRUE(writeAndFlush("slot1", R"(["sword"])", "inventory").ok());
    savegame::setGameDataVersion(2);
    savegame::WriteOptions options;
    options.version = 2;
    const savegame::SaveError refused = store.write("slot1", "main", storePayload(R"({"level":"cave"})"), options, nullptr);
    EXPECT_EQ(refused.code, savegame::SaveErrorCode::Migration) << refused.message;
    EXPECT_NE(refused.message.find("inventory"), std::string::npos) << refused.message;
    store.flush();
    store.unload();
    savegame::ReadInfo info;
    EXPECT_EQ(readJson("slot1", "main", &info), R"({"level":"forest"})");
    EXPECT_EQ(info.version, 1u);

    int runs = 0;
    savegame::setMigrator([&runs](const std::string& slot, savegame::SlotData& data, uint32_t version) -> savegame::SaveError {
        runs++;
        EXPECT_EQ(slot, "slot1");
        EXPECT_EQ(data.info.version, 1u);
        EXPECT_EQ(version, 2u);
        for (savegame::SaveSection& section : data.sections){
            if (section.name == "inventory") section.payload = std::make_shared<const std::string>(storePayload(R"(["sword","shield"])"));
        }
        return {};
    });
    const savegame::SaveError written = store.write("slot1", "main", storePayload(R"({"level":"cave"})"), options, nullptr);
    savegame::setMigrator(nullptr);
    ASSERT_TRUE(written.ok()) << written.message;
    ASSERT_TRUE(store.flush("slot1").ok());
    store.unload();
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(readJson("slot1", "inventory", &info), R"(["sword","shield"])");
    EXPECT_EQ(info.version, 2u);
    EXPECT_EQ(readJson("slot1"), R"({"level":"cave"})");
}

// A generation that can't be read now (a sharing violation, an antivirus scan) is not damage: the
// read fails and every file stays, older set-aside ones included; nor does a write start afresh
TEST_F(SaveGameStoreTest, UnreadableGenerationIsLeftAlone){
    const fs::path saves = dir.path() / "saves";
    for (int i = 1; i <= 3; i++) { ASSERT_TRUE(writeAndFlush("slot1", "{\"i\":" + std::to_string(i) + "}").ok()); }
    std::ofstream(saves / "slot1.g0.jsave.corrupt", std::ios::binary) << "earlier damage";
    store.unload();
    const std::vector<std::string> before = storeFileNames(saves);
    savegame::setFileFaultInjector([](std::string_view step, const fs::path& path) -> std::error_code {
        if (step == "read" && path.filename() == "slot1.g3.jsave") return std::make_error_code(std::errc::permission_denied);
        return {};
    });
    const std::string result = readJson("slot1");
    EXPECT_EQ(result.rfind("error io: ", 0), 0u) << result;
    EXPECT_NE(result.find("slot1.g3.jsave"), std::string::npos) << result;
    EXPECT_EQ(storeFileNames(saves), before);
    EXPECT_EQ(store.write("slot1", "main", storePayload("{}"), savegame::WriteOptions(), nullptr).code, savegame::SaveErrorCode::Io);
    store.flush();
    EXPECT_EQ(storeFileNames(saves), before);
    savegame::setFileFaultInjector(nullptr);
    EXPECT_EQ(readJson("slot1"), R"({"i":3})");
}

// The footer is checked before the format field: a damaged format byte is damage (the read
// recovers from an older generation), not a newer engine's file that blocks the slot
TEST_F(SaveGameStoreTest, DamagedFormatFieldFallsBackToAnOlderGeneration){
    const fs::path saves = dir.path() / "saves";
    for (int i = 1; i <= 2; i++) { ASSERT_TRUE(writeAndFlush("slot1", "{\"i\":" + std::to_string(i) + "}").ok()); }
    std::string bytes;
    {
        std::ifstream in(saves / "slot1.g2.jsave", std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    bytes[8] = 2;
    std::ofstream(saves / "slot1.g2.jsave", std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    store.unload();
    savegame::ReadInfo info;
    EXPECT_EQ(readJson("slot1", "main", &info), R"({"i":1})");
    EXPECT_EQ(info.recoveredFrom, 1u);
    EXPECT_TRUE(writeAndFlush("slot1", R"({"i":3})").ok());
}

// A slot deleted while its async read was on its way is not brought back by the read's result
TEST_F(SaveGameStoreTest, DeleteDuringAnAsyncReadStaysDeleted){
    ASSERT_TRUE(writeAndFlush("slot1", R"({"i":1})").ok());
    store.unload();
    const size_t reads = savegame::SaveGameStore::fileReads();
    bool done = false;
    savegame::SaveError result;
    store.readSlotAsync("slot1", [&](const savegame::SaveError& error, const savegame::SlotData&){ done = true; result = error; });
    // the job read the file; what it read waits for the main thread
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (savegame::SaveGameStore::fileReads() == reads && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_GT(savegame::SaveGameStore::fileReads(), reads);
    ASSERT_TRUE(store.remove("slot1").ok());
    ASSERT_TRUE(drainUntil([&]{ return done; }));
    EXPECT_EQ(result.code, savegame::SaveErrorCode::NotFound) << result.message;
    EXPECT_FALSE(store.exists("slot1"));
    EXPECT_TRUE(storeFileNames(dir.path() / "saves").empty());
}

// A delete never runs beside a write it could not wait for: that write would publish its
// generation after the files went, and the slot would come back
TEST_F(SaveGameStoreTest, DeleteWhileAWriteRunsIsRefused){
    ASSERT_TRUE(writeAndFlush("slot1", R"({"i":1})").ok());
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    io::setAtomicFileFaultInjector([&](std::string_view step, const fs::path& path) -> std::error_code {
        if (step != "write" || path.filename().string().find("slot1.g2") == std::string::npos) return {};
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait_for(lock, std::chrono::seconds(30), [&]{ return release; });
        return {};
    });
    bool landed = false;
    ASSERT_TRUE(store.write("slot1", "main", storePayload(R"({"i":2})"), savegame::WriteOptions(),
                            [&landed](const savegame::SaveError&){ landed = true; }).ok());
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(10), [&]{ return entered; }));
    }
    const savegame::SaveError removed = store.remove("slot1", std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    ASSERT_TRUE(drainUntil([&]{ return landed; }));
    io::setAtomicFileFaultInjector(nullptr);
    EXPECT_FALSE(removed.ok());
    EXPECT_NE(removed.message.find("still being written"), std::string::npos) << removed.message;
    EXPECT_TRUE(store.exists("slot1"));
    store.unload();
    EXPECT_EQ(readJson("slot1"), R"({"i":2})");
}

// A kill between a write's temporary file and its rename leaves the temporary: opening the folder
// deletes them, storage() counts one left meanwhile, and deleting the slot takes it too
TEST_F(SaveGameStoreTest, LeftoverTempFilesAreSwept){
    const fs::path saves = dir.path() / "saves";
    ASSERT_TRUE(writeAndFlush("slot1", R"({"i":1})").ok());
    const std::string dead = ".slot1.g7.jsave.2147483000-1.je-tmp";
    const std::string own = ".slot1.g8.jsave." + std::to_string(io::currentProcessId()) + "-99999.je-tmp";
    std::ofstream(saves / dead, std::ios::binary) << "half a save";
    std::ofstream(saves / own, std::ios::binary) << "half a save";
    std::ofstream(saves / "notes.txt") << "not a save";
    store.reset();
    store.setFolder(saves);
    EXPECT_EQ(store.list().size(), 1u);
    EXPECT_EQ(storeFileNames(saves), (std::vector<std::string>{"notes.txt", "slot1.g1.jsave"}));

    std::ofstream(saves / dead, std::ios::binary) << "half a save";
    std::error_code ec;
    const int64_t generation = static_cast<int64_t>(fs::file_size(saves / "slot1.g1.jsave", ec));
    EXPECT_EQ(store.storage().used, generation + 11);
    ASSERT_TRUE(store.remove("slot1").ok());
    EXPECT_EQ(storeFileNames(saves), (std::vector<std::string>{"notes.txt"}));
}

// Each IndexedDB sync confirms only the writes that landed before it started, with its own result
TEST(SaveGameSync, EachSyncConfirmsOnlyTheWritesBeforeIt){
    savegame::SyncWaiters waiters;
    std::vector<std::string> results;
    auto callback = [&results](const char* name){
        return std::vector<savegame::WriteCallback>{[&results, name](const savegame::SaveError& error){
            results.push_back(std::string(name) + ":" + (error.ok() ? "ok" : savegame::errorCodeName(error.code)));
        }};
    };
    auto run = [](std::vector<savegame::SyncWaiters::Delivery> deliveries){
        for (savegame::SyncWaiters::Delivery& delivery : deliveries)
            for (savegame::WriteCallback& done : delivery.callbacks) done(delivery.error);
    };
    waiters.landed(callback("a"), {});
    waiters.started(1);
    waiters.landed(callback("b"), {});
    waiters.started(2);
    run(waiters.finished(1, true, "", ""));
    EXPECT_EQ(results, (std::vector<std::string>{"a:ok"}));
    EXPECT_EQ(waiters.waiting(), 1u);
    run(waiters.finished(2, false, "QuotaExceededError", "The quota has been exceeded."));
    EXPECT_EQ(results, (std::vector<std::string>{"a:ok", "b:quota"}));
    // a sync this store never saw start confirms nothing
    waiters.landed(callback("c"), {});
    run(waiters.finished(3, true, "", ""));
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(waiters.waiting(), 1u);
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
    std::string command = "\"" + self + "\" --gtest_also_run_disabled_tests"
                          " --gtest_filter=SaveGameStore.DISABLED_NothingExistsUntilFirstUseChild"
                          " \"--gtest_output=xml:" + (scratch / "child.xml").string() + "\"";
#ifdef _WIN32
    // cmd.exe /c drops the outermost quotes of a line that holds more than two
    command = "\"" + command + "\"";
#endif
    const int status = std::system(command.c_str());
    EXPECT_EQ(status, 0) << command;
}

}  // namespace SaveGameStoreTest_cpp
