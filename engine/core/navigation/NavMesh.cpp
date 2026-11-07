// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The tile pool and the checks of tile data before Detour sees it follow OpenEngine's DetourNavMap
// (Engine/Modules/Pathfinding/Source/DetourNavMap.cpp), Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License, extended to off-mesh connections and tile-cache layers.
// See NOTICE.md.

#include "NavMesh.h"
#include "NavHash.h"

#include "Log.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::nav;

// ---------------------------------------------------------------- data, filters (both builds)

void NavMeshData::sortTiles() {
    std::sort(tiles.begin(), tiles.end(), [](const NavTileBlob& a, const NavTileBlob& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.x != b.x) return a.x < b.x;
        return a.layer < b.layer;
    });
}

size_t NavMeshData::memoryBytes() const {
    size_t bytes = sizeof(NavMeshData) + header.linkAreaCosts.capacity() * sizeof(float);
    for (const NavTileBlob& tile : tiles)
        bytes += sizeof(NavTileBlob) + tile.data.capacity() + tile.links.capacity() * sizeof(NavLinkInput);
    return bytes;
}

const NavTileBlob* NavMeshData::findTile(int x, int y, int layer) const {
    for (const NavTileBlob& tile : tiles) {
        if (tile.x == x && tile.y == y && tile.layer == layer)
            return &tile;
    }
    return nullptr;
}

NavFilter::NavFilter() {
    for (int i = 0; i < AREA_COUNT; i++)
        areaCosts[i] = 1.0f;
}

NavFilter NavFilter::fromSettings(uint16_t areaMask) {
    NavFilter filter;
    filter.areaMask = areaMask;
    const NavigationSettings& settings = getSettings();
    for (int i = 0; i < AREA_COUNT; i++)
        filter.areaCosts[i] = std::max(1.0f, settings.areas[(size_t)i].cost);
    return filter;
}

NavFilter NavFilter::fromQuery(const QueryFilter& query) {
    NavFilter filter = fromSettings(query.areaMask);
    for (int i = 0; i < AREA_COUNT; i++) {
        if (query.costMask & (1u << i))
            filter.areaCosts[i] = clampAreaCost(query.areaCosts[(size_t)i]);
    }
    return filter;
}

// ---------------------------------------------------------------- the file format

namespace {

    constexpr char MAGIC[4] = {'J', 'N', 'A', 'V'};

    class Writer {
    public:
        std::vector<uint8_t> bytes;
        void raw(const void* data, size_t size) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), p, p + size);
        }
        void u8(uint8_t v) { bytes.push_back(v); }
        void u32(uint32_t v) { for (int i = 0; i < 4; i++) bytes.push_back((uint8_t)(v >> (8 * i))); }
        void i32(int32_t v) { u32((uint32_t)v); }
        void u64(uint64_t v) { for (int i = 0; i < 8; i++) bytes.push_back((uint8_t)(v >> (8 * i))); }
        void f32(float v) {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            u32(bits);
        }
        void string(const std::string& s) {
            u32((uint32_t)s.size());
            raw(s.data(), s.size());
        }
        void pad4() { while (bytes.size() % 4) bytes.push_back(0); }
    };

    class Reader {
    public:
        Reader(const uint8_t* data, size_t size): data(data), size(size) {}
        bool ok = true;
        size_t offset = 0;

        bool need(size_t n) {
            if (!ok || n > size - offset) {
                ok = false;
                return false;
            }
            return true;
        }
        uint8_t u8() { if (!need(1)) return 0; return data[offset++]; }
        uint32_t u32() {
            if (!need(4)) return 0;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) v |= (uint32_t)data[offset + (size_t)i] << (8 * i);
            offset += 4;
            return v;
        }
        int32_t i32() { return (int32_t)u32(); }
        uint64_t u64() {
            if (!need(8)) return 0;
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)data[offset + (size_t)i] << (8 * i);
            offset += 8;
            return v;
        }
        float f32() {
            const uint32_t bits = u32();
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
        }
        std::string string(size_t maxLength) {
            const uint32_t length = u32();
            if (length > maxLength || !need(length)) {
                ok = false;
                return std::string();
            }
            std::string s(reinterpret_cast<const char*>(data + offset), length);
            offset += length;
            return s;
        }
        bool bytes(std::vector<uint8_t>& out, size_t length) {
            if (!need(length)) return false;
            out.assign(data + offset, data + offset + length);
            offset += length;
            return true;
        }
        void skip4() { while (ok && offset % 4) { if (!need(1)) return; offset++; } }

    private:
        const uint8_t* data;
        size_t size;
    };

    void writeConfig(Writer& w, const NavBuildConfig& c) {
        w.u8((uint8_t)c.plane);
        w.u8(c.carving ? 1 : 0);
        w.f32(c.cellSize);
        w.f32(c.cellHeight);
        w.i32(c.tileSize);
        w.f32(c.agentRadius);
        w.f32(c.agentHeight);
        w.f32(c.agentClimb);
        w.f32(c.agentMaxSlope);
        w.i32(c.walkableHeight);
        w.i32(c.walkableClimb);
        w.i32(c.walkableRadius);
        w.i32(c.borderSize);
        w.i32(c.minRegionArea);
        w.i32(c.mergeRegionArea);
        w.i32(c.maxEdgeLen);
        w.f32(c.maxSimplificationError);
        w.f32(c.detailSampleDist);
        w.f32(c.detailSampleMaxError);
        w.i32(c.maxVertsPerPoly);
        w.i32(c.maxPolysPerTile);
    }

    void readConfig(Reader& r, NavBuildConfig& c) {
        c.plane = r.u8() == 1 ? NavPlane::XY : NavPlane::XZ;
        c.carving = r.u8() != 0;
        c.cellSize = r.f32();
        c.cellHeight = r.f32();
        c.tileSize = r.i32();
        c.agentRadius = r.f32();
        c.agentHeight = r.f32();
        c.agentClimb = r.f32();
        c.agentMaxSlope = r.f32();
        c.walkableHeight = r.i32();
        c.walkableClimb = r.i32();
        c.walkableRadius = r.i32();
        c.borderSize = r.i32();
        c.minRegionArea = r.i32();
        c.mergeRegionArea = r.i32();
        c.maxEdgeLen = r.i32();
        c.maxSimplificationError = r.f32();
        c.detailSampleDist = r.f32();
        c.detailSampleMaxError = r.f32();
        c.maxVertsPerPoly = r.i32();
        c.maxPolysPerTile = r.i32();
    }

    void writeLink(Writer& w, const NavLinkInput& link) {
        for (float v : link.start) w.f32(v);
        for (float v : link.end) w.f32(v);
        w.f32(link.radius);
        w.u8(link.bidirectional ? 1 : 0);
        w.u8(link.area);
        w.u8(link.polyArea);
        w.u32(link.userId);
    }

    void readLink(Reader& r, NavLinkInput& link) {
        for (float& v : link.start) v = r.f32();
        for (float& v : link.end) v = r.f32();
        link.radius = r.f32();
        link.bidirectional = r.u8() != 0;
        link.area = r.u8();
        link.polyArea = r.u8();
        link.userId = r.u32();
    }

    bool finite(float v) { return std::isfinite(v); }

}

std::vector<uint8_t> NavMeshFile::write(const NavMeshData& data) {
    Writer w;
    w.raw(MAGIC, 4);
    w.u32(VERSION);

    const NavMeshHeader& h = data.header;
    w.i32(h.agent.id);
    w.string(h.agent.name);
    w.f32(h.agent.radius);
    w.f32(h.agent.height);
    w.f32(h.agent.stepHeight);
    w.f32(h.agent.maxSlope);
    w.u64(h.areaTableHash);
    writeConfig(w, h.config);
    w.u64(h.config.hash());
    w.u32((uint32_t)data.tiles.size());
    // dtNavMeshParams: a zero origin, square tiles of the tile size, the pool and the poly limit
    w.f32(0.0f);
    w.f32(0.0f);
    w.f32(0.0f);
    w.f32(h.config.tileWorldSize());
    w.f32(h.config.tileWorldSize());
    w.i32(h.maxTiles);
    w.i32(h.config.maxPolysPerTile);
    w.i32(h.maxObstacles);
    w.u32((uint32_t)h.linkAreaCosts.size());
    for (float cost : h.linkAreaCosts)
        w.f32(cost);

    for (const NavTileBlob& tile : data.tiles) {
        w.i32(tile.x);
        w.i32(tile.y);
        w.i32(tile.layer);
        w.u64(tile.hash);
        w.u32(tile.flags);
        w.u32((uint32_t)tile.data.size());
        w.pad4();
        w.raw(tile.data.data(), tile.data.size());
        w.pad4();
        w.u32((uint32_t)tile.links.size());
        for (const NavLinkInput& link : tile.links)
            writeLink(w, link);
    }
    w.u64(hashBytes(w.bytes.data(), w.bytes.size()));
    return std::move(w.bytes);
}

bool NavMeshFile::read(const uint8_t* bytes, size_t size, NavMeshData& out, std::string& error) {
    out = NavMeshData();
    if constexpr (std::endian::native != std::endian::little) {
        error = "navmesh data is little-endian and this platform is big-endian";
        return false;
    }
    if (size < 4 + 4 + 8 || std::memcmp(bytes, MAGIC, 4) != 0) {
        error = "not a navmesh file (no JNAV magic)";
        return false;
    }
    Reader r(bytes, size - 8);
    r.offset = 4;
    const uint32_t version = r.u32();
    if (version != VERSION) {
        error = version > VERSION ? "navmesh format version " + std::to_string(version) + " is newer than this engine reads (" +
                                        std::to_string(VERSION) + "); rebake it or update the engine"
                                  : "unknown navmesh format version " + std::to_string(version);
        return false;
    }
    {
        Reader trailer(bytes, size);
        trailer.offset = size - 8;
        if (trailer.u64() != hashBytes(bytes, size - 8)) {
            error = "the navmesh file is damaged (its checksum does not match)";
            return false;
        }
    }

    NavMeshHeader& h = out.header;
    h.agent.id = r.i32();
    h.agent.name = r.string(1024);
    h.agent.radius = r.f32();
    h.agent.height = r.f32();
    h.agent.stepHeight = r.f32();
    h.agent.maxSlope = r.f32();
    h.areaTableHash = r.u64();
    readConfig(r, h.config);
    const uint64_t configHash = r.u64();
    const uint32_t tileCount = r.u32();
    float orig[3];
    for (float& v : orig) v = r.f32();
    const float tileWidth = r.f32();
    const float tileHeight = r.f32();
    h.maxTiles = r.i32();
    const int maxPolys = r.i32();
    h.maxObstacles = r.i32();
    const uint32_t costCount = r.u32();
    if (!r.ok || costCount > (uint32_t)(MAX_POLY_AREAS - FIRST_LINK_COST_AREA)) {
        error = "the navmesh header is truncated or invalid";
        return false;
    }
    for (uint32_t i = 0; i < costCount; i++)
        h.linkAreaCosts.push_back(r.f32());
    const NavBuildConfig& c = h.config;
    if (!r.ok || configHash != c.hash() || !(c.cellSize > 0.0f) || !(c.cellHeight > 0.0f) || c.tileSize < 1 ||
        c.tileSize > 1024 || maxPolys != c.maxPolysPerTile || maxPolys < 1 || h.maxTiles < 1 ||
        !finite(tileWidth) || std::fabs(tileWidth - c.tileWorldSize()) > 1e-4f * std::max(1.0f, tileWidth) ||
        tileWidth != tileHeight || orig[0] != 0.0f || orig[1] != 0.0f || orig[2] != 0.0f ||
        h.maxObstacles < 0 || h.maxObstacles > 65535 || tileCount > (uint32_t)h.maxTiles * 4u) {
        error = "the navmesh header is invalid";
        return false;
    }
    if (saltBitsFor(h.maxTiles, maxPolys) < 10) {
        error = checkPolyRefBits(h.maxTiles, maxPolys);
        return false;
    }

    out.tiles.reserve(tileCount);
    for (uint32_t i = 0; i < tileCount; i++) {
        NavTileBlob tile;
        tile.x = r.i32();
        tile.y = r.i32();
        tile.layer = r.i32();
        tile.hash = r.u64();
        tile.flags = r.u32();
        const uint32_t dataSize = r.u32();
        r.skip4();
        if (!r.ok || dataSize > 64u * 1024u * 1024u || !r.bytes(tile.data, dataSize)) {
            error = "the navmesh file is truncated (tile " + std::to_string(i) + ")";
            return false;
        }
        r.skip4();
        const uint32_t linkCount = r.u32();
        if (!r.ok || linkCount > 65535) {
            error = "the navmesh file is truncated (tile " + std::to_string(i) + " links)";
            return false;
        }
        for (uint32_t l = 0; l < linkCount; l++) {
            NavLinkInput link;
            readLink(r, link);
            tile.links.push_back(link);
        }
        if (!r.ok || tile.layer < 0 || tile.layer > 255) {
            error = "the navmesh file is truncated (tile " + std::to_string(i) + ")";
            return false;
        }
        out.tiles.push_back(std::move(tile));
    }
    if (!r.ok || r.offset != size - 8) {
        error = "the navmesh file has trailing or missing bytes";
        return false;
    }
    out.sortTiles();
    return true;
}

#if JASPERENGINE_NAVIGATION

#include "NavDetourSupport.h"
#include "NavPolyState.h"

#include "DetourAlloc.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourNode.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"

namespace {

    constexpr int MAX_PATH_POLYS = 2048;
    constexpr int MAX_CORNERS = 2048;
    constexpr int QUERY_NODES = 2048;
    // dtTileCache's request and update lists hold 64 entries, and an obstacle touches at most
    // DT_MAX_TOUCHED_TILES (8) layers: 8 requests at a time never overflow either (spec S74 R11)
    constexpr int TILE_CACHE_LIST = 64;
    constexpr int OBSTACLE_BATCH = TILE_CACHE_LIST / DT_MAX_TOUCHED_TILES;
    // the tile-cache layers of one tile (buildNavMeshTilesAt's own limit)
    constexpr int MAX_TILE_LAYERS = 32;

    std::atomic<uint64_t> revisions{1};
    std::atomic<uint64_t> generations{1};

    // ---- S76 queries
    // the most tries findRandomPointAround makes for a point within the radius (and island)
    constexpr int RANDOM_AROUND_TRIES = 8;
    // the polygons moveAlongSurface may cross (Detour's tiny node pool holds 64)
    constexpr int MOVE_MAX_VISITED = 64;
    // area tables a mesh keeps for anywhere random points (spec S76 R39)
    constexpr size_t MAX_RANDOM_TABLES = 4;

    // Detour's random callbacks take no context: the stream of the call in progress (main thread)
    thread_local NavRandom* activeRandom = nullptr;
    float drawActiveRandom() {
        return activeRandom->nextFloat();
    }

    // The area a query reports for a polygon: its flag bit (an area is a flag; a link has the flag
    // of its area while its polygon area holds its cost), Not Walkable for flags 0 (spec S76 D3)
    uint8_t reportedArea(const dtPoly* poly) {
        const unsigned short flags = poly->flags;
        if (flags == 0)
            return AREA_NOT_WALKABLE;
        return (uint8_t)std::countr_zero((unsigned int)flags);
    }

    // Why Detour cannot hold this static tile safely, or null when it can: Detour's addTile and
    // its queries take every count and index in the data as they are
    const char* refuseTileData(const NavTileBlob& tile, int maxPolys) {
        const std::vector<uint8_t>& data = tile.data;
        if (data.size() < sizeof(dtMeshHeader))
            return "the tile is shorter than its header";
        dtMeshHeader header;
        std::memcpy(&header, data.data(), sizeof(header));
        if (header.magic != DT_NAVMESH_MAGIC || header.version != DT_NAVMESH_VERSION)
            return "the tile is not Detour tile data of this version";
        if (header.x != tile.x || header.y != tile.y || header.layer != tile.layer)
            return "the tile's coordinates are not its record's";
        const int counts[] = {header.polyCount, header.vertCount, header.maxLinkCount, header.detailMeshCount,
                              header.detailVertCount, header.detailTriCount, header.bvNodeCount, header.offMeshConCount,
                              header.offMeshBase};
        for (const int count : counts) {
            if (count < 0 || count > (1 << 20))
                return "a count in the tile's header is out of range";
        }
        if (header.polyCount > maxPolys || header.maxLinkCount < 1 || header.offMeshBase + header.offMeshConCount != header.polyCount ||
            header.detailMeshCount != header.offMeshBase || header.bvNodeCount > header.polyCount * 2)
            return "the tile's header does not describe a tile this engine builds";

        const size_t headerSize = (size_t)dtAlign4(sizeof(dtMeshHeader));
        const size_t vertsSize = (size_t)dtAlign4(sizeof(float) * 3 * header.vertCount);
        const size_t polysSize = (size_t)dtAlign4(sizeof(dtPoly) * header.polyCount);
        const size_t linksSize = (size_t)dtAlign4(sizeof(dtLink) * header.maxLinkCount);
        const size_t detailMeshesSize = (size_t)dtAlign4(sizeof(dtPolyDetail) * header.detailMeshCount);
        const size_t detailVertsSize = (size_t)dtAlign4(sizeof(float) * 3 * header.detailVertCount);
        const size_t detailTrisSize = (size_t)dtAlign4(sizeof(unsigned char) * 4 * header.detailTriCount);
        const size_t bvTreeSize = (size_t)dtAlign4(sizeof(dtBVNode) * header.bvNodeCount);
        const size_t offMeshSize = (size_t)dtAlign4(sizeof(dtOffMeshConnection) * header.offMeshConCount);
        if (headerSize + vertsSize + polysSize + linksSize + detailMeshesSize + detailVertsSize + detailTrisSize + bvTreeSize +
                offMeshSize != data.size())
            return "the tile's sections do not add up to its size";

        const uint8_t* vertsAt = data.data() + headerSize;
        const uint8_t* polysAt = vertsAt + vertsSize;
        const uint8_t* detailMeshesAt = polysAt + polysSize + linksSize;
        const uint8_t* detailTrisAt = detailMeshesAt + detailMeshesSize + detailVertsSize;
        const uint8_t* bvTreeAt = detailTrisAt + detailTrisSize;
        const uint8_t* offMeshAt = bvTreeAt + bvTreeSize;
        for (int v = 0; v < header.vertCount * 3; v++) {
            float value;
            std::memcpy(&value, vertsAt + sizeof(float) * (size_t)v, sizeof(value));
            if (!std::isfinite(value))
                return "a vertex of the tile is not finite";
        }
        for (int p = 0; p < header.polyCount; p++) {
            dtPoly poly;
            std::memcpy(&poly, polysAt + sizeof(dtPoly) * (size_t)p, sizeof(poly));
            const bool offMesh = p >= header.offMeshBase;
            if (offMesh) {
                if (poly.getType() != DT_POLYTYPE_OFFMESH_CONNECTION || poly.vertCount != 2)
                    return "an off-mesh polygon of the tile is malformed";
            } else if (poly.vertCount < 3 || poly.vertCount > DT_VERTS_PER_POLYGON || poly.getType() != DT_POLYTYPE_GROUND) {
                return "a polygon of the tile is malformed";
            }
            if (poly.getArea() >= MAX_POLY_AREAS)
                return "a polygon of the tile has an unknown area";
            for (unsigned int v = 0; v < poly.vertCount; v++) {
                if (poly.verts[v] >= header.vertCount)
                    return "a polygon of the tile names a vertex it does not have";
                const unsigned short neighbour = poly.neis[v];
                if (!offMesh && neighbour != 0 && (neighbour & DT_EXT_LINK) == 0 && neighbour > header.offMeshBase)
                    return "a polygon of the tile names a neighbour it does not have";
            }
            if (offMesh)
                continue;
            dtPolyDetail detail;
            std::memcpy(&detail, detailMeshesAt + sizeof(dtPolyDetail) * (size_t)p, sizeof(detail));
            if ((uint64_t)detail.vertBase + detail.vertCount > (uint64_t)header.detailVertCount ||
                (uint64_t)detail.triBase + detail.triCount > (uint64_t)header.detailTriCount)
                return "a detail mesh of the tile lies outside its arrays";
            for (unsigned int t = 0; t < detail.triCount; t++) {
                const uint8_t* triangle = detailTrisAt + 4 * ((size_t)detail.triBase + t);
                for (int corner = 0; corner < 3; corner++) {
                    if (triangle[corner] >= poly.vertCount + detail.vertCount)
                        return "a detail triangle of the tile names a vertex it does not have";
                }
            }
        }
        for (int n = 0; n < header.bvNodeCount; n++) {
            dtBVNode node;
            std::memcpy(&node, bvTreeAt + sizeof(dtBVNode) * (size_t)n, sizeof(node));
            if (node.i >= header.offMeshBase || node.i < -header.bvNodeCount)
                return "a bounding-volume node of the tile names a polygon it does not have";
        }
        for (int c = 0; c < header.offMeshConCount; c++) {
            dtOffMeshConnection con;
            std::memcpy(&con, offMeshAt + sizeof(dtOffMeshConnection) * (size_t)c, sizeof(con));
            if (con.poly < header.offMeshBase || con.poly >= header.polyCount)
                return "an off-mesh connection of the tile names a polygon it does not have";
            for (float v : con.pos) {
                if (!std::isfinite(v))
                    return "an off-mesh connection of the tile is not finite";
            }
        }
        return nullptr;
    }

    const char* refuseLayerData(const NavTileBlob& tile, int tileSize) {
        if (tile.data.size() < sizeof(dtTileCacheLayerHeader))
            return "the layer is shorter than its header";
        dtTileCacheLayerHeader header;
        std::memcpy(&header, tile.data.data(), sizeof(header));
        if (header.magic != DT_TILECACHE_MAGIC || header.version != DT_TILECACHE_VERSION)
            return "the layer is not Detour tile-cache data of this version";
        if (header.tx != tile.x || header.ty != tile.y || header.tlayer != tile.layer)
            return "the layer's coordinates are not its record's";
        if (header.width != tileSize || header.height != tileSize || header.minx > header.maxx || header.maxx >= header.width ||
            header.miny > header.maxy || header.maxy >= header.height)
            return "the layer's size is not the surface's tile size";
        for (int a = 0; a < 3; a++) {
            if (!std::isfinite(header.bmin[a]) || !std::isfinite(header.bmax[a]))
                return "the layer's bounds are not finite";
        }
        return nullptr;
    }

    // Detour's own classification of an off-mesh end against a tile (dtCreateNavMeshData)
    unsigned char classifyOffMeshPoint(const float* pt, const float* bmin, const float* bmax) {
        constexpr unsigned char XP = 1 << 0, ZP = 1 << 1, XM = 1 << 2, ZM = 1 << 3;
        unsigned char outcode = 0;
        outcode |= (pt[0] >= bmax[0]) ? XP : 0;
        outcode |= (pt[2] >= bmax[2]) ? ZP : 0;
        outcode |= (pt[0] < bmin[0]) ? XM : 0;
        outcode |= (pt[2] < bmin[2]) ? ZM : 0;
        switch (outcode) {
            case XP: return 0;
            case XP | ZP: return 1;
            case ZP: return 2;
            case XM | ZP: return 3;
            case XM: return 4;
            case XM | ZM: return 5;
            case ZM: return 6;
            case XP | ZM: return 7;
        }
        return 0xff;
    }

    unsigned int nextPow2(unsigned int v) {
        if (v <= 1) return 1;
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
        return v + 1;
    }

    struct TileKey {
        int x, y;
        bool operator<(const TileKey& o) const { return y < o.y || (y == o.y && x < o.x); }
    };

}

struct NavMesh::Impl {
    NavMeshHeader header;
    bool valid = false;
    bool keepData = false;
    NavMeshData data;
    dtNavMesh* navMesh = nullptr;
    mutable dtNavMeshQuery* query = nullptr;
    dtTileCache* tileCache = nullptr;
    detail::DeflateCompressor compressor;
    detail::LinearAllocator allocator;
    detail::MeshProcess meshProcess;
    // carving: the links of each tile (the mesh process adds them to the tiles it builds)
    std::map<TileKey, std::vector<NavLinkInput>> links;
    uint64_t revision = 0;
    uint64_t generation = 0;

    // ---- carving obstacles (spec S74 R11-R17): the registry, its request FIFO, the tile-cache
    // builds Detour's update list holds (mirrored, in its order: each update() builds the first),
    // and the tiles whose build is deferred (R15)
    struct Obstacle {
        NavObstacleDesc desc;
        // 0 until its add reaches the tile cache
        dtObstacleRef ref = 0;
        bool addQueued = false;
    };
    struct Request {
        bool add = true;
        uint32_t id = 0;
        dtObstacleRef ref = 0;
        uint64_t serial = 0;
    };
    std::map<uint32_t, Obstacle> obstacles;
    uint32_t nextObstacle = 1;
    std::deque<Request> requests;
    std::vector<dtCompressedTileRef> pending;
    std::deque<TileKey> deferred;
    bool deferBuilds = false;
    // request serials: the last queued, the last whose builds are done, and the one isComplete()
    // waits for (an adoption's, a re-request's)
    uint64_t requestSerial = 0;
    uint64_t batchSerial = 0;
    uint64_t doneSerial = 0;
    uint64_t barrierSerial = 0;
    int failedBuilds = 0;
    std::set<std::pair<int, int>> failedTiles;
    // whether some tile has off-mesh connections, at linksRevision (a crowd asks every tick)
    mutable uint64_t linksRevision = UINT64_MAX;
    mutable bool hasLinks = false;
    // S75: the island labels of `labelledRevision` (made on demand, R8), the searches run
    mutable NavIslandLabels islands;
    mutable uint64_t labelledRevision = 0;
    mutable uint64_t searches = 0;
    // S76: runtime overrides and link states, made on the first one (R18)
    std::unique_ptr<detail::NavPolyState> polyState;
    // ---- S76 queries: the passable polygons of a mask (and island) with their cumulative areas,
    // for random points uniform by area; dropped when the revision changes
    struct RandomTable {
        uint16_t mask = 0;
        int island = -1;
        uint64_t lastUse = 0;
        std::vector<dtPolyRef> refs;
        std::vector<float> cumulative;
    };
    mutable std::vector<RandomTable> randomTables;
    mutable uint64_t randomTablesRevision = 0;
    mutable uint64_t randomTableUses = 0;

    ~Impl() { release(); }

    // S76 R14: overrides and link states over the tiles just added or rebuilt; true when a
    // polygon changed (the caller bumps the revision)
    bool tilesChanged() {
        if (!polyState)
            return false;
        bindPolyState();
        return polyState->onTilesChanged();
    }

    void bindPolyState() {
        const NavBuildConfig& c = header.config;
        // a polygon's centroid within a step of a volume's height range is in it
        polyState->bind(navMesh, c.tileWorldSize(), std::max(c.agentClimb, c.cellHeight));
    }

    detail::NavPolyState& polys() {
        if (!polyState)
            polyState = std::make_unique<detail::NavPolyState>();
        bindPolyState();
        return *polyState;
    }

    void release() {
        if (polyState)
            polyState->forget();
        islands = NavIslandLabels();
        labelledRevision = 0;
        if (query) dtFreeNavMeshQuery(query);
        if (tileCache) dtFreeTileCache(tileCache);
        if (navMesh) dtFreeNavMesh(navMesh);
        query = nullptr;
        tileCache = nullptr;
        navMesh = nullptr;
        valid = false;
        links.clear();
        randomTables.clear();
        requests.clear();
        pending.clear();
        deferred.clear();
        failedTiles.clear();
        failedBuilds = 0;
        doneSerial = batchSerial = barrierSerial = requestSerial;
        // the registry outlives its tile cache: a new one cuts every obstacle again
        for (auto& entry : obstacles) {
            entry.second.ref = 0;
            entry.second.addQueued = false;
        }
    }

    // ---- carving

    void queueAdd(uint32_t id, Obstacle& obstacle) {
        if (obstacle.addQueued)
            return;
        obstacle.addQueued = true;
        requests.push_back(Request{true, id, 0, ++requestSerial});
    }

    void queueRemove(Obstacle& obstacle) {
        if (obstacle.ref == 0)
            return;
        requests.push_back(Request{false, 0, obstacle.ref, ++requestSerial});
        obstacle.ref = 0;
    }

    // every obstacle cut again from its description (a new tile cache, replaced layers)
    void requeueObstacles(const std::function<bool(const Obstacle&)>& which) {
        for (auto& entry : obstacles) {
            if (!which(entry.second))
                continue;
            queueRemove(entry.second);
            queueAdd(entry.first, entry.second);
        }
        barrierSerial = requestSerial;
    }

    // Detour's bounds of an obstacle (dtTileCache::getObstacleBounds)
    void obstacleBounds(const NavObstacleDesc& desc, float* bmin, float* bmax) const {
        if (desc.shape == NavObstacleDesc::Shape::Box) {
            const float r = 1.41f * std::max(desc.halfExtents.x, desc.halfExtents.z);
            bmin[0] = desc.position.x - r;
            bmin[1] = desc.position.y - desc.halfExtents.y;
            bmin[2] = desc.position.z - r;
            bmax[0] = desc.position.x + r;
            bmax[1] = desc.position.y + desc.halfExtents.y;
            bmax[2] = desc.position.z + r;
        } else {
            bmin[0] = desc.position.x - desc.radius;
            bmin[1] = desc.position.y;
            bmin[2] = desc.position.z - desc.radius;
            bmax[0] = desc.position.x + desc.radius;
            bmax[1] = desc.position.y + desc.height;
            bmax[2] = desc.position.z + desc.radius;
        }
    }

    // the tile-cache layers whose bounds the obstacle's meet (dtTileCache::queryTiles, unbounded)
    int countLayers(const NavObstacleDesc& desc) const {
        float bmin[3], bmax[3];
        obstacleBounds(desc, bmin, bmax);
        const float size = header.config.tileWorldSize();
        const int x0 = (int)std::floor(bmin[0] / size), x1 = (int)std::floor(bmax[0] / size);
        const int y0 = (int)std::floor(bmin[2] / size), y1 = (int)std::floor(bmax[2] / size);
        int count = 0;
        dtCompressedTileRef refs[MAX_TILE_LAYERS];
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const int n = tileCache->getTilesAt(x, y, refs, MAX_TILE_LAYERS);
                for (int i = 0; i < n; i++) {
                    float tmin[3], tmax[3];
                    tileCache->calcTightTileBounds(tileCache->getTileByRef(refs[i])->header, tmin, tmax);
                    if (dtOverlapBounds(bmin, bmax, tmin, tmax))
                        count++;
                }
            }
        }
        return count;
    }

    void expect(const dtCompressedTileRef* refs, int count) {
        for (int i = 0; i < count; i++) {
            if ((int)pending.size() < TILE_CACHE_LIST && std::find(pending.begin(), pending.end(), refs[i]) == pending.end())
                pending.push_back(refs[i]);
        }
    }

    // Hands up to OBSTACLE_BATCH requests to the idle tile cache and mirrors the builds they cause.
    // False when nothing could be submitted (a request Detour refused stays first, for later).
    bool submitBatch() {
        int submitted = 0;
        uint64_t last = 0;
        while (!requests.empty() && submitted < OBSTACLE_BATCH) {
            const Request request = requests.front();
            if (request.add) {
                auto found = obstacles.find(request.id);
                if (found == obstacles.end() || !found->second.addQueued) {
                    // removed (or added again) since it was queued
                    last = request.serial;
                    requests.pop_front();
                    continue;
                }
                Obstacle& obstacle = found->second;
                // no layer under it: nothing to cut (and Detour would never free its slot)
                if (countLayers(obstacle.desc) == 0) {
                    obstacle.addQueued = false;
                    last = request.serial;
                    requests.pop_front();
                    continue;
                }
                const dtObstacleRef ref = submit(obstacle.desc);
                if (!ref)
                    break;
                obstacle.ref = ref;
                obstacle.addQueued = false;
                // the layers Detour's add will touch: its own query, as update() runs it next
                const dtTileCacheObstacle* added = tileCache->getObstacleByRef(ref);
                float bmin[3], bmax[3];
                tileCache->getObstacleBounds(added, bmin, bmax);
                dtCompressedTileRef touched[DT_MAX_TOUCHED_TILES];
                int count = 0;
                tileCache->queryTiles(bmin, bmax, touched, &count, DT_MAX_TOUCHED_TILES);
                expect(touched, count);
            } else {
                const dtTileCacheObstacle* removed = tileCache->getObstacleByRef(request.ref);
                if (dtStatusFailed(tileCache->removeObstacle(request.ref)))
                    break;
                if (removed)
                    expect(removed->touched, removed->ntouched);
            }
            last = request.serial;
            requests.pop_front();
            submitted++;
        }
        if (last != 0)
            batchSerial = last;
        if (submitted == 0) {
            doneSerial = batchSerial;
            return false;
        }
        if (pending.empty()) {
            // requests without a build: the tile cache takes them now
            bool upToDate = false;
            tileCache->update(0.0f, navMesh, &upToDate);
            doneSerial = batchSerial;
        }
        return true;
    }

    void buildFailed(int x, int y, int layer, dtStatus status) {
        failedBuilds++;
        if (failedTiles.insert({x, y}).second)
            Log::error("Navigation: the tile cache could not rebuild tile (%d, %d) layer %d (Detour status 0x%x); it keeps its "
                       "previous polygons", x, y, layer, (unsigned)status);
    }

    // the carved Detour tiles of (x, y) from their layers, and none for a layer that is gone
    void buildTile(const TileKey& key) {
        dtCompressedTileRef refs[MAX_TILE_LAYERS];
        const int count = tileCache->getTilesAt(key.x, key.y, refs, MAX_TILE_LAYERS);
        const dtMeshTile* tiles[MAX_TILE_LAYERS * 2];
        const int built = navMesh->getTilesAt(key.x, key.y, tiles, MAX_TILE_LAYERS * 2);
        std::vector<dtTileRef> gone;
        for (int t = 0; t < built; t++) {
            bool kept = false;
            for (int i = 0; i < count && !kept; i++)
                kept = tileCache->getTileByRef(refs[i])->header->tlayer == tiles[t]->header->layer;
            if (!kept)
                gone.push_back(navMesh->getTileRef(tiles[t]));
        }
        for (dtTileRef ref : gone)
            navMesh->removeTile(ref, nullptr, nullptr);
        for (int i = 0; i < count; i++) {
            const dtStatus status = tileCache->buildNavMeshTile(refs[i], navMesh);
            if (dtStatusFailed(status))
                buildFailed(key.x, key.y, tileCache->getTileByRef(refs[i])->header->tlayer, status);
        }
    }

    void deferTile(const TileKey& key) {
        for (const TileKey& queued : deferred) {
            if (queued.x == key.x && queued.y == key.y)
                return;
        }
        deferred.push_back(key);
    }

    void bump() { revision = revisions.fetch_add(1, std::memory_order_relaxed) + 1; }
    void newGeneration() { generation = generations.fetch_add(1, std::memory_order_relaxed) + 1; }

    dtObstacleRef submit(const NavObstacleDesc& desc) {
        dtObstacleRef ref = 0;
        if (desc.shape == NavObstacleDesc::Shape::Box) {
            const float c[3] = {desc.position.x, desc.position.y, desc.position.z};
            const float h[3] = {desc.halfExtents.x, desc.halfExtents.y, desc.halfExtents.z};
            if (dtStatusFailed(tileCache->addBoxObstacle(c, h, desc.yRadians, &ref)))
                return 0;
        } else {
            const float p[3] = {desc.position.x, desc.position.y, desc.position.z};
            if (dtStatusFailed(tileCache->addObstacle(p, desc.radius, desc.height, &ref)))
                return 0;
        }
        return ref;
    }

    void fillFilter(const NavFilter& filter, dtQueryFilter& out) const {
        out.setIncludeFlags(filter.areaMask);
        out.setExcludeFlags(0);
        for (int i = 0; i < AREA_COUNT; i++)
            out.setAreaCost(i, std::max(1.0f, filter.areaCosts[i]));
        for (size_t i = 0; i < header.linkAreaCosts.size() && FIRST_LINK_COST_AREA + (int)i < MAX_POLY_AREAS; i++)
            out.setAreaCost(FIRST_LINK_COST_AREA + (int)i, std::max(0.0f, header.linkAreaCosts[i]));
    }

    bool initNavMesh(int maxTiles, std::string& error) {
        dtNavMeshParams params;
        std::memset(&params, 0, sizeof(params));
        params.tileWidth = header.config.tileWorldSize();
        params.tileHeight = params.tileWidth;
        params.maxTiles = maxTiles;
        params.maxPolys = header.config.maxPolysPerTile;
        const std::string tooLarge = checkPolyRefBits(maxTiles, params.maxPolys);
        if (!tooLarge.empty()) {
            error = tooLarge;
            return false;
        }
        navMesh = dtAllocNavMesh();
        if (!navMesh || dtStatusFailed(navMesh->init(&params))) {
            error = "Detour could not create the navmesh (" + std::to_string(maxTiles) + " tiles)";
            return false;
        }
        query = dtAllocNavMeshQuery();
        if (!query || dtStatusFailed(query->init(navMesh, QUERY_NODES))) {
            error = "Detour could not create the navmesh query";
            return false;
        }
        return true;
    }

    bool initTileCache(int maxTiles, std::string& error) {
        dtTileCacheParams params;
        std::memset(&params, 0, sizeof(params));
        const NavBuildConfig& c = header.config;
        params.cs = c.cellSize;
        params.ch = c.cellHeight;
        params.width = c.tileSize;
        params.height = c.tileSize;
        params.walkableHeight = (float)c.walkableHeight * c.cellHeight;
        params.walkableRadius = (float)c.walkableRadius * c.cellSize;
        params.walkableClimb = (float)c.walkableClimb * c.cellHeight;
        params.maxSimplificationError = c.maxSimplificationError;
        params.maxTiles = maxTiles;
        // room for the removals in flight beside Max Carving Obstacles (a batch removes at most
        // OBSTACLE_BATCH, freed once their tiles are built): an add never runs out of slots
        params.maxObstacles = std::max(1, header.maxObstacles) + OBSTACLE_BATCH;
        meshProcess.linksAt = [this](int x, int y) -> const std::vector<NavLinkInput>* {
            auto found = links.find(TileKey{x, y});
            return found != links.end() ? &found->second : nullptr;
        };
        tileCache = dtAllocTileCache();
        if (!tileCache || dtStatusFailed(tileCache->init(&params, &allocator, &compressor, &meshProcess))) {
            error = "Detour could not create the tile cache";
            return false;
        }
        return true;
    }

    // a static tile into Detour (its own copy of the bytes)
    bool addStaticTile(const NavTileBlob& blob, std::string& error) {
        if (blob.data.empty())
            return true;
        if (const char* refused = refuseTileData(blob, header.config.maxPolysPerTile)) {
            error = "tile (" + std::to_string(blob.x) + ", " + std::to_string(blob.y) + "): " + refused;
            return false;
        }
        unsigned char* copy = (unsigned char*)dtAlloc(blob.data.size(), DT_ALLOC_PERM);
        if (!copy) {
            error = "out of memory for a navmesh tile";
            return false;
        }
        std::memcpy(copy, blob.data.data(), blob.data.size());
        if (dtStatusFailed(navMesh->addTile(copy, (int)blob.data.size(), DT_TILE_FREE_DATA, 0, nullptr))) {
            dtFree(copy);
            error = "Detour refused tile (" + std::to_string(blob.x) + ", " + std::to_string(blob.y) + ")";
            return false;
        }
        return true;
    }

    bool addLayer(const NavTileBlob& blob, std::string& error) {
        if (blob.data.empty())
            return true;
        if (const char* refused = refuseLayerData(blob, header.config.tileSize)) {
            error = "tile (" + std::to_string(blob.x) + ", " + std::to_string(blob.y) + ") layer " + std::to_string(blob.layer) + ": " + refused;
            return false;
        }
        unsigned char* copy = (unsigned char*)dtAlloc(blob.data.size(), DT_ALLOC_PERM);
        if (!copy) {
            error = "out of memory for a tile-cache layer";
            return false;
        }
        std::memcpy(copy, blob.data.data(), blob.data.size());
        if (dtStatusFailed(tileCache->addTile(copy, (int)blob.data.size(), DT_COMPRESSEDTILE_FREE_DATA, nullptr))) {
            dtFree(copy);
            error = "Detour refused tile-cache layer (" + std::to_string(blob.x) + ", " + std::to_string(blob.y) + ", " +
                    std::to_string(blob.layer) + ")";
            return false;
        }
        if (blob.layer == 0 && !blob.links.empty())
            links[TileKey{blob.x, blob.y}] = blob.links;
        return true;
    }

    void removeNavTilesAt(int x, int y) {
        const dtMeshTile* tiles[64];
        const int count = navMesh->getTilesAt(x, y, tiles, 64);
        for (int i = 0; i < count; i++)
            navMesh->removeTile(navMesh->getTileRef(tiles[i]), nullptr, nullptr);
    }

    void removeLayersAt(int x, int y) {
        dtCompressedTileRef refs[256];
        const int count = tileCache->getTilesAt(x, y, refs, 256);
        for (int i = 0; i < count; i++)
            tileCache->removeTile(refs[i], nullptr, nullptr);
        links.erase(TileKey{x, y});
    }

    bool buildCarvedTiles(const std::vector<TileKey>& keys, std::string& error) {
        for (const TileKey& key : keys) {
            removeNavTilesAt(key.x, key.y);
            if (dtStatusFailed(tileCache->buildNavMeshTilesAt(key.x, key.y, navMesh))) {
                error = "Detour could not build tile (" + std::to_string(key.x) + ", " + std::to_string(key.y) + ") from its layers";
                return false;
            }
        }
        return true;
    }

    int layerCount(const NavMeshData& source) const {
        return (int)source.tiles.size();
    }

    // the tile under a nav-space point, for the queries' start polygons
    dtPolyRef nearest(const Vector3& point, const Vector3& halfExtents, const dtQueryFilter& filter, float* nearestPoint) const {
        dtPolyRef ref = 0;
        const float p[3] = {point.x, point.y, point.z};
        const float extents[3] = {halfExtents.x, halfExtents.y, halfExtents.z};
        if (dtStatusFailed(query->findNearestPoly(p, extents, &filter, &ref, nearestPoint)))
            return 0;
        return ref;
    }

    bool labelsCurrent() const { return valid && labelledRevision == revision && revision != 0; }

    void ensureLabels() const {
        if (!valid || labelsCurrent())
            return;
        labelIslands(navMesh, islands);
        labelledRevision = revision;
    }

    // the island of a polygon when the labels are current, else -1 (no labelling)
    int islandOf(dtPolyRef ref) const {
        if (!ref || !labelsCurrent())
            return -1;
        unsigned int salt = 0, it = 0, ip = 0;
        navMesh->decodePolyId(ref, salt, it, ip);
        return islands.labelOf(it, ip);
    }

    // whether a polygon of any area is in the box (an end the mask filtered, R3 step 3)
    bool anyPolyNear(const Vector3& point, const Vector3& halfExtents, const dtQueryFilter& filter) const {
        dtQueryFilter all = filter;
        all.setIncludeFlags(ALL_AREAS);
        float ignored[3];
        return nearest(point, halfExtents, all, ignored) != 0;
    }

    uint8_t areaOf(dtPolyRef ref) const {
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (!ref || dtStatusFailed(navMesh->getTileAndPolyByRef(ref, &tile, &poly)))
            return AREA_WALKABLE;
        return reportedArea(poly);
    }

    // the table of a mask and island (S75's labels, -1: any) at this revision, built on first use
    const RandomTable& randomTable(uint16_t mask, int island, const dtQueryFilter& filter) const {
        if (randomTablesRevision != revision) {
            randomTables.clear();
            randomTablesRevision = revision;
        }
        randomTableUses++;
        for (RandomTable& table : randomTables) {
            if (table.mask == mask && table.island == island) {
                table.lastUse = randomTableUses;
                return table;
            }
        }
        if (randomTables.size() >= MAX_RANDOM_TABLES) {
            auto oldest = std::min_element(randomTables.begin(), randomTables.end(),
                                           [](const RandomTable& a, const RandomTable& b) { return a.lastUse < b.lastUse; });
            randomTables.erase(oldest);
        }
        RandomTable table;
        table.mask = mask;
        table.island = island;
        table.lastUse = randomTableUses;
        float total = 0.0f;
        const dtNavMesh* mesh = navMesh;
        for (int i = 0; i < mesh->getMaxTiles(); i++) {
            const dtMeshTile* tile = mesh->getTile(i);
            if (!tile || !tile->header)
                continue;
            const dtPolyRef base = mesh->getPolyRefBase(tile);
            for (int p = 0; p < tile->header->polyCount; p++) {
                const dtPoly* poly = &tile->polys[p];
                const dtPolyRef ref = base | (dtPolyRef)p;
                // dtQueryFilter::passFilter's test (Detour defines it inline in its .cpp)
                if (poly->getType() != DT_POLYTYPE_GROUND || (poly->flags & filter.getIncludeFlags()) == 0 ||
                    (poly->flags & filter.getExcludeFlags()) != 0)
                    continue;
                float area = 0.0f;
                const float* a = &tile->verts[poly->verts[0] * 3];
                for (int v = 2; v < poly->vertCount; v++)
                    area += std::fabs(dtTriArea2D(a, &tile->verts[poly->verts[v - 1] * 3], &tile->verts[poly->verts[v] * 3]));
                if (!(area > 0.0f))
                    continue;
                if (island >= 0 && islandOf(ref) != island)
                    continue;
                total += area;
                table.refs.push_back(ref);
                table.cumulative.push_back(total);
            }
        }
        randomTables.push_back(std::move(table));
        return randomTables.back();
    }

    Vector3 polyNormal(dtPolyRef ref, const float* position) const {
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusFailed(navMesh->getTileAndPolyByRef(ref, &tile, &poly)) || poly->getType() != DT_POLYTYPE_GROUND)
            return Vector3(0.0f, 1.0f, 0.0f);
        const unsigned int index = (unsigned int)(poly - tile->polys);
        auto normalOf = [](const float* a, const float* b, const float* c) {
            const Vector3 e0(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            const Vector3 e1(c[0] - a[0], c[1] - a[1], c[2] - a[2]);
            Vector3 n = e0.crossProduct(e1);
            const float length = n.length();
            if (length <= 0.0f) return Vector3(0.0f, 1.0f, 0.0f);
            n = n / length;
            return n.y < 0.0f ? -n : n;
        };
        if (tile->detailMeshes) {
            const dtPolyDetail& detail = tile->detailMeshes[index];
            for (int t = 0; t < detail.triCount; t++) {
                const unsigned char* tri = &tile->detailTris[(detail.triBase + (unsigned int)t) * 4];
                const float* v[3];
                for (int k = 0; k < 3; k++) {
                    if (tri[k] < poly->vertCount)
                        v[k] = &tile->verts[poly->verts[tri[k]] * 3];
                    else
                        v[k] = &tile->detailVerts[(detail.vertBase + (tri[k] - poly->vertCount)) * 3];
                }
                float h;
                if (dtClosestHeightPointTriangle(position, v[0], v[1], v[2], h))
                    return normalOf(v[0], v[1], v[2]);
            }
        }
        return normalOf(&tile->verts[poly->verts[0] * 3], &tile->verts[poly->verts[1] * 3], &tile->verts[poly->verts[2] * 3]);
    }
};

NavMesh::NavMesh(): impl(std::make_unique<Impl>()) {}
NavMesh::~NavMesh() = default;

bool NavMesh::create(NavMeshData data, bool keepData, std::string& error, const NavMeshCreateOptions& options) {
    impl->release();
    impl->newGeneration();
    impl->deferBuilds = options.deferCarvedBuilds;
    impl->data = NavMeshData();
    impl->header = data.header;
    if (options.maxObstacles >= 0)
        impl->header.maxObstacles = options.maxObstacles;
    impl->keepData = keepData;
    data.sortTiles();

    const NavBuildConfig& config = impl->header.config;
    const int records = impl->layerCount(data);
    // the pool keeps room for an incremental bake to add tiles
    const int maxTiles = (int)nextPow2((unsigned int)std::max({impl->header.maxTiles, records, 1}));
    impl->header.maxTiles = maxTiles;
    if (!impl->initNavMesh(maxTiles, error)) {
        impl->release();
        return false;
    }
    if (config.carving) {
        if (!impl->initTileCache(maxTiles, error)) {
            impl->release();
            return false;
        }
        std::vector<TileKey> keys;
        for (const NavTileBlob& blob : data.tiles) {
            if (!impl->addLayer(blob, error)) {
                impl->release();
                return false;
            }
            if (keys.empty() || keys.back().x != blob.x || keys.back().y != blob.y)
                keys.push_back(TileKey{blob.x, blob.y});
        }
        if (impl->deferBuilds) {
            // built by updateCarving, within the carving budget (spec S74 R15)
            for (const TileKey& key : keys)
                impl->deferTile(key);
        } else if (!impl->buildCarvedTiles(keys, error)) {
            impl->release();
            return false;
        }
        // a new tile cache: the obstacles kept from before (the pool-growth fallback) cut again
        // (spec S74 R12)
        impl->requeueObstacles([](const Impl::Obstacle&) { return true; });
    } else {
        for (const NavTileBlob& blob : data.tiles) {
            if (!impl->addStaticTile(blob, error)) {
                impl->release();
                return false;
            }
        }
    }
    data.header.maxTiles = maxTiles;
    if (keepData)
        impl->data = std::move(data);
    impl->valid = true;
    impl->tilesChanged();
    impl->bump();
    return true;
}

void NavMesh::clear() {
    impl->release();
    impl->newGeneration();
    impl->obstacles.clear();
    impl->data = NavMeshData();
    impl->header = NavMeshHeader();
    impl->bump();
}

bool NavMesh::isValid() const {
    return impl->valid;
}

bool NavMesh::isComplete() const {
    return impl->deferred.empty() && impl->doneSerial >= impl->barrierSerial;
}

int NavMesh::getQueuedBuilds() const {
    return (int)(impl->deferred.size() + impl->pending.size() + impl->requests.size());
}

uint64_t NavMesh::getGeneration() const {
    return impl->generation;
}

const NavMeshHeader& NavMesh::getHeader() const {
    return impl->header;
}

const NavMeshData* NavMesh::getData() const {
    return impl->keepData && impl->valid ? &impl->data : nullptr;
}

bool NavMesh::replaceTiles(const std::vector<NavTileCoord>& coords, std::vector<NavTileBlob> blobs, std::string& error) {
    if (!impl->valid || !impl->keepData) {
        error = "the navmesh keeps no tile records to replace";
        return false;
    }
    std::vector<TileKey> keys;
    for (const NavTileCoord& coord : coords)
        keys.push_back(TileKey{coord.x, coord.y});
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end(), [](const TileKey& a, const TileKey& b) { return a.x == b.x && a.y == b.y; }), keys.end());
    auto replaced = [&](const NavTileBlob& blob) {
        return std::binary_search(keys.begin(), keys.end(), TileKey{blob.x, blob.y});
    };

    NavMeshData next;
    next.header = impl->data.header;
    for (NavTileBlob& blob : impl->data.tiles) {
        if (!replaced(blob))
            next.tiles.push_back(std::move(blob));
    }
    for (NavTileBlob& blob : blobs)
        next.tiles.push_back(std::move(blob));
    next.sortTiles();

    // A pool too small for the new count, or a failed swap: the whole mesh again, in a new
    // Detour mesh. It builds at once even where builds are deferred: no old tile survives it.
    const auto rebuild = [&]() {
        NavMeshCreateOptions options;
        options.maxObstacles = impl->header.maxObstacles;
        const bool defer = impl->deferBuilds;
        const bool ok = create(std::move(next), true, error, options);
        impl->deferBuilds = defer;
        return ok;
    };
    if ((int)next.tiles.size() > impl->header.maxTiles) {
        next.header.maxTiles = (int)nextPow2((unsigned int)next.tiles.size());
        return rebuild();
    }

    if (impl->header.config.carving) {
        for (const TileKey& key : keys) {
            impl->removeLayersAt(key.x, key.y);
            // deferred: the old Detour tile serves until its rebuild replaces it (spec S74 R15)
            if (!impl->deferBuilds)
                impl->removeNavTilesAt(key.x, key.y);
        }
        for (const NavTileBlob& blob : next.tiles) {
            if (replaced(blob) && !impl->addLayer(blob, error))
                return rebuild();
        }
        if (impl->deferBuilds) {
            for (const TileKey& key : keys)
                impl->deferTile(key);
        } else if (!impl->buildCarvedTiles(keys, error)) {
            return rebuild();
        }
        // the new layers have new refs, which no obstacle touches yet: the obstacles over them cut
        // again (spec S74 R12)
        const float size = impl->header.config.tileWorldSize();
        impl->requeueObstacles([&](const Impl::Obstacle& obstacle) {
            float bmin[3], bmax[3];
            impl->obstacleBounds(obstacle.desc, bmin, bmax);
            for (const TileKey& key : keys) {
                if (bmax[0] >= (float)key.x * size && bmin[0] < (float)(key.x + 1) * size && bmax[2] >= (float)key.y * size &&
                    bmin[2] < (float)(key.y + 1) * size)
                    return true;
            }
            return false;
        });
    } else {
        for (const TileKey& key : keys)
            impl->removeNavTilesAt(key.x, key.y);
        for (const NavTileBlob& blob : next.tiles) {
            if (replaced(blob) && !impl->addStaticTile(blob, error))
                return rebuild();
        }
    }
    impl->data = std::move(next);
    impl->tilesChanged();
    impl->bump();
    return true;
}

int NavMesh::getTileCount() const {
    if (!impl->valid) return 0;
    int count = 0;
    const dtNavMesh* mesh = impl->navMesh;
    for (int i = 0; i < mesh->getMaxTiles(); i++) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (tile && tile->header)
            count++;
    }
    return count;
}

int NavMesh::getPolyCount() const {
    if (!impl->valid) return 0;
    int count = 0;
    const dtNavMesh* mesh = impl->navMesh;
    for (int i = 0; i < mesh->getMaxTiles(); i++) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (tile && tile->header)
            count += tile->header->offMeshBase;
    }
    return count;
}

size_t NavMesh::getMemoryBytes() const {
    if (!impl->valid) return 0;
    size_t bytes = sizeof(dtNavMesh) + (size_t)impl->header.maxTiles * sizeof(dtMeshTile);
    const dtNavMesh* mesh = impl->navMesh;
    for (int i = 0; i < mesh->getMaxTiles(); i++) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (tile && tile->header)
            bytes += (size_t)tile->dataSize;
    }
    if (impl->tileCache) {
        for (int i = 0; i < impl->tileCache->getTileCount(); i++) {
            const dtCompressedTile* tile = impl->tileCache->getTile(i);
            if (tile && tile->header)
                bytes += (size_t)tile->dataSize;
        }
        bytes += (size_t)impl->tileCache->getObstacleCount() * sizeof(dtTileCacheObstacle);
    }
    // the query's node pool
    bytes += (size_t)QUERY_NODES * (sizeof(dtNode) + sizeof(dtNodeIndex) * 2) + (size_t)QUERY_NODES * sizeof(dtNode*);
    // random point area tables
    for (const Impl::RandomTable& table : impl->randomTables)
        bytes += table.refs.capacity() * sizeof(dtPolyRef) + table.cumulative.capacity() * sizeof(float);
    if (impl->keepData)
        bytes += impl->data.memoryBytes();
    if (impl->polyState)
        bytes += impl->polyState->memoryBytes();
    return bytes;
}

bool NavMesh::isCarving() const {
    return impl->valid && impl->tileCache != nullptr;
}

namespace {

    // a Detour failure on references the query found itself: the mesh is not what it should be
    void logDetourFailure(const char* call) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log::error("Navigation: Detour's %s failed on a valid navmesh; the path reports not_baked", call);
    }

    bool outOfNodes(dtStatus status) {
        return dtStatusDetail(status, DT_OUT_OF_NODES) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL);
    }

}

NavPath NavMesh::findPath(const Vector3& start, const Vector3& end, const NavFilter& filter, const Vector3& halfExtents,
                          const PathOptions& options) const {
    NavPath path;
    path.start = start;
    path.end = end;
    // an answer without corners: from the snapped start (else the requested one) to the end
    auto invalid = [&](PathFailure failure) {
        path.failure = failure;
        path.closestPoint = path.start;
        path.gap = path.start.distance(path.end);
        return path;
    };
    if (!impl->valid)
        return invalid(PathFailure::NotBaked);
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float startPoint[3], endPoint[3];
    const dtPolyRef startRef = impl->nearest(start, halfExtents, f, startPoint);
    const dtPolyRef endRef = impl->nearest(end, halfExtents, f, endPoint);
    if (startRef) {
        path.start = Vector3(startPoint[0], startPoint[1], startPoint[2]);
        path.startSnapDistance = path.start.distance(start);
        path.startIsland = impl->islandOf(startRef);
    }
    if (endRef) {
        path.end = Vector3(endPoint[0], endPoint[1], endPoint[2]);
        path.endSnapDistance = path.end.distance(end);
        path.endIsland = impl->islandOf(endRef);
    }
    // R3 step 3: Filtered when a polygon of another area is there
    if (!startRef)
        return invalid(impl->anyPolyNear(start, halfExtents, f) ? PathFailure::StartFiltered : PathFailure::StartOffMesh);
    if (!endRef)
        return invalid(impl->anyPolyNear(end, halfExtents, f) ? PathFailure::EndFiltered : PathFailure::EndOffMesh);

    static thread_local std::vector<dtPolyRef> polys;
    polys.resize(MAX_PATH_POLYS);
    int count = 0;
    impl->searches++;
    const dtStatus status = impl->query->findPath(startRef, endRef, startPoint, endPoint, &f, polys.data(), &count, MAX_PATH_POLYS);
    if (dtStatusFailed(status) || count == 0) {
        logDetourFailure("findPath");
        return invalid(PathFailure::NotBaked);
    }

    float target[3] = {endPoint[0], endPoint[1], endPoint[2]};
    const bool partial = polys[(size_t)count - 1] != endRef || dtStatusDetail(status, DT_PARTIAL_RESULT);
    if (polys[(size_t)count - 1] != endRef)
        impl->query->closestPointOnPoly(polys[(size_t)count - 1], endPoint, target, nullptr);

    // the corners, with Detour's flags and the polygon each one enters (S76 D3)
    static thread_local std::vector<float> corners;
    static thread_local std::vector<unsigned char> flags;
    static thread_local std::vector<dtPolyRef> refs;
    corners.resize((size_t)MAX_CORNERS * 3);
    flags.resize((size_t)MAX_CORNERS);
    refs.resize((size_t)MAX_CORNERS);
    int cornerCount = 0;
    const int cornerOptions = options.areaCrossings ? DT_STRAIGHTPATH_AREA_CROSSINGS : 0;
    if (dtStatusFailed(impl->query->findStraightPath(startPoint, target, polys.data(), count, corners.data(), flags.data(), refs.data(),
                                                      &cornerCount, MAX_CORNERS, cornerOptions)) || cornerCount == 0) {
        logDetourFailure("findStraightPath");
        return invalid(PathFailure::NotBaked);
    }
    const uint8_t endArea = impl->areaOf(polys[(size_t)count - 1]);
    path.corners.reserve((size_t)cornerCount);
    path.cornerAreas.reserve((size_t)cornerCount);
    path.cornerFlags.reserve((size_t)cornerCount);
    for (int i = 0; i < cornerCount; i++) {
        const Vector3 corner(corners[(size_t)i * 3], corners[(size_t)i * 3 + 1], corners[(size_t)i * 3 + 2]);
        if (!path.corners.empty())
            path.length += path.corners.back().distance(corner);
        path.corners.push_back(corner);
        path.cornerFlags.push_back(flags[(size_t)i]);
        path.cornerAreas.push_back(refs[(size_t)i] ? impl->areaOf(refs[(size_t)i]) : endArea);
    }

    // Cost and lengths per area come from a pass with a corner wherever the area changes: each
    // stretch then lies in the area of the polygon its first corner enters. A corridor of one
    // Detour area (the common case) needs no second pass.
    bool oneArea = true;
    {
        const dtMeshTile* tile = nullptr;
        const dtPoly* first = nullptr;
        impl->navMesh->getTileAndPolyByRefUnsafe(polys[0], &tile, &first);
        for (int i = 1; i < count && oneArea; i++) {
            const dtPoly* poly = nullptr;
            impl->navMesh->getTileAndPolyByRefUnsafe(polys[(size_t)i], &tile, &poly);
            oneArea = poly->getArea() == first->getArea();
        }
    }
    int crossingCount = cornerCount;
    if (!options.areaCrossings && !oneArea) {
        crossingCount = 0;
        if (dtStatusFailed(impl->query->findStraightPath(startPoint, target, polys.data(), count, corners.data(), nullptr, refs.data(),
                                                          &crossingCount, MAX_CORNERS, DT_STRAIGHTPATH_AREA_CROSSINGS)))
            crossingCount = 0;
    }
    for (int i = 0; i + 1 < crossingCount; i++) {
        const float* a = &corners[(size_t)i * 3];
        const float* b = &corners[(size_t)(i + 1) * 3];
        const dtPolyRef ref = refs[(size_t)i] ? refs[(size_t)i] : polys[(size_t)count - 1];
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        impl->navMesh->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
        const float length = dtVdist(a, b);
        path.cost += length * f.getAreaCost(poly->getArea());
        path.areaLengths[reportedArea(poly)] += length;
    }

    // the corridor before S75's all-areas search reuses the buffer
    path.polys.assign(polys.begin(), polys.begin() + count);
    path.meshRevision = impl->revision;

    path.closestPoint = path.corners.back();
    if (!partial) {
        path.status = PathStatus::Complete;
        path.failure = PathFailure::None;
        return path;
    }
    path.status = PathStatus::Partial;
    path.gap = path.closestPoint.distance(path.end);
    // R3 steps 4-6
    if (outOfNodes(status)) {
        path.failure = PathFailure::OutOfNodes;
    } else if (impl->polyState && !impl->polyState->idle()) {
        // S76 R27: a runtime volume (Not Walkable: flags 0, which no mask passes) or a deactivated
        // link may be what blocks it. Every area over the baked flags: a path there means an area
        // stops it, the mask's or a runtime override's
        dtQueryFilter all = f;
        all.setIncludeFlags(ALL_AREAS);
        int allCount = 0;
        dtStatus allStatus = DT_FAILURE;
        impl->searches++;
        impl->polyState->withBakedState([&]() {
            allStatus = impl->query->findPath(startRef, endRef, startPoint, endPoint, &all, polys.data(), &allCount, MAX_PATH_POLYS);
        });
        if (dtStatusSucceed(allStatus) && allCount > 0 && polys[(size_t)allCount - 1] == endRef && !dtStatusDetail(allStatus, DT_PARTIAL_RESULT))
            path.failure = PathFailure::BlockedByArea;
        else if (dtStatusSucceed(allStatus) && outOfNodes(allStatus))
            path.failure = PathFailure::OutOfNodes;
        else
            path.failure = PathFailure::Disconnected;
    } else if (filter.areaMask == ALL_AREAS) {
        path.failure = PathFailure::Disconnected;
    } else if (path.startIsland >= 0 && path.endIsland >= 0 && path.startIsland != path.endIsland) {
        // the islands say it at once: no second search
        path.failure = PathFailure::Disconnected;
    } else {
        // the same costs with every area: a path there means the mask blocks it
        dtQueryFilter all = f;
        all.setIncludeFlags(ALL_AREAS);
        int allCount = 0;
        impl->searches++;
        const dtStatus allStatus = impl->query->findPath(startRef, endRef, startPoint, endPoint, &all, polys.data(), &allCount, MAX_PATH_POLYS);
        if (dtStatusSucceed(allStatus) && allCount > 0 && polys[(size_t)allCount - 1] == endRef && !dtStatusDetail(allStatus, DT_PARTIAL_RESULT))
            path.failure = PathFailure::BlockedByArea;
        else if (dtStatusSucceed(allStatus) && outOfNodes(allStatus))
            path.failure = PathFailure::OutOfNodes;
        else
            path.failure = PathFailure::Disconnected;
    }
    return path;
}

NavHit NavMesh::samplePosition(const Vector3& point, float maxDistance, const NavFilter& filter) const {
    NavHit hit;
    if (!impl->valid || !(maxDistance > 0.0f))
        return hit;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float nearestPoint[3];
    const dtPolyRef ref = impl->nearest(point, Vector3(maxDistance, maxDistance, maxDistance), f, nearestPoint);
    if (!ref)
        return hit;
    const Vector3 position(nearestPoint[0], nearestPoint[1], nearestPoint[2]);
    const float distance = position.distance(point);
    if (distance > maxDistance)
        return hit;
    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;
    impl->navMesh->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
    hit.hit = true;
    hit.position = position;
    hit.distance = distance;
    hit.area = poly->getArea() < AREA_COUNT ? poly->getArea() : AREA_WALKABLE;
    hit.normal = impl->polyNormal(ref, nearestPoint);
    return hit;
}

NavHit NavMesh::raycast(const Vector3& from, const Vector3& to, const NavFilter& filter, const Vector3& halfExtents) const {
    NavHit hit;
    if (!impl->valid)
        return hit;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float startPoint[3];
    const dtPolyRef startRef = impl->nearest(from, halfExtents, f, startPoint);
    if (!startRef) {
        // Unity: a start off the mesh hits at once
        hit.hit = true;
        hit.position = from;
        return hit;
    }
    const float end[3] = {to.x, to.y, to.z};
    static thread_local std::vector<dtPolyRef> polys;
    polys.resize(MAX_PATH_POLYS);
    // with the costs of the polygons it crosses (S76 R6)
    dtRaycastHit rayHit;
    std::memset(&rayHit, 0, sizeof(rayHit));
    rayHit.path = polys.data();
    rayHit.maxPath = MAX_PATH_POLYS;
    if (dtStatusFailed(impl->query->raycast(startRef, startPoint, end, &f, DT_RAYCAST_USE_COSTS, &rayHit)))
        return hit;
    const float t = rayHit.t;
    const float* normal = rayHit.hitNormal;
    const int count = rayHit.pathCount;
    hit.cost = rayHit.pathCost;
    const Vector3 start(startPoint[0], startPoint[1], startPoint[2]);
    const Vector3 delta = to - start;
    if (t == std::numeric_limits<float>::max() || t > 1.0f) {
        // reached the end: the position is on the last polygon under `to`
        hit.hit = false;
        float height = to.y;
        if (count > 0)
            impl->query->getPolyHeight(polys[(size_t)count - 1], end, &height);
        hit.position = Vector3(to.x, height, to.z);
        hit.distance = Vector3(delta.x, 0.0f, delta.z).length();
        if (count > 0) {
            const dtMeshTile* tile = nullptr;
            const dtPoly* poly = nullptr;
            impl->navMesh->getTileAndPolyByRefUnsafe(polys[(size_t)count - 1], &tile, &poly);
            hit.area = poly->getArea() < AREA_COUNT ? poly->getArea() : AREA_WALKABLE;
        }
        return hit;
    }
    hit.hit = true;
    Vector3 position = start + delta * t;
    if (count > 0) {
        float height = position.y;
        const float p[3] = {position.x, position.y, position.z};
        if (dtStatusSucceed(impl->query->getPolyHeight(polys[(size_t)count - 1], p, &height)))
            position.y = height;
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        impl->navMesh->getTileAndPolyByRefUnsafe(polys[(size_t)count - 1], &tile, &poly);
        hit.area = poly->getArea() < AREA_COUNT ? poly->getArea() : AREA_WALKABLE;
    }
    hit.position = position;
    hit.normal = Vector3(normal[0], normal[1], normal[2]);
    hit.distance = Vector3(delta.x, 0.0f, delta.z).length() * t;
    return hit;
}

NavHit NavMesh::findClosestEdge(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents, float maxDistance) const {
    NavHit hit;
    if (!impl->valid)
        return hit;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float startPoint[3];
    const dtPolyRef startRef = impl->nearest(point, halfExtents, f, startPoint);
    if (!startRef)
        return hit;
    // within maxDistance, else as far as the query's node pool reaches
    const float radius = maxDistance > 0.0f ? maxDistance : impl->header.config.tileWorldSize() * 64.0f;
    float distance = 0.0f;
    // Detour leaves the position alone when no wall is within the radius
    constexpr float NONE = std::numeric_limits<float>::max();
    float position[3] = {NONE, NONE, NONE}, normal[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(impl->query->findDistanceToWall(startRef, startPoint, radius, &f, &distance, position, normal)))
        return hit;
    hit.area = impl->areaOf(startRef);
    if (position[0] == NONE) {
        hit.distance = radius;
        hit.position = Vector3(startPoint[0], startPoint[1], startPoint[2]);
        hit.normal = Vector3(0.0f, 1.0f, 0.0f);
        return hit;
    }
    hit.hit = true;
    hit.distance = distance;
    hit.position = Vector3(position[0], position[1], position[2]);
    hit.normal = Vector3(normal[0], normal[1], normal[2]);
    return hit;
}

bool NavMesh::findRandomPoint(const NavFilter& filter, NavRandom& random, int island, NavHit& out) const {
    out = NavHit();
    if (!impl->valid)
        return false;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    const Impl::RandomTable& table = impl->randomTable(filter.areaMask, island, f);
    if (table.refs.empty())
        return false;
    // draw 1: a polygon by area; draws 2-3: a point in it (S76 D2)
    const float pick = random.nextFloat() * table.cumulative.back();
    size_t index = (size_t)(std::upper_bound(table.cumulative.begin(), table.cumulative.end(), pick) - table.cumulative.begin());
    index = std::min(index, table.refs.size() - 1);
    const dtPolyRef ref = table.refs[index];
    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;
    impl->navMesh->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
    float verts[3 * DT_VERTS_PER_POLYGON];
    float areas[DT_VERTS_PER_POLYGON];
    for (int v = 0; v < poly->vertCount; v++)
        dtVcopy(&verts[v * 3], &tile->verts[poly->verts[v] * 3]);
    const float s = random.nextFloat();
    const float t = random.nextFloat();
    float point[3];
    dtRandomPointInConvexPoly(verts, poly->vertCount, areas, s, t, point);
    impl->query->closestPointOnPoly(ref, point, point, nullptr);
    out.hit = true;
    out.position = Vector3(point[0], point[1], point[2]);
    out.normal = impl->polyNormal(ref, point);
    out.area = reportedArea(poly);
    return true;
}

bool NavMesh::findRandomPointAround(const Vector3& center, float radius, const NavFilter& filter, const Vector3& halfExtents,
                                    NavRandom& random, int island, NavHit& out) const {
    out = NavHit();
    if (!impl->valid || !(radius > 0.0f))
        return false;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float startPoint[3];
    const dtPolyRef startRef = impl->nearest(center, halfExtents, f, startPoint);
    if (!startRef)
        return false;
    activeRandom = &random;
    bool found = false;
    dtPolyRef ref = 0;
    float point[3];
    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;
    // Detour picks a polygon the search reached within the radius, then any point of it. A
    // polygon much larger than the circle puts most of those points past the radius: the point is
    // then drawn in the circle until it falls in that polygon, and only then is a polygon picked
    // again
    for (int attempt = 0; attempt < RANDOM_AROUND_TRIES && !found; attempt++) {
        if (dtStatusFailed(impl->query->findRandomPointAroundCircle(startRef, startPoint, radius, &f, drawActiveRandom, &ref, point)))
            break;
        if (island >= 0 && impl->islandOf(ref) != island)
            continue;
        const float dx = point[0] - center.x, dz = point[2] - center.z;
        found = dx * dx + dz * dz <= radius * radius;
        if (found)
            break;
        impl->navMesh->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
        float verts[3 * DT_VERTS_PER_POLYGON];
        for (int v = 0; v < poly->vertCount; v++)
            dtVcopy(&verts[v * 3], &tile->verts[poly->verts[v] * 3]);
        for (int inCircle = 0; inCircle < RANDOM_AROUND_TRIES && !found; inCircle++) {
            const float r = radius * std::sqrt(random.nextFloat());
            const float angle = 6.28318531f * random.nextFloat();
            const float candidate[3] = {center.x + r * std::cos(angle), startPoint[1], center.z + r * std::sin(angle)};
            if (dtPointInPolygon(candidate, verts, poly->vertCount)) {
                impl->query->closestPointOnPoly(ref, candidate, point, nullptr);
                found = true;
            }
        }
    }
    activeRandom = nullptr;
    if (!found)
        return false;
    impl->navMesh->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
    out.hit = true;
    out.position = Vector3(point[0], point[1], point[2]);
    out.normal = impl->polyNormal(ref, point);
    out.area = reportedArea(poly);
    out.distance = Vector3(point[0] - center.x, 0.0f, point[2] - center.z).length();
    return true;
}

bool NavMesh::moveAlongSurface(const Vector3& from, const Vector3& to, const NavFilter& filter, const Vector3& halfExtents,
                               NavHit& out) const {
    out = NavHit();
    if (!impl->valid)
        return false;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float startPoint[3];
    const dtPolyRef startRef = impl->nearest(from, halfExtents, f, startPoint);
    if (!startRef)
        return false;
    const float end[3] = {to.x, to.y, to.z};
    float result[3];
    dtPolyRef visited[MOVE_MAX_VISITED];
    int visitedCount = 0;
    if (dtStatusFailed(impl->query->moveAlongSurface(startRef, startPoint, end, &f, result, visited, &visitedCount, MOVE_MAX_VISITED)))
        return false;
    const dtPolyRef last = visitedCount > 0 ? visited[visitedCount - 1] : startRef;
    // Detour moves in the XZ plane: the height is the polygon's there
    float height = result[1];
    if (dtStatusSucceed(impl->query->getPolyHeight(last, result, &height)))
        result[1] = height;
    out.position = Vector3(result[0], result[1], result[2]);
    out.distance = Vector3(result[0] - startPoint[0], 0.0f, result[2] - startPoint[2]).length();
    out.hit = Vector3(to.x - result[0], 0.0f, to.z - result[2]).length() > 1e-3f;
    out.area = impl->areaOf(last);
    out.normal = impl->polyNormal(last, result);
    return true;
}

bool NavMesh::getHeight(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents, float& height) const {
    if (!impl->valid)
        return false;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float nearestPoint[3];
    const dtPolyRef ref = impl->nearest(point, halfExtents, f, nearestPoint);
    if (!ref)
        return false;
    const float p[3] = {point.x, point.y, point.z};
    return dtStatusSucceed(impl->query->getPolyHeight(ref, p, &height));
}

uint32_t NavMesh::addObstacle(const NavObstacleDesc& desc) {
    if (!impl->valid || !impl->tileCache)
        return 0;
    // queued adds count: Detour's slots never run out (spec S74 R11, R17)
    if ((int)impl->obstacles.size() >= impl->header.maxObstacles)
        return 0;
    const uint32_t id = impl->nextObstacle++;
    Impl::Obstacle& obstacle = impl->obstacles[id];
    obstacle.desc = desc;
    impl->queueAdd(id, obstacle);
    return id;
}

void NavMesh::moveObstacle(uint32_t id, const NavObstacleDesc& desc) {
    auto found = impl->obstacles.find(id);
    if (found == impl->obstacles.end())
        return;
    found->second.desc = desc;
    // a queued add takes the new description when it is submitted
    impl->queueRemove(found->second);
    impl->queueAdd(id, found->second);
}

void NavMesh::removeObstacle(uint32_t id) {
    auto found = impl->obstacles.find(id);
    if (found == impl->obstacles.end())
        return;
    impl->queueRemove(found->second);
    impl->obstacles.erase(found);
}

bool NavMesh::hasObstacle(uint32_t id) const {
    return impl->obstacles.count(id) != 0;
}

void NavMesh::adoptObstacles(NavMesh& previous) {
    if (&previous == this)
        return;
    std::map<uint32_t, Impl::Obstacle> adopted = std::move(previous.impl->obstacles);
    previous.impl->obstacles.clear();
    previous.impl->requests.clear();
    impl->nextObstacle = std::max(impl->nextObstacle, previous.impl->nextObstacle);
    if (!impl->valid || !impl->tileCache)
        return;
    // the same ids, in their order, as far as Max Carving Obstacles allows
    for (auto& entry : adopted) {
        if ((int)impl->obstacles.size() >= impl->header.maxObstacles)
            break;
        Impl::Obstacle& obstacle = impl->obstacles[entry.first];
        obstacle.desc = entry.second.desc;
        impl->queueAdd(entry.first, obstacle);
    }
    impl->barrierSerial = impl->requestSerial;
}

int NavMesh::countTileLayers(const NavObstacleDesc& desc) const {
    if (!impl->valid || !impl->tileCache)
        return 0;
    return impl->countLayers(desc);
}

bool NavMesh::updateCarving(const NavCarveBudget& budget, bool& upToDate, int& tilesRebuilt) {
    upToDate = true;
    tilesRebuilt = 0;
    if (!impl->valid || !impl->tileCache)
        return false;
    Impl& m = *impl;
    const auto begin = std::chrono::steady_clock::now();
    // the wall-clock ceiling stops a tick after at least one build (spec S74 R14)
    const auto spent = [&]() {
        return budget.ceilingMs > 0.0 && tilesRebuilt > 0 &&
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() >= budget.ceilingMs;
    };
    bool changed = false;
    while (tilesRebuilt < budget.maxTileBuilds && !spent()) {
        // deferred tiles first (spec S74 R15)
        if (!m.deferred.empty()) {
            const TileKey key = m.deferred.front();
            m.deferred.pop_front();
            m.buildTile(key);
            tilesRebuilt++;
            changed = true;
            continue;
        }
        // then the obstacle requests, handed over only while the tile cache is idle
        if (m.pending.empty()) {
            if (m.requests.empty())
                break;
            if (!m.submitBatch())
                break;
            changed = true;
            if (m.pending.empty())
                continue;
        }
        // each update() builds the first tile of Detour's update list (the requests were taken
        // by the first)
        const dtCompressedTileRef ref = m.pending.front();
        m.pending.erase(m.pending.begin());
        const dtCompressedTile* tile = m.tileCache->getTileByRef(ref);
        bool idle = false;
        const dtStatus status = m.tileCache->update(0.0f, m.navMesh, &idle);
        if (m.pending.empty())
            m.doneSerial = m.batchSerial;
        // a layer replaced since the obstacle touched it: Detour skips it, no work was done
        if (!tile)
            continue;
        tilesRebuilt++;
        changed = true;
        if (dtStatusFailed(status))
            m.buildFailed(tile->header->tx, tile->header->ty, tile->header->tlayer, status);
    }
    upToDate = m.deferred.empty() && m.pending.empty() && m.requests.empty();
    // S76 R14: the overrides check every tile's reference, so tiles Detour's update replaced and
    // deferred tiles built above both get their runtime areas and link states again
    const bool overridden = m.tilesChanged();
    if (changed || overridden)
        m.bump();
    return true;
}

void NavMesh::setDeferCarvedBuilds(bool defer) {
    impl->deferBuilds = defer;
}

int NavMesh::getTileCacheObstacleCount() const {
    if (!impl->valid || !impl->tileCache)
        return 0;
    int count = 0;
    for (int i = 0; i < impl->tileCache->getObstacleCount(); i++) {
        if (impl->tileCache->getObstacle(i)->state != DT_OBSTACLE_EMPTY)
            count++;
    }
    return count;
}

int NavMesh::getFailedCarveBuilds() const {
    return impl->failedBuilds;
}

bool NavMesh::getLinkConnections(uint32_t userId, std::vector<NavLinkInput>& out) const {
    out.clear();
    if (!impl->valid)
        return false;
    if (impl->tileCache) {
        for (const auto& entry : impl->links) {
            for (const NavLinkInput& link : entry.second) {
                if (link.userId == userId)
                    out.push_back(link);
            }
        }
        return !out.empty();
    }
    // static tiles: the connections as Detour holds them, in tile order (updateLink's)
    const dtNavMesh* mesh = impl->navMesh;
    for (int i = 0; i < mesh->getMaxTiles(); i++) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (!tile || !tile->header)
            continue;
        for (int c = 0; c < tile->header->offMeshConCount; c++) {
            const dtOffMeshConnection& con = tile->offMeshCons[c];
            if (con.userId != userId)
                continue;
            const dtPoly& poly = tile->polys[con.poly];
            NavLinkInput link;
            dtVcopy(link.start, &con.pos[0]);
            dtVcopy(link.end, &con.pos[3]);
            link.radius = con.rad;
            link.bidirectional = (con.flags & DT_OFFMESH_CON_BIDIR) != 0;
            link.area = AREA_JUMP;
            for (int a = 0; a < AREA_COUNT; a++) {
                if (poly.flags == (1u << a))
                    link.area = (uint8_t)a;
            }
            link.polyArea = poly.getArea();
            link.userId = con.userId;
            out.push_back(link);
        }
    }
    return !out.empty();
}

bool NavMesh::updateLink(uint32_t userId, const std::vector<NavLinkInput>& connections, std::string& error) {
    if (!impl->valid) {
        error = "the navmesh is not built";
        return false;
    }
    const float tileSize = impl->header.config.tileWorldSize();
    if (impl->tileCache) {
        // the tile cache rebuilds the tiles from their layers with the moved links
        std::vector<TileKey> touched;
        for (auto& entry : impl->links) {
            auto& list = entry.second;
            const size_t before = list.size();
            list.erase(std::remove_if(list.begin(), list.end(), [&](const NavLinkInput& l) { return l.userId == userId; }), list.end());
            if (list.size() != before)
                touched.push_back(entry.first);
        }
        for (const NavLinkInput& link : connections) {
            const TileKey key{(int)std::floor(link.start[0] / tileSize), (int)std::floor(link.start[2] / tileSize)};
            impl->links[key].push_back(link);
            touched.push_back(key);
        }
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end(), [](const TileKey& a, const TileKey& b) { return a.x == b.x && a.y == b.y; }), touched.end());
        if (impl->keepData) {
            for (NavTileBlob& blob : impl->data.tiles) {
                if (blob.layer != 0)
                    continue;
                auto found = impl->links.find(TileKey{blob.x, blob.y});
                blob.links = found != impl->links.end() ? found->second : std::vector<NavLinkInput>();
            }
        }
        if (impl->deferBuilds) {
            // rebuilt within the carving budget; the tiles keep their layers, so their cuts stay
            for (const TileKey& key : touched)
                impl->deferTile(key);
        } else if (!impl->buildCarvedTiles(touched, error)) {
            return false;
        }
        impl->tilesChanged();
        impl->bump();
        return true;
    }

    // static tiles: patch the connections in a copy of their tile and re-add it. A tile is copied
    // in its baked state (S76): its overrides and link states apply again on the way out
    dtNavMesh* mesh = impl->navMesh;
    auto refuse = [&](const char* why) {
        error = why;
        impl->tilesChanged();
        return false;
    };
    size_t next = 0;
    std::vector<std::pair<dtTileRef, std::vector<unsigned char>>> patched;
    for (int i = 0; i < mesh->getMaxTiles() && next <= connections.size(); i++) {
        const dtMeshTile* tile = static_cast<const dtNavMesh*>(mesh)->getTile(i);
        if (!tile || !tile->header || tile->header->offMeshConCount == 0)
            continue;
        std::vector<unsigned char> copy;
        for (int c = 0; c < tile->header->offMeshConCount; c++) {
            if (tile->offMeshCons[c].userId != userId)
                continue;
            if (next >= connections.size())
                return refuse("the link has fewer connections than it was baked with; rebake the surface");
            const NavLinkInput& link = connections[next++];
            const float* bmin = tile->header->bmin;
            const float* bmax = tile->header->bmax;
            if (link.start[0] < bmin[0] || link.start[0] >= bmax[0] || link.start[2] < bmin[2] || link.start[2] >= bmax[2] ||
                link.start[1] < bmin[1] || link.start[1] > bmax[1])
                return refuse("the link's start left its baked tile; rebake the surface");
            if (copy.empty()) {
                if (impl->polyState)
                    impl->polyState->restoreTile(tile);
                copy.assign(tile->data, tile->data + tile->dataSize);
            }
            const size_t offMeshOffset = (size_t)((const unsigned char*)tile->offMeshCons - tile->data);
            const size_t vertsOffset = (size_t)((const unsigned char*)tile->verts - tile->data);
            const size_t polysOffset = (size_t)((const unsigned char*)tile->polys - tile->data);
            dtOffMeshConnection con;
            std::memcpy(&con, copy.data() + offMeshOffset + sizeof(dtOffMeshConnection) * (size_t)c, sizeof(con));
            dtVcopy(&con.pos[0], link.start);
            dtVcopy(&con.pos[3], link.end);
            con.rad = link.radius;
            con.flags = link.bidirectional ? DT_OFFMESH_CON_BIDIR : 0;
            con.side = classifyOffMeshPoint(link.end, bmin, bmax);
            std::memcpy(copy.data() + offMeshOffset + sizeof(dtOffMeshConnection) * (size_t)c, &con, sizeof(con));
            dtPoly poly;
            std::memcpy(&poly, copy.data() + polysOffset + sizeof(dtPoly) * con.poly, sizeof(poly));
            std::memcpy(copy.data() + vertsOffset + sizeof(float) * 3 * poly.verts[0], link.start, sizeof(float) * 3);
            std::memcpy(copy.data() + vertsOffset + sizeof(float) * 3 * poly.verts[1], link.end, sizeof(float) * 3);
            poly.flags = (unsigned short)(1u << link.area);
            poly.setArea(link.polyArea);
            std::memcpy(copy.data() + polysOffset + sizeof(dtPoly) * con.poly, &poly, sizeof(poly));
        }
        if (!copy.empty())
            patched.emplace_back(mesh->getTileRef(tile), std::move(copy));
    }
    if (next != connections.size())
        return refuse("the link has more connections than it was baked with; rebake the surface");
    for (auto& entry : patched) {
        mesh->removeTile(entry.first, nullptr, nullptr);
        unsigned char* data = (unsigned char*)dtAlloc(entry.second.size(), DT_ALLOC_PERM);
        std::memcpy(data, entry.second.data(), entry.second.size());
        if (dtStatusFailed(mesh->addTile(data, (int)entry.second.size(), DT_TILE_FREE_DATA, 0, nullptr))) {
            dtFree(data);
            return refuse("Detour refused a patched tile");
        }
    }
    impl->tilesChanged();
    impl->bump();
    return true;
}

void NavMesh::getDebugGeometry(const std::function<bool(const float* bmin, const float* bmax)>& visible, size_t maxTriangles,
                               bool detailMesh, NavDebugGeometry& out, uint32_t extras) const {
    out = NavDebugGeometry();
    if (!impl->valid)
        return;
    const bool withIslands = (extras & DEBUG_ISLANDS) != 0;
    const bool withTiles = (extras & DEBUG_TILES) != 0;
    if (withIslands)
        impl->ensureLabels();
    const dtNavMesh* mesh = impl->navMesh;
    size_t triangles = 0;
    for (int i = 0; i < mesh->getMaxTiles(); i++) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (!tile || !tile->header)
            continue;
        if (visible && !visible(tile->header->bmin, tile->header->bmax))
            continue;
        const int32_t tileIndex = (int32_t)(out.tileBounds.size() / 6);
        out.tileBounds.insert(out.tileBounds.end(), tile->header->bmin, tile->header->bmin + 3);
        out.tileBounds.insert(out.tileBounds.end(), tile->header->bmax, tile->header->bmax + 3);
        out.tileCoords.push_back(tile->header->x);
        out.tileCoords.push_back(tile->header->y);
        out.tileCoords.push_back(tile->header->layer);

        for (int p = 0; p < tile->header->polyCount; p++) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;
            const uint8_t area = poly.getArea() < AREA_COUNT ? poly.getArea() : AREA_WALKABLE;
            const int32_t island = withIslands ? impl->islands.labelOf((unsigned int)i, (unsigned int)p) : -1;
            auto tag = [&]() {
                if (withIslands)
                    out.triangleIslands.push_back(island);
                if (withTiles)
                    out.triangleTiles.push_back(tileIndex);
            };
            if (detailMesh && tile->detailMeshes) {
                const dtPolyDetail& detail = tile->detailMeshes[p];
                for (int t = 0; t < detail.triCount; t++) {
                    if (triangles >= maxTriangles) {
                        out.truncated = true;
                        break;
                    }
                    const unsigned char* tri = &tile->detailTris[(detail.triBase + (unsigned int)t) * 4];
                    for (int k = 0; k < 3; k++) {
                        const float* v = tri[k] < poly.vertCount ? &tile->verts[poly.verts[tri[k]] * 3]
                                                                  : &tile->detailVerts[(detail.vertBase + (tri[k] - poly.vertCount)) * 3];
                        out.triangles.insert(out.triangles.end(), v, v + 3);
                    }
                    out.triangleAreas.push_back(area);
                    tag();
                    triangles++;
                }
            } else {
                for (int t = 2; t < poly.vertCount; t++) {
                    if (triangles >= maxTriangles) {
                        out.truncated = true;
                        break;
                    }
                    const float* a = &tile->verts[poly.verts[0] * 3];
                    const float* b = &tile->verts[poly.verts[t - 1] * 3];
                    const float* c = &tile->verts[poly.verts[t] * 3];
                    out.triangles.insert(out.triangles.end(), a, a + 3);
                    out.triangles.insert(out.triangles.end(), b, b + 3);
                    out.triangles.insert(out.triangles.end(), c, c + 3);
                    out.triangleAreas.push_back(area);
                    tag();
                    triangles++;
                }
            }
            for (int e = 0; e < poly.vertCount; e++) {
                const float* a = &tile->verts[poly.verts[e] * 3];
                const float* b = &tile->verts[poly.verts[(e + 1) % poly.vertCount] * 3];
                bool boundary = poly.neis[e] == 0;
                if (!boundary && (poly.neis[e] & DT_EXT_LINK)) {
                    boundary = true;
                    for (unsigned int link = poly.firstLink; link != DT_NULL_LINK; link = tile->links[link].next) {
                        if (tile->links[link].edge == e) {
                            boundary = false;
                            break;
                        }
                    }
                }
                // an inner edge once (from its lower polygon index)
                if (!boundary && !(poly.neis[e] & DT_EXT_LINK) && poly.neis[e] - 1 < p)
                    continue;
                std::vector<float>& edges = boundary ? out.boundaryEdges : out.polygonEdges;
                edges.insert(edges.end(), a, a + 3);
                edges.insert(edges.end(), b, b + 3);
            }
        }
        for (int c = 0; c < tile->header->offMeshConCount; c++) {
            const dtOffMeshConnection& con = tile->offMeshCons[c];
            const dtPoly& poly = tile->polys[con.poly];
            NavDebugGeometry::Link link;
            std::memcpy(link.start, &con.pos[0], sizeof(link.start));
            std::memcpy(link.end, &con.pos[3], sizeof(link.end));
            link.radius = con.rad;
            link.bidirectional = (con.flags & DT_OFFMESH_CON_BIDIR) != 0;
            unsigned short flags = poly.flags;
            uint8_t area = AREA_JUMP;
            for (int a = 0; a < AREA_COUNT; a++) {
                if (flags == (1u << a))
                    area = (uint8_t)a;
            }
            link.area = area;
            link.userId = con.userId;
            out.links.push_back(link);
        }
        if (out.truncated)
            break;
    }
}

void* NavMesh::detourNavMesh() const {
    return impl->valid ? impl->navMesh : nullptr;
}

void* NavMesh::detourQuery() const {
    return impl->valid ? impl->query : nullptr;
}

uint64_t NavMesh::getRevision() const {
    return impl->revision;
}

bool NavMesh::hasOffMeshConnections() const {
    if (!impl->valid) return false;
    if (impl->linksRevision != impl->revision) {
        impl->hasLinks = false;
        const dtNavMesh* mesh = impl->navMesh;
        for (int i = 0; i < mesh->getMaxTiles() && !impl->hasLinks; i++) {
            const dtMeshTile* tile = mesh->getTile(i);
            impl->hasLinks = tile && tile->header && tile->header->offMeshConCount > 0;
        }
        impl->linksRevision = impl->revision;
    }
    return impl->hasLinks;
}

// ---------------------------------------------------------------- S75 diagnostics

const std::vector<NavIsland>& NavMesh::getIslands() const {
    impl->ensureLabels();
    return impl->islands.islands;
}

float NavMesh::getWalkableArea() const {
    impl->ensureLabels();
    return impl->islands.walkableArea;
}

bool NavMesh::areIslandsCurrent() const {
    return impl->labelsCurrent();
}

int NavMesh::getIslandOf(uint32_t polyRef) const {
    return impl->islandOf(polyRef);
}

uint64_t NavMesh::getLabelledRevision() const {
    return impl->labelledRevision;
}

size_t NavMesh::getIslandMemoryBytes() const {
    return impl->islands.memoryBytes();
}

NavLocation NavMesh::locate(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents, float maxDistance) const {
    NavLocation location;
    if (!impl->valid)
        return location;
    impl->ensureLabels();
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float nearestPoint[3];
    dtPolyRef ref = impl->nearest(point, halfExtents, f, nearestPoint);
    location.onMesh = ref != 0;
    if (!ref && maxDistance > 0.0f) {
        ref = impl->nearest(point, Vector3(maxDistance, maxDistance, maxDistance), f, nearestPoint);
        if (ref && Vector3(nearestPoint[0], nearestPoint[1], nearestPoint[2]).distance(point) > maxDistance)
            ref = 0;
    }
    if (!ref)
        return location;
    location.found = true;
    location.position = Vector3(nearestPoint[0], nearestPoint[1], nearestPoint[2]);
    location.snapDistance = location.position.distance(point);
    location.island = impl->islandOf(ref);
    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;
    impl->navMesh->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
    location.area = poly->getArea() < AREA_COUNT ? poly->getArea() : AREA_WALKABLE;
    return location;
}

bool NavMesh::covers(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents) const {
    if (!impl->valid)
        return false;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    const float p[3] = {point.x, point.y, point.z};
    const float extents[3] = {halfExtents.x, halfExtents.y, halfExtents.z};
    dtPolyRef ref = 0;
    float nearestPoint[3];
    bool over = false;
    if (dtStatusFailed(impl->query->findNearestPoly(p, extents, &f, &ref, nearestPoint, &over)))
        return false;
    return ref != 0 && over;
}

bool NavMesh::isReachable(const Vector3& start, const Vector3& end, const NavFilter& filter, const Vector3& halfExtents,
                          PathFailure* why) const {
    if (impl->valid && impl->labelsCurrent()) {
        // R6: two lookups, no search, when the islands tell
        dtQueryFilter f;
        impl->fillFilter(filter, f);
        float ignored[3];
        const int a = impl->islandOf(impl->nearest(start, halfExtents, f, ignored));
        const int b = a >= 0 ? impl->islandOf(impl->nearest(end, halfExtents, f, ignored)) : -1;
        if (a >= 0 && b >= 0 && a != b) {
            if (why)
                *why = PathFailure::Disconnected;
            return false;
        }
    }
    const NavPath path = findPath(start, end, filter, halfExtents);
    if (why)
        *why = path.failure;
    return path.status == PathStatus::Complete;
}

uint64_t NavMesh::getSearchCount() const {
    return impl->searches;
}

void NavMesh::getLinkStates(std::vector<NavLinkState>& out) const {
    out.clear();
    if (!impl->valid)
        return;
    impl->ensureLabels();
    const dtNavMesh* mesh = impl->navMesh;
    for (int i = 0; i < mesh->getMaxTiles(); i++) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (!tile || !tile->header)
            continue;
        for (int c = 0; c < tile->header->offMeshConCount; c++) {
            const dtOffMeshConnection& con = tile->offMeshCons[c];
            NavLinkState state;
            state.userId = con.userId;
            state.start = Vector3(con.pos[0], con.pos[1], con.pos[2]);
            state.end = Vector3(con.pos[3], con.pos[4], con.pos[5]);
            // Detour links the connection's polygon to its start's ground polygon with edge 0
            // and to its end's with edge 1 (DetourNavMesh.cpp baseOffMeshLinks, connectExtOffMeshLinks)
            const dtPoly& poly = tile->polys[con.poly];
            for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
                const dtLink& link = tile->links[l];
                unsigned int salt = 0, it = 0, ip = 0;
                mesh->decodePolyId(link.ref, salt, it, ip);
                if (link.edge == 0) {
                    state.startConnected = true;
                    state.startIsland = impl->islands.labelOf(it, ip);
                } else if (link.edge == 1) {
                    state.endConnected = true;
                    state.endIsland = impl->islands.labelOf(it, ip);
                }
            }
            out.push_back(state);
        }
    }
}

void NavMesh::getTileInfos(std::vector<NavTileInfo>& out) const {
    out.clear();
    if (!impl->valid)
        return;
    const dtNavMesh* mesh = impl->navMesh;
    for (int i = 0; i < mesh->getMaxTiles(); i++) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (!tile || !tile->header)
            continue;
        NavTileInfo info;
        info.x = tile->header->x;
        info.y = tile->header->y;
        info.layer = tile->header->layer;
        info.polys = tile->header->offMeshBase;
        info.bytes = (size_t)tile->dataSize;
        std::memcpy(info.bmin, tile->header->bmin, sizeof(info.bmin));
        std::memcpy(info.bmax, tile->header->bmax, sizeof(info.bmax));
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const NavTileInfo& a, const NavTileInfo& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.x != b.x) return a.x < b.x;
        return a.layer < b.layer;
    });
}

uint32_t NavMesh::getPolyAt(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents) const {
    if (!impl->valid)
        return 0;
    dtQueryFilter f;
    impl->fillFilter(filter, f);
    float nearestPoint[3];
    return impl->nearest(point, halfExtents, f, nearestPoint);
}

bool NavMesh::setPolyFlags(uint32_t polyRef, uint16_t flags) {
    if (!impl->valid || dtStatusFailed(impl->navMesh->setPolyFlags(polyRef, flags)))
        return false;
    impl->bump();
    return true;
}

bool NavMesh::tileCovers(const NavTileBlob& staticTile, const NavBuildConfig& config, const Vector3& point, const Vector3& halfExtents) {
    if (staticTile.data.size() < sizeof(dtMeshHeader))
        return false;
    dtMeshHeader header;
    std::memcpy(&header, staticTile.data.data(), sizeof(header));
    const int maxPolys = (int)nextPow2((unsigned int)std::max({config.maxPolysPerTile, header.polyCount, 1}));
    if (refuseTileData(staticTile, maxPolys))
        return false;
    dtNavMeshParams params;
    std::memset(&params, 0, sizeof(params));
    params.tileWidth = config.tileWorldSize();
    params.tileHeight = params.tileWidth;
    params.maxTiles = 1;
    params.maxPolys = maxPolys;
    dtNavMesh* mesh = dtAllocNavMesh();
    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    bool covered = false;
    unsigned char* copy = (unsigned char*)dtAlloc(staticTile.data.size(), DT_ALLOC_PERM);
    if (mesh && query && copy && dtStatusSucceed(mesh->init(&params))) {
        std::memcpy(copy, staticTile.data.data(), staticTile.data.size());
        if (dtStatusSucceed(mesh->addTile(copy, (int)staticTile.data.size(), DT_TILE_FREE_DATA, 0, nullptr))) {
            copy = nullptr;
            if (dtStatusSucceed(query->init(mesh, 64))) {
                dtQueryFilter filter;
                filter.setIncludeFlags(ALL_AREAS);
                filter.setExcludeFlags(0);
                const float p[3] = {point.x, point.y, point.z};
                const float extents[3] = {halfExtents.x, halfExtents.y, halfExtents.z};
                dtPolyRef ref = 0;
                float nearestPoint[3];
                bool over = false;
                covered = dtStatusSucceed(query->findNearestPoly(p, extents, &filter, &ref, nearestPoint, &over)) && ref != 0 && over;
            }
        }
    }
    if (copy)
        dtFree(copy);
    if (query)
        dtFreeNavMeshQuery(query);
    if (mesh)
        dtFreeNavMesh(mesh);
    return covered;
}

// ---------------------------------------------------------------- S76 areas

uint32_t NavMesh::addAreaOverride(const NavConvexVolume& volume, uint8_t area, uint64_t order) {
    bool changed = false;
    const uint32_t id = impl->polys().add(volume, area, order, changed);
    if (changed)
        impl->bump();
    return id;
}

bool NavMesh::updateAreaOverride(uint32_t id, const NavConvexVolume& volume, uint8_t area) {
    if (!impl->polyState)
        return false;
    bool changed = false;
    const bool found = impl->polys().update(id, volume, area, changed);
    if (changed)
        impl->bump();
    return found;
}

bool NavMesh::removeAreaOverride(uint32_t id) {
    if (!impl->polyState)
        return false;
    bool changed = false;
    const bool found = impl->polys().remove(id, changed);
    if (changed)
        impl->bump();
    return found;
}

int NavMesh::getAreaOverridePolyCount(uint32_t id) const {
    if (!impl->polyState || !impl->valid)
        return impl->polyState && impl->polyState->has(id) ? 0 : -1;
    return impl->polyState->polyCount(id);
}

void NavMesh::setLinkActive(uint32_t userId, bool active) {
    // every link starts active: nothing to make for it
    if (active && !impl->polyState)
        return;
    if (impl->polys().setLinkActive(userId, active))
        impl->bump();
}

bool NavMesh::isLinkActive(uint32_t userId) const {
    return !impl->polyState || impl->polyState->isLinkActive(userId);
}

void NavMesh::takeChangedPolys(std::vector<uint32_t>& out) {
    out.clear();
    if (impl->polyState)
        impl->polyState->takeChanged(out);
}

size_t NavMesh::getPolyStateTileCount() const {
    return impl->polyState ? impl->polyState->snapshotCount() : 0;
}

std::vector<uint8_t> NavMesh::getTileStateBytes(int x, int y, int layer) const {
    std::vector<uint8_t> bytes;
    if (!impl->valid)
        return bytes;
    const dtMeshTile* tile = static_cast<const dtNavMesh*>(impl->navMesh)->getTileAt(x, y, layer);
    if (!tile || !tile->header)
        return bytes;
    bytes.resize((size_t)impl->navMesh->getTileStateSize(tile));
    impl->navMesh->storeTileState(tile, bytes.data(), (int)bytes.size());
    return bytes;
}

#else // JASPERENGINE_NAVIGATION

// Without Recast/Detour: the data model and its file format work (an editor can read and write
// .navmesh files), but nothing builds a runtime mesh; every query fails and logs once.

namespace {
    void logUnavailable() {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log::error("Navigation: this build has no navmesh support (JASPERENGINE_NAVIGATION is off)");
    }
}

struct NavMesh::Impl {
    NavMeshHeader header;
};

NavMesh::NavMesh(): impl(std::make_unique<Impl>()) {}
NavMesh::~NavMesh() = default;

bool NavMesh::create(NavMeshData, bool, std::string& error, const NavMeshCreateOptions&) {
    logUnavailable();
    error = "navmesh support is not in this build (JASPERENGINE_NAVIGATION is off)";
    return false;
}
void NavMesh::clear() {}
bool NavMesh::isValid() const { return false; }
bool NavMesh::isComplete() const { return true; }
int NavMesh::getQueuedBuilds() const { return 0; }
uint64_t NavMesh::getGeneration() const { return 0; }
const NavMeshHeader& NavMesh::getHeader() const { return impl->header; }
const NavMeshData* NavMesh::getData() const { return nullptr; }
bool NavMesh::replaceTiles(const std::vector<NavTileCoord>&, std::vector<NavTileBlob>, std::string& error) {
    logUnavailable();
    error = "navmesh support is not in this build (JASPERENGINE_NAVIGATION is off)";
    return false;
}
int NavMesh::getTileCount() const { return 0; }
int NavMesh::getPolyCount() const { return 0; }
size_t NavMesh::getMemoryBytes() const { return 0; }
bool NavMesh::isCarving() const { return false; }
NavPath NavMesh::findPath(const Vector3& start, const Vector3& end, const NavFilter&, const Vector3&, const PathOptions&) const {
    logUnavailable();
    NavPath path;
    path.failure = PathFailure::NotBaked;
    path.start = path.closestPoint = start;
    path.end = end;
    path.gap = start.distance(end);
    return path;
}
NavHit NavMesh::samplePosition(const Vector3&, float, const NavFilter&) const { logUnavailable(); return NavHit(); }
NavHit NavMesh::raycast(const Vector3&, const Vector3&, const NavFilter&, const Vector3&) const { logUnavailable(); return NavHit(); }
NavHit NavMesh::findClosestEdge(const Vector3&, const NavFilter&, const Vector3&, float) const { logUnavailable(); return NavHit(); }
bool NavMesh::findRandomPoint(const NavFilter&, NavRandom&, int, NavHit& out) const { logUnavailable(); out = NavHit(); return false; }
bool NavMesh::findRandomPointAround(const Vector3&, float, const NavFilter&, const Vector3&, NavRandom&, int, NavHit& out) const {
    logUnavailable();
    out = NavHit();
    return false;
}
bool NavMesh::moveAlongSurface(const Vector3&, const Vector3&, const NavFilter&, const Vector3&, NavHit& out) const {
    logUnavailable();
    out = NavHit();
    return false;
}
bool NavMesh::getHeight(const Vector3&, const NavFilter&, const Vector3&, float&) const { logUnavailable(); return false; }
uint32_t NavMesh::addObstacle(const NavObstacleDesc&) { logUnavailable(); return 0; }
void NavMesh::moveObstacle(uint32_t, const NavObstacleDesc&) {}
void NavMesh::removeObstacle(uint32_t) {}
bool NavMesh::hasObstacle(uint32_t) const { return false; }
void NavMesh::adoptObstacles(NavMesh&) {}
int NavMesh::countTileLayers(const NavObstacleDesc&) const { return 0; }
bool NavMesh::updateCarving(const NavCarveBudget&, bool& upToDate, int& tilesRebuilt) { upToDate = true; tilesRebuilt = 0; return false; }
void NavMesh::setDeferCarvedBuilds(bool) {}
int NavMesh::getTileCacheObstacleCount() const { return 0; }
int NavMesh::getFailedCarveBuilds() const { return 0; }
bool NavMesh::getLinkConnections(uint32_t, std::vector<NavLinkInput>& out) const { out.clear(); return false; }
bool NavMesh::updateLink(uint32_t, const std::vector<NavLinkInput>&, std::string& error) {
    error = "navmesh support is not in this build (JASPERENGINE_NAVIGATION is off)";
    return false;
}
void NavMesh::getDebugGeometry(const std::function<bool(const float*, const float*)>&, size_t, bool, NavDebugGeometry& out, uint32_t) const {
    out = NavDebugGeometry();
}
void* NavMesh::detourNavMesh() const { return nullptr; }
void* NavMesh::detourQuery() const { return nullptr; }
uint64_t NavMesh::getRevision() const { return 0; }
bool NavMesh::hasOffMeshConnections() const { return false; }
const std::vector<NavIsland>& NavMesh::getIslands() const { static const std::vector<NavIsland> none; return none; }
float NavMesh::getWalkableArea() const { return 0.0f; }
bool NavMesh::areIslandsCurrent() const { return false; }
int NavMesh::getIslandOf(uint32_t) const { return -1; }
uint64_t NavMesh::getLabelledRevision() const { return 0; }
size_t NavMesh::getIslandMemoryBytes() const { return 0; }
NavLocation NavMesh::locate(const Vector3&, const NavFilter&, const Vector3&, float) const { logUnavailable(); return NavLocation(); }
bool NavMesh::covers(const Vector3&, const NavFilter&, const Vector3&) const { logUnavailable(); return false; }
bool NavMesh::isReachable(const Vector3&, const Vector3&, const NavFilter&, const Vector3&, PathFailure* why) const {
    logUnavailable();
    if (why) *why = PathFailure::NotBaked;
    return false;
}
uint64_t NavMesh::getSearchCount() const { return 0; }
void NavMesh::getLinkStates(std::vector<NavLinkState>& out) const { out.clear(); }
void NavMesh::getTileInfos(std::vector<NavTileInfo>& out) const { out.clear(); }
uint32_t NavMesh::getPolyAt(const Vector3&, const NavFilter&, const Vector3&) const { return 0; }
bool NavMesh::setPolyFlags(uint32_t, uint16_t) { logUnavailable(); return false; }
uint32_t NavMesh::addAreaOverride(const NavConvexVolume&, uint8_t, uint64_t) { logUnavailable(); return 0; }
bool NavMesh::updateAreaOverride(uint32_t, const NavConvexVolume&, uint8_t) { return false; }
bool NavMesh::removeAreaOverride(uint32_t) { return false; }
int NavMesh::getAreaOverridePolyCount(uint32_t) const { return -1; }
void NavMesh::setLinkActive(uint32_t, bool) {}
bool NavMesh::isLinkActive(uint32_t) const { return true; }
void NavMesh::takeChangedPolys(std::vector<uint32_t>& out) { out.clear(); }
size_t NavMesh::getPolyStateTileCount() const { return 0; }
std::vector<uint8_t> NavMesh::getTileStateBytes(int, int, int) const { return {}; }
bool NavMesh::tileCovers(const NavTileBlob&, const NavBuildConfig&, const Vector3&, const Vector3&) { logUnavailable(); return false; }

#endif // JASPERENGINE_NAVIGATION
