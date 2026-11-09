// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a reference starts with when a texture is assigned in the editor (docs/specs/S42-pixel-art-
// mode.md R7, D4): the import's Filter Mode (absent: Point under Pixel Art, else the project's
// Default Filter Mode) as the reference's min/mag filters, and the import's Pixels Per Unit for a
// Sprite or Tilemap when it differs from the project's. The reference stays the truth for
// sampling; the filtering rule reports references that drifted. Used by the Inspector's texture
// fields and drops, the Project window's drags and MCP's create_entity / set_properties /
// add_component.

#include "pixelart/PixelArtCheck.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine {
    class Texture;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    struct AssignmentDefaults {
        bool known = false;                 // a project image (else nothing is copied)
        int minFilter = 0, magFilter = 0;   // TextureFilter values
        // the import's Pixels Per Unit when it has one that differs from the project's
        std::optional<int> pixelsPerUnit;
    };

    // texturePath: as a Texture stores it (assets-relative)
    AssignmentDefaults assignmentDefaults(const Project& project, const std::string& texturePath);
    // the texture's filters set from them; false when the texture is no project image
    bool applyAssignmentFilters(const Project& project, Texture& texture);

    // the importer's "Apply to N references": the references to the texture in the open scenes
    // whose filters differ from the import's Filter Mode, as filtering-fix changes (applyChanges)
    std::vector<FixChange> referenceFilterChanges(Project& project, const std::string& texturePath);

}
