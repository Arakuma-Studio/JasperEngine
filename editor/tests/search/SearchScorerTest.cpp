// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one scorer (S25 §6.1, AC6): OpenEngine's tiers as ported, the whole-candidate fallback
// and Filter mode.

#include "search/SearchScorer.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;

TEST(SearchScorer, NormalizeSplitsCamelCaseAcronymsAndPunctuation) {
    EXPECT_EQ(search::normalize("RigidBody2D"), "rigid body2 d");
    EXPECT_EQ(search::normalize("HTMLDocument"), "html document");
    EXPECT_EQ(search::normalize("  game_object--Name "), "game object name");
    EXPECT_EQ(search::normalize("Mesh Renderer"), "mesh renderer");
    EXPECT_EQ(search::normalize(""), "");
}

TEST(SearchScorer, DamerauCountsAnInversionAsOneEdit) {
    EXPECT_EQ(search::damerau("branch", "branch", 2), 0);
    EXPECT_EQ(search::damerau("brnach", "branch", 2), 1);
    EXPECT_EQ(search::damerau("lihgt", "light", 1), 1);
    EXPECT_EQ(search::damerau("abc", "xyz", 1), 2);         // cutoff + 1
    EXPECT_EQ(search::damerau("a", "abcdef", 2), 3);        // length gap alone exceeds it
}

TEST(SearchScorer, PortedSemantics) {
    // tiers: exact, prefix, substring
    EXPECT_EQ(search::fuzzyScore("light", "light"), 1200);
    EXPECT_EQ(search::fuzzyScore("light", "light probe"), 1000 - 6);
    EXPECT_EQ(search::fuzzyScore("probe", "light probe"), 800 - (6 * 4 + 6));
    EXPECT_GT(search::fuzzyScore("light", "light"), search::fuzzyScore("light", "light probe"));
    EXPECT_GT(search::fuzzyScore("light", "light probe"), search::fuzzyScore("light", "spot light"));

    // per word: prefix 180, typo 140 - 35 per edit, subsequence
    EXPECT_EQ(search::fuzzyScore("mesh rend", "mesh renderer"), 1000 - 4);   // a prefix still
    EXPECT_EQ(search::fuzzyScore("rend mesh", "mesh renderer"), (180 - 4) + (180 - 0));
    EXPECT_EQ(search::fuzzyScore("lihgt", "point light"), 140 - 35);
    // no word holds "rgdbdy" in order: the whole-candidate subsequence does
    EXPECT_EQ(search::fuzzyScore("rgdbdy", "rigid body"), search::subsequence("rgdbdy", "rigid body"));
    EXPECT_GT(search::fuzzyScore("rgdbdy", "rigid body"), 0);
    EXPECT_EQ(search::fuzzyScore("zzz", "point light"), -1);
    EXPECT_EQ(search::fuzzyScore("", "anything"), 0);
    EXPECT_EQ(search::fuzzyScore("a", ""), -1);

    // the whole-candidate fallback: a query spanning words without spaces
    EXPECT_GT(search::fuzzyScore("gmo", "game object"), 0);
    EXPECT_EQ(search::fuzzyScore("ogm", "game object"), -1);

    // word starts and runs score more
    EXPECT_GT(search::subsequence("pl", "point light"), search::subsequence("pl", "apple"));
    EXPECT_EQ(search::subsequence("xy", "point light"), -1);
}

TEST(SearchScorer, FilterModeIsStricterThanRank) {
    // a bare subsequence ranks but does not filter
    EXPECT_GE(search::fuzzyScore("pl", "spot light"), 0);
    EXPECT_FALSE(search::filterMatch("pl", "spot light"));
    // substring, compact words, word prefixes, typos from three letters
    EXPECT_TRUE(search::filterMatch("light", "spot light"));
    EXPECT_TRUE(search::filterMatch("spotlight", "spot light"));
    EXPECT_TRUE(search::filterMatch("rigid body", "rigidbody2 d"));
    EXPECT_TRUE(search::filterMatch("sp li", "spot light"));
    EXPECT_TRUE(search::filterMatch("lihgt", "spot light"));
    EXPECT_FALSE(search::filterMatch("ab", "spot light"));
    EXPECT_FALSE(search::filterMatch("xq", "xa"));
    EXPECT_FALSE(search::filterMatch("vsync", "auto sync"));   // a typo keeps its first letter
    EXPECT_TRUE(search::filterMatch("vsnyc", "editor vsync"));
    EXPECT_TRUE(search::filterMatch("", "anything"));
    EXPECT_FALSE(search::filterMatch("a", ""));
    EXPECT_TRUE(search::matches(search::MatchMode::Rank, "pl", "spot light"));
    EXPECT_FALSE(search::matches(search::MatchMode::Filter, "pl", "spot light"));
}

TEST(SearchScorer, HighlightRangesCoverTheMatchedText) {
    using Ranges = std::vector<std::pair<size_t, size_t>>;
    EXPECT_EQ(search::highlightRanges("light", "Spot Light"), (Ranges{{5, 10}}));
    EXPECT_EQ(search::highlightRanges("spot light", "Spot Light"), (Ranges{{0, 4}, {5, 10}}));
    EXPECT_TRUE(search::highlightRanges("", "Spot Light").empty());
    const Ranges sub = search::highlightRanges("gmo", "GameObject");
    ASSERT_FALSE(sub.empty());
    EXPECT_EQ(sub.front().first, 0u);
}

// S82 R27, AC11: UTF-8 letters are word characters, so Japanese names are searchable in every
// filter. Red control: the normalize() before R27 turned every byte >= 0x80 into a space and
// returned "glow" for "激熱 Glow".
TEST(SearchScorerTest, KeepsUtf8) {
    EXPECT_EQ(search::normalize("激熱 Glow"), "激熱 glow");
    EXPECT_EQ(search::normalize("フチ"), "フチ");
    // no camelCase boundary inside or next to a multi-byte character
    EXPECT_EQ(search::normalize("激熱Glow"), "激熱glow");
    EXPECT_EQ(search::normalize("Café Bar"), "café bar");
    // Unicode punctuation and spaces separate words as ASCII punctuation does
    EXPECT_EQ(search::normalize("Light · Main Scene"), "light main scene");
    EXPECT_EQ(search::normalize("フチ・ぼかし"), "フチ ぼかし");
    EXPECT_EQ(search::normalize("激熱\xE3\x80\x80Glow"), "激熱 glow");     // U+3000
    EXPECT_EQ(search::normalize("「激熱」"), "激熱");
    // a stray byte is kept rather than dropped (nothing is lost from a name)
    EXPECT_EQ(search::normalize("a\xFF" "b"), "a\xFF" "b");

    size_t length = 0;
    EXPECT_EQ(search::decodeUtf8("激", 0, length), U'激');
    EXPECT_EQ(length, 3u);
    EXPECT_EQ(search::decodeUtf8("\xE6\xBF", 0, length), search::INVALID_UTF8);    // cut short
    EXPECT_EQ(length, 1u);
    EXPECT_EQ(search::decodeUtf8("\xC0\xAF", 0, length), search::INVALID_UTF8);    // overlong
    EXPECT_TRUE(search::isSeparator(U'・'));
    EXPECT_FALSE(search::isSeparator(U'ー'));
    EXPECT_FALSE(search::isSeparator(U'激'));

    // matching: substrings and word prefixes of Japanese words
    EXPECT_TRUE(search::filterMatch(search::normalize("激"), search::normalize("激熱")));
    EXPECT_TRUE(search::filterMatch(search::normalize("熱"), search::normalize("激熱 Glow")));
    EXPECT_GT(search::fuzzyScore(search::normalize("激"), search::normalize("激熱")), 0);
    // no typo tolerance between Japanese words (one byte apart, a different word)
    EXPECT_FALSE(search::filterMatch("あい", "あう"));
    EXPECT_EQ(search::fuzzyScore("あい", "あう"), -1);
    // a subsequence matches whole characters: い's bytes are not taken from あ and Ą
    EXPECT_EQ(search::subsequence("い", "あĄ"), -1);
    EXPECT_GT(search::subsequence("あい", "あかい"), 0);
}
