// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_MACRO_NORMAL_H
#define TERRAIN_MACRO_NORMAL_H

// T3 R14: the terrain-wide normal map, baked on the CPU from the height map's retained texels: RG8
// (x and z of the terrain-local normal, y is rebuilt), at the height map's resolution capped at 2048,
// with renormalized mips. Pure and thread-safe over its inputs, so it runs on a Background job; on the
// single-thread web build a bake advances by rows within the job slice.

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::terraincore {

    inline constexpr int TERRAIN_MACRO_NORMAL_MAX_SIZE = 2048;

    struct MacroNormalSource {
        const uint8_t* pixels = nullptr;
        int width = 0, height = 0, channels = 1, bytesPerChannel = 1;
        float terrainSize = 0.0f;
        float maxHeight = 0.0f;
    };

    // The side of the bake's map of one height map axis
    inline int macroNormalSize(int heightMapSize) {
        return heightMapSize < TERRAIN_MACRO_NORMAL_MAX_SIZE ? heightMapSize : TERRAIN_MACRO_NORMAL_MAX_SIZE;
    }

    // One level: size x size x 2 bytes, row by row
    struct MacroNormalLevel {
        int width = 0, height = 0;
        std::vector<uint8_t> rg;
    };

    // Rows [rowBegin, rowEnd) of the top level, into top (width x height x 2 bytes): central
    // differences of the height at the same texel centres the terrain's vertices read
    JASPERENGINE_API void bakeMacroNormalRows(const MacroNormalSource& source, int width, int height, int rowBegin, int rowEnd, uint8_t* top);

    // The mips below level 0, each normal renormalized
    JASPERENGINE_API void buildMacroNormalMips(std::vector<MacroNormalLevel>& levels);

    // The whole chain, for whoever may take the time
    JASPERENGINE_API std::vector<MacroNormalLevel> bakeMacroNormal(const MacroNormalSource& source);

}

#endif // TERRAIN_MACRO_NORMAL_H
