// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The typed error every pixel-art tool reports ({kind, message, fix, field}, lifted from the
// sprite normal generator's Problem, which is now this type) and the family's verdict on a piece
// of art (ritmo's order: clean < fixable < needs_person). docs/specs/S44-aseprite-import.md §6.3.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine::editor::pixelart {

    struct PixelProblem {
        std::string kind;      // "not_aseprite", "truncated", "unknown_layer", ...: a stable machine name
        std::string field;     // the setting or file part at fault ("layers.include", "chunk 0x2005 @ 1234"), "" when none
        std::string message;   // one sentence naming the value and the limit
        std::string fix;       // what to do instead ("" when there is nothing to do)

        bool empty() const { return kind.empty(); }
    };

    enum class Verdict : uint8_t { Clean, Fixable, NeedsPerson };

    // "clean", "fixable", "needs_person"
    inline const char* verdictName(Verdict verdict) {
        switch (verdict) {
            case Verdict::Clean: return "clean";
            case Verdict::Fixable: return "fixable";
            case Verdict::NeedsPerson: return "needs_person";
        }
        return "clean";
    }

    inline bool parseVerdict(std::string_view text, Verdict& out) {
        for (Verdict v : {Verdict::Clean, Verdict::Fixable, Verdict::NeedsPerson}) {
            if (text == verdictName(v)) {
                out = v;
                return true;
            }
        }
        return false;
    }

    // The worse of two verdicts (a sheet is as bad as its worst frame)
    inline Verdict worse(Verdict a, Verdict b) { return a > b ? a : b; }

}
