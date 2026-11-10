// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R25-R32, R53, R62; S82 R4, R28, §6.8)
// - Purpose: S81's part of the Title Designer, registered through S82's hooks: the Document page's
//   Runtime section (R62's Characters UI: presets, Custom, Custom File, the document's own
//   characters, the count, Check Characters with its per-font subset estimate and "Add to Custom"
//   fixes, and the Glyph Layer) and Preview As Glyph Atlas, which shows what that Render Mode
//   ships: a real Glyph Atlas title (TitleArtRuntime, run by S81's own scheduler) on the committed
//   document, made again when the file changes and dropped when the mode is left.
// - Every edit is an S80 `set` of `runtime.*` through TitleDesignerState (one named step).
// - Thread-safety: main thread.

#pragma once

namespace jasperengine::editor {

    // once, with the Title Designer's other extensions (idempotent)
    void registerTitleArtRuntimeDesigner();

}
