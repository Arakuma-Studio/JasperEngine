// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "terrain/TerrainRawIO.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // T1 R61, R58: a height file's import fields (Depth, Width, Height, Byte Order, Flip Vertically,
    // Value Range), each guessed value marked, and the preview (sizes, data range, mean, share
    // clipped, histogram). Shared by the Terrain window's Import Raw... and Create Terrain's Start
    // Shape. A changed field decodes the file again; true then. maxHeight: metres for the mean.
    // setHeightFromRange: shown with Source Min/Max when not null.
    bool drawHeightImportFields(const std::string& path, const std::vector<uint8_t>& bytes, HeightImportOptions& options,
                                HeightImport& preview, float maxHeight, bool* setHeightFromRange);

}
