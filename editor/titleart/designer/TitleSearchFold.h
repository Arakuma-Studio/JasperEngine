// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R27)
// - Purpose: the one text fold of Title Art searches, so English and Japanese queries meet the
//   labels however they were typed: the Properties search (TitlePropertyIndex) and S84's Library
//   search (TitleLibrarySearch) both fold queries and texts with it, so they agree on what
//   matches.
// - Design: full-width ASCII becomes ASCII, U+3000 a space, half-width katakana full-width (a
//   following ﾞ or ﾟ composed: ｶﾞ -> ガ, ﾊﾟ -> パ), katakana hiragana (ボカシ finds ぼかし), ASCII
//   letters lower case, and every run of spaces, punctuation and symbols (search::isSeparator)
//   one space, trimmed. Everything else (kanji, hiragana, ー, other scripts, stray bytes) is
//   kept as it is. No designer dependency: S84 reuses it.
// - Thread-safety: pure.

#pragma once

#include <string>
#include <string_view>

namespace jasperengine::editor::titledesigner {

    std::string foldForSearch(std::string_view text);

}
