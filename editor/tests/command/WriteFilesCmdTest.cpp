// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's project-history file write (editor/command/type/WriteFilesCmd), which the pose
// animation document commits through: undo/redo, merge keys (drags) and all-or-nothing writes.
// Compiled into this executable with CommandHistory (the editor has no googletest target).

#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct TempDir {
        fs::path path;
        TempDir(){
            static std::atomic<int> counter{0};
            path = fs::temp_directory_path() / ("jasper-writefiles-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++));
            fs::create_directories(path);
        }
        ~TempDir(){
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    std::string read(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void write(const fs::path& path, const std::string& text){
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    WriteFilesCmd* command(std::vector<WriteFilesCmd::Item> items, const std::string& key, int* notified = nullptr){
        auto* cmd = new WriteFilesCmd(std::move(items), [notified](const std::vector<fs::path>&){
            if (notified) (*notified)++;
        });
        if (key.empty()) cmd->setNoMerge();
        else cmd->setMergeKey(key);
        return cmd;
    }

}

TEST(WriteFilesCmd, UndoRedoCreatesAndRemovesFilesAndDirectories){
    TempDir dir;
    CommandHistory history;
    const fs::path a = dir.path / "anim" / "deep" / "walk.poseanim";
    const fs::path b = dir.path / "existing.humanoid";
    write(b, "old");
    int notified = 0;
    ASSERT_TRUE(history.addCommand(command({{a, "A"}, {b, "B"}}, "", &notified)));
    EXPECT_EQ(read(a), "A");
    EXPECT_EQ(read(b), "B");
    EXPECT_EQ(notified, 1);
    history.undo();
    EXPECT_FALSE(fs::exists(a));
    EXPECT_FALSE(fs::exists(dir.path / "anim"));   // the directories it created go too
    EXPECT_EQ(read(b), "old");
    EXPECT_EQ(notified, 2);
    history.redo();
    EXPECT_EQ(read(a), "A");
    EXPECT_EQ(read(b), "B");
    EXPECT_EQ(notified, 3);
}

TEST(WriteFilesCmd, SameMergeKeyIsOneUndoStep){
    TempDir dir;
    CommandHistory history;
    const fs::path file = dir.path / "drag.poseanim";
    write(file, "start");
    ASSERT_TRUE(history.addCommand(command({{file, "1"}}, "drag pose 0")));
    ASSERT_TRUE(history.addCommand(command({{file, "2"}}, "drag pose 0")));
    ASSERT_TRUE(history.addCommand(command({{file, "3"}}, "drag pose 0")));
    EXPECT_EQ(read(file), "3");
    EXPECT_EQ(history.position(), 1u);
    history.undo();
    EXPECT_EQ(read(file), "start");     // back to before the first edit of the run
    EXPECT_FALSE(history.canUndo());
    history.redo();
    EXPECT_EQ(read(file), "3");
}

TEST(WriteFilesCmd, OtherKeysOrNoKeyAreSeparateSteps){
    TempDir dir;
    CommandHistory history;
    const fs::path file = dir.path / "a.poseanim";
    write(file, "0");
    ASSERT_TRUE(history.addCommand(command({{file, "1"}}, "drag pose 0")));
    ASSERT_TRUE(history.addCommand(command({{file, "2"}}, "drag pose 1")));
    ASSERT_TRUE(history.addCommand(command({{file, "3"}}, "")));
    ASSERT_TRUE(history.addCommand(command({{file, "4"}}, "")));
    EXPECT_EQ(history.position(), 4u);
    history.undo();
    EXPECT_EQ(read(file), "3");
    history.undo();
    EXPECT_EQ(read(file), "2");
    history.undo();
    EXPECT_EQ(read(file), "1");
    history.undo();
    EXPECT_EQ(read(file), "0");
    // a key does not merge across other files
    const fs::path other = dir.path / "b.poseanim";
    CommandHistory second;
    ASSERT_TRUE(second.addCommand(command({{file, "x"}}, "k")));
    ASSERT_TRUE(second.addCommand(command({{other, "y"}}, "k")));
    EXPECT_EQ(second.position(), 2u);
}

TEST(WriteFilesCmd, AFailedWriteRestoresWhatTheCommandWrote){
    TempDir dir;
    CommandHistory history;
    const fs::path first = dir.path / "walk.poseanim";
    const fs::path created = dir.path / "new.poseanim";
    write(first, "before");
    const fs::path blocker = dir.path / "blocker";
    write(blocker, "a file, not a directory");
    int notified = 0;
    // the third file's parent is a regular file: the write fails
    EXPECT_FALSE(history.addCommand(command({{first, "after"}, {created, "new"}, {blocker / "x.humanoid", "z"}}, "", &notified)));
    EXPECT_EQ(read(first), "before");
    EXPECT_FALSE(fs::exists(created));
    EXPECT_EQ(notified, 0);
    EXPECT_FALSE(history.canUndo());
}
