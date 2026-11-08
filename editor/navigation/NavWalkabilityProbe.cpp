// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavWalkabilityProbe.h"

#include "Scene.h"
#include "component/NavMeshSurfaceComponent.h"
#include "math/AABB.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr float PI = 3.14159265358979323846f;

    // one input triangle (or terrain cell half) under the point, in nav space
    struct ProbeGround {
        float height = 0.0f;
        // degrees from up
        float slope = 0.0f;
        uint8_t recastArea = 1;
        Entity source = NULL_ENTITY;
    };

    std::string probeNumber(float value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2g", (double)value);
        if (std::fabs(value) >= 10.0f)
            std::snprintf(text, sizeof(text), "%.0f", (double)value);
        else if (std::fabs(value) >= 1.0f)
            std::snprintf(text, sizeof(text), "%.1f", (double)value);
        return text;
    }

    std::string probeName(Scene* scene, Entity entity) {
        return "'" + scene->getEntityName(entity) + "'";
    }

    // the triangle's height at the point's x, z when the point is over it (its xz footprint)
    bool heightOver(const float* a, const float* b, const float* c, const Vector3& p, float& height) {
        const float v0x = b[0] - a[0], v0z = b[2] - a[2];
        const float v1x = c[0] - a[0], v1z = c[2] - a[2];
        const float v2x = p.x - a[0], v2z = p.z - a[2];
        const float denom = v0x * v1z - v1x * v0z;
        // vertical (a wall) or degenerate: nothing stands on it
        if (std::fabs(denom) < 1e-8f)
            return false;
        const float u = (v2x * v1z - v1x * v2z) / denom;
        const float v = (v0x * v2z - v2x * v0z) / denom;
        if (u < -1e-5f || v < -1e-5f || u + v > 1.0f + 1e-5f)
            return false;
        height = a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]);
        return true;
    }

    float slopeOf(const float* a, const float* b, const float* c) {
        const Vector3 e0(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
        const Vector3 e1(c[0] - a[0], c[1] - a[1], c[2] - a[2]);
        Vector3 n = e0.crossProduct(e1);
        const float length = n.length();
        if (length <= 0.0f)
            return 90.0f;
        return std::acos(std::clamp(std::fabs(n.y) / length, 0.0f, 1.0f)) * 180.0f / PI;
    }

    // every surface under (and over) the point: the tile's triangles and the terrains' cells
    std::vector<ProbeGround> groundUnder(const nav::NavBakeInput& input, const Vector3& p) {
        std::vector<ProbeGround> out;
        const float point[3] = {p.x, p.y, p.z};
        const nav::NavTileCoord coord = input.tileAt(point);
        if (coord.x >= input.minTileX && coord.x <= input.maxTileX && coord.y >= input.minTileY && coord.y <= input.maxTileY) {
            const uint32_t index = input.tileIndex(coord.x, coord.y);
            if (index < input.tileTriangles.size()) {
                for (uint32_t t : input.tileTriangles[index]) {
                    const float* a = &input.vertices[(size_t)input.triangles[(size_t)t * 3] * 3];
                    const float* b = &input.vertices[(size_t)input.triangles[(size_t)t * 3 + 1] * 3];
                    const float* c = &input.vertices[(size_t)input.triangles[(size_t)t * 3 + 2] * 3];
                    ProbeGround ground;
                    if (!heightOver(a, b, c, p, ground.height))
                        continue;
                    ground.slope = slopeOf(a, b, c);
                    ground.recastArea = input.triangleAreas[t];
                    ground.source = t < input.triangleSources.size() ? input.triangleSources[t] : NULL_ENTITY;
                    out.push_back(ground);
                }
            }
        }
        for (size_t i = 0; i < input.terrains.size(); i++) {
            const nav::NavTerrainInput& terrain = input.terrains[i];
            const float boxMin[3] = {p.x - 1e-3f, terrain.bmin[1], p.z - 1e-3f};
            const float boxMax[3] = {p.x + 1e-3f, terrain.bmax[1], p.z + 1e-3f};
            int i0, i1, j0, j1;
            if (!terrain.cellRange(boxMin, boxMax, i0, i1, j0, j1))
                continue;
            for (int j = j0; j < j1; j++) {
                for (int k = i0; k < i1; k++) {
                    const Vector3 corners[4] = {terrain.toNav * terrain.localAt(k, j), terrain.toNav * terrain.localAt(k + 1, j),
                                                terrain.toNav * terrain.localAt(k + 1, j + 1), terrain.toNav * terrain.localAt(k, j + 1)};
                    const int halves[2][3] = {{0, 2, 1}, {0, 3, 2}};
                    for (const auto& half : halves) {
                        const float a[3] = {corners[half[0]].x, corners[half[0]].y, corners[half[0]].z};
                        const float b[3] = {corners[half[1]].x, corners[half[1]].y, corners[half[1]].z};
                        const float c[3] = {corners[half[2]].x, corners[half[2]].y, corners[half[2]].z};
                        ProbeGround ground;
                        if (!heightOver(a, b, c, p, ground.height))
                            continue;
                        ground.slope = slopeOf(a, b, c);
                        ground.recastArea = terrain.recastArea;
                        ground.source = i < input.terrainSources.size() ? input.terrainSources[i] : NULL_ENTITY;
                        out.push_back(ground);
                    }
                }
            }
        }
        return out;
    }

    bool insideVolume(const nav::NavConvexVolume& volume, const Vector3& p) {
        if (p.y < volume.minY || p.y > volume.maxY || volume.points.size() < 6)
            return false;
        // convex, either winding: the point is on one side of every edge
        const size_t n = volume.points.size() / 2;
        int sign = 0;
        for (size_t i = 0; i < n; i++) {
            const float ax = volume.points[i * 2], az = volume.points[i * 2 + 1];
            const float bx = volume.points[((i + 1) % n) * 2], bz = volume.points[((i + 1) % n) * 2 + 1];
            const float cross = (bx - ax) * (p.z - az) - (bz - az) * (p.x - ax);
            const int s = cross > 0.0f ? 1 : (cross < 0.0f ? -1 : 0);
            if (s == 0)
                continue;
            if (sign == 0)
                sign = s;
            else if (s != sign)
                return false;
        }
        return true;
    }

    // the modifier that set a GameObject's area (itself or the nearest ancestor applying to it)
    Entity modifierOf(Scene* scene, Entity entity) {
        const Entity modifier = nav::NavBakeCollector::areaModifierOf(scene, entity);
        return modifier != NULL_ENTITY ? modifier : entity;
    }

    // whether the tile under the point was built from other inputs than these
    bool tileStale(const nav::NavBakeInput& input, const nav::NavMesh& mesh, const Vector3& p) {
        const nav::NavMeshData* data = mesh.getData();
        if (!data)
            return false;
        const float point[3] = {p.x, p.y, p.z};
        const nav::NavTileCoord coord = input.tileAt(point);
        if (mesh.getHeader().config.hash() != input.config.hash())
            return true;
        const nav::NavTileBlob* blob = data->findTile(coord.x, coord.y);
        const bool inGrid = coord.x >= input.minTileX && coord.x <= input.maxTileX && coord.y >= input.minTileY && coord.y <= input.maxTileY;
        const uint64_t hash = inGrid ? input.tileHashes[input.tileIndex(coord.x, coord.y)] : 0;
        if (!blob)
            return hash != 0;
        return blob->hash != hash;
    }

}

NavProbeResult NavWalkabilityProbe::explain(Scene* scene, NavigationSystem& system, Entity surface, const Vector3& world,
                                            const nav::QueryFilter& filter, std::shared_ptr<const nav::NavBakeInput> input) {
    NavProbeResult result;
    auto reason = [&](const char* code, std::string message, Entity entity = NULL_ENTITY, float value = 0.0f, float limit = 0.0f,
                      std::string detail = std::string()) {
        NavProbeReason item;
        item.code = code;
        item.message = std::move(message);
        item.entity = entity;
        item.value = value;
        item.limit = limit;
        item.detail = std::move(detail);
        result.reasons.push_back(std::move(item));
    };
    const NavMeshSurfaceComponent* component = scene ? scene->findComponent<NavMeshSurfaceComponent>(surface) : nullptr;
    if (!component)
        return result;
    result.location = system.locateOn(surface, world, filter.areaMask);
    const nav::NavMesh* mesh = system.getNavMesh(surface);
    const std::string surfaceName = probeName(scene, surface);
    if (!mesh || !component->enabled) {
        reason("not_baked", "NavMesh Surface " + surfaceName + " has no navmesh: bake it.", surface);
        return result;
    }
    if (!input) {
        nav::NavCollectOptions options;
        options.recordSources = true;
        input = system.inspectInputs(surface, options, true).input;
    }

    const nav::NavFrame frame = nav::NavBakeCollector::frameOf(scene, surface);
    const Vector3 p = frame.toNav(world);
    const nav::NavBuildConfig& config = mesh->getHeader().config;
    const float unit = nav::NavBakeCollector::unitScaleOf(scene, *component);
    const nav::AgentTypeSettings agent = nav::getSettings().agentTypeOrDefault(component->agentType);
    // walkable here: a polygon right under the point, within a step of its height
    const float tolerance = config.agentClimb + 2.0f * config.cellHeight;
    const Vector3 tight(config.cellSize, tolerance, config.cellSize);
    const nav::NavFilter withMask = nav::NavFilter::fromSettings(filter.areaMask);
    const nav::NavFilter everyArea = nav::NavFilter::fromSettings(nav::ALL_AREAS);
    const bool covered = mesh->covers(p, withMask, tight);
    const bool coveredAll = covered || mesh->covers(p, everyArea, tight);
    const bool stale = input && tileStale(*input, *mesh, p);

    auto staleReason = [&]() {
        if (stale)
            reason("stale_tile", "The tile under the point changed since NavMesh Surface " + surfaceName +
                                     " was baked: bake again before trusting the navmesh here.", surface);
    };
    // on the mesh: only what still applies there (R11)
    if (coveredAll) {
        staleReason();
        if (!covered) {
            const nav::NavLocation any = system.locateOn(surface, world, nav::ALL_AREAS);
            const std::string area = nav::getSettings().areas[(size_t)std::min<int>(any.area, nav::AREA_COUNT - 1)].name;
            reason("filtered", "The navmesh here is area '" + area + "', which the Area Mask leaves out.", NULL_ENTITY, (float)any.area, 0.0f,
                   area);
        }
        const nav::NavLocation here = system.locateOn(surface, world, nav::ALL_AREAS);
        const std::vector<nav::NavIsland> islands = system.getIslands(surface);
        if (here.island >= 0 && islands.size() > 1) {
            float largest = 0.0f;
            int largestId = -1;
            for (const nav::NavIsland& island : islands) {
                if (island.area > largest) {
                    largest = island.area;
                    largestId = island.id;
                }
            }
            if (here.island != largestId && (size_t)here.island < islands.size()) {
                const float area = islands[(size_t)here.island].area;
                reason("small_island", "This is island #" + std::to_string(here.island) + " (" + probeNumber(area) +
                                           " m\xC2\xB2), not the largest (" + probeNumber(largest) + " m\xC2\xB2): agents elsewhere cannot reach it.",
                       surface, area, largest);
            }
        }
        return result;
    }

    staleReason();
    if (!input)
        return result;
    // where the inputs are
    if (component->collectObjects == NavCollectObjects::VOLUME) {
        const Vector3 half = component->volumeSize * 0.5f;
        const Vector3 lo = component->volumeCenter - half, hi = component->volumeCenter + half;
        const bool inside = frame.plane == nav::NavPlane::XY ? (p.x >= lo.x && p.x <= hi.x && p.z >= lo.y && p.z <= hi.y)
                                                             : (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z);
        if (!inside) {
            reason("outside_volume", "The point is outside NavMesh Surface " + surfaceName +
                                         "'s volume (Collect Objects = Volume): grow its Size or move its Center.", surface);
            return result;
        }
    }
    if (p.x < input->bmin[0] || p.x > input->bmax[0] || p.z < input->bmin[2] || p.z > input->bmax[2]) {
        reason("outside_bounds", "The point is outside everything NavMesh Surface " + surfaceName + " collected.", surface);
        return result;
    }

    const std::vector<ProbeGround> candidates = groundUnder(*input, p);
    // the ground: the highest surface within a step above the point, down to an agent's height below
    const ProbeGround* ground = nullptr;
    for (const ProbeGround& candidate : candidates) {
        if (candidate.height <= p.y + tolerance && candidate.height >= p.y - config.agentHeight &&
            (!ground || candidate.height > ground->height))
            ground = &candidate;
    }
    // GameObjects the collection left out, where the point is
    for (const auto& excluded : input->excludedEntities) {
        AABB box;
        if (!nav::NavBakeCollector::meshWorldBounds(scene, excluded.first, box))
            continue;
        Vector3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (int c = 0; c < 8; c++) {
            const Vector3 corner = frame.toNav(Vector3((c & 1) ? box.getMaximum().x : box.getMinimum().x, (c & 2) ? box.getMaximum().y : box.getMinimum().y,
                                                    (c & 4) ? box.getMaximum().z : box.getMinimum().z));
            lo.makeFloor(corner);
            hi.makeCeil(corner);
        }
        const bool vertical = frame.plane == nav::NavPlane::XY || (p.y >= lo.y - tolerance && p.y <= hi.y + tolerance);
        if (p.x >= lo.x && p.x <= hi.x && p.z >= lo.z && p.z <= hi.z && vertical)
            reason("excluded", probeName(scene, excluded.first) + " is left out of the bake (" + excluded.second +
                                   "): nothing of it is walkable.", excluded.first, 0.0f, 0.0f, excluded.second);
    }
    if (!ground) {
        reason("no_geometry", "Nothing NavMesh Surface " + surfaceName + " collected is under the point.", surface);
        return result;
    }

    // areas: a Not Walkable volume or mesh, a 2D collider footprint
    for (size_t v = 0; v < input->volumes.size(); v++) {
        const nav::NavConvexVolume& volume = input->volumes[v];
        const Entity source = v < input->volumeSources.size() ? input->volumeSources[v] : NULL_ENTITY;
        if (volume.recastArea != nav::RECAST_NULL_AREA || source == NULL_ENTITY)
            continue;
        if (insideVolume(volume, Vector3(p.x, ground->height, p.z)))
            reason("not_walkable_area", probeName(scene, source) + " marks this place Not Walkable.", source);
    }
    if (ground->recastArea == nav::RECAST_NOT_WALKABLE || ground->recastArea == nav::RECAST_NULL_AREA) {
        const Entity modifier = modifierOf(scene, ground->source);
        reason("not_walkable_area", probeName(scene, modifier) + " marks this ground Not Walkable.", modifier);
    }

    // the agent's limits
    if (frame.plane == nav::NavPlane::XZ) {
        if (ground->slope > config.agentMaxSlope + 0.01f)
            reason("too_steep", "The ground slopes " + probeNumber(ground->slope) + "\xC2\xB0 here; " + agent.name + "'s Max Slope is " +
                                    probeNumber(config.agentMaxSlope) + "\xC2\xB0.", ground->source, ground->slope, config.agentMaxSlope);
        float ceiling = std::numeric_limits<float>::max();
        Entity ceilingSource = NULL_ENTITY;
        for (const ProbeGround& candidate : candidates) {
            if (candidate.height > ground->height + config.cellHeight && candidate.height < ceiling) {
                ceiling = candidate.height;
                ceilingSource = candidate.source;
            }
        }
        const float headroom = ceiling - ground->height;
        if (ceiling < std::numeric_limits<float>::max() && headroom < config.agentHeight)
            reason("low_clearance", "There is " + probeNumber(headroom / unit) + " m of headroom here under " + probeName(scene, ceilingSource) +
                                        "; " + agent.name + "'s Height is " + probeNumber(config.agentHeight / unit) + " m.",
                   ceilingSource, headroom / unit, config.agentHeight / unit);
    }

    // Recast's own limits: the tile rebuilt with one of them relaxed (D5); a static tile even on a
    // carving surface, so it can be tested
    const float point[3] = {p.x, p.y, p.z};
    const nav::NavTileCoord coord = input->tileAt(point);
    if (coord.x < input->minTileX || coord.x > input->maxTileX || coord.y < input->minTileY || coord.y > input->maxTileY)
        return result;
    const uint32_t tile = input->tileIndex(coord.x, coord.y);
    const Vector3 onGround(p.x, ground->height, p.z);
    // the ground under the point is walkable (the point is above it): no Recast limit is why
    if (mesh->covers(onGround, withMask, tight))
        return result;
    nav::NavTileBlob rebuilt;
    nav::NavBuildConfig rebuiltConfig;
    auto coversWith = [&](const std::function<void(nav::NavBuildConfig&)>& relax) {
        rebuiltConfig = input->config;
        rebuiltConfig.carving = false;
        relax(rebuiltConfig);
        nav::NavTileBuildResult built = nav::NavMeshBuilder::buildTile(*input, tile, &rebuiltConfig);
        if (!built.ok || built.blobs.empty() || !nav::NavMesh::tileCovers(built.blobs.front(), rebuiltConfig, onGround, tight))
            return false;
        rebuilt = std::move(built.blobs.front());
        return true;
    };
    // stale: when the current inputs already give a polygon here, a bake is all it takes
    if (stale && coversWith([](nav::NavBuildConfig&) {}))
        return result;
    if (coversWith([](nav::NavBuildConfig& c) { c.walkableRadius = 0; })) {
        const float distance = result.location.found ? std::max(0.0f, config.agentRadius - result.location.snapDistance) : 0.0f;
        reason("near_wall", "The point is about " + probeNumber(distance / unit) + " m from a wall or edge; " + agent.name + "'s Radius is " +
                                probeNumber(config.agentRadius / unit) + " m.", NULL_ENTITY, distance / unit, config.agentRadius / unit);
    }
    if (frame.plane == nav::NavPlane::XZ && coversWith([](nav::NavBuildConfig& c) { c.walkableClimb = c.walkableHeight; })) {
        const float step = result.location.found ? std::fabs(ground->height - frame.toNav(result.location.position).y) : 0.0f;
        reason("step_too_high", "A ledge of " + probeNumber(step / unit) + " m is next to the point; " + agent.name + "'s Step Height is " +
                                    probeNumber(config.agentClimb / unit) + " m.", NULL_ENTITY, step / unit, config.agentClimb / unit);
    }
    if (coversWith([](nav::NavBuildConfig& c) {
            c.minRegionArea = 0;
            c.mergeRegionArea = 0;
        })) {
        // the patch's area: its island in the rebuilt tile
        float patch = 0.0f;
        nav::NavMeshData data;
        data.header.config = rebuiltConfig;
        data.header.maxTiles = 1;
        data.tiles.push_back(std::move(rebuilt));
        nav::NavMesh single;
        std::string error;
        if (single.create(std::move(data), false, error)) {
            const nav::NavLocation here = single.locate(onGround, everyArea, tight, 0.0f);
            const std::vector<nav::NavIsland>& islands = single.getIslands();
            if (here.island >= 0 && (size_t)here.island < islands.size())
                patch = islands[(size_t)here.island].area / (unit * unit);
        }
        reason("small_region", "The walkable patch here (" + probeNumber(patch) + " m\xC2\xB2) is smaller than NavMesh Surface " + surfaceName +
                                   "'s Minimum Region Area (" + probeNumber(component->minRegionArea) + " m\xC2\xB2), so the bake removes it.",
               surface, patch, component->minRegionArea);
    }
    return result;
}
