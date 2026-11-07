// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Where an imported asset's generated runtime files are (docs/specs/S44-aseprite-import.md R47).
// A source an importer turns into runtime files (an .aseprite file: a sheet record, page images,
// clips) is never shipped; its outputs are found by "<source>#<key>":
//   "sprites/hero.aseprite#sheet"            the jasperengine.spritesheet record
//   "sprites/hero.aseprite#image#0"          page 0
//   "sprites/hero.aseprite#animation:walk"   a tag's clip (.anim text); "#animation#0" untagged / duplicate names
// S45 adds "<atlas>#sheet" and "<atlas>#image#<n>" (a Sprite Atlas's sheet and pages), and for a
// source an atlas in use packs (docs/specs/S45-sprite-atlas.md R2) "<source>#sheet" names its
// remapped view into the pages, "<source>#image#<n>" the pages and "<source>#atlas" the atlas
// (a texture with a Sprite Mode also has "<texture>#sheet", its frames). An exported game reads assets/je-imported/index.json
// ({"format": "jasperengine.imported", "version": 1, "files": {"<source>#<key>": "<asset-relative file>"}});
// the editor installs a provider answering from its import cache (.jasperengine/cache/imported/v1)
// instead. Sources are asset-relative, '/' separated (CookedTextures::normalize's rules).
// Thread-safety: every function takes the module's mutex (pool loads run on job threads); the
// provider is called outside it.

#ifndef IMPORTEDASSETS_H
#define IMPORTEDASSETS_H

#include "Export.h"

#include <functional>
#include <string>

namespace jasperengine {

    class JASPERENGINE_API ImportedAssets {
    public:
        static constexpr const char* kImportedDir = "je-imported";
        static constexpr const char* kIndexFormat = "jasperengine.imported";
        static constexpr int kIndexVersion = 1;

        // The generated file of "<source>#<key>": a path FileData opens (asset-relative in exports,
        // absolute in the editor). False when there is none.
        static bool find(const std::string& sourceRef, std::string& fileOut);

        using Provider = std::function<bool(const std::string& sourceRef, std::string& fileOut)>;
        // the editor's cache; null goes back to the export's index.json
        static void setProvider(Provider provider);
        static bool hasProvider();
        // S45's editor answers (atlases, their remaps, texture sheets): asked before the provider,
        // which answers what it does not; null removes it
        static void setSheetProvider(Provider provider);

        // "asset://a/../b.aseprite#sheet" -> "b.aseprite#sheet": the source part normalized, the key kept
        static std::string normalize(const std::string& sourceRef);
        // "<source>#<key>"
        static std::string reference(const std::string& source, const std::string& key);
        // forgets the loaded index (tests; a resource pack switch)
        static void reset();
    };

}

#endif // IMPORTEDASSETS_H
