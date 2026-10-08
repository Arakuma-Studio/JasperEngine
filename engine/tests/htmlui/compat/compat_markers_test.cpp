// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// List markers (phase-2 §3.11.6): the text of every list type for indices 1, 4, 9, 27, 3999,
// 4000 (and the edges: 0, negatives), as Chromium formats them.

#include "htmlui/compat/runtime/ListMarkers.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Row {
        const char* type;
        const char* texts[6];   // indices 1, 4, 9, 27, 3999, 4000
    };

    const int kIndices[6] = {1, 4, 9, 27, 3999, 4000};

    const Row kRows[] = {
        {"decimal", {"1. ", "4. ", "9. ", "27. ", "3999. ", "4000. "}},
        {"decimal-leading-zero", {"01. ", "04. ", "09. ", "27. ", "3999. ", "4000. "}},
        {"lower-alpha", {"a. ", "d. ", "i. ", "aa. ", "ewu. ", "ewv. "}},
        {"lower-latin", {"a. ", "d. ", "i. ", "aa. ", "ewu. ", "ewv. "}},
        {"upper-alpha", {"A. ", "D. ", "I. ", "AA. ", "EWU. ", "EWV. "}},
        {"upper-latin", {"A. ", "D. ", "I. ", "AA. ", "EWU. ", "EWV. "}},
        {"lower-roman", {"i. ", "iv. ", "ix. ", "xxvii. ", "mmmcmxcix. ", "4000. "}},
        {"upper-roman", {"I. ", "IV. ", "IX. ", "XXVII. ", "MMMCMXCIX. ", "4000. "}},
        // 24 letters: 27 = αγ, 3999 = ζχο (5, 21, 14), 4000 = ζχπ
        {"lower-greek", {"\xCE\xB1. ", "\xCE\xB4. ", "\xCE\xB9. ", "\xCE\xB1\xCE\xB3. ", "\xCE\xB6\xCF\x87\xCE\xBF. ", "\xCE\xB6\xCF\x87\xCF\x80. "}},
        {"disc", {"\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 "}},
        {"circle", {"\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 "}},
        {"square", {"\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA "}},
        {"disclosure-open", {"\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE "}},
        {"disclosure-closed", {"\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 "}},
        {"none", {"", "", "", "", "", ""}},
    };

}

TEST(CompatMarkers, EveryTypeAtTheTestIndices){
    for (const Row& row : kRows){
        for (int i = 0; i < 6; i++)
            EXPECT_EQ(ListMarkers::text(row.type, kIndices[i]), row.texts[i]) << row.type << " " << kIndices[i];
    }
}

TEST(CompatMarkers, OutOfRangeFallsBackToDecimal){
    EXPECT_EQ(ListMarkers::text("lower-alpha", 0), "0. ");
    EXPECT_EQ(ListMarkers::text("upper-alpha", -2), "-2. ");
    EXPECT_EQ(ListMarkers::text("lower-roman", 0), "0. ");
    EXPECT_EQ(ListMarkers::text("lower-greek", -1), "-1. ");
    EXPECT_EQ(ListMarkers::text("decimal", -5), "-5. ");
    EXPECT_EQ(ListMarkers::text("decimal-leading-zero", -3), "-03. ");
    EXPECT_EQ(ListMarkers::text("decimal-leading-zero", 0), "00. ");
    EXPECT_EQ(ListMarkers::text("lower-alpha", 26), "z. ");
    EXPECT_EQ(ListMarkers::text("lower-alpha", 702), "zz. ");
    EXPECT_EQ(ListMarkers::text("lower-alpha", 703), "aaa. ");
    EXPECT_EQ(ListMarkers::text("lower-greek", 24), "\xCF\x89. ");
}

TEST(CompatMarkers, TypesAndStrings){
    EXPECT_EQ(ListMarkers::parseType("LOWER-ROMAN"), ListMarkers::Type::LowerRoman);
    EXPECT_EQ(ListMarkers::parseType(" disc "), ListMarkers::Type::Disc);
    EXPECT_EQ(ListMarkers::parseType("armenian"), ListMarkers::Type::Decimal);
    EXPECT_EQ(ListMarkers::parseType("cjk-decimal"), ListMarkers::Type::Decimal);
    EXPECT_EQ(ListMarkers::parseType("-> "), ListMarkers::Type::String);
    EXPECT_TRUE(ListMarkers::isCounter(ListMarkers::Type::LowerGreek));
    EXPECT_FALSE(ListMarkers::isCounter(ListMarkers::Type::Disc));
    EXPECT_STREQ(ListMarkers::typeName(ListMarkers::Type::UpperRoman), "upper-roman");
    EXPECT_STREQ(ListMarkers::typeName(ListMarkers::Type::String), "string");
    // strings are used as written (their spaces kept), quotes removed
    EXPECT_EQ(ListMarkers::text("\xE2\x86\x92 ", 3), "\xE2\x86\x92 ");
    EXPECT_EQ(ListMarkers::text("\"- \"", 3), "- ");
    EXPECT_EQ(ListMarkers::text("'*'", 3), "*");
    EXPECT_EQ(ListMarkers::counter(ListMarkers::Type::UpperRoman, 1994), "MCMXCIV");
    EXPECT_EQ(ListMarkers::counter(ListMarkers::Type::Disc, 1), "");
}
