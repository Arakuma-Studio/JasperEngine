// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A pixel particle preset made for a project (docs/specs/S49-pixel-particles.md R20): its ramps
// on the project's copy of builtin:pixelfx-effects (made on first use, S43 R7), its child effects
// (a sub-emitter's preset) as .particles assets, its constants scaled by the project's Pixels Per
// Unit. The caller writes `writes` as one undo step (ProjectFileWrites::writeUndoable) with the
// edit that uses the document, and links the two.

#include "command/type/WriteFilesCmd.h"
#include "particles/ParticleEffect.h"

#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    struct ProjectPixelPreset {
        particles::ParticleEffectDocument document;
        std::vector<WriteFilesCmd::Item> writes;      // the palette copy and the children not there yet
        std::vector<std::string> children;            // their assets-relative paths
    };

    // `childDirectory`: where the child effects go, assets-relative ("" : the assets folder)
    bool prepareProjectPixelPreset(Project& project, const std::string& name, const std::string& childDirectory, ProjectPixelPreset& out,
                                   std::string& error);

}
