// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R16-R18, R32, R33, R42, §6.17; S82 Deviation 7, 16)
// - Purpose: the Title Designer's Library (S82 draws it): S84's Looks, Color Schemes and
//   Templates through S82's TitleLibraryProvider, registered in place of S82's starter set.
//   Items, families, the ops a click commits and a hover previews (the library's own ops: R11),
//   the step names, favorites and recents, the Save As dialogs, rename, delete and dropped files.
// - Tiles: the window's live tile (the item's ops applied to the user's layer, rendered through
//   the one preview queue at priority Tiles); until it is ready, the baked thumbnail of the
//   script the layer's text is in (kanji/kana, digits only, else Latin; R33), drawn at 50 %.
// - Settings (R18, §6.17, personal): AppSettings' `title_library` node: favorites (refs, starred
//   order), recents per kind (8, newest first, recorded by Title Designer applies only),
//   apply_font, keep_color_scheme, folded_sections. edit_title never touches them.
// - Thread-safety: main thread.

#pragma once

#include "TitleLibrary.h"
#include "titleart/designer/TitleDesignerHooks.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    // the personal settings of §6.17
    struct LibraryPrefs {
        std::vector<std::string> favorites;                     // "<kind>:<ref>", starred order
        std::map<std::string, std::vector<std::string>> recents;   // kind name -> refs, newest first
        bool applyFont = false;
        bool keepColorScheme = true;
        std::vector<std::string> foldedSections;                // "<kind>/<family>"

        static LibraryPrefs load();
        void save() const;
        bool isFavorite(LibraryKind kind, const std::string& ref) const;
        void setFavorite(LibraryKind kind, const std::string& ref, bool favorite);
        void recordApply(LibraryKind kind, const std::string& ref);
        // favorites of a kind whose item no longer resolves ("Missing" tiles) are dropped
        void dropMissing();
    };
    constexpr size_t TITLE_LIBRARY_RECENTS = 8;

    // the baked thumbnail of an item for a sample text kind ("latin", "kanji", "digits"; templates "en", "ja")
    std::filesystem::path bakedThumbnailPath(LibraryKind kind, const std::string& ref, const std::string& text);
    // which baked sample matches a text: kanji/kana -> "kanji", digits only -> "digits", else "latin"
    std::string sampleKindOf(const std::string& text);

    std::shared_ptr<titledesigner::TitleLibraryProvider> makeTitleLibraryProvider();

}
