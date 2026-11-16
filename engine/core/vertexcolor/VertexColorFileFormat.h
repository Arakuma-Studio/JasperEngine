// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The .vcolors header's constants, shared by its reader (VertexColorFile.cpp) and writer
// (VertexColorFileWrite.cpp)

#include <cstdint>

namespace jasperengine::vertexcolor::fileformat {

    inline constexpr uint8_t VCOLORS_MAGIC[4] = {'J', 'E', 'V', 'C'};
    inline constexpr uint8_t VCOLORS_FLAG_EDITOR = 1;
    inline constexpr uint8_t VCOLORS_FLAG_ZSTD = 2;

}
