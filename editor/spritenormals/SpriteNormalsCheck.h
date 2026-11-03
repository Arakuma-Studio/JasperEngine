// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// `check --normals` (docs/mcp/checks.md "What the normal map check runs"): the generated sprite
// normal maps whose source changed or went missing (their jasper.spritenormals recipe), and the
// normal maps scenes and prefabs use that don't match their colour texture (size, frames,
// filter), look like DirectX or aren't normal maps, or that no Light 2D can show (height 0).
// Pure parsing, no engine: PNG headers (the chunks before IDAT), the scene and prefab YAML (keys
// as editor/Stream.cpp writes them), and pixels only for a normal map in use without a recipe.
//
// Cache: .jasperengine/normals-check-cache.json (written when .jasperengine/ exists): per PNG
// {mtime, size, recipe (parsed, or null), recipe_error?, width, height, analysis?}, per source
// {mtime, size, sha1}, and the reverse index source -> [generated PNGs] rebuilt from the cached
// recipes. A second run on an unchanged project decodes and hashes nothing.

#include "util/ProjectChecks.h"

#include <filesystem>
#include <vector>

namespace jasperengine::editor::spritenormals {

    struct NormalsCheck {
        std::vector<ProjectChecks::Finding> findings;   // tool "normals", code normals-*, suggestion set
        size_t recipes = 0;                             // PNGs with a recipe checked
        size_t maps = 0;                                // normal maps in use checked
    };

    // `files` empty: the whole project (recipes, then the scenes' and prefabs' normal maps).
    // Otherwise (absolute paths): an image with a recipe is checked as a generated map, another
    // image as a source (the maps whose recipe names it, through the cache index, plus the
    // default-name probe); no scene findings.
    NormalsCheck checkSpriteNormals(const std::filesystem::path& projectRoot, const std::vector<std::filesystem::path>& files);

    // The generated PNGs the cache's reverse index lists for `source` (absolute in and out);
    // empty without a cache. Reads the cache only (the file-sync hook: no scan).
    std::vector<std::filesystem::path> cachedMapsOf(const std::filesystem::path& projectRoot, const std::filesystem::path& source);

}
