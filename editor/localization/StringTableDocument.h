// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/game-ui-localization.md R1, R2, R33, R39, R41, D3, §6.1)
// - A String Table is `Name.stringtable` (JSON): its GUID, namespace (S87's rule, from the file stem
//   at creation, kept on rename), source language and ordered entries {key, text, context,
//   max_chars}; each language's translations are `Name.<lang>.translation` beside it: owner {guid,
//   path}, language, and per key {text, status, source_hash} sorted by key.
// - Files carry `format` and `version: 1`; members this editor does not know are kept and written
//   back; a newer version is refused with the editor version it needs. Saved deterministically: two
//   spaces, LF, a final newline.
// - Ops (the window and edit_string_table): set, remove, rename, add_language, remove_language,
//   mark_reviewed, applied to a set in memory, all or nothing ("op 2 (set): ..." names the op and
//   the fix). Writing a source text turns each translation whose source_hash differs needs_review.
// - Thread-safety: pure functions over values; the file helpers read from any thread.

#pragma once

#include "json.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::l10n {

    using Json = nlohmann::ordered_json;

    inline constexpr const char* STRINGTABLE_FORMAT = "jasperengine.stringtable";
    inline constexpr const char* TRANSLATION_FORMAT = "jasperengine.translation";
    inline constexpr int STRINGTABLE_VERSION = 1;
    inline constexpr const char* STRINGTABLE_EXTENSION = ".stringtable";
    inline constexpr const char* TRANSLATION_EXTENSION = ".translation";

    struct TableEntry {
        std::string key;
        std::string text;
        std::string context;
        int maxChars = 0;
        Json extra = Json::object();
    };

    struct StringTableDoc {
        std::string guid;
        std::string ns;
        std::string sourceLanguage = "en";
        std::vector<TableEntry> entries;
        Json extra = Json::object();        // unknown top-level members (formerGuids among them)

        const TableEntry* find(const std::string& key) const;
        TableEntry* find(const std::string& key);
    };

    struct TranslationEntry {
        std::string text;
        std::string status = "translated";  // translated | needs_review
        std::string sourceHash;
        Json extra = Json::object();
    };

    struct TranslationDoc {
        std::string guid;
        std::string ownerGuid;
        std::string ownerPath;              // project-relative
        std::string language;
        std::map<std::string, TranslationEntry> entries;
        Json extra = Json::object();
        Json ownerExtra = Json::object();
    };

    struct ParseError {
        std::string code;                   // not_json, wrong_format, version_newer, bad_member
        std::string message;
    };

    bool parseStringTable(const std::string& text, StringTableDoc& out, ParseError* error = nullptr);
    std::string saveStringTable(const StringTableDoc& doc);
    bool parseTranslation(const std::string& text, TranslationDoc& out, ParseError* error = nullptr);
    std::string saveTranslation(const TranslationDoc& doc);

    // S87's rules: a namespace is [a-z0-9_]{1,48}, a key [a-z0-9_.-]{1,64}
    bool isValidNamespace(const std::string& ns);
    bool isValidKey(const std::string& key);
    // a namespace made from a file stem ("Main Menu" -> "main_menu")
    std::string namespaceFromStem(const std::string& stem);
    // S87's source_hash: FNV-1a 32 of the source text, 8 hex digits
    std::string sourceHash(const std::string& text);
    // the CLDR categories of a language as a plural skeleton (Insert Plural)
    std::string pluralSkeleton(const std::string& language, const std::string& argument = "count");

    // ---- a table with its translations

    struct StringTableSet {
        std::filesystem::path owner;                            // absolute
        std::string ownerRelative;                              // project-relative ("assets/l10n/ui.stringtable")
        // a .dialogue owner (dialogue-graphs.md R3): `table` is read from its nodes' texts and is
        // never written here; only its translations are (its texts change through the graph)
        bool dialogue = false;
        StringTableDoc table;
        std::map<std::string, TranslationDoc> translations;     // language -> its file's content

        // the source language, then the translations' in order
        std::vector<std::string> languages() const;
        // the files this set is, as written: absolute path -> text
        std::map<std::filesystem::path, std::string> files() const;
        // per language the share of entries with a text (the source: 1)
        double coverage(const std::string& language) const;
        size_t missing(const std::string& language) const;
        size_t stale(const std::string& language) const;
    };

    std::filesystem::path translationPathFor(const std::filesystem::path& owner, const std::string& language);
    // "<stem>.<lang>.translation" files beside an owner: language -> path
    std::map<std::string, std::filesystem::path> translationSiblings(const std::filesystem::path& owner);
    bool loadSet(const std::filesystem::path& projectPath, const std::filesystem::path& owner, StringTableSet& out, std::string& error);

    // every .stringtable of the project (under assets/, sorted)
    std::vector<std::filesystem::path> findTables(const std::filesystem::path& projectPath);
    // every owner of texts: the String Tables and the dialogues (.dialogue), sorted
    std::vector<std::filesystem::path> findOwners(const std::filesystem::path& projectPath);
    // who holds a namespace: a String Table's or a Title Art document's project-relative path ("" when free)
    std::string namespaceOwner(const std::filesystem::path& projectPath, const std::string& ns, const std::filesystem::path& except = {});

    // a new table (`ns` from the stem when empty); false with the l10n-namespace-taken error naming the owner
    bool createTable(const std::filesystem::path& projectPath, const std::filesystem::path& owner, const std::string& ns,
                     const std::string& sourceLanguage, StringTableSet& out, std::string& error);

    // all or nothing; `notes` gets what an op did not do ("rename: 2 references not rewritten ...")
    bool applyOps(StringTableSet& set, const Json& ops, std::string& error, std::vector<std::string>* notes = nullptr);

    // a reference to a key in the project's HTML (data-l10n-id, data-l10n-attr-*), scenes (Text
    // localizationKey) and Lua scripts (a quoted "ns.key")
    struct KeyReference {
        std::string file;       // project-relative
        int line = 0;
        std::string kind;       // html, scene, script
    };
    std::vector<KeyReference> findReferences(const std::filesystem::path& projectPath, const std::string& key);
    // every key the project's HTML and scenes name (for the unknown-key checks); with `scriptCalls`
    // also the literal keys Lua passes to Localization.get/has, setLocalized and localizationKey
    std::map<std::string, KeyReference> referencedKeys(const std::filesystem::path& projectPath, bool scriptCalls = false);

}
