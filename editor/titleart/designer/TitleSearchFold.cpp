// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleSearchFold.h"

#include "search/SearchScorer.h"

namespace {

    namespace search = jasperengine::editor::search;

    // U+FF66..U+FF9D (ｦ .. ﾝ) as full-width katakana
    constexpr char32_t HALF_WIDTH_KANA[] = {
        0x30F2,                                         // ｦ
        0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,         // ｧ ｨ ｩ ｪ ｫ
        0x30E3, 0x30E5, 0x30E7, 0x30C3,                 // ｬ ｭ ｮ ｯ
        0x30FC,                                         // ｰ
        0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA,         // ｱ ｲ ｳ ｴ ｵ
        0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,         // ｶ ｷ ｸ ｹ ｺ
        0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,         // ｻ ｼ ｽ ｾ ｿ
        0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,         // ﾀ ﾁ ﾂ ﾃ ﾄ
        0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE,         // ﾅ ﾆ ﾇ ﾈ ﾉ
        0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,         // ﾊ ﾋ ﾌ ﾍ ﾎ
        0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2,         // ﾏ ﾐ ﾑ ﾒ ﾓ
        0x30E4, 0x30E6, 0x30E8,                         // ﾔ ﾕ ﾖ
        0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED,         // ﾗ ﾘ ﾙ ﾚ ﾛ
        0x30EF, 0x30F3,                                 // ﾜ ﾝ
    };
    constexpr char32_t HALF_VOICED = 0xFF9E;            // ﾞ
    constexpr char32_t HALF_SEMI_VOICED = 0xFF9F;       // ﾟ

    // the voiced form of a katakana (カ -> ガ, ウ -> ヴ), 0 when it has none
    char32_t voiced(char32_t kana) {
        if (kana == 0x30A6) return 0x30F4;                                          // ウ -> ヴ
        if (kana >= 0x30AB && kana <= 0x30C1 && (kana - 0x30AB) % 2 == 0) return kana + 1;   // カ .. チ
        if (kana == 0x30C4 || kana == 0x30C6 || kana == 0x30C8) return kana + 1;    // ツ テ ト
        if (kana >= 0x30CF && kana <= 0x30DB && (kana - 0x30CF) % 3 == 0) return kana + 1;   // ハ .. ホ
        if (kana == 0x30EF) return 0x30F7;                                          // ワ -> ヷ
        if (kana == 0x30F2) return 0x30FA;                                          // ヲ -> ヺ
        return 0;
    }

    // the semi-voiced form (ハ -> パ), 0 when it has none
    char32_t semiVoiced(char32_t kana) {
        if (kana >= 0x30CF && kana <= 0x30DB && (kana - 0x30CF) % 3 == 0) return kana + 2;
        return 0;
    }

    void appendUtf8(std::string& out, char32_t c) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }

}

std::string jasperengine::editor::titledesigner::foldForSearch(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    auto emit = [&](char32_t c) {
        if (search::isSeparator(c)) {
            pendingSpace = !out.empty();
            return;
        }
        if (pendingSpace) out.push_back(' ');
        pendingSpace = false;
        appendUtf8(out, c);
    };

    for (size_t i = 0; i < text.size();) {
        size_t length = 1;
        char32_t c = search::decodeUtf8(text, i, length);
        if (c == search::INVALID_UTF8) {
            // a stray byte is kept as it is (folding never loses part of a name)
            if (pendingSpace) out.push_back(' ');
            pendingSpace = false;
            out.push_back(text[i]);
            i++;
            continue;
        }
        i += length;

        if (c >= 0xFF01 && c <= 0xFF5E) c = c - 0xFF01 + 0x21;          // full-width ASCII
        else if (c == 0x3000) c = ' ';                                   // ideographic space
        else if (c >= 0xFF66 && c <= 0xFF9D) {
            c = HALF_WIDTH_KANA[c - 0xFF66];
            // a following half-width sound mark composes with it
            if (i < text.size()) {
                size_t markLength = 1;
                const char32_t mark = search::decodeUtf8(text, i, markLength);
                const char32_t composed = mark == HALF_VOICED ? voiced(c) : mark == HALF_SEMI_VOICED ? semiVoiced(c) : 0;
                if (composed != 0) {
                    c = composed;
                    i += markLength;
                }
            }
        } else if (c == HALF_VOICED) c = 0x309B;                         // a lone ﾞ: ゛
        else if (c == HALF_SEMI_VOICED) c = 0x309C;                      // a lone ﾟ: ゜

        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        else if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60;                  // katakana -> hiragana (ァ .. ヶ)
        else if (c == 0x30FD || c == 0x30FE) c -= 0x60;                  // ヽ ヾ -> ゝ ゞ
        emit(c);
    }
    return out;
}
