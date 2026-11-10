// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What one edit_sprite batch changed, as dirty-rect patches (docs/specs/S47-agent-sprite-drawing.md
// R32, D7): the undo step's content and the way any earlier state of the file is rebuilt (R29).
// Immutable once made and shared between SpriteEditCmd (which owns it) and SpriteDocCache (which
// watches it weakly: a trimmed undo step is a revision that can no longer be rebuilt).

#pragma once

#include "SpriteDoc.h"
#include "SpriteEdit.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    // One touched image cel: the rect that changed, in canvas coordinates, both sides of it, and
    // the cel's bounds before and after (an edit can grow a cel). Links resolved: `frame` holds the image.
    struct CelPatch {
        int layer = 0;
        int frame = 0;                  // from 0
        IntRect rect;
        IntRect celBefore, celAfter;
        bool existedBefore = false, existedAfter = false;
        bool indexed = false;
        std::vector<uint8_t> data;      // before then after: indices (w*h) on indexed sprites, else RGBA (w*h*4)
    };

    // The whole structure on both sides (layers, frames, tags, slices, palette, canvas). Images are
    // shared, immutable pointers, so a removed cel's pixels are all it pays for.
    struct StructurePatch {
        aseprite::Document before, after;
    };

    struct SpritePatchSet {
        std::string revisionBefore, revisionAfter;
        std::vector<CelPatch> cels;
        std::unique_ptr<StructurePatch> structure;
        // the file as it was, when re-encoding the before side would not give its bytes back (a
        // cel chunk Aseprite compressed, first touched by this edit)
        std::shared_ptr<const std::vector<uint8_t>> beforeFile;
        std::vector<DirtyRect> rects;   // what changed, for the live view
        std::string by;                 // the agent's name
        std::string summary;            // "line, fill x2"

        // the patch bytes (R32)
        size_t memoryBytes() const;
    };

    // The difference between a document and the batch's working copy of it. A structure batch keeps
    // both sides whole; a pixel batch keeps only the touched cels' changed rects.
    std::shared_ptr<SpritePatchSet> makePatch(const SpriteDoc& before, const SpriteDoc& after, bool structureChanged);
    // `doc` at one side of the patch to the other (forward: before to after). The caller's copy keeps its provenance.
    void applyPatch(SpriteDoc& doc, const SpritePatchSet& patch, bool forward);
    // true when the patch's after pixels are still what `doc` shows (R34.3: a save that kept the agent's work)
    bool patchSurvives(const SpriteDoc& doc, const SpritePatchSet& patch);

}
