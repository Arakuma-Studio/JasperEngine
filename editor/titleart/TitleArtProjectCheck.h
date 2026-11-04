// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R23, R33, R58, §6.9)
// - Purpose: `jasperengine-editor check --titles`: every `.titleart` of a project (or those
//   `files` name, a baked PNG naming its document) checked without a GPU: the document
//   (title-parse-failed, title-version-newer), its references (title-asset-missing), missing
//   glyphs (title-glyph-missing) and the freshness of each output by its recipe
//   (title-bake-missing, title-bake-stale, title-recipe-invalid), each with the command that fixes
//   it. Asset hashes are cached by mtime and size in .jasperengine/titleart-check-cache.json.
// - Thread-safety: the CLI's main thread.

#pragma once

#include "util/ProjectChecks.h"

#include <filesystem>
#include <vector>

namespace jasperengine::editor {

    struct TitleArtProjectCheck {
        std::vector<ProjectChecks::Finding> findings;
        size_t documents = 0;
    };

    // `assetsDir`: the project's assets directory, project-relative ("" = the project root)
    TitleArtProjectCheck checkTitleArtFiles(const std::filesystem::path& projectRoot, const std::filesystem::path& assetsDir,
                                            const std::vector<std::filesystem::path>& files);

}
