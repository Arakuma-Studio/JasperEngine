// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The compat CSS parser against css-parsing-tests (stylesheet.json, rule_list.json,
// one_rule.json, declaration_list.json, one_declaration.json, blocks_contents.json) and the
// CSS Nesting forms the rewriter flattens (phase-2 §3.9.1).

#include "CompatTestCommon.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssParser.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine::test::compat;

namespace {

    // Inputs whose 2021 expectation differs from the CSS Syntax 2024 algorithms the parser
    // implements (nesting); each has its 2024 result checked elsewhere.
    const std::set<std::string> kDeclarationList2024 = {
        // 2021: a bad declaration swallowed everything up to the next ';', so "a:b" was lost;
        // 2024 reparses "@ media screen { div{;}}" as a qualified rule (blocks_contents.json
        // has the 2024 expectation for the same input)
        "@ media screen { div{;}} a:b;; @media print{div{",
    };
    const std::set<std::string> kOneDeclaration2024 = {
        // 2021 "parse a declaration" took everything to the end; 2024 stops at ';'
        "foo:;",
        "foo:;bar:;",
    };

    nlohmann::json trimDeclarationValues(nlohmann::json items){
        // the 2021 tests keep the whitespace around declaration values; 2024 trims it
        for (auto& item : items){
            if (item.is_array() && item.size() == 4 && item[0] == "declaration")
                item[2] = trimJsonWhitespace(item[2]);
        }
        return items;
    }

    void runRuleListTests(const char* file, bool topLevel){
        const nlohmann::json tests = loadParsingTests(file);
        ASSERT_TRUE(tests.is_array()) << file;
        for (size_t i = 0; i + 1 < tests.size(); i += 2){
            const std::string input = tests[i].get<std::string>();
            std::vector<ParseError> errors;
            const std::vector<Rule> rules = parseRuleList(1, input, 0, topLevel, nullptr, &errors);
            ParsingTestsJson json(input, 0, errors);
            const nlohmann::json actual = itemsWithErrors(json, {}, rules, errors);
            EXPECT_TRUE(jsonNear(actual, tests[i + 1])) << file << " input: " << nlohmann::json(input).dump()
                << "\nexpected: " << tests[i + 1].dump() << "\nactual:   " << actual.dump();
        }
    }

    // the only diagnostics of a parse, as toText lines
    std::vector<std::string> diagnosticLines(const DiagnosticSink& sink){
        std::vector<std::string> lines;
        for (const Diagnostic& diagnostic : sink.diagnostics())
            lines.push_back(toText(diagnostic));
        return lines;
    }

}

TEST(CompatParser, Stylesheet){
    runRuleListTests("stylesheet.json", true);
}

TEST(CompatParser, RuleList){
    runRuleListTests("rule_list.json", false);
}

TEST(CompatParser, OneRule){
    const nlohmann::json tests = loadParsingTests("one_rule.json");
    ASSERT_TRUE(tests.is_array());
    for (size_t i = 0; i + 1 < tests.size(); i += 2){
        const std::string input = tests[i].get<std::string>();
        std::vector<ParseError> errors;
        const std::optional<Rule> rule = parseRule(1, input, 0, nullptr, &errors);
        ParsingTestsJson json(input, 0, errors);
        nlohmann::json actual;
        if (rule){
            actual = json.rule(*rule);
        }else{
            ASSERT_FALSE(errors.empty()) << input;
            const ParseError::Kind kind = errors.back().kind;
            actual = nlohmann::json::array({"error", kind == ParseError::Kind::Empty ? "empty"
                                                   : kind == ParseError::Kind::ExtraInput ? "extra-input" : "invalid"});
        }
        EXPECT_TRUE(jsonNear(actual, tests[i + 1])) << "input: " << nlohmann::json(input).dump()
            << "\nexpected: " << tests[i + 1].dump() << "\nactual:   " << actual.dump();
    }
}

TEST(CompatParser, DeclarationList){
    const nlohmann::json tests = loadParsingTests("declaration_list.json");
    ASSERT_TRUE(tests.is_array());
    for (size_t i = 0; i + 1 < tests.size(); i += 2){
        const std::string input = tests[i].get<std::string>();
        if (kDeclarationList2024.count(input))
            continue;
        std::vector<ParseError> errors;
        const Rule block = parseBlockContents(1, input, 0, nullptr, &errors);
        ParsingTestsJson json(input, 0, errors);
        const nlohmann::json actual = itemsWithErrors(json, block.declarations, block.rules, errors);
        EXPECT_TRUE(jsonNear(actual, trimDeclarationValues(tests[i + 1]))) << "input: " << nlohmann::json(input).dump()
            << "\nexpected: " << tests[i + 1].dump() << "\nactual:   " << actual.dump();
    }
}

TEST(CompatParser, OneDeclaration){
    const nlohmann::json tests = loadParsingTests("one_declaration.json");
    ASSERT_TRUE(tests.is_array());
    for (size_t i = 0; i + 1 < tests.size(); i += 2){
        const std::string input = tests[i].get<std::string>();
        std::vector<ParseError> errors;
        const std::optional<Declaration> declaration = parseDeclaration(1, input, 0, nullptr, &errors);
        if (kOneDeclaration2024.count(input)){
            // the 2024 result: the value stops before the ';'
            ASSERT_TRUE(declaration.has_value());
            EXPECT_EQ(declaration->name, "foo");
            EXPECT_TRUE(declaration->value.empty());
            continue;
        }
        ParsingTestsJson json(input, 0, errors);
        nlohmann::json actual;
        if (declaration){
            actual = json.declaration(*declaration);
        }else{
            ASSERT_FALSE(errors.empty()) << input;
            actual = nlohmann::json::array({"error", errors.back().kind == ParseError::Kind::Empty ? "empty" : "invalid"});
        }
        nlohmann::json expected = tests[i + 1];
        if (expected.is_array() && expected.size() == 4 && expected[0] == "declaration")
            expected[2] = trimJsonWhitespace(expected[2]);
        EXPECT_TRUE(jsonNear(actual, expected)) << "input: " << nlohmann::json(input).dump()
            << "\nexpected: " << expected.dump() << "\nactual:   " << actual.dump();
    }
}

TEST(CompatParser, BlocksContents){
    const nlohmann::json tests = loadParsingTests("blocks_contents.json");
    ASSERT_TRUE(tests.is_array());
    for (size_t i = 0; i + 1 < tests.size(); i += 2){
        const std::string input = tests[i].get<std::string>();
        std::vector<ParseError> errors;
        const Rule block = parseBlockContents(1, input, 0, nullptr, &errors);
        ParsingTestsJson json(input, 0, errors);
        const nlohmann::json actual = itemsWithErrors(json, block.declarations, block.rules, errors);
        EXPECT_TRUE(jsonNear(actual, trimDeclarationValues(tests[i + 1]))) << "input: " << nlohmann::json(input).dump()
            << "\nexpected: " << tests[i + 1].dump() << "\nactual:   " << actual.dump();
    }
}

TEST(CompatParser, NestingMixedDeclarationsAndRules){
    SourceFiles files;
    const std::string css =
        ".card {\n"
        "  color: red;\n"
        "  & .title { font-weight: bold }\n"
        "  .icon { width: 16px; }\n"
        "  &:hover { color: blue }\n"
        "  > p { margin: 0 }\n"
        "  @media (min-width: 600px) { padding: 8px; & .title { font-size: 20px } }\n"
        "  background: #000;\n"
        "  --Accent: { a: b } ;\n"
        "}\n";
    const uint32_t file = files.add("ui/nest.css", css);
    DiagnosticSink sink(files);
    const Stylesheet sheet = parseStylesheet(file, css, sink);
    EXPECT_EQ(sink.size(), 0u) << (sink.size() ? toText(sink.diagnostics()[0]) : "");
    ASSERT_EQ(sheet.rules.size(), 1u);

    const Rule& card = sheet.rules[0];
    EXPECT_EQ(card.kind, Rule::Kind::Style);
    EXPECT_EQ(files.slice(card.preludeRef), ".card");
    ASSERT_EQ(card.declarations.size(), 3u);
    EXPECT_EQ(card.declarations[0].name, "color");
    EXPECT_EQ(card.declarations[1].name, "background");
    // a custom property keeps its case and may hold a {} block
    EXPECT_EQ(card.declarations[2].name, "--Accent");
    ASSERT_EQ(card.declarations[2].value.size(), 1u);
    EXPECT_TRUE(card.declarations[2].value[0].isBlock(TokenType::LeftBrace));

    ASSERT_EQ(card.rules.size(), 5u);
    EXPECT_EQ(files.slice(card.rules[0].preludeRef), "& .title");
    EXPECT_EQ(files.slice(card.rules[1].preludeRef), ".icon");
    EXPECT_EQ(files.slice(card.rules[2].preludeRef), "&:hover");
    EXPECT_EQ(files.slice(card.rules[3].preludeRef), "> p");
    const Rule& media = card.rules[4];
    EXPECT_TRUE(media.isAt("media"));
    EXPECT_EQ(files.slice(media.preludeRef), "(min-width: 600px)");
    ASSERT_EQ(media.declarations.size(), 1u);
    EXPECT_EQ(media.declarations[0].name, "padding");
    ASSERT_EQ(media.rules.size(), 1u);
    EXPECT_EQ(files.slice(media.rules[0].preludeRef), "& .title");

    // declarations after nested rules keep their source position (2024: they follow the
    // nested rules in the cascade); the rewriter orders by ref
    EXPECT_GT(card.declarations[1].ref.offset, media.ref.offset);

    // positions
    const SourcePos colorPos = files.position(file, card.declarations[0].nameRef.offset);
    EXPECT_EQ(colorPos.line, 2u);
    EXPECT_EQ(colorPos.column, 3u);
    EXPECT_EQ(files.slice(card.declarations[0].valueRef), "red");
    EXPECT_EQ(files.slice(card.declarations[0].ref), "color: red");
    EXPECT_EQ(files.slice(card.rules[1].declarations[0].ref), "width: 16px");
}

TEST(CompatParser, DeclarationsImportantCaseAndRefs){
    SourceFiles files;
    const std::string css = "a { COLOR: Red !IMPORTANT; Width : calc( 1px + 2px ) ! important ; margin:0 }";
    const uint32_t file = files.add("ui/a.css", css);
    DiagnosticSink sink(files);
    const Stylesheet sheet = parseStylesheet(file, css, sink);
    ASSERT_EQ(sheet.rules.size(), 1u);
    const std::vector<Declaration>& d = sheet.rules[0].declarations;
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0].name, "color");
    EXPECT_TRUE(d[0].important);
    EXPECT_EQ(files.slice(d[0].valueRef), "Red");
    EXPECT_EQ(files.slice(d[0].ref), "COLOR: Red !IMPORTANT");
    EXPECT_EQ(files.slice(d[0].nameRef), "COLOR");
    EXPECT_EQ(d[1].name, "width");
    EXPECT_TRUE(d[1].important);
    ASSERT_EQ(d[1].value.size(), 1u);
    EXPECT_TRUE(d[1].value[0].isFunction("calc"));
    EXPECT_EQ(files.slice(d[1].valueRef), "calc( 1px + 2px )");
    EXPECT_FALSE(d[2].important);
    EXPECT_EQ(sink.size(), 0u);
}

TEST(CompatParser, SyntaxErrorsAreRecoveredAndReported){
    SourceFiles files;
    const std::string css =
        "a { color: red; }\n"
        "b { width: 10px; oops; height: 5px }\n"
        "c { content: \"unclosed\n"
        "  ; top: 1px }\n"
        "d { background: url(a b.png); left: 2px }\n"
        "foo\n";
    const uint32_t file = files.add("ui/broken.css", css);
    DiagnosticSink sink(files);
    const Stylesheet sheet = parseStylesheet(file, css, sink);

    ASSERT_EQ(sheet.rules.size(), 4u);
    ASSERT_EQ(sheet.rules[1].declarations.size(), 2u);   // "oops" dropped, the rest kept
    EXPECT_EQ(sheet.rules[1].declarations[1].name, "height");
    ASSERT_EQ(sheet.rules[2].declarations.size(), 2u);
    EXPECT_EQ(sheet.rules[2].declarations[1].name, "top");
    ASSERT_EQ(sheet.rules[3].declarations.size(), 2u);
    EXPECT_EQ(sheet.rules[3].declarations[1].name, "left");

    const std::vector<std::string> lines = diagnosticLines(sink);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0], "ui/broken.css:2:18: warning: invalid rule 'oops' dropped (fix: give the selector a { ... } block) [HTMLUI-CSS-0008]");
    EXPECT_EQ(lines[1].rfind("ui/broken.css:3:14: warning: a string is not closed before the end of the line", 0), 0u) << lines[1];
    EXPECT_EQ(lines[2].rfind("ui/broken.css:5:17: warning: invalid url(", 0), 0u) << lines[2];
    EXPECT_EQ(lines[3].rfind("ui/broken.css:6:1: warning: invalid rule 'foo", 0), 0u) << lines[3];
    for (const Diagnostic& diagnostic : sink.diagnostics())
        EXPECT_EQ(diagnostic.code, codes::CssSyntaxError);
}

TEST(CompatParser, UnclosedBlockAtEndOfFile){
    SourceFiles files;
    const std::string css = "@media (x) {\n  a { color: red;\n";
    const uint32_t file = files.add("ui/open.css", css);
    DiagnosticSink sink(files);
    const Stylesheet sheet = parseStylesheet(file, css, sink);
    ASSERT_EQ(sheet.rules.size(), 1u);
    ASSERT_EQ(sheet.rules[0].rules.size(), 1u);
    ASSERT_EQ(sheet.rules[0].rules[0].declarations.size(), 1u);
    const std::vector<std::string> lines = diagnosticLines(sink);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "ui/open.css:1:12: warning: '{' is not closed before the end of the stylesheet (fix: add the missing closing bracket) [HTMLUI-CSS-0008]");
    EXPECT_EQ(lines[1].rfind("ui/open.css:2:5:", 0), 0u) << lines[1];
}

TEST(CompatParser, InlineStyleOffsetsMapToTheHtmlFile){
    // an inline <style> parsed from its position inside the HTML file
    SourceFiles files;
    const std::string html = "<html>\n<head>\n<style>\n  p { color: blue; bad }\n</style>";
    const uint32_t file = files.add("ui/page.html", html);
    const size_t begin = html.find("<style>") + 7;
    const size_t end = html.find("</style>");
    DiagnosticSink sink(files);
    const Stylesheet sheet = parseStylesheet(file, std::string_view(html).substr(begin, end - begin), sink, (uint32_t)begin);
    ASSERT_EQ(sheet.rules.size(), 1u);
    const SourcePos pos = files.position(file, sheet.rules[0].declarations[0].nameRef.offset);
    EXPECT_EQ(pos.line, 4u);
    EXPECT_EQ(pos.column, 7u);
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink.diagnostics()[0].line, 4u);
    EXPECT_EQ(sink.diagnostics()[0].column, 20u);
}

TEST(CompatParser, DeclarationListDropsNestedRules){
    SourceFiles files;
    const std::string style = "color: red; a { b: c } width: 1px";
    const uint32_t file = files.add("ui/x.html", style);
    DiagnosticSink sink(files);
    const std::vector<Declaration> declarations = parseDeclarationList(file, style, 0, sink);
    ASSERT_EQ(declarations.size(), 2u);
    EXPECT_EQ(declarations[0].name, "color");
    EXPECT_EQ(declarations[1].name, "width");
    EXPECT_EQ(sink.size(), 1u);
}

TEST(CompatParser, KeyframesAndFontFaceBlocks){
    SourceFiles files;
    const std::string css =
        "@keyframes fade { from { opacity: 0 } 50%, 75% { opacity: .5 } to { opacity: 1 } }\n"
        "@font-face { font-family: \"Inter\"; src: url(fonts/inter.ttf) format('truetype'); unicode-range: U+0000-00FF; }\n"
        "@import url(\"base.css\") layer(base) supports(display: grid) screen;\n";
    const uint32_t file = files.add("ui/k.css", css);
    DiagnosticSink sink(files);
    const Stylesheet sheet = parseStylesheet(file, css, sink);
    EXPECT_EQ(sink.size(), 0u);
    ASSERT_EQ(sheet.rules.size(), 3u);

    const Rule& keyframes = sheet.rules[0];
    EXPECT_TRUE(keyframes.isAt("keyframes"));
    ASSERT_EQ(keyframes.rules.size(), 3u);
    EXPECT_EQ(files.slice(keyframes.rules[1].preludeRef), "50%, 75%");
    ASSERT_EQ(keyframes.rules[1].declarations.size(), 1u);

    const Rule& fontFace = sheet.rules[1];
    EXPECT_TRUE(fontFace.isAt("font-face"));
    ASSERT_EQ(fontFace.declarations.size(), 3u);
    const Declaration& range = fontFace.declarations[2];
    EXPECT_EQ(range.name, "unicode-range");
    ASSERT_EQ(range.value.size(), 1u);
    EXPECT_TRUE(range.value[0].isToken(TokenType::UnicodeRange));
    EXPECT_EQ(range.value[0].token.rangeEnd, 0xFFu);
    const ComponentValue& url = fontFace.declarations[1].value[0];
    EXPECT_TRUE(url.isToken(TokenType::Url));
    EXPECT_EQ(url.token.value, "fonts/inter.ttf");

    const Rule& import = sheet.rules[2];
    EXPECT_TRUE(import.isAt("import"));
    EXPECT_FALSE(import.hasBlock);
    EXPECT_EQ(files.slice(import.preludeRef), "url(\"base.css\") layer(base) supports(display: grid) screen");
}

TEST(CompatParser, HelpersSplitAndTrim){
    const std::vector<ComponentValue> values = parseComponentValueList(" a b , c(d, e) , f ");
    const std::vector<std::vector<ComponentValue>> groups = splitAtCommas(values);
    ASSERT_EQ(groups.size(), 3u);
    EXPECT_EQ(trimWhitespace(groups[0]).size(), 3u);
    ASSERT_EQ(trimWhitespace(groups[1]).size(), 1u);
    EXPECT_TRUE(trimWhitespace(groups[1])[0].isFunction("c"));
    EXPECT_EQ(trimWhitespace(groups[2]).size(), 1u);
}
