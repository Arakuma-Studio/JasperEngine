// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/game-ui-localization.md D6; S87 R20-R23, §6.6)
// - Purpose: texts out to translators and back, for String Tables and titles alike (lifted from
//   TitleStrings, whose functions wrap these): CSV (RFC 4180; UTF-8 with BOM, comma, CRLF on
//   export; BOM optional, `,` `;` or tab detected from the header, quoted newlines, and Unity
//   Localization headers "German (de)" on import) and XLIFF (1.2 export, one file per target
//   language with one <file> per document; 1.2 and 2.0 import). Content is never altered.
// - Rows are keyed by the global key (`<namespace>.<key>`); `context` is the translator note and
//   `max_chars` XLIFF's maxwidth. Several documents add a `file` column (project-relative);
//   documents with different source languages name the source column `source` and add
//   `source_language`.
// - An XLIFF target with inline markup is refused per unit (inline_markup).
// - Parsers bound every read by the input (untrusted files).
// - Thread-safety: pure functions.

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    // one row of a strings file
    struct StringRow {
        std::string file;                   // "" when the file has no file column
        std::string key;
        std::string context;
        int maxChars = 0;
        std::string sourceLanguage;
        std::string source;
        std::map<std::string, std::string> targets;     // code -> text ("" cells kept out)
        std::map<std::string, std::string> states;      // XLIFF: code -> translated | needs_review
        int line = 0;                       // where it starts (1-based)
    };

    struct StringsNote {
        std::string code;                   // unknown_key, unknown_language, source_changed, inline_markup, bad_row, bad_message
        std::string key;
        std::string language;
        std::string message;
        int line = 0;
    };

    struct StringsFile {
        std::vector<std::string> languages;     // the target language columns (CSV) or files (XLIFF), canonical
        std::vector<StringRow> rows;
        std::vector<StringsNote> notes;         // what the parse refused (inline markup, malformed rows)
    };

    // what one document exports: its units with their translations
    struct StringsExportUnit {
        std::string key;                    // the global key
        std::string context;
        std::string source;
        int maxChars = 0;
        struct Target {
            std::string text;
            bool needsReview = false;
        };
        std::map<std::string, Target> targets;  // code -> translation (absent: none)
    };

    struct StringsExportDoc {
        std::string file;                   // project-relative
        std::string sourceLanguage;
        std::vector<std::string> languages;     // its translation languages, in order
        std::vector<StringsExportUnit> units;
    };

    // ---- export ----

    // `languages`: the target codes (empty: every translation language of the documents)
    std::string exportStringsCsv(const std::vector<StringsExportDoc>& docs, const std::vector<std::string>& languages);
    // one XLIFF 1.2 file per target language: (code, text)
    std::vector<std::pair<std::string, std::string>> exportStringsXliff(const std::vector<StringsExportDoc>& docs,
                                                                        const std::vector<std::string>& languages);

    // ---- import ----

    bool parseStringsCsv(const std::string& text, StringsFile& out, std::string& error);
    bool parseStringsXliff(const std::string& text, StringsFile& out, std::string& error);
    // by content: XML -> XLIFF, else CSV
    bool parseStrings(const std::string& text, const std::string& format, StringsFile& out, std::string& error);

}
