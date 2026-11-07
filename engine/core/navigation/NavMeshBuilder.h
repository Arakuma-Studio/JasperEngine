// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Tile builds as pure functions of their input, run on any thread, follow OpenEngine's
// DetourNavMap::BuildTile (Engine/Modules/Pathfinding/Source/DetourNavMap.cpp), Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef NAV_MESH_BUILDER_H
#define NAV_MESH_BUILDER_H

// One navmesh tile from a NavBakeInput (spec D3): Recast's rasterization, filters, areas,
// erosion, regions, contours, polygons and detail mesh, then Detour's tile data (a static
// surface) or deflated tile-cache layers (a surface with runtime carving). A build reads only
// the input and its own scratch, so tiles build in parallel and any order gives the same bytes.
//
// TileJob runs the build in stages (a few milliseconds each on a 64-cell tile), which is how a
// web build without threads bakes within its per-frame budget (spec R23): step() until done().

#include "Export.h"
#include "navigation/NavBakeInput.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::nav {

    // One record of a navmesh: Detour tile data (static) or one deflated tile-cache layer
    // (carving), with what the .navmesh file keeps beside it
    struct NavTileBlob {
        int x = 0;
        int y = 0;
        int layer = 0;
        // the tile's input hash (NavBakeInput::tileHashes)
        uint64_t hash = 0;
        uint32_t flags = 0;
        std::vector<uint8_t> data;
        // carving only, on layer 0: the tile's off-mesh links (the tile cache adds them to every
        // navmesh tile it builds from the layers)
        std::vector<NavLinkInput> links;
    };

    struct NavTileBuildResult {
        NavTileCoord coord;
        uint64_t hash = 0;
        // static: none (no walkable surface) or one; carving: one per layer
        std::vector<NavTileBlob> blobs;
        int polyCount = 0;
        bool ok = true;
        std::string error;
        double milliseconds = 0.0;
        // what Recast warned about (S74 R4) and the layers dropped past the cap
        std::vector<std::string> warnings;
    };

    class JASPERENGINE_API NavMeshBuilder {
    public:
        // The whole build of the tile at `index` (NavBakeInput::tileIndex); configOverride (S75's
        // probe) replaces the input's Recast limits for this build: walkableRadius, walkableClimb,
        // walkableHeight, minRegionArea, mergeRegionArea (the tile grid and border stay the input's)
        static NavTileBuildResult buildTile(const NavBakeInput& input, uint32_t index, const NavBuildConfig* configOverride = nullptr);

        class JASPERENGINE_API TileJob {
        public:
            TileJob(std::shared_ptr<const NavBakeInput> input, uint32_t index, const NavBuildConfig* configOverride = nullptr);
            ~TileJob();
            TileJob(TileJob&&) noexcept;
            TileJob& operator=(TileJob&&) noexcept;
            TileJob(const TileJob&) = delete;
            TileJob& operator=(const TileJob&) = delete;

            // Runs the next stage; true once the build is done (result() is ready)
            bool step();
            bool done() const;
            uint32_t index() const;
            NavTileBuildResult& result();
            // the stage step() runs next ("gather", "rasterize", ..., "done")
            const char* stageName() const;

        private:
            struct State;
            std::unique_ptr<State> state;
        };
    };

}

#endif // NAV_MESH_BUILDER_H
