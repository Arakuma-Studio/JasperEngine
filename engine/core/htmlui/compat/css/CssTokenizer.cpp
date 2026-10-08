// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CssTokenizer.h"

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

        uint32_t peek(size_t ahead = 0) const{
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
            size_t length = 0;
            const uint32_t c = decode(pos, length);
            pos += length;
            return c;
        }

        bool atEnd() const { return pos >= text.size(); }
        size_t position() const { return pos; }
        uint32_t offset() const { return base + (uint32_t)pos; }
        void reset(size_t to) { pos = to; }

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
        Tokenizer(std::string_view css, uint32_t base, const TokenizeOptions& options, std::vector<TokenizeError>* errors):
            in(css, base), source(css), base(base), options(options), errors(errors) {}

        std::vector<Token> run(){
            std::vector<Token> tokens;
            tokens.reserve(source.size() / 3 + 2);
            while (true){
                Token token = consumeToken();
                const bool end = token.type == TokenType::EndOfFile;
                tokens.push_back(std::move(token));
                if (end)
                    break;
            }
            return tokens;
        }

    private:
        void error(TokenizeError::Kind kind){
            if (errors)
                errors->push_back(TokenizeError{kind, in.offset()});
        }

        Token make(TokenType type, size_t start){
            Token token;
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

        std::string consumeIdentSequence(){
            std::string result;
            while (true){
                const uint32_t c = in.peek();
                if (isIdentCodePoint(c)){
                    appendUtf8(result, in.next());
                }else if (validEscape(c, in.peek(1))){
                    in.next();
                    appendUtf8(result, consumeEscape());
                }else{
                    return result;
                }
            }
        }

        // the number's characters; value and integer flag
        void consumeNumber(double& value, bool& integer){
            integer = true;
            std::string repr;
            if (in.peek() == '+' || in.peek() == '-')
                repr += (char)in.next();
            while (isDigit(in.peek()))
                repr += (char)in.next();
            if (in.peek() == '.' && isDigit(in.peek(1))){
                repr += (char)in.next();
                while (isDigit(in.peek()))
                    repr += (char)in.next();
                integer = false;
            }
            const uint32_t e = in.peek();
            if (e == 'e' || e == 'E'){
                const uint32_t afterE = in.peek(1);
                if (isDigit(afterE) || ((afterE == '+' || afterE == '-') && isDigit(in.peek(2)))){
                    repr += (char)in.next();
                    if (!isDigit(in.peek()))
                        repr += (char)in.next();
                    while (isDigit(in.peek()))
                        repr += (char)in.next();
                    integer = false;
                }
            }
            value = convertNumber(repr);
        }

        // §4.3.13 "convert a string to a number"
        static double convertNumber(const std::string& repr){
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
            double value = integerPart;
            if (fractionDigits > 0)
                value += fraction / std::pow(10.0, fractionDigits);
            return sign * value * std::pow(10.0, exponentSign * exponent);
        }

        Token consumeNumeric(size_t start){
            double value = 0;
            bool integer = true;
            consumeNumber(value, integer);
            if (startsIdent(in.peek(), in.peek(1), in.peek(2))){
                std::string unit = consumeIdentSequence();
                Token token = make(TokenType::Dimension, start);
                token.number = value;
                token.isInteger = integer;
                token.unit = std::move(unit);
                return token;
            }
            if (in.peek() == '%'){
                in.next();
                Token token = make(TokenType::Percentage, start);
                token.number = value;
                token.isInteger = integer;
                return token;
            }
            Token token = make(TokenType::Number, start);
            token.number = value;
            token.isInteger = integer;
            return token;
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
        Token consumeUrl(size_t start){
            std::string value;
            while (isWhitespace(in.peek()))
                in.next();
            while (true){
                const uint32_t c = in.next();
                if (c == ')'){
                    Token token = make(TokenType::Url, start);
                    token.value = std::move(value);
                    return token;
                }
                if (c == kEof){
                    error(TokenizeError::Kind::EofInUrl);
                    Token token = make(TokenType::Url, start);
                    token.value = std::move(value);
                    return token;
                }
                if (isWhitespace(c)){
                    while (isWhitespace(in.peek()))
                        in.next();
                    if (in.peek() == ')'){
                        in.next();
                        Token token = make(TokenType::Url, start);
                        token.value = std::move(value);
                        return token;
                    }
                    if (in.peek() == kEof){
                        in.next();
                        error(TokenizeError::Kind::EofInUrl);
                        Token token = make(TokenType::Url, start);
                        token.value = std::move(value);
                        return token;
                    }
                    consumeBadUrlRemnants();
                    return make(TokenType::BadUrl, start);
                }
                if (c == '"' || c == '\'' || c == '(' || isNonPrintable(c)){
                    consumeBadUrlRemnants();
                    return make(TokenType::BadUrl, start);
                }
                if (c == '\\'){
                    if (validEscape(c, in.peek())){
                        appendUtf8(value, consumeEscape());
                    }else{
                        error(TokenizeError::Kind::InvalidEscape);
                        consumeBadUrlRemnants();
                        return make(TokenType::BadUrl, start);
                    }
                    continue;
                }
                appendUtf8(value, c);
            }
        }

        Token consumeIdentLike(size_t start){
            std::string name = consumeIdentSequence();
            const bool isUrl = name.size() == 3 && (name[0] | 0x20) == 'u' && (name[1] | 0x20) == 'r' && (name[2] | 0x20) == 'l';
            if (isUrl && in.peek() == '('){
                in.next();
                // "url(" followed by whitespace and a quote is a function, the string an argument
                while (isWhitespace(in.peek()) && isWhitespace(in.peek(1)))
                    in.next();
                const uint32_t a = in.peek();
                const uint32_t b = in.peek(1);
                if (a == '"' || a == '\'' || (isWhitespace(a) && (b == '"' || b == '\''))){
                    Token token = make(TokenType::Function, start);
                    token.value = std::move(name);
                    return token;
                }
                return consumeUrl(start);
            }
            if (in.peek() == '('){
                in.next();
                Token token = make(TokenType::Function, start);
                token.value = std::move(name);
                return token;
            }
            Token token = make(TokenType::Ident, start);
            token.value = std::move(name);
            return token;
        }

        Token consumeString(uint32_t quote, size_t start){
            std::string value;
            while (true){
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
                    return make(TokenType::BadString, start);
                }
                in.next();
                if (c == '\\'){
                    const uint32_t after = in.peek();
                    if (after == kEof)
                        continue;
                    if (isNewline(after)){
                        in.next();
                        continue;
                    }
                    appendUtf8(value, consumeEscape());
                    continue;
                }
                appendUtf8(value, c);
            }
            Token token = make(TokenType::String, start);
            token.value = std::move(value);
            return token;
        }

        Token consumeUnicodeRange(size_t start){
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
            Token token = make(TokenType::UnicodeRange, start);
            token.rangeStart = first;
            token.rangeEnd = last;
            return token;
        }

        Token consumeToken(){
            const size_t start = in.position();
            const uint32_t c = in.peek();

            if (c == kEof)
                return make(TokenType::EndOfFile, start);

            if (c == '/' && in.peek(1) == '*'){
                in.next();
                in.next();
                while (true){
                    const uint32_t d = in.next();
                    if (d == kEof){
                        error(TokenizeError::Kind::EofInComment);
                        break;
                    }
                    if (d == '*' && in.peek() == '/'){
                        in.next();
                        break;
                    }
                }
                return make(TokenType::Comment, start);
            }

            if (isWhitespace(c)){
                while (isWhitespace(in.peek()))
                    in.next();
                return make(TokenType::Whitespace, start);
            }

            if (c == '"' || c == '\''){
                in.next();
                return consumeString(c, start);
            }

            if (c == '#'){
                if (isIdentCodePoint(in.peek(1)) || validEscape(in.peek(1), in.peek(2))){
                    in.next();
                    const bool id = startsIdent(in.peek(), in.peek(1), in.peek(2));
                    std::string name = consumeIdentSequence();
                    Token token = make(TokenType::Hash, start);
                    token.value = std::move(name);
                    token.hashIsId = id;
                    return token;
                }
                return delim(start);
            }

            switch (c){
                case '(': in.next(); return make(TokenType::LeftParen, start);
                case ')': in.next(); return make(TokenType::RightParen, start);
                case '[': in.next(); return make(TokenType::LeftBracket, start);
                case ']': in.next(); return make(TokenType::RightBracket, start);
                case '{': in.next(); return make(TokenType::LeftBrace, start);
                case '}': in.next(); return make(TokenType::RightBrace, start);
                case ',': in.next(); return make(TokenType::Comma, start);
                case ':': in.next(); return make(TokenType::Colon, start);
                case ';': in.next(); return make(TokenType::Semicolon, start);
                default: break;
            }

            if (c == '+' || c == '.'){
                if (startsNumber(c, in.peek(1), in.peek(2)))
                    return consumeNumeric(start);
                return delim(start);
            }

            if (c == '-'){
                if (startsNumber(c, in.peek(1), in.peek(2)))
                    return consumeNumeric(start);
                if (in.peek(1) == '-' && in.peek(2) == '>'){
                    in.next();
                    in.next();
                    in.next();
                    return make(TokenType::CDC, start);
                }
                if (startsIdent(c, in.peek(1), in.peek(2)))
                    return consumeIdentLike(start);
                return delim(start);
            }

            if (c == '<'){
                if (in.peek(1) == '!' && in.peek(2) == '-' && in.peek(3) == '-'){
                    in.next();
                    in.next();
                    in.next();
                    in.next();
                    return make(TokenType::CDO, start);
                }
                return delim(start);
            }

            if (c == '@'){
                if (startsIdent(in.peek(1), in.peek(2), in.peek(3))){
                    in.next();
                    std::string name = consumeIdentSequence();
                    Token token = make(TokenType::AtKeyword, start);
                    token.value = std::move(name);
                    return token;
                }
                return delim(start);
            }

            if (c == '\\'){
                if (validEscape(c, in.peek(1)))
                    return consumeIdentLike(start);
                error(TokenizeError::Kind::InvalidEscape);
                return delim(start);
            }

            if (isDigit(c))
                return consumeNumeric(start);

            if (options.unicodeRanges && (c == 'u' || c == 'U') && in.peek(1) == '+' &&
                (isHexDigit(in.peek(2)) || in.peek(2) == '?')){
                in.next();
                in.next();
                return consumeUnicodeRange(start);
            }

            if (isIdentStartCodePoint(c))
                return consumeIdentLike(start);

            return delim(start);
        }

        Token delim(size_t start){
            const uint32_t c = in.next();
            Token token = make(TokenType::Delim, start);
            appendUtf8(token.value, c);
            return token;
        }

        Reader in;
        std::string_view source;
        uint32_t base;
        TokenizeOptions options;
        std::vector<TokenizeError>* errors;
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
    Token probe = token;
    probe.type = TokenType::Ident;
    return probe.isIdent(lowercaseName);
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

std::vector<Token> jasperengine::htmlui::compat::tokenize(std::string_view css, uint32_t baseOffset,
                                                          const TokenizeOptions& options, std::vector<TokenizeError>* errors){
    Tokenizer tokenizer(css, baseOffset, options, errors);
    return tokenizer.run();
}
