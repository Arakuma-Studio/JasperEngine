// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Saved Data window's steps and Project Settings > Player > Saved Data (docs/specs/savegame.md
// R34, R36, R44; AC20): Delete Slot, Restore Backup and Corrupt Newest Generation undo byte for
// byte, undo refuses once the game saved the slot since, and the settings round-trip through
// project.yaml and game.json leaving the defaults out.

#include "Generator.h"
#include "Project.h"
#include "SaveGameSettings.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/CommandHistory.h"
#include "command/type/SaveGameSettingsCmd.h"
#include "command/type/SaveSlotCmd.h"
#include "jobs/JobSystem.h"
#include "player/PlayerConfig.h"
#include "save/SaveGameStore.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

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
        test::TempDir dir;
        savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
        SavesOnTempDir() { store.setFolder(dir.path() / "saves"); }
        ~SavesOnTempDir() {
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

    EXPECT_EQ(Generator::saveGameInitSource(SaveGameSettings()), "");
    EXPECT_NE(Generator::saveGameInitSource(changed).find("saveSettings.backups = 5;"), std::string::npos);

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
