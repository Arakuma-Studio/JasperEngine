// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R31, R73)
// - Purpose: S81's checks in S80's registry: `glyph_atlas` lists what a document's effects and
//   style members do in Glyph Atlas (unsafe: skipped, with fix ops; approximate: info) and whether
//   its text falls back to Realtime; it needs no instance and no GPU (check --titles runs it).
// - Thread-safety: main thread (registration), pure checks.

#pragma once

namespace jasperengine::editor {

    // once (S80's registerBuiltinTitleChecks calls it after its own)
    void registerTitleArtRuntimeChecks();

}
