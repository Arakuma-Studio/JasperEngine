// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R40-R47, Deviation 16)
// - Purpose: the Library's content while no Library provider is registered (S84 registers its
//   own): a few Looks and Color Schemes written as S80 ops, so the Library strip is in view from
//   the start (D18) and its tiles show the user's own text with the item applied.
// - applyOps reads the committed document of the window's state: a Look replaces a styled
//   layer's face fill, bevel, rims and effects; a Color Scheme recolours the face and the rims it
//   finds. With no target layer a Look adds a text "TITLE" in that look. Hover and click run the
//   same ops (R44).
// - Tiles come from the window (`TileSource`): the window renders the target layer with the ops
//   applied, through the one preview queue (R46).
// - It has no templates, favorites, recents or saving (those are S84's).
// - Thread-safety: main thread.

#pragma once

#include "TitleDesignerHooks.h"

#include <functional>

namespace jasperengine::editor::titledesigner {

    class TitleDesignerState;

    class TitleStarterLibrary : public TitleLibraryProvider {
    public:
        using TileSource = std::function<LibraryTile(LibraryTab, const LibraryItem&, float width, float height, bool visible)>;

        TitleStarterLibrary(const TitleDesignerState* state, TileSource tiles);

        std::vector<LibraryItem> items(LibraryTab tab) override;
        std::vector<std::string> families() override;
        Json applyOps(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) override;
        std::string stepName(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) override;
        LibraryTile tile(LibraryTab tab, const LibraryItem& item, const std::string& targetKey, float width, float height, bool visible) override;
        bool savesItems() const override { return false; }

    private:
        const TitleDesignerState* state = nullptr;
        TileSource tiles;
    };

}
