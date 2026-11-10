// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The search fold of Title Art (docs/specs/S82-title-designer.md R27, AC11): one rule per test.
// Red controls: each expectation fails when its rule is removed from foldForSearch (the input
// comes back unfolded, e.g. "ＧＬＯＷ" stays full-width, "ｶﾞ" stays half-width with a separate mark).

#include "titleart/designer/TitleSearchFold.h"

#include <gtest/gtest.h>

using jasperengine::editor::titledesigner::foldForSearch;

TEST(TitleSearchFoldTest, FullWidthAsciiBecomesAscii) {
    EXPECT_EQ(foldForSearch("ＧＬＯＷ"), "glow");
    EXPECT_EQ(foldForSearch("Ｒｉｍ２"), "rim2");
    EXPECT_EQ(foldForSearch("ｒｉｍ　ｗｉｄｔｈ"), "rim width");
}

TEST(TitleSearchFoldTest, IdeographicSpaceIsASpace) {
    EXPECT_EQ(foldForSearch("光彩\xE3\x80\x80外側"), "光彩 外側");
    EXPECT_EQ(foldForSearch("\xE3\x80\x80光彩\xE3\x80\x80"), "光彩");
}

TEST(TitleSearchFoldTest, HalfWidthKatakanaWithSoundMarks) {
    EXPECT_EQ(foldForSearch("ｶﾞ"), "が");
    EXPECT_EQ(foldForSearch("ﾊﾟ"), "ぱ");
    EXPECT_EQ(foldForSearch("ﾎﾞｶｼ"), "ぼかし");
    EXPECT_EQ(foldForSearch("ｸﾞﾗﾃﾞｰｼｮﾝ"), "ぐらでーしょん");
    EXPECT_EQ(foldForSearch("ｳﾞ"), "ゔ");
    // a mark that composes with nothing stays a mark
    EXPECT_EQ(foldForSearch("ｱﾞ"), "あ゛");
    // the half-width punctuation separates
    EXPECT_EQ(foldForSearch("ﾌﾁ･ﾎﾞｶｼ"), "ふち ぼかし");
}

TEST(TitleSearchFoldTest, KatakanaBecomesHiragana) {
    EXPECT_EQ(foldForSearch("ボカシ"), "ぼかし");
    EXPECT_EQ(foldForSearch("ぼかし"), "ぼかし");
    EXPECT_EQ(foldForSearch("フチ"), foldForSearch("ふち"));
    EXPECT_EQ(foldForSearch("ヴァ"), "ゔぁ");
    EXPECT_EQ(foldForSearch("グロー"), "ぐろー");     // ー is kept
    EXPECT_EQ(foldForSearch("光彩"), "光彩");          // kanji untouched
}

TEST(TitleSearchFoldTest, AsciiLowerCase) {
    EXPECT_EQ(foldForSearch("Outer Glow"), "outer glow");
    EXPECT_EQ(foldForSearch("RGB"), "rgb");
    EXPECT_EQ(foldForSearch("Café"), "café");          // other scripts are kept as they are
}

TEST(TitleSearchFoldTest, SpacesAndPunctuationCollapse) {
    EXPECT_EQ(foldForSearch("  Rim 2 › Bevel  "), "rim 2 bevel");
    EXPECT_EQ(foldForSearch("style.rims[1].width"), "style rims 1 width");
    EXPECT_EQ(foldForSearch("光彩 (外側)"), "光彩 外側");
    EXPECT_EQ(foldForSearch("「フチ」、ぼかし。"), "ふち ぼかし");
    EXPECT_EQ(foldForSearch("tate-chu-yoko"), "tate chu yoko");
    EXPECT_EQ(foldForSearch("\t\n"), "");
    EXPECT_EQ(foldForSearch(""), "");
}
