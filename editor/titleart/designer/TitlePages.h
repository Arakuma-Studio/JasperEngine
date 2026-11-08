// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R18, R19, R26, D4, §6.8 addSection)
// - Purpose: the Properties pages (Document, layers by type, parts, effects, masks), written once
//   and run in two modes: Draw (the panel) and Collect (the property search's index, R26). The
//   collect pass runs every page of the selection's nodes plus the Document page with every
//   section open, so the index is exactly what the pages can show on this data.
// - Sections later specs register (S81's Runtime, S86's Dimension subsections and 3D Scene, S87's
//   Languages) draw DocRows through the same RowContext, so they are searchable too.
// - Thread-safety: main thread.

#pragma once

#include "TitleRows.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    // a section a later spec adds to a page (§6.8)
    struct SectionDef {
        enum class Page : uint8_t { Document, Layer, Part };
        Page page = Page::Layer;
        std::string id;
        std::string title;
        std::string after;                  // the section it follows ("" = the end)
        // drawn inside S82's own section of this id ("dimension": S86's subsections), after its rows
        std::string parent;
        // drawn instead of S82's own section of this id while it applies ("appearance" on a
        // layer page, "material" on a part page: a Dimension 3D layer's parts are S86's)
        std::string replaces;
        // draws its rows for the page's layers (empty for the Document page) / parts
        std::function<void(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>& parts)> draw;
        // shown for this selection (null: always)
        std::function<bool(const TitleDesignerState& state)> applies;
        std::function<std::string(const TitleDesignerState& state)> summary;
    };
    void addSection(SectionDef section);
    void removeSection(const std::string& id);

    // the page of the state's primary selection (and every selected node of the same kind)
    void drawSelectionPage(RowContext& ctx);
    void documentPage(RowContext& ctx);
    // several layers: only the sections all of them have
    void layerPage(RowContext& ctx, const std::vector<std::string>& layers);
    void partPage(RowContext& ctx, const std::string& layer, const std::vector<std::string>& parts);
    void effectPage(RowContext& ctx, const NodeRef& node);
    void maskPage(RowContext& ctx, const NodeRef& node);
    // the header rows of a layer page (name, Blend, Opacity: R13, R18)
    void layerHeaderRows(RowContext& ctx, const std::vector<std::string>& layers);

    // the collect pass (D4): the selection's pages, each part, effect and mask of its layers, and
    // the Document page; with `everyLayer` every layer of the document (R26's "Search every layer")
    void collectIndex(TitleDesignerState& state, PropertyHost* host, PropertyIndex& index, bool everyLayer);
    // the same pass as units of one page each, so the window builds a few per frame (R69): a
    // layer page, a part's, an effect's, a mask's, the Document page, then the "+ Add" offers
    struct CollectUnit {
        enum class Kind : uint8_t { Layer, Part, Effect, Mask, Document, Offers };
        Kind kind = Kind::Layer;
        NodeRef node;
        int slice = -1;     // a layer page in slices: its top-level section of this index (-1: whole)
    };
    constexpr int LAYER_PAGE_SLICES = 16;     // the last also takes any section past it
    std::vector<CollectUnit> planCollect(const TitleDesignerState& state, bool everyLayer);
    // collects one unit into `index` (a unit whose node is gone adds nothing); the caller keeps a
    // CollectLayerLookup of the shown document alive around a batch of units
    void collectUnit(TitleDesignerState& state, PropertyHost* host, PropertyIndex& index, const CollectUnit& unit);
    // the effects and generators the search offers as "+ Add" (every registry entry)
    void collectAddOffers(PropertyIndex& index);

    // a section title with its summary (Typography, Effects): exposed for the window's tests
    std::string typographySummary(const Json& layer);
    std::string transformSummary(const Json& layer);

}
