// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R37, R38, R40, §6.6)
// - Purpose: export_title's `png` format: one PNG per language on its trimmed frame (or the
//   shared one with `registered`), or with `parts` each visible layer rendered alone on that
//   frame plus `<stem>[.<code>].parts.json`; `split_titles` splits each 2D title into rim N
//   (alone, the face hidden, its width cumulative), the face and `fx` (the whole layer), each part
//   with its own part effects only. The PSD writer uses the same parts plan.
// - Thread-safety: main thread.

#pragma once

#include "titleart/interchange/TitleExport.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitlePart {
        std::string layerId;
        std::string path;           // "Logo/Title#1"
        std::string name;
        std::string fileStem;       // the layer name's slug, unique in the export
        std::string part;           // "" (the layer), "rim_1"..., "face", "fx"
        int rimIndex = -1;          // rim_N: the rim's index in style.rims
        std::string blend = "normal";
        double opacity = 1.0;
        int order = 0;              // bottom first
        bool hidden = false;        // split parts other than fx, layers hidden in the document
    };

    // the parts of a (resolved) document, bottom first
    std::vector<TitlePart> titleExportParts(const titleart::TitleArtDoc& doc, bool splitTitles);
    // the document that draws one part (to render with the part's layer isolated): the layer at
    // full opacity and normal blend, and for split parts only its rim, face or whole layer
    titleart::TitleArtDoc titleExportPartDoc(const titleart::TitleArtDoc& doc, const TitlePart& part);

    void registerTitlePngExport();
    // export_title's ktx2 format (R43)
    void registerTitleKtx2Export();

}
