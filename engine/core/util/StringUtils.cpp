// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringUtils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

using namespace jasperengine;

std::string StringUtils::toUTF8(char32_t codepoint){
    uint32_t cp = static_cast<uint32_t>(codepoint);

    // Replace invalid Unicode scalar values.
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = 0xFFFDu;
    }

    std::string out;
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    return out;
}

namespace {

// Keep validation and malformed-byte consumption identical for all UTF-8 consumers.
// Counting does not need an intermediate codepoint vector.
template <typename Emit>
void visitUtf8Codepoints(const std::string& text, bool& hadInvalid, Emit emit) {
    hadInvalid = false;

    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        unsigned char b0 = bytes[i];

        if (b0 <= 0x7F) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= text.size()) {
                hadInvalid = true;
                cp = 0xFFFD;
                i += 1;
            } else {
                unsigned char b1 = bytes[i + 1];
                uint32_t v = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
                if ((b1 & 0xC0) != 0x80 || v < 0x80) {
                    hadInvalid = true;
                    cp = 0xFFFD;
                    i += 1;
                } else {
                    cp = v;
                    i += 2;
                }
            }
        } else if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= text.size()) {
                hadInvalid = true;
                cp = 0xFFFD;
                i += 1;
            } else {
                unsigned char b1 = bytes[i + 1];
                unsigned char b2 = bytes[i + 2];
                uint32_t v = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
                if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) {
                    hadInvalid = true;
                    cp = 0xFFFD;
                    i += 1;
                } else {
                    cp = v;
                    i += 3;
                }
            }
        } else if ((b0 & 0xF8) == 0xF0) {
            if (i + 3 >= text.size()) {
                hadInvalid = true;
                cp = 0xFFFD;
                i += 1;
            } else {
                unsigned char b1 = bytes[i + 1];
                unsigned char b2 = bytes[i + 2];
                unsigned char b3 = bytes[i + 3];
                uint32_t v = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12) |
                             ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
                if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80 || v < 0x10000 || v > 0x10FFFF) {
                    hadInvalid = true;
                    cp = 0xFFFD;
                    i += 1;
                } else {
                    cp = v;
                    i += 4;
                }
            }
        } else {
            hadInvalid = true;
            cp = 0xFFFD;
            i += 1;
        }

        emit(cp);
    }
}

// The length of the valid UTF-8 sequence at text[i], 0 when it is not one (RFC 3629 table 3)
size_t utf8SequenceLength(std::string_view text, size_t i) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char c = byte(i);
    if (c < 0x80) return 1;
    size_t length = 0;
    unsigned char low = 0x80, high = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) length = 2;
    else if (c == 0xE0) { length = 3; low = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) length = 3;
    else if (c == 0xED) { length = 3; high = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) length = 3;
    else if (c == 0xF0) { length = 4; low = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) length = 4;
    else if (c == 0xF4) { length = 4; high = 0x8F; }
    else return 0;
    if (i + length > text.size()) return 0;
    if (byte(i + 1) < low || byte(i + 1) > high) return 0;
    for (size_t k = 2; k < length; k++) {
        if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) return 0;
    }
    return length;
}

// The length of the ASCII run at the start of text, eight bytes at a time
size_t asciiPrefix(std::string_view text) {
    size_t i = 0;
    while (i + 8 <= text.size()) {
        uint64_t word;
        std::memcpy(&word, text.data() + i, sizeof(word));
        if (word & 0x8080808080808080ull) break;
        i += 8;
    }
    while (i < text.size() && static_cast<unsigned char>(text[i]) < 0x80) i++;
    return i;
}

} // namespace

bool StringUtils::isValidUtf8(std::string_view text) {
    size_t i = asciiPrefix(text);
    while (i < text.size()) {
        const size_t length = utf8SequenceLength(text, i);
        if (length == 0) return false;
        i += length;
        if (length == 1) i += asciiPrefix(text.substr(i));
    }
    return true;
}

std::string StringUtils::repairUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    size_t i = 0;
    while (i < text.size()) {
        const size_t length = utf8SequenceLength(text, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            i++;
        } else {
            out.append(text.data() + i, length);
            i += length;
        }
    }
    return out;
}

std::string StringUtils::legacyToUtf8(std::string_view text) {
    if (isValidUtf8(text)) return std::string(text);
#ifdef _WIN32
    // a process in the UTF-8 code page writes no ANSI text: what is not UTF-8 is broken text
    // (a cut string, a child's bytes), repaired below; read as ANSI it would become other letters
    if (GetACP() == CP_UTF8) return repairUtf8(text);
    // the system's ANSI code page (before Windows 10 1903 the process runs in it)
    static const UINT codePage = []() {
        DWORD value = 0;
        const int got = GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                                        reinterpret_cast<LPWSTR>(&value), sizeof(value) / sizeof(WCHAR));
        return got > 0 && value != 0 ? static_cast<UINT>(value) : 1252u;
    }();
    const int length = static_cast<int>(text.size());
    const int wideSize = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (wideSize > 0) {
        std::wstring wide(static_cast<size_t>(wideSize), L'\0');
        MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), wideSize);
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            std::string utf8(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, utf8.data(), size, nullptr, nullptr);
            return utf8;
        }
    }
#endif
    return repairUtf8(text);
}

std::vector<uint32_t> StringUtils::decodeUtf8ToCodepoints(const std::string& text, bool& hadInvalid) {
    hadInvalid = false;
    std::vector<uint32_t> out;
    out.reserve(text.size());
    visitUtf8Codepoints(text, hadInvalid, [&out](uint32_t cp) { out.push_back(cp); });
    return out;
}

std::wstring StringUtils::utf8ToWString(const std::string& text, bool& hadInvalid) {
    std::vector<uint32_t> cps = decodeUtf8ToCodepoints(text, hadInvalid);

    std::wstring out;
    out.reserve(cps.size());

    if constexpr (sizeof(wchar_t) == 2) {
        for (uint32_t cp : cps) {
            if (cp <= 0xFFFFu) {
                out.push_back(static_cast<wchar_t>(cp));
            } else {
                cp -= 0x10000u;
                wchar_t high = static_cast<wchar_t>(0xD800u + (cp >> 10));
                wchar_t low = static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu));
                out.push_back(high);
                out.push_back(low);
            }
        }
    } else {
        for (uint32_t cp : cps) {
            if (cp > static_cast<uint32_t>(std::numeric_limits<wchar_t>::max())) {
                out.push_back(static_cast<wchar_t>(0xFFFDu));
                hadInvalid = true;
            } else {
                out.push_back(static_cast<wchar_t>(cp));
            }
        }
    }

    return out;
}

std::vector<std::string> StringUtils::split(const std::string& text, char separator) {
    std::vector<std::string> out;

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(separator, start);
        if (end == std::string::npos) {
            end = text.size();
        }

        size_t first = text.find_first_not_of(" \t\r\n", start);
        if (first != std::string::npos && first < end) {
            size_t last = text.find_last_not_of(" \t\r\n", end - 1);
            out.push_back(text.substr(first, last - first + 1));
        }

        start = end + 1;
    }

    return out;
}

size_t StringUtils::countCodepoints(const std::string& text) {
    bool hadInvalid = false;
    size_t count = 0;
    visitUtf8Codepoints(text, hadInvalid, [&count](uint32_t) { ++count; });
    return count;
}

void StringUtils::eraseCodepointsUtf8(std::string& text, size_t startIndex, size_t endIndex) {
    if (startIndex >= endIndex) {
        return;
    }

    bool hadInvalid = false;
    std::vector<uint32_t> codepoints = decodeUtf8ToCodepoints(text, hadInvalid);
    if (startIndex >= codepoints.size()) {
        return;
    }

    endIndex = std::min(endIndex, codepoints.size());
    codepoints.erase(codepoints.begin() + static_cast<std::ptrdiff_t>(startIndex),
                     codepoints.begin() + static_cast<std::ptrdiff_t>(endIndex));

    text.clear();
    for (uint32_t cp : codepoints) {
        text += toUTF8(static_cast<char32_t>(cp));
    }
}

void StringUtils::insertUtf8AtCodepoint(std::string& text, size_t cpIndex, const std::string& utf8) {
    bool hadInvalid = false;
    std::vector<uint32_t> codepoints = decodeUtf8ToCodepoints(text, hadInvalid);
    std::vector<uint32_t> insertCodepoints = decodeUtf8ToCodepoints(utf8, hadInvalid);

    cpIndex = std::min(cpIndex, codepoints.size());
    codepoints.insert(codepoints.begin() + static_cast<std::ptrdiff_t>(cpIndex),
                      insertCodepoints.begin(), insertCodepoints.end());

    text.clear();
    for (uint32_t cp : codepoints) {
        text += toUTF8(static_cast<char32_t>(cp));
    }
}

void StringUtils::eraseLastCodepointUtf8(std::string& text) {
    if (text.empty()) {
        return;
    }

    // Remove trailing UTF-8 continuation bytes (10xxxxxx)
    size_t i = text.size();
    while (i > 0) {
        unsigned char c = static_cast<unsigned char>(text[i - 1]);
        if ((c & 0xC0) != 0x80) {
            break;
        }
        --i;
    }

    // If we ended up at 0, the string was all continuation bytes; clear it.
    if (i == 0) {
        text.clear();
        return;
    }

    text.erase(i - 1);
}
