// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R51, R52)
// - Purpose: editing a title in one of its languages, the way Unity edits a prefab instance:
//   while a translation language is selected in the Title Designer, an edit of an overridable
//   member (text, font, face index, fallback fonts, shaping language, size, tracking, line height,
//   horizontal and vertical scale, vertical, align, features, variations, per-character and
//   per-line adjustments, position, fit) writes that language's Translation; other members
//   (colours, effects, rims) edit the shared document. Size edits become `size_scale` against the
//   source size and position edits an `offset` from the source position, so the source stays.
// - The routing rewrites op batches (TitleDesignerState::routed), so every gesture stays one undo
//   step, named "Set Translation (Title: de)".
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {
    class TitleDesignerState;
}

namespace jasperengine::editor {

    // `ops` (one op or a list) with the edits of overridable members in `language` written to its
    // Translations of `doc`'s layers; `language` "" or the source returns them unchanged
    titleart::Json routeTitleLanguageOps(const titleart::TitleArtDoc& doc, const std::string& language, const titleart::Json& ops);
    // a field path of a text layer a Translation overrides ("size", "fit.mode", "chars.3.rotation")
    bool titleFieldIsOverridable(const std::string& path);
    // the overrides `layer` has in `language` (Translation members present beyond the text)
    std::vector<std::string> titleLayerOverrides(const titleart::Json& layer, const std::string& language);
    // the op that copies a language's override into the source layer (Apply to Source), null when none
    titleart::Json titleApplyToSourceOp(const titleart::TitleArtDoc& doc, const std::string& layerId, const std::string& language,
                                        const std::string& member);

    // Next / Previous Language (the source, then each language, wrapping)
    void titleDesignerCycleLanguage(titledesigner::TitleDesignerState& state, int step);
    // the Title Designer's language UI: the toolbar switcher, Edit Languages, Compare Languages,
    // the Translations panel, the Language section and the canvas overlay
    void registerTitleDesignerLanguages();
    // the Translations panel (TitleDesignerTranslations.cpp), from registerTitleDesignerLanguages
    void registerTitleDesignerTranslations();

}
