// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.4-4.6, 6, 7)
// - Purpose: what the agent-facing front ends share, the MCP tool bake_vertex_animation and the
//   bake-vat CLI (the baker window may use it too):
//   * routeVatSource: a source given as files (a model, "model#Clip", .anim files, one
//     .poseanim, a Houdini VAT 3 / OpenVAT export path) -> a baker VatSourceSpec or an importer
//     path. GameObjects are the caller's (its own addressing): the route then only tells what the
//     files are, and `on` names what .anim / .poseanim files play on (a model or prefab file;
//     empty: the caller gives a GameObject, or a .poseanim's preview model is used).
//   * imports written as the baker window writes them (vat/VatImportWrite.h: the default output
//     next to the export, the existing-file rule, one undo step): planVatImport / writeImportedVat
//     add what a front end gives besides, the scene to place in and the crowd (crowdPlacement);
//     reimportVat is the Rebake of an import recipe (written over the .vat: GUID and hand edits
//     kept by the existing-file rule).
// - Thread-safety: main thread (reads the project, runs commands); importVat itself may run on
//   any thread before writeImportedVat.
// - Paths: project-relative with '/' in and out; absolute paths for import sources outside the
//   project are accepted only when the caller allows them (the CLI).

#ifndef VAT_FRONT_END_H
#define VAT_FRONT_END_H

#include "vat/VatBakeSession.h"
#include "vat/VatBakeTypes.h"
#include "vat/VatImportWrite.h"
#include "vat/import/VatImport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{
    class Project;
}

namespace jasperengine::editor::vat{

    struct VatSourceRoute{
        bool import = false;
        // baker sources: kind, path (the `on` file / the model), anims; clips from "#Clip" suffixes
        VatSourceSpec source;
        std::vector<std::string> clips;
        // .anim / .poseanim given without `on` and without a preview model: the caller must
        // name a GameObject (source.scene / sceneId + path), else it is an error
        bool needsTarget = false;
        // imports
        std::string importPath;             // absolute
        VatImportDetection detection;
    };

    // `files` the source entries (project-relative, or absolute inside the project; absolute
    // anywhere for imports when `allowOutside`). False with `error` (kind invalid_source, a fix
    // naming what is accepted).
    bool routeVatSource(Project* project, const std::vector<std::string>& files, const std::string& on, bool allowOutside,
                        VatSourceRoute& out, VatIssue& error);

    // a project file argument -> project-relative ('/'), false with `error` when it is outside
    // the project or (mustExist) missing
    bool projectRelativeFile(Project* project, const std::string& given, bool mustExist, std::string& relative, std::string& error);

    // a .vat file's data; false with `error`
    bool readVatAsset(Project* project, const std::string& vatPath, jasperengine::vat::VatData& out, std::string* error);
    bool isImportRecipe(const jasperengine::vat::VatRecipe& recipe);

    struct VatImportPlan{
        std::string output;                 // project-relative .vat; empty: the default (see the contract)
        bool overwrite = false;
        VatPlace place = VatPlace::None;    // replace needs a GameObject: refused for imports
        VatCrowd crowd;
        uint32_t sceneId = 0;               // the scene to place in (0: the selected scene)
    };

    // where writeImportedVat would write (project-relative) and whether it rewrites an existing file
    bool planVatImport(Project* project, const VatImportResult& result, const VatImportDetection& detection,
                          const VatImportPlan& options, std::string& vatPath, bool& rewrite, std::string& error);
    // writes a successful import (one undo step); `result` keeps its data (guid/mesh/layout hash set)
    bool writeImportedVat(Project* project, VatImportResult& result, const VatImportDetection& detection,
                        const VatImportPlan& options, VatWriteResult& out, std::string& error);
    // Rebake of an imported .vat: its recipe imported again and written over it (GUID and edits
    // kept). `report` (optional) gets the import's report. False with `error`.
    bool reimportVat(Project* project, const std::string& vatPath, VatWriteResult& out, std::string& error,
                     VatImportReport* report = nullptr);

}

#endif //VAT_FRONT_END_H
