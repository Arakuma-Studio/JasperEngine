// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.4, 4.6)
// - Purpose: an import's in-memory result (import/VatImport.h: importVat) written into the project
//   as one undo step, with the baker's output rules, for the baker window's Import (and any other
//   front end that imports):
//   * the default output: "<export folder>/<asset>.vat" when the export is inside the assets
//     folder, else "assets/<asset>.vat";
//   * the existing-file rule (VatBakeSession's): a default path holding a .vat imported from the
//     same export (recipe source kind and path) is re-imported in place, keeping its GUID and its
//     hand edits (clip names, events, speeds, wraps, interpolate by name or in order); any other
//     existing file makes the import take a free name ("Wave 2.vat"); an explicit output that exists
//     is refused unless `overwrite` (its GUID kept);
//   * place none: WriteFilesCmd on the project history; beside: CreateVatCmd on the selected scene's
//     history (a new GameObject "<asset> (VAT)" at the origin, selected); crowd: the same with
//     `crowd` building the crowd commands (VatPlacement::crowd); replace is refused (an import has
//     no GameObject to replace).
// - Thread-safety: main thread (writes files, runs commands).

#pragma once

#include "command/type/CreateVatCmd.h"
#include "vat/VatBakeSession.h"
#include "vat/import/VatImport.h"

#include <string>

namespace jasperengine::editor::vat{

    struct VatImportWriteOptions{
        std::string output;                 // project-relative .vat; empty: the default
        bool overwrite = false;
        VatPlace place = VatPlace::None;
        // place crowd: the crowd commands (VatPlacement::crowd)
        std::function<bool(Project* project, uint32_t sceneId, Entity entity, std::vector<Command*>& commands, std::string& error)> crowd;
    };

    // "<export folder>/<asset>.vat" or "assets/<asset>.vat" (project-relative)
    std::string defaultImportOutput(Project* project, const VatImportDetection& detection);

    // where writeVatImport would write: false with `error` for an output it refuses
    bool plannedImportOutput(Project* project, const VatImportResult& result, const VatImportDetection& detection,
                             const VatImportWriteOptions& options, std::string& vatPath, bool& rewrite, std::string& error);

    // writes the import (one undo step, see the contract); `result.data` is encoded (its guid set)
    bool writeVatImport(Project* project, VatImportResult& result, const VatImportDetection& detection,
                        const VatImportWriteOptions& options, VatWriteResult& out, std::string& error);

}
