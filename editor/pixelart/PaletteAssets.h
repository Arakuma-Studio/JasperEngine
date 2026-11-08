// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The project's palettes (docs/specs/S43-palettes.md R4b, R7): listing them, the Default
// Palette, and the one way a palette is named anywhere (asset fields, edit_palette and
// apply_palette arguments, the presets, the window): a .palette path, or "builtin:<id>", which
// resolves through the project's copy of that built-in (Assets/Palettes/<id>.palette, made on
// first use and found again by its source {kind: builtin, id}). What gets stored is always the
// project's path; the runtime never reads an editor resource.

#include "pixelart/PixelProblem.h"
#include "pixelart/Palette.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    struct ProjectPalette {
        std::string path;        // project-relative ("assets/palettes/hero.palette")
        std::string assetPath;   // relative to the assets directory, as materials and settings store it
        jasperengine::pixelart::Palette palette;
    };

    // every readable .palette under the assets directory, by path
    std::vector<ProjectPalette> listProjectPalettes(const Project& project);

    // the project's copy of a built-in (source.kind builtin, that id), or empty
    std::string findBuiltinCopy(const Project& project, const std::string& id);
    // where a new copy of a built-in goes: Palettes/<id>.palette under the assets directory (a
    // free name when another palette holds that one), project-relative
    std::string builtinCopyPath(const Project& project, const std::string& id);

    struct ResolvedPalette {
        bool ok = false;
        jasperengine::pixelart::Palette palette;
        std::string path;           // project-relative
        std::string assetPath;      // assets-relative
        bool builtin = false;       // named as a built-in
        // a built-in with no copy yet: write copyText at path (one undo step with the edit
        // that uses it), then store assetPath
        bool needsCopy = false;
        std::string copyText;
        PixelProblem error;
    };

    // "builtin:<id>" (a bare built-in id too when allowBareBuiltin), a .palette path (project- or
    // assets-relative), or "" for the project's Default Palette
    ResolvedPalette resolvePaletteName(const Project& project, const std::string& name, bool allowBareBuiltin = true);

    // the Default Palette (Project Settings, palettes.default), project-relative; empty: none
    std::string defaultPalettePath(const Project& project);

}
