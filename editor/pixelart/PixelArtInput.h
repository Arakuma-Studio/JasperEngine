// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The validator's input from a Project (docs/specs/S42-pixel-art-mode.md D3): the live scenes with
// their unsaved edits, the closed scenes of the build list read from their files, the textures'
// import settings and image headers. The CLI loads the project headless (check's engine
// categories) and calls the same function, so a saved project gives the same input both ways.
// Main thread: it reads the scenes.

#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelProblem.h"

#include <vector>

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::pixelart {

    // errors: a scene or path of the scope that does not exist (kind not_found, field scenes/paths)
    PixelArtCheckInput inputFromProject(Project& project, const Scope& scope, std::vector<PixelProblem>& errors);

    // the scenes the scope names, in build order (start scene first); closed scenes have a null
    // scene and are read from their files by inputFromProject
    std::vector<uint32_t> scopeScenes(Project& project, const Scope& scope, std::vector<PixelProblem>& errors);

    // the project-relative path a finding names a scene by
    std::string scenePathOf(const Project& project, const SceneProject& scene);

}
