// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R53, §6.2)
// - Purpose: the characters a Realtime or Glyph Atlas title can show in a build, which its fonts
//   are subset to (TitleFontSubsetter) and the character checks test (TitleCharacterChecks): the
//   document's `runtime.characters` = its presets ∪ Custom ∪ Custom File ∪ (when
//   `document_text`) every text of the document.
// - Presets (exact code point lists): digits (`0123456789+-.,:/%×x$€£¥` and space), latin
//   (U+0020-U+007E), latin_extended (latin, U+00A0-U+024F, U+1E00-U+1EFF, U+2000-U+206F,
//   U+20A0-U+20CF), kana (U+3000-U+303F, U+3040-U+309F, U+30A0-U+30FF, U+31F0-U+31FF,
//   U+FF00-U+FFEF), joyo_kanji (the 2,136 characters of the 2010 list,
//   editor/resources/titleart/charsets/joyo-kanji.txt, embedded in the editor).
// - Document text: every text layer's `text` at any depth, the current value of each target of
//   an exposed `text` parameter (its default), and every string under a `translations` member of
//   a layer or of the document (S87's languages; absent until S87 lands).
// - Text files (Custom File: .txt, .csv, .json): every code point of the UTF-8 text counts but
//   line ends, tabs and a byte order mark.
// - Thread-safety: pure functions and values; the preset tables are built once, thread-safe.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::editor {

    // A sorted set of unique Unicode code points (surrogates and values past U+10FFFF are refused)
    class TitleCharacterSet {
    public:
        TitleCharacterSet() = default;

        // false for a surrogate or a value past U+10FFFF (nothing added)
        bool add(uint32_t codepoint);
        void addRange(uint32_t first, uint32_t last);              // inclusive
        // every code point of UTF-8 text (invalid bytes are skipped); `skipControls` leaves out
        // C0 controls, DEL and U+FEFF (a text file's line ends, tabs and byte order mark)
        void addUtf8(std::string_view text, bool skipControls = false);
        void merge(const TitleCharacterSet& other);

        bool contains(uint32_t codepoint) const;
        size_t size() const { return codepoints.size(); }
        bool empty() const { return codepoints.empty(); }
        const std::vector<uint32_t>& list() const { return codepoints; }

        // the code points of `text` (in order of first appearance, once each) that the set lacks
        std::vector<uint32_t> missingFrom(std::string_view text) const;
        // 16 lowercase hex of a stable hash of the code points (the subset cache key)
        std::string hash() const;
        std::string toUtf8() const;

        bool operator==(const TitleCharacterSet& other) const { return codepoints == other.codepoints; }

    private:
        std::vector<uint32_t> codepoints;       // sorted, unique

        void mergeSorted(const std::vector<uint32_t>& sorted);
    };

    // UTF-8 of one code point
    std::string titleUtf8Of(uint32_t codepoint);

    // the preset names of runtime.characters.presets, in the schema's order
    const std::vector<std::string>& titleCharacterPresetNames();
    // a preset's code points; false for an unknown name
    bool titleCharacterPreset(const std::string& name, TitleCharacterSet& out);

    // Reads a Custom File's bytes; false with `error`. The default (titleCharacterFileReader) goes
    // through an AssetResolver.
    using TitleCharacterFileReader = std::function<bool(const titleart::TitleAssetRef& ref, std::string& text, std::string& error)>;
    TitleCharacterFileReader titleCharacterFileReader(const titleart::AssetResolver& assets);

    // what building a document's set read and could not read
    struct TitleCharactersReport {
        std::vector<std::string> presets;           // the presets used
        size_t customCount = 0;                     // code points Custom added
        size_t fileCount = 0;                       // code points the Custom File held
        size_t documentCount = 0;                   // code points of the document's texts
        // the characters Custom and the Custom File name one by one: a glyph missing for one of
        // them is a problem, a preset's gap is not (presets are broad ranges)
        TitleCharacterSet declared;
        std::vector<std::string> warnings;          // an unreadable Custom File, an unknown preset
    };

    // The document's Characters (R53). `readFile` reads the Custom File (null: the file is not
    // read, with a warning when one is set).
    TitleCharacterSet titleDocumentCharacters(const titleart::TitleArtDoc& doc, const TitleCharacterFileReader& readFile,
                                              TitleCharactersReport* report = nullptr);
    // only the document's texts (layers, exposed text defaults, translations)
    void addTitleDocumentText(const titleart::TitleArtDoc& doc, TitleCharacterSet& out);
    // the document's Characters count, as the Title Designer and the Inspector show it
    size_t titleDocumentCharacterCount(const titleart::TitleArtDoc& doc, const TitleCharacterFileReader& readFile);

}
