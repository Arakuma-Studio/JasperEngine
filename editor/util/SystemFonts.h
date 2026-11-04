// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The system's CJK and emoji fonts as fallbacks of the editor's fonts, so Chinese, Japanese and
// Korean names and text draw as glyphs instead of boxes. Derived from Doriax's
// editor/util/SystemFonts.cpp (84bb05ab0): the lookup runs off the main thread, macOS asks
// CoreText, and the Han glyph forms follow a preference.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImFont;

namespace jasperengine::editor {

    // Han characters are drawn differently per language (Han unification)
    enum class CjkLanguage { Auto, Japanese, Korean, SimplifiedChinese, TraditionalChinese };

    class SystemFonts {
    public:
        // A font file the editor merges: memory-mapped for the editor's lifetime (ImGui does not
        // own the data)
        struct Font {
            std::string path;
            int faceIndex = 0;
            std::string covers;      // "ja", "ko", "zh-Hans", "zh-Hant" or "emoji"
            const void* data = nullptr;
            size_t size = 0;
        };

        // A font the editor draws with, and the settings its fallbacks are merged with
        struct Target {
            ImFont* font = nullptr;
            float sizePixels = 0.0f;
            float rasterizerMultiply = 1.0f;
        };

        enum class State { Idle, Searching, Ready, None };

        // Starts the search on the jobs' Background lane, once (later calls do nothing); the job
        // system runs from Engine::systemInit
        static void startSearch(CjkLanguage preference);
        static State state();
        // The fonts found (empty until Ready)
        static std::vector<Font> fonts();
        // The language whose glyph forms come first (the preference, or the user's language)
        static CjkLanguage effectiveLanguage();
        // Main thread, before ImGui::NewFrame: merges the fonts into the targets once they are
        // found. True on the call that merged.
        static bool applyPending(const std::vector<Target>& targets);
        // Merges these fonts into the targets (applyPending's work; tests give their own fonts)
        static void mergeInto(const std::vector<Font>& fonts, const std::vector<Target>& targets);

        // Preferences values (settings.yaml editor.cjk_font_language): auto, japanese, korean,
        // simplified_chinese, traditional_chinese
        static const char* settingName(CjkLanguage language);
        static CjkLanguage fromSettingName(std::string_view name);
        static bool isLanguageSetting(std::string_view name);
        // BCP 47 tag: "ja", "ko", "zh-Hans", "zh-Hant" ("auto" for Auto)
        static const char* languageTag(CjkLanguage language);

        // The language of a locale name ("ja_JP.UTF-8", "zh-Hant-TW", "ko-KR"); anything else is
        // Simplified Chinese, whose fonts cover the most Han characters
        static CjkLanguage languageOfLocale(std::string_view locale);
        // The languages searched, in order: the preferred one, then Simplified Chinese (Han
        // coverage), Korean (Hangul), Japanese (kana), each once
        static std::vector<CjkLanguage> searchOrder(CjkLanguage preference, std::string_view userLocale);

        // The fonts in `fc-match -s -f '%{file}\t%{index}\t%{lang}\n'` output that ImGui can load
        // (.ttf, .ttc, .otf; no variable-font instance) and that cover fontconfig language `lang`
        static std::vector<std::pair<std::string, int>> parseFontconfig(std::string_view output, std::string_view lang);
        // The checks ImGui's stb_truetype loader asserts on
        static bool isLoadable(const void* data, size_t size, int faceIndex);
        // Each face's family name and PostScript name (one entry for a single font)
        static std::vector<std::pair<std::string, std::string>> faceNames(const void* data, size_t size);
        // True when the face has an outline (not only a bitmap) for the code point
        static bool hasOutline(const void* data, size_t size, int faceIndex, uint32_t codepoint);
    };

}
