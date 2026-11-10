// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R38, R51, R53: a Terrain World's folder as a player reads it. The export writes world.bin (the
// manifest and its tiles, as the player parses no YAML), overview.jtile and each tile recompressed
// at level 19 with editor-only sections left out, its seams repaired on the copy (the lower (x, z)
// wins) and each repair reported. Same input, same bytes.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TerrainWorldCookReport {
        int tiles = 0;
        int seamRepairs = 0;
        uint64_t compressedBytes = 0;
        std::vector<std::string> warnings;
    };

    class TerrainWorldCooker {
    public:
        // R38: the web's limits on a world's compressed tiles
        static constexpr uint64_t WEB_WARN_BYTES = 64ull << 20;
        static constexpr uint64_t WEB_MAX_BYTES = 96ull << 20;
        static constexpr int WEB_MAX_TILE_QUADS = 256;

        // destination is the cooked folder (Name.terrainworld under the export's assets)
        static bool cook(const std::filesystem::path& source, const std::filesystem::path& destination, bool web, TerrainWorldCookReport& report,
                         std::string& error);
    };

}
