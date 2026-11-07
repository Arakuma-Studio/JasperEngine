// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_POLY_STATE_H
#define NAV_POLY_STATE_H

// Runtime polygon state of one navmesh (docs/specs/S55-navigation-gameplay-api.md R11-R18, D5):
// area overrides (convex prisms in nav space) and deactivated links over the baked flags and
// areas. Engine-internal, owned by NavMesh::Impl and made on the first override or link change,
// so a mesh without them stores nothing and its tile changes cost one null check.
//
// Each touched tile keeps Detour's storeTileState snapshot of its baked state, keyed by tile
// reference (salt included): any change to a tile restores the snapshot, then applies every
// override touching it in order and the link states. Removing the last override of a tile
// restores it byte for byte and drops the snapshot. Main thread.

#include "navigation/NavBakeInput.h"

#include <cstdint>
#include <functional>
#include <vector>

class dtNavMesh;
struct dtMeshTile;

namespace jasperengine::nav::detail {

    class NavPolyState {
    public:
        // the mesh it edits (null: released) and how far a polygon's centroid may lie below or
        // above a volume and still be in it (nav units)
        void bind(dtNavMesh* mesh, float tileWidth, float heightTolerance);
        // the mesh was freed: its snapshots go, the overrides and link states stay
        void forget();

        // Overrides apply in ascending (order, id); a later one wins, Not Walkable always wins.
        // Each returns whether a polygon changed.
        uint32_t add(const NavConvexVolume& volume, uint8_t area, uint64_t order, bool& changed);
        bool update(uint32_t id, const NavConvexVolume& volume, uint8_t area, bool& changed);
        bool remove(uint32_t id, bool& changed);
        bool has(uint32_t id) const;
        // the ground polygons whose centroid lies in the override's volume (-1: no such override)
        int polyCount(uint32_t id) const;

        bool setLinkActive(uint32_t userId, bool active);
        bool isLinkActive(uint32_t userId) const;

        // After tiles were added or rebuilt: snapshots of tiles that are gone are dropped, tiles
        // the overrides and links touch without a snapshot of their reference get one and the
        // state. True when a polygon changed.
        bool onTilesChanged();
        // Puts the tile back to its baked state and forgets it (its data is about to be copied
        // into a replacement tile, which onTilesChanged then sets up)
        void restoreTile(const dtMeshTile* tile);

        // the polygon references whose flags or area changed since the last call
        void takeChanged(std::vector<uint32_t>& out);
        // Runs `search` with every touched tile in its baked state, then puts the overrides and
        // link states back as they were: no revision, no changed polygon (S55 R27: why a path a
        // runtime volume or a deactivated link blocks fails)
        void withBakedState(const std::function<void()>& search);
        // no override, no deactivated link, no snapshot
        bool idle() const { return overrides.empty() && inactiveLinks.empty() && snapshots.empty(); }
        size_t snapshotCount() const { return snapshots.size(); }
        size_t memoryBytes() const;

    private:
        struct Override {
            uint32_t id = 0;
            uint64_t order = 0;
            NavConvexVolume volume;
            uint8_t area = 0;
        };
        struct Snapshot {
            uint64_t ref = 0;
            std::vector<unsigned char> state;
        };

        dtNavMesh* mesh = nullptr;
        float tileWidth = 1.0f;
        float heightTolerance = 0.0f;
        uint32_t nextId = 1;
        std::vector<Override> overrides;
        // sorted
        std::vector<uint32_t> inactiveLinks;
        std::vector<Snapshot> snapshots;
        std::vector<uint32_t> changed;
        // scratch of applyTile: the flags and areas before it
        std::vector<uint32_t> before;

        bool touchesTile(const dtMeshTile* tile) const;
        bool applyTile(const dtMeshTile* tile);
        // every tile the volume's bounds overlap (appended, may repeat)
        void tilesOf(const NavConvexVolume& volume, std::vector<const dtMeshTile*>& out) const;
        void linkTiles(std::vector<const dtMeshTile*>& out) const;
        bool refresh(std::vector<const dtMeshTile*>& tiles);
        Snapshot* findSnapshot(uint64_t ref);
    };

}

#endif // NAV_POLY_STATE_H
