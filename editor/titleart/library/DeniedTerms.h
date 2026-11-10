// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R38, §6.7.3, AC18)
// - Purpose: the denied terms (franchise, brand, product, company and creator names) no built-in
//   library item may carry, and the matcher the Pachislop port, `title-library verify` and the
//   gate share. The list is tools/titleart/denied_terms.txt, embedded at build time
//   (title_denied_terms.h, editor/titleart/TitleArtEditorSources.cmake); tests and tools may
//   replace it with setDeniedTermsText.
// - File: one term per line, `#` starts a comment, blank lines ignored; terms are kept lowercase
//   (ASCII folding) in file order, duplicates dropped.
// - Matching: a term with Japanese script (kana, kanji) matches as a substring; any other term
//   matches whole words, case-insensitive over ASCII: the text and the term are folded to
//   lowercase words separated by single spaces (every run of ASCII characters that are not
//   letters or digits is one separator; bytes >= 0x80 are word characters), and the term's words
//   must appear consecutively there. "Hiro-Gold" matches "hiro gold"; "segal" does not match
//   "sega".
// - Thread-safety: the list is built on first use; setDeniedTermsText is for startup and tests
//   (not concurrent with matching).

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    // the terms of a denied_terms.txt text (comments and blank lines removed, lowercase, unique)
    std::vector<std::string> parseDeniedTerms(const std::string& text);
    // replaces the list (the embedded file until then)
    void setDeniedTermsText(const std::string& text);
    const std::vector<std::string>& deniedTerms();

    // true when `text` holds a denied term; `term` (optional) receives the first one found, in
    // list order
    bool containsDeniedTerm(std::string_view text, std::string* term);

    // one denied term found in a JSON value: the JSON path of the string ("name.en",
    // "layers[2].text", a member name ends in "{key}") and the term
    struct DeniedTermHit {
        std::string path;
        std::string term;
    };
    // every string value and member name of `value`, depth first
    std::vector<DeniedTermHit> findDeniedTerms(const titleart::Json& value, const std::string& path = {});

}
