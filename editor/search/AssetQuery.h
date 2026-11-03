// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Project window's search grammar (Unity's, S25 R20, R27), one parser for the search
// box, saved searches, the palette's asset: scope and MCP (list_assets search):
//
//   hero "boss room" t:texture t:model l:Hero ext:png path:textures/ re:^tile_\d+$ ref:<path|guid> missing:
//
// Terms are separated by spaces. Bare words match the name in Filter mode (every word, typos
// forgiven); a "quoted phrase" is an exact substring of the name. key:value terms: t: (an S04
// type id or a Unity alias: Texture2D, Material, Scene, Prefab, Model, AudioClip, Font, Shader,
// Script, TextAsset, Folder), l: (a label), ext:, path: (a substring of the project-relative
// path), re: (ECMAScript, case-insensitive, on the name; one that does not compile matches
// nothing and reports why), ref: (assets that reference it, S04's dependency index) and
// missing: (assets with missing references, S04's tracker). Terms with the same key OR
// together, different keys AND together, and " OR " (upper case) joins whole groups. Keys the
// table does not have are bare text. More keys register in the token table (registerToken):
// S29 adds vcs: that way.
//
// Evaluation is pure over an AssetIndexView, so tests feed it a fake index and the editor an
// in-memory one (AssetSearchIndex): a search never walks the file system.

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace jasperengine::editor {

    // One asset or folder as the search sees it
    struct AssetRecord {
        std::string guid;                 // empty for folders
        std::string path;                 // project-relative, '/' separated
        std::string name;                 // the file name ("hero.png"; a folder's own name)
        std::string ext;                  // lower case, with the dot (".png"), empty for folders
        std::string type;                 // S04 type name ("texture"), "folder" for folders
        std::vector<std::string> labels;
        bool folder = false;
        // precomputed for speed: lower-case name and path, normalized name (SearchScorer)
        std::string nameLower;
        std::string pathLower;
        std::string nameNormalized;
        std::string pathNormalized;
    };

    // Fills the precomputed fields of a record from the others
    void prepareAssetRecord(AssetRecord& record);

    class AssetIndexView {
    public:
        virtual ~AssetIndexView() = default;
        virtual size_t size() const = 0;
        virtual const AssetRecord& at(size_t index) const = 0;
        // ref:<path|guid>: the GUIDs (or, for files without one, the paths) of the assets
        // that reference it
        virtual std::unordered_set<std::string> referrersOf(const std::string& pathOrGuid) const = 0;
        // missing:: the GUIDs of assets holding references that resolve nowhere
        virtual std::unordered_set<std::string> withMissingReferences() const = 0;
    };

    class AssetQuery {
    public:
        struct Term {
            std::string key;      // "" bare, "\"" a quoted phrase, else the token ("t", "l", ...)
            std::string value;
        };
        struct Group {
            std::vector<Term> terms;
        };

        // A key's matcher: does the record match one of the values given with that key?
        // Context holds what a token computed once per evaluation (ref:'s referrer set)
        struct Context {
            const AssetIndexView* index = nullptr;
            std::map<std::string, std::shared_ptr<void>> cache;
        };
        using Matcher = std::function<bool(const AssetRecord& record, const std::string& value, Context& context)>;
        // Adds a key (lower case, without the colon); the built-in keys register this way
        static void registerToken(const std::string& key, Matcher matcher, std::string description = {});
        static bool isToken(const std::string& key);
        // key -> description, for the docs and the search field's tooltip
        static std::vector<std::pair<std::string, std::string>> tokens();

        // Unity's t: names and S04's type ids -> the type names they select
        static std::vector<std::string> typeNamesFor(const std::string& value);

        static AssetQuery parse(std::string_view text);
        // The query as text (the groups joined with " OR ")
        std::string toString() const;

        bool empty() const;
        const std::vector<Group>& groups() const { return groupList; }
        // A re: that did not compile, else ""
        const std::string& error() const { return errorText; }

        // Does the record match? *score: the bare text's rank against the name (0 without one)
        bool matches(const AssetRecord& record, Context& context, int* score = nullptr) const;

        struct Match {
            size_t index = 0;
            int score = 0;
        };
        // Every match below `folder` (project-relative, "" the whole project; the folder
        // itself excluded), by score then name
        std::vector<Match> evaluate(const AssetIndexView& index, const std::string& folder = {}) const;

    private:
        std::vector<Group> groupList;
        std::string errorText;
        // per group: the bare words (Filter mode), compiled re: patterns
        struct Compiled {
            std::string bareNormalized;
            std::vector<std::string> phrases;           // lower case
            // each key's matcher (looked up once: evaluate runs it over every asset) and values
            std::vector<std::pair<Matcher, std::vector<std::string>>> keyed;
            bool regexFailed = false;
        };
        std::vector<Compiled> compiled;
        void compile();
    };

}
