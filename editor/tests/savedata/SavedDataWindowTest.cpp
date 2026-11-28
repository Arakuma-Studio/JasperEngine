// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Saved Data window's steps and Project Settings > Player > Saved Data (docs/specs/savegame.md
// R34, R36, R44; AC20): Delete Slot, Restore Backup and Corrupt Newest Generation undo byte for
// byte, undo refuses once the game saved the slot since, and the settings round-trip through
// project.yaml and game.json leaving the defaults out.

#include "Project.h"
#include "SaveGameSettings.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/CommandHistory.h"
#include "command/type/SaveGameSettingsCmd.h"
#include "command/type/SaveSlotCmd.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "player/PlayerConfig.h"
#include "save/SaveGameStore.h"
#include "window/SavedDataWindow.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SavedDataWindowTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // every file of the saves folder: name -> bytes
    std::map<std::string, std::string> savedFiles(const fs::path& dir) {
        std::map<std::string, std::string> files;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::ifstream in(it->path(), std::ios::binary);
            files[it->path().filename().string()].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        return files;
    }

    void writeSlot(savegame::SaveGameStore& store, const std::string& slot, const std::string& json) {
        std::string payload;
        savegame::SaveError error;
        ASSERT_TRUE(savegame::fromJson(nlohmann::json::parse(json), payload, error)) << error.message;
        ASSERT_TRUE(store.write(slot, "main", payload, savegame::WriteOptions(), nullptr).ok());
        ASSERT_TRUE(store.flush(slot).ok());
    }

    struct SavesOnTempDir {
        jasperengine::test::TempDir dir;
        savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
        SavesOnTempDir() { store.setFolder(dir.path() / "saves"); }
        ~SavesOnTempDir() {
            savegame::setFileFaultInjector(nullptr);
            io::setAtomicFileFaultInjector(nullptr);
            store.flush();
            jobs::drainMainQueue();
            store.setFolder({});
        }
        fs::path saves() const { return dir.path() / "saves"; }
    };

}

TEST(SavedDataWindow, DeleteRestoreAndCorruptAreUndoable) {
    SavesOnTempDir env;
    for (int i = 1; i <= 3; i++) writeSlot(env.store, "slot1", "{\"i\":" + std::to_string(i) + "}");
    writeSlot(env.store, "slot2", R"({"other":true})");
    const auto original = savedFiles(env.saves());
    CommandHistory history;

    ASSERT_TRUE(history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::Delete, "slot1")));
    EXPECT_FALSE(env.store.exists("slot1"));
    EXPECT_EQ(SaveSlotCmd(SaveSlotCmd::Action::Delete, "slot1").describe(), "Delete Save Slot 'slot1'");
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(savedFiles(env.saves()), original);

    ASSERT_TRUE(history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::Restore, "slot1", 1)));
    EXPECT_NE(savedFiles(env.saves()), original);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(savedFiles(env.saves()), original);

    for (savegame::CorruptMode mode : {savegame::CorruptMode::Truncate, savegame::CorruptMode::Flip, savegame::CorruptMode::Header}) {
        ASSERT_TRUE(history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::Corrupt, "slot1", 0, mode)));
        EXPECT_NE(savedFiles(env.saves()), original);
        ASSERT_TRUE(history.undo());
        EXPECT_EQ(savedFiles(env.saves()), original);
    }

    ASSERT_TRUE(history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::ClearAll, "")));
    EXPECT_TRUE(savedFiles(env.saves()).empty());
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(savedFiles(env.saves()), original);
    // redo deletes again
    ASSERT_TRUE(history.redo());
    EXPECT_TRUE(savedFiles(env.saves()).empty());
    ASSERT_TRUE(history.undo());

    // the store reads what undo put back
    std::string payload;
    savegame::ReadInfo info;
    ASSERT_TRUE(env.store.read("slot1", "main", payload, info).ok());
    EXPECT_EQ(info.generation, 3u);
}

TEST(SavedDataWindow, UndoRefusedWhenTheSlotChangedSince) {
    SavesOnTempDir env;
    writeSlot(env.store, "slot1", R"({"i":1})");
    writeSlot(env.store, "slot1", R"({"i":2})");
    CommandHistory history;
    ASSERT_TRUE(history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::Restore, "slot1", 1)));
    // the game saves the slot again
    writeSlot(env.store, "slot1", R"({"i":9})");
    const auto now = savedFiles(env.saves());
    EXPECT_FALSE(history.undo());
    EXPECT_EQ(history.lastRefusal(), "Can't undo \"Restore Save Slot 'slot1' Generation 1\": the game saved that slot since.");
    EXPECT_EQ(savedFiles(env.saves()), now) << "a refused undo touches nothing";

    ASSERT_TRUE(history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::Delete, "slot1")));
    writeSlot(env.store, "slot1", R"({"i":10})");
    EXPECT_FALSE(history.undo());
    EXPECT_EQ(history.lastRefusal(), "Can't undo \"Delete Save Slot 'slot1'\": the game saved that slot since.");
}

TEST(SavedDataWindow, SettingsRoundTripAndOmitDefaults) {
    Project project;
    Stream::decodeProject(&project, YAML::Load("vsync: true\n"), Workspace());
    EXPECT_TRUE(project.getSaveGameSettings() == SaveGameSettings());
    YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["saveGame"]) << "the defaults write no key";
    EXPECT_FALSE(player::PlayerConfig().toJson().contains("saveGame"));

    SaveGameSettings changed;
    changed.backups = 5;
    project.setSaveGameSettings(changed);
    EXPECT_EQ(savegame::settings().backups, 5) << "Play's store takes it at its next write";
    encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    ASSERT_TRUE(encoded["saveGame"]);
    EXPECT_EQ(encoded["saveGame"]["backups"].as<int>(), 5);
    EXPECT_FALSE(encoded["saveGame"]["maxSlotMiB"]) << "only the values away from the defaults";

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load("saveGame:\n  backups: 5\n  maxSlotMiB: 200\n"), Workspace());
    EXPECT_EQ(loaded.getSaveGameSettings().backups, 5);
    EXPECT_EQ(loaded.getSaveGameSettings().maxSlotMiB, 64) << "a hand-edited value is clamped";

    player::PlayerConfig config;
    config.saveGame = changed.engineSettings();
    player::PlayerConfig back;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(config.toJson(), back, error)) << error;
    EXPECT_EQ(back.saveGame.backups, 5);

    // the settings change is one undo step
    CommandHistory history;
    SaveGameSettings bigger = changed;
    bigger.maxSlotMiB = 16;
    ASSERT_TRUE(history.addCommand(new SaveGameSettingsCmd(&project, bigger)));
    EXPECT_EQ(project.getSaveGameSettings().maxSlotMiB, 16);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(project.getSaveGameSettings().maxSlotMiB, 8);
    project.setSaveGameSettings(SaveGameSettings());
}

// Showing a slot reads it, never repairs it: a damaged newest generation stays where it is (the
// game's own read sets it aside)
TEST(SavedDataWindow, ViewingASlotChangesNoFile) {
    SavesOnTempDir env;
    for (int i = 1; i <= 3; i++) writeSlot(env.store, "slot1", "{\"i\":" + std::to_string(i) + "}");
    env.store.unload();
    std::ofstream(env.saves() / "slot1.g3.jsave", std::ios::binary | std::ios::trunc) << "damaged";
    const auto before = savedFiles(env.saves());
    SavedDataWindow window(nullptr);
    window.select("slot1");
    window.refresh(true);
    EXPECT_EQ(savedFiles(env.saves()), before);
}

// The window's refreshes read no save file while nothing changed (it did every generation in
// full once a second)
TEST(SavedDataWindow, RefreshWithoutChangesReadsNoSaveFile) {
    SavesOnTempDir env;
    for (int i = 1; i <= 3; i++) writeSlot(env.store, "slot1", "{\"i\":" + std::to_string(i) + "}");
    SavedDataWindow window(nullptr);
    window.select("slot1");
    const size_t reads = savegame::SaveGameStore::fileReads();
    for (int i = 0; i < 3; i++) window.refresh(true);
    EXPECT_EQ(savegame::SaveGameStore::fileReads(), reads);
}

// Clear All Saves that deletes one slot and fails on the next stays in the history: undo brings
// back what it deleted
TEST(SavedDataWindow, PartlyFailedClearAllStaysUndoable) {
    SavesOnTempDir env;
    writeSlot(env.store, "slot1", R"({"a":1})");
    writeSlot(env.store, "slot2", R"({"b":2})");
    const auto original = savedFiles(env.saves());
    auto first = std::make_shared<std::string>();
    savegame::setFileFaultInjector([first](std::string_view step, const fs::path& path) -> std::error_code {
        if (step != "remove") return {};
        const std::string name = path.filename().string();
        const std::string slot = name.substr(0, name.find('.'));
        if (first->empty()) *first = slot;
        return slot == *first ? std::error_code() : std::make_error_code(std::errc::permission_denied);
    });
    CommandHistory history;
    const bool added = history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::ClearAll, ""));
    savegame::setFileFaultInjector(nullptr);
    EXPECT_TRUE(added) << "one slot was deleted: the step is in the history";
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(savedFiles(env.saves()), original);
}

// A step that can't wait for the game's write on its way (it outlasts the flush limit) is refused
// before it changes anything: the restore it would have queued never lands after the refusal
TEST(SavedDataWindowSlow, RestoreWhileTheGameWritesIsRefused) {
    SavesOnTempDir env;
    writeSlot(env.store, "slot1", R"({"i":1})");
    writeSlot(env.store, "slot1", R"({"i":2})");
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    io::setAtomicFileFaultInjector([&](std::string_view step, const fs::path& path) -> std::error_code {
        if (step != "write" || path.filename().string().find("slot1.g3") == std::string::npos) return {};
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait_for(lock, std::chrono::seconds(30), [&] { return release; });
        return {};
    });
    std::string payload;
    savegame::SaveError encodeError;
    ASSERT_TRUE(savegame::fromJson(nlohmann::json::parse(R"({"i":3})"), payload, encodeError));
    ASSERT_TRUE(env.store.write("slot1", "main", payload, savegame::WriteOptions(), nullptr).ok());
    {
        // the game's write is on a worker, stopped inside its file write
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
    }
    CommandHistory history;
    const bool added = history.addCommand(new SaveSlotCmd(SaveSlotCmd::Action::Restore, "slot1", 1));
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    ASSERT_TRUE(env.store.flush("slot1", std::chrono::seconds(10)).ok());
    jobs::drainMainQueue();
    io::setAtomicFileFaultInjector(nullptr);
    EXPECT_FALSE(added);
    std::vector<std::string> names;
    for (const auto& file : savedFiles(env.saves())) names.push_back(file.first);
    EXPECT_EQ(names, (std::vector<std::string>{"slot1.g1.jsave", "slot1.g2.jsave", "slot1.g3.jsave"}));
}

}  // namespace SavedDataWindowTest_cpp
