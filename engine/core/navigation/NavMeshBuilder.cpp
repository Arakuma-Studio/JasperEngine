// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The tile build follows OpenEngine's DetourNavMap::BuildTile (Engine/Modules/Pathfinding/Source/
// DetourNavMap.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License,
// with areas, modifier volumes, off-mesh links and tile-cache layers added. See NOTICE.md.

#include "NavMeshBuilder.h"

#if JASPERENGINE_NAVIGATION

#include "NavDetourSupport.h"

#include "Recast.h"
#include "math/Matrix3.h"
#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourTileCacheBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    // triangles rasterized per stage of a TileJob
    constexpr size_t RASTER_BATCH = 16384;
    // tile-cache layers of one tile at most (Detour's sample)
    constexpr int MAX_LAYERS = 32;

    enum class Stage {
        Gather,
        Rasterize,
        Compact,
        Areas,
        Regions,
        Contours,
        Detail,
        Finish,
        Done
    };

    // The Recast intermediates of one tile, freed on every exit path
    struct RecastScratch {
        rcHeightfield* heightfield = nullptr;
        rcCompactHeightfield* compact = nullptr;
        rcContourSet* contours = nullptr;
        rcPolyMesh* polyMesh = nullptr;
        rcPolyMeshDetail* detailMesh = nullptr;
        rcHeightfieldLayerSet* layers = nullptr;

        ~RecastScratch() { release(); }
        void release() {
            rcFreeHeightField(heightfield);
            rcFreeCompactHeightfield(compact);
            rcFreeContourSet(contours);
            rcFreePolyMesh(polyMesh);
            rcFreePolyMeshDetail(detailMesh);
            rcFreeHeightfieldLayerSet(layers);
            heightfield = nullptr;
            compact = nullptr;
            contours = nullptr;
            polyMesh = nullptr;
            detailMesh = nullptr;
            layers = nullptr;
        }
    };

    void triangleNormal(const float* a, const float* b, const float* c, float* n) {
        const float e0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        n[0] = e0[1] * e1[2] - e0[2] * e1[1];
        n[1] = e0[2] * e1[0] - e0[0] * e1[2];
        n[2] = e0[0] * e1[1] - e0[1] * e1[0];
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 0.0f) {
            n[0] /= length;
            n[1] /= length;
            n[2] /= length;
        }
    }

}

struct NavMeshBuilder::TileJob::State {
    std::shared_ptr<const NavBakeInput> input;
    uint32_t index = 0;
    Stage stage = Stage::Gather;
    // the step within a stage split into several (each Recast pass of a time-sliced bake is one)
    int part = 0;
    NavTileBuildResult result;
    std::chrono::steady_clock::time_point started;
    double busyMs = 0.0;

    rcContext context{false};
    rcConfig cfg{};
    RecastScratch scratch;

    // the tile's own triangles (local indices)
    std::vector<float> vertices;
    std::vector<int> triangles;
    std::vector<unsigned char> areas;
    size_t rasterized = 0;
    std::vector<NavLinkInput> links;

    const NavBakeInput& in() const { return *input; }

    void fail(const std::string& message) {
        result.ok = false;
        result.error = "tile (" + std::to_string(result.coord.x) + ", " + std::to_string(result.coord.y) + "): " + message;
        result.blobs.clear();
        stage = Stage::Done;
        scratch.release();
    }

    void finishEmpty() {
        result.blobs.clear();
        result.polyCount = 0;
        stage = Stage::Done;
        scratch.release();
    }

    void gather();
    void rasterize();
    void compact();
    void markAreas();
    void regions();
    void contours();
    void detail();
    void finish();
    void buildLayers();
};

void NavMeshBuilder::TileJob::State::gather() {
    const NavBakeInput& input = in();
    const NavBuildConfig& config = input.config;
    result.coord = input.tileCoord(index);
    result.hash = index < input.tileHashes.size() ? input.tileHashes[index] : 0;

    float tileMin[3], tileMax[3];
    input.tileBounds(result.coord.x, result.coord.y, tileMin, tileMax);
    const float border = config.borderWorldSize();

    cfg.cs = config.cellSize;
    cfg.ch = config.cellHeight;
    cfg.walkableSlopeAngle = config.agentMaxSlope;
    cfg.walkableHeight = config.walkableHeight;
    cfg.walkableClimb = config.walkableClimb;
    cfg.walkableRadius = config.walkableRadius;
    cfg.maxEdgeLen = config.maxEdgeLen;
    cfg.maxSimplificationError = config.maxSimplificationError;
    cfg.minRegionArea = config.minRegionArea;
    cfg.mergeRegionArea = config.mergeRegionArea;
    cfg.maxVertsPerPoly = config.maxVertsPerPoly;
    cfg.detailSampleDist = config.detailSampleDist;
    cfg.detailSampleMaxError = config.detailSampleMaxError;
    cfg.tileSize = config.tileSize;
    cfg.borderSize = config.borderSize;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.bmin[0] = tileMin[0] - border;
    cfg.bmin[1] = input.bmin[1] - cfg.ch;
    cfg.bmin[2] = tileMin[2] - border;
    cfg.bmax[0] = tileMax[0] + border;
    cfg.bmax[1] = input.bmax[1] + cfg.ch;
    cfg.bmax[2] = tileMax[2] + border;

    // the bucket's triangles, vertices copied in first-use order
    const float walkableCos = std::cos(config.agentMaxSlope / 180.0f * 3.14159265358979323846f);
    std::vector<int> remap;
    const std::vector<uint32_t>& bucket = input.tileTriangles[index];
    if (!bucket.empty())
        remap.assign(input.vertices.size() / 3, -1);
    auto addTriangle = [&](const float* a, const float* b, const float* c, unsigned char recastArea) {
        float n[3];
        triangleNormal(a, b, c, n);
        const unsigned char area = n[1] > walkableCos ? recastArea : (unsigned char)RC_NULL_AREA;
        const int base = (int)(vertices.size() / 3);
        vertices.insert(vertices.end(), a, a + 3);
        vertices.insert(vertices.end(), b, b + 3);
        vertices.insert(vertices.end(), c, c + 3);
        triangles.push_back(base);
        triangles.push_back(base + 1);
        triangles.push_back(base + 2);
        areas.push_back(area);
    };
    for (uint32_t t : bucket) {
        int local[3];
        for (int c = 0; c < 3; c++) {
            const int source = input.triangles[(size_t)t * 3 + (size_t)c];
            if (remap[(size_t)source] < 0) {
                remap[(size_t)source] = (int)(vertices.size() / 3);
                const float* v = &input.vertices[(size_t)source * 3];
                vertices.insert(vertices.end(), v, v + 3);
            }
            local[c] = remap[(size_t)source];
        }
        const float* a = &vertices[(size_t)local[0] * 3];
        const float* b = &vertices[(size_t)local[1] * 3];
        const float* c = &vertices[(size_t)local[2] * 3];
        float n[3];
        triangleNormal(a, b, c, n);
        triangles.push_back(local[0]);
        triangles.push_back(local[1]);
        triangles.push_back(local[2]);
        areas.push_back(n[1] > walkableCos ? input.triangleAreas[t] : (unsigned char)RC_NULL_AREA);
    }

    // terrain cells under the tile, two triangles each, facing up in the lattice's space
    const float boxMin[3] = {cfg.bmin[0], cfg.bmin[1], cfg.bmin[2]};
    const float boxMax[3] = {cfg.bmax[0], cfg.bmax[1], cfg.bmax[2]};
    for (const NavTerrainInput& terrain : input.terrains) {
        int i0, i1, j0, j1;
        if (!terrain.cellRange(boxMin, boxMax, i0, i1, j0, j1))
            continue;
        const bool mirrored = terrain.toNav.linear().determinant() < 0.0f;
        for (int j = j0; j < j1; j++) {
            for (int i = i0; i < i1; i++) {
                if (terrain.isHoleCell(i, j))
                    continue;
                const Vector3 p00 = terrain.toNav * terrain.localAt(i, j);
                const Vector3 p10 = terrain.toNav * terrain.localAt(i + 1, j);
                const Vector3 p01 = terrain.toNav * terrain.localAt(i, j + 1);
                const Vector3 p11 = terrain.toNav * terrain.localAt(i + 1, j + 1);
                const float a[3] = {p00.x, p00.y, p00.z};
                const float b[3] = {p10.x, p10.y, p10.z};
                const float c[3] = {p01.x, p01.y, p01.z};
                const float d[3] = {p11.x, p11.y, p11.z};
                // (i, j) -> (i, j+1) -> (i+1, j+1) faces +y for x right and z forward
                if (!mirrored) {
                    addTriangle(a, c, d, terrain.recastArea);
                    addTriangle(a, d, b, terrain.recastArea);
                } else {
                    addTriangle(a, d, c, terrain.recastArea);
                    addTriangle(a, b, d, terrain.recastArea);
                }
            }
        }
    }

    // the links whose start is in this tile
    for (const NavLinkInput& link : input.links) {
        const NavTileCoord tile = input.tileAt(link.start);
        if (tile == result.coord)
            links.push_back(link);
    }

    if (triangles.empty()) {
        finishEmpty();
        return;
    }

    scratch.heightfield = rcAllocHeightfield();
    if (!scratch.heightfield ||
        !rcCreateHeightfield(&context, *scratch.heightfield, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        fail("Recast could not allocate the heightfield");
        return;
    }
    stage = Stage::Rasterize;
}

void NavMeshBuilder::TileJob::State::rasterize() {
    const size_t total = areas.size();
    const size_t count = std::min(RASTER_BATCH, total - rasterized);
    if (count > 0 &&
        !rcRasterizeTriangles(&context, vertices.data(), (int)(vertices.size() / 3), triangles.data() + rasterized * 3,
                              areas.data() + rasterized, (int)count, *scratch.heightfield, cfg.walkableClimb)) {
        fail("Recast could not rasterize the triangles");
        return;
    }
    rasterized += count;
    if (rasterized >= total) {
        vertices = std::vector<float>();
        triangles = std::vector<int>();
        areas = std::vector<unsigned char>();
        stage = Stage::Compact;
    }
}

void NavMeshBuilder::TileJob::State::compact() {
    // one Recast pass per step: a time-sliced bake never runs two of them in one go
    switch (part++) {
        case 0: rcFilterLowHangingWalkableObstacles(&context, cfg.walkableClimb, *scratch.heightfield); return;
        case 1: rcFilterLedgeSpans(&context, cfg.walkableHeight, cfg.walkableClimb, *scratch.heightfield); return;
        case 2: rcFilterWalkableLowHeightSpans(&context, cfg.walkableHeight, *scratch.heightfield); return;
        default: break;
    }
    part = 0;

    scratch.compact = rcAllocCompactHeightfield();
    if (!scratch.compact ||
        !rcBuildCompactHeightfield(&context, cfg.walkableHeight, cfg.walkableClimb, *scratch.heightfield, *scratch.compact)) {
        fail("Recast could not compact the heightfield");
        return;
    }
    rcFreeHeightField(scratch.heightfield);
    scratch.heightfield = nullptr;

    // Not Walkable won the span merge; now it is the null area
    rcCompactHeightfield& chf = *scratch.compact;
    for (int i = 0; i < chf.spanCount; i++) {
        if (chf.areas[i] == RECAST_NOT_WALKABLE)
            chf.areas[i] = RC_NULL_AREA;
    }
    stage = Stage::Areas;
}

void NavMeshBuilder::TileJob::State::markAreas() {
    const NavBakeInput& input = in();
    rcCompactHeightfield& chf = *scratch.compact;
    // volumes in the collector's order (ascending area, Not Walkable and 2D footprints last),
    // before erosion, so agents keep their radius from what they may not enter
    if (part == 0) {
        part = 1;
        std::vector<float> polygon;
        for (const NavConvexVolume& volume : input.volumes) {
            if (volume.bmax[0] < cfg.bmin[0] || volume.bmin[0] > cfg.bmax[0] ||
                volume.bmax[2] < cfg.bmin[2] || volume.bmin[2] > cfg.bmax[2] ||
                volume.points.size() < 6)
                continue;
            polygon.clear();
            for (size_t i = 0; i + 1 < volume.points.size(); i += 2) {
                polygon.push_back(volume.points[i]);
                polygon.push_back(0.0f);
                polygon.push_back(volume.points[i + 1]);
            }
            // Recast turns the heights into span cells as ints: an unbounded volume (a 2D footprint)
            // is clamped to the tile's heights first (beyond them it would overflow and mark nothing)
            const float low = std::max(volume.minY, chf.bmin[1] - chf.ch);
            const float high = std::min(volume.maxY, chf.bmax[1] + chf.ch);
            if (low > high)
                continue;
            rcMarkConvexPolyArea(&context, polygon.data(), (int)(polygon.size() / 3), low, high, volume.recastArea, chf);
        }
        return;
    }
    if (part == 1) {
        part = 2;
        if (!rcErodeWalkableArea(&context, cfg.walkableRadius, chf))
            fail("Recast could not erode the walkable area");
        return;
    }
    part = 0;
    if (input.config.carving) {
        buildLayers();
        return;
    }

    if (!rcBuildDistanceField(&context, chf)) {
        fail("Recast could not build the distance field");
        return;
    }
    stage = Stage::Regions;
}

void NavMeshBuilder::TileJob::State::regions() {
    if (!rcBuildRegions(&context, *scratch.compact, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) {
        fail("Recast could not partition the tile into regions");
        return;
    }
    stage = Stage::Contours;
}

void NavMeshBuilder::TileJob::State::contours() {
    scratch.contours = rcAllocContourSet();
    if (!scratch.contours ||
        !rcBuildContours(&context, *scratch.compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *scratch.contours)) {
        fail("Recast could not trace the contours");
        return;
    }
    if (scratch.contours->nconts == 0) {
        finishEmpty();
        return;
    }
    scratch.polyMesh = rcAllocPolyMesh();
    if (!scratch.polyMesh || !rcBuildPolyMesh(&context, *scratch.contours, cfg.maxVertsPerPoly, *scratch.polyMesh)) {
        fail("Recast could not build the polygons (too many vertices in one tile? use a smaller tile size)");
        return;
    }
    rcFreeContourSet(scratch.contours);
    scratch.contours = nullptr;
    if (scratch.polyMesh->npolys == 0) {
        finishEmpty();
        return;
    }
    if (scratch.polyMesh->npolys > in().config.maxPolysPerTile) {
        fail(std::to_string(scratch.polyMesh->npolys) + " polygons, more than a tile may hold (" +
             std::to_string(in().config.maxPolysPerTile) + "); use a smaller tile size or a larger cell size");
        return;
    }
    stage = Stage::Detail;
}

void NavMeshBuilder::TileJob::State::detail() {
    scratch.detailMesh = rcAllocPolyMeshDetail();
    if (!scratch.detailMesh ||
        !rcBuildPolyMeshDetail(&context, *scratch.polyMesh, *scratch.compact, cfg.detailSampleDist, cfg.detailSampleMaxError,
                               *scratch.detailMesh)) {
        fail("Recast could not build the detail mesh");
        return;
    }
    rcFreeCompactHeightfield(scratch.compact);
    scratch.compact = nullptr;
    stage = Stage::Finish;
}

void NavMeshBuilder::TileJob::State::finish() {
    rcPolyMesh& pmesh = *scratch.polyMesh;
    rcPolyMeshDetail& dmesh = *scratch.detailMesh;
    if (pmesh.nverts >= 0xffff) {
        fail("too many vertices in one tile; use a smaller tile size");
        return;
    }

    std::vector<unsigned short> flags((size_t)pmesh.npolys);
    for (int i = 0; i < pmesh.npolys; i++)
        detail::polyAreaAndFlags(pmesh.areas[i], pmesh.areas[i], flags[(size_t)i]);

    std::vector<float> linkVerts, linkRadii;
    std::vector<unsigned short> linkFlags;
    std::vector<unsigned char> linkAreas, linkDirs;
    std::vector<unsigned int> linkIds;
    detail::appendLinks(links, pmesh.bmin, pmesh.bmax, linkVerts, linkRadii, linkFlags, linkAreas, linkDirs, linkIds);

    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof(params));
    params.verts = pmesh.verts;
    params.vertCount = pmesh.nverts;
    params.polys = pmesh.polys;
    params.polyAreas = pmesh.areas;
    params.polyFlags = flags.data();
    params.polyCount = pmesh.npolys;
    params.nvp = pmesh.nvp;
    params.detailMeshes = dmesh.meshes;
    params.detailVerts = dmesh.verts;
    params.detailVertsCount = dmesh.nverts;
    params.detailTris = dmesh.tris;
    params.detailTriCount = dmesh.ntris;
    params.offMeshConVerts = linkVerts.empty() ? nullptr : linkVerts.data();
    params.offMeshConRad = linkRadii.empty() ? nullptr : linkRadii.data();
    params.offMeshConFlags = linkFlags.empty() ? nullptr : linkFlags.data();
    params.offMeshConAreas = linkAreas.empty() ? nullptr : linkAreas.data();
    params.offMeshConDir = linkDirs.empty() ? nullptr : linkDirs.data();
    params.offMeshConUserID = linkIds.empty() ? nullptr : linkIds.data();
    params.offMeshConCount = (int)linkRadii.size();
    params.walkableHeight = (float)cfg.walkableHeight * cfg.ch;
    params.walkableRadius = (float)cfg.walkableRadius * cfg.cs;
    params.walkableClimb = (float)cfg.walkableClimb * cfg.ch;
    params.tileX = result.coord.x;
    params.tileY = result.coord.y;
    params.tileLayer = 0;
    std::memcpy(params.bmin, pmesh.bmin, sizeof(params.bmin));
    std::memcpy(params.bmax, pmesh.bmax, sizeof(params.bmax));
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* data = nullptr;
    int dataSize = 0;
    if (!dtCreateNavMeshData(&params, &data, &dataSize)) {
        fail("Detour could not create the tile data");
        return;
    }
    NavTileBlob blob;
    blob.x = result.coord.x;
    blob.y = result.coord.y;
    blob.layer = 0;
    blob.hash = result.hash;
    blob.data.assign(data, data + dataSize);
    dtFree(data);
    result.polyCount = pmesh.npolys;
    result.blobs.clear();
    result.blobs.push_back(std::move(blob));
    scratch.release();
    stage = Stage::Done;
}

void NavMeshBuilder::TileJob::State::buildLayers() {
    if (cfg.tileSize > 255) {
        fail("runtime carving needs a tile size of at most 255 cells");
        return;
    }
    scratch.layers = rcAllocHeightfieldLayerSet();
    if (!scratch.layers ||
        !rcBuildHeightfieldLayers(&context, *scratch.compact, cfg.borderSize, cfg.walkableHeight, *scratch.layers)) {
        fail("Recast could not build the tile-cache layers");
        return;
    }
    rcFreeCompactHeightfield(scratch.compact);
    scratch.compact = nullptr;

    detail::DeflateCompressor compressor;
    const int count = std::min(scratch.layers->nlayers, MAX_LAYERS);
    result.blobs.clear();
    for (int i = 0; i < count; i++) {
        const rcHeightfieldLayer& layer = scratch.layers->layers[i];
        dtTileCacheLayerHeader header;
        std::memset(&header, 0, sizeof(header));
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx = result.coord.x;
        header.ty = result.coord.y;
        header.tlayer = i;
        std::memcpy(header.bmin, layer.bmin, sizeof(header.bmin));
        std::memcpy(header.bmax, layer.bmax, sizeof(header.bmax));
        header.width = (unsigned char)layer.width;
        header.height = (unsigned char)layer.height;
        header.minx = (unsigned char)layer.minx;
        header.maxx = (unsigned char)layer.maxx;
        header.miny = (unsigned char)layer.miny;
        header.maxy = (unsigned char)layer.maxy;
        header.hmin = (unsigned short)layer.hmin;
        header.hmax = (unsigned short)layer.hmax;

        unsigned char* data = nullptr;
        int dataSize = 0;
        if (dtStatusFailed(dtBuildTileCacheLayer(&compressor, &header, layer.heights, layer.areas, layer.cons, &data, &dataSize))) {
            fail("Detour could not compress a tile-cache layer");
            return;
        }
        NavTileBlob blob;
        blob.x = result.coord.x;
        blob.y = result.coord.y;
        blob.layer = i;
        blob.hash = result.hash;
        blob.data.assign(data, data + dataSize);
        dtFree(data);
        if (i == 0)
            blob.links = links;
        result.blobs.push_back(std::move(blob));
    }
    // layers have no polygons until the tile cache builds them; count the layer cells as a hint
    result.polyCount = 0;
    scratch.release();
    stage = Stage::Done;
}

NavMeshBuilder::TileJob::TileJob(std::shared_ptr<const NavBakeInput> input, uint32_t index)
    : state(std::make_unique<State>()) {
    state->input = std::move(input);
    state->index = index;
}

NavMeshBuilder::TileJob::~TileJob() = default;
NavMeshBuilder::TileJob::TileJob(TileJob&&) noexcept = default;
NavMeshBuilder::TileJob& NavMeshBuilder::TileJob::operator=(TileJob&&) noexcept = default;

const char* NavMeshBuilder::TileJob::stageName() const {
    switch (state->stage) {
        case Stage::Gather: return "gather";
        case Stage::Rasterize: return "rasterize";
        case Stage::Compact: return "compact";
        case Stage::Areas: return "areas";
        case Stage::Regions: return "regions";
        case Stage::Contours: return "contours";
        case Stage::Detail: return "detail";
        case Stage::Finish: return "finish";
        case Stage::Done: return "done";
    }
    return "done";
}

bool NavMeshBuilder::TileJob::step() {
    State& s = *state;
    if (s.stage == Stage::Done)
        return true;
    const auto begin = std::chrono::steady_clock::now();
    switch (s.stage) {
        case Stage::Gather: s.gather(); break;
        case Stage::Rasterize: s.rasterize(); break;
        case Stage::Compact: s.compact(); break;
        case Stage::Areas: s.markAreas(); break;
        case Stage::Regions: s.regions(); break;
        case Stage::Contours: s.contours(); break;
        case Stage::Detail: s.detail(); break;
        case Stage::Finish: s.finish(); break;
        case Stage::Done: break;
    }
    s.busyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    if (s.stage == Stage::Done)
        s.result.milliseconds = s.busyMs;
    return s.stage == Stage::Done;
}

bool NavMeshBuilder::TileJob::done() const {
    return state->stage == Stage::Done;
}

uint32_t NavMeshBuilder::TileJob::index() const {
    return state->index;
}

NavTileBuildResult& NavMeshBuilder::TileJob::result() {
    return state->result;
}

NavTileBuildResult NavMeshBuilder::buildTile(const NavBakeInput& input, uint32_t index) {
    // a non-owning pointer: the caller holds the input for the call
    TileJob job(std::shared_ptr<const NavBakeInput>(std::shared_ptr<const NavBakeInput>(), &input), index);
    while (!job.step()) {
    }
    return std::move(job.result());
}

#else // JASPERENGINE_NAVIGATION

#include "Log.h"

#include <atomic>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {
    void logUnavailable() {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log::error("Navigation: this build has no navmesh support (JASPERENGINE_NAVIGATION is off)");
    }
}

struct NavMeshBuilder::TileJob::State {
    uint32_t index = 0;
    NavTileBuildResult result;
};

NavMeshBuilder::TileJob::TileJob(std::shared_ptr<const NavBakeInput> input, uint32_t index)
    : state(std::make_unique<State>()) {
    state->index = index;
    if (input)
        state->result.coord = input->tileCoord(index);
    state->result.ok = false;
    state->result.error = "navmesh support is not in this build (JASPERENGINE_NAVIGATION is off)";
    logUnavailable();
}

NavMeshBuilder::TileJob::~TileJob() = default;
NavMeshBuilder::TileJob::TileJob(TileJob&&) noexcept = default;
NavMeshBuilder::TileJob& NavMeshBuilder::TileJob::operator=(TileJob&&) noexcept = default;

bool NavMeshBuilder::TileJob::step() { return true; }
const char* NavMeshBuilder::TileJob::stageName() const { return "done"; }
bool NavMeshBuilder::TileJob::done() const { return true; }
uint32_t NavMeshBuilder::TileJob::index() const { return state->index; }
NavTileBuildResult& NavMeshBuilder::TileJob::result() { return state->result; }

NavTileBuildResult NavMeshBuilder::buildTile(const NavBakeInput& input, uint32_t index) {
    NavTileBuildResult result;
    result.coord = input.tileCoord(index);
    result.ok = false;
    result.error = "navmesh support is not in this build (JASPERENGINE_NAVIGATION is off)";
    logUnavailable();
    return result;
}

#endif // JASPERENGINE_NAVIGATION
