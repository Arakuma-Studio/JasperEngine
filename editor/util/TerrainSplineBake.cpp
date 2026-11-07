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
#include "command/type/TerrainMapDeltaCmd.h"
#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "spline/SplineEvaluator.h"
#include "spline/SplinePolygonFill.h"
#include "spline/SplineStations.h"
#include "subsystem/SplineSystem.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainOps.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>

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

        // T1 R20, R40: fn(world, current, sample) over the footprint through applyField, returning
        // what its bytes changed
        template<typename Fn>
        editor::TerrainMapRegion forEachTexel(terrain::TerrainMapStore& store, terrain::MapId map, Fn&& fn) const{
            editor::TerrainMapRegion changed;
            const terrain::MapFormat format = store.format(map);
            if (!format.exists){
                return changed;
            }
            const editor::TerrainMapRegion region = footprint(format.bounds.w, format.bounds.h);
            if (region.empty()){
                return changed;
            }
            struct Call{
                const RouteField* field;
                Fn* fn;
            } call{this, &fn};
            const terrain::FieldFn callback = [](void* user, float localX, float localZ, std::span<const float> current){
                const Call& c = *static_cast<const Call*>(user);
                terrain::FieldSample sample;
                (*c.fn)(c.field->frame.localToWorld * Vector3(localX, 0.0f, localZ), current, sample);
                return sample;
            };
            const terrain::TexelRect rect{region.minX, region.minY, region.width(), region.height()};
            const terrain::TerrainOpResult result = terrain::applyField(store, map, rect, callback, &call);
            for (const terrain::MapChangeReport& report : result.changed){
                changed.merge(report.rect.x, report.rect.y, report.rect.right() - 1, report.rect.bottom() - 1);
            }
            return changed;
        }
    };

    // The bake's kernels, on any store: heights

    editor::TerrainMapRegion storeBakeHeights(terrain::TerrainMapStore& store, const std::vector<spline::SplineStation>& route, const std::vector<Vector3>& closedOutline,
                                              const SplineTerrainSettings& settings, const editor::TerrainBakeFrame& frame){
        if (frame.maxHeight == 0.0f){
            return editor::TerrainMapRegion();
        }
        const terrain::MapId map{terrain::MapKind::Height, 0};
        const RouteField field(route, closedOutline, settings, frame, settings.falloff);
        return field.forEachTexel(store, map, [&](const Vector3& world, std::span<const float> current, terrain::FieldSample& out){
            float weight = 0.0f;
            float bed = 0.0f;
            if (!field.sample(world.x, world.z, 0.0f, settings.falloff, weight, bed)){
                return;
            }
            // the bed in the terrain's local units, normalized to maxHeight
            const float target = (frame.worldToLocal * Vector3(world.x, bed, world.z)).y / frame.maxHeight;
            const float next = current[0] + (target - current[0]) * weight;
            if ((settings.flattenMode == SplineFlattenMode::CutOnly && next >= current[0]) ||
                (settings.flattenMode == SplineFlattenMode::FillOnly && next <= current[0])){
                return;
            }
            out.weight = weight;
            out.target[0] = target;
        });
    }

    editor::TerrainMapRegion storeBakeBlend(terrain::TerrainMapStore& store, int blendMap, const std::vector<spline::SplineStation>& route, const std::vector<Vector3>& closedOutline,
                                            const SplineTerrainSettings& settings, const editor::TerrainBakeFrame& frame, int channel){
        const terrain::MapId map{terrain::MapKind::Blend, blendMap};
        const terrain::MapFormat format = store.format(map);
        if (!format.exists || format.channels < 3 || channel < 0 || channel > 2){
            return editor::TerrainMapRegion();
        }
        const float falloff = std::max(0.0f, settings.paintFalloff);
        const float noise = std::clamp(settings.paintNoise, 0.0f, 1.0f);
        const float strength = std::clamp(settings.paintStrength, 0.0f, 1.0f);
        const RouteField field(route, closedOutline, settings, frame, falloff + noise * falloff);
        return field.forEachTexel(store, map, [&](const Vector3& world, std::span<const float> current, terrain::FieldSample& out){
            // the edge breaks up: the band grows or shrinks by up to paintNoise of the falloff
            const float band = noise > 0.0f ? (valueNoise(world.x, world.z) - 0.5f) * 2.0f * noise * falloff : 0.0f;
            float weight = 0.0f;
            float bed = 0.0f;
            if (!field.sample(world.x, world.z, band, falloff, weight, bed)){
                return;
            }
            out.weight = weight * strength;
            if (!(out.weight > 0.0f)){
                return;
            }
            for (int c = 0; c < 3; c++){
                out.target[c] = c == channel ? 1.0f : 0.0f;
            }
            // whatever the weight, a painted texel ends opaque: the target that lands exactly on 1
            if (current.size() > 3){
                out.target[3] = current[3] + (1.0f - current[3]) / out.weight;
            }
        });
    }

    editor::TerrainMapRegion storeBakeDensity(terrain::TerrainMapStore& store, int layer, const std::vector<spline::SplineStation>& route, const std::vector<Vector3>& closedOutline,
                                              const SplineTerrainSettings& settings, const editor::TerrainBakeFrame& frame){
        const RouteField field(route, closedOutline, settings, frame, settings.falloff);
        return field.forEachTexel(store, terrain::MapId{terrain::MapKind::Density, layer}, [&](const Vector3& world, std::span<const float>, terrain::FieldSample& out){
            float weight = 0.0f;
            float bed = 0.0f;
            if (field.sample(world.x, world.z, 0.0f, settings.falloff, weight, bed)){
                out.weight = weight;
                out.target[0] = 0.0f;
            }
        });
    }

    // The pure bake's frame and pixels as a store
    terrain::RawMapStore rawStoreOf(const editor::TerrainBakeFrame& frame, terrain::MapId map, editor::TerrainBakeMap& pixels){
        terrain::RawMapStore store(frame.terrainSize, frame.maxHeight);
        store.borrowMap(map, pixels.width, pixels.height, pixels.channels, pixels.bytesPerChannel * 8, pixels.pixels);
        return store;
    }

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
    if (!map.valid()){
        return TerrainMapRegion();
    }
    terrain::RawMapStore store = rawStoreOf(frame, terrain::MapId{terrain::MapKind::Height, 0}, map);
    return storeBakeHeights(store, route, closedOutline, settings, frame);
}

editor::TerrainMapRegion editor::TerrainSplineBake::bakeBlend(const std::vector<spline::SplineStation>& route,
        const std::vector<Vector3>& closedOutline, const SplineTerrainSettings& settings, const TerrainBakeFrame& frame,
        int channel, TerrainBakeMap& map){
    if (!map.valid()){
        return TerrainMapRegion();
    }
    terrain::RawMapStore store = rawStoreOf(frame, terrain::MapId{terrain::MapKind::Blend, 0}, map);
    return storeBakeBlend(store, 0, route, closedOutline, settings, frame, channel);
}

editor::TerrainMapRegion editor::TerrainSplineBake::bakeDensity(const std::vector<spline::SplineStation>& route,
        const std::vector<Vector3>& closedOutline, const SplineTerrainSettings& settings, const TerrainBakeFrame& frame,
        TerrainBakeMap& map){
    if (!map.valid()){
        return TerrainMapRegion();
    }
    terrain::RawMapStore store = rawStoreOf(frame, terrain::MapId{terrain::MapKind::Density, 0}, map);
    return storeBakeDensity(store, 0, route, closedOutline, settings, frame);
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
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainComponent* terrain = sceneProject && terrainEntity != NULL_ENTITY ? sceneProject->scene->findComponent<TerrainComponent>(terrainEntity) : nullptr;
    if (!terrain){
        changes = TerrainBakeChanges();
        error = !sceneProject ? "No such scene." : "No terrain under the spline.";
        return false;
    }
    EditorTerrainStore store(project, sceneId, sceneProject->scene, terrainEntity, *terrain);
    return bakeInto(store, project, sceneId, splineEntity, terrainEntity, changes, error);
}

bool editor::TerrainSplineBake::bakeInto(terrain::TerrainMapStore& store, Project* project, uint32_t sceneId, Entity splineEntity, Entity terrainEntity,
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
        const terrain::MapFormat heights = store.format(terrain::MapId{terrain::MapKind::Height, 0});
        if (!heights.exists){
            changes.warnings.push_back("The terrain's heightmap has no data: nothing flattened.");
        }else{
            if (heights.bitsPerChannel < 16){
                changes.warnings.push_back("The heightmap is 8-bit: the road bed steps visibly (Convert heightmap to 16-bit).");
            }
            changes.heights = storeBakeHeights(store, route, outline, settings, frame);
        }
    }
    if (settings.paint){
        const int blendMap = static_cast<int>(settings.paintLayer / 3);
        if (blendMap >= static_cast<int>(terrain->blendMaps.size())){
            changes.warnings.push_back("The terrain has no blend map for surface layer " + std::to_string(settings.paintLayer) +
                                       ": paint it once with the Terrain tool to create it.");
        }else if (store.format(terrain::MapId{terrain::MapKind::Blend, blendMap}).exists){
            changes.blend = storeBakeBlend(store, blendMap, route, outline, settings, frame, static_cast<int>(settings.paintLayer % 3));
            changes.blendMap = blendMap;
        }else{
            changes.warnings.push_back("The blend map has no data: nothing painted.");
        }
    }
    if (settings.clearFoliage){
        for (size_t layer = 0; layer < terrain->foliageLayers.size() && layer < 32; layer++){
            if (!(settings.foliageMask & (1u << layer))){
                continue;
            }
            const TerrainMapRegion region = storeBakeDensity(store, static_cast<int>(layer), route, outline, settings, frame);
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

    // The bake lands on an overlay, and the step applies what it holds: the maps are written once
    EditorTerrainStore store(project, sceneId, scene, terrainEntity, terrain);
    terrain::OverlayMapStore overlay(store);
    if (!bakeInto(overlay, project, sceneId, splineEntity, terrainEntity, changes, error)){
        return false;
    }

    auto delta = std::make_unique<TerrainMapDeltaCmd>(project, sceneId, terrainEntity, splineEntity, overlay, false, "Apply Spline to Terrain");
    overlay.discard();
    // T5 R29: the footprint's trees go in the same undo step as the maps
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    const bool treesCleared = clearSplineTrees(project, sceneId, splineEntity, terrainEntity);
    if (delta->empty()){
        if (treesCleared) history->groupSince(mark);
        return true;
    }
    auto warnIfNoFile = [&](const Texture* texture){
        if (texture && texture->getPath(0).empty()){
            changes.warnings.push_back("A terrain map is not saved in a file: the bake of it is kept until the scene is saved.");
        }
    };
    if (!changes.heights.empty()) warnIfNoFile(&terrain.heightMap);
    if (!changes.blend.empty() && changes.blendMap >= 0) warnIfNoFile(&terrain.blendMaps[changes.blendMap]);
    for (const auto& item : changes.density){
        warnIfNoFile(&terrain.foliageLayers[item.first].densityMap);
    }
    history->addCommandNoMerge(delta.release());
    if (treesCleared) history->groupSince(mark);
    return true;
}

// ---- Live Terrain

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
    store = std::make_unique<EditorTerrainStore>(project, sceneId, scene, terrainEntity, scene->getComponent<TerrainComponent>(terrainEntity));
    overlay = std::make_unique<terrain::OverlayMapStore>(*store);
    // the map's file waits until the session ends, as for a stroke
    store->beginStroke();
    bakedVersion = 0;
    active = true;
    return update(error);
}

bool editor::TerrainSplineLiveSession::update(std::string& error){
    if (!active){
        return false;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    SplineComponent* splineComponent = sceneProject ? sceneProject->scene->findComponent<SplineComponent>(spline) : nullptr;
    const Transform* transform = sceneProject ? sceneProject->scene->findComponent<Transform>(spline) : nullptr;
    TerrainComponent* terrainComponent = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(terrain) : nullptr;
    if (!splineComponent || !transform || !terrainComponent){
        cancel();
        error = "The spline is gone.";
        return false;
    }
    store->rebind(*terrainComponent);
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
    // what an earlier re-bake wrote goes back, then the bake on the terrain as the session found it
    overlay->rewind();
    TerrainBakeChanges changes;
    if (!TerrainSplineBake::bakeInto(*overlay, project, sceneId, spline, terrain, changes, error)){
        return false;
    }
    overlay->flushToBase();
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
    if (t){
        store->rebind(*t);
        overlay->flushToBase();
        auto delta = std::make_unique<TerrainMapDeltaCmd>(project, sceneId, terrain, spline, *overlay, true, "Apply Spline to Terrain");
        if (!delta->empty()){
            CommandHandle::get(sceneId)->addCommandNoMerge(delta.release());
        }
    }
    overlay.reset();
    store.reset();
}

void editor::TerrainSplineLiveSession::cancel(){
    if (active){
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        TerrainComponent* t = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(terrain) : nullptr;
        if (t){
            store->rebind(*t);
            overlay->rewind();
        }
    }
    active = false;
    overlay.reset();
    store.reset();
    bakedVersion = 0;
}
