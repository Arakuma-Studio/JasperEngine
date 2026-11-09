// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R53, §6.1 Language.name)
// - Purpose: the built-in table of the 60 most used game languages (code, English name, native
//   name) the Edit Languages dialog searches and a Language with an empty `name` shows.
// - Thread-safety: an immutable table.

#pragma once

#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitleLanguageName {
        const char* code;
        const char* english;
        const char* native;
    };

    const std::vector<TitleLanguageName>& titleLanguageNames();
    // the English name of a code (the table's entry for it, else of its language subtag with the
    // region in parentheses: "German (AT)"), "" when unknown
    std::string titleLanguageEnglishName(const std::string& code);
    // "German (de)": the switcher's and the CSV header's form; the code alone when unknown
    std::string titleLanguageLabel(const std::string& code, const std::string& name = {});

}
