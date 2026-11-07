// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_TEXT_INPUT_H
#define ANDROID_TEXT_INPUT_H

// The text conversions of the Android soft keyboard (GameTextInput) and the component
// InputField's diff, without Android headers, so the core tests run them on any host.

#include "util/StringUtils.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace jasperengine::android {

// GameTextInput text is Java's modified UTF-8 (NUL as C0 80, characters outside the BMP as
// two encoded surrogates) and its spans count UTF-16 units. Decodes to UTF-16 units; plain
// 4-byte UTF-8 is accepted too and malformed bytes become U+FFFD.
inline std::u16string decodeModifiedUtf8(const char* text, int32_t length) {
    std::u16string out;
    if (!text || length <= 0)
        return out;

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text);
    const size_t size = strnlen(text, (size_t)length);
    out.reserve(size);

    auto isCont = [&](size_t i) { return (bytes[i] & 0xC0) == 0x80; };

    size_t i = 0;
    while (i < size) {
        unsigned char b0 = bytes[i];
        if (b0 < 0x80) {
            out.push_back(b0);
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < size && isCont(i + 1)) {
            out.push_back((char16_t)(((b0 & 0x1F) << 6) | (bytes[i + 1] & 0x3F)));
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < size && isCont(i + 1) && isCont(i + 2)) {
            out.push_back((char16_t)(((b0 & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F)));
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < size && isCont(i + 1) && isCont(i + 2) && isCont(i + 3)) {
            uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(bytes[i + 1] & 0x3F) << 12) |
                          ((uint32_t)(bytes[i + 2] & 0x3F) << 6) | (uint32_t)(bytes[i + 3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                cp -= 0x10000;
                out.push_back((char16_t)(0xD800 + (cp >> 10)));
                out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
            } else {
                out.push_back(0xFFFD);
            }
            i += 4;
        } else {
            out.push_back(0xFFFD);
            i += 1;
        }
    }
    return out;
}

// Code points of UTF-16 units. unitToCodepoint has units.size() + 1 entries: the code point
// index at each unit (the second unit of a pair maps past it). Lone surrogates become U+FFFD.
inline void utf16ToCodepoints(const std::u16string& units, std::u32string& text, std::vector<size_t>& unitToCodepoint) {
    text.clear();
    text.reserve(units.size());
    unitToCodepoint.assign(units.size() + 1, 0);

    size_t i = 0;
    while (i < units.size()) {
        unitToCodepoint[i] = text.size();
        char32_t unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units.size() &&
                units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            text.push_back(0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            unitToCodepoint[i + 1] = text.size();
            i += 2;
        } else {
            text.push_back((unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFD : unit);
            i += 1;
        }
    }
    unitToCodepoint[units.size()] = text.size();
}

// Code points of a wide string. wchar_t is 32 bits on Android, but surrogate pairs that
// arrive as two units are combined too.
inline std::u32string wideToCodepoints(const std::wstring& text) {
    std::u32string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        uint32_t cp = (uint32_t)text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() &&
                (uint32_t)text[i + 1] >= 0xDC00 && (uint32_t)text[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)text[i + 1] - 0xDC00);
            i++;
        } else if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            cp = 0xFFFD;
        }
        out.push_back(cp);
    }
    return out;
}

// Java modified UTF-8 of code points, what GameTextInput hands to NewStringUTF;
// utf16Length receives the length in UTF-16 units
inline std::string codepointsToModifiedUtf8(const std::u32string& text, int32_t& utf16Length) {
    std::string out;
    utf16Length = 0;

    auto put3 = [&out](uint32_t unit) {
        out.push_back((char)(0xE0 | (unit >> 12)));
        out.push_back((char)(0x80 | ((unit >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (unit & 0x3F)));
    };

    for (char32_t c : text) {
        uint32_t cp = c;
        if (cp != 0 && cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            put3(cp);
        } else {
            cp -= 0x10000;
            put3(0xD800 + (cp >> 10));
            put3(0xDC00 + (cp & 0x3FF));
            utf16Length++;
        }
        utf16Length++;
    }
    return out;
}

inline std::string codepointsToUtf8(const std::u32string& text, size_t start, size_t end) {
    std::string out;
    for (size_t i = start; i < end && i < text.size(); i++) {
        out += StringUtils::toUTF8(text[i]);
    }
    return out;
}

// What a text change the keyboard reports does to the component InputField: backspaces at
// the end of the text, then characters typed there
struct TextEdit {
    size_t backspaces = 0;
    std::u32string typed;
};

// The component InputField's view of the keyboard's text: the soft keyboard holds the text
// in UTF-16 units, the field in code points
class ComponentTextInput {
public:
    // the field's text when the keyboard opens (showSoftInput), or after a document field took
    // the keyboard's text
    void reset(const std::u32string& codepoints) {
        text.assign(codepoints.begin(), codepoints.end());
    }

    // the keyboard's new text: what to send to the field, by whole characters (a pair of
    // UTF-16 units is one character, typed once and erased with one backspace)
    TextEdit update(const std::u16string& units) {
        std::u32string next;
        std::vector<size_t> unitToCodepoint;
        utf16ToCodepoints(units, next, unitToCodepoint);
        size_t prefix = 0;
        while (prefix < text.size() && prefix < next.size() && text[prefix] == next[prefix]) {
            prefix++;
        }
        TextEdit edit;
        edit.backspaces = text.size() - prefix;
        edit.typed = next.substr(prefix);
        text = std::move(next);
        return edit;
    }

    const std::u32string& current() const {
        return text;
    }

private:
    std::u32string text;
};

}

#endif // ANDROID_TEXT_INPUT_H
