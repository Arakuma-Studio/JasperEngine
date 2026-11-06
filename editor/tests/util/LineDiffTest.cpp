// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The code editor's line diff (docs/superpowers/specs/2026-11-04-code-editor-git-design.md 6.1):
// change ranges, the clean-up heuristics, the edit cap, the unified text and its budget.

#include "util/LineDiff.h"

#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    using Lines = std::vector<std::string>;

    std::vector<LineChange> diff(const Lines& a, const Lines& b) {
        return LineDiff::compute(a, b);
    }

}

TEST(LineDiff, Basics) {
    const Lines base = {"a", "b", "c", "d", "e"};
    EXPECT_TRUE(diff(base, base).empty());

    // an insertion after b
    std::vector<LineChange> changes = diff(base, {"a", "b", "x", "y", "c", "d", "e"});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{2, 2, 2, 4}));
    EXPECT_EQ(changes[0].kind(), LineChange::Kind::Added);

    // a deletion of c and d
    changes = diff(base, {"a", "b", "e"});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{2, 4, 2, 2}));
    EXPECT_EQ(changes[0].kind(), LineChange::Kind::Deleted);

    // a modification of c
    changes = diff(base, {"a", "b", "C", "d", "e"});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{2, 3, 2, 3}));
    EXPECT_EQ(changes[0].kind(), LineChange::Kind::Modified);

    // two separate changes, first and last line
    changes = diff(base, {"A", "b", "c", "d", "E"});
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0], (LineChange{0, 1, 0, 1}));
    EXPECT_EQ(changes[1], (LineChange{4, 5, 4, 5}));

    // everything from nothing, nothing from everything
    changes = diff({}, base);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{0, 0, 0, 5}));
    changes = diff(base, {});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{0, 5, 0, 0}));

    EXPECT_STREQ(lineChangeKindName(LineChange::Kind::Deleted), "deleted");
}

TEST(LineDiff, Heuristics) {
    // a function inserted between two others: of the equally scored slides (both edges between
    // unindented lines) the earliest wins, right after the first "end"
    const Lines base = {"function a()", "    return 1", "end", "", "function c()", "    return 3", "end"};
    const Lines added = {"function a()", "    return 1", "end", "", "function b()", "    return 2", "end", "",
                         "function c()", "    return 3", "end"};
    std::vector<LineChange> changes = diff(base, added);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{3, 3, 3, 7}));

    // two edits kept apart only by a line with barely any text join into one change
    const Lines block = {"local a = 1", "local b = 2", "local c = 3", "end", "local d = 4", "local e = 5", "local f = 6"};
    const Lines edited = {"local A = 10", "local B = 20", "local C = 30", "end", "local D = 40", "local E = 50", "local F = 60"};
    changes = diff(block, edited);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{0, 7, 0, 7}));

    // an inserted blank line among blank lines slides to the less indented edge
    const Lines blanks = {"if a then", "    x()", "", "end"};
    changes = diff(blanks, {"if a then", "    x()", "", "", "end"});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{3, 3, 3, 4}));
}

TEST(LineDiff, EditCap) {
    // more edits than the cap: the differing middle is one change
    Lines a, b;
    a.push_back("head");
    b.push_back("head");
    for (int i = 0; i < LineDiff::kMaxEdits; i++) {
        a.push_back("a" + std::to_string(i));
        b.push_back("b" + std::to_string(i));
    }
    a.push_back("tail");
    b.push_back("tail");
    const std::vector<LineChange> changes = diff(a, b);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], (LineChange{1, 1 + LineDiff::kMaxEdits, 1, 1 + LineDiff::kMaxEdits}));
}

TEST(LineDiff, Unified) {
    const Lines base = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
    const Lines edited = {"1", "two", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13"};
    const std::string text = LineDiff::unified(base, edited, LineDiff::compute(base, edited), "s.lua", "s.lua");
    EXPECT_EQ(text,
              "--- a/s.lua\n+++ b/s.lua\n"
              "@@ -1,5 +1,5 @@\n 1\n-2\n+two\n 3\n 4\n 5\n"
              "@@ -10,3 +10,4 @@\n 10\n 11\n 12\n+13\n");
    EXPECT_TRUE(LineDiff::unified(base, base, {}, "s.lua", "s.lua").empty());

    // a deletion at the start: the empty side names the line before it
    const Lines longer = {"1", "2", "3"};
    const Lines shorter = {"2", "3"};
    EXPECT_EQ(LineDiff::unified(longer, shorter, LineDiff::compute(longer, shorter), "x", "x"),
              "--- a/x\n+++ b/x\n@@ -1,3 +1,2 @@\n-1\n 2\n 3\n");
}

TEST(LineDiff, SplitLinesDropsCarriageReturns) {
    EXPECT_EQ(LineDiff::splitLines("a\r\nb\nc"), (Lines{"a", "b", "c"}));
    EXPECT_EQ(LineDiff::splitLines("a\n"), (Lines{"a", ""}));
    EXPECT_EQ(LineDiff::splitLines(""), (Lines{""}));
}

TEST(LineDiffBench, SingleEditTenThousandLines) {
    Lines base;
    for (int i = 0; i < 10000; i++) base.push_back("    local value" + std::to_string(i) + " = compute(" + std::to_string(i) + ")");
    Lines edited = base;
    edited[5000] += " -- edited";

    double best = 1e9;
    for (int run = 0; run < 5; run++) {
        const auto start = std::chrono::steady_clock::now();
        const std::vector<LineChange> changes = LineDiff::compute(base, edited);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ASSERT_EQ(changes.size(), 1u);
    }
    std::printf("[ LineDiffBench ] one edit in 10,000 lines: %.3f ms (budget 1 ms)\n", best);
    const double slack = test::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(best, 1.0 * slack);
    }
}
