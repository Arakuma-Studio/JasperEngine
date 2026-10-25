// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Editor-side logic kept free of ImGui and editor state, compiled into this target as extra
// sources (phase 1, p1-editor).

#include "window/widget/HtmlCssLexer.h"
#include "util/HtmlUITemplates.h"
#include "htmlui/HtmlUIDependencies.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <ostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// HtmlCssLexer: the code editor's HTML/RML and CSS/RCSS highlighter (spec §3.8.5)
// ---------------------------------------------------------------------------------------------

namespace {

    using jasperengine::editor::HtmlCssLexer;
    using Kind = HtmlCssLexer::Kind;
    using Lang = HtmlCssLexer::Language;
    using LexerState = HtmlCssLexer::State;

    const char* kindName(Kind kind) {
        switch (kind) {
            case Kind::Comment: return "Comment";
            case Kind::Keyword: return "Keyword";
            case Kind::Type: return "Type";
            case Kind::String: return "String";
            case Kind::Number: return "Number";
            case Kind::Builtin: return "Builtin";
            case Kind::Preprocessor: return "Preprocessor";
        }
        return "?";
    }

    struct LexToken {
        int start;
        int length;
        Kind kind;

        bool operator==(const LexToken& other) const {
            return start == other.start && length == other.length && kind == other.kind;
        }
    };

    std::ostream& operator<<(std::ostream& os, const LexToken& token) {
        return os << "{" << token.start << ", " << token.length << ", " << kindName(token.kind) << "}";
    }

    std::vector<LexToken> lex(Lang language, const std::string& line, LexerState& state) {
        std::vector<LexToken> result;
        for (const auto& token : HtmlCssLexer::lexLine(language, line, state)) {
            result.push_back({token.start, token.length, token.kind});
        }
        return result;
    }

    // The token covering the occurrence-th (0-based) occurrence of text in line
    LexToken at(const std::string& line, const std::string& text, Kind kind, int occurrence = 0) {
        size_t pos = line.find(text);
        for (int i = 0; i < occurrence && pos != std::string::npos; ++i) pos = line.find(text, pos + 1);
        EXPECT_NE(pos, std::string::npos) << "'" << text << "' not in '" << line << "'";
        return {static_cast<int>(pos), static_cast<int>(text.size()), kind};
    }

    // State after lexing every line in turn from a fresh state
    LexerState stateAfter(Lang language, const std::vector<std::string>& lines) {
        LexerState state;
        for (const auto& line : lines) HtmlCssLexer::lexLine(language, line, state);
        return state;
    }

} // namespace

TEST(HtmlCssLexer, HtmlTagAndQuotedAttributes) {
    LexerState state;
    const std::string line = "<div id=\"main\" class='box'>";
    const std::vector<LexToken> expected = {
        {0, 4, Kind::Keyword},   // <div
        {5, 2, Kind::Type},      // id
        {8, 6, Kind::String},    // "main"
        {15, 5, Kind::Type},     // class
        {21, 5, Kind::String},   // 'box'
        {26, 1, Kind::Keyword},  // >
    };
    EXPECT_EQ(lex(Lang::Html, line, state), expected);
    EXPECT_FALSE(state.inHtmlTag);
    EXPECT_EQ(state.attributeQuote, 0);
}

TEST(HtmlCssLexer, HtmlClosingTagsUnquotedValuesAndEntities) {
    LexerState state;
    const std::string line = "<img src=logo.png data-x:y.z /><p>Tom &amp; Jerry & co &#169; &#x1F600;</p>";
    const std::vector<LexToken> expected = {
        at(line, "<img", Kind::Keyword),
        at(line, "src", Kind::Type),
        at(line, "logo.png", Kind::String),
        at(line, "data-x:y.z", Kind::Type),
        at(line, "/>", Kind::Keyword),
        at(line, "<p", Kind::Keyword),
        at(line, ">", Kind::Keyword, 1),
        at(line, "&amp;", Kind::Builtin),
        // the bare '&' is text
        at(line, "&#169;", Kind::Builtin),
        at(line, "&#x1F600;", Kind::Builtin),
        at(line, "</p", Kind::Keyword),
        at(line, ">", Kind::Keyword, 2),
    };
    EXPECT_EQ(lex(Lang::Html, line, state), expected);
}

TEST(HtmlCssLexer, HtmlDoctypeAndPlainText) {
    LexerState state;
    EXPECT_EQ(lex(Lang::Html, "<!DOCTYPE html>", state), (std::vector<LexToken>{{0, 15, Kind::Preprocessor}}));
    EXPECT_TRUE(lex(Lang::Html, "a < b and c > d", state).empty());
    EXPECT_TRUE(lex(Lang::Html, "", state).empty());
}

TEST(HtmlCssLexer, HtmlCommentCarriedAcrossLines) {
    LexerState state;

    const std::string first = "<p>x</p> <!-- start";
    const std::vector<LexToken> firstExpected = {
        {0, 2, Kind::Keyword},   // <p
        {2, 1, Kind::Keyword},   // >
        {4, 3, Kind::Keyword},   // </p
        {7, 1, Kind::Keyword},   // >
        {9, 10, Kind::Comment},  // <!-- start
    };
    EXPECT_EQ(lex(Lang::Html, first, state), firstExpected);
    EXPECT_TRUE(state.inHtmlComment);

    // Markup inside the comment stays comment, an empty line keeps the state
    EXPECT_EQ(lex(Lang::Html, "  <b>still</b>", state), (std::vector<LexToken>{{0, 14, Kind::Comment}}));
    EXPECT_TRUE(lex(Lang::Html, "", state).empty());
    EXPECT_TRUE(state.inHtmlComment);

    const std::string last = "end --> <b>";
    const std::vector<LexToken> lastExpected = {
        {0, 7, Kind::Comment},   // end -->
        {8, 2, Kind::Keyword},   // <b
        {10, 1, Kind::Keyword},  // >
    };
    EXPECT_EQ(lex(Lang::Html, last, state), lastExpected);
    EXPECT_FALSE(state.inHtmlComment);

    // Opened and closed on one line
    LexerState single;
    EXPECT_EQ(lex(Lang::Html, "<!-- a --> &lt;", single),
              (std::vector<LexToken>{{0, 10, Kind::Comment}, {11, 4, Kind::Builtin}}));
    EXPECT_FALSE(single.inHtmlComment);
}

TEST(HtmlCssLexer, HtmlAttributeValueCarriedAcrossLines) {
    LexerState state;
    const std::string first = "<div title=\"first";
    EXPECT_EQ(lex(Lang::Html, first, state),
              (std::vector<LexToken>{{0, 4, Kind::Keyword}, {5, 5, Kind::Type}, {11, 6, Kind::String}}));
    EXPECT_TRUE(state.inHtmlTag);
    EXPECT_EQ(state.attributeQuote, '"');

    const std::string second = "second\" id=x>";
    const std::vector<LexToken> expected = {
        {0, 7, Kind::String},    // second"
        {8, 2, Kind::Type},      // id
        {11, 1, Kind::String},   // x
        {12, 1, Kind::Keyword},  // >
    };
    EXPECT_EQ(lex(Lang::Html, second, state), expected);
    EXPECT_EQ(state.attributeQuote, 0);
    EXPECT_FALSE(state.inHtmlTag);
}

TEST(HtmlCssLexer, HtmlTagSpanningLines) {
    LexerState state;
    EXPECT_EQ(lex(Lang::Html, "<button", state), (std::vector<LexToken>{{0, 7, Kind::Keyword}}));
    EXPECT_TRUE(state.inHtmlTag);
    EXPECT_EQ(lex(Lang::Html, "    disabled onclick='go()'>Go", state),
              (std::vector<LexToken>{{4, 8, Kind::Type}, {13, 7, Kind::Type}, {21, 6, Kind::String}, {27, 1, Kind::Keyword}}));
    EXPECT_FALSE(state.inHtmlTag);
}

TEST(HtmlCssLexer, HtmlStyleBlockLexedAsCss) {
    LexerState state;

    EXPECT_EQ(lex(Lang::Html, "<style>", state), (std::vector<LexToken>{{0, 6, Kind::Keyword}, {6, 1, Kind::Keyword}}));
    EXPECT_TRUE(state.inStyleBlock);

    const std::string rule = "  .title, #header:hover { color: #ff0; margin: 4px !important; }";
    const std::vector<LexToken> ruleExpected = {
        {2, 6, Kind::Builtin},   // .title
        {10, 7, Kind::Builtin},  // #header
        {17, 6, Kind::Builtin},  // :hover
        {26, 5, Kind::Type},     // color
        {33, 4, Kind::Number},   // #ff0
        {39, 6, Kind::Type},     // margin
        {47, 3, Kind::Number},   // 4px
        {51, 10, Kind::Keyword}, // !important
    };
    EXPECT_EQ(lex(Lang::Html, rule, state), ruleExpected);
    EXPECT_TRUE(state.inStyleBlock);
    EXPECT_EQ(state.cssDepth, 0);

    const std::string media = "  @media (max-width: 600dp) {";
    EXPECT_EQ(lex(Lang::Html, media, state),
              (std::vector<LexToken>{at(media, "@media", Kind::Preprocessor), at(media, "600dp", Kind::Number)}));
    EXPECT_EQ(state.cssDepth, 1);

    // Inside @media the content is rules again: a type selector, then declarations
    const std::string nested = "    h1 { font-size: 1.5em; }";
    EXPECT_EQ(lex(Lang::Html, nested, state),
              (std::vector<LexToken>{at(nested, "h1", Kind::Keyword), at(nested, "font-size", Kind::Type),
                                     at(nested, "1.5em", Kind::Number)}));
    EXPECT_TRUE(lex(Lang::Html, "  }", state).empty());
    EXPECT_EQ(state.cssDepth, 0);

    // </style> leaves CSS on the same line
    const std::string close = "</style><p>#fff</p>";
    EXPECT_EQ(lex(Lang::Html, close, state),
              (std::vector<LexToken>{{0, 7, Kind::Keyword}, {7, 1, Kind::Keyword}, {8, 2, Kind::Keyword},
                                     {10, 1, Kind::Keyword}, {15, 3, Kind::Keyword}, {18, 1, Kind::Keyword}}));
    EXPECT_FALSE(state.inStyleBlock);

    // Opened and closed on one line, case-insensitive tag
    LexerState single;
    const std::string oneLine = "<STYLE>p { margin: 0 }</Style>";
    EXPECT_EQ(lex(Lang::Html, oneLine, single),
              (std::vector<LexToken>{{0, 6, Kind::Keyword}, {6, 1, Kind::Keyword}, {7, 1, Kind::Keyword},
                                     {11, 6, Kind::Type}, {19, 1, Kind::Number}, {22, 7, Kind::Keyword},
                                     {29, 1, Kind::Keyword}}));
    EXPECT_FALSE(single.inStyleBlock);
}

TEST(HtmlCssLexer, HtmlStyleAttributeLexedAsDeclarations) {
    LexerState state;
    const std::string line = "<p style=\"width: 50%; color: red\" class=\"a\">";
    const std::vector<LexToken> expected = {
        at(line, "<p", Kind::Keyword),
        at(line, "style", Kind::Type),
        {9, 1, Kind::String},    // opening quote
        at(line, "width", Kind::Type),
        at(line, "50%", Kind::Number),
        at(line, "color", Kind::Type),
        {32, 1, Kind::String},   // closing quote
        at(line, "class", Kind::Type),
        at(line, "\"a\"", Kind::String),
        at(line, ">", Kind::Keyword),
    };
    EXPECT_EQ(lex(Lang::Html, line, state), expected);
    EXPECT_FALSE(state.inHtmlTag);
}

TEST(HtmlCssLexer, HtmlScriptContentUncoloured) {
    LexerState state;
    const std::string line = "<script>if (a < b) x = \"s\";</script>";
    EXPECT_EQ(lex(Lang::Html, line, state),
              (std::vector<LexToken>{{0, 7, Kind::Keyword}, {7, 1, Kind::Keyword},
                                     at(line, "</script", Kind::Keyword), {35, 1, Kind::Keyword}}));
    EXPECT_FALSE(state.inScriptBlock);
}

TEST(HtmlCssLexer, CssRuleTokens) {
    LexerState state;
    const std::string line = "div > .item::before, a:hover { margin: -2px 1.5rem; --main-gap : 4px }";
    const std::vector<LexToken> expected = {
        {0, 3, Kind::Keyword},   // div
        {6, 5, Kind::Builtin},   // .item
        {11, 8, Kind::Builtin},  // ::before
        {21, 1, Kind::Keyword},  // a
        {22, 6, Kind::Builtin},  // :hover
        {31, 6, Kind::Type},     // margin
        {39, 4, Kind::Number},   // -2px
        {44, 6, Kind::Number},   // 1.5rem
        {52, 10, Kind::Type},    // --main-gap
        {65, 3, Kind::Number},   // 4px
    };
    EXPECT_EQ(lex(Lang::Css, line, state), expected);
    EXPECT_EQ(state.cssDepth, 0);
    EXPECT_FALSE(state.cssInValue);
}

TEST(HtmlCssLexer, CssHexColoursAndIdSelectors) {
    LexerState state;
    // #fff is an id in a selector and a colour in a value
    const std::string line = "#fff { color: #fff }";
    EXPECT_EQ(lex(Lang::Css, line, state),
              (std::vector<LexToken>{{0, 4, Kind::Builtin}, {7, 5, Kind::Type}, {14, 4, Kind::Number}}));

    const std::string values = "#menu { background: #1a2b3c80; color: #abcd; border-color: #12345; outline-color: #ggg }";
    const std::vector<LexToken> expected = {
        at(values, "#menu", Kind::Builtin),
        at(values, "background", Kind::Type),
        at(values, "#1a2b3c80", Kind::Number),
        at(values, "color", Kind::Type),
        at(values, "#abcd", Kind::Number),
        at(values, "border-color", Kind::Type),
        // #12345 (5 digits) and #ggg are no colours
        at(values, "outline-color", Kind::Type),
    };
    EXPECT_EQ(lex(Lang::Css, values, state), expected);
}

TEST(HtmlCssLexer, CssAtRules) {
    LexerState state;

    EXPECT_EQ(lex(Lang::Css, "@import \"base.rcss\";", state),
              (std::vector<LexToken>{{0, 7, Kind::Preprocessor}, {8, 11, Kind::String}}));
    EXPECT_FALSE(state.cssInAtRulePrelude);

    // @font-face holds declarations; an unquoted url() is a string
    const std::string face = "@font-face { font-family: \"Lato\"; src: url(fonts/Lato.ttf); }";
    EXPECT_EQ(lex(Lang::Css, face, state),
              (std::vector<LexToken>{at(face, "@font-face", Kind::Preprocessor), at(face, "font-family", Kind::Type),
                                     at(face, "\"Lato\"", Kind::String), at(face, "src", Kind::Type),
                                     at(face, "fonts/Lato.ttf", Kind::String)}));

    // @keyframes holds rules: keyframe selectors, then declarations
    const std::string frames = "@keyframes pulse { from { opacity: 0; } 50% { opacity: .5; } }";
    EXPECT_EQ(lex(Lang::Css, frames, state),
              (std::vector<LexToken>{at(frames, "@keyframes", Kind::Preprocessor), at(frames, "from", Kind::Keyword),
                                     at(frames, "opacity", Kind::Type), at(frames, "0", Kind::Number),
                                     at(frames, "50%", Kind::Number), at(frames, "opacity", Kind::Type, 1),
                                     at(frames, ".5", Kind::Number)}));
    EXPECT_EQ(state.cssDepth, 0);

    // A prelude spanning lines
    EXPECT_EQ(lex(Lang::Css, "@media screen and", state), (std::vector<LexToken>{{0, 6, Kind::Preprocessor}}));
    EXPECT_TRUE(state.cssInAtRulePrelude);
    const std::string rest = "  (min-width: 800px) { .a { width: 1px } }";
    EXPECT_EQ(lex(Lang::Css, rest, state),
              (std::vector<LexToken>{at(rest, "800px", Kind::Number), at(rest, ".a", Kind::Builtin),
                                     at(rest, "width", Kind::Type, 1), at(rest, "1px", Kind::Number)}));
    EXPECT_EQ(state.cssDepth, 0);
}

TEST(HtmlCssLexer, CssCommentCarriedAcrossLines) {
    LexerState state;

    const std::string first = "a { color: red; } /* first";
    EXPECT_EQ(lex(Lang::Css, first, state),
              (std::vector<LexToken>{{0, 1, Kind::Keyword}, {4, 5, Kind::Type}, {18, 8, Kind::Comment}}));
    EXPECT_TRUE(state.inCssComment);

    EXPECT_EQ(lex(Lang::Css, "   .second { }", state), (std::vector<LexToken>{{0, 14, Kind::Comment}}));
    EXPECT_TRUE(lex(Lang::Css, "", state).empty());
    EXPECT_TRUE(state.inCssComment);

    const std::string last = "end */ b { margin: 0 }";
    EXPECT_EQ(lex(Lang::Css, last, state),
              (std::vector<LexToken>{{0, 6, Kind::Comment}, {7, 1, Kind::Keyword}, {11, 6, Kind::Type},
                                     {19, 1, Kind::Number}}));
    EXPECT_FALSE(state.inCssComment);
}

TEST(HtmlCssLexer, CssDeclarationBlockCarriedAcrossLines) {
    LexerState state;
    EXPECT_EQ(lex(Lang::Css, "body {", state), (std::vector<LexToken>{{0, 4, Kind::Keyword}}));
    EXPECT_EQ(state.cssDepth, 1);
    // Without the brace state "width" would be a type selector
    EXPECT_EQ(lex(Lang::Css, "  width: 10px;", state), (std::vector<LexToken>{{2, 5, Kind::Type}, {9, 4, Kind::Number}}));
    EXPECT_EQ(lex(Lang::Css, "  font-family: 'A B',", state),
              (std::vector<LexToken>{{2, 11, Kind::Type}, {15, 5, Kind::String}}));
    EXPECT_TRUE(state.cssInValue);
    EXPECT_TRUE(lex(Lang::Css, "    sans-serif;", state).empty());
    EXPECT_TRUE(lex(Lang::Css, "}", state).empty());
    EXPECT_EQ(state.cssDepth, 0);
}

TEST(HtmlCssLexer, ExpectsCssProperty) {
    EXPECT_TRUE(HtmlCssLexer::expectsCssProperty(Lang::Css, stateAfter(Lang::Css, {"a { "})));
    EXPECT_TRUE(HtmlCssLexer::expectsCssProperty(Lang::Css, stateAfter(Lang::Css, {"a {", "  color: red;"})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Css, stateAfter(Lang::Css, {"a { color: "})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Css, stateAfter(Lang::Css, {"a "})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Css, stateAfter(Lang::Css, {"@media screen { "})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Css, stateAfter(Lang::Css, {"a { /* "})));

    EXPECT_TRUE(HtmlCssLexer::expectsCssProperty(Lang::Html, stateAfter(Lang::Html, {"<p style=\""})));
    EXPECT_TRUE(HtmlCssLexer::expectsCssProperty(Lang::Html, stateAfter(Lang::Html, {"<p style=\"color: red; "})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Html, stateAfter(Lang::Html, {"<p style=\"color: "})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Html, stateAfter(Lang::Html, {"<p title=\""})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Html, stateAfter(Lang::Html, {"<p "})));
    EXPECT_TRUE(HtmlCssLexer::expectsCssProperty(Lang::Html, stateAfter(Lang::Html, {"<style>", "p { "})));
    EXPECT_FALSE(HtmlCssLexer::expectsCssProperty(Lang::Html, stateAfter(Lang::Html, {"<style>p { }</style>"})));
}

// S17 R38: element names after "<", attribute names inside a tag
TEST(HtmlCssLexer, HtmlNamePosition) {
    using Pos = HtmlCssLexer::NamePosition;
    auto at = [](const std::string& line, int column, LexerState state = LexerState()) {
        return HtmlCssLexer::htmlNamePosition(line, column, state);
    };
    EXPECT_EQ(at("<list-v", 1), Pos::Tag);
    EXPECT_EQ(at("</tree-", 2), Pos::Tag);
    EXPECT_EQ(at("<list-view drop", 11), Pos::Attribute);
    EXPECT_EQ(at("<div title=\"a b", 15), Pos::None) << "in a value";
    EXPECT_EQ(at("<div title=x", 11), Pos::None);
    EXPECT_EQ(at("plain text", 6), Pos::None);
    EXPECT_EQ(at("<p>text more", 8), Pos::None);
    // a tag spanning lines: the next line's words are attribute names
    EXPECT_EQ(at("  tooltip=\"#t\">", 2, stateAfter(Lang::Html, {"<button"})), Pos::Attribute);
}

// ---------------------------------------------------------------------------------------------
// End of HtmlCssLexer
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// HtmlUIDependencies: the static scanner behind hot reload and the export check (spec §3.8.6)
// ---------------------------------------------------------------------------------------------

namespace {

    using jasperengine::HtmlUIReference;
    using jasperengine::HtmlUIFileReader;

    // in-memory asset tree
    HtmlUIFileReader memoryReader(const std::map<std::string, std::string>& files){
        return [files](const std::string& path, std::string* content) -> bool {
            auto it = files.find(path);
            if (it == files.end())
                return false;
            if (content)
                *content = it->second;
            return true;
        };
    }

    // 1-based column of `needle` in line `line` (1-based) of `text`
    int columnOf(const std::string& text, int line, const std::string& needle){
        size_t start = 0;
        for (int i = 1; i < line; i++){
            start = text.find('\n', start) + 1;
        }
        const size_t end = text.find('\n', start);
        const std::string lineText = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t found = lineText.find(needle);
        return found == std::string::npos ? -1 : (int)found + 1;
    }

    struct ExpectedReference {
        std::string path;
        HtmlUIReference::Kind kind;
        std::string fromFile;
        int line;
        int column;
        bool remote;
        bool missing;
        bool outsideAssets;
    };

    void expectReferences(const std::vector<HtmlUIReference>& actual, const std::vector<ExpectedReference>& expected){
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); i++){
            SCOPED_TRACE("reference " + std::to_string(i) + ": " + expected[i].path);
            EXPECT_EQ(actual[i].path, expected[i].path);
            EXPECT_STREQ(jasperengine::getHtmlUIReferenceKindName(actual[i].kind), jasperengine::getHtmlUIReferenceKindName(expected[i].kind));
            EXPECT_EQ(actual[i].fromFile, expected[i].fromFile);
            EXPECT_EQ(actual[i].line, expected[i].line);
            EXPECT_EQ(actual[i].column, expected[i].column);
            EXPECT_EQ(actual[i].remote, expected[i].remote);
            EXPECT_EQ(actual[i].missing, expected[i].missing);
            EXPECT_EQ(actual[i].outsideAssets, expected[i].outsideAssets);
        }
    }

    const std::string scannedDocument =
        "<html>\n"                                                                   // 1
        "<head>\n"                                                                   // 2
        "    <link type=\"text/css\" href=\"doc.css\"/>\n"                           // 3
        "    <link rel=\"stylesheet\" href=\"https://cdn.example.com/x.css\"/>\n"    // 4
        "    <style>@import \"extra.css\"; .a { background: url( 'img/bg.png' ); }</style>\n"  // 5
        "</head>\n"                                                                  // 6
        "<body style=\"decorator: image(icons/a.png);\">\n"                          // 7
        "    <img src=\"img/logo.png\"/>\n"                                          // 8
        "    <svg src=\"icons/star.svg\"/>\n"                                        // 9
        "    <lottie src=\"anim/spin.json\"/>\n"                                     // 10
        "    <img src=\"missing.png\"/>\n"                                           // 11
        "    <img src=\"data:image/png;base64,AAAA\"/>\n"                            // 12
        "    <!-- <img src=\"commented.png\"/> -->\n"                                // 13
        "    <div style=\"color: #fff\">&amp; text url(not-a-ref.png)</div>\n"       // 14
        "</body>\n"                                                                  // 15
        "</html>\n";

    const std::string scannedStyleSheet =
        "@font-face { font-family: \"X\"; src: url(../fonts/x.ttf); }\n"             // 1
        "@import url(\"theme.css\");\n"                                              // 2
        "#b { decorator: svg( icons/b.svg ); }\n"                                    // 3
        "#c { decorator: image(sprite-name); }\n"                                    // 4
        "/* url(commented.png) */ #d { decorator: image(../../outside.png); }\n"     // 5
        "@spritesheet sheet { src: atlas.png; icon: 0px 0px 16px 16px; }\n"          // 6
        "@font-face { font-family: Y; src: y.otf; }\n";                              // 7

    const std::string scannedTheme = ".t {\n    background-image: url(img/t.png?v=2);\n}\n";

    std::map<std::string, std::string> scannedTree(){
        return {
            {"ui/doc.html", scannedDocument}, {"ui/doc.css", scannedStyleSheet}, {"ui/theme.css", scannedTheme},
            {"ui/extra.css", ""}, {"ui/img/bg.png", "x"}, {"ui/icons/a.png", "x"}, {"ui/img/logo.png", "x"},
            {"ui/icons/star.svg", "x"}, {"ui/anim/spin.json", "x"}, {"fonts/x.ttf", "x"}, {"ui/icons/b.svg", "x"},
            {"ui/atlas.png", "x"}, {"y.otf", "x"}, {"ui/img/t.png", "x"}
        };
    }

}

TEST(HtmlUIDependencies, CollectsEveryReferenceWithPositions){
    using K = HtmlUIReference;
    const std::string& d = scannedDocument;
    const std::string& c = scannedStyleSheet;
    const std::string& t = scannedTheme;

    const std::vector<HtmlUIReference> references = jasperengine::collectHtmlUIReferences("ui/doc.html", memoryReader(scannedTree()));

    expectReferences(references, {
        // the document, in reading order
        {"ui/doc.css", K::STYLESHEET, "ui/doc.html", 3, columnOf(d, 3, "doc.css"), false, false, false},
        {"https://cdn.example.com/x.css", K::STYLESHEET, "ui/doc.html", 4, columnOf(d, 4, "https:"), true, false, false},
        {"ui/extra.css", K::STYLESHEET, "ui/doc.html", 5, columnOf(d, 5, "extra.css"), false, false, false},
        {"ui/img/bg.png", K::IMAGE, "ui/doc.html", 5, columnOf(d, 5, "img/bg.png"), false, false, false},
        {"ui/icons/a.png", K::IMAGE, "ui/doc.html", 7, columnOf(d, 7, "icons/a.png"), false, false, false},
        {"ui/img/logo.png", K::IMAGE, "ui/doc.html", 8, columnOf(d, 8, "img/logo.png"), false, false, false},
        {"ui/icons/star.svg", K::SVG, "ui/doc.html", 9, columnOf(d, 9, "icons/star.svg"), false, false, false},
        {"ui/anim/spin.json", K::LOTTIE, "ui/doc.html", 10, columnOf(d, 10, "anim/spin.json"), false, false, false},
        {"ui/missing.png", K::IMAGE, "ui/doc.html", 11, columnOf(d, 11, "missing.png"), false, true, false},
        // then the stylesheets it loads, depth first
        {"fonts/x.ttf", K::FONT, "ui/doc.css", 1, columnOf(c, 1, "../fonts"), false, false, false},
        {"ui/theme.css", K::STYLESHEET, "ui/doc.css", 2, columnOf(c, 2, "theme.css"), false, false, false},
        {"ui/icons/b.svg", K::SVG, "ui/doc.css", 3, columnOf(c, 3, "icons/b.svg"), false, false, false},
        {"outside.png", K::IMAGE, "ui/doc.css", 5, columnOf(c, 5, "../../outside.png"), false, true, true},
        {"ui/atlas.png", K::IMAGE, "ui/doc.css", 6, columnOf(c, 6, "atlas.png"), false, false, false},
        // RCSS's bare @font-face src: RmlUi opens it from the asset root
        {"y.otf", K::FONT, "ui/doc.css", 7, columnOf(c, 7, "y.otf"), false, false, false},
        {"ui/img/t.png", K::IMAGE, "ui/theme.css", 2, columnOf(t, 2, "img/t.png"), false, false, false},
    });

    // as written, and the query of a web URL dropped from the path
    EXPECT_EQ(references[3].raw, "img/bg.png");
    EXPECT_EQ(references.back().raw, "img/t.png?v=2");
}

TEST(HtmlUIDependencies, ImportCyclesAndMissingDocument){
    std::map<std::string, std::string> files = {
        {"a.css", "@import \"b.css\";"},
        {"b.css", "@import 'a.css';"},
        {"doc.rml", "<rml><head><link type=\"text/rcss\" href=\"a.css\"/></head><body/></rml>"}
    };
    const std::vector<HtmlUIReference> references = jasperengine::collectHtmlUIReferences("doc.rml", memoryReader(files));
    ASSERT_EQ(references.size(), 3u);
    EXPECT_EQ(references[0].path, "a.css");
    EXPECT_EQ(references[1].path, "b.css");
    EXPECT_EQ(references[2].path, "a.css");   // b.css's import of a.css, not followed again

    EXPECT_TRUE(jasperengine::collectHtmlUIReferences("nothing.html", memoryReader(files)).empty());
}

TEST(HtmlUIDependencies, TemplatesAndAbsolutePaths){
    std::map<std::string, std::string> files = {
        {"ui/doc.html", "<html><head>\n<link type=\"text/template\" href=\"window.rml\"/>\n</head><body><img src=\"/abs/pic.png\"/></body></html>"},
        {"ui/window.rml", "<template name=\"window\" content=\"c\"><head><link type=\"text/css\" href=\"window.css\"/></head><body/></template>"},
        {"ui/window.css", ""}
    };
    const std::vector<HtmlUIReference> references = jasperengine::collectHtmlUIReferences("ui/doc.html", memoryReader(files));
    ASSERT_EQ(references.size(), 3u);
    EXPECT_EQ(references[0].path, "ui/window.rml");
    EXPECT_EQ(references[0].kind, HtmlUIReference::TEMPLATE);
    EXPECT_EQ(references[0].line, 2);
    EXPECT_EQ(references[1].path, "/abs/pic.png");
    EXPECT_TRUE(references[1].absolute);
    EXPECT_TRUE(references[1].missing);
    // the template's own stylesheet
    EXPECT_EQ(references[2].path, "ui/window.css");
    EXPECT_EQ(references[2].fromFile, "ui/window.rml");
}

TEST(HtmlUIDependencies, Paths){
    bool outside = true;
    EXPECT_EQ(jasperengine::resolveHtmlUIPath("ui/menu/doc.html", "../img/a.png", &outside), "ui/img/a.png");
    EXPECT_FALSE(outside);
    EXPECT_EQ(jasperengine::resolveHtmlUIPath("doc.html", "../a.png", &outside), "a.png");
    EXPECT_TRUE(outside);
    EXPECT_EQ(jasperengine::resolveHtmlUIPath("ui/doc.html", "asset://fonts/x.ttf", &outside), "fonts/x.ttf");
    EXPECT_FALSE(outside);
    EXPECT_EQ(jasperengine::resolveHtmlUIPath("/tmp/p/ui/doc.html", "./a.css"), "/tmp/p/ui/a.css");
    EXPECT_EQ(jasperengine::resolveHtmlUIPath("ui\\doc.html", "img\\a.png"), "ui/img/a.png");

    EXPECT_EQ(jasperengine::normalizeHtmlUIPath("asset://ui/./x/../doc.css"), "ui/doc.css");
    EXPECT_EQ(jasperengine::normalizeHtmlUIPath("/abs//x.css"), "/abs/x.css");

    EXPECT_EQ(jasperengine::getHtmlUIReferenceKind("a.RCSS"), HtmlUIReference::STYLESHEET);
    EXPECT_EQ(jasperengine::getHtmlUIReferenceKind("a.woff2"), HtmlUIReference::FONT);
    EXPECT_EQ(jasperengine::getHtmlUIReferenceKind("a.lottie"), HtmlUIReference::LOTTIE);
    EXPECT_EQ(jasperengine::getHtmlUIReferenceKind("a.jpeg"), HtmlUIReference::IMAGE);
    EXPECT_EQ(jasperengine::getHtmlUIReferenceKind("a.bin"), HtmlUIReference::OTHER);
}

TEST(HtmlUIDependencies, MessageLocations){
    std::string file;
    int line = 0;
    EXPECT_TRUE(jasperengine::parseHtmlUIMessageLocation("XML parse error on line 12 of ui/menu.html.", file, line));
    EXPECT_EQ(file, "ui/menu.html");
    EXPECT_EQ(line, 12);
    EXPECT_TRUE(jasperengine::parseHtmlUIMessageLocation("ui/menu.rcss:7: Invalid link type 'x'", file, line));
    EXPECT_EQ(file, "ui/menu.rcss");
    EXPECT_EQ(line, 7);
    EXPECT_TRUE(jasperengine::parseHtmlUIMessageLocation("@font-face block missing src at asset://ui/a.css:3.", file, line));
    EXPECT_EQ(file, "ui/a.css");
    EXPECT_EQ(line, 3);
    EXPECT_FALSE(jasperengine::parseHtmlUIMessageLocation("Could not load texture: ui/a.png", file, line));
    EXPECT_FALSE(jasperengine::parseHtmlUIMessageLocation("time 12:30 is not a path", file, line));
}

// ---------------------------------------------------------------------------------------------
// HtmlUITemplates: Assets > Create > HTML Document (the document loads in TestDocumentReload)
// ---------------------------------------------------------------------------------------------

TEST(HtmlUITemplates, CreatesDocumentAndStyleSheetWithFreeNames){
    namespace fs = std::filesystem;
    using jasperengine::editor::HtmlUITemplates;

    const fs::path directory = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "editor-logic" / "templates";
    fs::remove_all(directory);
    fs::create_directories(directory);

    EXPECT_EQ(HtmlUITemplates::uniqueDocumentName(directory), "NewDocument");
    std::string error;
    ASSERT_TRUE(HtmlUITemplates::createDocument(directory, "NewDocument", error)) << error;
    EXPECT_TRUE(fs::is_regular_file(directory / "NewDocument.html"));
    EXPECT_TRUE(fs::is_regular_file(directory / "NewDocument.css"));

    // a taken .html or .css alone makes the name taken
    EXPECT_EQ(HtmlUITemplates::uniqueDocumentName(directory), "NewDocument1");
    std::ofstream(directory / "NewDocument1.css") << "";
    EXPECT_EQ(HtmlUITemplates::uniqueDocumentName(directory), "NewDocument2");

    // the document links its stylesheet and has one heading and one button
    std::ifstream html(directory / "NewDocument.html");
    const std::string text((std::istreambuf_iterator<char>(html)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("<link rel=\"stylesheet\" href=\"NewDocument.css\">"), std::string::npos);
    EXPECT_NE(text.find("<h1"), std::string::npos);
    EXPECT_NE(text.find("<button"), std::string::npos);

    // the scanner sees the stylesheet as its only dependency
    const std::vector<HtmlUIReference> references = jasperengine::collectHtmlUIReferences(
        "NewDocument.html", [&directory](const std::string& path, std::string* content) -> bool {
            std::ifstream file(directory / path, std::ios::binary);
            if (!file) return false;
            if (content) content->assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            return true;
        });
    ASSERT_EQ(references.size(), 1u);
    EXPECT_EQ(references[0].path, "NewDocument.css");
    EXPECT_FALSE(references[0].missing);

    // an unwritable directory (under a file: the atomic writer makes missing directories) fails
    EXPECT_FALSE(HtmlUITemplates::createDocument(directory / "NewDocument.html" / "dir", "X", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(directory / "NewDocument.html" / "dir" / "X.css"));
}
