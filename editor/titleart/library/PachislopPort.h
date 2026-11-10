// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R20, R21, R24, R38, §6.7, §6.10.1, AC11, AC18)
// - Purpose: stage 2 of the Pachislop port (by SafEight, used with the author's permission): the
//   dump stage 1 wrote (tools/titleart/pachislop-port/dump.mjs: looks, templates, colourways and
//   registries of one Pachislop commit) mapped onto this program's library files. S87's Pachislop
//   project importer shares the same map and the same member mapping.
// - Inputs: the dump, editor/resources/titleart/pachislop-map.json (ids, families, names, fonts,
//   environments, field exceptions, dropped members with reasons, template texts, tag synonyms),
//   S83's PachislopFxMap / PachislopGeneratorMap (effects, generators), S86's
//   title3DFromPachislop (3D styles and materials), the font pack's fonts.json and S86's HDRI
//   manifest (file names), and tools/titleart/pachislop-port/overrides.json (per-item RFC 7386
//   merge patches applied last: the tuning done after the port, kept by every re-run).
// - Mapping: keys and enum values camelCase -> snake_case unless the map's `fields` says
//   otherwise (§6.7.1); every member lands on a member TitleArtSchema (or the effect and generator
//   registries) knows, or is listed in `dropped`, or the run fails naming it: nothing is dropped
//   silently. Colours are written lowercase; px values, angles and coordinates copy as they are
//   (S80 kept Pachislop's space and conventions) except where `fields` negates (rim offsets are
//   +y down in Pachislop, +y up here; gradient-mask angles are counter-clockwise there).
// - Output: repo-relative paths to canonical text (looks: writeLook; templates: writeTemplate;
//   color schemes: the colourway port; editor/resources/titleart/provenance/pachislop.json), the
//   per-item notes (mapped / approximated / dropped) and the errors. `ok` is false when any error
//   was found; files are produced for the items that mapped. Every output string is checked
//   against the denied terms (DeniedTerms.h); a hit is an error. Items whose source id holds a
//   denied term are `withheld` in the map: their provenance records `item: null`.
// - Hashed sources (Deviation 34): no denied term is written in the repository, so the map keys a
//   Pachislop id or template text that holds one by pachislopSourceKey (`sha256:` and the hex
//   SHA-256 of its UTF-8), and the committed dump holds such strings in that form. The port matches
//   a source as it reads it or by that key, so a fresh dump of a clone maps as the committed one.
// - Determinism: the same dump, map and overrides give the same bytes. Template ids (layers, rims,
//   bands, masks, effects, lights) are re-minted as FNV-1a of the item id and the object's path in
//   the document; the document guid is FNV-1a of the item id.
// - Thread-safety: pure (reads the schema and registries, built once).

#pragma once

#include "titleart/TitleArtDoc.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    using Json = jasperengine::titleart::Json;

    // one member rule of pachislop-map.json's `fields` table ("kind.key" -> rule): the importer of
    // Pachislop projects (S87) applies the same rules as the port
    struct PachislopFieldRule {
        std::string to;
        int component = -1;
        bool negate = false;
        bool environment = false;
        bool lift = false;          // a member of the object that holds this one ("^opacity")
        Json values = Json::object();
    };
    PachislopFieldRule parsePachislopFieldRule(const Json& value);
    // the editor's embedded pachislop-map.json
    const Json& embeddedPachislopMap();
    // a Pachislop id or text as the map and the committed dump write it when it holds a denied
    // term: "sha256:" and the 64 lowercase hex digits of the SHA-256 of its UTF-8
    std::string pachislopSourceKey(const std::string& source);
    // true when the map's `key` names `source`: equal, or the source's pachislopSourceKey
    bool pachislopSourceMatches(const std::string& key, const std::string& source);
    // the rule of `key` in schema kind `kind` (a variant kind falls back to its base), null when none
    const PachislopFieldRule* pachislopFieldRule(const std::string& kind, const std::string& key);
    // the pack font id of a Pachislop builtin font id (the map's `fonts`; `japanese`: the face for
    // kana and kanji), "" when the map has none
    std::string pachislopPackFont(const std::string& fontId, bool japanese = false);
    // the photo environment of a Pachislop 2D reflection map name (the map's `environments`), "" when none
    std::string pachislopEnvironment(const std::string& reflectionMap);

    // the Pachislop commit the committed dump was made from (tools/titleart/pachislop-port/dump/)
    constexpr const char* PACHISLOP_COMMIT = "1c32502";
    constexpr const char* PACHISLOP_LICENSE_NOTE = "used with the author's permission";
    // repository-relative paths the port reads and writes
    constexpr const char* PACHISLOP_DUMP_PATH = "tools/titleart/pachislop-port/dump/pachislop-1c32502.json";
    constexpr const char* PACHISLOP_MAP_PATH = "editor/resources/titleart/pachislop-map.json";
    constexpr const char* PACHISLOP_OVERRIDES_PATH = "tools/titleart/pachislop-port/overrides.json";
    constexpr const char* PACHISLOP_REVIEW_PATH = "tools/titleart/pachislop-port/review.json";
    constexpr const char* PACHISLOP_PROVENANCE_PATH = "editor/resources/titleart/provenance/pachislop.json";
    constexpr const char* TITLE_LIBRARY_RESOURCES_PATH = "editor/resources/titleart";

    struct PortOptions {
        Json map;                           // pachislop-map.json
        Json overrides = Json::object();    // overrides.json: our ref -> merge patch
        Json fontPack;                      // fonts/fonts.json (the pack ids' files)
        Json environments;                  // hdri/manifest.json (the environment ids' files)
    };
    // the inputs above from a repository checkout; false with `error` naming the file that is
    // missing or not JSON
    bool loadPortOptions(const std::filesystem::path& repoDir, PortOptions& options, std::string* error);

    struct PortResult {
        bool ok = false;
        std::map<std::string, std::string> files;   // repo-relative path -> canonical text
        Json provenance;                            // what provenance/pachislop.json holds
        std::vector<std::string> notes;             // "<ref>: mapped|approximated|dropped: ..."
        std::vector<std::string> errors;            // "<ref> (<pachislop path>): ..."
        int looks = 0, templates = 0, colorSchemes = 0;
    };
    PortResult portDump(const Json& dump, const PortOptions& options);

    // ---- the second half of the pipeline: normalize and compare (R21, AC11) ----

    // the canonical text of a library file by its extension (.titlelook: parseLook + writeLook;
    // .titletemplate: parseTemplate + writeTemplate); false with `error` when it does not parse or
    // is not a library file
    bool canonicalLibraryText(const std::string& fileName, const std::string& text, std::string& canonical, std::string* error);

    // one difference between the port's files and a tree
    struct PortFinding {
        std::string path;       // repo-relative
        std::string what;       // differs, missing (produced, not in the tree), stale (in the tree with Pachislop provenance, not produced)
        std::string detail;     // differs: the first differing line
    };
    // `files` (path -> text, as written) against the files under `root`: every produced file must
    // be there with the same bytes, and every look, template and color scheme under
    // `root`/editor/resources/titleart whose provenance source is pachislop must be produced
    std::vector<PortFinding> comparePortFiles(const std::map<std::string, std::string>& files, const std::filesystem::path& root);
    // the port's files made canonical in memory (what `normalize` does to them on disk)
    std::map<std::string, std::string> normalizedPortFiles(const PortResult& result, std::vector<std::string>* errors);

}
