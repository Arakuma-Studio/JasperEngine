// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Using a named palette in an edit (docs/specs/S43-palettes.md R7): the Palette Swap source and
// targets, the Palette Quantize palette and the presets that name a built-in. A built-in the
// project has no copy of yet gets one, written as a step of the project history just before the
// edit that stores its path; linkPaletteCopy then makes the two one undo step (the MCP tools need
// not: a call's steps are folded and linked by McpUndo).

#include "pixelart/PaletteAssets.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    // Resolves `name` (resolvePaletteName: a .palette path or "builtin:<id>"; never the Default
    // Palette: an empty name is an error here) and writes a built-in's copy when the project has
    // none. `wroteCopy` says a project history step was made. False with `error` (the
    // resolution's message, or why the copy could not be written).
    bool usePalette(Project& project, const std::string& name, ResolvedPalette& out, bool& wroteCopy, std::string& error);

    // The project history's newest step (the copy) and the scene history's newest step (the
    // edit that stores it) undo and redo together
    void linkPaletteCopy(Project& project, uint32_t sceneId);

}
