// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R9a, §6.1, §6.15)
// - Purpose: the one table of every `.titleart` member: per object kind (document, canvas,
//   layer and its seven variants, material, rim, mask...), its members in canonical order with
//   value type, hard range, default, enum values, unit, Inspector label, animation class (S85),
//   interpolation and combination, glyph safety (S81) and the content hash it feeds
//   (TitleArtHash). normalizeTitleArt fills and clamps by it, the canonical writer orders by it
//   (normalize rebuilds every object in table order), TitleArtOps validates and suggests by it,
//   S82's Inspector rows and S85's track classes read it.
// - Polymorphic objects (layers, masks, adjustments) carry a discriminator member (`type`):
//   the base kind holds the common members, a variant kind ("layer.text") the rest, written
//   after them. An object whose discriminator this build does not know is kept verbatim (an
//   `unknown_type` problem, never deleted on save).
// - Registration: S80's table is built on first use; a later spec adds its members (S85's
//   `transform.stretch`, S86's `style_3d`, S87's sparse `translations`) with addMember / addKind
//   from its registration function, which the table's builder calls (registerProgramMembers in
//   TitleArtSchema.cpp, one line per spec). Reserved names (the document members and member
//   additions §6.1 lists for S81, S84-S87) are kept like unknown members until their owner
//   registers them, and TitleArtOps answers `not_available` naming the owner for a path into one.
// - Sparse kinds (S87's Translation): normalize keeps only the members present (plus the
//   kind's `alwaysWritten`), each normalized, in table order, so a default is never frozen into
//   an override.
// - Thread-safety: the table is built once, on first use; registration happens at startup or
//   from a test before other threads read it. Reads are const and safe from any thread.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::titleart {

    struct ParamDef;

    enum class ValueType : uint8_t {
        Number,         // a finite number, clamped to [min, max]
        Integer,        // a whole number, clamped; `allowed` limits it to discrete values (round_to)
        Bool,
        String,         // `pattern` (a regex) when the value has a grammar (exposed names, output ids)
        Enum,           // a string of `options`
        Color,          // "#rrggbb" (MCP's #rgb and [r, g, b] are accepted and stored as #rrggbb)
        Vec2,           // [x, y]
        Vec2List,       // exactly `maxItems` Vec2s (distort corners and mesh)
        AssetRef,       // {path, guid} or null
        AssetRefList,   // [AssetRef]
        Id,             // the object's own id (8 hex; normalize makes ids unique per document)
        LayerRef,       // a layer id or "" (masks of type layer, follow, exposed targets)
        StringList,
        Object,         // an object of `kind` (or null when `nullable`)
        ObjectArray,    // [object of `kind`], at most `maxItems` (0 = no limit)
        ObjectMap,      // {key: object of `kind`}, keys by `mapKeys`, written sorted
        ObjectArrayMap, // {key: [object of `kind`]} (part_effects: a part key to its effect stack)
        NumberMap,      // {key: number} (variations), written sorted
        IntegerMap,     // {key: integer} (features), written sorted
        EffectParams,   // the params of the registry definition named by the sibling `type`
        GeneratorParams,
        Json            // any JSON; `normalize` (when set) checks it (S85's `animation`)
    };
    JASPERENGINE_API const char* valueTypeName(ValueType type);

    // the keys of an ObjectMap / NumberMap / IntegerMap
    enum class MapKeys : uint8_t {
        Any,            // any non-empty string, sorted lexically
        Index,          // a non-negative integer (chars, lines), sorted numerically
        Tag,            // 4 printable ASCII characters (OpenType tags)
        PartKey,        // face, rim:<id>, bevel, side, band:<id> (§6.1 part keys)
        LanguageCode    // BCP 47 (S87's translations)
    };

    struct NormalizeContext;

    struct JASPERENGINE_API SchemaMember {
        std::string key;
        ValueType type = ValueType::Number;
        // the default value; for Object members null means the kind's own defaults, an object
        // overrides them (a curve member with its own default points)
        Json defaultValue;
        double min = -std::numeric_limits<double>::infinity();
        double max = std::numeric_limits<double>::infinity();
        std::vector<std::string> options;       // Enum values, snake_case
        std::vector<double> allowed;            // Integer values allowed (empty: any in range)
        std::string pattern;                    // String grammar (ECMAScript regex), empty: any
        std::string kind;                       // Object / ObjectArray / ObjectMap element kind
        size_t maxItems = 0;                    // ObjectArray / AssetRefList / StringList limit, Vec2List count
        MapKeys mapKeys = MapKeys::Any;
        bool nullable = false;                  // Object members that may be null (CharAdjust.face), ObjectArray entries
        bool readOnly = false;                  // not settable by `set` (ids, type, format, version, guid)
        ParamUnit unit = ParamUnit::None;
        std::string label;                      // Inspector label, Unity-like ("Line Height")
        FxAnim anim = FxAnim::Rerender;
        ParamInterp interp = ParamInterp::Smooth;
        ParamCombine combine = ParamCombine::Replace;
        GlyphSafety glyphSafety = GlyphSafety::Safe;
        HashScope hash = HashScope::Inherit;
        std::string owner = "S80";              // the spec that registered it
        // a custom check of a Json member (S85's `animation`): fixes `value` in place, warning
        // through `context` with JSON paths under `path`
        void (*normalize)(Json& value, const std::string& path, NormalizeContext& context) = nullptr;
    };

    struct JASPERENGINE_API SchemaKind {
        std::string name;                       // "material", "layer", "layer.text"
        std::string label;                      // "Material", "Text Layer"
        std::vector<SchemaMember> members;      // canonical order
        // polymorphic base kinds: the discriminator member and its values' variant kinds
        std::string discriminator;
        std::map<std::string, std::string> variants;
        // a variant kind: its base, whose members come first
        std::string base;
        // the hash scope of members that say Inherit, when this kind is the root of a walk
        HashScope hash = HashScope::Node;
        bool sparse = false;
        std::vector<std::string> alwaysWritten; // sparse kinds: written even when absent
        // runs before the members are normalized (defaults that depend on other members, such
        // as an image layer's face fill `source`)
        void (*prepare)(Json& object, NormalizeContext& context) = nullptr;
        // runs after (sorting, cross-member rules: gradient stops, curve end points)
        void (*finish)(Json& object, const std::string& path, NormalizeContext& context) = nullptr;
    };

    // A document member a later spec owns and has not registered in this build (§6.1's reserved
    // names): kept verbatim, refused by ops with `not_available` naming `owner`
    struct SchemaReservation {
        std::string kind;
        std::string key;
        std::string owner;      // "S85"
    };

    class JASPERENGINE_API TitleArtSchema {
    public:
        // the table, built on first use (S80's members, then every registered spec's)
        static TitleArtSchema& get();

        const SchemaKind* kind(std::string_view name) const;
        // the variant kind of `object` (its discriminator's value), `base` itself when it has no
        // variants; null when the discriminator names no variant this build knows
        const SchemaKind* variantOf(const SchemaKind& base, const Json& object) const;
        // the members of a kind with its base's first (a variant's full member list)
        const std::vector<const SchemaMember*>& membersOf(const SchemaKind& kind) const;
        // one member of `kind` (or its base) by key; null when none
        const SchemaMember* member(const SchemaKind& kind, std::string_view key) const;
        // the default object of a kind (every member at its default); a base with variants needs
        // `variant` ("text")
        Json defaultObject(std::string_view kind, std::string_view variant = {}) const;

        // ---- registration (startup or tests) ----
        // false when a kind of that name exists
        bool addKind(SchemaKind kind);
        // appended to `kind`'s members (a variant's own list for a variant kind); false when the
        // kind is missing or has the key already. A reservation of that key is lifted.
        bool addMember(std::string_view kind, SchemaMember member);
        // a variant of a polymorphic kind (S83's new layer-free effect types need none; a later
        // layer or mask type would add one)
        bool addVariant(std::string_view baseKind, std::string_view value, std::string_view variantKind);
        // tests: undo an addMember / addKind
        bool removeMember(std::string_view kind, std::string_view key);
        bool removeKind(std::string_view name);
        void reserve(std::string_view kind, std::string_view key, std::string_view owner);
        // the owner of a reserved member ("S85"), empty when not reserved
        std::string reservedOwner(std::string_view kind, std::string_view key) const;
        const std::vector<SchemaReservation>& reservations() const { return reserved; }

        // ---- field patterns ----
        // every leaf pattern of a kind ("style.rims[*].width", "chars.*.rotation"), the members of
        // objects followed (variants of a polymorphic member are not expanded: list "layer.text"
        // for the text layer's)
        std::vector<std::string> patterns(std::string_view kind) const;
        // the member a pattern names, walked from `kind` without a document ([*] and * accepted for
        // indexes and map keys; a polymorphic object's own members only: resolve a variant's from
        // its kind, "layer.text"); null when it names none
        const SchemaMember* resolvePattern(std::string_view kind, std::string_view pattern) const;

        std::vector<std::string> kindNames() const;

    private:
        TitleArtSchema() = default;
        std::vector<std::unique_ptr<SchemaKind>> kinds;
        std::vector<SchemaReservation> reserved;
        // lookups rebuilt after every registration (normalize reads them for every object)
        std::map<std::string, SchemaKind*, std::less<>> byName;
        std::map<const SchemaKind*, std::vector<const SchemaMember*>> resolved;
        SchemaKind* mutableKind(std::string_view name);
        void reindex();
        friend struct SchemaBuilderAccess;
    };

    // ---- field paths (§6.2) ----

    // One segment of a field path: a member or map key, or an [index]
    struct FieldSegment {
        std::string key;
        bool isIndex = false;
        size_t index = 0;
    };
    // "style.rims[1].width" -> style, rims, [1], width; "chars.3.rotation" -> chars, 3, rotation.
    // False with `error` for an empty segment, an unclosed bracket or a bad index.
    JASPERENGINE_API bool parseFieldPath(std::string_view path, std::vector<FieldSegment>& out, std::string* error);
    JASPERENGINE_API std::string formatFieldPath(const std::vector<FieldSegment>& path);

    // A field found in a document object (ops, exposed targets, S82's rows)
    struct JASPERENGINE_API FieldLookup {
        bool ok = false;
        Json* value = nullptr;                  // the value (after createEntries, an entry's default)
        const SchemaMember* member = nullptr;   // the schema member (null inside params)
        const ParamDef* param = nullptr;        // the definition's parameter (effect/generator params)
        const SchemaKind* owner = nullptr;      // the kind holding the member
        std::string longPath;                   // the path as written, indexes resolved
        std::string pattern;                    // its pattern ("style.rims[*].width")
        // failure: which segment, why, the names valid there and the nearest of them
        size_t failedSegment = 0;
        std::string error;
        std::string code;                       // bad_path, out_of_range, not_available, unknown_type
        std::vector<std::string> validHere;
        std::string nearest;
    };
    // Walks `path` in `object` (of `kind`). With `createEntries`, a missing map entry is created at
    // its kind's defaults (`chars.3.rotation` on a text without char adjustments), else it fails.
    JASPERENGINE_API FieldLookup lookupField(Json& object, const SchemaKind& kind, const std::vector<FieldSegment>& path, bool createEntries);

    // Every member value of `object` (of `kind`) and of the objects in it, depth first in table
    // order: `visit(member, param, value, path)` with `member` the schema member (null inside
    // effect and generator params, where `param` is set). Children of layers (groups) are visited
    // too; objects of unknown type are skipped. Return false from `visit` to skip what is inside
    // that value.
    using MemberVisitor = std::function<bool(const SchemaMember* member, const ParamDef* param, Json& value, const std::string& path)>;
    JASPERENGINE_API void forEachMember(Json& object, const SchemaKind& kind, const MemberVisitor& visit, const std::string& path = {});

    // the closest of `candidates` to `word` (edit distance, at most a third of the word plus one,
    // so a transposed pair is close); empty when none is close
    JASPERENGINE_API std::string nearestName(std::string_view word, const std::vector<std::string>& candidates);

}
