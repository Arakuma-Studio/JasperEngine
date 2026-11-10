// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R15, R20, R23, R24, R58)
// - Purpose: the editor's side of the Title Art runtime: the engine's scheduler resolves fonts,
//   images and SVGs through the project's asset database (GUID first, the editor's resolver),
//   takes Project Settings > Title Art (budgets per S56 platform group, the Edit mode budget,
//   Release Delay, Cache Budget), and keeps the editor drawing while a Realtime or Glyph Atlas
//   title renders in Edit mode (R24).
// - Thread-safety: main thread (the editor's frame).

#pragma once

namespace jasperengine::editor {

    class Project;

    // once a frame (App): true while a title has work (the caller requests another frame)
    bool syncTitleArtRuntime(Project* project);

}
