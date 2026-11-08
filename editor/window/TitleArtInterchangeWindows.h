// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R31, R56-R58)
// - Purpose: the editor windows of Title Art's interchange: the Import Title Art dialog and its
//   Import Report, the Title Designer's Export dialog and Photoshop style imports, and the Title
//   Art Inspector's languages line and import notes.
// - Thread-safety: main thread (drawn from the App's frame).

#pragma once

namespace jasperengine::editor {

    class Project;

    // Assets > Import Title Art...
    void openTitleArtImportDialog();
    // every frame, after the Title Designer
    void drawTitleArtInterchangeWindows(Project* project);
    // the Designer's toolbar item and the Inspector's section (from registerTitleInterchange)
    void registerTitleArtInterchangeUi();

}
