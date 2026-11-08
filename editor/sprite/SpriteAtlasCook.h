// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// How an export cooks an atlas page (docs/specs/S45-sprite-atlas.md R16, R19): a lossless page is
// S14 quality `none` (RGBA8 + Zstandard), at its own size, without mips and without the alpha
// bleed (a transparent texel keeps its source colour), so the cooked level 0 is the page's pixels;
// a Platform Default page cooks as the project's sprites do.

#pragma once

#include "texture/TextureCookQueue.h"

#include <cstddef>

namespace jasperengine::editor::sprite {

    struct AtlasBuildResult;

    TextureCookInputs atlasPageCookInputs(const AtlasBuildResult& result, size_t page);

}
