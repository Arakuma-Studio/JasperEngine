// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R1-R6, R16, R18, R39, R41, R49, D5, §6.13, §6.16)
// - Purpose: the Title Art library store: the built-in Looks, Color Schemes and Templates
//   (embedded in the editor, zstd-compressed: titleart_library.h), the project's items
//   (<project>/EditorSettings/TitleLibrary/Looks|ColorSchemes|Templates/) and the personal ones
//   (<config dir>/title-library/looks|color-schemes|templates/), indexed lazily on the first
//   library call (never at editor start), resolved by reference, searched (TitleLibrarySearch).
// - References (R4): a built-in item is `<family>/<slug>` (schemes: a flat slug), a project item
//   `project/<slug>`, a personal one `user/<slug>`. An unknown reference fails
//   `library_item_not_found` with the three closest references and the list_title_library call
//   that finds more; a renamed built-in resolves through manifest.json's `aliases` with a
//   `deprecated` warning.
// - Availability (R6): each item's `requires` is computed when the index is built (the effect,
//   generator and 3D types it uses that this build lacks): such an item lists with
//   `available: false` and a reason ("needs effect drips (S83)") and cannot be applied.
// - Scopes rescan when their folder changes (each call compares the folder's listing with the
//   last scan); a personal library written by a newer editor is read-only.
// - The heavy assets (fonts, thumbnails; S86's HDRIs) are staged with the install
//   (packDir(): FileUtils::getTitleArtLibraryDir(), JASPERENGINE_TITLEART_LIBRARY_DIR overrides);
//   a missing pack is a state, never an error of the item data.
// - Thread-safety: main thread.

#pragma once

#include "TitleLibraryFormats.h"
#include "titleart/ColorScheme.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    enum class LibraryKind : uint8_t { Look, ColorScheme, Template };
    enum class LibraryScope : uint8_t { BuiltIn, Project, User };
    constexpr LibraryKind LIBRARY_KINDS[] = {LibraryKind::Look, LibraryKind::ColorScheme, LibraryKind::Template};

    const char* libraryKindName(LibraryKind kind);          // "looks", "color_schemes", "templates"
    const char* libraryKindLabel(LibraryKind kind);         // "Look", "Color Scheme", "Template"
    bool libraryKindFromName(std::string_view name, LibraryKind& out);
    const char* libraryScopeName(LibraryScope scope);       // "builtin", "project", "user"
    const char* libraryExtension(LibraryKind kind);         // ".titlelook" ...

    struct LibraryItemInfo {
        std::string ref;
        LibraryKind kind = LibraryKind::Look;
        LibraryScope scope = LibraryScope::BuiltIn;
        std::string family;                     // looks and templates; schemes: their first family_hint
        std::vector<std::string> familyHint;    // schemes
        int revision = 1;
        LocalizedText name, description;
        std::vector<std::string> tags;
        LocalizedText synonymsText;             // the synonyms joined by spaces, per language (search)
        std::string dimension;                  // "2d", "3d" ("" for schemes)
        std::vector<std::string> uses;          // "effect:outer_glow", "generator:glints", "pattern:zebra", "texture:brushed", "material:gold"
        std::vector<std::string> features;      // §6.13: bevel, knockout, erode, glass, reflection, finish, pattern:<k>, texture:<k>, 3d, inflate, extrude, generators
        std::string cost = "C0";                // S80's CostClass of its heaviest effect or generator; at least C3 for 3D
        std::vector<std::string> requirements;      // what this build lacks ("effect:drips")
        bool available = true;
        std::string unavailableReason;
        bool readOnly = false;                  // a newer version's file, or a built-in
        std::vector<std::string> fontPair;      // looks: the pack ids {latin, ja}
        std::filesystem::path file;             // project and personal items
        std::string builtinPath;                // built-ins: the archive path ("looks/metal/classic-gold.titlelook")
        std::string stamp;                      // the file's size and time (caches of parsed items key by it)
        std::vector<std::string> loadWarnings;
    };

    struct LibraryError {
        std::string code;                       // library_item_not_found, library_item_unavailable, library_item_builtin, bad_ref ...
        std::string message;
        std::vector<std::string> suggestions;
        std::string next;                       // the list_title_library call that finds more
        bool deprecated = false;                // resolved through an alias (a warning, not an error)
        std::string resolvedRef;
    };

    struct LibraryFamily {
        std::string id;
        LocalizedText name;
    };

    class TitleLibrary {
    public:
        static TitleLibrary& get();

        // the project whose scope is listed ("" none): its items rescan
        void setProject(const std::filesystem::path& projectDir, const std::filesystem::path& assetsDir = {});
        const std::filesystem::path& projectDir() const { return project; }
        // its assets directory (where imports go); <project>/assets when not given
        const std::filesystem::path& assetsDir() const { return assets; }
        // <config dir>/title-library (tests point it elsewhere)
        std::filesystem::path userDir() const;
        void setUserDirForTesting(const std::filesystem::path& dir);
        // the folder of a kind in a scope ("" for built-ins, or the project scope without a project)
        std::filesystem::path scopeDir(LibraryScope scope, LibraryKind kind) const;
        // the staged pack (fonts, thumbnails, HDRIs)
        static std::filesystem::path packDir();

        // every item of a kind (built-in, project, user), the index built on first use
        const std::vector<LibraryItemInfo>& items(LibraryKind kind);
        // the item a reference names; null with `error` (suggestions, next) when none; an alias
        // resolves with error->deprecated set
        const LibraryItemInfo* info(LibraryKind kind, std::string_view ref, LibraryError* error);
        std::shared_ptr<const TitleLook> look(std::string_view ref, LibraryError* error);
        std::shared_ptr<const titleart::ColorScheme> colorScheme(std::string_view ref, LibraryError* error);
        std::shared_ptr<const TitleTemplate> templateDoc(std::string_view ref, LibraryError* error);
        // the canonical bytes of an item (thumbnail and preview keys, sheets, copy)
        std::string itemText(const LibraryItemInfo& item);
        // the raw JSON of an item (scheme data for `color_schemes`, list_title_library details)
        Json itemJson(const LibraryItemInfo& item);

        const Json& manifest();
        const std::vector<LibraryFamily>& families();
        const LibraryFamily* family(std::string_view id);
        // a template's featured slot (S82's empty canvas cards)
        std::vector<std::string> featuredTemplates();

        // every item of every kind is read again on the next call
        void invalidate();
        // rises whenever a listing changes (a scope rescanned, invalidate): caches of rows key by it
        uint64_t revision() const { return changes; }
        // tests: the built-in set from these archive paths and texts instead of the embedded one
        // (an empty map restores the embedded set)
        void setBuiltinsForTesting(std::map<std::string, std::string> files);
        // tests (AC2): whether this build has a requirement ("effect:drips"); null restores the registry
        void setRequirementCheckForTesting(std::function<bool(const std::string&)> check);
        // the built-in archive's files (archive path -> text), decompressed once
        const std::map<std::string, std::string>& builtinFiles();

    private:
        TitleLibrary() = default;

        struct ScopeState {
            std::vector<LibraryItemInfo> items;
            std::string signature;              // the folder listing (names, sizes, times) of the last scan
            bool scanned = false;
        };
        struct KindState {
            std::vector<LibraryItemInfo> builtins;
            bool builtinsIndexed = false;
            ScopeState project, user;
            std::vector<LibraryItemInfo> all;
            bool dirty = true;
        };
        std::map<LibraryKind, KindState> state;
        uint64_t changes = 1;
        std::filesystem::path project;
        std::filesystem::path assets;
        std::filesystem::path userOverride;
        std::map<std::string, std::string> builtinOverride;
        bool overrideBuiltins = false;
        std::map<std::string, std::string> embedded;
        bool embeddedLoaded = false;
        Json manifestJson;
        bool manifestLoaded = false;
        std::vector<LibraryFamily> familyList;
        std::function<bool(const std::string&)> requirementCheck;
        // parsed items by ref + revision (applies and previews read them again and again)
        std::map<std::string, std::shared_ptr<const TitleLook>> lookCache;
        std::map<std::string, std::shared_ptr<const titleart::ColorScheme>> schemeCache;
        std::map<std::string, std::shared_ptr<const TitleTemplate>> templateCache;

        void refresh(LibraryKind kind);
        void indexBuiltins(LibraryKind kind, KindState& s);
        void scanScope(LibraryKind kind, LibraryScope scope, ScopeState& s);
        bool indexItem(LibraryKind kind, LibraryScope scope, const std::string& text, LibraryItemInfo& out);
        void computeAvailability(LibraryItemInfo& item);
        const std::map<std::string, std::string>& builtinSource();
    };

    // the requirement texts of an item ("effect:drips" -> "needs effect drips (S83)")
    std::string requirementReason(const std::string& requirement);
    // what an item uses, its derived features and its cost (§6.13), from its raw JSON
    void deriveItemTerms(LibraryKind kind, const Json& item, LibraryItemInfo& out);

}
