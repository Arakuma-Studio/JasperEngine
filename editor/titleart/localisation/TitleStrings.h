// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R20-R23, D3, §6.6)
// - Purpose: a title's texts out to translators and back: CSV (RFC 4180; UTF-8 with BOM, comma,
//   CRLF on export; BOM optional, `,` `;` or tab detected from the header, quoted newlines, and
//   Unity Localization headers "German (de)" on import) and XLIFF (1.2 export, one file per target
//   language with one <file> per document; 1.2 and 2.0 import). Content is never altered (no
//   formula escaping, §9).
// - Rows: one per text layer with `translate` true and a text, keyed `<string_namespace>.<text_key>`
//   (D3, the key a later project-wide string table reuses); `context` is the translator note.
//   Several documents add a `file` column (project-relative); documents with different source
//   languages name the source column `source` and add `source_language`.
// - Import (the op import_strings, one undo step): rows route by `file`, else by the key's
//   namespace; an unknown key is reported (unknown_key), an unknown language column too
//   (unknown_language) unless add_languages; an empty cell or the current value changes nothing;
//   a row whose source text differs from the document's imports as needs_review (source_changed);
//   else `translated` (or the op's status) with source_hash stamped. An XLIFF target with inline
//   markup is refused per unit (inline_markup).
// - Parsers bound every read by the input (untrusted files, R47).
// - Thread-safety: pure functions; the op registers on the main thread.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    struct TitleStringsDocument {
        std::string file;                   // project-relative ("assets/titles/logo.titleart")
        const titleart::TitleArtDoc* doc = nullptr;
    };

    // one row of a strings file
    struct TitleStringRow {
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

    struct TitleStringsNote {
        std::string code;                   // unknown_key, unknown_language, source_changed, inline_markup, bad_row
        std::string key;
        std::string language;
        std::string message;
        int line = 0;
    };

    struct TitleStringsFile {
        std::vector<std::string> languages;     // the target language columns (CSV) or files (XLIFF), canonical
        std::vector<TitleStringRow> rows;
        std::vector<TitleStringsNote> notes;    // what the parse refused (inline markup, malformed rows)
    };

    // ---- export ----

    // `languages`: the target codes (empty: every translation language of the documents)
    std::string exportTitleStringsCsv(const std::vector<TitleStringsDocument>& docs, const std::vector<std::string>& languages);
    // one XLIFF 1.2 file per target language: (code, text)
    std::vector<std::pair<std::string, std::string>> exportTitleStringsXliff(const std::vector<TitleStringsDocument>& docs,
                                                                             const std::vector<std::string>& languages);

    // ---- import ----

    bool parseTitleStringsCsv(const std::string& text, TitleStringsFile& out, std::string& error);
    bool parseTitleStringsXliff(const std::string& text, TitleStringsFile& out, std::string& error);
    // by content: XML -> XLIFF, else CSV
    bool parseTitleStrings(const std::string& text, const std::string& format, TitleStringsFile& out, std::string& error);

    // the rows of `file` that are this document's: its `file` column, else its namespace
    bool titleStringRowIsFor(const TitleStringRow& row, const std::string& documentFile, const titleart::TitleArtDoc& doc);

    // the op import_strings (S87 §6.2)
    void registerTitleStringsOps();
    // export_title's csv and xliff formats
    void registerTitleStringsExport();

}
