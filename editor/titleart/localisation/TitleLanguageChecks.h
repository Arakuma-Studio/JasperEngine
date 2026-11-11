// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R11, R18)
// - Purpose: registers check_title's language checks (group `languages`, owner S87):
//   languages_complete, translation_stale, text_overflow, fit_scale_low, fit_without_box,
//   project_language_missing, translation_unused. Document checks: they read the document and
//   measure geometry (no render), so the Designer and `check --titles` run them headless.
// - Thread-safety: main thread.

#pragma once

namespace jasperengine::editor {

    // once; from registerBuiltinTitleChecks
    void registerTitleLanguageChecks();

}
