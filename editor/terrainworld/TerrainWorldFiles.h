// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R31, R48, R49, R52, R57: a Terrain World's folder in the project. Name.terrainworld/ holds
// manifest.yaml (its GUID, lattice and maps) and tiles/x_z.jtile; the tile set is the set of files
// present. The editor's source reads it for the runtime, and the editor flushes edited tiles back
// through the atomic writer at T1 R30's points. The overview is a cache under .jasperengine/.

#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldData.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // zstd both ways, level 9 (D7)
    terrain::TileCodec terrainWorldEditorCodec();

    bool readWorldManifest(const std::filesystem::path& folder, terrain::WorldManifest& out, std::string& error);
    bool writeWorldManifest(const std::filesystem::path& folder, const terrain::WorldManifest& manifest, std::string& error);
    // the tiles/x_z.jtile files present, in key order
    std::vector<terrain::TileCoord> scanWorldTiles(const std::filesystem::path& folder);
    bool writeTileFile(const std::filesystem::path& folder, const terrain::WorldTile& tile, std::string& error);
    bool removeTileFile(const std::filesystem::path& folder, terrain::TileCoord tile, std::string& error);
    // R32: a tile file's encoded bytes as they are (an import's swap, its undo), atomically
    bool writeTileBytes(const std::filesystem::path& folder, terrain::TileCoord tile, const std::string& bytes, std::string& error);

    // R40, R43: a new world folder: manifest and every listed tile at the base height, with the
    // macro colour of these layer colours (terrainWorldLayerColours) when given, as the runtime
    // would derive it, so an undo that puts a tile back writes the bytes it had. Refuses a folder
    // that exists.
    bool createWorldFolder(const std::filesystem::path& folder, terrain::WorldManifest& manifest, const std::vector<terrain::TileCoord>& tiles,
                           std::string& error, const std::vector<float>& layerColours = {}, int layerCount = 0);
    // R33: a new world folder of tiles made elsewhere (a converted Terrain), each with T5's tree file
    // when it has trees (tiles/x_z.jetrees, R27); the manifest last. Refuses a folder that exists.
    bool writeWorldFolder(const std::filesystem::path& folder, terrain::WorldManifest& manifest,
                          const std::vector<std::shared_ptr<terrain::WorldTile>>& tiles,
                          const std::map<uint32_t, std::vector<uint8_t>>& trees, std::string& error);

    // R52: the overview cache of a world and the fingerprint (tile names, sizes, times) it was made from
    std::filesystem::path worldOverviewCachePath(const std::filesystem::path& projectInternal, const std::string& guid);
    std::string worldTileFingerprint(const std::filesystem::path& folder);

    // The project folder of a world, for the editor's runtime
    class EditorWorldSource final : public TerrainWorldSource {
    public:
        EditorWorldSource(std::filesystem::path folder, std::filesystem::path projectInternal);
        bool manifest(terrain::WorldManifest& out, std::string& error) override;
        std::string tilePath(terrain::TileCoord tile) const override;
        std::string overviewPath() const override;
        std::string treePath(terrain::TileCoord tile) const override;
        bool editable() const override { return true; }

        const std::filesystem::path& folder() const { return worldFolder; }
        std::filesystem::path overviewCache() const;

    private:
        std::filesystem::path worldFolder;
        std::filesystem::path internal;
        std::string guid;
    };

    // T1 R30's forced flush points and its debounce, for every edit-mode world of every open scene
    class TerrainWorldFiles {
    public:
        static constexpr double DEBOUNCE_SECONDS = 1.0;
        // Writes every dirty tile, removed tiles, manifests and overview caches; false when a write failed
        static bool flushAll();
        // Once a frame: what has been quiet for the debounce
        static void update(double nowSeconds);
        // One world now (an MCP call that reads files back)
        static bool flushWorld(TerrainWorldRuntime& runtime, std::string* error = nullptr);
    };

}
