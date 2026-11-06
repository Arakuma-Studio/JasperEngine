// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R14, §6.3 "Content hashes and caches")
// - Purpose: per-layer content hashes, computed once per commit or load (never per frame), that
//   key the renderer's cascade of caches: geometry (CPU outlines) -> mask (coverage) -> field
//   (distance fields, height, satin) -> stack (shaded elements in frame space) -> node (the
//   layer's pixels in its target, with effects, masks and placement).
// - Which member feeds which hash is the schema's (SchemaMember::hash, HashScope): a member of
//   scope Stack (a colour) changes stack and node but never mask or field; Placement (position)
//   only the node; Identity (ids, names) nothing, so rendering never depends on ids. A member a
//   later spec registers declares its scope and is hashed with no change here.
// - Hashes are 64-bit FNV-1a over the canonical text (writeTitleArtJson) of the members in each
//   scope, chained down the cascade. Document members feed every layer: `seed` the geometry,
//   `global_light` the stack (the bevel light stays fixed on screen), `canvas` the node. An
//   asset a layer names (font, image, SVG, reflection map) feeds its member's scope through its
//   content revision, so a changed font reaches every hash that names it. A group's node hash
//   chains its children's in order; a node also chains the node hashes of the layers it reads
//   (mask layers, the `follow` layer), so a source change re-renders its readers.
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace jasperengine::titleart {

    JASPERENGINE_API uint64_t fnv1a64(std::string_view bytes, uint64_t seed = 0xcbf29ce484222325ull);
    JASPERENGINE_API uint64_t hashCombine(uint64_t a, uint64_t b);

    struct LayerHashes {
        uint64_t geometry = 0;
        uint64_t mask = 0;          // geometry + rim widths, joins, plates, erode
        uint64_t field = 0;         // mask + bevel / inner glow / satin sizes and profiles
        uint64_t stack = 0;         // field + materials, rotation, flips, global light
        uint64_t effects = 0;       // the layer's effect stacks (layer and part effects)
        uint64_t masks = 0;         // the layer's masks
        uint64_t placement = 0;     // position (and what only places the cached stack)
        uint64_t node = 0;          // all of these, children, sources, blend and opacity, canvas
        // what the layer's own pixels show wherever it is: stack, effects and masks, its
        // children's node hashes and its sources', without its own placement, blend and opacity
        // (the renderer's node cache re-places them on a whole-pixel move, R42)
        uint64_t content = 0;
    };

    struct TitleArtHashes {
        std::map<std::string, LayerHashes> layers;     // by layer id
        uint64_t document = 0;      // every layer's node hash in order, and the document members
    };

    // the content revision of an asset a document names (0 when unknown); the editor's
    // AssetDatabase content hash or S81's player loader. Called once per reference.
    using AssetRevisionFn = std::function<uint64_t(const TitleAssetRef& ref)>;

    JASPERENGINE_API TitleArtHashes hashTitleArt(const TitleArtDoc& doc, const AssetRevisionFn& assetRevision = {});

}
