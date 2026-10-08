// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/ListMarkers.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine::htmlui::compat;

namespace {

    struct TypeName {
        const char* name;
        ListMarkers::Type type;
    };

    const TypeName kTypes[] = {
        {"none", ListMarkers::Type::None},
        {"disc", ListMarkers::Type::Disc},
        {"circle", ListMarkers::Type::Circle},
        {"square", ListMarkers::Type::Square},
        {"decimal", ListMarkers::Type::Decimal},
        {"decimal-leading-zero", ListMarkers::Type::DecimalLeadingZero},
        {"lower-alpha", ListMarkers::Type::LowerAlpha},
        {"lower-latin", ListMarkers::Type::LowerAlpha},
        {"upper-alpha", ListMarkers::Type::UpperAlpha},
        {"upper-latin", ListMarkers::Type::UpperAlpha},
        {"lower-roman", ListMarkers::Type::LowerRoman},
        {"upper-roman", ListMarkers::Type::UpperRoman},
        {"lower-greek", ListMarkers::Type::LowerGreek},
        {"disclosure-open", ListMarkers::Type::DisclosureOpen},
        {"disclosure-closed", ListMarkers::Type::DisclosureClosed},
    };

    // CSS Counter Styles 3 predefined styles the engine does not draw: numbered ones count
    // in decimal (the CSS fallback of every counter style)
    const char* const kDecimalFallbacks[] = {
        "arabic-indic", "armenian", "bengali", "cambodian", "cjk-decimal", "cjk-earthly-branch",
        "cjk-heavenly-stem", "cjk-ideographic", "devanagari", "ethiopic-numeric", "georgian", "gujarati",
        "gurmukhi", "hebrew", "hiragana", "hiragana-iroha", "japanese-formal", "japanese-informal", "kannada",
        "katakana", "katakana-iroha", "khmer", "korean-hangul-formal", "korean-hanja-formal",
        "korean-hanja-informal", "lao", "lower-armenian", "malayalam", "mongolian", "myanmar", "oriya",
        "persian", "simp-chinese-formal", "simp-chinese-informal", "tamil", "telugu", "thai", "tibetan",
        "trad-chinese-formal", "trad-chinese-informal", "upper-armenian",
    };

    std::string lower(std::string_view text){
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return out;
    }

    std::string trim(std::string_view text){
        const size_t first = text.find_first_not_of(" \t\r\n\f");
        if (first == std::string_view::npos)
            return std::string();
        const size_t last = text.find_last_not_of(" \t\r\n\f");
        return std::string(text.substr(first, last - first + 1));
    }

    // CSS alphabetic counter system: a b ... z aa ab ...
    std::string alphabetic(int index, const char* const* symbols, int count){
        std::string out;
        unsigned int value = (unsigned int)index;
        while (value > 0){
            value--;
            out.insert(0, symbols[value % (unsigned int)count]);
            value /= (unsigned int)count;
        }
        return out;
    }

    std::string roman(int index, bool upper){
        static const int values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
        static const char* const lowerSymbols[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
        static const char* const upperSymbols[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
        std::string out;
        int value = index;
        for (int i = 0; i < 13; i++){
            while (value >= values[i]){
                out += upper ? upperSymbols[i] : lowerSymbols[i];
                value -= values[i];
            }
        }
        return out;
    }

}

ListMarkers::Type ListMarkers::parseType(std::string_view keyword){
    const std::string name = lower(trim(keyword));
    for (const TypeName& entry : kTypes){
        if (name == entry.name)
            return entry.type;
    }
    for (const char* fallback : kDecimalFallbacks){
        if (name == fallback)
            return Type::Decimal;
    }
    return Type::String;
}

const char* ListMarkers::typeName(Type type){
    if (type == Type::String)
        return "string";
    for (const TypeName& entry : kTypes){
        if (entry.type == type)
            return entry.name;
    }
    return "none";
}

bool ListMarkers::isCounter(Type type){
    switch (type){
        case Type::Decimal:
        case Type::DecimalLeadingZero:
        case Type::LowerAlpha:
        case Type::UpperAlpha:
        case Type::LowerRoman:
        case Type::UpperRoman:
        case Type::LowerGreek:
            return true;
        default:
            return false;
    }
}

std::string ListMarkers::counter(Type type, int index){
    static const char* const lowerLatin[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                              "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
    static const char* const upperLatin[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                              "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    // CSS lower-greek: 24 letters, no final sigma
    static const char* const greek[] = {"\xCE\xB1", "\xCE\xB2", "\xCE\xB3", "\xCE\xB4", "\xCE\xB5", "\xCE\xB6",
                                         "\xCE\xB7", "\xCE\xB8", "\xCE\xB9", "\xCE\xBA", "\xCE\xBB", "\xCE\xBC",
                                         "\xCE\xBD", "\xCE\xBE", "\xCE\xBF", "\xCF\x80", "\xCF\x81", "\xCF\x83",
                                         "\xCF\x84", "\xCF\x85", "\xCF\x86", "\xCF\x87", "\xCF\x88", "\xCF\x89"};
    switch (type){
        case Type::DecimalLeadingZero:{
            // CSS: two digits, the minus sign before the padding ("-01")
            const std::string digits = std::to_string(index < 0 ? -(long long)index : (long long)index);
            return (index < 0 ? "-" : "") + (digits.size() < 2 ? "0" + digits : digits);
        }
        case Type::LowerAlpha:
            return index >= 1 ? alphabetic(index, lowerLatin, 26) : std::to_string(index);
        case Type::UpperAlpha:
            return index >= 1 ? alphabetic(index, upperLatin, 26) : std::to_string(index);
        case Type::LowerGreek:
            return index >= 1 ? alphabetic(index, greek, 24) : std::to_string(index);
        case Type::LowerRoman:
            return (index >= 1 && index <= 3999) ? roman(index, false) : std::to_string(index);
        case Type::UpperRoman:
            return (index >= 1 && index <= 3999) ? roman(index, true) : std::to_string(index);
        case Type::Decimal:
            return std::to_string(index);
        default:
            return std::string();
    }
}

std::string ListMarkers::text(Type type, int index){
    switch (type){
        case Type::None:
        case Type::String:
            return std::string();
        // Chromium's bullet glyphs, followed by a space
        case Type::Disc: return "\xE2\x80\xA2 ";               // U+2022 BULLET
        case Type::Circle: return "\xE2\x97\xA6 ";             // U+25E6 WHITE BULLET
        case Type::Square: return "\xE2\x96\xAA ";             // U+25AA BLACK SMALL SQUARE
        case Type::DisclosureOpen: return "\xE2\x96\xBE ";     // U+25BE BLACK DOWN-POINTING SMALL TRIANGLE
        case Type::DisclosureClosed: return "\xE2\x96\xB8 ";   // U+25B8 BLACK RIGHT-POINTING SMALL TRIANGLE
        default:
            return counter(type, index) + ". ";
    }
}

std::string ListMarkers::text(std::string_view listStyleType, int index){
    const Type type = parseType(listStyleType);
    if (type != Type::String)
        return text(type, index);
    // a string keeps its spaces ("\2192 " in CSS); only quotes around it are removed
    const std::string trimmed = trim(listStyleType);
    if (trimmed.size() >= 2 && (trimmed.front() == '"' || trimmed.front() == '\'') && trimmed.back() == trimmed.front())
        return trimmed.substr(1, trimmed.size() - 2);
    return std::string(listStyleType);
}
