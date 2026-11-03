// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The UTF-8 helpers of text fields and the HTML UI. Only codepoints up to U+FFFF are re-encoded
// here: toUTF8 takes a wchar_t, 16 bits wide on Windows.

#include "util/StringUtils.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;

TEST(StringUtils, ToUTF8EncodesOneToThreeBytes){
    EXPECT_EQ(StringUtils::toUTF8(L'A'), "A");
    EXPECT_EQ(StringUtils::toUTF8(static_cast<wchar_t>(0xE9)), "\xC3\xA9");
    EXPECT_EQ(StringUtils::toUTF8(static_cast<wchar_t>(0x20AC)), "\xE2\x82\xAC");
}

TEST(StringUtils, ToUTF8ReplacesSurrogates){
    EXPECT_EQ(StringUtils::toUTF8(static_cast<wchar_t>(0xD800)), "\xEF\xBF\xBD");
}

TEST(StringUtils, DecodesCodepoints){
    bool invalid = true;
    const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", invalid);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(codepoints, (std::vector<uint32_t>{0x61, 0xE9, 0x20AC, 0x1F600}));
}

TEST(StringUtils, InvalidBytesBecomeReplacementCharacters){
    bool invalid = false;
    const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints("a\xFF" "b\xC3", invalid);
    EXPECT_TRUE(invalid);
    EXPECT_EQ(codepoints, (std::vector<uint32_t>{0x61, 0xFFFD, 0x62, 0xFFFD}));
}

TEST(StringUtils, OverlongEncodingsAreInvalid){
    bool invalid = false;
    const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints("\xC0\xAF", invalid);
    EXPECT_TRUE(invalid);
    EXPECT_EQ(codepoints.front(), 0xFFFDu);
}

TEST(StringUtils, EncodedSurrogatesAreInvalid){
    bool invalid = false;
    StringUtils::decodeUtf8ToCodepoints("\xED\xA0\x80", invalid);
    EXPECT_TRUE(invalid);
}

TEST(StringUtils, CountsCodepointsNotBytes){
    EXPECT_EQ(StringUtils::countCodepoints(""), 0u);
    EXPECT_EQ(StringUtils::countCodepoints("abc"), 3u);
    EXPECT_EQ(StringUtils::countCodepoints("\xC3\xA9t\xC3\xA9"), 3u);
    EXPECT_EQ(StringUtils::countCodepoints("\xF0\x9F\x98\x80"), 1u);
}

TEST(StringUtils, Utf8ToWString){
    bool invalid = true;
    EXPECT_EQ(StringUtils::utf8ToWString("h\xC3\xA9", invalid), std::wstring(L"hé"));
    EXPECT_FALSE(invalid);
}

TEST(StringUtils, EraseLastCodepointRemovesWholeSequences){
    std::string text = "a\xC3\xA9";
    StringUtils::eraseLastCodepointUtf8(text);
    EXPECT_EQ(text, "a");
    StringUtils::eraseLastCodepointUtf8(text);
    EXPECT_EQ(text, "");
    StringUtils::eraseLastCodepointUtf8(text);
    EXPECT_EQ(text, "");
}

TEST(StringUtils, EraseCodepointsByIndex){
    std::string text = "h\xC3\xA9llo";
    StringUtils::eraseCodepointsUtf8(text, 1, 3);
    EXPECT_EQ(text, "hlo");
}

TEST(StringUtils, EraseCodepointsClampsAndIgnoresEmptyRanges){
    std::string text = "abc";
    StringUtils::eraseCodepointsUtf8(text, 2, 2);
    EXPECT_EQ(text, "abc");
    StringUtils::eraseCodepointsUtf8(text, 5, 9);
    EXPECT_EQ(text, "abc");
    StringUtils::eraseCodepointsUtf8(text, 1, 99);
    EXPECT_EQ(text, "a");
}

TEST(StringUtils, InsertAtCodepoint){
    std::string text = "\xC3\xA9t\xC3\xA9";
    StringUtils::insertUtf8AtCodepoint(text, 1, "\xE2\x82\xAC");
    EXPECT_EQ(text, "\xC3\xA9\xE2\x82\xACt\xC3\xA9");
    StringUtils::insertUtf8AtCodepoint(text, 99, "!");
    EXPECT_EQ(text, "\xC3\xA9\xE2\x82\xACt\xC3\xA9!");
}

TEST(StringUtils, SplitTrimsAndSkipsEmptyParts){
    EXPECT_EQ(StringUtils::split("a, b ,c", ','), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(StringUtils::split(" one ;; two ", ';'), (std::vector<std::string>{"one", "two"}));
    EXPECT_TRUE(StringUtils::split("", ',').empty());
    EXPECT_TRUE(StringUtils::split(" , ", ',').empty());
}
