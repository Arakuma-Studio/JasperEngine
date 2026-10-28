// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The compat CSS tokenizer against css-parsing-tests (component_value_list.json,
// one_component_value.json, stylesheet_bytes.json minus the encodings the engine does not
// decode), and the byte offsets of every token.

#include "CompatTestCommon.h"

#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssTokenizer.h"

#include <gtest/gtest.h>

using namespace jasperengine::test::compat;

namespace {

    std::string fromJsonBytes(const std::string& utf8){
        // the tests write bytes as code points U+0000-U+00FF
        std::string bytes;
        size_t i = 0;
        while (i < utf8.size()){
            const unsigned char c = (unsigned char)utf8[i];
            if (c < 0x80){
                bytes += (char)c;
                i++;
            }else{
                // two-byte UTF-8 sequence for U+0080-U+00FF
                const unsigned char next = (unsigned char)utf8[i + 1];
                bytes += (char)(((c & 0x1F) << 6) | (next & 0x3F));
                i += 2;
            }
        }
        return bytes;
    }

    nlohmann::json lowercaseAtRuleNames(nlohmann::json value){
        if (value.is_array()){
            if (value.size() == 4 && value[0] == "at-rule" && value[1].is_string()){
                std::string name = value[1].get<std::string>();
                for (char& c : name){
                    if (c >= 'A' && c <= 'Z')
                        c = (char)(c - 'A' + 'a');
                }
                value[1] = name;
            }
            for (auto& child : value)
                child = lowercaseAtRuleNames(child);
        }
        return value;
    }

}

TEST(CompatTokenizer, ComponentValueList){
    const nlohmann::json tests = loadParsingTests("component_value_list.json");
    ASSERT_TRUE(tests.is_array());
    ASSERT_GT(tests.size(), 0u);
    for (size_t i = 0; i + 1 < tests.size(); i += 2){
        const std::string input = tests[i].get<std::string>();
        std::vector<ParseError> errors;
        TokenizeOptions options;
        options.unicodeRanges = true;   // the 2021 draft makes them everywhere
        const ValueList values = parseComponentValueList(input, 0, options, &errors);
        ParsingTestsJson json(input, 0, errors);
        const nlohmann::json actual = json.list(values);
        EXPECT_TRUE(jsonNear(actual, tests[i + 1])) << "input: " << nlohmann::json(input).dump()
            << "\nexpected: " << tests[i + 1].dump() << "\nactual:   " << actual.dump();
    }
}

TEST(CompatTokenizer, OneComponentValue){
    const nlohmann::json tests = loadParsingTests("one_component_value.json");
    ASSERT_TRUE(tests.is_array());
    for (size_t i = 0; i + 1 < tests.size(); i += 2){
        const std::string input = tests[i].get<std::string>();
        std::vector<ParseError> errors;
        const std::optional<ComponentValue> value = parseComponentValue(input, 0, TokenizeOptions(), &errors);
        nlohmann::json actual;
        if (value){
            ParsingTestsJson json(input, 0, errors);
            actual = json.one(*value);
        }else{
            ASSERT_FALSE(errors.empty());
            actual = nlohmann::json::array({"error", parseErrorName(errors.back().kind)});
        }
        EXPECT_TRUE(jsonNear(actual, tests[i + 1])) << "input: " << nlohmann::json(input).dump()
            << "\nexpected: " << tests[i + 1].dump() << "\nactual:   " << actual.dump();
    }
}

TEST(CompatTokenizer, StylesheetBytesUtf8){
    // Only UTF-8 is decoded (a BOM is stripped first); the cases whose expected encoding is
    // another one are skipped: the loader reports HTMLUI-GEN-0005 for them instead.
    const nlohmann::json tests = loadParsingTests("stylesheet_bytes.json");
    ASSERT_TRUE(tests.is_array());
    size_t checked = 0;
    for (size_t i = 0; i + 1 < tests.size(); i += 2){
        const nlohmann::json& expected = tests[i + 1];
        if (expected[1] != "utf-8")
            continue;
        std::string bytes = fromJsonBytes(tests[i]["css_bytes"].get<std::string>());
        stripUtf8Bom(bytes);
        std::vector<ParseError> errors;
        const RuleList rules = parseRuleList(1, bytes, 0, true, nullptr, &errors);
        ParsingTestsJson json(bytes, 0, errors);
        const nlohmann::json actual = itemsWithErrors(json, {}, rules, errors);
        // at-rule names are lowercased by the parser (phase-2 §3.9.1)
        EXPECT_TRUE(jsonNear(actual, lowercaseAtRuleNames(expected[0]))) << "input: " << tests[i].dump()
            << "\nexpected: " << expected[0].dump() << "\nactual:   " << actual.dump();
        checked++;
    }
    EXPECT_GE(checked, 14u);
}


TEST(CompatTokenizer, PositionsCountBytesAfterNormalizedNewlines){
    // "\r\n" is one newline token-wise, but offsets stay those of the bytes
    const std::string css = "a\r\nb\rc\fd /* x */ \"\xC3\xA9\"";
    const std::vector<Token> tokens = tokenize(css, 10);
    ASSERT_EQ(tokens[0].type, TokenType::Ident);
    EXPECT_EQ(tokens[0].offset, 10u);
    ASSERT_EQ(tokens[1].type, TokenType::Whitespace);
    EXPECT_EQ(tokens[1].offset, 11u);
    EXPECT_EQ(tokens[1].length, 2u);
    EXPECT_EQ(tokens[2].value, "b");
    EXPECT_EQ(tokens[2].offset, 13u);
    EXPECT_EQ(tokens[4].value, "c");
    EXPECT_EQ(tokens[6].value, "d");
    ASSERT_EQ(tokens[8].type, TokenType::Comment);
    EXPECT_EQ(css.substr(tokens[8].offset - 10, tokens[8].length), "/* x */");
    ASSERT_EQ(tokens[10].type, TokenType::String);
    EXPECT_EQ(tokens[10].value, "\xC3\xA9");
    EXPECT_EQ(tokens[10].length, 4u);
    EXPECT_EQ(tokens.back().type, TokenType::EndOfFile);
}

TEST(CompatTokenizer, NumbersUnitsAndEscapes){
    const std::vector<Token> tokens = tokenize("12.5px -.5em 50% 1e3 1e-2 +3 10\\70x \\31 23 #fff #-a #1a");
    std::vector<Token> t;
    for (const Token& token : tokens){
        if (token.type != TokenType::Whitespace)
            t.push_back(token);
    }
    ASSERT_EQ(t[0].type, TokenType::Dimension);
    EXPECT_DOUBLE_EQ(t[0].number, 12.5);
    EXPECT_EQ(t[0].unit, "px");
    EXPECT_FALSE(t[0].isInteger);
    EXPECT_DOUBLE_EQ(t[1].number, -0.5);
    EXPECT_EQ(t[1].unit, "em");
    ASSERT_EQ(t[2].type, TokenType::Percentage);
    EXPECT_DOUBLE_EQ(t[2].number, 50);
    EXPECT_TRUE(t[2].isInteger);
    EXPECT_DOUBLE_EQ(t[3].number, 1000);
    EXPECT_FALSE(t[3].isInteger);
    EXPECT_DOUBLE_EQ(t[4].number, 0.01);
    EXPECT_DOUBLE_EQ(t[5].number, 3);
    ASSERT_EQ(t[6].type, TokenType::Dimension);
    EXPECT_EQ(t[6].unit, "px");
    ASSERT_EQ(t[7].type, TokenType::Ident);
    EXPECT_EQ(t[7].value, "123");
    ASSERT_EQ(t[8].type, TokenType::Hash);
    EXPECT_EQ(t[8].value, "fff");
    EXPECT_TRUE(t[8].hashIsId);
    EXPECT_TRUE(t[9].hashIsId);
    EXPECT_FALSE(t[10].hashIsId);
}

TEST(CompatTokenizer, UnicodeRangesOnlyWhenAsked){
    std::vector<Token> plain = tokenize("u+a1");
    EXPECT_EQ(plain[0].type, TokenType::Ident);
    EXPECT_TRUE(plain[1].isDelim('+'));   // "+a1" is not a number: '+', then an ident
    EXPECT_EQ(plain[2].value, "a1");
    TokenizeOptions options;
    options.unicodeRanges = true;
    std::vector<Token> ranges = tokenize("U+0025-00FF", 0, options);
    ASSERT_EQ(ranges[0].type, TokenType::UnicodeRange);
    EXPECT_EQ(ranges[0].rangeStart, 0x25u);
    EXPECT_EQ(ranges[0].rangeEnd, 0xFFu);
}

TEST(CompatTokenizer, InvalidUtf8AndNulReadAsReplacement){
    const std::string css = std::string("a") + '\0' + "b\xFF" + "c";
    const std::vector<Token> tokens = tokenize(css);
    ASSERT_EQ(tokens[0].type, TokenType::Ident);
    EXPECT_EQ(tokens[0].value, "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c");
    EXPECT_EQ(tokens[0].length, css.size());
}

TEST(CompatTokenizer, ErrorsAreReportedWithOffsets){
    std::vector<TokenizeError> errors;
    tokenize("a { b: \"x", 0, TokenizeOptions(), &errors);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].kind, TokenizeError::Kind::EofInString);
    EXPECT_EQ(errors[0].offset, 9u);
    errors.clear();
    tokenize("/* open", 0, TokenizeOptions(), &errors);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].kind, TokenizeError::Kind::EofInComment);
}
