// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSAST_H
#define COMPAT_CSS_CSSAST_H

// Tokens, component values, declarations and rules of CSS Syntax Level 3 (phase-2 §3.9.1),
// with byte positions into the source file for diagnostics and line maps.

#include "htmlui/compat/Arena.h"
#include "htmlui/compat/CompatTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    enum class TokenType : uint8_t { Ident, Function, AtKeyword, Hash, String, BadString, Url,
        BadUrl, Delim, Number, Percentage, Dimension, UnicodeRange, Whitespace, CDO, CDC, Colon,
        Semicolon, Comma, LeftBracket, RightBracket, LeftParen, RightParen, LeftBrace, RightBrace,
        Comment, EndOfFile };

// (a comparison with a literal is inlined: the literal's length is then known where it is written)
#if defined(_MSC_VER)
    #define COMPAT_TEXT_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define COMPAT_TEXT_INLINE inline __attribute__((always_inline))
#else
    #define COMPAT_TEXT_INLINE inline
#endif

    // The text of a token: bytes of the source or of the current Arena, never freed one by one
    // (16 bytes, copied as a pointer and a length). Made from a string it copies the string
    // into the current arena; Text::view() refers to bytes that outlive it (the parser's copy
    // of the source, literals). Reads as a std::string_view.
    class Text {
    public:
        constexpr Text() noexcept: data_(""), size_(0) {}
        Text(std::string_view text): data_(text.empty() ? "" : Arena::current().copy(text.data(), text.size())), size_((uint32_t)text.size()) {}
        Text(const std::string& text): Text(std::string_view(text)) {}
        Text(const char* text): Text(std::string_view(text)) {}
        static constexpr Text view(std::string_view text) noexcept { return Text(text.data(), (uint32_t)text.size()); }

        constexpr operator std::string_view() const noexcept { return std::string_view(data_, size_); }
        operator std::string() const { return std::string(data_, size_); }
        constexpr std::string_view view() const noexcept { return std::string_view(data_, size_); }
        std::string str() const { return std::string(data_, size_); }

        constexpr const char* data() const noexcept { return data_; }
        constexpr size_t size() const noexcept { return size_; }
        constexpr size_t length() const noexcept { return size_; }
        constexpr bool empty() const noexcept { return size_ == 0; }
        constexpr const char* begin() const noexcept { return data_; }
        constexpr const char* end() const noexcept { return data_ + size_; }
        constexpr char operator[](size_t index) const noexcept { return data_[index]; }
        constexpr char front() const noexcept { return data_[0]; }
        constexpr char back() const noexcept { return data_[size_ - 1]; }
        int compare(std::string_view other) const noexcept { return view().compare(other); }
        int compare(size_t position, size_t count, std::string_view other) const { return view().compare(position, count, other); }
        size_t find(std::string_view text, size_t position = 0) const noexcept { return view().find(text, position); }
        size_t find(char c, size_t position = 0) const noexcept { return view().find(c, position); }
        size_t rfind(std::string_view text, size_t position = std::string_view::npos) const noexcept { return view().rfind(text, position); }
        size_t rfind(char c, size_t position = std::string_view::npos) const noexcept { return view().rfind(c, position); }
        std::string substr(size_t position = 0, size_t count = std::string::npos) const { return std::string(view().substr(position, count)); }

        friend bool operator==(const Text& a, const Text& b) noexcept { return a.view() == b.view(); }
        friend bool operator==(const Text& a, std::string_view b) noexcept { return a.view() == b; }
        friend bool operator==(const Text& a, const std::string& b) noexcept { return a.view() == std::string_view(b); }
        friend COMPAT_TEXT_INLINE bool operator==(const Text& a, const char* b) noexcept {
            const size_t length = std::char_traits<char>::length(b);
            return a.size_ == length && std::memcmp(a.data_, b, length) == 0;
        }
        friend bool operator==(std::string_view a, const Text& b) noexcept { return a == b.view(); }
        friend bool operator==(const std::string& a, const Text& b) noexcept { return std::string_view(a) == b.view(); }
        friend COMPAT_TEXT_INLINE bool operator==(const char* a, const Text& b) noexcept { return b == a; }
        friend bool operator!=(const Text& a, const Text& b) noexcept { return !(a == b); }
        friend bool operator!=(const Text& a, std::string_view b) noexcept { return !(a == b); }
        friend bool operator!=(const Text& a, const std::string& b) noexcept { return !(a == b); }
        friend COMPAT_TEXT_INLINE bool operator!=(const Text& a, const char* b) noexcept { return !(a == b); }
        friend bool operator!=(std::string_view a, const Text& b) noexcept { return !(a == b); }
        friend bool operator!=(const std::string& a, const Text& b) noexcept { return !(a == b); }
        friend COMPAT_TEXT_INLINE bool operator!=(const char* a, const Text& b) noexcept { return !(b == a); }
        friend bool operator<(const Text& a, const Text& b) noexcept { return a.view() < b.view(); }

        friend std::string operator+(const std::string& a, const Text& b){ std::string out(a); out.append(b.data_, b.size_); return out; }
        friend std::string operator+(std::string&& a, const Text& b){ a.append(b.data_, b.size_); return std::move(a); }
        friend std::string operator+(const char* a, const Text& b){ std::string out(a); out.append(b.data_, b.size_); return out; }
        friend std::string operator+(const Text& a, const std::string& b){ std::string out(a.data_, a.size_); out += b; return out; }
        friend std::string operator+(const Text& a, const char* b){ std::string out(a.data_, a.size_); out += b; return out; }
        friend std::string operator+(const Text& a, const Text& b){ std::string out(a.data_, a.size_); out.append(b.data_, b.size_); return out; }

    private:
        constexpr Text(const char* data, uint32_t size) noexcept: data_(data), size_(size) {}

        const char* data_;
        uint32_t size_;
    };

    // (members ordered by size: 64 bytes, copied as they are)
    struct Token {
        TokenType type = TokenType::EndOfFile;
        bool isInteger = false;            // Number/Percentage/Dimension
        bool hashIsId = false;             // Hash: "id" type (a valid identifier follows '#')
        uint32_t offset = 0, length = 0;   // source bytes (with the base offset of tokenize())
        uint32_t rangeStart = 0, rangeEnd = 0;   // UnicodeRange
        double number = 0;                 // Number/Percentage/Dimension (Percentage: 50 for "50%")
        Text value;                        // unescaped ident/string/url/function/at-keyword/hash name; Delim char (UTF-8)
        Text unit;                         // Dimension, unescaped, case kept

        bool is(TokenType t) const { return type == t; }
        bool isDelim(char c) const { return type == TokenType::Delim && value.size() == 1 && value[0] == c; }
        // Ident whose value equals `lowercase` ASCII case-insensitively
        bool isIdent(const char* lowercase) const;
    };

    struct ComponentValue;
    // a list of component values (in the current Arena)
    using ValueList = ArenaList<ComponentValue>;

    // A token, a simple block ({}, [], ()) or a function (phase-2 §3.9.1). 96 bytes; moved as
    // its bytes.
    struct ComponentValue {
        enum class Kind : uint8_t { Token, Block, Function };
        Token token;                        // Token; for a block its opening token, for a function the Function token
        ValueList children;
        uint32_t offset = 0, length = 0;    // whole extent in the source (blocks: through the closing token, if any)
        Kind kind = Kind::Token;
        bool closed = true;                 // Block/Function: false when the input ended before the closing token

        bool isToken(TokenType t) const { return kind == Kind::Token && token.type == t; }
        bool isBlock(TokenType opening) const { return kind == Kind::Block && token.type == opening; }
        bool isFunction(const char* lowercaseName) const;
        bool isWhitespace() const { return isToken(TokenType::Whitespace); }
        // Function name / block opening without the '('
        const Text& name() const { return token.value; }
    };

    struct Declaration {
        Text name;                          // lowercase unless custom property (--x keeps case)
        ValueList value;  // trimmed, without !important
        bool important = false;
        SourceRef ref, nameRef, valueRef;   // whole declaration (through the value / !important), name, value
        // the declaration as written (the bytes of ref, in the parser's copy of the source);
        // empty for a declaration that was not parsed from a style sheet or a style attribute
        Text text;

        bool isCustomProperty() const { return name.size() >= 2 && name[0] == '-' && name[1] == '-'; }
    };

    struct Rule {
        enum class Kind : uint8_t { Style, At };
        Kind kind = Kind::Style;
        Text atName;                              // lowercase, without '@'
        ValueList prelude;      // selector list or at-rule prelude (raw, whitespace kept)
        SourceRef preludeRef, ref;                // the prelude; the whole rule
        Text preludeText;                         // the prelude as written, trimmed (the bytes of preludeRef, in the parser's copy of the source)
        bool hasBlock = false;
        ArenaList<Declaration> declarations;      // style rules, @font-face, @page, keyframe blocks, ...
        ArenaList<Rule> rules;                    // nested style rules, @media/@supports/@layer bodies, keyframe selectors
        // The block's raw contents, as "consume a simple block" sees them; declarations and
        // rules above are the same tokens parsed as a block's contents (CSS Syntax 2024):
        // every block holds declarations and rules mixed, in source order by their refs.
        // Filled by the CSS Syntax entry points (parseRuleList, parseRule, ...) only:
        // parseStylesheet and parseDeclarationList leave it empty (one parse per block).
        ValueList block;
        SourceRef blockRef;                       // the block, braces included

        bool isAt(const char* lowercaseName) const { return kind == Kind::At && atName == lowercaseName; }
    };

    // lists of declarations and rules (in the current Arena, as their values)
    using DeclarationList = ArenaList<Declaration>;
    using RuleList = ArenaList<Rule>;

    struct Stylesheet {
        RuleList rules;
        uint32_t file = 0;
    };

}

#endif //COMPAT_CSS_CSSAST_H
