// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "Out.h"
#include "window/ConsoleWindow.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

using namespace jasperengine::editor;

TEST(ConsoleWindow, SelectionFindsRetainedIdsAndSurvivesEviction) {
    if (!Out::memorySinkId()) Out::startLogging();
    Out::clearConsole();
    ConsoleWindow window;
    EXPECT_FALSE(window.hasSelectedMessage());
    EXPECT_FALSE(window.selectLog(0));

    std::vector<uint64_t> ids;
    for (int i = 0; i < 64; i++) {
        Out::build("selection record " + std::to_string(i));
        ids.push_back(Out::lastLogId());
    }
    for (size_t index : {size_t(0), size_t(31), size_t(63)}) {
        ASSERT_TRUE(window.selectLog(ids[index]));
        EXPECT_TRUE(window.hasSelectedMessage());
        const auto state = window.viewState(0);
        EXPECT_TRUE(state.hasSelection);
        EXPECT_EQ(state.selectedId, ids[index]);
    }
    EXPECT_FALSE(window.selectLog(std::numeric_limits<uint64_t>::max()));
    EXPECT_TRUE(window.hasSelectedMessage());

    ASSERT_TRUE(window.selectLog(ids.front()));
    Out::build(std::string(10000, '\n'));
    const uint64_t oversizedId = Out::lastLogId();
    EXPECT_FALSE(window.selectLog(ids.front()));
    EXPECT_FALSE(window.hasSelectedMessage());
    ASSERT_TRUE(window.selectLog(oversizedId));
    EXPECT_TRUE(window.hasSelectedMessage());
    window.clear();
    EXPECT_FALSE(window.hasSelectedMessage());
    EXPECT_FALSE(window.selectLog(oversizedId));
    Out::clearConsole();
}

TEST(ConsoleWindow, SelectionHandlesGapsBetweenMonotonicIds) {
    if (!Out::memorySinkId()) Out::startLogging();
    Out::clearConsole();
    ConsoleWindow window;
    Out::build("retained selection record");
    const uint64_t first = Out::lastLogId();
    ASSERT_TRUE(window.selectLog(first));

    Out::build("record cleared before the window pulls it");
    const uint64_t gap = Out::lastLogId();
    Out::clearConsole();
    Out::build("newer selection record");
    const uint64_t last = Out::lastLogId();
    ASSERT_GT(last, gap);
    ASSERT_TRUE(window.selectLog(last));
    EXPECT_FALSE(window.selectLog(gap));
    ASSERT_TRUE(window.selectLog(first));
    const auto state = window.viewState(0);
    EXPECT_TRUE(state.hasSelection);
    EXPECT_EQ(state.selectedId, first);
    Out::clearConsole();
}
