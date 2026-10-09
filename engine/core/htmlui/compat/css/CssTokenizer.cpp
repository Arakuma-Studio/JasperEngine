// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CssTokenizer.h"
#include "htmlui/compat/css/CssLexer.h"
#include "htmlui/compat/LiteralCompare.h"

#include <cmath>

using namespace jasperengine::htmlui::compat;

namespace {

    constexpr uint32_t kReplacement = 0xFFFD;
    constexpr uint32_t kEof = 0xFFFFFFFF;

    bool isHexDigit(uint32_t c){
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    uint32_t hexValue(uint32_t c){
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    }

    bool isDigit(uint32_t c){
        return c >= '0' && c <= '9';
    }

    bool isNewline(uint32_t c){
        return c == '\n';   // \r, \r\n and \f are read as \n
    }

    bool isWhitespace(uint32_t c){
        return c == '\n' || c == '\t' || c == ' ';
    }

    bool isNonPrintable(uint32_t c){
        return c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1F) || c == 0x7F;
    }

    class Reader {
    public:
        Reader(std::string_view text, uint32_t base): text(text), base(base) {}

        // the code point at byte position `at`, its byte length in `length`
        uint32_t decode(size_t at, size_t& length) const{
            if (at >= text.size()){
                length = 0;
                return kEof;
            }
            const unsigned char c = (unsigned char)text[at];
            if (c < 0x80){
                length = 1;
                if (c == '\r'){
                    if (at + 1 < text.size() && text[at + 1] == '\n')
                        length = 2;
                    return '\n';
                }
                if (c == '\f')
                    return '\n';
                if (c == 0)
                    return kReplacement;
                return c;
            }

            int count;
            uint32_t value;
            uint32_t minimum;
            if ((c & 0xE0) == 0xC0){ count = 1; value = c & 0x1F; minimum = 0x80; }
            else if ((c & 0xF0) == 0xE0){ count = 2; value = c & 0x0F; minimum = 0x800; }
            else if ((c & 0xF8) == 0xF0){ count = 3; value = c & 0x07; minimum = 0x10000; }
            else { length = 1; return kReplacement; }

            for (int i = 1; i <= count; i++){
                if (at + (size_t)i >= text.size()){
                    length = 1;
                    return kReplacement;
                }
                const unsigned char next = (unsigned char)text[at + (size_t)i];
                if ((next & 0xC0) != 0x80){
                    length = 1;
                    return kReplacement;
                }
                value = (value << 6) | (next & 0x3F);
            }
            length = (size_t)count + 1;
            if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
                return kReplacement;
            return value;
        }

        // the byte at the position (0 at the end): fast paths over plain ASCII
        unsigned char byte(size_t ahead = 0) const{
            return pos + ahead < text.size() ? (unsigned char)text[pos + ahead] : 0;
        }
        // a byte decode() returns as is (ASCII but CR, FF and NUL)
        static bool plain(unsigned char c){
            return c < 0x80 && c != '\r' && c != '\f' && c != 0;
        }

        uint32_t peek() const{
            if (pos < text.size()){
                const unsigned char c = (unsigned char)text[pos];
                if (plain(c))
                    return c;
            }
            size_t length = 0;
            return decode(pos, length);
        }

        uint32_t peek(size_t ahead) const{
            if (pos + ahead < text.size()){
                bool allPlain = true;
                for (size_t i = 0; i <= ahead && allPlain; i++)
                    allPlain = plain((unsigned char)text[pos + i]);
                if (allPlain)
                    return (unsigned char)text[pos + ahead];
            }
            size_t at = pos;
            size_t length = 0;
            uint32_t c = decode(at, length);
            for (size_t i = 0; i < ahead; i++){
                if (c == kEof)
                    return kEof;
                at += length;
                c = decode(at, length);
            }
            return c;
        }

        uint32_t next(){
            if (pos < text.size()){
                const unsigned char c = (unsigned char)text[pos];
                if (plain(c)){
                    pos++;
                    return c;
                }
            }
            size_t length = 0;
            const uint32_t c = decode(pos, length);
            pos += length;
            return c;
        }

        bool atEnd() const { return pos >= text.size(); }
        size_t position() const { return pos; }
        uint32_t offset() const { return base + (uint32_t)pos; }
        void reset(size_t to) { pos = to; }
        void skip(size_t bytes) { pos += bytes; }
        std::string_view slice(size_t from, size_t to) const { return text.substr(from, to - from); }
        std::string_view rest() const { return text.substr(pos); }

    private:
        std::string_view text;
        uint32_t base;
        size_t pos = 0;
    };

    // a backslash before EOF is valid too: the escape reads as U+FFFD
    bool validEscape(uint32_t first, uint32_t second){
        return first == '\\' && second != '\n';
    }

    bool startsIdent(uint32_t a, uint32_t b, uint32_t c){
        if (a == '-')
            return isIdentStartCodePoint(b) || b == '-' || validEscape(b, c);
        if (isIdentStartCodePoint(a))
            return true;
        if (a == '\\')
            return validEscape(a, b);
        return false;
    }

    bool startsNumber(uint32_t a, uint32_t b, uint32_t c){
        if (a == '+' || a == '-'){
            if (isDigit(b))
                return true;
            return b == '.' && isDigit(c);
        }
        if (a == '.')
            return isDigit(b);
        return isDigit(a);
    }

    class Tokenizer {
    public:
        Tokenizer(std::string_view css, uint32_t base, const TokenizeOptions& options, std::vector<TokenizeError>* errors, LexResult& out,
                  bool comments):
            in(css, base), source(css), base(base), options(options), errors(errors), out(out), comments(comments) {}

        void run(){
            out.source = source;
            out.text.clear();
            std::vector<LexToken>& tokens = out.tokens;
            tokens.clear();
            out.bad.clear();
            // about one token per 2 bytes in style sheets (fewer with comments); LexTokens are small
            tokens.reserve(source.size() / 2 + 8);
            // the blocks and functions still open (LexToken::closer is set as each one closes)
            std::vector<uint32_t> open;
            while (true){
                const uint32_t index = (uint32_t)tokens.size();
                LexToken& token = tokens.emplace_back();
                current = &token;
                // one-character tokens first (consumeToken reads them the same way)
                const size_t at = in.position();
                if (at < source.size()){
                    TokenType punctuation = TokenType::EndOfFile;
                    TokenType closes = TokenType::EndOfFile;   // the opening token it mirrors
                    bool opens = false;
                    switch (source[at]){
                        case '(': punctuation = TokenType::LeftParen; opens = true; break;
                        case ')': punctuation = TokenType::RightParen; closes = TokenType::LeftParen; break;
                        case '[': punctuation = TokenType::LeftBracket; opens = true; break;
                        case ']': punctuation = TokenType::RightBracket; closes = TokenType::LeftBracket; break;
                        case '{': punctuation = TokenType::LeftBrace; opens = true; break;
                        case '}': punctuation = TokenType::RightBrace; closes = TokenType::LeftBrace; break;
                        case ',': punctuation = TokenType::Comma; break;
                        case ':': punctuation = TokenType::Colon; break;
                        case ';': punctuation = TokenType::Semicolon; break;
                        default: break;
                    }
                    if (punctuation != TokenType::EndOfFile){
                        token.type = punctuation;
                        token.offset = base + (uint32_t)at;
                        token.length = 1;
                        in.skip(1);
                        if (opens){
                            open.push_back(index);
                        }else if (closes != TokenType::EndOfFile && !open.empty()){
                            LexToken& opening = tokens[open.back()];
                            if ((opening.type == TokenType::Function ? TokenType::LeftParen : opening.type) == closes){
                                opening.closer = index;
                                open.pop_back();
                            }
                        }
                        continue;
                    }
                    // white space and plain ASCII names (consumeToken reads them the same way)
                    const unsigned char b = (unsigned char)source[at];
                    if (b == ' ' || b == '\n' || b == '\t'){
                        size_t i = at + 1;
                        while (i < source.size()){
                            const char w = source[i];
                            if (w != ' ' && w != '\n' && w != '\t' && w != '\r' && w != '\f')
                                break;
                            i++;
                        }
                        in.skip(i - at);
                        token.type = TokenType::Whitespace;
                        token.offset = base + (uint32_t)at;
                        token.length = (uint32_t)(i - at);
                        continue;
                    }
                    const unsigned char lower = (unsigned char)(b | 0x20);
                    if (((lower >= 'a' && lower <= 'z') && !(options.unicodeRanges && lower == 'u')) || b == '_'){
                        const size_t run = identRun();
                        const size_t after = at + run;
                        const unsigned char next = after < source.size() ? (unsigned char)source[after] : 0;
                        const bool ended = after == source.size() || (next < 0x80 && next != 0 && next != '\\');
                        const bool url = run == 3 && lower == 'u' && (source[at + 1] | 0x20) == 'r' && (source[at + 2] | 0x20) == 'l';
                        if (ended && !(url && next == '(')){
                            token.value = LexText{(uint32_t)at, (uint32_t)run, false};
                            token.offset = base + (uint32_t)at;
                            if (next == '('){
                                in.skip(run + 1);
                                token.type = TokenType::Function;
                                token.length = (uint32_t)run + 1;
                                open.push_back(index);
                            }else{
                                in.skip(run);
                                token.type = TokenType::Ident;
                                token.length = (uint32_t)run;
                            }
                            continue;
                        }
                    }
                }
                consumeToken();
                // (a closing token is one character: the branch above read it)
                switch (token.type){
                    case TokenType::Function: open.push_back(index); break;
                    case TokenType::BadString:
                    case TokenType::BadUrl: out.bad.push_back(index); break;
                    default: break;
                }
                if (token.type == TokenType::EndOfFile)
                    break;
                if (!comments && token.type == TokenType::Comment)
                    tokens.pop_back();
            }
            const uint32_t end = (uint32_t)tokens.size() - 1;
            for (uint32_t index : open)
                tokens[index].closer = end;
        }

    private:
        void error(TokenizeError::Kind kind){
            if (errors)
                errors->push_back(TokenizeError{kind, in.offset()});
        }

        // ---- values as source runs (or decoded text once something had to be decoded)

        void toDecoded(LexText& t){
            const size_t begin = out.text.size();
            out.text.append(source.data() + t.begin, t.length);
            t.begin = (uint32_t)begin;
            t.decoded = true;
        }

        void appendSource(LexText& t, size_t from, size_t count){
            if (count == 0)
                return;
            if (!t.decoded){
                if (t.length == 0){
                    t.begin = (uint32_t)from;
                    t.length = (uint32_t)count;
                    return;
                }
                if (t.begin + t.length == from){
                    t.length += (uint32_t)count;
                    return;
                }
                toDecoded(t);
            }
            out.text.append(source.data() + from, count);
            t.length += (uint32_t)count;
        }

        void appendCodePoint(LexText& t, uint32_t c){
            if (!t.decoded)
                toDecoded(t);
            const size_t before = out.text.size();
            appendUtf8(out.text, c);
            t.length += (uint32_t)(out.text.size() - before);
        }

        // c was read by in.next() from `at`: the source byte itself when it is one plain ASCII byte
        void appendRead(LexText& t, uint32_t c, size_t at){
            if (c < 0x80 && in.position() == at + 1 && (unsigned char)source[at] == c)
                appendSource(t, at, 1);
            else
                appendCodePoint(t, c);
        }

        std::string_view view(const LexText& t) const {
            return std::string_view((t.decoded ? out.text.data() : source.data()) + t.begin, t.length);
        }

        // the token being read (an element of the output, filled in place)
        LexToken& make(TokenType type, size_t start){
            LexToken& token = *current;
            token.type = type;
            token.offset = base + (uint32_t)start;
            token.length = (uint32_t)(in.position() - start);
            return token;
        }

        // after the backslash
        uint32_t consumeEscape(){
            const uint32_t c = in.next();
            if (c == kEof){
                error(TokenizeError::Kind::EofInEscape);
                return kReplacement;
            }
            if (isHexDigit(c)){
                uint32_t value = hexValue(c);
                int digits = 1;
                while (digits < 6 && isHexDigit(in.peek())){
                    value = value * 16 + hexValue(in.next());
                    digits++;
                }
                if (isWhitespace(in.peek()))
                    in.next();
                if (value == 0 || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF)
                    return kReplacement;
                return value;
            }
            return c;
        }

        // ASCII name characters: a-z A-Z 0-9 - _
        static bool asciiIdentByte(unsigned char c){
            return (unsigned char)((c | 0x20) - 'a') < 26 || (unsigned char)(c - '0') < 10 || c == '-' || c == '_';
        }

        // the length of the run of ASCII name characters at the position
        size_t identRun() const {
            const char* const begin = source.data() + in.position();
            const char* const end = source.data() + source.size();
            const char* p = begin;
            while (p < end && asciiIdentByte((unsigned char)*p))
                p++;
            return (size_t)(p - begin);
        }

        // appends the sequence to result
        void consumeIdentSequence(LexText& result){
            while (true){
                // a run of ASCII name bytes is a run of the source
                const size_t run = identRun();
                if (run > 0){
                    appendSource(result, in.position(), run);
                    in.skip(run);
                }
                // what ends a name in practice: an ASCII byte that is neither a name character nor '\\'
                const unsigned char b = in.byte();
                if (b < 0x80 && b != 0 && b != '\\')
                    return;
                const uint32_t c = in.peek();
                if (isIdentCodePoint(c)){
                    appendCodePoint(result, in.next());
                }else if (c == '\\' && validEscape(c, in.peek(1))){
                    in.next();
                    appendCodePoint(result, consumeEscape());
                }else{
                    return;
                }
            }
        }

        // the number's characters; value and integer flag (the characters are ASCII: the
        // source bytes themselves)
        void consumeNumber(double& value, bool& integer){
            integer = true;
            const size_t from = in.position();
            if (in.byte() == '+' || in.byte() == '-')
                in.skip(1);
            while (isDigit(in.byte()))
                in.skip(1);
            if (in.byte() == '.' && isDigit(in.byte(1))){
                in.skip(1);
                while (isDigit(in.byte()))
                    in.skip(1);
                integer = false;
            }
            const unsigned char e = in.byte();
            if (e == 'e' || e == 'E'){
                const unsigned char afterE = in.byte(1);
                if (isDigit(afterE) || ((afterE == '+' || afterE == '-') && isDigit(in.byte(2)))){
                    in.skip(1);
                    if (!isDigit(in.byte()))
                        in.skip(1);
                    while (isDigit(in.byte()))
                        in.skip(1);
                    integer = false;
                }
            }
            value = convertNumber(in.slice(from, in.position()));
        }

        // §4.3.13 "convert a string to a number"
        static double convertNumber(std::string_view repr){
            size_t i = 0;
            double sign = 1;
            if (i < repr.size() && (repr[i] == '+' || repr[i] == '-')){
                if (repr[i] == '-')
                    sign = -1;
                i++;
            }
            double integerPart = 0;
            while (i < repr.size() && isDigit((unsigned char)repr[i]))
                integerPart = integerPart * 10 + (repr[i++] - '0');
            double fraction = 0;
            int fractionDigits = 0;
            if (i < repr.size() && repr[i] == '.'){
                i++;
                while (i < repr.size() && isDigit((unsigned char)repr[i])){
                    fraction = fraction * 10 + (repr[i++] - '0');
                    fractionDigits++;
                }
            }
            double exponentSign = 1;
            double exponent = 0;
            if (i < repr.size() && (repr[i] == 'e' || repr[i] == 'E')){
                i++;
                if (i < repr.size() && (repr[i] == '+' || repr[i] == '-')){
                    if (repr[i] == '-')
                        exponentSign = -1;
                    i++;
                }
                while (i < repr.size() && isDigit((unsigned char)repr[i]))
                    exponent = exponent * 10 + (repr[i++] - '0');
            }
            // powers of ten up to 1e22 are exact doubles: pow() would return the same values
            static constexpr double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12,
                                                1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
            double value = integerPart;
            if (fractionDigits > 0)
                value += fraction / (fractionDigits <= 22 ? kPow10[fractionDigits] : std::pow(10.0, fractionDigits));
            if (exponent == 0)
                return sign * value;   // * pow(10, 0) == * 1
            return sign * value * std::pow(10.0, exponentSign * exponent);
        }

        void consumeNumeric(size_t start){
            double value = 0;
            bool integer = true;
            consumeNumber(value, integer);
            {
                // what usually follows: a unit of ASCII letters, '%', or a byte that starts no name
                const unsigned char b = in.byte();
                const unsigned char lower = (unsigned char)(b | 0x20);
                const bool plainEnd = b == ' ' || b == ';' || b == ',' || b == ')' || b == '}' || b == '/' || b == '\n' ||
                                      b == '\t' || (b == 0 && in.atEnd());
                if (plainEnd){
                    LexToken& token = make(TokenType::Number, start);
                    token.number = value;
                    token.isInteger = integer;
                    return;
                }
                if (lower >= 'a' && lower <= 'z'){
                    // startsIdent: a letter always starts a name
                    consumeIdentSequence(current->unit);
                    LexToken& token = make(TokenType::Dimension, start);
                    token.number = value;
                    token.isInteger = integer;
                    return;
                }
            }
            if (startsIdent(in.peek(), in.peek(1), in.peek(2))){
                consumeIdentSequence(current->unit);
                LexToken& token = make(TokenType::Dimension, start);
                token.number = value;
                token.isInteger = integer;
                return;
            }
            if (in.peek() == '%'){
                in.next();
                LexToken& token = make(TokenType::Percentage, start);
                token.number = value;
                token.isInteger = integer;
                return;
            }
            LexToken& token = make(TokenType::Number, start);
            token.number = value;
            token.isInteger = integer;
            return;
        }

        void consumeBadUrlRemnants(){
            while (true){
                const uint32_t c = in.next();
                if (c == ')' || c == kEof)
                    return;
                if (validEscape(c, in.peek()))
                    consumeEscape();
            }
        }

        // after "url(" and its whitespace
        void consumeUrl(size_t start){
            LexText value;
            while (isWhitespace(in.peek()))
                in.next();
            while (true){
                const size_t at = in.position();
                const uint32_t c = in.next();
                if (c == ')'){
                    LexToken& token = make(TokenType::Url, start);
                    token.value = value;
                    return;
                }
                if (c == kEof){
                    error(TokenizeError::Kind::EofInUrl);
                    LexToken& token = make(TokenType::Url, start);
                    token.value = value;
                    return;
                }
                if (isWhitespace(c)){
                    while (isWhitespace(in.peek()))
                        in.next();
                    if (in.peek() == ')'){
                        in.next();
                        LexToken& token = make(TokenType::Url, start);
                        token.value = value;
                        return;
                    }
                    if (in.peek() == kEof){
                        in.next();
                        error(TokenizeError::Kind::EofInUrl);
                        LexToken& token = make(TokenType::Url, start);
                        token.value = value;
                        return;
                    }
                    consumeBadUrlRemnants();
                    { make(TokenType::BadUrl, start); return; }
                }
                if (c == '"' || c == '\'' || c == '(' || isNonPrintable(c)){
                    consumeBadUrlRemnants();
                    { make(TokenType::BadUrl, start); return; }
                }
                if (c == '\\'){
                    if (validEscape(c, in.peek())){
                        appendCodePoint(value, consumeEscape());
                    }else{
                        error(TokenizeError::Kind::InvalidEscape);
                        consumeBadUrlRemnants();
                        { make(TokenType::BadUrl, start); return; }
                    }
                    continue;
                }
                appendRead(value, c, at);
            }
        }

        void consumeIdentLike(size_t start){
            // the name is read into the token's value (the token being built)
            consumeIdentSequence(current->value);
            const std::string_view name = view(current->value);
            const bool isUrl = name.size() == 3 && (name[0] | 0x20) == 'u' && (name[1] | 0x20) == 'r' && (name[2] | 0x20) == 'l';
            if (isUrl && in.peek() == '('){
                in.next();
                // "url(" followed by whitespace and a quote is a function, the string an argument
                while (isWhitespace(in.peek()) && isWhitespace(in.peek(1)))
                    in.next();
                const uint32_t a = in.peek();
                const uint32_t b = in.peek(1);
                if (a == '"' || a == '\'' || (isWhitespace(a) && (b == '"' || b == '\''))){
                    make(TokenType::Function, start);
                    return;
                }
                current->value = LexText();
                consumeUrl(start);
                return;
            }
            if (in.peek() == '('){
                in.next();
                make(TokenType::Function, start);
                return;
            }
            make(TokenType::Ident, start);
        }

        void consumeString(uint32_t quote, size_t start){
            LexText value;
            while (true){
                // a run of plain ASCII characters is copied at once
                size_t run = 0;
                while (true){
                    const unsigned char b = in.byte(run);
                    if (b == 0 || b >= 0x80 || b == quote || b == '\\' || b == '\n' || b == '\r' || b == '\f')
                        break;
                    run++;
                }
                if (run > 0){
                    appendSource(value, in.position(), run);
                    in.skip(run);
                }
                const uint32_t c = in.peek();
                if (c == quote){
                    in.next();
                    break;
                }
                if (c == kEof){
                    error(TokenizeError::Kind::EofInString);
                    break;
                }
                if (isNewline(c)){
                    // not consumed: it starts the next token
                    { make(TokenType::BadString, start); return; }
                }
                const size_t at = in.position();
                in.next();
                if (c == '\\'){
                    const uint32_t after = in.peek();
                    if (after == kEof)
                        continue;
                    if (isNewline(after)){
                        in.next();
                        continue;
                    }
                    appendCodePoint(value, consumeEscape());
                    continue;
                }
                appendRead(value, c, at);
            }
            LexToken& token = make(TokenType::String, start);
            token.value = value;
            return;
        }

        void consumeUnicodeRange(size_t start){
            // "U+" already consumed
            std::string digits;
            int questionMarks = 0;
            while (digits.size() < 6 && isHexDigit(in.peek()))
                digits += (char)in.next();
            while (digits.size() + (size_t)questionMarks < 6 && in.peek() == '?'){
                in.next();
                questionMarks++;
            }
            uint32_t first = 0, last = 0;
            if (questionMarks > 0){
                for (char c : digits){
                    first = first * 16 + hexValue((uint32_t)c);
                    last = last * 16 + hexValue((uint32_t)c);
                }
                for (int i = 0; i < questionMarks; i++){
                    first = first * 16;
                    last = last * 16 + 15;
                }
            }else{
                for (char c : digits)
                    first = first * 16 + hexValue((uint32_t)c);
                last = first;
                if (in.peek() == '-' && isHexDigit(in.peek(1))){
                    in.next();
                    last = 0;
                    int count = 0;
                    while (count < 6 && isHexDigit(in.peek())){
                        last = last * 16 + hexValue(in.next());
                        count++;
                    }
                }
            }
            LexToken& token = make(TokenType::UnicodeRange, start);
            token.range[0] = first;
            token.range[1] = last;
            return;
        }

        void consumeToken(){
            const size_t start = in.position();
            {
                // the common starts first: a name (letters, '_') or a number (digits); the
                // general dispatch below decides them the same way
                const unsigned char b = in.byte();
                const unsigned char lower = (unsigned char)(b | 0x20);
                if (((lower >= 'a' && lower <= 'z') && !(options.unicodeRanges && lower == 'u')) || b == '_'){
                    // a name of ASCII name characters ended by a plain ASCII byte: an ident, or a
                    // function when '(' follows (but url(, which consumeIdentLike reads)
                    const size_t run = identRun();
                    const size_t after = start + run;
                    const unsigned char next = after < source.size() ? (unsigned char)source[after] : 0;
                    const bool ended = after == source.size() || (next < 0x80 && next != 0 && next != '\\');
                    const bool url = run == 3 && (b | 0x20) == 'u' && (source[start + 1] | 0x20) == 'r' && (source[start + 2] | 0x20) == 'l';
                    if (ended && !(url && next == '(')){
                        current->value = LexText{(uint32_t)start, (uint32_t)run, false};
                        in.skip(run);
                        if (next == '('){
                            in.skip(1);
                            make(TokenType::Function, start);
                        }else{
                            make(TokenType::Ident, start);
                        }
                        return;
                    }
                    consumeIdentLike(start);
                    return;
                }
                if (b == ' ' || b == '\n' || b == '\t'){
                    size_t i = start + 1;
                    while (i < source.size()){
                        const char w = source[i];
                        if (w != ' ' && w != '\n' && w != '\t' && w != '\r' && w != '\f')
                            break;
                        i++;
                    }
                    in.skip(i - start);
                    make(TokenType::Whitespace, start);
                    return;
                }
                if (b >= '0' && b <= '9'){
                    consumeNumeric(start);
                    return;
                }
            }
            const uint32_t c = in.peek();

            if (c == kEof)
                { make(TokenType::EndOfFile, start); return; }

            if (c == '/' && in.byte(1) == '*'){
                in.skip(2);
                const std::string_view rest = in.rest();
                const size_t close = rest.find("*/");
                if (close == std::string_view::npos){
                    in.skip(rest.size());
                    error(TokenizeError::Kind::EofInComment);
                }else{
                    in.skip(close + 2);
                }
                { make(TokenType::Comment, start); return; }
            }

            if (isWhitespace(c)){
                while (true){
                    const unsigned char b = in.byte();
                    if (b != ' ' && b != '\t' && b != '\n' && b != '\r' && b != '\f')
                        break;
                    in.skip(1);
                }
                { make(TokenType::Whitespace, start); return; }
            }

            if (c == '"' || c == '\''){
                in.next();
                { consumeString(c, start); return; }
            }

            if (c == '#'){
                if (isIdentCodePoint(in.peek(1)) || validEscape(in.peek(1), in.peek(2))){
                    in.next();
                    const bool id = startsIdent(in.peek(), in.peek(1), in.peek(2));
                    consumeIdentSequence(current->value);
                    LexToken& token = make(TokenType::Hash, start);
                    token.hashIsId = id;
                    return;
                }
                { delim(start); return; }
            }

            switch (c){
                case '(': in.next(); { make(TokenType::LeftParen, start); return; }
                case ')': in.next(); { make(TokenType::RightParen, start); return; }
                case '[': in.next(); { make(TokenType::LeftBracket, start); return; }
                case ']': in.next(); { make(TokenType::RightBracket, start); return; }
                case '{': in.next(); { make(TokenType::LeftBrace, start); return; }
                case '}': in.next(); { make(TokenType::RightBrace, start); return; }
                case ',': in.next(); { make(TokenType::Comma, start); return; }
                case ':': in.next(); { make(TokenType::Colon, start); return; }
                case ';': in.next(); { make(TokenType::Semicolon, start); return; }
                default: break;
            }

            if (c == '+' || c == '.'){
                if (startsNumber(c, in.peek(1), in.peek(2)))
                    { consumeNumeric(start); return; }
                { delim(start); return; }
            }

            if (c == '-'){
                if (startsNumber(c, in.peek(1), in.peek(2)))
                    { consumeNumeric(start); return; }
                if (in.peek(1) == '-' && in.peek(2) == '>'){
                    in.next();
                    in.next();
                    in.next();
                    { make(TokenType::CDC, start); return; }
                }
                if (startsIdent(c, in.peek(1), in.peek(2)))
                    { consumeIdentLike(start); return; }
                { delim(start); return; }
            }

            if (c == '<'){
                if (in.peek(1) == '!' && in.peek(2) == '-' && in.peek(3) == '-'){
                    in.next();
                    in.next();
                    in.next();
                    in.next();
                    { make(TokenType::CDO, start); return; }
                }
                { delim(start); return; }
            }

            if (c == '@'){
                if (startsIdent(in.peek(1), in.peek(2), in.peek(3))){
                    in.next();
                    consumeIdentSequence(current->value);
                    LexToken& token = make(TokenType::AtKeyword, start);
                    return;
                }
                { delim(start); return; }
            }

            if (c == '\\'){
                if (validEscape(c, in.peek(1)))
                    { consumeIdentLike(start); return; }
                error(TokenizeError::Kind::InvalidEscape);
                { delim(start); return; }
            }

            if (isDigit(c))
                { consumeNumeric(start); return; }

            if (options.unicodeRanges && (c == 'u' || c == 'U') && in.peek(1) == '+' &&
                (isHexDigit(in.peek(2)) || in.peek(2) == '?')){
                in.next();
                in.next();
                { consumeUnicodeRange(start); return; }
            }

            if (isIdentStartCodePoint(c))
                { consumeIdentLike(start); return; }

            { delim(start); return; }
        }

        void delim(size_t start){
            const uint32_t c = in.next();
            LexToken& token = make(TokenType::Delim, start);
            appendRead(token.value, c, start);
            return;
        }

        Reader in;
        std::string_view source;
        uint32_t base;
        TokenizeOptions options;
        std::vector<TokenizeError>* errors;
        LexResult& out;
        bool comments;
        LexToken* current = nullptr;
    };

}

bool Token::isIdent(const char* lowercase) const{
    if (type != TokenType::Ident)
        return false;
    size_t i = 0;
    for (; lowercase[i] != 0; i++){
        if (i >= value.size())
            return false;
        char c = value[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != lowercase[i])
            return false;
    }
    return i == value.size();
}

bool ComponentValue::isFunction(const char* lowercaseName) const{
    if (kind != Kind::Function)
        return false;
    // (Token::isIdent's comparison, on the function's name)
    const Text& value = token.value;
    size_t i = 0;
    for (; lowercaseName[i] != 0; i++){
        if (i >= value.size())
            return false;
        char c = value[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != lowercaseName[i])
            return false;
    }
    return i == value.size();
}

bool jasperengine::htmlui::compat::isIdentStartCodePoint(uint32_t c){
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 0x80 && c != kEof);
}

bool jasperengine::htmlui::compat::isIdentCodePoint(uint32_t c){
    return isIdentStartCodePoint(c) || isDigit(c) || c == '-';
}

void jasperengine::htmlui::compat::appendUtf8(std::string& out, uint32_t c){
    if (c < 0x80){
        out += (char)c;
    }else if (c < 0x800){
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    }else if (c < 0x10000){
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }else{
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

const char* jasperengine::htmlui::compat::tokenTypeName(TokenType type){
    switch (type){
        case TokenType::Ident: return "ident";
        case TokenType::Function: return "function";
        case TokenType::AtKeyword: return "at-keyword";
        case TokenType::Hash: return "hash";
        case TokenType::String: return "string";
        case TokenType::BadString: return "bad-string";
        case TokenType::Url: return "url";
        case TokenType::BadUrl: return "bad-url";
        case TokenType::Delim: return "delim";
        case TokenType::Number: return "number";
        case TokenType::Percentage: return "percentage";
        case TokenType::Dimension: return "dimension";
        case TokenType::UnicodeRange: return "unicode-range";
        case TokenType::Whitespace: return "whitespace";
        case TokenType::CDO: return "CDO";
        case TokenType::CDC: return "CDC";
        case TokenType::Colon: return "colon";
        case TokenType::Semicolon: return "semicolon";
        case TokenType::Comma: return "comma";
        case TokenType::LeftBracket: return "[";
        case TokenType::RightBracket: return "]";
        case TokenType::LeftParen: return "(";
        case TokenType::RightParen: return ")";
        case TokenType::LeftBrace: return "{";
        case TokenType::RightBrace: return "}";
        case TokenType::Comment: return "comment";
        case TokenType::EndOfFile: return "EOF";
    }
    return "?";
}

void jasperengine::htmlui::compat::lexCss(std::string_view css, uint32_t baseOffset, const TokenizeOptions& options,
                                          std::vector<TokenizeError>* errors, LexResult& out, bool comments){
    Tokenizer tokenizer(css, baseOffset, options, errors, out, comments);
    tokenizer.run();
}

std::vector<Token> jasperengine::htmlui::compat::tokenize(std::string_view css, uint32_t baseOffset,
                                                          const TokenizeOptions& options, std::vector<TokenizeError>* errors){
    LexResult lexed;
    lexCss(css, baseOffset, options, errors, lexed);
    // the tokens' texts are views into copies of the source and of the decoded text
    Arena& arena = Arena::current();
    const char* sourceCopy = arena.copy(lexed.source.data(), lexed.source.size());
    const char* textCopy = arena.copy(lexed.text.data(), lexed.text.size());
    std::vector<Token> tokens(lexed.tokens.size());
    for (size_t i = 0; i < tokens.size(); i++)
        lexed.toToken(lexed.tokens[i], tokens[i], sourceCopy, textCopy);
    return tokens;
}
