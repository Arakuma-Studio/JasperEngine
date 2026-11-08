// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R1-R3, R8, R21, D6, §6.2)
// - Purpose: the language ops of edit_title and the Title Designer (one ops grammar):
//   add_language, remove_language, set_source_language, set_translation, revert_translation,
//   mark_reviewed and import_strings (CSV / XLIFF, StringsExchange.h). They run inside S80's
//   batch (TitleArtOps::registerOp): all or nothing, one undo step.
// - add_language sets `source_language` when empty (its `source`, else the project default, else
//   "en"), makes the missing text keys and the string namespace, and gives every text layer
//   without a fit box `fit {mode: shrink, box: measured extent + 10 %}` (R8), so a new
//   translation fits by default.
// - Setting a Translation's text stamps `source_hash` with the current source text (R2).
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtOps.h"

#include <string>

namespace jasperengine::editor {

    void registerTitleLanguageOps();

    // R3: the layer's text key (made from its name, unique in the document) and the document's
    // namespace (from the file stem): filled where empty; true when anything changed
    bool ensureTitleTextKeys(TitleOpContext& ctx);
    // "<string_namespace>.<text_key>" (D3)
    std::string titleStringKey(const titleart::Json& root, const titleart::Json& layer);
    // "logo" of ".../logo.titleart" as a namespace ([a-z0-9_]{1,48})
    std::string titleNamespaceFromPath(const std::string& documentPath);

}
