// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavPolyState.h"

#if JASPERENGINE_NAVIGATION

#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::nav;
using namespace jasperengine::nav::detail;

namespace {

    // whether the xz point lies in the convex polygon (either winding; on an edge counts)
    bool insideConvex(const std::vector<float>& points, float x, float z) {
        const size_t count = points.size() / 2;
        if (count < 3)
            return false;
        float sign = 0.0f;
        for (size_t i = 0; i < count; i++) {
            const size_t j = (i + 1) % count;
            const float ax = points[i * 2], az = points[i * 2 + 1];
            const float bx = points[j * 2], bz = points[j * 2 + 1];
            const float cross = (bx - ax) * (z - az) - (bz - az) * (x - ax);
            if (cross == 0.0f)
                continue;
            if (sign == 0.0f)
                sign = cross;
            else if ((cross > 0.0f) != (sign > 0.0f))
                return false;
        }
        return true;
    }

    void volumeBounds(const NavConvexVolume& volume, float* bmin, float* bmax) {
        bmin[0] = bmin[2] = HUGE_VALF;
        bmax[0] = bmax[2] = -HUGE_VALF;
        for (size_t i = 0; i + 1 < volume.points.size(); i += 2) {
            bmin[0] = std::min(bmin[0], volume.points[i]);
            bmax[0] = std::max(bmax[0], volume.points[i]);
            bmin[2] = std::min(bmin[2], volume.points[i + 1]);
            bmax[2] = std::max(bmax[2], volume.points[i + 1]);
        }
        bmin[1] = volume.minY;
        bmax[1] = volume.maxY;
    }

    bool tileOverlaps(const dtMeshTile* tile, const float* bmin, const float* bmax, float tolerance) {
        const float* tmin = tile->header->bmin;
        const float* tmax = tile->header->bmax;
        return bmin[0] <= tmax[0] && bmax[0] >= tmin[0] && bmin[2] <= tmax[2] && bmax[2] >= tmin[2] &&
               bmin[1] - tolerance <= tmax[1] && bmax[1] + tolerance >= tmin[1];
    }

    // the flags and area of a polygon in one value (change detection)
    uint32_t polyKey(const dtPoly& poly) {
        return ((uint32_t)poly.flags << 8) | poly.getArea();
    }

}

void NavPolyState::bind(dtNavMesh* navMesh, float width, float tolerance) {
    mesh = navMesh;
    tileWidth = width > 0.0f ? width : 1.0f;
    heightTolerance = tolerance;
}

void NavPolyState::forget() {
    snapshots.clear();
    mesh = nullptr;
}

NavPolyState::Snapshot* NavPolyState::findSnapshot(uint64_t ref) {
    for (Snapshot& snapshot : snapshots) {
        if (snapshot.ref == ref)
            return &snapshot;
    }
    return nullptr;
}

void NavPolyState::tilesOf(const NavConvexVolume& volume, std::vector<const dtMeshTile*>& out) const {
    if (!mesh || volume.points.size() < 6)
        return;
    float bmin[3], bmax[3];
    volumeBounds(volume, bmin, bmax);
    // the tile grid's origin is 0 (NavMesh's dtNavMeshParams)
    const int x0 = (int)std::floor(bmin[0] / tileWidth), x1 = (int)std::floor(bmax[0] / tileWidth);
    const int y0 = (int)std::floor(bmin[2] / tileWidth), y1 = (int)std::floor(bmax[2] / tileWidth);
    const dtNavMesh* readable = mesh;
    const dtMeshTile* found[32];
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const int count = readable->getTilesAt(x, y, found, 32);
            for (int i = 0; i < count; i++) {
                if (tileOverlaps(found[i], bmin, bmax, heightTolerance))
                    out.push_back(found[i]);
            }
        }
    }
}

void NavPolyState::linkTiles(std::vector<const dtMeshTile*>& out) const {
    if (!mesh || inactiveLinks.empty())
        return;
    const dtNavMesh* readable = mesh;
    for (int i = 0; i < readable->getMaxTiles(); i++) {
        const dtMeshTile* tile = readable->getTile(i);
        if (!tile || !tile->header || tile->header->offMeshConCount == 0)
            continue;
        for (int c = 0; c < tile->header->offMeshConCount; c++) {
            if (std::binary_search(inactiveLinks.begin(), inactiveLinks.end(), tile->offMeshCons[c].userId)) {
                out.push_back(tile);
                break;
            }
        }
    }
}

bool NavPolyState::touchesTile(const dtMeshTile* tile) const {
    for (const Override& entry : overrides) {
        float bmin[3], bmax[3];
        volumeBounds(entry.volume, bmin, bmax);
        if (tileOverlaps(tile, bmin, bmax, heightTolerance))
            return true;
    }
    if (!inactiveLinks.empty()) {
        for (int c = 0; c < tile->header->offMeshConCount; c++) {
            if (std::binary_search(inactiveLinks.begin(), inactiveLinks.end(), tile->offMeshCons[c].userId))
                return true;
        }
    }
    return false;
}

bool NavPolyState::applyTile(const dtMeshTile* constTile) {
    // the mesh is ours to edit; Detour hands out its tiles const
    dtMeshTile* tile = const_cast<dtMeshTile*>(constTile);
    const uint64_t ref = mesh->getTileRef(tile);
    Snapshot* snapshot = findSnapshot(ref);
    const bool touched = touchesTile(tile);
    if (!snapshot && !touched)
        return false;

    const int polyCount = tile->header->polyCount;
    before.resize((size_t)polyCount);
    for (int p = 0; p < polyCount; p++)
        before[(size_t)p] = polyKey(tile->polys[p]);

    if (snapshot) {
        mesh->restoreTileState(tile, snapshot->state.data(), (int)snapshot->state.size());
    } else {
        Snapshot fresh;
        fresh.ref = ref;
        fresh.state.resize((size_t)mesh->getTileStateSize(tile));
        mesh->storeTileState(tile, fresh.state.data(), (int)fresh.state.size());
        snapshots.push_back(std::move(fresh));
    }

    if (touched) {
        // the overrides over this tile, in order
        std::vector<const Override*> over;
        for (const Override& entry : overrides) {
            float bmin[3], bmax[3];
            volumeBounds(entry.volume, bmin, bmax);
            if (tileOverlaps(tile, bmin, bmax, heightTolerance))
                over.push_back(&entry);
        }
        if (!over.empty()) {
            for (int p = 0; p < tile->header->offMeshBase; p++) {
                dtPoly& poly = tile->polys[p];
                if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount == 0)
                    continue;
                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                for (unsigned int v = 0; v < poly.vertCount; v++) {
                    const float* vert = &tile->verts[poly.verts[v] * 3];
                    cx += vert[0];
                    cy += vert[1];
                    cz += vert[2];
                }
                const float inverse = 1.0f / (float)poly.vertCount;
                cx *= inverse;
                cy *= inverse;
                cz *= inverse;
                int area = -1;
                bool blocked = false;
                for (const Override* entry : over) {
                    if (cy < entry->volume.minY - heightTolerance || cy > entry->volume.maxY + heightTolerance ||
                        !insideConvex(entry->volume.points, cx, cz))
                        continue;
                    if (entry->area == AREA_NOT_WALKABLE)
                        blocked = true;
                    else
                        area = entry->area;
                }
                if (blocked) {
                    poly.flags = 0;
                    poly.setArea(AREA_NOT_WALKABLE);
                } else if (area >= 0) {
                    poly.flags = (unsigned short)(1u << area);
                    poly.setArea((unsigned char)area);
                }
            }
        }
        // a deactivated link: its connections pass nothing; their polygon area (a link cost
        // area) stays
        if (!inactiveLinks.empty()) {
            for (int c = 0; c < tile->header->offMeshConCount; c++) {
                const dtOffMeshConnection& con = tile->offMeshCons[c];
                if (std::binary_search(inactiveLinks.begin(), inactiveLinks.end(), con.userId))
                    tile->polys[con.poly].flags = 0;
            }
        }
    } else {
        // nothing touches it any more: it stays baked and is forgotten
        snapshots.erase(snapshots.begin() + (findSnapshot(ref) - snapshots.data()));
    }

    bool any = false;
    const dtPolyRef base = mesh->getPolyRefBase(tile);
    for (int p = 0; p < polyCount; p++) {
        if (polyKey(tile->polys[p]) != before[(size_t)p]) {
            changed.push_back((uint32_t)(base | (dtPolyRef)p));
            any = true;
        }
    }
    return any;
}

bool NavPolyState::refresh(std::vector<const dtMeshTile*>& tiles) {
    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    bool any = false;
    for (const dtMeshTile* tile : tiles)
        any |= applyTile(tile);
    return any;
}

uint32_t NavPolyState::add(const NavConvexVolume& volume, uint8_t area, uint64_t order, bool& changedAny) {
    Override entry;
    entry.id = nextId++;
    entry.order = order;
    entry.volume = volume;
    entry.area = area < AREA_COUNT ? area : AREA_WALKABLE;
    const auto at = std::upper_bound(overrides.begin(), overrides.end(), entry, [](const Override& a, const Override& b) {
        return a.order != b.order ? a.order < b.order : a.id < b.id;
    });
    overrides.insert(at, std::move(entry));
    std::vector<const dtMeshTile*> tiles;
    tilesOf(volume, tiles);
    changedAny = refresh(tiles);
    return nextId - 1;
}

bool NavPolyState::update(uint32_t id, const NavConvexVolume& volume, uint8_t area, bool& changedAny) {
    changedAny = false;
    auto found = std::find_if(overrides.begin(), overrides.end(), [&](const Override& entry) { return entry.id == id; });
    if (found == overrides.end())
        return false;
    std::vector<const dtMeshTile*> tiles;
    tilesOf(found->volume, tiles);
    found->volume = volume;
    found->area = area < AREA_COUNT ? area : AREA_WALKABLE;
    tilesOf(volume, tiles);
    changedAny = refresh(tiles);
    return true;
}

bool NavPolyState::remove(uint32_t id, bool& changedAny) {
    changedAny = false;
    auto found = std::find_if(overrides.begin(), overrides.end(), [&](const Override& entry) { return entry.id == id; });
    if (found == overrides.end())
        return false;
    std::vector<const dtMeshTile*> tiles;
    tilesOf(found->volume, tiles);
    overrides.erase(found);
    changedAny = refresh(tiles);
    return true;
}

bool NavPolyState::has(uint32_t id) const {
    return std::any_of(overrides.begin(), overrides.end(), [&](const Override& entry) { return entry.id == id; });
}

int NavPolyState::polyCount(uint32_t id) const {
    auto found = std::find_if(overrides.begin(), overrides.end(), [&](const Override& entry) { return entry.id == id; });
    if (found == overrides.end())
        return -1;
    std::vector<const dtMeshTile*> tiles;
    tilesOf(found->volume, tiles);
    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    int count = 0;
    for (const dtMeshTile* tile : tiles) {
        for (int p = 0; p < tile->header->offMeshBase; p++) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount == 0)
                continue;
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            for (unsigned int v = 0; v < poly.vertCount; v++) {
                const float* vert = &tile->verts[poly.verts[v] * 3];
                cx += vert[0];
                cy += vert[1];
                cz += vert[2];
            }
            const float inverse = 1.0f / (float)poly.vertCount;
            cy *= inverse;
            if (cy >= found->volume.minY - heightTolerance && cy <= found->volume.maxY + heightTolerance &&
                insideConvex(found->volume.points, cx * inverse, cz * inverse))
                count++;
        }
    }
    return count;
}

bool NavPolyState::setLinkActive(uint32_t userId, bool active) {
    const auto at = std::lower_bound(inactiveLinks.begin(), inactiveLinks.end(), userId);
    const bool inactive = at != inactiveLinks.end() && *at == userId;
    if (inactive == !active)
        return false;
    // the tiles holding the link before the change (its last deactivated tile must be restored)
    std::vector<const dtMeshTile*> tiles;
    if (active) {
        linkTiles(tiles);
        inactiveLinks.erase(at);
    } else {
        inactiveLinks.insert(at, userId);
        linkTiles(tiles);
    }
    return refresh(tiles);
}

bool NavPolyState::isLinkActive(uint32_t userId) const {
    return !std::binary_search(inactiveLinks.begin(), inactiveLinks.end(), userId);
}

bool NavPolyState::onTilesChanged() {
    if (!mesh || idle())
        return false;
    const dtNavMesh* readable = mesh;
    // a removed or rebuilt tile has a new salt: its snapshot describes nothing any more
    snapshots.erase(std::remove_if(snapshots.begin(), snapshots.end(),
                                   [&](const Snapshot& snapshot) { return readable->getTileByRef((dtTileRef)snapshot.ref) == nullptr; }),
                    snapshots.end());
    std::vector<const dtMeshTile*> tiles;
    for (const Override& entry : overrides)
        tilesOf(entry.volume, tiles);
    linkTiles(tiles);
    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    bool any = false;
    for (const dtMeshTile* tile : tiles) {
        if (!findSnapshot(mesh->getTileRef(tile)))
            any |= applyTile(tile);
    }
    return any;
}

void NavPolyState::withBakedState(const std::function<void()>& search) {
    if (!mesh || snapshots.empty()) {
        search();
        return;
    }
    std::vector<std::vector<unsigned char>> current(snapshots.size());
    std::vector<dtMeshTile*> tiles(snapshots.size(), nullptr);
    for (size_t i = 0; i < snapshots.size(); i++) {
        dtMeshTile* tile = const_cast<dtMeshTile*>(mesh->getTileByRef(snapshots[i].ref));
        if (!tile)
            continue;
        tiles[i] = tile;
        current[i].resize((size_t)mesh->getTileStateSize(tile));
        mesh->storeTileState(tile, current[i].data(), (int)current[i].size());
        mesh->restoreTileState(tile, snapshots[i].state.data(), (int)snapshots[i].state.size());
    }
    search();
    for (size_t i = 0; i < snapshots.size(); i++) {
        if (tiles[i])
            mesh->restoreTileState(tiles[i], current[i].data(), (int)current[i].size());
    }
}

void NavPolyState::restoreTile(const dtMeshTile* tile) {
    if (!mesh || snapshots.empty())
        return;
    const uint64_t ref = mesh->getTileRef(tile);
    Snapshot* snapshot = findSnapshot(ref);
    if (!snapshot)
        return;
    mesh->restoreTileState(const_cast<dtMeshTile*>(tile), snapshot->state.data(), (int)snapshot->state.size());
    snapshots.erase(snapshots.begin() + (snapshot - snapshots.data()));
}

void NavPolyState::takeChanged(std::vector<uint32_t>& out) {
    out.swap(changed);
    changed.clear();
}

size_t NavPolyState::memoryBytes() const {
    size_t bytes = sizeof(*this) + overrides.capacity() * sizeof(Override) + inactiveLinks.capacity() * sizeof(uint32_t) +
                   changed.capacity() * sizeof(uint32_t) + before.capacity() * sizeof(uint32_t);
    for (const Override& entry : overrides)
        bytes += entry.volume.points.capacity() * sizeof(float);
    for (const Snapshot& snapshot : snapshots)
        bytes += sizeof(Snapshot) + snapshot.state.capacity();
    return bytes;
}

#endif // JASPERENGINE_NAVIGATION
