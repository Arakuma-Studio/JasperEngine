// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The `jasperengine.spritesheet` v1 text (docs/specs/S44-aseprite-import.md R45, §6.11).
// - Pure functions of their arguments: any thread (the importer's jobs, the pool's loads).
// - parseSheet fails (sheet empty, error set) only for text that is not JSON, a non-object root,
//   a missing or different `format`, and a missing or newer `version`. Everything else is
//   lenient, as .anim is: a bad page, frame, tag, slice or value is dropped (or repaired) with a
//   warning carrying its JSON path ("frames[3].rect"), the rest loads. Frame names stay unique (a
//   duplicate is renamed "<name> 2" with a warning).
// - writeSheet is canonical and deterministic: keys in the order of §6.11, frames in sheet order,
//   one frame / tag / slice per line, integers only; writeSheet(parseSheet(t)) == t for every
//   text writeSheet made.

#ifndef SPRITE_SHEET_IO_H
#define SPRITE_SHEET_IO_H

#include "Export.h"
#include "sprite/SpriteSheet.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine{

    constexpr const char* SPRITESHEET_FORMAT = "jasperengine.spritesheet";
    constexpr int SPRITESHEET_VERSION = 1;

    struct JASPERENGINE_API SpriteSheetParseResult{
        std::optional<SpriteSheetData> sheet;
        std::vector<std::string> warnings;     // "<json path>: <what was dropped and why>"
        std::string error;

        bool ok() const { return sheet.has_value(); }
    };

    JASPERENGINE_API SpriteSheetParseResult parseSheet(const std::string& text);
    JASPERENGINE_API std::string writeSheet(const SpriteSheetData& sheet);

    // "#rrggbbaa" <-> 0..1 RGBA (the record's colour form)
    JASPERENGINE_API std::string spriteColorText(const Vector4& color);
    JASPERENGINE_API bool spriteColorFromText(const std::string& text, Vector4& out);

}

#endif // SPRITE_SHEET_IO_H
