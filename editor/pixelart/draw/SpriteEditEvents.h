// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The live view's hooks (docs/specs/S47-agent-sprite-drawing.md R41): told on the main thread after
// every write, undo and redo of a sprite by edit_sprite, and when a save from outside the editor
// overwrote agent edits (R34.3). One listener per owner.

#pragma once

#include "SpriteEdit.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    struct SpriteEdited {
        std::string path;                  // project-relative
        std::string revision;
        std::vector<DirtyRect> rects;
        std::string origin;                // "agent", "undo", "redo"
        std::string by;                    // the agent's name ("Claude Code")
        std::string summary;               // "line, fill x2"
        uint64_t undoId = 0;               // the history entry, 0 when not known yet
    };

    struct SpriteExternalSave {
        std::string path;
        int overwrittenEdits = 0;
        std::string message;               // the Console's sentence
    };

    namespace SpriteEditEvents {
        void addListener(const void* owner, std::function<void(const SpriteEdited&)> edited, std::function<void(const SpriteExternalSave&)> external);
        void removeListener(const void* owner);
        void edited(const SpriteEdited& event);
        void externalSave(const SpriteExternalSave& event);
    }

}
