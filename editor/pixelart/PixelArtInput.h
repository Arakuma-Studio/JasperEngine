// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The validator's input from a Project (docs/specs/S42-pixel-art-mode.md D3): the live scenes with
// their unsaved edits, the closed scenes of the build list read from their files, the textures'
// import settings and image headers. The CLI loads the project headless (check's engine
// categories) and calls the same function, so a saved project gives the same input both ways.
// Main thread: it reads the scenes. What a closed scene's file holds and what a texture's meta and
// header say are kept between calls while the files are unchanged (R16: a warm check reads no
// scene file again); an asset batch that adds, moves or removes files, or changes a prefab,
// forgets them.

#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelProblem.h"

#include <functional>
#include <string>
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

    // A later spec's drawables (S52's Sprite Batch, S50's Tilemap Layers): called for every scene
    // read, it appends what that scene draws; `base` gives a drawable of a GameObject with its
    // scene, entity path and Transform filled in. A known id replaces the collector.
    using DrawableBase = std::function<PixelArtDrawable(uint32_t entity, DrawableKind kind)>;
    using DrawableCollector = std::function<void(const SceneProject& scene, const DrawableBase& base, std::vector<PixelArtDrawable>& out)>;
    void registerDrawableCollector(std::string id, DrawableCollector collector);
    void unregisterDrawableCollector(const std::string& id);

    // drops what is kept of closed scenes and textures (an asset batch that adds, moves or removes
    // files, or changes a prefab)
    void forgetInputCache();
    // what the last inputFromProject read from files: closed scenes decoded, textures whose meta
    // and header were read (the rest came from the cache)
    struct InputReads {
        int scenesDecoded = 0;
        int texturesRead = 0;
    };
    InputReads lastInputReads();

}
