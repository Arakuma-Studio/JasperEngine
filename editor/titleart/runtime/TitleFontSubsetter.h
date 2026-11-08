// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R54, R56, R57)
// - Purpose: the fonts a build ships for Realtime and Glyph Atlas titles. readTitleFontInfo reads
//   what the export decides on (OS/2 fsType, the name table's family, copyright, licence);
//   checkTitleFontEmbedding applies R56; TitleFontSubsetter subsets a font to a character set with
//   HarfBuzz's hb_subset_or_fail (the editor-only harfbuzz-subset target, R50); titleFontCreditsText
//   writes licenses/THIRD-PARTY-FONTS.txt.
// - The subset keeps the set's code points and their layout closure (GSUB alternates such as
//   vert, ligatures, every feature), GSUB/GPOS/GDEF, the variation tables (fvar, gvar, avar,
//   HVAR, MVAR, STAT, CFF2 blends), name (every name ID and language, Mac records too), OS/2 and
//   post; hinting goes (HB_SUBSET_FLAGS_NO_HINTING) and so do PostScript glyph names.
// - fsType 0x0100 (No Subsetting) ships the file as it is (`whole`), untouched: no rename either,
//   since an unmodified OFL font may carry its Reserved Font Name.
// - Reserved Font Names (R57): a subset of an OFL font whose copyright declares one is a modified
//   version, so its names are rewritten to `TA <8 hex of the font hash>`: IDs 1, 4, 16 and the
//   ones that repeat the family (3, 18, 21), and the PostScript names 6 and 25 as `TA-<8 hex>`
//   (PostScript names have no spaces), in every platform record (Unicode and Windows as UTF-16BE,
//   Mac Roman as ASCII).
// - Cache: subsets are files in the cache directory (the project's
//   .jasperengine/cache/titleart/subsets/) named by (font hash, set hash, options); a hit reads the
//   file instead of subsetting. The Inspector's size estimate calls subset() too: a Latin subset of
//   a 400 KB font takes a few milliseconds and the result is cached, so there is no separate
//   estimator.
// - Thread-safety: the free functions are pure; one TitleFontSubsetter may be used from one thread
//   at a time, several subsetters (or threads with their own) may share a cache directory (files are
//   written atomically).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // OS/2 fsType bits R56 acts on
    constexpr uint16_t TITLE_FS_TYPE_RESTRICTED = 0x0002;
    constexpr uint16_t TITLE_FS_TYPE_NO_SUBSETTING = 0x0100;
    constexpr uint16_t TITLE_FS_TYPE_BITMAP_ONLY = 0x0200;

    // the font's .meta key of the embedding acknowledgement (R56), false when absent
    constexpr const char* TITLE_FONT_ACK_META_KEY = "import.font.embeddingAcknowledged";
    // the Inspector's checkbox for it
    constexpr const char* TITLE_FONT_ACK_LABEL = "I have a licence that permits embedding this font in a game";

    struct TitleFontInfo {
        uint16_t fsType = 0;
        std::string family;             // name ID 16, else 1
        std::string copyright;          // ID 0
        std::string license;            // ID 13
        std::string licenseUrl;         // ID 14
        bool ofl = false;               // the licence or its URL names the SIL Open Font License
        bool reservedFontName = false;  // the copyright declares a Reserved Font Name
        bool variable = false;          // fvar
        int glyphCount = 0;
    };

    // false with `error` when the bytes are not a font (or the face index is out of range)
    bool readTitleFontInfo(const std::vector<uint8_t>& bytes, int faceIndex, TitleFontInfo& out, std::string& error);

    struct TitleFontEmbeddingCheck {
        bool ok = true;                 // false: the build stops (an error code)
        bool shipWhole = false;         // fsType forbids subsetting
        std::string code;               // "", E-TITLEART-FONT-RESTRICTED, E-TITLEART-FONT-BITMAP-ONLY, W-TITLEART-FONT-NO-SUBSET
        std::string message;            // one sentence: the cause and the fix
    };

    // R56 for a font that ships (fontPath: assets-relative, for the message)
    TitleFontEmbeddingCheck checkTitleFontEmbedding(const TitleFontInfo& info, bool acknowledged, const std::string& fontPath);

    // the font's .meta import.font.embeddingAcknowledged (an absent meta or key: false); takes the
    // font's absolute path or its .meta's
    bool titleFontEmbeddingAcknowledged(const std::filesystem::path& fontMetaOrFontPath);

    // 16 lowercase hex of the font file's content hash (the cache key and the rename's 8 hex)
    std::string titleFontHash(const std::vector<uint8_t>& bytes);

    struct TitleSubsetRequest {
        std::vector<uint8_t> bytes;
        int faceIndex = 0;
        std::vector<uint32_t> codepoints;
        bool renameReserved = true;     // R57: rename the subset of an OFL font with a Reserved Font Name
        std::string fontHash;           // titleFontHash(bytes); computed when empty
    };

    struct TitleSubsetResult {
        bool ok = false;
        std::vector<uint8_t> bytes;     // the font to ship (a single-face sfnt, also for a .ttc member)
        bool whole = false;             // the original bytes (fsType 0x0100)
        bool renamed = false;
        bool cached = false;            // read from the cache directory
        std::string family;             // the family the shipped font carries
        std::string error;
        double ms = 0.0;                // time spent (subsetting, or reading the cache)
        int glyphs = 0;                 // glyphs of the shipped font
    };

    class TitleFontSubsetter {
    public:
        // an empty cacheDir: no cache
        explicit TitleFontSubsetter(std::filesystem::path cacheDir);

        TitleSubsetResult subset(const TitleSubsetRequest& request);

        const std::filesystem::path& cacheDirectory() const { return cacheDir; }
        // the cache directory of a project
        static std::filesystem::path projectCacheDir(const std::filesystem::path& projectPath);

    private:
        std::filesystem::path cacheDir;
    };

    // one shipped font of licenses/THIRD-PARTY-FONTS.txt
    struct TitleFontCredit {
        std::string family;
        std::string file;               // the path in the build ('/' separated)
        std::string copyright;
        std::string license;
        std::string licenseUrl;
        bool ofl = false;
        bool subset = true;             // false: shipped whole
    };

    constexpr const char* TITLE_FONT_CREDITS_FILE = "THIRD-PARTY-FONTS.txt";

    TitleFontCredit titleFontCredit(const TitleFontInfo& info, const std::string& file, bool subset);
    // the file's text: per font (sorted by file) its family, file, copyright, licence, licence URL,
    // "subset" or "whole"; then the OFL 1.1 text once when any font is OFL
    std::string titleFontCreditsText(const std::vector<TitleFontCredit>& credits, const std::string& projectName);

}
