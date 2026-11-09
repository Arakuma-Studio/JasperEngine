// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md §6.8; S84 §6.7.1's member rules)
// - Purpose: Pachislop's style, material, mask, adjustment and outline members as `.titleart`
//   members, guided by TitleArtSchema: each key goes camelCase -> snake_case unless a rename
//   names it, nested objects convert by their member's kind, enum values go camelCase ->
//   snake_case, colours lowercase, x/y pairs (`offsetX`/`offsetY`, `dx`/`dy`, `x`/`y`) become
//   Vec2s, asset ids become AssetRefs through the caller, and a key the schema has no member
//   for is reported (dropped). The same rules as S84's PachislopPort, so a look converted by S84
//   and a project imported here give the same members.
// - Data: S84's `pachislop-map.json` (titlelibrary::pachislopFieldRule: the `fields` rules by kind,
//   with their components and sign flips), the same table PachislopPort converts the library with,
//   and the key's camelCase -> snake_case rule for everything the table does not name. Font and
//   environment ids resolve through the same map (titlelibrary::pachislopPackFont, ...), the
//   files come from S84's pack through its ImportPlan.
// - Thread-safety: conversions are pure apart from the callbacks.

#pragma once

#include "PachislopJson.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::pachislop {

    struct MemberNote {
        std::string field;          // the Pachislop path ("style.face.reflection.map")
        std::string what;
        std::string action;         // dropped | approximated
    };

    struct MemberContext {
        // a Pachislop asset id -> an AssetRef (null when it has none)
        std::function<Json(const std::string& assetId, const std::string& field)> asset;
        // a Pachislop layer id -> the new layer id ("" when none)
        std::function<std::string(const std::string& pachislopId)> layer;
        // a 2D reflection map name -> its environment's AssetRef (null: unavailable, with its own note)
        std::function<Json(const std::string& mapName, const std::string& field)> environment;
        std::vector<MemberNote>* notes = nullptr;
        bool has3D = false;         // S86's members are registered (style_3d, char 3D members)
        void note(const std::string& field, const std::string& what, const std::string& action = "dropped") const {
            if (notes) notes->push_back({field, what, action});
        }
    };

    // camelCase -> snake_case ("arcLower" -> "arc_lower")
    std::string snakeCase(const std::string& key);

    // a Pachislop object of the shape that `kind` (a TitleArtSchema kind) holds, converted;
    // `path` names it in the notes
    Json convertMembers(const Json& pachislop, const std::string& kind, const MemberContext& context, const std::string& path);

}
