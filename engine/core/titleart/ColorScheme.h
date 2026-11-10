// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R2, R5, R12, §6.2, §6.16)
// - Purpose: a Color Scheme (`.titlecolors`): the paints a recolouring gives each role of a title
//   (face, dark keyline, accent rings inner to outer, dropped base, the 3D bevel rim and side),
//   the light colours (glow, shadow, glint, the light ramp) and the rainbow behaviour, with its
//   identity and search members. ColorSchemeApply.h applies one to a document. It lives in the
//   engine because a player switches the schemes a document exposes (S81).
// - Payloads: fills, gradients, 3D materials and patterns are held as their S80 / S86 JSON
//   objects (the schema's kinds `fill`, `gradient`, `material_3d`, `pattern`), complete after
//   parse (every member at its value or default). Colours are "#rrggbb", lowercase. A null `ramp`
//   keeps an element's colour unmapped; a null `pattern` gives no marks.
// - Format: one JSON object, "format": "jasperengine.titlecolors", "version" (1), then §6.2's
//   members in that order: id, revision, name, description, family_hint, tags, synonyms, paints
//   {face, dark, accents, base, bevel_rim_3d, side_3d}, face_glow, glow, glow_gradient, shadow,
//   glint, light_ramp, orb {ramp, radial}, pattern, pattern_ground, spectrum, swatch, provenance;
//   members this build does not know follow in the order read and survive any number of
//   rewrites. The identity and search members (id, revision, name, description, family_hint,
//   tags, synonyms, swatch, provenance) are carried as JSON and written whenever present.
// - Sparse (R5): every other member equal to its default is omitted (inside fills, materials,
//   gradients and patterns against the schema's defaults; arrays are written whole with sparse
//   elements). Defaults of format version 1: paints at S80/S86 defaults with shade #000000, no
//   ramp, no pattern, no bevel rim; face_glow, glow and glint #ffffff; shadow #000000;
//   glow_gradient and orb.ramp the schema's gradient default; light_ramp and swatch empty;
//   orb.radial true; pattern and pattern_ground null; spectrum false. Changing any of these, or a
//   default of a member a scheme may omit (S80, S83, S86), bumps TITLE_COLORS_VERSION with a step
//   that writes the old default into files that omit it.
// - Lenient read: a wrong-typed member takes its default with a warning ("<json path>: message");
//   only a non-object, a wrong `format`, a bad `version` or one newer than this build fails
//   (codes not_object, bad_format, bad_version, version_newer, migration_failed).
// - Spectrum: with `spectrum` true (the `rainbow` scheme), `glow_gradient` is the spectrum the
//   gradient parameters of light take (§6.5.2).
// - Thread-safety: pure functions over values (the schema is built once).

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtMigrations.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleArtSchema;

    constexpr const char* TITLE_COLORS_FORMAT = "jasperengine.titlecolors";
    constexpr int TITLE_COLORS_VERSION = 1;

    // One role's paint (§6.2)
    struct JASPERENGINE_API ColorPaint {
        Json fill;                  // S80 fill (null: the schema's default)
        Json ramp;                  // S80 gradient of the gradient-map finish of bevelled and finished elements, or null
        Json material3d;            // S86 material_3d (null: the schema's default)
        std::string shade = "#000000";  // the bevel shadow tint
        Json pattern;               // S80 pattern marks (zebra, giraffe, leopard faces), or null
        Json extra = Json::object();    // members this build does not know
    };

    // The body tint of an orb: light centre -> body -> deep edge, radial or (spectra) diagonal
    struct JASPERENGINE_API OrbTint {
        Json ramp;                  // S80 gradient (null: the schema's default)
        bool radial = true;
        Json extra = Json::object();
    };

    struct JASPERENGINE_API ColorScheme {
        // identity and search members, carried as they are (null: absent)
        std::string id;
        int revision = 1;
        Json name, description, familyHint, tags, synonyms, swatch, provenance;

        ColorPaint face, dark, base;
        std::vector<ColorPaint> accents;            // inner to outer; the outermost ring takes the last
        std::optional<ColorPaint> bevelRim3d;       // 3D bands in the face's own material (else they follow the face)
        Json side3d;                                // S86 material_3d of side walls of their own (null: the default)
        Json paintsExtra = Json::object();

        std::string faceGlow = "#ffffff";           // the face's inner glow
        std::string glow = "#ffffff";
        Json glowGradient;                          // S80 gradient (null: the default); the spectrum when `spectrum`
        std::string shadow = "#000000";
        std::string glint = "#ffffff";
        std::vector<std::string> lightRamp;         // 4 to 7 colours, luma ascending
        OrbTint orb;
        Json pattern;                               // the marks pattern fills take (null: the face paint's, if any)
        std::optional<std::string> patternGround;   // the ground under them (null: #ffc81e)
        bool spectrum = false;

        Json extra = Json::object();                // unknown top-level members, in the order read
    };

    // `.titlecolors` migrations (S80's framework; the v1 table is empty)
    JASPERENGINE_API TitleJsonMigrations& titleColorsMigrations();

    // `j` read leniently into `out` (warnings appended to `warnings` when given). False with
    // `errorCode` (not_object, bad_format, bad_version, version_newer, migration_failed) when
    // the object is not a scheme this build reads; `out` is then unchanged.
    JASPERENGINE_API bool parseColorScheme(const Json& j, ColorScheme& out, std::vector<std::string>* warnings,
                                           std::string* errorCode = nullptr);
    // the canonical sparse object (§6.2's order)
    JASPERENGINE_API Json writeColorScheme(const ColorScheme& s);
    // the file's text: writeColorScheme with the document's formatting and a final newline
    JASPERENGINE_API std::string writeColorSchemeText(const ColorScheme& s);

    // `full`, an object of schema kind `kind`, without the members equal to `defaults` (the kind's
    // default object when null): nested objects recursively, arrays whole with each element sparse
    // against its kind's default; members the schema does not know are kept
    JASPERENGINE_API Json sparseSchemaObject(const Json& full, const std::string& kind, const Json& defaults = Json());

    // S84's document member `color_schemes` (scheme id -> scheme data, keys sorted), written only
    // when it holds a scheme; called by TitleArtSchema's registerProgramMembers
    void registerColorSchemeMembers(TitleArtSchema& schema);

}
