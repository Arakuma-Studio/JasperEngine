// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssTokenizer.h"

#include <cmath>
#include <cstdint>

using namespace jasperengine::htmlui::compat;

namespace {

    // the token kinds of the CSS Syntax §9 separator table
    enum class Kind { Ident, Function, Url, BadUrl, AtKeyword, Hash, Dimension, Number, Percentage, CDC,
                      OpenParen, Hyphen, NumberSign, At, PeriodOrPlus, Slash, Asterisk, Other };

    Kind firstKind(const ComponentValue& value){
        if (value.kind == ComponentValue::Kind::Function)
            return Kind::Function;
        if (value.kind == ComponentValue::Kind::Block)
            return value.token.type == TokenType::LeftParen ? Kind::OpenParen : Kind::Other;
        const Token& t = value.token;
        switch (t.type){
            case TokenType::Ident: return Kind::Ident;
            case TokenType::Url: return Kind::Url;
            case TokenType::BadUrl: return Kind::BadUrl;
            case TokenType::AtKeyword: return Kind::AtKeyword;
            case TokenType::Hash: return Kind::Hash;
            case TokenType::Dimension: return Kind::Dimension;
            case TokenType::Number: return Kind::Number;
            case TokenType::Percentage: return Kind::Percentage;
            case TokenType::CDC: return Kind::CDC;
            case TokenType::Delim:
                if (t.value == "-") return Kind::Hyphen;
                if (t.value == "#") return Kind::NumberSign;
                if (t.value == "@") return Kind::At;
                if (t.value == "." || t.value == "+") return Kind::PeriodOrPlus;
                if (t.value == "/") return Kind::Slash;
                if (t.value == "*") return Kind::Asterisk;
                return Kind::Other;
            default:
                return Kind::Other;
        }
    }

    Kind lastKind(const ComponentValue& value){
        // a block or function ends with its closing bracket
        if (value.kind != ComponentValue::Kind::Token)
            return Kind::Other;
        return firstKind(value);
    }

    bool needsSeparator(Kind a, Kind b){
        const bool identLike = b == Kind::Ident || b == Kind::Function || b == Kind::Url || b == Kind::BadUrl;
        const bool numeric = b == Kind::Number || b == Kind::Percentage || b == Kind::Dimension;
        switch (a){
            case Kind::Ident:
                return identLike || b == Kind::Hyphen || numeric || b == Kind::CDC || b == Kind::OpenParen;
            case Kind::AtKeyword:
            case Kind::Hash:
            case Kind::Dimension:
                return identLike || b == Kind::Hyphen || numeric || b == Kind::CDC;
            case Kind::NumberSign:
            case Kind::Hyphen:
                return identLike || b == Kind::Hyphen || numeric;
            case Kind::Number:
                return identLike || numeric;
            case Kind::At:
                return identLike || b == Kind::Hyphen;
            case Kind::PeriodOrPlus:
                return numeric;
            case Kind::Slash:
                return b == Kind::Asterisk;
            default:
                return false;
        }
    }

    void appendEscapedCodePoint(std::string& out, uint32_t c){
        static const char* hex = "0123456789abcdef";
        char buffer[8];
        int length = 0;
        do {
            buffer[length++] = hex[c & 0xF];
            c >>= 4;
        } while (c != 0);
        out += '\\';
        while (length > 0)
            out += buffer[--length];
        out += ' ';
    }

    // decodes one UTF-8 code point (the input is valid UTF-8: the tokenizer wrote it)
    uint32_t nextCodePoint(std::string_view text, size_t& i){
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x80){
            i++;
            return c;
        }
        int count = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
        uint32_t value = c & (count == 1 ? 0x1F : count == 2 ? 0x0F : 0x07);
        i++;
        for (int k = 0; k < count && i < text.size(); k++, i++)
            value = (value << 6) | ((unsigned char)text[i] & 0x3F);
        return value;
    }

    void appendBlockOrFunction(std::string& out, const ComponentValue& value, const SerializeOptions& options);

    void appendList(std::string& out, const std::vector<ComponentValue>& values, const SerializeOptions& options){
        bool lastWasWhitespace = false;
        const ComponentValue* previous = nullptr;
        for (const ComponentValue& value : values){
            if (value.isWhitespace()){
                if (!lastWasWhitespace)
                    out += ' ';
                lastWasWhitespace = true;
                previous = nullptr;
                continue;
            }
            if (previous && needsSeparator(lastKind(*previous), firstKind(value)))
                out += options.exactSeparators ? "/**/" : " ";
            lastWasWhitespace = false;
            if (value.kind == ComponentValue::Kind::Token)
                appendToken(out, value.token);
            else
                appendBlockOrFunction(out, value, options);
            previous = &value;
        }
    }

    void appendBlockOrFunction(std::string& out, const ComponentValue& value, const SerializeOptions& options){
        if (value.kind == ComponentValue::Kind::Function){
            appendIdent(out, value.token.value);
            out += '(';
            appendList(out, value.children, options);
            out += ')';
            return;
        }
        char open = '(', close = ')';
        if (value.token.type == TokenType::LeftBrace){ open = '{'; close = '}'; }
        else if (value.token.type == TokenType::LeftBracket){ open = '['; close = ']'; }
        out += open;
        appendList(out, value.children, options);
        out += close;
    }

}

namespace {

    // the decimal digits of v at the end of buffer (returns the first one)
    char* writeDigits(char* end, uint64_t v){
        do {
            *--end = (char)('0' + (int)(v % 10));
            v /= 10;
        } while (v != 0);
        return end;
    }

}

void jasperengine::htmlui::compat::appendNumber(std::string& out, double value, int decimals){
    if (!std::isfinite(value)){
        out += '0';
        return;
    }
    if (decimals < 0)
        decimals = 0;
    if (decimals > 9)
        decimals = 9;

    double scale = 1;
    for (int i = 0; i < decimals; i++)
        scale *= 10;

    const bool negative = value < 0;
    double magnitude = std::fabs(value);
    // beyond the range of exact integer scaling: whole numbers only
    if (magnitude * scale > 9.0e15){
        const double whole = std::floor(magnitude + 0.5);
        std::string digits;
        double rest = whole;
        do {
            const double digit = std::fmod(rest, 10.0);
            digits.insert(digits.begin(), (char)('0' + (int)digit));
            rest = std::floor(rest / 10.0);
        } while (rest >= 1.0);
        if (negative)
            out += '-';
        out += digits;
        return;
    }

    const uint64_t scaled = (uint64_t)std::llround(magnitude * scale);
    if (scaled == 0){
        out += '0';
        return;
    }
    const uint64_t divisor = (uint64_t)scale;
    const uint64_t integer = scaled / divisor;
    uint64_t fraction = scaled % divisor;

    char buffer[48];
    char* const end = buffer + sizeof(buffer);
    char* text = writeDigits(end - 16, integer);   // integer part, then the fraction after it
    char* cursor = end - 16;
    if (negative)
        *--text = '-';
    if (fraction != 0){
        // `decimals` digits with leading zeros, trailing zeros dropped
        char digits[16];
        for (int i = decimals - 1; i >= 0; i--){
            digits[i] = (char)('0' + (int)(fraction % 10));
            fraction /= 10;
        }
        int count = decimals;
        while (count > 0 && digits[count - 1] == '0')
            count--;
        *cursor++ = '.';
        for (int i = 0; i < count; i++)
            *cursor++ = digits[i];
    }
    out.append(text, (size_t)(cursor - text));
}

std::string jasperengine::htmlui::compat::formatNumber(double value, int decimals){
    std::string out;
    appendNumber(out, value, decimals);
    return out;
}

void jasperengine::htmlui::compat::appendIdent(std::string& out, std::string_view ident){
    // the common case: ASCII name characters, not starting with a digit or "-<digit>", not "-"
    {
        bool plain = !ident.empty() && !(ident.size() == 1 && ident[0] == '-') && !(ident[0] >= '0' && ident[0] <= '9') &&
                     !(ident.size() >= 2 && ident[0] == '-' && ident[1] >= '0' && ident[1] <= '9');
        for (size_t i = 0; plain && i < ident.size(); i++){
            const char c = ident[i];
            plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        }
        if (plain){
            out.append(ident.data(), ident.size());
            return;
        }
    }
    size_t i = 0;
    size_t index = 0;
    while (i < ident.size()){
        const uint32_t c = nextCodePoint(ident, i);
        if (c == 0){
            out += "\xEF\xBF\xBD";
        }else if ((c >= 0x01 && c <= 0x1F) || c == 0x7F){
            appendEscapedCodePoint(out, c);
        }else if (index == 0 && c >= '0' && c <= '9'){
            appendEscapedCodePoint(out, c);
        }else if (index == 1 && c >= '0' && c <= '9' && ident[0] == '-'){
            appendEscapedCodePoint(out, c);
        }else if (index == 0 && c == '-' && ident.size() == 1){
            out += "\\-";
        }else if (c >= 0x80 || c == '-' || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')){
            appendUtf8(out, c);
        }else{
            out += '\\';
            appendUtf8(out, c);
        }
        index++;
    }
}

std::string jasperengine::htmlui::compat::serializeIdent(std::string_view ident){
    std::string out;
    appendIdent(out, ident);
    return out;
}

void jasperengine::htmlui::compat::appendString(std::string& out, std::string_view text){
    out += '"';
    size_t i = 0;
    while (i < text.size()){
        // a run of printable ASCII without '"' and '\\' is written as it is
        size_t run = i;
        while (run < text.size()){
            const unsigned char b = (unsigned char)text[run];
            if (b < 0x20 || b >= 0x7F || b == '"' || b == '\\')
                break;
            run++;
        }
        if (run > i){
            out.append(text.data() + i, run - i);
            i = run;
            continue;
        }
        const uint32_t c = nextCodePoint(text, i);
        if (c == 0)
            out += "\xEF\xBF\xBD";
        else if ((c >= 0x01 && c <= 0x1F) || c == 0x7F)
            appendEscapedCodePoint(out, c);
        else if (c == '"' || c == '\\'){
            out += '\\';
            out += (char)c;
        }else
            appendUtf8(out, c);
    }
    out += '"';
}

std::string jasperengine::htmlui::compat::serializeString(std::string_view text){
    std::string out;
    appendString(out, text);
    return out;
}

std::string jasperengine::htmlui::compat::serializeUrl(std::string_view url){
    std::string out = "url(";
    appendString(out, url);
    out += ')';
    return out;
}

void jasperengine::htmlui::compat::appendToken(std::string& out, const Token& t){
    switch (t.type){
        case TokenType::Ident: appendIdent(out, t.value); return;
        case TokenType::Function: appendIdent(out, t.value); out += '('; return;
        case TokenType::AtKeyword: out += '@'; appendIdent(out, t.value); return;
        case TokenType::Hash: {
            out += '#';
            if (t.hashIsId){
                appendIdent(out, t.value);
                return;
            }
            // "#1a": the name is not an identifier; escape only what is not a name character
            size_t i = 0;
            while (i < t.value.size()){
                const uint32_t c = nextCodePoint(t.value, i);
                if (isIdentCodePoint(c))
                    appendUtf8(out, c);
                else if ((c >= 0x01 && c <= 0x1F) || c == 0x7F)
                    appendEscapedCodePoint(out, c);
                else{
                    out += '\\';
                    appendUtf8(out, c);
                }
            }
            return;
        }
        case TokenType::String: appendString(out, t.value); return;
        case TokenType::BadString: return;
        case TokenType::Url: out += "url("; appendString(out, t.value); out += ')'; return;
        case TokenType::BadUrl: return;
        case TokenType::Delim: out += t.value; return;
        case TokenType::Number: appendNumber(out, t.number); return;
        case TokenType::Percentage: appendNumber(out, t.number); out += '%'; return;
        case TokenType::Dimension: {
            appendNumber(out, t.number);
            // "1e3" as a unit would read as an exponent: escape the 'e' of such units
            if (!t.unit.empty() && (t.unit[0] == 'e' || t.unit[0] == 'E') && t.unit.size() > 1 &&
                ((t.unit[1] >= '0' && t.unit[1] <= '9') || t.unit[1] == '-' || t.unit[1] == '+'))
                out += '\\';
            appendIdent(out, t.unit);
            return;
        }
        case TokenType::UnicodeRange: {
            static const char* hex = "0123456789ABCDEF";
            auto toHex = [](uint32_t v){
                std::string s;
                do { s.insert(s.begin(), hex[v & 0xF]); v >>= 4; } while (v != 0);
                return s;
            };
            out += "U+" + toHex(t.rangeStart);
            if (t.rangeEnd != t.rangeStart)
                out += "-" + toHex(t.rangeEnd);
            return;
        }
        case TokenType::Whitespace: out += ' '; return;
        case TokenType::CDO: out += "<!--"; return;
        case TokenType::CDC: out += "-->"; return;
        case TokenType::Colon: out += ':'; return;
        case TokenType::Semicolon: out += ';'; return;
        case TokenType::Comma: out += ','; return;
        case TokenType::LeftBracket: out += '['; return;
        case TokenType::RightBracket: out += ']'; return;
        case TokenType::LeftParen: out += '('; return;
        case TokenType::RightParen: out += ')'; return;
        case TokenType::LeftBrace: out += '{'; return;
        case TokenType::RightBrace: out += '}'; return;
        case TokenType::Comment:
        case TokenType::EndOfFile:
            return;
    }
}

std::string jasperengine::htmlui::compat::serializeToken(const Token& t){
    std::string out;
    appendToken(out, t);
    return out;
}

void jasperengine::htmlui::compat::appendSerialized(std::string& out, const ComponentValue& value, const SerializeOptions& options){
    if (value.kind == ComponentValue::Kind::Token)
        appendToken(out, value.token);
    else
        appendBlockOrFunction(out, value, options);
}

void jasperengine::htmlui::compat::appendSerialized(std::string& out, const std::vector<ComponentValue>& values, const SerializeOptions& options){
    appendList(out, values, options);
}

std::string jasperengine::htmlui::compat::serialize(const ComponentValue& value, const SerializeOptions& options){
    std::string out;
    appendSerialized(out, value, options);
    return out;
}

std::string jasperengine::htmlui::compat::serialize(const std::vector<ComponentValue>& values, const SerializeOptions& options){
    std::string out;
    appendList(out, values, options);
    return out;
}
