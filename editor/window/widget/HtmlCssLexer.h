// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Syntax highlighting of HTML/RML and CSS/RCSS, one line at a time. No ImGui and no editor
    // state, so the HTML UI test target compiles it too
    class HtmlCssLexer {
    public:
        enum class Language {
            Html,
            Css
        };

        // Colour class of a token, CustomTextEditor maps it onto its TokenType
        enum class Kind {
            Comment,
            Keyword,
            Type,
            String,
            Number,
            Builtin,
            Preprocessor
        };

        struct Token {
            int start;
            int length;
            Kind kind;
        };

        // What one line hands to the next
        struct State {
            // Html
            bool inHtmlComment = false;           // inside <!-- -->
            bool inHtmlTag = false;               // between "<name" and its '>'
            bool tagOpensStyle = false;           // that tag is <style>, its '>' starts a style block
            bool tagOpensScript = false;          // that tag is <script>
            bool expectingAttributeValue = false; // after "name="
            bool attributeIsStyle = false;        // the last attribute name read is "style"
            char attributeQuote = 0;              // inside a quoted attribute value: its quote
            bool inStyleBlock = false;            // between <style> and </style>, lexed as CSS
            bool inScriptBlock = false;           // between <script> and </script>, left uncoloured

            // A style="" value, lexed as CSS declarations
            bool styleAttributeInValue = false;
            bool styleAttributeInComment = false;

            // Css: a stylesheet, or the content of a style block
            bool inCssComment = false;
            bool cssInValue = false;              // after "property:" until ';' or '}'
            bool cssInAtRulePrelude = false;      // after "@rule" until '{' or ';'
            bool cssAtRuleHoldsRules = false;     // that at-rule's block holds rules (@media), not declarations
            int cssDepth = 0;                     // open braces
            uint32_t cssDeclarationBlocks = 0;    // bit d set: the block at depth d + 1 holds declarations
        };

        // Tokens of one line, in order and not overlapping; text between them has the default
        // colour. state holds the previous line's end state on entry and this line's on return
        static std::vector<Token> lexLine(Language language, const std::string& line, State& state);

        // True if state, the state right before a word, puts that word where a CSS property name
        // goes: in a declaration block before its ':', or in an Html style="" value
        static bool expectsCssProperty(Language language, const State& state);
    };

} // namespace jasperengine::editor
