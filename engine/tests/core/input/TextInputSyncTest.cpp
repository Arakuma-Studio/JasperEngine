// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The soft keyboard's snapshots turned into the field's edits (engine/core/input/TextInputSync.h,
// text-entry spec D3, AC2): the diff Android used for document text fields, moved out of
// NativeEngine.cpp, now shared with iOS.

#include "input/TextInputSync.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace TextInputSyncTest_cpp {

using namespace jasperengine;

namespace {

    // every edit, in order, as text: "caret -2", "back", "insert at", "compose ka 2", "cancel"
    class SyncRecorder : public TextInputSync::Output {
    public:
        std::vector<std::string> edits;

        void moveCaret(int steps) override { edits.push_back("caret " + std::to_string(steps)); }
        void backspace() override { edits.push_back("back"); }
        void insertText(const std::string& utf8) override { edits.push_back("insert " + utf8); }
        void compose(const std::string& utf8, int cursor) override { edits.push_back("compose " + utf8 + " " + std::to_string(cursor)); }
        void cancelComposition() override { edits.push_back("cancel"); }
    };

    std::u16string syncUnitsOf(const std::u32string& text){
        std::u16string units;
        for (char32_t cp : text){
            if (cp >= 0x10000){
                cp -= 0x10000;
                units.push_back((char16_t)(0xD800 + (cp >> 10)));
                units.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
            }else{
                units.push_back((char16_t)cp);
            }
        }
        return units;
    }

    // a snapshot with the caret at `caret` (units) and no composing region
    void syncTyped(TextInputSync& sync, const std::u32string& text, int caret, SyncRecorder& out){
        const std::u16string units = syncUnitsOf(text);
        sync.update(units, caret, caret, -1, -1, out);
    }

    // a snapshot with the caret at the end of the composing region [start, end) (units)
    void syncComposing(TextInputSync& sync, const std::u32string& text, int start, int end, SyncRecorder& out){
        const std::u16string units = syncUnitsOf(text);
        sync.update(units, end, end, start, end, out);
    }

    using SyncEdits = std::vector<std::string>;

}

TEST(TextInputSync, SuggestionCompletingAWordInsertsTheRestOnly){
    TextInputSync sync;
    SyncRecorder out;
    sync.reset(U"Wh", 2);
    syncTyped(sync, U"What", 4, out);
    EXPECT_EQ(out.edits, (SyncEdits{"insert at"}));
    EXPECT_EQ(sync.text(), U"What");
    EXPECT_EQ(sync.caret(), 4u);
}

TEST(TextInputSync, CorrectionDeletesThenInserts){
    TextInputSync sync;
    SyncRecorder out;
    sync.reset(U"teh", 3);
    syncTyped(sync, U"the", 3, out);
    EXPECT_EQ(out.edits, (SyncEdits{"back", "back", "insert he"}));
}

TEST(TextInputSync, CompositionUpdatedCommittedAndCancelled){
    TextInputSync sync;
    SyncRecorder out;
    sync.reset(U"a", 1);

    // composing "k", then "ka": the composition follows
    syncComposing(sync, U"ak", 1, 2, out);
    syncComposing(sync, U"aka", 1, 3, out);
    EXPECT_TRUE(sync.isComposing());
    EXPECT_EQ(out.edits, (SyncEdits{"compose k 1", "compose ka 2"}));

    // the same snapshot again: nothing
    out.edits.clear();
    syncComposing(sync, U"aka", 1, 3, out);
    EXPECT_TRUE(out.edits.empty());

    // the region ends with a conversion: committed
    syncTyped(sync, U"aか", 2, out);
    EXPECT_EQ(out.edits, (SyncEdits{"insert \xE3\x81\x8B"}));
    EXPECT_FALSE(sync.isComposing());
    EXPECT_EQ(sync.caret(), 2u);

    // a composition whose text goes away: cancelled
    out.edits.clear();
    syncComposing(sync, U"aかx", 2, 3, out);
    syncTyped(sync, U"aか", 2, out);
    EXPECT_EQ(out.edits, (SyncEdits{"compose x 1", "cancel"}));
    EXPECT_FALSE(sync.isComposing());

    // a new composition right after a commit follows it
    out.edits.clear();
    syncComposing(sync, U"aかn", 2, 3, out);
    syncComposing(sync, U"aかny", 3, 4, out);
    EXPECT_EQ(out.edits, (SyncEdits{"compose n 1", "insert n", "compose y 1"}));

    // text outside the composition changed too: dropped, then the rest diffed
    out.edits.clear();
    syncTyped(sync, U"Zかn", 3, out);
    EXPECT_EQ(out.edits, (SyncEdits{"cancel", "caret -2", "back", "insert Z", "caret 2"}));
}

TEST(TextInputSync, AnEditAwayFromTheCaretMovesItFirst){
    TextInputSync sync;
    SyncRecorder out;
    sync.reset(U"one twx three", 13);
    // a suggestion fixes "twx" while the caret is at the end
    syncTyped(sync, U"one two three", 13, out);
    EXPECT_EQ(out.edits, (SyncEdits{"caret -6", "back", "insert o", "caret 6"}));
    EXPECT_EQ(sync.caret(), 13u);

    // the IME moved its caret alone (a swipe on the space bar)
    out.edits.clear();
    syncTyped(sync, U"one two three", 3, out);
    EXPECT_EQ(out.edits, (SyncEdits{"caret -10"}));
}

TEST(TextInputSync, EmojiAndSurrogatePairsCountOneCodePoint){
    TextInputSync sync;
    SyncRecorder out;
    sync.reset(U"hi", 2);

    // an emoji is two UTF-16 units and one code point
    syncTyped(sync, U"hi\U0001F600", 4, out);
    EXPECT_EQ(out.edits, (SyncEdits{"insert \xF0\x9F\x98\x80"}));
    EXPECT_EQ(sync.caret(), 3u);

    // erased with one backspace
    out.edits.clear();
    syncTyped(sync, U"hi", 2, out);
    EXPECT_EQ(out.edits, (SyncEdits{"back"}));

    // a lone surrogate is U+FFFD
    std::u32string text;
    std::vector<size_t> unitToCodepoint;
    utf16ToCodepoints(std::u16string{u'a', (char16_t)0xD83D}, text, unitToCodepoint);
    EXPECT_EQ(text, (std::u32string{U'a', 0xFFFD}));
}

TEST(TextInputSync, ResetStartsOverWithoutEdits){
    TextInputSync sync;
    SyncRecorder out;
    sync.reset(U"abc", 2);
    syncComposing(sync, U"abxc", 2, 3, out);
    ASSERT_TRUE(sync.isComposing());

    // the field's text was sent back to the keyboard
    out.edits.clear();
    sync.reset(U"ab", 9);
    EXPECT_FALSE(sync.isComposing());
    EXPECT_EQ(sync.caret(), 2u);
    syncTyped(sync, U"ab", 2, out);
    EXPECT_TRUE(out.edits.empty());
}

namespace {

    class CountingEdits : public TextInputSync::Output {
    public:
        size_t edits = 0;
        void moveCaret(int) override { edits++; }
        void backspace() override { edits++; }
        void insertText(const std::string&) override { edits++; }
        void compose(const std::string&, int) override { edits++; }
        void cancelComposition() override { edits++; }
    };

}

// R26: one edit batch of a 4,096 code point field costs O(length), within 50 us on the mobile
// reference device; printed here, enforced with JASPER_TEST_PERF_SLACK (the benchmark run)
TEST(TextInputSyncBench, EditOfA4096CodePointField){
    std::u32string base(4096, U'a');
    std::u32string edited = base;
    edited[2048] = U'b';    // a correction in the middle, the caret at the end
    const std::u16string after = syncUnitsOf(edited);

    double best = 1e9;
    for (int run = 0; run < 50; run++){
        TextInputSync sync;
        sync.reset(base, base.size());
        CountingEdits out;
        const auto start = std::chrono::steady_clock::now();
        sync.update(after, 4096, 4096, -1, -1, out);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        best = std::min(best, us);
        ASSERT_EQ(out.edits, 4u) << "caret there, one backspace, the text, the caret back";
        ASSERT_EQ(sync.text(), edited);
    }
    std::printf("[bench] TextInputSync, one edit of a 4096 code point field %.1f us (budget 50 us mobile)\n", best);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(best, 50.0 * testsupport::perfSlack());
    }
}

}  // namespace TextInputSyncTest_cpp
