// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3 "Draw tree", Pachislop's drawTree)
// - Purpose: the pure visibility pre-pass every render goes through (previews, bakes,
//   thumbnails, checks, S81): from a document snapshot and a request's options to the nodes that
//   draw, with their clip groups.
// - The one visibility rule: a layer draws when `visible`, `opacity > 0` and (includeGuides or
//   not `guide`); a hidden group hides its subtree; a group left with no drawn child is dropped;
//   the clip base of a layer with `clip` is the nearest sibling below with `clip` false, and the
//   clipped layers of a hidden or empty base hide with it; a clip with no base, or over an
//   adjustment layer, is ignored (the layer draws unclipped). Layers of unknown type draw nothing.
// - Isolation keeps the path from the top to the isolated layer (its ancestors' transforms still
//   place it; it and its ancestors draw even when hidden, as a layer mask's source does) and
//   drops everything else; OutputContent::Layers keeps the listed layers in place
//   (their ancestors kept, every other layer hidden). Ink and Glow are decided per effect by the
//   renderer (EffectDef::addsLight), not here.
// - Thread-safety: pure; the nodes point into the snapshot, which must outlive them.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    enum class DrawNodeKind : uint8_t { Leaf, Group, Adjust };

    struct DrawNode {
        DrawNodeKind kind = DrawNodeKind::Leaf;
        const Json* layer = nullptr;        // into the snapshot
        std::string layerId;
        LayerType type = LayerType::Unknown;
        int depth = 0;                      // 0 at the top level
        std::vector<DrawNode> children;     // Group: drawn children, bottom first
        std::vector<DrawNode> clipped;      // the layers clipped to this one, bottom first
    };

    struct DrawTreeOptions {
        bool includeGuides = false;
        std::string isolateLayer;
        OutputContent content = OutputContent::All;
        std::vector<std::string> contentLayers;
    };

    // the top-level nodes, bottom first
    JASPERENGINE_API std::vector<DrawNode> buildDrawTree(const TitleArtDoc& doc, const DrawTreeOptions& options);

}
