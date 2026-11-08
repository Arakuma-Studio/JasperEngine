// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_DIAGNOSTICS_H
#define NAV_DIAGNOSTICS_H

// What navigation says about itself (docs/specs/S75-navigation-diagnostics.md, docs/navigation.md
// "Why navigation fails"): the bake's error codes and hints, the findings every surface of
// diagnostics shares (the Navigation window, get_navigation_info, the CLI check, the export
// preflight), the per-tile bake report, and the island labelling of a Detour navmesh.
//
// Everything here runs on the main thread, on demand: nothing runs inside a bake slice (R19).

#include "Export.h"
#include "ecs/Entity.h"
#include "navigation/NavTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::nav {

    // Why a bake did not land (NavBakeResult::errorCode); values may only be appended
    enum class NavBakeError : uint8_t {
        None,
        // the GameObject has no NavMeshSurface
        NoSurface,
        // the build has no Recast/Detour (JASPERENGINE_NAVIGATION is off)
        Unavailable,
        // a bake of the surface is running
        Busy,
        NothingCollected,
        // the tile grid exceeds Detour's 32-bit polygon references (S19 R6)
        TooLarge,
        // a tile failed to build (too many polygons or vertices in one tile, ...)
        TileFailed,
        // Detour refused the assembled navmesh
        NavMeshFailed,
        Cancelled,
        // the editor could not write the .navmesh asset
        SaveFailed
    };

    // "none", "no_surface", "unavailable", "busy", "nothing_collected", "too_large",
    // "tile_failed", "navmesh_failed", "cancelled", "save_failed"
    JASPERENGINE_API const char* bakeErrorName(NavBakeError error);
    // one fixed sentence per code: what to do about it ("" for None)
    JASPERENGINE_API const char* bakeErrorHint(NavBakeError error);
    // false (out unchanged) for an unknown name
    JASPERENGINE_API bool bakeErrorFromName(const std::string& name, NavBakeError& out);

    enum class FindingSeverity : uint8_t {
        Info,
        Warning,
        Error
    };

    // "info", "warning", "error"
    JASPERENGINE_API const char* findingSeverityName(FindingSeverity severity);

    // One problem of a scene's navigation (R14): a fixed code ("not_baked", "agent_off_mesh", ...),
    // the GameObject it is about (NULL_ENTITY: the scene), a message naming it and the fix, and
    // numeric details ("dirty_tiles", "snap_distance", ...)
    struct NavFinding {
        std::string code;
        FindingSeverity severity = FindingSeverity::Warning;
        Entity entity = NULL_ENTITY;
        std::string message;
        std::vector<std::pair<std::string, double>> details;

        bool operator==(const NavFinding& other) const {
            return code == other.code && severity == other.severity && entity == other.entity && message == other.message &&
                   details == other.details;
        }
    };

    struct NavFindingOptions {
        // false: no stale_bake (no collection)
        bool stale = true;
        // a surface's dirty tile count from the caller's cache (the editor's); unset: the
        // surface is collected again (CLI, export)
        std::function<int(Entity surface)> dirtyTilesOf;
        // small_island findings per surface, largest first
        size_t maxSmallIslands = 5;
    };

    // One tile of a surface's navmesh and its last build in this session (R17)
    struct NavTileReport {
        int x = 0, y = 0, layer = 0, polys = 0;
        size_t bytes = 0;
        // -1: not built since the navmesh loaded
        double milliseconds = -1.0;
    };

    struct NavCollectOptions {
        // NavBakeInput keeps the GameObject of each triangle, terrain and volume, and
        // NavCollectStats::excludedEntities why each left-out GameObject was (the editor's probe)
        bool recordSources = false;
    };

    // ---- islands (R7, D2): the connected components of a navmesh's polygons with non-zero
    // flags, joined through polygon links and off-mesh connections in either direction

    struct JASPERENGINE_API NavIslandLabels {
        // per tile slot of the Detour mesh, up to the last one in use, the index of its first
        // polygon's label: slot s has base(s + 1) - base(s) labels. 16-bit while every index fits
        // (R21: the table stays well under a byte per polygon), else 32-bit.
        std::vector<uint16_t> tileBase16;
        std::vector<uint32_t> tileBase32;
        // the island of every polygon (off-mesh connection polygons included), -1 for a polygon
        // with zero flags
        std::vector<int32_t> labels;
        // nav space: bounds and areas in nav units (^2)
        std::vector<NavIsland> islands;
        // the ground polygons' area, nav units^2
        float walkableArea = 0.0f;

        // the label of a polygon by its tile slot and index (-1 out of range)
        int labelOf(unsigned int tileSlot, unsigned int poly) const;
        size_t tableSize() const { return tileBase16.empty() ? tileBase32.size() : tileBase16.size(); }
        uint32_t base(size_t entry) const { return tileBase16.empty() ? tileBase32[entry] : tileBase16[entry]; }
        size_t memoryBytes() const;
    };

    // Labels the islands of a dtNavMesh (NavMesh::detourNavMesh()): one union-find pass over
    // every tile's polygons and links; ids in discovery order over the tiles ascending (y, x,
    // layer), then polygon index, so the same navmesh always gets the same ids
    JASPERENGINE_API void labelIslands(const void* detourNavMesh, NavIslandLabels& out);

}

#endif // NAV_DIAGNOSTICS_H
