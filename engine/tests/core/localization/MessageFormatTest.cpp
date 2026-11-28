// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ICU message subset of the String Tables (docs/specs/game-ui-localization.md R3-R5, AC1):
// named arguments, CLDR plural categories, select and nesting, number symbols per language, and
// patterns that do not parse printing their own text.

#include <gtest/gtest.h>

#include "localization/MessageFormat.h"
#include "localization/PluralRules.h"

#include <random>
#include <string>

namespace MessageFormatTest_cpp {

using namespace jasperengine;
using namespace jasperengine::l10n;

namespace {

    std::string fmt(std::string_view pattern, const LocalizationArgs& args, std::string_view language = "en", bool showMissing = true) {
        FormatOptions options;
        options.language = language;
        options.args = &args;
        options.showMissing = showMissing;
        std::string out;
        MessageFormat::format(pattern, options, out);
        return out;
    }

    const char* kCategories = "{n, plural, zero {zero} one {one} two {two} few {few} many {many} other {other}}";

    std::string category(std::string_view language, LocalizationValue n) {
        return fmt(kCategories, {{"n", n}}, language);
    }

}

TEST(MessageFormat, SubstitutesNamedArguments) {
    EXPECT_EQ(fmt("Hello, {name}!", {{"name", std::string("Ana")}}), "Hello, Ana!");
    EXPECT_EQ(fmt("{a} and {b}", {{"b", std::string("B")}, {"a", std::string("A")}}), "A and B");
    EXPECT_EQ(fmt("{ score }", {{"score", int64_t(1234567)}}, "en"), "1,234,567");
    EXPECT_EQ(fmt("{score}", {{"score", int64_t(1234567)}}, "de"), "1.234.567");
    EXPECT_EQ(fmt("{x}", {{"x", 2.5}}, "en"), "2.5");
    // a missing argument: its name in development, nothing in release (R5)
    EXPECT_EQ(fmt("Hi {name}", {}), "Hi {name}");
    EXPECT_EQ(fmt("Hi {name}", {}, "en", false), "Hi ");
    // quoting: '' is one apostrophe, a quote before a brace starts a literal run
    EXPECT_EQ(fmt("It''s {n}", {{"n", int64_t(5)}}), "It's 5");
    EXPECT_EQ(fmt("Don't", {}), "Don't");
    EXPECT_EQ(fmt("'{literal}' {n}", {{"n", int64_t(1)}}), "{literal} 1");
    EXPECT_TRUE(MessageFormat::isPlain("Play"));
    EXPECT_FALSE(MessageFormat::isPlain("{n} coins"));
}

TEST(MessageFormat, PluralPicksTheCldrCategory) {
    // Russian: one 1, 21; few 2; many 5; other 1.5
    EXPECT_EQ(category("ru", int64_t(1)), "one");
    EXPECT_EQ(category("ru", int64_t(2)), "few");
    EXPECT_EQ(category("ru", int64_t(5)), "many");
    EXPECT_EQ(category("ru", int64_t(21)), "one");
    EXPECT_EQ(category("ru", 1.5), "other");
    // Arabic: zero, one, two, few (3..10), many (11..99), other (100)
    EXPECT_EQ(category("ar", int64_t(0)), "zero");
    EXPECT_EQ(category("ar", int64_t(1)), "one");
    EXPECT_EQ(category("ar", int64_t(2)), "two");
    EXPECT_EQ(category("ar", int64_t(3)), "few");
    EXPECT_EQ(category("ar", int64_t(11)), "many");
    EXPECT_EQ(category("ar", int64_t(100)), "other");
    // Polish
    EXPECT_EQ(category("pl", int64_t(1)), "one");
    EXPECT_EQ(category("pl", int64_t(22)), "few");
    EXPECT_EQ(category("pl", int64_t(12)), "many");
    EXPECT_EQ(category("pl", int64_t(25)), "many");
    // Japanese has only other; a subtag falls back to its language; an unknown language is root
    EXPECT_EQ(category("ja", int64_t(1)), "other");
    EXPECT_EQ(category("ru-RU", int64_t(2)), "few");
    EXPECT_EQ(category("xx", int64_t(1)), "other");
    // an explicit =N wins over the keyword, wherever it is written
    EXPECT_EQ(fmt("{n, plural, one {one} =1 {exactly one} other {#}}", {{"n", int64_t(1)}}), "exactly one");
    EXPECT_EQ(fmt("{n, plural, =0 {no coins} one {# coin} other {# coins}}", {{"n", int64_t(0)}}), "no coins");
    EXPECT_EQ(fmt("{n, plural, =0 {no coins} one {# coin} other {# coins}}", {{"n", int64_t(1000)}}), "1,000 coins");
    // offset: # and the keyword use n - offset, =N the argument itself
    const char* offset = "{n, plural, offset:1 =0 {nobody} =1 {only you} one {you and # other} other {you and # others}}";
    EXPECT_EQ(fmt(offset, {{"n", int64_t(0)}}), "nobody");
    EXPECT_EQ(fmt(offset, {{"n", int64_t(1)}}), "only you");
    EXPECT_EQ(fmt(offset, {{"n", int64_t(2)}}), "you and 1 other");
    EXPECT_EQ(fmt(offset, {{"n", int64_t(3)}}), "you and 2 others");
    // operands: "1.0" is not one in English (v = 1)
    EXPECT_EQ(PluralRules::select("en", 1.0), PluralCategory::One);
    PluralOperands operands;
    ASSERT_TRUE(PluralRules::operands("1.0", operands));
    EXPECT_EQ(PluralRules::select("en", operands), PluralCategory::Other);
    EXPECT_EQ(PluralRules::categories("ru"), "one few many other");
    EXPECT_EQ(PluralRules::categories("ja"), "other");
}

TEST(MessageFormat, SelectAndNestedPlural) {
    const char* pattern =
        "{g, select, female {{n, plural, one {She has # cat} other {She has # cats}}} "
        "other {{n, plural, one {They have # cat} other {They have # cats}}}}";
    EXPECT_EQ(fmt(pattern, {{"g", std::string("female")}, {"n", int64_t(1)}}), "She has 1 cat");
    EXPECT_EQ(fmt(pattern, {{"g", std::string("male")}, {"n", int64_t(3)}}), "They have 3 cats");
    // # inside a select nested in a plural is the plural's number
    EXPECT_EQ(fmt("{n, plural, other {{g, select, a {# A} other {# O}}}}", {{"n", int64_t(7)}, {"g", std::string("a")}}), "7 A");
    // four levels parse; a fifth is refused
    const char* four = "{a, select, other {{b, select, other {{c, select, other {{d, select, other {deep}}}}}}}}";
    EXPECT_TRUE(MessageFormat::validate(four));
    EXPECT_EQ(fmt(four, {{"a", std::string("x")}, {"b", std::string("x")}, {"c", std::string("x")}, {"d", std::string("x")}}), "deep");
    const char* five = "{a, select, other {{b, select, other {{c, select, other {{d, select, other {{e, select, other {x}}}}}}}}}}";
    MessageError error;
    EXPECT_FALSE(MessageFormat::validate(five, &error));
    EXPECT_NE(error.message.find("4 levels"), std::string::npos) << error.message;
    // an "@ns.key" string argument is resolved through the resolver (one level)
    FormatOptions options;
    options.language = "en";
    LocalizationArgs args = {{"item", std::string("@items.sword")}};
    options.args = &args;
    options.resolveKey = [](void*, std::string_view key, std::string& text, std::string& language) {
        if (key != "items.sword") return false;
        text = "sword";
        language = "en";
        return true;
    };
    std::string out;
    MessageFormat::format("You found a {item}", options, out);
    EXPECT_EQ(out, "You found a sword");
}

TEST(MessageFormat, NumberUsesLanguageSymbols) {
    EXPECT_EQ(fmt("{n, number}", {{"n", 1234.5}}, "de"), "1.234,5");
    // French groups with a narrow no-break space (U+202F)
    EXPECT_EQ(fmt("{n, number}", {{"n", 1234.5}}, "fr"), "1\xE2\x80\xAF" "234,5");
    EXPECT_EQ(fmt("{n, number}", {{"n", 1234.5}}, "en"), "1,234.5");
    EXPECT_EQ(fmt("{n, number, integer}", {{"n", 1234.5}}, "en"), "1,234");
    EXPECT_EQ(fmt("{n, number, percent}", {{"n", 0.25}}, "en"), "25%");
    EXPECT_EQ(fmt("{n, number, percent}", {{"n", 0.25}}, "de"), "25\xC2\xA0%");
    // Spanish groups from five digits (minimum grouping 2), Hindi in twos after the first three
    EXPECT_EQ(fmt("{n}", {{"n", int64_t(1234)}}, "es"), "1234");
    EXPECT_EQ(fmt("{n}", {{"n", int64_t(12345)}}, "es"), "12.345");
    EXPECT_EQ(fmt("{n}", {{"n", int64_t(1234567)}}, "hi"), "12,34,567");
    EXPECT_EQ(fmt("{n}", {{"n", int64_t(-42)}}, "en"), "-42");
    EXPECT_EQ(fmt("{n, plural, one {# Münze} other {# Münzen}}", {{"n", int64_t(1500)}}, "de"), "1.500 Münzen");
}

TEST(MessageFormat, BadPatternsPrintSourceText) {
    const char* broken[] = {"{", "}", "{n", "{n,", "{n, plural, one {x}}", "{n, select, a {x}}", "{n, bogus}", "{, number}",
                            "{n, number, currency}", "{n, plural, one {x} other {y}", "{n, plural, seven {x} other {y}}", "a } b"};
    for (const char* pattern : broken) {
        MessageError error;
        EXPECT_FALSE(MessageFormat::validate(pattern, &error)) << pattern;
        EXPECT_FALSE(error.message.empty()) << pattern;
        EXPECT_LE(error.offset, std::string_view(pattern).size()) << pattern;
        EXPECT_EQ(fmt(pattern, {{"n", int64_t(1)}}), pattern);
    }
    // 500 mutations of valid patterns: never a throw, never longer than the pattern plus the arguments
    const std::string seeds[] = {"{n, plural, offset:1 =0 {none} one {# item} other {# items}}",
                                 "{g, select, a {{n, plural, one {x} other {#}}} other {y}}", "It''s '{'{name}'}' {n, number, percent}"};
    const LocalizationArgs args = {{"n", int64_t(3)}, {"g", std::string("a")}, {"name", std::string("Zoe")}};
    const char alphabet[] = "{}#,'=: 0123456789abcdefghijklmnopqrstuvwxyz";
    std::mt19937 random(20261119);
    for (int k = 0; k < 500; k++) {
        std::string pattern = seeds[k % 3];
        const int edits = 1 + static_cast<int>(random() % 4);
        for (int e = 0; e < edits && !pattern.empty(); e++) {
            const size_t at = random() % pattern.size();
            switch (random() % 3) {
                case 0: pattern.erase(at, 1); break;
                case 1: pattern.insert(at, 1, alphabet[random() % (sizeof(alphabet) - 1)]); break;
                default: pattern[at] = alphabet[random() % (sizeof(alphabet) - 1)]; break;
            }
        }
        std::string out;
        EXPECT_NO_THROW(out = fmt(pattern, args));
        EXPECT_LE(out.size(), pattern.size() * 8 + 64) << pattern;
    }
}

}  // namespace MessageFormatTest_cpp
