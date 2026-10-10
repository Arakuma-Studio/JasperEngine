// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_SOURCEFILES_H
#define COMPAT_SOURCEFILES_H

#include "Export.h"
#include "CompatTypes.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // The files one compile session reads (the HTML document, its stylesheets, imported
    // sheets, inline style blocks live in their HTML file): ids for SourceRef, and
    // offset -> line/column. Line starts are computed once per file; "\r\n", "\r" and "\n"
    // end a line; a column counts UTF-8 lead bytes from the line start, plus one.
    class JASPERENGINE_API SourceFiles {
    public:
        SourceFiles();
        ~SourceFiles();
        SourceFiles(SourceFiles&&) noexcept;
        SourceFiles& operator=(SourceFiles&&) noexcept;
        SourceFiles(const SourceFiles&);
        SourceFiles& operator=(const SourceFiles&);

        // Registers a file once per compile session; returns its id (>= 1). `text` is kept
        // as given: a caller strips the BOM before (offsets count after it). A path added
        // twice gets the id of the first registration (its text is not replaced).
        uint32_t add(std::string path, std::string text, SourceRef includedFrom = {});
        // the id of a registered path, 0 when none
        uint32_t find(std::string_view path) const;

        size_t count() const;                                         // registered files
        const std::string& path(uint32_t file) const;                 // "" for 0 or unknown ids
        std::string_view text(uint32_t file) const;                   // "" for 0 or unknown ids
        SourcePos position(uint32_t file, uint32_t offset) const;     // {0, 0} when unknown
        uint32_t line(uint32_t file, uint32_t offset) const;          // position().line, without the column
        SourceRef includedFrom(uint32_t file) const;                  // @import / <link> chain

        // the code points of [offset, offset + length) of a file (Diagnostic::length)
        uint32_t codePointLength(SourceRef ref) const;
        // the source text of a ref ("" when invalid)
        std::string_view slice(SourceRef ref) const;

        // "imported by ui/menu.css:3" for a file reached through @import/<link>, else ""
        std::string originText(uint32_t file) const;

    private:
        struct File;
        std::vector<std::unique_ptr<File>> files;
    };

    // Strips a UTF-8 byte order mark; true when one was there.
    JASPERENGINE_API bool stripUtf8Bom(std::string& text);
    // Replaces every invalid UTF-8 sequence (and NUL) by U+FFFD; returns the byte offsets of
    // the replacements in the result (HTMLUI-GEN-0003 is reported at the first).
    JASPERENGINE_API std::vector<uint32_t> sanitizeUtf8(std::string& text);

}

#endif //COMPAT_SOURCEFILES_H
