// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R43, R44, §6.15)
// - Purpose: S84's part of list_title_library and edit_title: the library kinds (looks,
//   color_schemes, templates) and the pack rows of `fonts` in tiers (the catalogue, rows with a
//   ready `use` op, one item's detail, a numbered sheet of items rendered on the agent's text or
//   layer), and edit_title's library-only batches and `create.template`.
// - Sizes: catalogue <= 600 characters, a row <= 120, a detail <= 1,600, a sheet <= 9 cells and
//   <= 1024 x 576 pixels. A sheet renders through the library's preview (the same ops a click
//   commits, nothing imported) on S80's renderer, one cell per editor frame, as a job.
// - Thread-safety: main thread.

#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"
#include "titleart/TitleArtOps.h"

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp::titletools {

    // answers list_title_library for kinds looks / color_schemes / templates; false for other kinds
    bool listLibraryKind(const CallPtr& call);
    // the catalogue's library part ({looks: {total, available, families}, color_schemes, templates})
    Json libraryCatalogue();
    // the pack's font rows matching `query` ({id, family, style, scripts, pack: true})
    std::vector<Json> packFontRows(const std::string& query);
    // edit_title without `path` whose ops are all library ops; false when `args` is not one
    bool editLibraryOnly(const CallPtr& call);
    // edit_title {create: {template | look, language?, text?}}: the new document and the files its
    // fonts and environments add (one step, "Create Title Art from Template <name>"); false + `error`
    bool createFromLibrary(Project* project, const Json& create, titleart::TitleArtDoc& out, std::vector<TitleOpFile>& files,
                           std::string& undoName, std::string& error);

}
