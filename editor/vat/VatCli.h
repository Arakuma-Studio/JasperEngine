// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 7)
// - Purpose: the Vertex Animation command line: `jasperengine-editor bake-vat` (bake a model, its
//   clips, .anim files on a model / prefab / GameObject, a .poseanim, a GameObject, or import a
//   Houdini VAT 3 / OpenVAT export; rebake a .vat) and the `.vat` part of `check --vat`.
// - bake-vat is the bake_vertex_animation MCP tool without a window (the headless engine pattern of
//   place-physics: the project loaded, the engine started with its game events paused and
//   synchronous loading): the source routed by vat/VatFrontEnd (absolute export paths outside the
//   project allowed), baked through VatBakeSession (check -> run -> write) or imported through
//   importVat + writeImportedVat, rebakes through VatBakeSession::loadRebake / importSpecFromRecipe + importVat. The
//   files are always files only (place none): encoded first, then written all or nothing by one
//   WriteFilesCmd. Without --output the default name next to the source is written (the
//   existing-file rule of spec 4.4: the bake of the same source there is rewritten, GUID kept;
//   another file there makes it take a free name); an explicit --output that exists needs
//   --overwrite. --dry-run writes nothing and prints the report (the MCP preview's, without the
//   image) and where it would write.
// - Output: text (a summary, warnings with their fixes as flags) or with --format json exactly one
//   object on stdout: {ok, dry_run, source, report, output: {vat, rewrite}, written: {vat, mesh,
//   files, guid, rewritten}, errors: [{kind, message, fix}]} (keys present when they apply). Errors
//   print "bake-vat: <kind>: <message> (fix: <fix>)" on stderr in text mode. The engine's log goes
//   to stderr (nowhere with -q), never to stdout.
// - Exit codes: 0 done (warnings allowed), 1 the bake, import or write failed (check's errors,
//   the importer's, output_exists...), 2 usage error (unknown option, bad value, missing --project
//   or --source, a source that is not a bake source, an unknown scene or GameObject, a project
//   that doesn't load).
// - check --vat (checkVatAssets): every .vat of the project, or the ones given: the format
//   (vat-invalid error at line:col, vat-format warnings with JSON paths), checkVatStaleness's
//   kinds (vat-source-changed / vat-source-missing warnings, vat-mesh-mismatch /
//   vat-layout-mismatch errors) and the device limit of the recipe's preset (vat-device-limit).
//   Findings are ProjectChecks findings, tool "vat", code = the kind, suggestion = the fix.
// - Paths: arguments are absolute, relative to the current directory (when the file is there),
//   else relative to the project.
// - Thread-safety: main thread; one command per process.

#ifndef VAT_CLI_H
#define VAT_CLI_H

#include "util/ProjectChecks.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace jasperengine::editor{
    class Project;
}

namespace jasperengine::editor::vat{

    // argv[0] is the subcommand. The result goes to `out`, problems and progress to `err`.
    int runBakeVat(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);
    void printBakeVatUsage(const std::string& commandName, std::ostream& out);

    struct VatCheck{
        std::vector<ProjectChecks::Finding> findings;
        size_t assets = 0;      // .vat files checked
    };
    // `files` (absolute .vat paths) empty: every .vat of the project (outside skipped directories).
    // `project` loaded (its path is the root of the recipe's paths).
    VatCheck checkVatAssets(Project* project, const std::vector<std::filesystem::path>& files);

    // The .vat files `--files` names, without the engine: a .vat itself, the .vat of a .vat.glb,
    // and every project .vat whose recipe read one of the other files (an edited source model
    // reports the bakes made from it). Absolute paths in and out, each once.
    std::vector<std::filesystem::path> vatAssetsForFiles(const std::filesystem::path& projectRoot,
                                                         const std::vector<std::filesystem::path>& files);

}

#endif //VAT_CLI_H
