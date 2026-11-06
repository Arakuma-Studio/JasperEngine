// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// GraphEditCmd tells who listens (BF23 F12): an open window or a pool hears the file change on
// execute and on undo, once the write is done, and nothing after it stops listening.

#include "command/type/GraphEditCmd.h"
#include "util/ProjectChecks.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string graphFileText(const fs::path& path) {
        std::string text;
        EXPECT_TRUE(ProjectChecks::readFile(path, text));
        return text;
    }

}

TEST(GraphEditCmd, ListenersHearExecuteAndUndo) {
    jasperengine::test::TempDir dir;
    const fs::path file = dir.write("a.testgraph", "before");
    std::vector<std::string> heard;
    const uint64_t listener = GraphEditCmd::addChangeListener([&](const fs::path& changed) {
        EXPECT_EQ(changed, file);
        heard.push_back(graphFileText(changed));
    });

    GraphEditCmd command(file, "before", "after", "Edit Graph");
    ASSERT_TRUE(command.execute());
    command.undo();
    EXPECT_EQ(heard, (std::vector<std::string>{"after", "before"})) << "called after each write, with the file as it is now";

    GraphEditCmd::removeChangeListener(listener);
    ASSERT_TRUE(command.execute());
    EXPECT_EQ(heard.size(), 2u) << "not after it stopped listening";
}

TEST(GraphEditCmd, AFailedWriteTellsNobody) {
    jasperengine::test::TempDir dir;
    int heard = 0;
    const uint64_t listener = GraphEditCmd::addChangeListener([&](const fs::path&) { heard++; });
    GraphEditCmd command(dir.path(), "before", "after", "Edit Graph");   // a directory: the write fails
    EXPECT_FALSE(command.execute());
    command.undo();
    EXPECT_EQ(heard, 0);
    GraphEditCmd::removeChangeListener(listener);
}

TEST(GraphEditCmd, AListenerRemovedDuringANotificationIsNotCalled) {
    jasperengine::test::TempDir dir;
    const fs::path file = dir.write("a.testgraph", "before");
    int secondCalls = 0;
    uint64_t second = 0;
    // whoever goes first may destroy what the next one captures
    const uint64_t first = GraphEditCmd::addChangeListener([&](const fs::path&) { GraphEditCmd::removeChangeListener(second); });
    second = GraphEditCmd::addChangeListener([&](const fs::path&) { secondCalls++; });
    GraphEditCmd command(file, "before", "after", "Edit Graph");
    ASSERT_TRUE(command.execute());
    EXPECT_EQ(secondCalls, 0);
    GraphEditCmd::removeChangeListener(first);
}
