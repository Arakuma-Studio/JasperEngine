// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The pixel-art family's typed error and verdict (docs/specs/S44-aseprite-import.md §6.3; S43
// adds this file when it lands first): every pixel-art tool reports {kind, message, fix, field}
// and judges art clean, fixable or needing a person (ritmo's order).

#include <cstdint>
#include <string>

namespace jasperengine::editor::pixelart {

    struct PixelProblem { std::string kind, field, message, fix; };
    enum class Verdict : uint8_t { Clean, Fixable, NeedsPerson };    // "clean", "fixable", "needs_person"

}
