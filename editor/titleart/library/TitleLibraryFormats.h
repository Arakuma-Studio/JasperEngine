// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R1, R3, R5, R49, §6.1, §6.3, D2, D3)
// - Purpose: the two library file kinds the editor owns: Looks (`.titlelook`, a sparse restyle of
//   one layer) and Templates (`.titletemplate`, a whole `.titleart` document plus a `template`
//   member). Color Schemes (`.titlecolors`) are the engine's (titleart/ColorScheme.h), because
//   players recolour exposed schemes.
// - Looks are sparse: inside `style` / `style_3d`, effect params, generator params and lighting, a
//   member equal to the default of this format version is omitted; arrays are written whole with
//   sparse elements; rims, bands and effects carry no ids. The identity and restyle containers
//   (§6.1's table) are always written. A member a look does not know (a later spec's, an unknown
//   effect type, an unknown param) is kept verbatim through any number of rewrites.
// - Canonical text: §6.1's member order, unknown members after, S80's formatting
//   (writeTitleArtJson: %.7g, colours lowercase, two-space indent), trailing newline.
//   write(parse(f)) == f for every canonical file.
// - Versions: TitleLookMigrations (v1 table empty) run before decode. Changing the default of any
//   member a look may omit bumps TITLE_LOOK_VERSION with a step writing the old default into files
//   that omit it (R5). A file of a newer version reads with `version_newer` and stays read-only.
// - Templates follow TITLE_ART_VERSION (the document's migrations); their `template` member has its
//   own `version` (1).
// - Thread-safety: pure functions over values (the schema and registry are built once).

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtMigrations.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    using Json = jasperengine::titleart::Json;

    constexpr const char* TITLE_LOOK_FORMAT = "jasperengine.titlelook";
    constexpr int TITLE_LOOK_VERSION = 1;
    constexpr const char* TITLE_LOOK_EXTENSION = ".titlelook";
    constexpr const char* TITLE_COLORS_EXTENSION = ".titlecolors";
    constexpr const char* TITLE_TEMPLATE_EXTENSION = ".titletemplate";
    constexpr int TITLE_TEMPLATE_MEMBER_VERSION = 1;
    // the layer `size` a look's px values are authored for when it says nothing
    constexpr double TITLE_LOOK_DEFAULT_REF_SIZE = 200.0;
    constexpr size_t TITLE_LIBRARY_MAX_TAGS = 12;

    // `.titlelook` migrations (S80's framework, TitleArtMigrations.h)
    titleart::TitleJsonMigrations& titleLookMigrations();

    // a text in both of the library's languages
    struct LocalizedText {
        std::string en, ja;
        // the English text, else the Japanese one
        const std::string& any() const { return en.empty() ? ja : en; }
        bool empty() const { return en.empty() && ja.empty(); }
        Json toJson() const;
    };
    // {en, ja} or a plain string (taken as English, or Japanese when it holds Japanese script)
    LocalizedText decodeLocalized(const Json& value);
    // true when `text` holds kana, kanji or CJK punctuation
    bool hasJapaneseScript(std::string_view text);

    // one problem of a library file (lenient read): its JSON path and what was done
    struct LibraryWarning {
        std::string code;           // bad_member, version_newer, unknown_part_key, bad_id ...
        std::string path;
        std::string message;
        std::string toString() const;
    };

    // ---- Look ----

    // A look held as its canonical sparse JSON (the file's object): members are read with the
    // accessors, the restyle is expanded by TitleLibraryApply
    struct TitleLook {
        Json root = Json::object();
        bool readOnly = false;          // a newer version's file

        std::string id() const;
        int revision() const;
        LocalizedText name() const;
        LocalizedText description() const;
        std::string family() const;
        std::vector<std::string> tags() const;
        std::string dimension() const;  // "2d" or "3d"
        std::string applies() const;    // "style" or "effects"
        double refSize() const;
        const Json& member(const char* key) const;  // null when absent
    };

    struct LookParseResult {
        bool ok = false;
        std::string errorCode;          // not_json, not_object, bad_format, version_newer, migration_failed
        std::string error;
        TitleLook look;
        std::vector<LibraryWarning> warnings;
    };
    LookParseResult parseLook(const std::string& text);
    LookParseResult parseLookJson(Json root);
    // the canonical text (sparse; §6.1 member order)
    std::string writeLook(const TitleLook& look);
    // `root` made canonical in memory: identity members typed, the restyle members sparse (a full
    // style, full effect params or full lighting become their sparse forms), ids dropped
    Json canonicalLookJson(const Json& root, std::vector<LibraryWarning>* warnings);

    // ---- sparse forms (S80 schema-driven) ----

    // `full` (an object of schema kind `kind`, normalized or not) minus every member equal to its
    // default: nested objects recursively, arrays whole with each element sparse against its own
    // kind's default, effect and generator params against their definition's defaults; members the
    // schema does not know are kept. `dropIds` removes the kind's `id` members on the way.
    Json sparseObject(const Json& full, std::string_view kind, bool dropIds);
    // one effect entry {type, enabled?, params} with sparse params (no id, from_look kept only when true)
    Json sparseEffect(const Json& effect, bool dropIds);
    // generator params minus the definition's defaults (unknown types and keys kept)
    Json sparseGeneratorParams(const std::string& type, const Json& params);
    // the full object a sparse one stands for (S80's normalize; new ids from `ids` when given)
    Json expandObject(const Json& sparse, std::string_view kind, titleart::TitleIdGenerator* ids);

    // ---- Template ----

    struct TitleTemplate {
        titleart::TitleArtDoc doc;      // the document (normalized), without the `template` member
        Json meta = Json::object();     // the `template` member (§6.3)
        bool readOnly = false;

        std::string id() const;
        int revision() const;
        LocalizedText name() const;
        std::string family() const;
        std::vector<std::string> tags() const;
        // the placeholders, in their order ({name, role, sample {en, ja}, max_chars, chars?})
        const Json& placeholders() const;
    };
    struct TemplateParseResult {
        bool ok = false;
        std::string errorCode;
        std::string error;
        TitleTemplate tmpl;
        std::vector<LibraryWarning> warnings;
    };
    TemplateParseResult parseTemplate(const std::string& text);
    // the document's canonical text with `template` after `guid`
    std::string writeTemplate(const TitleTemplate& tmpl);

    // ---- identities (R4) ----

    // `[a-z0-9-]{1,48}`
    bool isLibrarySlug(std::string_view slug);
    // the slug of a name: lowercase ASCII words joined by '-', other characters dropped, at most 48
    std::string slugOf(std::string_view name);
    // "<family>/<slug>", "project/<slug>", "user/<slug>"; a flat scheme id is a slug
    bool isLibraryRef(std::string_view ref);

}
