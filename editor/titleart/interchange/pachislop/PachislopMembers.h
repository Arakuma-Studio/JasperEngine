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
// - Data: `pachislop-map.json` (S84) in the Title Art library directory, when present, adds its
//   `fields` renames (camelCase key -> ours) and its `fonts` table (Pachislop font id -> pack id);
//   without it the built-in rules and the identity font table apply (TitleArtLibrary dir:
//   JASPERENGINE_TITLEART_LIBRARY_DIR, else `titleart/` beside the editor).
// - Thread-safety: the map file is read once (a mutex); conversions are pure apart from the
//   callbacks.

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

    // the Title Art library directory (S84's pack, pachislop-map.json)
    std::filesystem::path titleArtLibraryDir();
    // the font pack id of a Pachislop builtin font id ("" when it is not builtin)
    std::string packFontId(const std::string& pachislopFontId);
    // the family names of Pachislop's 12 builtin fonts by id (font_unavailable notes)
    std::string builtinFontFamily(const std::string& pachislopFontId);
    std::string builtinFontFile(const std::string& pachislopFontId);

}
