// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSLEXER_H
#define COMPAT_CSS_CSSLEXER_H

// Internal to the compat layer (CssTokenizer.cpp, CssParser.cpp): the tokenizer as a stream.
// CssLexer::next() reads one token straight into a Token (the parser's one-token lookahead,
// then the token of a component value): there is no token list. The source it reads must
// outlive the tokens: a token's value and unit are views of it, or arena copies of what had
// to be decoded (escapes, non-ASCII code points read one by one, U+FFFD replacements).
//
// The lexer can be set back to a position it has read (reset()): a token at a position is
// always the same token, so the parser re-reads a few tokens instead of looking ahead (a
// declaration that turns out to be a nested rule is read again as a rule). Errors and bad
// tokens are recorded once, the first time their position is read.

#include "htmlui/compat/css/CssAst.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/CssTokenizer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // How CssLexer::next() starts reading a token by its first byte
    enum class LexStart : uint8_t {
        Slow,          // the general reading (CssLexer::slow)
        Punctuation,   // ( ) [ ] { } , : ;
        Space,         // ' ' \t \n (\r and \f are read by the general reading)
        Name,          // a-z A-Z _
        Digit,
        Minus, Plus, Dot, Slash, Hash, At, Quote,
        Delim,         // an ASCII character that is a <delim-token> wherever it is
    };
    struct LexStartTable {
        LexStart start[256];
        TokenType punctuation[256];
        constexpr LexStartTable(): start(), punctuation() {
            for (int c = 0; c < 256; c++){
                punctuation[c] = TokenType::EndOfFile;
                LexStart s = LexStart::Slow;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
                    s = LexStart::Name;
                else if (c >= '0' && c <= '9')
                    s = LexStart::Digit;
                else if (c == ' ' || c == '\t' || c == '\n')
                    s = LexStart::Space;
                else if (c == '-') s = LexStart::Minus;
                else if (c == '+') s = LexStart::Plus;
                else if (c == '.') s = LexStart::Dot;
                else if (c == '/') s = LexStart::Slash;
                else if (c == '#') s = LexStart::Hash;
                else if (c == '@') s = LexStart::At;
                else if (c == '"' || c == '\'') s = LexStart::Quote;
                else if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == ',' || c == ':' || c == ';')
                    s = LexStart::Punctuation;
                else if (c >= 0x01 && c < 0x80 && c != '\\' && c != '<' && c != '\r' && c != '\f')
                    s = LexStart::Delim;
                start[c] = s;
            }
            punctuation[(unsigned char)'('] = TokenType::LeftParen;
            punctuation[(unsigned char)')'] = TokenType::RightParen;
            punctuation[(unsigned char)'['] = TokenType::LeftBracket;
            punctuation[(unsigned char)']'] = TokenType::RightBracket;
            punctuation[(unsigned char)'{'] = TokenType::LeftBrace;
            punctuation[(unsigned char)'}'] = TokenType::RightBrace;
            punctuation[(unsigned char)','] = TokenType::Comma;
            punctuation[(unsigned char)':'] = TokenType::Colon;
            punctuation[(unsigned char)';'] = TokenType::Semicolon;
        }
    };
    inline constexpr LexStartTable kLexStart{};

    class CssLexer {
    public:
        // a BadString or BadUrl token (the parser reports them)
        struct BadToken {
            TokenType type;
            uint32_t offset, length;
        };

        // comments false: no Comment tokens (the parser drops them)
        CssLexer(std::string_view source, uint32_t base, const TokenizeOptions& options, bool comments):
            source(source), base(base), options(options), comments(comments) {}

        // the next token (EndOfFile at the end, again and again)
        void next(Token& token){
            if (pos < source.size() && fast(token)){
                if (pos > highWater)
                    highWater = pos;
                return;
            }
            slow(token);
        }

        // CSS Syntax §4.3.13 "convert a string to a number"
        static double convertNumber(std::string_view representation);

        // the byte position of the next token
        size_t position() const { return pos; }
        // reads again from a position next() has stopped at before
        void reset(size_t position){ pos = position; }

        // recorded once per position, in source order
        std::vector<TokenizeError> errors;
        std::vector<BadToken> bad;

    private:
        class Slow;   // the general reading (CssTokenizer.cpp)

        unsigned char byteAt(size_t at) const { return at < source.size() ? (unsigned char)source[at] : 0; }
        static bool digit(unsigned char c){ return c >= '0' && c <= '9'; }
        static bool nameStart(unsigned char c){ return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c == '_'; }
        // the end of the run of ASCII name bytes from `at`; plain: the run ends the name (an
        // ASCII byte that is neither a name byte nor a backslash nor NUL follows, or nothing)
        size_t nameRun(size_t at, bool& plain) const {
            const char* const begin = source.data();
            const size_t size = source.size();
            while (at < size && kNameBytes.is[(unsigned char)begin[at]])
                at++;
            const unsigned char after = at < size ? (unsigned char)begin[at] : 0;
            plain = at == size || (after < 0x80 && after != 0 && after != '\\');
            return at;
        }
        void set(Token& token, TokenType type, size_t at, size_t end){
            token = Token();
            token.type = type;
            token.offset = base + (uint32_t)at;
            token.length = (uint32_t)(end - at);
            pos = end;
        }
        std::string_view slice(size_t from, size_t to) const { return std::string_view(source.data() + from, to - from); }

        // The tokens of plain ASCII, read as the general reading reads them; false leaves the
        // token to it (escapes, non-ASCII, comments, url(, CDO/CDC, unicode ranges, ...)
        bool fast(Token& token){
            const size_t at = pos;
            const unsigned char b = (unsigned char)source[at];
            switch (kLexStart.start[b]){
                case LexStart::Punctuation:
                    set(token, kLexStart.punctuation[b], at, at + 1);
                    return true;
                case LexStart::Space: {
                    size_t i = at + 1;
                    while (i < source.size()){
                        const char w = source[i];
                        if (w != ' ' && w != '\n' && w != '\t' && w != '\r' && w != '\f')
                            break;
                        i++;
                    }
                    set(token, TokenType::Whitespace, at, i);
                    return true;
                }
                case LexStart::Name:
                    if (options.unicodeRanges && (b | 0x20) == 'u')
                        return false;
                    return name(token, at);
                case LexStart::Digit:
                    return numeric(token, at);
                case LexStart::Minus: {
                    const unsigned char c1 = byteAt(at + 1);
                    if (digit(c1) || (c1 == '.' && digit(byteAt(at + 2))))
                        return numeric(token, at);
                    // (startsIdent; "-->" is a CDC token)
                    if (nameStart(c1) || (c1 == '-' && byteAt(at + 2) != '>'))
                        return name(token, at);
                    return false;
                }
                case LexStart::Plus: {
                    const unsigned char c1 = byteAt(at + 1);
                    if (digit(c1) || (c1 == '.' && digit(byteAt(at + 2))))
                        return numeric(token, at);
                    return delim(token, at);
                }
                case LexStart::Dot:
                    if (digit(byteAt(at + 1)))
                        return numeric(token, at);
                    return delim(token, at);
                case LexStart::Slash:
                    if (byteAt(at + 1) == '*')
                        return false;
                    return delim(token, at);
                case LexStart::Delim:
                    return delim(token, at);
                case LexStart::Hash: {
                    bool plain = false;
                    const size_t end = nameRun(at + 1, plain);
                    if (end == at + 1 || !plain)
                        return false;
                    const unsigned char c1 = (unsigned char)source[at + 1];
                    // an "id" hash: a name follows (startsIdent)
                    const bool id = nameStart(c1) || (c1 == '-' && end > at + 2 && (nameStart((unsigned char)source[at + 2]) || source[at + 2] == '-'));
                    set(token, TokenType::Hash, at, end);
                    token.value = Text::view(slice(at + 1, end));
                    token.hashIsId = id;
                    return true;
                }
                case LexStart::At: {
                    if (!nameStart(byteAt(at + 1)))
                        return false;
                    bool plain = false;
                    const size_t end = nameRun(at + 1, plain);
                    if (!plain)
                        return false;
                    set(token, TokenType::AtKeyword, at, end);
                    token.value = Text::view(slice(at + 1, end));
                    return true;
                }
                case LexStart::Quote: {
                    size_t i = at + 1;
                    const size_t size = source.size();
                    while (i < size){
                        const unsigned char c = (unsigned char)source[i];
                        if (c == b || c == '\\' || c == '\n' || c == '\r' || c == '\f' || c == 0 || c >= 0x80)
                            break;
                        i++;
                    }
                    if (i == size || (unsigned char)source[i] != b)
                        return false;
                    set(token, TokenType::String, at, i + 1);
                    token.value = Text::view(slice(at + 1, i));
                    return true;
                }
                case LexStart::Slow:
                    return false;
            }
            return false;
        }

        // an ident or a function from `at`
        bool name(Token& token, size_t at){
            bool plain = false;
            const size_t end = nameRun(at + 1, plain);
            if (!plain)
                return false;
            if (byteAt(end) == '('){
                // (url( is read by the general reading)
                if (end - at == 3 && (source[at] | 0x20) == 'u' && (source[at + 1] | 0x20) == 'r' && (source[at + 2] | 0x20) == 'l')
                    return false;
                set(token, TokenType::Function, at, end + 1);
            }else{
                set(token, TokenType::Ident, at, end);
            }
            token.value = Text::view(slice(at, end));
            return true;
        }

        bool delim(Token& token, size_t at){
            set(token, TokenType::Delim, at, at + 1);
            token.value = Text::view(slice(at, at + 1));
            return true;
        }

        // a number, percentage or dimension of ASCII characters (its first character starts one)
        bool numeric(Token& token, size_t at){
            const char* const s = source.data();
            const size_t size = source.size();
            size_t i = at;
            double sign = 1;
            if (s[i] == '+' || s[i] == '-'){
                if (s[i] == '-')
                    sign = -1;
                i++;
            }
            double integerPart = 0;
            while (i < size && digit((unsigned char)s[i]))
                integerPart = integerPart * 10 + (s[i++] - '0');
            bool integer = true;
            if (i + 1 < size && s[i] == '.' && digit((unsigned char)s[i + 1])){
                i++;
                while (i < size && digit((unsigned char)s[i]))
                    i++;
                integer = false;
            }
            if (i < size && (s[i] == 'e' || s[i] == 'E')){
                const unsigned char e1 = byteAt(i + 1);
                if (digit(e1) || ((e1 == '+' || e1 == '-') && digit(byteAt(i + 2)))){
                    i += digit(e1) ? 1 : 2;
                    while (i < size && digit((unsigned char)s[i]))
                        i++;
                    integer = false;
                }
            }
            const unsigned char after = byteAt(i);
            TokenType type = TokenType::Number;
            size_t end = i;
            if (after == '%'){
                type = TokenType::Percentage;
                end = i + 1;
            }else if (nameStart(after)){
                bool plain = false;
                end = nameRun(i, plain);
                if (!plain)
                    return false;
                type = TokenType::Dimension;
            }else if (after == '-' || after == '\\' || after >= 0x80 || (after == 0 && i < size)){
                // (what may start a unit: the general reading decides)
                return false;
            }
            // (an integer's value is what convertNumber computes from its digits)
            const double value = integer ? sign * integerPart : convertNumber(slice(at, i));
            set(token, type, at, end);
            if (type == TokenType::Dimension)
                token.unit = Text::view(slice(i, end));
            token.number = value;
            token.isInteger = integer;
            return true;
        }


        void advanced(){
            if (pos > highWater)
                highWater = pos;
        }
        // every other token (CssTokenizer.cpp)
        void slow(Token& token);


        std::string_view source;
        uint32_t base;
        TokenizeOptions options;
        bool comments;
        size_t pos = 0;
        size_t highWater = 0;      // the end of what has been read (errors before it are recorded)
        std::string decoded;       // a token's decoded value or unit, before it is copied to the arena
    };

}

#endif //COMPAT_CSS_CSSLEXER_H
