// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A String Table's translations to and from translators' files (docs/specs/game-ui-localization.md
// R28, R33, D6), shared by edit_string_table and the Localization Tables window's Import and Export:
// rows route by `file`, else by the key's namespace; a title's row, an unknown key or language and a
// message that does not parse are notes; a row whose source text changed imports as needs_review.

#pragma once

#include "localization/StringFiles.h"
#include "localization/StringTableDocument.h"

#include <string>
#include <vector>

namespace jasperengine::editor::l10n {

    // the rows of `file` into `set` (in memory); `changed` counts the translations written
    std::vector<StringsNote> importStrings(StringTableSet& set, const StringsFile& file, bool addLanguages, int& changed);
    // the set as StringFiles exports it (`languages` empty: every translation language)
    StringsExportDoc exportDocOf(const StringTableSet& set, const std::vector<std::string>& languages);

    // Extract to Table (R28): an entry with `text` (its key made from the text, "Options" ->
    // "text.options") added to `set`, and data-l10n-id="<ns>.<key>" written on the element of
    // `html` (a document's source) whose whole text it is. `key` gets the global key; false with
    // `error` when no element holds exactly that text or it is keyed already
    bool extractText(StringTableSet& set, std::string& html, const std::string& text, std::string& key, std::string& error);

}
