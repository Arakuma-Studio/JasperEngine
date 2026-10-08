// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSAST_H
#define COMPAT_CSS_CSSAST_H

// Tokens, component values, declarations and rules of CSS Syntax Level 3 (phase-2 §3.9.1),
// with byte positions into the source file for diagnostics and line maps.

#include "htmlui/compat/CompatTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    enum class TokenType : uint8_t { Ident, Function, AtKeyword, Hash, String, BadString, Url,
        BadUrl, Delim, Number, Percentage, Dimension, UnicodeRange, Whitespace, CDO, CDC, Colon,
        Semicolon, Comma, LeftBracket, RightBracket, LeftParen, RightParen, LeftBrace, RightBrace,
        Comment, EndOfFile };

    struct Token {
        TokenType type = TokenType::EndOfFile;
        uint32_t offset = 0, length = 0;   // source bytes (with the base offset of tokenize())
        std::string value;                 // unescaped ident/string/url/function/at-keyword/hash name; Delim char (UTF-8)
        double number = 0;                 // Number/Percentage/Dimension (Percentage: 50 for "50%")
        bool isInteger = false;
        std::string unit;                  // Dimension, unescaped, case kept
        bool hashIsId = false;             // Hash: "id" type (a valid identifier follows '#')
        uint32_t rangeStart = 0, rangeEnd = 0;   // UnicodeRange

        bool is(TokenType t) const { return type == t; }
        bool isDelim(char c) const { return type == TokenType::Delim && value.size() == 1 && value[0] == c; }
        // Ident whose value equals `lowercase` ASCII case-insensitively
        bool isIdent(const char* lowercase) const;
    };

    // A token, a simple block ({}, [], ()) or a function (phase-2 §3.9.1).
    struct ComponentValue {
        enum class Kind : uint8_t { Token, Block, Function };
        Kind kind = Kind::Token;
        Token token;                        // Token; for a block its opening token, for a function the Function token
        std::vector<ComponentValue> children;
        uint32_t offset = 0, length = 0;    // whole extent in the source (blocks: through the closing token, if any)
        bool closed = true;                 // Block/Function: false when the input ended before the closing token

        bool isToken(TokenType t) const { return kind == Kind::Token && token.type == t; }
        bool isBlock(TokenType opening) const { return kind == Kind::Block && token.type == opening; }
        bool isFunction(const char* lowercaseName) const;
        bool isWhitespace() const { return isToken(TokenType::Whitespace); }
        // Function name / block opening without the '('
        const std::string& name() const { return token.value; }
    };

    struct Declaration {
        std::string name;                   // lowercase unless custom property (--x keeps case)
        std::vector<ComponentValue> value;  // trimmed, without !important
        bool important = false;
        SourceRef ref, nameRef, valueRef;   // whole declaration (through the value / !important), name, value

        bool isCustomProperty() const { return name.size() >= 2 && name[0] == '-' && name[1] == '-'; }
    };

    struct Rule {
        enum class Kind : uint8_t { Style, At };
        Kind kind = Kind::Style;
        std::string atName;                       // lowercase, without '@'
        std::vector<ComponentValue> prelude;      // selector list or at-rule prelude (raw, whitespace kept)
        SourceRef preludeRef, ref;                // the prelude; the whole rule
        bool hasBlock = false;
        std::vector<Declaration> declarations;    // style rules, @font-face, @page, keyframe blocks, ...
        std::vector<Rule> rules;                  // nested style rules, @media/@supports/@layer bodies, keyframe selectors
        // The block's raw contents, as "consume a simple block" sees them; declarations and
        // rules above are the same tokens parsed as a block's contents (CSS Syntax 2024):
        // every block holds declarations and rules mixed, in source order by their refs.
        std::vector<ComponentValue> block;
        SourceRef blockRef;                       // the block, braces included

        bool isAt(const char* lowercaseName) const { return kind == Kind::At && atName == lowercaseName; }
    };

    struct Stylesheet {
        std::vector<Rule> rules;
        uint32_t file = 0;
    };

}

#endif //COMPAT_CSS_CSSAST_H
