// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The route resampling and segment-nearest evaluation follow OpenEngine's TerrainRoutePolyline
// (MIT, see the header and NOTICE.md).

#include "TerrainSplineBake.h"
#include "command/type/TreeEditCmd.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/FoliageRules.h"
#include "component/Transform.h"

#include "Catalog.h"
#include "command/CommandHandle.h"
#include "command/CommandNames.h"
#include "command/type/TerrainMapPatchCmd.h"
#include "component/TerrainComponent.h"
#include "spline/SplineEvaluator.h"
#include "spline/SplinePolygonFill.h"
#include "spline/SplineStations.h"
#include "subsystem/SplineSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

using namespace jasperengine;

namespace {

    float smoothstep01(float t){
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // the weight of a band edge: 1 inside, a smooth fade over falloff, 0 past it
    float bandWeight(float distance, float band, float falloff){
        if (distance <= band){
            return 1.0f;
        }
        if (falloff <= 0.0f){
            return 0.0f;
        }
        return 1.0f - smoothstep01((distance - band) / falloff);
    }

    // value noise in [0, 1] over world XZ, cells of 2 units: the painted band's ragged edge
    float hash2(int x, int z){
        uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    }

    float valueNoise(float x, float z){
        const float fx = x * 0.5f;
        const float fz = z * 0.5f;
        const int ix = static_cast<int>(std::floor(fx));
        const int iz = static_cast<int>(std::floor(fz));
        const float tx = smoothstep01(fx - static_cast<float>(ix));
        const float tz = smoothstep01(fz - static_cast<float>(iz));
        const float a = hash2(ix, iz), b = hash2(ix + 1, iz), c = hash2(ix, iz + 1), d = hash2(ix + 1, iz + 1);
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
    }

    bool insideOutline(const std::vector<Vector3>& outline, float x, float z){
        bool inside = false;
        const size_t n = outline.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++){
            const Vector3& a = outline[i];
            const Vector3& b = outline[j];
            if ((a.z > z) != (b.z > z)){
                const float cross = (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x;
                if (x < cross){
                    inside = !inside;
                }
            }
        }
        return inside;
    }

    float readTexel(const editor::TerrainBakeMap& map, size_t texel, int channel){
        const size_t byte = (texel * static_cast<size_t>(map.channels) + static_cast<size_t>(channel)) * static_cast<size_t>(map.bytesPerChannel);
        if (map.bytesPerChannel >= 2){
            const unsigned int value = static_cast<unsigned int>(map.pixels[byte]) | (static_cast<unsigned int>(map.pixels[byte + 1]) << 8);
            return static_cast<float>(value) / 65535.0f;
        }
        return static_cast<float>(map.pixels[byte]) / 255.0f;
    }

    // true when the stored value changed
    bool writeTexel(editor::TerrainBakeMap& map, size_t texel, int channel, float value){
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        const size_t byte = (texel * static_cast<size_t>(map.channels) + static_cast<size_t>(channel)) * static_cast<size_t>(map.bytesPerChannel);
        if (map.bytesPerChannel >= 2){
            const unsigned int q = static_cast<unsigned int>(std::lround(clamped * 65535.0f));
            const unsigned char lo = static_cast<unsigned char>(q & 0xFF);
            const unsigned char hi = static_cast<unsigned char>((q >> 8) & 0xFF);
            const bool changed = map.pixels[byte] != lo || map.pixels[byte + 1] != hi;
            map.pixels[byte] = lo;
            map.pixels[byte + 1] = hi;
            return changed;
        }
        const unsigned char q = static_cast<unsigned char>(std::lround(clamped * 255.0f));
        const bool changed = map.pixels[byte] != q;
        map.pixels[byte] = q;
        return changed;
    }

    // The route's evaluation over a map: for each texel centre of the footprint, its world
    // position and the weight/height of the nearest route segment
    struct RouteField{
        const std::vector<spline::SplineStation>& route;
        const std::vector<Vector3>& outline;
        const SplineTerrainSettings& settings;
        const editor::TerrainBakeFrame& frame;
        spline::SplineStationGridXZ grid;
        float reach = 0.0f;
        bool area = false;

        RouteField(const std::vector<spline::SplineStation>& route, const std::vector<Vector3>& outline,
                   const SplineTerrainSettings& settings, const editor::TerrainBakeFrame& frame, float falloff)
            : route(route), outline(outline), settings(settings), frame(frame){
            area = settings.region == SplineTerrainRegion::Area && outline.size() >= 3;
            float widest = 0.0f;
            for (const spline::SplineStation& s : route){
                widest = std::max(widest, std::max(s.halfWidthLeft, s.halfWidthRight));
            }
            reach = (area ? 0.0f : widest + std::max(0.0f, settings.extraWidth)) + std::max(0.0f, falloff);
            grid.build(route);
        }

        // texel range of the footprint (inclusive; empty when nothing is near the map)
        editor::TerrainMapRegion footprint(int width, int height) const{
            editor::TerrainMapRegion region;
            if (route.size() < 2){
                return region;
            }
            float minX = std::numeric_limits<float>::max(), minZ = minX;
            float maxX = std::numeric_limits<float>::lowest(), maxZ = maxX;
            auto add = [&](const Vector3& world){
                for (int dx = -1; dx <= 1; dx += 2){
                    for (int dz = -1; dz <= 1; dz += 2){
                        const Vector3 local = frame.worldToLocal * Vector3(world.x + dx * reach, world.y, world.z + dz * reach);
                        minX = std::min(minX, local.x); maxX = std::max(maxX, local.x);
                        minZ = std::min(minZ, local.z); maxZ = std::max(maxZ, local.z);
                    }
                }
            };
            for (const spline::SplineStation& s : route){
                add(s.position);
            }
            for (const Vector3& p : outline){
                add(p);
            }
            const float half = frame.terrainSize * 0.5f;
            const float texelX = frame.terrainSize / static_cast<float>(width);
            const float texelZ = frame.terrainSize / static_cast<float>(height);
            const int x0 = static_cast<int>(std::floor((minX + half) / texelX - 0.5f));
            const int x1 = static_cast<int>(std::ceil((maxX + half) / texelX - 0.5f));
            const int z0 = static_cast<int>(std::floor((minZ + half) / texelZ - 0.5f));
            const int z1 = static_cast<int>(std::ceil((maxZ + half) / texelZ - 0.5f));
            if (x1 < 0 || z1 < 0 || x0 >= width || z0 >= height){
                return region;
            }
            region.merge(std::max(0, x0), std::max(0, z0), std::min(width - 1, x1), std::min(height - 1, z1));
            return region;
        }

        // the weight at a world point (band, falloff, edgeNoise in units) and the bed height
        bool sample(float x, float z, float band, float falloff, float& weight, float& height) const{
            const float maxDistance = area ? std::numeric_limits<float>::max() : reach + band;
            const spline::SplineStationHitXZ hit = grid.closest(x, z, maxDistance);
            if (!hit.valid){
                return false;
            }
            height = hit.height + settings.heightOffset;
            if (area){
                weight = insideOutline(outline, x, z) ? 1.0f : bandWeight(hit.distanceXZ, 0.0f, falloff);
            }else{
                const float half = (hit.lateral < 0.0f ? hit.halfWidthLeft : hit.halfWidthRight) + std::max(0.0f, settings.extraWidth);
                weight = bandWeight(hit.distanceXZ, half + band, falloff);
            }
            return weight > 0.0f;
        }

        template<typename Fn>
        editor::TerrainMapRegion forEachTexel(int width, int height, Fn&& fn) const{
            editor::TerrainMapRegion changed;
            const editor::TerrainMapRegion region = footprint(width, height);
            if (region.empty()){
                return changed;
            }
            const float half = frame.terrainSize * 0.5f;
            const float texelX = frame.terrainSize / static_cast<float>(width);
            const float texelZ = frame.terrainSize / static_cast<float>(height);
            for (int j = region.minY; j <= region.maxY; j++){
                for (int i = region.minX; i <= region.maxX; i++){
                    const float lx = (static_cast<float>(i) + 0.5f) * texelX - half;
                    const float lz = (static_cast<float>(j) + 0.5f) * texelZ - half;
                    const Vector3 world = frame.localToWorld * Vector3(lx, 0.0f, lz);
                    if (fn(static_cast<size_t>(j) * static_cast<size_t>(width) + static_cast<size_t>(i), world)){
                        changed.merge(i, j, i, j);
                    }
                }
            }
            return changed;
        }
    };

    editor::TerrainBakeFrame frameOf(Scene* scene, Entity terrainEntity){
        editor::TerrainBakeFrame frame;
        const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
        const Transform& transform = scene->getComponent<Transform>(terrainEntity);
        frame.localToWorld = transform.modelMatrix;
        frame.worldToLocal = transform.modelMatrix.inverse();
        frame.terrainSize = terrain.terrainSize;
        frame.maxHeight = terrain.maxHeight;
        return frame;
    }

    editor::TerrainBakeMap viewOf(Texture& texture){
        editor::TerrainBakeMap map;
        if (!editor::TerrainMapUtils::hasLoadedData(texture)){
            return map;
        }
        TextureData& data = texture.getData();
        map.pixels = static_cast<unsigned char*>(data.getData());
        map.width = data.getWidth();
        map.height = data.getHeight();
        map.channels = data.getChannels();
        map.bytesPerChannel = TextureData::getBytesPerChannel(data.getColorFormat());
        return map;
    }

    size_t mapBytes(const editor::TerrainBakeMap& map){
        return static_cast<size_t>(map.width) * static_cast<size_t>(map.height) * static_cast<size_t>(map.channels) *
               static_cast<size_t>(map.bytesPerChannel);
    }

    // the spline's world route and (Area) outline, as the bake reads them
    bool routeOf(Scene* scene, Entity splineEntity, Entity terrainEntity, std::vector<spline::SplineStation>& route,
                 std::vector<Vector3>& outline, std::vector<std::string>& warnings, std::string& error){
        const SplineComponent* spline = scene->findComponent<SplineComponent>(splineEntity);
        std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>();
        if (!spline || !system){
            error = "The GameObject has no Spline.";
            return false;
        }
        std::vector<spline::SplineStation> stations;
        if (!system->getWorldStations(splineEntity, 0.0f, stations, true) || stations.size() < 2){
            error = "The spline needs two knots or more.";
            return false;
        }
        const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
        const Transform& terrainTransform = scene->getComponent<Transform>(terrainEntity);
        const Texture& heightMap = terrain.heightMap;
        const int texels = heightMap.empty() ? 1024 : std::max(1, static_cast<int>(const_cast<Texture&>(heightMap).getWidth()));
        const float scale = std::max(std::fabs(terrainTransform.worldScale.x), std::fabs(terrainTransform.worldScale.z));
        editor::TerrainSplineBake::routeStations(stations, terrain.terrainSize * std::max(scale, 1.0e-6f) / static_cast<float>(texels), route);

        outline.clear();
        if (spline->terrain.region == SplineTerrainRegion::Area){
            if (!spline->spline.effectivelyClosed()){
                warnings.push_back("Area needs a closed spline of three knots or more: baked as a Path.");
            }else{
                const Transform& transform = scene->getComponent<Transform>(splineEntity);
                spline::sampleFillOutline(spline->spline, 0.5f, outline);
                for (Vector3& p : outline){
                    p = transform.modelMatrix * p;
                }
            }
        }
        return true;
    }

}

bool editor::TerrainBakeChanges::empty() const{
    if (!heights.empty() || !blend.empty()){
        return false;
    }
    for (const auto& item : density){
        if (!item.second.empty()) return false;
    }
    return true;
}

void editor::TerrainSplineBake::routeStations(const std::vector<spline::SplineStation>& stations, float texelSize,
                                              std::vector<spline::SplineStation>& route){
    spline::resampleStations(stations, std::max(MIN_ROUTE_SPACING, texelSize * 0.5f), route);
}

editor::TerrainMapRegion editor::TerrainSplineBake::bakeHeights(const std::vector<spline::SplineStation>& route,
        const std::vector<Vector3>& closedOutline, const SplineTerrainSettings& settings, const TerrainBakeFrame& frame,
        TerrainBakeMap& map){
    if (!map.valid() || frame.maxHeight == 0.0f){
        return TerrainMapRegion();
    }
    const RouteField field(route, closedOutline, settings, frame, settings.falloff);
    return field.forEachTexel(map.width, map.height, [&](size_t texel, const Vector3& world){
        float weight = 0.0f;
        float bed = 0.0f;
        if (!field.sample(world.x, world.z, 0.0f, settings.falloff, weight, bed)){
            return false;
        }
        // the bed in the terrain's local units, normalized to maxHeight
        const float target = (frame.worldToLocal * Vector3(world.x, bed, world.z)).y / frame.maxHeight;
        const float current = readTexel(map, texel, 0);
        float next = current + (target - current) * weight;
        if (settings.flattenMode == SplineFlattenMode::CutOnly){
            next = std::min(current, next);
        }else if (settings.flattenMode == SplineFlattenMode::FillOnly){
            next = std::max(current, next);
        }
        return writeTexel(map, texel, 0, next);
    });
}

editor::TerrainMapRegion editor::TerrainSplineBake::bakeBlend(const std::vector<spline::SplineStation>& route,
        const std::vector<Vector3>& closedOutline, const SplineTerrainSettings& settings, const TerrainBakeFrame& frame,
        int channel, TerrainBakeMap& map){
    if (!map.valid() || map.channels < 3 || channel < 0 || channel > 2){
        return TerrainMapRegion();
    }
    const float falloff = std::max(0.0f, settings.paintFalloff);
    const float noise = std::clamp(settings.paintNoise, 0.0f, 1.0f);
    const float strength = std::clamp(settings.paintStrength, 0.0f, 1.0f);
    const RouteField field(route, closedOutline, settings, frame, falloff + noise * falloff);
    return field.forEachTexel(map.width, map.height, [&](size_t texel, const Vector3& world){
        // the edge breaks up: the band grows or shrinks by up to paintNoise of the falloff
        const float band = noise > 0.0f ? (valueNoise(world.x, world.z) - 0.5f) * 2.0f * noise * falloff : 0.0f;
        float weight = 0.0f;
        float bed = 0.0f;
        if (!field.sample(world.x, world.z, band, falloff, weight, bed)){
            return false;
        }
        weight *= strength;
        bool changed = false;
        for (int c = 0; c < 3; c++){
            const float current = readTexel(map, texel, c);
            const float target = c == channel ? 1.0f : 0.0f;
            changed = writeTexel(map, texel, c, current + (target - current) * weight) || changed;
        }
        if (map.channels > 3){
            changed = writeTexel(map, texel, 3, 1.0f) || changed;
        }
        return changed;
    });
}

editor::TerrainMapRegion editor::TerrainSplineBake::bakeDensity(const std::vector<spline::SplineStation>& route,
        const std::vector<Vector3>& closedOutline, const SplineTerrainSettings& settings, const TerrainBakeFrame& frame,
        TerrainBakeMap& map){
    if (!map.valid()){
        return TerrainMapRegion();
    }
    const RouteField field(route, closedOutline, settings, frame, settings.falloff);
    return field.forEachTexel(map.width, map.height, [&](size_t texel, const Vector3& world){
        float weight = 0.0f;
        float bed = 0.0f;
        if (!field.sample(world.x, world.z, 0.0f, settings.falloff, weight, bed)){
            return false;
        }
        return writeTexel(map, texel, 0, readTexel(map, texel, 0) * (1.0f - weight));
    });
}

Entity editor::TerrainSplineBake::findTerrain(Scene* scene, Entity splineEntity){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(splineEntity);
    if (!spline){
        return NULL_ENTITY;
    }
    if (spline->terrain.terrain != NULL_ENTITY && scene->findComponent<TerrainComponent>(spline->terrain.terrain)){
        return spline->terrain.terrain;
    }
    const Transform* transform = scene->findComponent<Transform>(splineEntity);
    if (!transform || spline->spline.knotCount() == 0){
        return NULL_ENTITY;
    }
    // the first terrain whose square holds a knot
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); i++){
        const Entity entity = terrains->getEntity(i);
        const TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        const Transform* terrainTransform = scene->findComponent<Transform>(entity);
        if (!terrainTransform){
            continue;
        }
        const Matrix4 toLocal = terrainTransform->modelMatrix.inverse();
        const float half = terrain.terrainSize * 0.5f;
        for (const spline::SplineKnot& knot : spline->spline.knots()){
            const Vector3 local = toLocal * (transform->modelMatrix * knot.position);
            if (std::fabs(local.x) <= half && std::fabs(local.z) <= half){
                return entity;
            }
        }
    }
    return NULL_ENTITY;
}

bool editor::TerrainSplineBake::isHeightMap8Bit(Scene* scene, Entity terrainEntity){
    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(terrainEntity);
    if (!terrain || !TerrainMapUtils::hasLoadedData(terrain->heightMap)){
        return false;
    }
    return TextureData::getBytesPerChannel(terrain->heightMap.getData().getColorFormat()) < 2;
}

bool editor::TerrainSplineBake::bakeInPlace(Project* project, uint32_t sceneId, Entity splineEntity, Entity terrainEntity,
                                           TerrainBakeChanges& changes, std::string& error){
    changes = TerrainBakeChanges();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        error = "No such scene.";
        return false;
    }
    Scene* scene = sceneProject->scene;
    TerrainComponent* terrain = terrainEntity != NULL_ENTITY ? scene->findComponent<TerrainComponent>(terrainEntity) : nullptr;
    const SplineComponent* spline = scene->findComponent<SplineComponent>(splineEntity);
    if (!terrain || !spline){
        error = !spline ? "The GameObject has no Spline." : "No terrain under the spline.";
        return false;
    }
    std::vector<spline::SplineStation> route;
    std::vector<Vector3> outline;
    if (!routeOf(scene, splineEntity, terrainEntity, route, outline, changes.warnings, error)){
        return false;
    }
    const TerrainBakeFrame frame = frameOf(scene, terrainEntity);
    const SplineTerrainSettings& settings = spline->terrain;

    if (settings.flatten){
        TerrainBakeMap map = viewOf(terrain->heightMap);
        if (!map.valid()){
            changes.warnings.push_back("The terrain's heightmap has no data: nothing flattened.");
        }else{
            if (map.bytesPerChannel < 2){
                changes.warnings.push_back("The heightmap is 8-bit: the road bed steps visibly (Convert heightmap to 16-bit).");
            }
            changes.heights = bakeHeights(route, outline, settings, frame, map);
        }
    }
    if (settings.paint){
        const int blendMap = static_cast<int>(settings.paintLayer / 3);
        if (blendMap >= static_cast<int>(terrain->blendMaps.size())){
            changes.warnings.push_back("The terrain has no blend map for surface layer " + std::to_string(settings.paintLayer) +
                                       ": paint it once with the Terrain tool to create it.");
        }else{
            TerrainBakeMap map = viewOf(terrain->blendMaps[blendMap]);
            if (map.valid()){
                changes.blend = bakeBlend(route, outline, settings, frame, static_cast<int>(settings.paintLayer % 3), map);
                changes.blendMap = blendMap;
            }else{
                changes.warnings.push_back("The blend map has no data: nothing painted.");
            }
        }
    }
    if (settings.clearFoliage){
        for (size_t layer = 0; layer < terrain->foliageLayers.size() && layer < 32; layer++){
            if (!(settings.foliageMask & (1u << layer))){
                continue;
            }
            TerrainBakeMap map = viewOf(terrain->foliageLayers[layer].densityMap);
            if (!map.valid()){
                continue;
            }
            const TerrainMapRegion region = bakeDensity(route, outline, settings, frame, map);
            if (!region.empty()){
                changes.density.emplace_back(static_cast<int>(layer), region);
            }
        }
    }
    changes.texels = static_cast<uint32_t>(changes.heights.width() * changes.heights.height() +
                                           changes.blend.width() * changes.blend.height());
    return true;
}

// The trees whose base is within the band of the route the bake flattened (S18 R11's footprint)
std::vector<uint32_t> editor::TerrainSplineBake::treesInFootprint(const terrain::TreeStore& store, float terrainSize, const std::vector<Vector2>& line, float reach){
    std::vector<uint32_t> ids;
    for (const terrain::TreeRecord& record : store.records()){
        const float x = terrain::foliageTreeLocalX(record, terrainSize);
        const float z = terrain::foliageTreeLocalZ(record, terrainSize);
        if (terrain::foliageDistanceToPolyline(line, x, z) <= reach){
            ids.push_back(record.id);
        }
    }
    return ids;
}

static bool clearSplineTrees(editor::Project* project, uint32_t sceneId, Entity splineEntity, Entity terrainEntity){
    editor::SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;
    const SplineComponent* spline = scene->findComponent<SplineComponent>(splineEntity);
    const Transform* terrainTransform = scene->findComponent<Transform>(terrainEntity);
    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(terrainEntity);
    if (!spline || !spline->terrain.clearTrees || !terrainTransform || !terrain){
        return false;
    }
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    std::shared_ptr<terrain::TreeStore> store = foliage->getTreeStore(terrainEntity);
    if (!store || store->empty()){
        return false;
    }
    std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>();
    std::vector<spline::SplineStation> stations;
    if (!system || !system->getWorldStations(splineEntity, 0.0f, stations, true) || stations.size() < 2){
        return false;
    }
    const Matrix4 inverse = terrainTransform->modelMatrix.inverse();
    std::vector<Vector2> line;
    line.reserve(stations.size());
    float radius = 0.0f;
    for (const spline::SplineStation& station : stations){
        const Vector3 local = inverse * station.position;
        line.emplace_back(local.x, local.z);
        radius = std::max({radius, station.halfWidthLeft, station.halfWidthRight});
    }
    const float reach = radius + spline->terrain.extraWidth + spline->terrain.falloff;
    const std::vector<uint32_t> removed = editor::TerrainSplineBake::treesInFootprint(*store, terrain->terrainSize, line, reach);
    if (removed.empty()){
        return false;
    }
    terrain::TreeDelta delta;
    store->apply(removed, {}, {}, &delta);
    editor::CommandHandle::get(sceneId)->addCommand(new editor::TreeEditCmd(project, sceneId, terrainEntity,
        "Erase Trees (" + scene->getEntityName(terrainEntity) + ")", std::move(delta)));
    return true;
}

bool editor::TerrainSplineBake::apply(Project* project, uint32_t sceneId, Entity splineEntity, TerrainBakeChanges& changes,
                                      std::string& error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        error = "No such scene.";
        return false;
    }
    Scene* scene = sceneProject->scene;
    const Entity terrainEntity = findTerrain(scene, splineEntity);
    if (terrainEntity == NULL_ENTITY){
        error = "No terrain under the spline (set its Terrain, or move it over one).";
        return false;
    }
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);

    // the maps as they are: the patches' before pixels
    struct Before{
        TerrainMapRef ref;
        std::vector<unsigned char> pixels;
    };
    std::vector<Before> before;
    auto keep = [&](const TerrainMapRef& ref){
        Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
        TerrainBakeMap map = texture ? viewOf(*texture) : TerrainBakeMap();
        if (map.valid()){
            before.push_back({ref, std::vector<unsigned char>(map.pixels, map.pixels + mapBytes(map))});
        }
    };
    keep(TerrainMapRef(TerrainMapTarget::HeightMap));
    for (int m = 0; m < static_cast<int>(terrain.blendMaps.size()); m++){
        keep(TerrainMapRef(TerrainMapTarget::BlendMap, m));
    }
    for (int l = 0; l < static_cast<int>(terrain.foliageLayers.size()); l++){
        keep(TerrainMapRef(TerrainMapTarget::DensityMap, l));
    }

    if (!bakeInPlace(project, sceneId, splineEntity, terrainEntity, changes, error)){
        return false;
    }

    // each changed rect: its map back to before, then a patch command writes the bake again
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    size_t patches = 0;
    // T5 R29: the footprint's trees go in the same undo step, through remove_trees' store
    if (clearSplineTrees(project, sceneId, splineEntity, terrainEntity)) patches++;
    auto commit = [&](const TerrainMapRef& ref, const TerrainMapRegion& region){
        if (region.empty()){
            return;
        }
        Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
        TerrainBakeMap map = texture ? viewOf(*texture) : TerrainBakeMap();
        auto it = std::find_if(before.begin(), before.end(), [&](const Before& b){ return b.ref == ref; });
        if (!map.valid() || it == before.end()){
            return;
        }
        const int bytesPerTexel = map.channels * map.bytesPerChannel;
        TerrainMapPatch patch;
        patch.path = texture->getPath(0);
        patch.colorFormat = texture->getData().getColorFormat();
        patch.mapWidth = map.width;
        patch.mapHeight = map.height;
        patch.channels = map.channels;
        patch.region = region;
        patch.afterPixels = TerrainMapUtils::copyRegion(map.pixels, map.width, bytesPerTexel, region);
        patch.beforePixels = TerrainMapUtils::copyRegion(it->pixels.data(), map.width, bytesPerTexel, region);
        TerrainMapUtils::writeRegion(map.pixels, map.width, bytesPerTexel, region, patch.beforePixels);
        if (patch.path.empty()){
            changes.warnings.push_back("A terrain map is not saved in a file: the bake of it is kept until the scene is saved.");
        }
        history->addCommandNoMerge(new TerrainMapPatchCmd(project, sceneId, terrainEntity, ref, std::move(patch)));
        patches++;
    };
    commit(TerrainMapRef(TerrainMapTarget::HeightMap), changes.heights);
    if (changes.blendMap >= 0){
        commit(TerrainMapRef(TerrainMapTarget::BlendMap, changes.blendMap), changes.blend);
    }
    for (const auto& item : changes.density){
        commit(TerrainMapRef(TerrainMapTarget::DensityMap, item.first), item.second);
    }
    if (patches > 0){
        history->groupSince(mark, "Apply Spline to Terrain (" + CommandNames::entity(project, sceneId, splineEntity) + ")");
    }
    return true;
}

// ---- Live Terrain

editor::TerrainSplineLiveSession::MapCopy* editor::TerrainSplineLiveSession::findMap(const TerrainMapRef& ref){
    for (MapCopy& map : maps){
        if (map.ref == ref) return &map;
    }
    return nullptr;
}

bool editor::TerrainSplineLiveSession::begin(Project* project, uint32_t sceneId, Entity splineEntity, std::string& error){
    cancel();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        error = "No such scene.";
        return false;
    }
    Scene* scene = sceneProject->scene;
    const Entity terrainEntity = TerrainSplineBake::findTerrain(scene, splineEntity);
    if (terrainEntity == NULL_ENTITY){
        error = "No terrain under the spline.";
        return false;
    }
    this->project = project;
    this->sceneId = sceneId;
    this->spline = splineEntity;
    this->terrain = terrainEntity;
    maps.clear();
    TerrainComponent& t = scene->getComponent<TerrainComponent>(terrainEntity);
    auto snapshot = [&](const TerrainMapRef& ref){
        Texture* texture = TerrainMapUtils::findTexture(t, ref);
        TerrainBakeMap map = texture ? viewOf(*texture) : TerrainBakeMap();
        if (!map.valid()){
            return;
        }
        MapCopy copy;
        copy.ref = ref;
        copy.path = texture->getPath(0);
        copy.width = map.width;
        copy.height = map.height;
        copy.bytesPerTexel = map.channels * map.bytesPerChannel;
        copy.start.assign(map.pixels, map.pixels + mapBytes(map));
        maps.push_back(std::move(copy));
    };
    snapshot(TerrainMapRef(TerrainMapTarget::HeightMap));
    for (int m = 0; m < static_cast<int>(t.blendMaps.size()); m++){
        snapshot(TerrainMapRef(TerrainMapTarget::BlendMap, m));
    }
    for (int l = 0; l < static_cast<int>(t.foliageLayers.size()); l++){
        snapshot(TerrainMapRef(TerrainMapTarget::DensityMap, l));
    }
    bakedVersion = 0;
    active = true;
    return update(error);
}

void editor::TerrainSplineLiveSession::restore(){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject){
        return;
    }
    TerrainComponent* t = sceneProject->scene->findComponent<TerrainComponent>(terrain);
    if (!t){
        return;
    }
    for (MapCopy& copy : maps){
        if (copy.touched.empty()){
            continue;
        }
        Texture* texture = TerrainMapUtils::findTexture(*t, copy.ref);
        TerrainBakeMap map = texture ? viewOf(*texture) : TerrainBakeMap();
        if (!map.valid() || map.width != copy.width || map.height != copy.height){
            continue;
        }
        const std::vector<unsigned char> start = TerrainMapUtils::copyRegion(copy.start.data(), copy.width, copy.bytesPerTexel, copy.touched);
        TerrainMapUtils::writeRegion(map.pixels, map.width, copy.bytesPerTexel, copy.touched, start);
    }
}

bool editor::TerrainSplineLiveSession::update(std::string& error){
    if (!active){
        return false;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    SplineComponent* splineComponent = sceneProject ? sceneProject->scene->findComponent<SplineComponent>(spline) : nullptr;
    const Transform* transform = sceneProject ? sceneProject->scene->findComponent<Transform>(spline) : nullptr;
    if (!splineComponent || !transform){
        cancel();
        error = "The spline is gone.";
        return false;
    }
    // a change of the curve or of its GameObject's place
    uint64_t version = splineComponent->spline.version() * 1000003ull;
    for (int c = 0; c < 4; c++){
        for (int r = 0; r < 4; r++){
            const float v = transform->modelMatrix.get(c, r);
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            version = (version ^ bits) * 1099511628211ull;
        }
    }
    if (version == bakedVersion){
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    // the snapshot back over the old footprint (every texel an earlier re-bake wrote), then
    // the bake on the terrain as the session found it
    restore();
    TerrainBakeChanges changes;
    if (!TerrainSplineBake::bakeInPlace(project, sceneId, spline, terrain, changes, error)){
        return false;
    }
    auto touch = [&](const TerrainMapRef& ref, const TerrainMapRegion& region){
        if (MapCopy* copy = findMap(ref); copy && !region.empty()){
            copy->touched.merge(region.minX, region.minY, region.maxX, region.maxY);
        }
    };
    touch(TerrainMapRef(TerrainMapTarget::HeightMap), changes.heights);
    if (changes.blendMap >= 0){
        touch(TerrainMapRef(TerrainMapTarget::BlendMap, changes.blendMap), changes.blend);
    }
    for (const auto& item : changes.density){
        touch(TerrainMapRef(TerrainMapTarget::DensityMap, item.first), item.second);
    }
    for (const MapCopy& copy : maps){
        if (!copy.touched.empty()){
            TerrainMapUtils::refresh(sceneProject, terrain, copy.ref, &copy.touched);
        }
    }
    lastBakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bakedVersion = version;
    return true;
}

void editor::TerrainSplineLiveSession::end(){
    if (!active){
        return;
    }
    active = false;
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainComponent* t = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(terrain) : nullptr;
    if (!t){
        maps.clear();
        return;
    }
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    size_t patches = 0;
    for (MapCopy& copy : maps){
        if (copy.touched.empty()){
            continue;
        }
        Texture* texture = TerrainMapUtils::findTexture(*t, copy.ref);
        TerrainBakeMap map = texture ? viewOf(*texture) : TerrainBakeMap();
        if (!map.valid() || map.width != copy.width || map.height != copy.height){
            continue;
        }
        TerrainMapPatch patch;
        patch.path = copy.path;
        patch.colorFormat = texture->getData().getColorFormat();
        patch.mapWidth = map.width;
        patch.mapHeight = map.height;
        patch.channels = map.channels;
        patch.region = copy.touched;
        patch.afterPixels = TerrainMapUtils::copyRegion(map.pixels, map.width, copy.bytesPerTexel, copy.touched);
        patch.beforePixels = TerrainMapUtils::copyRegion(copy.start.data(), copy.width, copy.bytesPerTexel, copy.touched);
        if (patch.afterPixels == patch.beforePixels){
            continue;
        }
        history->addCommandNoMerge(new TerrainMapPatchCmd(project, sceneId, terrain, copy.ref, std::move(patch)));
        patches++;
    }
    if (patches > 0){
        history->groupSince(mark, "Apply Spline to Terrain (" + CommandNames::entity(project, sceneId, spline) + ")");
    }
    maps.clear();
}

void editor::TerrainSplineLiveSession::cancel(){
    if (active){
        restore();
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (sceneProject){
            for (const MapCopy& copy : maps){
                if (!copy.touched.empty()){
                    TerrainMapUtils::refresh(sceneProject, terrain, copy.ref, &copy.touched);
                }
            }
        }
    }
    active = false;
    maps.clear();
    bakedVersion = 0;
}
