// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R53, R54, R56, R57)
// - Purpose: the fonts an export ships for its Realtime and Glyph Atlas titles. Per font every
//   shipped runtime document reads (text layers' fonts and fallbacks): the union of the
//   documents' Characters, the instances' Additional Characters and their text overrides is
//   subset (TitleFontSubsetter, editor-only HarfBuzz), written next to the font in the build as
//   "<stem>.subset-<font hash 8><characters hash 8><ext>", and the build's copies of those
//   documents name it. The original leaves the build when only Title Art documents reference it
//   (the asset index's edges), so a Text component or an HTML UI page that uses it keeps it.
// - fsType (R56): Restricted (without the font's acknowledgement) or bitmap-only stops the
//   export with the check's sentence; No Subsetting ships the font whole with a warning. A font
//   collection (.ttc/.otc) ships whole: its subset would be a single face and the documents'
//   face indices would change.
// - Thread-safety: main thread (the asset database and index).

#pragma once

#include "titleart/runtime/TitleArtRuntimeExport.h"
#include "titleart/runtime/TitleFontSubsetter.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct TitleArtFontShipResult {
        bool ok = true;
        std::string error;                      // the first E-TITLEART-FONT-* sentence
        std::vector<std::string> shipped;       // one line per shipped font (subset or whole)
        std::vector<std::string> warnings;
        std::vector<TitleFontCredit> credits;   // licenses/THIRD-PARTY-FONTS.txt
        size_t bytesBefore = 0;                 // the shipped fonts' original sizes
        size_t bytesAfter = 0;                  // what ships of them
    };

    // `assetsDst`: the build's assets root (the documents already copied into it)
    TitleArtFontShipResult shipTitleArtFonts(Project* project, const TitleArtExportPlan& plan, const std::filesystem::path& assetsDst);

}
