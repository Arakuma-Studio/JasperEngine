// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The UTF-8 helpers of text fields, the HTML UI and the log.

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

// toUTF8 takes a whole code point: a 16-bit wchar_t (Windows) would turn U+1F600 into U+F600
TEST(StringUtils, ToUtf8EncodesEveryPlane){
    EXPECT_EQ(StringUtils::toUTF8(U'\U0001F600'), "\xF0\x9F\x98\x80");
    EXPECT_EQ(StringUtils::toUTF8(U'\U0010FFFF'), "\xF4\x8F\xBF\xBF");
    EXPECT_EQ(StringUtils::toUTF8(static_cast<char32_t>(0x110000)), "\xEF\xBF\xBD");
    EXPECT_EQ(StringUtils::toUTF8(static_cast<char32_t>(0xDC00)), "\xEF\xBF\xBD");
}

// every InputField edit re-encodes the whole text: characters beyond U+FFFF already in it survive
TEST(StringUtils, EditsKeepCharactersBeyondTheBmp){
    std::string text = "a\xF0\x9F\x98\x80" "b";
    StringUtils::insertUtf8AtCodepoint(text, 1, "x");
    EXPECT_EQ(text, "ax\xF0\x9F\x98\x80" "b");
    StringUtils::eraseCodepointsUtf8(text, 0, 1);
    EXPECT_EQ(text, "x\xF0\x9F\x98\x80" "b");
    StringUtils::eraseCodepointsUtf8(text, 2, 3);
    EXPECT_EQ(text, "x\xF0\x9F\x98\x80");
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

// The rules nlohmann::json and the log hold text to: no overlong forms, no encoded surrogates,
// nothing past U+10FFFF; a repair replaces each bad byte with U+FFFD and keeps the rest
TEST(StringUtils, ValidatesAndRepairsUtf8){
    for (const char* valid : {"", "plain ASCII text, longer than eight bytes", "caf\xC3\xA9", "\xE2\x82\xAC",
                              "\xF0\x9F\x98\x80", "\xF4\x8F\xBF\xBF", "a\xEF\xBF\xBDz"}){
        EXPECT_TRUE(StringUtils::isValidUtf8(valid)) << valid;
        EXPECT_EQ(StringUtils::repairUtf8(valid), valid);
        EXPECT_EQ(StringUtils::legacyToUtf8(valid), valid);
    }
    for (const char* invalid : {"\xC0\xAF", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
                                "\xC3", "\xE2\x82", "\x80", "\xFF"}){
        EXPECT_FALSE(StringUtils::isValidUtf8(invalid)) << testing::PrintToString(std::string(invalid));
    }
    EXPECT_EQ(StringUtils::repairUtf8("ok \xFF\xC3 caf\xC3\xA9"), "ok \xEF\xBF\xBD\xEF\xBF\xBD caf\xC3\xA9");
    EXPECT_EQ(StringUtils::repairUtf8("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD") << "one per byte";
    EXPECT_EQ(StringUtils::repairUtf8("12345678\xE2\x82"), "12345678\xEF\xBF\xBD\xEF\xBF\xBD") << "cut after the ASCII run";
#ifndef _WIN32
    EXPECT_EQ(StringUtils::legacyToUtf8("Jo\xE3o"), "Jo\xEF\xBF\xBDo");
#endif
}
