// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R32, R36, R37, R46, R47, AC20)
// - Purpose: the `title-library` subcommands besides the port's: list, verify, lint, gen-docs (no
//   GPU) and bake-thumbnails, review-sheet, calibrate (GPU: rendered by S80's renderer in a one-shot
//   headless editor on a scratch project, driven through `batch` and render_title / check_title,
//   so the CLI and the MCP tools render the same way).
// - A scratch project holds a base document and copies of the pack's fonts and environments at
//   the paths a library apply names (assets/TitleArt/...), so every item renders with its own fonts.
// - Exit codes: 0 ok, 1 findings (stale thumbnails, verify findings, a failed render, over budget),
//   2 usage or I/O errors.
// - Thread-safety: the CLI's main thread.

#pragma once

#include "TitleLibraryFormats.h"

#include <string>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    int runListCommand(const std::vector<std::string>& args);
    int runVerifyCommand(const std::vector<std::string>& args);
    int runLintCommand(const std::vector<std::string>& args);
    int runGenDocsCommand(const std::vector<std::string>& args);
    // the new templates from their recipes (tools/titleart/templates/*.json: identity, texts and
    // edit_title ops on an empty document), written as canonical .titletemplate files (--check compares)
    int runBuildTemplatesCommand(const std::vector<std::string>& args);
    // one recipe as its template (a fixed id sequence and a guid from the id: the same bytes on every
    // run); false with `error` when an op fails
    bool buildTemplateFromRecipe(const titleart::Json& recipe, TitleTemplate& out, std::string& error);
    int runBakeThumbnailsCommand(const std::vector<std::string>& args, const char* executable);
    int runReviewSheetCommand(const std::vector<std::string>& args, const char* executable);
    int runCalibrateCommand(const std::vector<std::string>& args, const char* executable);

}
