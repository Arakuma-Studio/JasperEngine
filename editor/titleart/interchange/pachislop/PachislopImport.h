// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R28-R31, §6.2)
// - Purpose: `edit_title {create: {from: "x.pslot.json"}}`: a Pachislop project file read,
//   migrated to v11, its embedded fonts and assets written to `<stem>_assets/` beside the new
//   document (sanitised names, SHA-256 reuse, GIF as PNG, WebP refused with a note), Pachislop's
//   builtin fonts taken from S84's font pack into `assets/TitleArt/Fonts/<pack id>/`, the v4
//   per-character scale compensated with the imported fonts, then mapped (PachislopMapper.h).
//   The files go back to the caller, which writes them with the document in one undo step.
// - The op `reimport_pachislop_3d {layer?}` re-maps the kept `pachislop_style3d` of 3D layers
//   imported without the 3D module (R30).
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtOps.h"

#include <string>

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::editor::pachislop {

    // the importer of create.from (what registerPachislopImport registers)
    bool importProjectFile(const TitleCreateRequest& request, TitleCreateResult& out, std::string& code, std::string& error);

    // v4 -> v5: every glyph of a text layer with a scaled character keeps its v4 position and
    // size (src/io/migrateText.ts, against TitleTextLayout with the document's fonts)
    bool compensateV4CharScale(titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets);

}

namespace jasperengine::editor {

    void registerPachislopImport();

}
