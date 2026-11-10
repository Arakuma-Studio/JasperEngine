// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Tilemap Layers in the pixel-art validator (docs/specs/S50-grid-tilemap.md R74a): each layer gives
// one Tilemap drawable per tileset page, so filtering, mipmaps_compression, texel_size, off_grid
// and palette cover tile pages and layers. Registered once with S42's collectors.

namespace jasperengine::editor::tilemap {

    void registerTilemapPixelArtDrawables();

}
