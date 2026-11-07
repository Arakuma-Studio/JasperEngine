// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S81's built-in data (docs/specs/S81-title-art-runtime.md R53, R57), embedded in the editor from
// editor/resources/titleart by TitleArtEditorSources.cmake, so an installed editor needs no data
// folder for them.

#pragma once

#include <string_view>

namespace jasperengine::editor {

    // charsets/joyo-kanji.txt: '#' comment lines, then the 2,136 Joyo Kanji
    std::string_view titleJoyoKanjiText();
    // OFL-1.1.txt: the SIL Open Font License 1.1, which the font credits carry for OFL fonts
    std::string_view titleOflText();

}
