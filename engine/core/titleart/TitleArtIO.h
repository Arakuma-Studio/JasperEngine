// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R1, R2, R47, §6.1)
// - Purpose: the `.titleart` codec: the lenient reader, normalize, the canonical writer, and the
//   document a new Title Art starts as.
// - Format: a UTF-8 JSON object whose first members are "format": "jasperengine.titleart",
//   "version" (1) and "guid" (32 lowercase hex), then the members of TitleArtSchema's
//   "document" kind. Every member is written, defaults included (a later change of a default
//   never changes how a file renders), except the members of sparse kinds (S87's translations),
//   which are written only when present.
// - Canonical text: members in table order, unknown members after the known ones in the order
//   read; 2-space indent, one member per line; arrays of numbers on one line; numbers %.7g
//   (integers plain, -0 as 0); colours lowercase; maps (chars, lines, part_effects, variations,
//   features, material_links, effect params) sorted by key, chars and lines numerically.
//   write(parse(write(d))) == write(d).
// - Lenient read: only text that is not a JSON object, a wrong or missing `format`, or a
//   `version` newer than this build (`version_newer`) fails, with line:col when the text is at
//   fault. A member of the wrong type takes its default, an out-of-range one is clamped, each
//   with a warning carrying its JSON path. Unknown members and objects of unknown `type`
//   (layers, masks, adjustments, effects, generators) survive any number of edits and saves.
// - Older versions migrate in memory before decode (TitleArtMigrations.h); the file is
//   rewritten in the current version only by the next commit.
// - Thread-safety: pure functions (they read the schema and the registry, built once).

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    // Where normalize reports, and the ids it hands out
    struct JASPERENGINE_API NormalizeContext {
        std::vector<TitleWarning>* warnings = nullptr;
        TitleIdGenerator* ids = nullptr;        // new ids for missing or duplicate ones (null: a local one)

        void warn(const std::string& path, const std::string& message);
    };

    // a JSON path segment helpers share ("layers[2]", "style.rims")
    JASPERENGINE_API std::string jsonPathMember(const std::string& path, const std::string& name);
    JASPERENGINE_API std::string jsonPathIndex(const std::string& path, size_t index);

    struct JASPERENGINE_API TitleArtParseError {
        std::string code;       // not_json, not_object, bad_format, version_newer, bad_version, migration_failed
        int line = 0;           // 1-based; 0 when not at a text position
        int column = 0;
        std::string message;

        std::string toString() const;
    };

    struct JASPERENGINE_API TitleArtParseResult {
        bool ok = false;
        TitleArtParseError error;
        TitleArtDoc doc;
        std::vector<TitleWarning> warnings;
        int versionRead = 0;                        // the file's "version"
        std::vector<std::string> migrationsApplied; // the steps run, oldest first ("v1->v2 name")

        // a save writes another version than the file has
        bool migrated() const { return !migrationsApplied.empty(); }
    };

    JASPERENGINE_API TitleArtParseResult parseTitleArt(const std::string& text);
    // the same from a parsed root (the CLI's and MCP's `from` objects); `root` is migrated and
    // normalized in place
    JASPERENGINE_API TitleArtParseResult parseTitleArtJson(Json root);
    JASPERENGINE_API std::string writeTitleArt(const TitleArtDoc& doc);

    // Makes `doc` canonical: every schema member present and clamped, unknown types kept,
    // unique ids (a missing, malformed or repeated id gets a new one), the §6.1 limits (layers,
    // depth, effects, rims, masks, exposed, outputs, text length, canvas), part keys of rims that
    // no longer exist dropped (positional `rim#n` aliases turned into `rim:<id>`), unique output
    // ids and exposed names. Idempotent.
    JASPERENGINE_API void normalizeTitleArt(TitleArtDoc& doc, std::vector<TitleWarning>* warnings = nullptr);
    JASPERENGINE_API void normalizeTitleArt(TitleArtDoc& doc, NormalizeContext& context);
    // one object of a schema kind (a pasted layer, an effect entry, a mask) made canonical, as
    // inside a document (ids are not made unique: normalizeTitleArt does that per document)
    JASPERENGINE_API void normalizeTitleArtObject(Json& object, const std::string& kind, const std::string& path, NormalizeContext& context);

    // The canonical text of any JSON value under the document's formatting rules (used by the
    // writer, the content hashes and tools that print document fragments)
    JASPERENGINE_API std::string writeTitleArtJson(const Json& value, int indent = 0);

    // A new document (R7): canvas 1920 x 1080, one Text layer "Title" with `text` in the
    // built-in font, the exposed parameter `text` driving it, the output `main`
    struct NewTitleArtOptions {
        int width = 1920;
        int height = 1080;
        std::string text = "Title";
        std::string guid;       // empty: a new one
        uint64_t idSeed = 0;    // 0: random ids; else a fixed sequence (a reproducible build of a template)
    };
    JASPERENGINE_API TitleArtDoc makeNewTitleArt(const NewTitleArtOptions& options = {});

    // Problems a document has without rendering it: objects of unknown types (unknown_type),
    // exposed targets that do not resolve (exposed_target_missing), Dimension 3D layers without
    // the 3D module (dimension_unavailable), `source` fills on layers without pixels
    // (fill_source_without_pixels), masks and `follow` naming missing layers
    // (mask_layer_missing, follow_missing)
    JASPERENGINE_API std::vector<TitleProblem> titleArtDocumentProblems(const TitleArtDoc& doc);

    // ---- setting fields (TitleArtOps' `set`, applyFieldOverrides, S82's rows) ----

    struct SchemaMember;
    struct ParamDef;

    // what one field write did
    struct JASPERENGINE_API FieldSetResult {
        bool ok = false;
        std::string code;                   // bad_path, bad_value, read_only, not_available, unknown_type, out_of_range
        std::string error;
        std::string longPath;               // the path with shorthands expanded and indexes resolved
        std::string pattern;                // "style.rims[*].width"
        Json from, to;                      // the value before and after (after: clamped, normalized)
        bool clamped = false;
        std::string clampMessage;           // "140 clamped to 100"
        const SchemaMember* member = nullptr;
        const ParamDef* param = nullptr;
        std::string ownerLabel;             // the label of what holds it ("Rim", "Outer Glow"; "" for a layer)
        std::vector<std::string> validHere;     // a bad path: the names valid at the failing segment
        std::string nearest;                    // and the closest of them
    };

    // Writes `value` at field path `path` (shorthands of §6.2 accepted) of `object`, an object of
    // schema kind `kind` ("layer", "document"). The value is checked against its member or
    // parameter: a value of the wrong type fails (bad_value, nothing written); one out of range is
    // clamped (clamped). Read-only members (ids, types, format, version, guid) are refused unless
    // `allowReadOnly`. A missing map entry on the way (`chars.3.rotation`) is created at its
    // defaults.
    JASPERENGINE_API FieldSetResult setTitleArtField(Json& object, const std::string& kind, const std::string& path,
                                                     const Json& value, bool allowReadOnly = false);
    // the value at a field path (shorthands accepted); null with `error` when it does not resolve
    JASPERENGINE_API const Json* getTitleArtField(const Json& object, const std::string& kind, const std::string& path,
                                                  std::string* longPath = nullptr, std::string* error = nullptr);

    // A field path with §6.2's shorthands written out for `layer` (a normalized layer object, or
    // null for document paths): face. -> style.face., rim:<id>. / rim#<n>. -> style.rims[i].,
    // fx:<type>[#n]. -> effects[i].params. (or the entry's own enabled / id / type / from_look),
    // <part>/fx:<type>[#n]. -> part_effects.<part key>[i].params., mask:<id>. / mask#<n>. ->
    // masks[i]., gen. -> generator.params., adj. -> adjustment.; band:, band# and 3d. are S86's
    // (not_available until it registers style_3d). False with `code` and `error` (and the valid
    // names) when a shorthand names nothing.
    struct JASPERENGINE_API ExpandedFieldPath {
        bool ok = false;
        std::string path;
        std::string code;
        std::string error;
        std::vector<std::string> validHere;
        std::string nearest;
    };
    JASPERENGINE_API ExpandedFieldPath expandFieldShorthand(const Json* layer, const std::string& path);

    // the snapshot copy of one render: each override written into `doc` (S81's exposed values,
    // S85's poses, S87's languages), then the document normalized; an override that does not
    // resolve is skipped with a problem (override_skipped) and never fails the render
    JASPERENGINE_API void applyFieldOverrides(TitleArtDoc& doc, const std::vector<FieldOverride>& overrides,
                                              std::vector<TitleProblem>* problems = nullptr);

    // the "guid" of a .titleart text, "" when it has none or the text does not parse (the asset database)
    JASPERENGINE_API std::string readTitleArtGuid(const std::string& text);

}
