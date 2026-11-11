// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R51, D16, §6.9)
// - Purpose: Pachislop's generator types, preset ids and params in ours, for S84's converter and
//   S87's importers. The table is the data file editor/resources/titleart/pachislop-fx-map.yaml,
//   embedded at build time (TitleArtEditorSources.cmake, beside the effects' half) and parsed on first use. Each row maps one
//   Pachislop type: its preset ids, the param holding the followed layer, Vec2 pairs, renames
//   beyond camelCase -> snake_case, negated angles (our keys), enum value maps and numbered row
//   params. The file holds `generators`; Pachislop's effects get an `effects` section of the same
//   row shape when the effects stream maps them, read the same way (TitlePachislopRow).
// - Conversion (convertPachislopGenerator) of one Pachislop generator's params: the followed
//   layer's id goes to `follow` (the generator layer's member), Vec2 pairs merge into one [x, y]
//   (a half given keeps our default's other component), every generator angle is negated
//   (Pachislop's counter-clockwise y-up into our clockwise), enum values map, gradients get the
//   stop alpha, mid and interp Pachislop leaves out, and numbered params (row{n}*) become the rows
//   of a RowArray (as many as Pachislop's count param, the rest of each row our default). Keys our
//   definition lacks are dropped with a note; no value is clamped (TitleArtIO does that on load).
// - Thread-safety: parsed once on first use (main thread), read-only afterwards.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    // one Pachislop type's row of the map
    struct TitlePachislopRow {
        std::string pachislop;                                          // Pachislop's type ("corona")
        std::string type;                                               // ours ("aura")
        std::vector<std::pair<std::string, std::string>> presets;       // Pachislop preset id -> ours, its order
        std::string follow;                                             // its param naming the followed layer, "" none
        std::map<std::string, std::pair<std::string, int>> vec2;        // its key -> our Vec2 and component (0 x, 1 y)
        std::map<std::string, std::string> renames;                     // its key -> ours, beyond snake_case
        std::set<std::string> negated;                                  // our keys; "rows[].angle" = a row field
        std::map<std::string, std::map<std::string, std::string>> values;   // our key -> its value -> ours
        std::string rowParams;                                          // "row{n}": numbered params of one row ("" none)
        std::string rowCount;                                           // its param holding the row count
        std::string rowInto;                                            // our RowArray param
    };

    // where one Pachislop param lands
    struct TitlePachislopParam {
        enum class Kind : uint8_t {
            Param,          // our param `key`
            Component,      // component `index` of our Vec2 `key`
            RowField,       // field `field` of row `index` of our RowArray `key`
            RowCount,       // the number of rows of our RowArray `key`
            LayerFollow     // the generator layer's `follow` member
        };
        Kind kind = Kind::Param;
        std::string key;
        std::string field;
        int index = -1;
        bool negated = false;
    };

    // a Pachislop generator in ours
    struct TitlePachislopGenerator {
        std::string type;                                   // ours; "" when the map has no row for it
        titleart::Json params = titleart::Json::object();   // the params it set, converted (not filled with defaults)
        std::string follow;                                 // the Pachislop id of the layer it follows, "" free
    };

    // the generator rows, the data file's order (Pachislop's registry order)
    const std::vector<TitlePachislopRow>& titlePachislopGeneratorRows();
    const TitlePachislopRow* findTitlePachislopGenerator(std::string_view pachislopType);

    // our generator type for a Pachislop one ("corona" -> "aura"), and back; "" when unmapped
    std::string titlePachislopGeneratorType(std::string_view pachislopType);
    std::string titlePachislopTypeOfGenerator(std::string_view type);
    // our preset id for a Pachislop generator preset ("corona", "auraStrong" -> "strong"); "" when unmapped
    std::string titlePachislopGeneratorPresetId(std::string_view pachislopType, std::string_view pachislopPresetId);
    // where a Pachislop param key of `row` lands (by the row's rules alone: whether our definition
    // has that key is convertPachislopGenerator's check)
    TitlePachislopParam titlePachislopGeneratorParam(const TitlePachislopRow& row, std::string_view pachislopKey);

    // Pachislop's params of a generator `pachislopType` in ours; `notes` (optional) collects what
    // could not be converted
    TitlePachislopGenerator convertPachislopGenerator(std::string_view pachislopType, const titleart::Json& pachislopParams,
                                                      std::vector<std::string>* notes);
    // a whole Pachislop generator layer ({generator: {type, params}, companionOf?}): its generator,
    // and a note for `companionOf` (the companions of a layer become a visible Group "<layer> FX", D3,
    // which the importer builds)
    TitlePachislopGenerator convertPachislopGeneratorLayer(const titleart::Json& pachislopLayer, std::vector<std::string>* notes);

    // camelCase -> snake_case ("streakLength" -> "streak_length")
    std::string titlePachislopSnakeCase(std::string_view key);

}
