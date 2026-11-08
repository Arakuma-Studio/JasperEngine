// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavDiagnostics.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::nav;

// ---------------------------------------------------------------- names (both builds)

namespace {

    struct BakeErrorText {
        NavBakeError error;
        const char* name;
        const char* hint;
    };

    // in enum order
    constexpr BakeErrorText BAKE_ERRORS[] = {
        {NavBakeError::None, "none", ""},
        {NavBakeError::NoSurface, "no_surface", "Add a NavMesh Surface to the GameObject, or bake another one."},
        {NavBakeError::Unavailable, "unavailable",
         "This build has no navmesh support: turn on Project Settings > Navigation > Include in Builds."},
        {NavBakeError::Busy, "busy", "A bake of this surface is running: wait for it, or cancel it first."},
        {NavBakeError::NothingCollected, "nothing_collected",
         "Nothing was collected: check Collect Objects, Use Geometry and Include Layers, and that the level's meshes "
         "are static (no moving Rigidbody, agent or obstacle on them)."},
        {NavBakeError::TooLarge, "too_large",
         "The surface is too large for 32-bit polygon references: raise Voxel Size or Tile Size, or split it into "
         "smaller surfaces (Collect Objects = Volume)."},
        {NavBakeError::TileFailed, "tile_failed",
         "A tile could not be built: lower Tile Size or raise Voxel Size so each tile holds fewer polygons."},
        {NavBakeError::NavMeshFailed, "navmesh_failed", "Detour refused the navmesh: bake every tile again (Bake, mode full)."},
        {NavBakeError::Cancelled, "cancelled", "The bake was cancelled: the previous navmesh is kept. Bake again."},
        {NavBakeError::SaveFailed, "save_failed",
         "The .navmesh asset could not be written: check that its folder exists and is writable, then bake again."},
    };

}

const char* nav::bakeErrorName(NavBakeError error) {
    for (const BakeErrorText& text : BAKE_ERRORS) {
        if (text.error == error)
            return text.name;
    }
    return "none";
}

const char* nav::bakeErrorHint(NavBakeError error) {
    for (const BakeErrorText& text : BAKE_ERRORS) {
        if (text.error == error)
            return text.hint;
    }
    return "";
}

bool nav::bakeErrorFromName(const std::string& name, NavBakeError& out) {
    for (const BakeErrorText& text : BAKE_ERRORS) {
        if (name == text.name) {
            out = text.error;
            return true;
        }
    }
    return false;
}

const char* nav::findingSeverityName(FindingSeverity severity) {
    switch (severity) {
        case FindingSeverity::Info: return "info";
        case FindingSeverity::Warning: return "warning";
        case FindingSeverity::Error: return "error";
    }
    return "warning";
}

int NavIslandLabels::labelOf(unsigned int tileSlot, unsigned int poly) const {
    if ((size_t)tileSlot + 1 >= tableSize())
        return -1;
    const uint32_t first = base(tileSlot);
    if (poly >= base((size_t)tileSlot + 1) - first)
        return -1;
    return labels[(size_t)first + poly];
}

size_t NavIslandLabels::memoryBytes() const {
    return tileBase16.capacity() * sizeof(uint16_t) + tileBase32.capacity() * sizeof(uint32_t) + labels.capacity() * sizeof(int32_t) +
           islands.capacity() * sizeof(NavIsland);
}

#if JASPERENGINE_NAVIGATION

#include "DetourNavMesh.h"

namespace {

    int32_t islandRoot(std::vector<int32_t>& parent, int32_t i) {
        int32_t root = i;
        while (parent[(size_t)root] != root)
            root = parent[(size_t)root];
        // path compression
        while (parent[(size_t)i] != root) {
            const int32_t next = parent[(size_t)i];
            parent[(size_t)i] = root;
            i = next;
        }
        return root;
    }

}

void nav::labelIslands(const void* detourNavMesh, NavIslandLabels& out) {
    out.tileBase16.clear();
    out.tileBase32.clear();
    out.labels.clear();
    out.islands.clear();
    out.walkableArea = 0.0f;
    const dtNavMesh* mesh = static_cast<const dtNavMesh*>(detourNavMesh);
    if (!mesh)
        return;
    // the slots up to the last one in use, and where each slot's labels begin
    int slots = 0;
    for (int s = 0; s < mesh->getMaxTiles(); s++) {
        const dtMeshTile* tile = mesh->getTile(s);
        if (tile && tile->header && tile->header->polyCount > 0)
            slots = s + 1;
    }
    std::vector<uint32_t> tileBase((size_t)slots + 1, 0);
    for (int s = 0; s < slots; s++) {
        const dtMeshTile* tile = mesh->getTile(s);
        const int count = tile && tile->header ? tile->header->polyCount : 0;
        tileBase[(size_t)s + 1] = tileBase[(size_t)s] + (uint32_t)count;
    }
    const size_t total = tileBase.back();
    // union-find in the label array itself: a polygon's parent, -1 for zero flags
    std::vector<int32_t>& parent = out.labels;
    parent.assign(total, -1);
    for (int s = 0; s < slots; s++) {
        const dtMeshTile* tile = mesh->getTile(s);
        if (!tile || !tile->header)
            continue;
        for (int p = 0; p < tile->header->polyCount; p++) {
            if (tile->polys[p].flags != 0)
                parent[tileBase[(size_t)s] + (size_t)p] = (int32_t)(tileBase[(size_t)s] + (uint32_t)p);
        }
    }
    for (int s = 0; s < slots; s++) {
        const dtMeshTile* tile = mesh->getTile(s);
        if (!tile || !tile->header)
            continue;
        for (int p = 0; p < tile->header->polyCount; p++) {
            const int32_t self = (int32_t)(tileBase[(size_t)s] + (uint32_t)p);
            if (parent[(size_t)self] < 0)
                continue;
            // links in either direction (off-mesh connections link both their ends)
            for (unsigned int l = tile->polys[p].firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
                unsigned int salt = 0, it = 0, ip = 0;
                mesh->decodePolyId(tile->links[l].ref, salt, it, ip);
                if ((int)it >= slots || ip >= tileBase[(size_t)it + 1] - tileBase[(size_t)it])
                    continue;
                const int32_t other = (int32_t)(tileBase[(size_t)it] + ip);
                if (parent[(size_t)other] < 0)
                    continue;
                const int32_t a = islandRoot(parent, self), b = islandRoot(parent, other);
                if (a != b)
                    parent[(size_t)std::max(a, b)] = std::min(a, b);
            }
        }
    }

    // ids in discovery order: the tiles ascending (y, x, layer), then the ground polygons
    std::vector<int> order;
    for (int s = 0; s < slots; s++) {
        const dtMeshTile* tile = mesh->getTile(s);
        if (tile && tile->header && tile->header->polyCount > 0)
            order.push_back(s);
    }
    std::sort(order.begin(), order.end(), [mesh](int a, int b) {
        const dtMeshHeader* ha = mesh->getTile(a)->header;
        const dtMeshHeader* hb = mesh->getTile(b)->header;
        if (ha->y != hb->y) return ha->y < hb->y;
        if (ha->x != hb->x) return ha->x < hb->x;
        return ha->layer < hb->layer;
    });
    std::vector<int32_t> roots(total);
    for (size_t i = 0; i < total; i++)
        roots[i] = parent[i] < 0 ? -1 : islandRoot(parent, (int32_t)i);
    std::vector<int32_t> idOfRoot(total, -1);
    constexpr float FLOAT_MAX = std::numeric_limits<float>::max();
    for (int s : order) {
        const dtMeshTile* tile = mesh->getTile(s);
        const uint32_t base = tileBase[(size_t)s];
        for (int p = 0; p < tile->header->polyCount; p++) {
            const dtPoly& poly = tile->polys[p];
            const int32_t root = roots[base + (size_t)p];
            if (root < 0 || poly.getType() != DT_POLYTYPE_GROUND)
                continue;
            int32_t& id = idOfRoot[(size_t)root];
            if (id < 0) {
                id = (int32_t)out.islands.size();
                NavIsland island;
                island.id = id;
                island.boundsMin = Vector3(FLOAT_MAX, FLOAT_MAX, FLOAT_MAX);
                island.boundsMax = Vector3(-FLOAT_MAX, -FLOAT_MAX, -FLOAT_MAX);
                out.islands.push_back(island);
            }
            NavIsland& island = out.islands[(size_t)id];
            island.polys++;
            const float* v0 = &tile->verts[poly.verts[0] * 3];
            float area = 0.0f;
            for (unsigned int k = 0; k < poly.vertCount; k++) {
                const float* v = &tile->verts[poly.verts[k] * 3];
                island.boundsMin.makeFloor(Vector3(v[0], v[1], v[2]));
                island.boundsMax.makeCeil(Vector3(v[0], v[1], v[2]));
                if (k >= 2) {
                    const float* a = &tile->verts[poly.verts[k - 1] * 3];
                    const Vector3 e0(a[0] - v0[0], a[1] - v0[1], a[2] - v0[2]);
                    const Vector3 e1(v[0] - v0[0], v[1] - v0[1], v[2] - v0[2]);
                    area += 0.5f * e0.crossProduct(e1).length();
                }
            }
            island.area += area;
            out.walkableArea += area;
        }
    }
    // labels: the island of each polygon (an off-mesh connection joined to no ground: none)
    for (size_t i = 0; i < total; i++)
        parent[i] = roots[i] < 0 ? -1 : idOfRoot[(size_t)roots[i]];
    if (total <= 0xFFFF)
        out.tileBase16.assign(tileBase.begin(), tileBase.end());
    else
        out.tileBase32 = std::move(tileBase);
}

#else // JASPERENGINE_NAVIGATION

void nav::labelIslands(const void*, NavIslandLabels& out) {
    out = NavIslandLabels();
}

#endif // JASPERENGINE_NAVIGATION
