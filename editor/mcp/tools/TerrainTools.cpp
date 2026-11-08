// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R68, R69, R95: edit_terrain, the declarative write route for agents (every op of Table 1 through
// the engine's applyOps, one undo step a call, all-or-nothing, dry_run), and export_terrain_heightmap.

#include "FoliageTools.h"
#include "McpTools.h"
#include "PlayViewCommon.h"
#include "TerrainToolsSupport.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/TerrainMaterialInfo.h"

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/TerrainMapDeltaCmd.h"
#include "command/type/TerrainTextureEditCmd.h"
#include "util/Angle.h"
#include "render/SceneRender.h"
#include "sceneview/SceneViewController.h"
#include "terrain/TerrainOverlay.h"
#if JASPERENGINE_TERRAIN_WORLD
#include "subsystem/TerrainWorldRuntime.h"
#include "terrainworld/TerrainWorldOverlay.h"
#endif
#include "window/SceneWindow.h"
#include "object/TerrainEdit.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/TerrainIssues.h"
#include "terrain/TerrainMapPersistence.h"
#include "util/TerrainMapFileWriter.h"
#include "terrain/TerrainRawIO.h"
#include "terrain/TerrainSizePresets.h"
#include "terrain/TerrainLayers.h"
#include "terrain/FoliageOps.h"
#include "terrain/TerrainOps.h"
#include "object/TerrainOpsJson.h"
#include "terrain/TerrainOpsUndo.h"
#include "util/FileUtils.h"
#include "util/FloatImage.h"
#include "util/ImpostorBaker.h"
#include "util/SafeWrite.h"
#include "subsystem/NavigationSystem.h"
#include "util/TerrainMapUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace terrain_tools {

namespace fs = std::filesystem;

using terrain::MapId;
using terrain::MapKind;
using terrain::Op;
using terrain::OpType;
using terrain::TexelRect;


bool hasKey(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

struct TerrainCallTarget {
    Project* project = nullptr;
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
};

// The terrain: `entity`, or the scene's only one, or the selected one
bool resolveTerrainTarget(const CallPtr& call, TerrainCallTarget& target) {
    target.project = call->ctx().project();
    std::string error;
    const Json& args = call->args();
    target.scene = McpRefs::resolveScene(target.project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!target.scene || !target.scene->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return false;
    }
    Scene* scene = target.scene->scene;
    if (hasKey(args, "entity")) {
        target.entity = McpRefs::resolveEntity(target.scene, args["entity"], error);
        if (target.entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
    } else {
        std::vector<Entity> terrains;
        for (Entity entity : target.scene->entities) {
            if (scene->findComponent<TerrainComponent>(entity)) terrains.push_back(entity);
        }
        if (terrains.size() == 1) {
            target.entity = terrains[0];
        } else {
            call->fail(terrains.empty() ? "The scene has no terrain. Create one with create_entity type terrain."
                                        : "The scene has several terrains: pass entity.");
            return false;
        }
    }
    if (!scene->findComponent<TerrainComponent>(target.entity) || !terrainWorldPlacement(scene, target.entity).valid) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" has no Terrain component.");
        return false;
    }
    return true;
}

terrain::OpSpace spaceOf(const TerrainWorldPlacement& placement, bool world) {
    return terrain::OpSpace::of(placement.model, placement.position, placement.scale, placement.rotated, world);
}

// Image files a mask or an import reads, through the project
bool readProjectFile(Project* project, const std::string& path, std::vector<uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    const fs::path absolute = project->resolveAssetPath(path);
    if (!fs::is_regular_file(absolute, ec)) {
        error = "\"" + path + "\" is not a file of the project.";
        return false;
    }
    std::ifstream file(absolute, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        error = "\"" + path + "\" is empty.";
        return false;
    }
    return true;
}

// What edit_terrain adds to the engine's op reader: project files for masks and imports, and the
// ops that place GameObjects stay with sculpt_terrain
terrain::OpJsonHooks editorHooks(Project* project, HeightImport& lastImport) {
    terrain::OpJsonHooks hooks;
    hooks.refused = {{OpType::PlaceObjects, "places GameObjects: use sculpt_terrain with place_object or erase_object."},
                     {OpType::EraseObjects, "places GameObjects: use sculpt_terrain with place_object or erase_object."}};
    hooks.loadMask = [project](const std::string& path, terrain::Shape& shape, std::string& error) {
        std::vector<uint8_t> bytes;
        if (!readProjectFile(project, path, bytes, error)) return false;
        FloatImage image;
        if (!readFloatImageFromMemory(bytes.data(), bytes.size(), path, image, &error)) return false;
        shape.maskWidth = image.width;
        shape.maskHeight = image.height;
        shape.maskPixels.resize(static_cast<size_t>(image.width) * image.height);
        for (size_t i = 0; i < shape.maskPixels.size(); i++) shape.maskPixels[i] = image.rgba[i * 4];
        return true;
    };
    hooks.importHeightmap = [project, &lastImport](const Json& item, Op& op, std::string& field, std::string& error) {
        field = "path";
        if (!hasKey(item, "path") || !item["path"].is_string()) {
            error = "is required: the heightmap file.";
            return false;
        }
        HeightImportOptions options;
        if (hasKey(item, "format")) {
            const std::string format = item["format"].is_string() ? item["format"].get<std::string>() : "";
            if (!TerrainRawIO::formatFromName(format, options.format)) {
                field = "format";
                error = "must be auto, r8, r16, r32f, png or exr.";
                return false;
            }
        }
        if (hasKey(item, "width")) options.width = item["width"].is_number_integer() ? item["width"].get<int>() : -1;
        if (hasKey(item, "height")) options.height = item["height"].is_number_integer() ? item["height"].get<int>() : -1;
        if (options.width < 0 || options.height < 0) {
            field = "width";
            error = "width and height are sample counts.";
            return false;
        }
        if (hasKey(item, "byte_order")) {
            const std::string order = item["byte_order"].is_string() ? item["byte_order"].get<std::string>() : "";
            if (order == "auto") options.byteOrder = HeightByteOrder::Auto;
            else if (order == "little") options.byteOrder = HeightByteOrder::Little;
            else if (order == "big") options.byteOrder = HeightByteOrder::Big;
            else {
                field = "byte_order";
                error = "must be auto, little or big.";
                return false;
            }
        }
        if (hasKey(item, "flip_vertically")) options.flipVertically = item["flip_vertically"].is_boolean() && item["flip_vertically"].get<bool>();
        if (hasKey(item, "range")) {
            const std::string range = item["range"].is_string() ? item["range"].get<std::string>() : "";
            if (range == "auto") options.range = HeightRange::Auto;
            else if (range == "full") options.range = HeightRange::Full;
            else if (range == "source") options.range = HeightRange::Source;
            else {
                field = "range";
                error = "must be auto, full or source.";
                return false;
            }
        }
        if (options.range == HeightRange::Source) {
            if (!hasKey(item, "source_min") || !hasKey(item, "source_max") || !item["source_min"].is_number() || !item["source_max"].is_number()) {
                field = "range";
                error = "source needs source_min and source_max.";
                return false;
            }
            options.sourceMin = item["source_min"].get<float>();
            options.sourceMax = item["source_max"].get<float>();
        }
        if (hasKey(item, "set_max_height") || hasKey(item, "resolution")) {
            field = hasKey(item, "set_max_height") ? "set_max_height" : "resolution";
            error = "changes the terrain itself: set maxHeight with set_properties, and a map of another size comes from importing a file of that size into a terrain without a height map.";
            return false;
        }
        std::vector<uint8_t> bytes;
        const std::string path = item["path"].get<std::string>();
        if (!readProjectFile(project, path, bytes, error)) return false;
        lastImport = TerrainRawIO::decode(path, bytes, options);
        if (!lastImport.ok) {
            error = lastImport.error;
            return false;
        }
        op.importedWidth = lastImport.width;
        op.importedHeight = lastImport.height;
        op.importedHeights = lastImport.normalized;
        return true;
    };
    return hooks;
}

// R68: what import_heightmap guessed, on its op's entry, so a wrong guess is seen before it is kept
void addImportDetection(Json& out, const std::vector<Op>& ops, const HeightImport& imported) {
    if (!imported.ok) return;
    for (size_t i = 0; i < ops.size() && i < out["ops"].size(); i++) {
        if (ops[i].type != OpType::ImportHeightmap) continue;
        out["ops"][i]["detected"] = {{"format", TerrainRawIO::formatName(imported.format)}, {"format_guessed", imported.formatGuessed},
                                     {"width", imported.width}, {"height", imported.height}, {"size_guessed", imported.sizeGuessed},
                                     {"byte_order", imported.byteOrder == HeightByteOrder::Big ? "big" : "little"},
                                     {"byte_order_guessed", imported.byteOrderGuessed}, {"data_min", imported.dataMin},
                                     {"data_max", imported.dataMax}, {"range_low", imported.rangeLow}, {"range_high", imported.rangeHigh},
                                     {"clipped_low", imported.clippedLow}, {"clipped_high", imported.clippedHigh}};
    }
}

// The call that verifies or fixes what a warning says (R95: next)
std::string nextFor(const terrain::TerrainOpResult& result) {
    if (result.hasWarning("clipped_high")) return "Heights reached the terrain's top: raise maxHeight with set_properties, then remap_heights to keep the shape.";
    if (result.hasWarning("filter_excluded_all")) return "The filter kept every texel out: check it with sample_terrain stats [\"height\", \"slope\"].";
    return "Check the result with sample_terrain (stats, hash) or look with visualize_terrain.";
}



// R70: stride when the map is larger than this per side
constexpr int MAX_STATS_SIDE = 2049;

double percentile(std::vector<float>& values, double share) {
    if (values.empty()) return 0.0;
    const size_t at = std::min(values.size() - 1, static_cast<size_t>(share * static_cast<double>(values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

} // namespace terrain_tools

bool sampleTerrainAsksExtras(const Json& args) {
    using terrain_tools::hasKey;
    return hasKey(args, "stats") || hasKey(args, "issues") || hasKey(args, "hash") || hasKey(args, "region") || hasKey(args, "world");
}

bool sampleTerrainExtras(const CallPtr& call, Project* project, SceneProject* sceneProject, Entity entity, bool world, Json& data) {
    using namespace terrain_tools;
    (void)project;
    const Json& args = call->args();
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    const TerrainWorldPlacement transform = terrainWorldPlacement(scene, entity);
    TerrainComponentStore store(scene, entity, terrain);
    const MapId heightMap{MapKind::Height, 0};

    if (hasKey(args, "issues") && args["issues"].get<bool>()) {
        Json issues = Json::array();
        for (const TerrainIssue& issue : TerrainIssues::check(terrain, TerrainMapFileWriter::get().failedCount())) {
            Json item = {{"code", issue.code}, {"severity", TerrainIssues::severityName(issue.severity)}, {"message", issue.message}};
            if (!issue.fixLabel.empty()) item["fix"] = issue.fixLabel;
            issues.push_back(item);
        }
        // R80: an edit leaves a NavMesh over the terrain as baked; the bake's own staleness says so
        auto surfaces = scene->getComponentArray<NavMeshSurfaceComponent>();
        auto navigation = scene->getSystem<NavigationSystem>();
        for (size_t i = 0; navigation && i < surfaces->size(); i++) {
            const Entity surface = surfaces->getEntity(i);
            if (navigation->countDirtyTiles(surface) > 0) {
                issues.push_back({{"code", "navmesh_stale"}, {"severity", "warning"},
                                  {"message", "The NavMesh of \"" + scene->getEntityName(surface) + "\" no longer matches the scene."},
                                  {"fix", "bake_navigation"}});
            }
        }
        data["issues"] = issues;
    }

    if (hasKey(args, "hash") && args["hash"].get<bool>()) {
        Json hashes = Json::object();
        std::vector<MapId> maps = {heightMap, MapId{MapKind::Hole, 0}};
        for (size_t i = 0; i < terrain.blendMaps.size(); i++) maps.push_back(MapId{MapKind::Blend, static_cast<int>(i)});
        for (size_t i = 0; i < terrain.foliageLayers.size(); i++) maps.push_back(MapId{MapKind::Density, static_cast<int>(i)});
        for (const MapId& map : maps) {
            if (store.format(map).exists) hashes[terrain::mapIdName(map)] = terrain::hashHex(terrain::mapHash(store, map));
        }
        data["hashes"] = hashes;
    }

    if (!hasKey(args, "stats")) return true;
    if (!args["stats"].is_array()) {
        call->fail("stats must list height, slope, layers or coverage.");
        return false;
    }
    bool wantHeight = false, wantSlope = false, wantLayers = false, wantCoverage = false;
    for (const Json& name : args["stats"]) {
        const std::string stat = name.is_string() ? name.get<std::string>() : "";
        if (stat == "height") wantHeight = true;
        else if (stat == "slope") wantSlope = true;
        else if (stat == "layers") wantLayers = true;
        else if (stat == "coverage") wantCoverage = true;
        else {
            call->fail("stats: \"" + stat + "\" is not a statistic: height, slope, layers, coverage.");
            return false;
        }
    }
    const terrain::MapFormat format = store.format(heightMap);
    if (!format.exists) {
        call->fail("stats: the terrain has no height map. edit_terrain create makes one.");
        return false;
    }

    // region: a box in the call's space, snapped outward to texels
    const terrain::OpSpace space = spaceOf(transform, world);
    const float half = terrain.terrainSize * 0.5f;
    float minX = -half, minZ = -half, maxX = half, maxZ = half;
    float boxHeight = terrain.maxHeight;
#if JASPERENGINE_TERRAIN_WORLD
    // T4 R4: a World's box is its tiles'
    jasperengine::terrain::TileCoord low, high;
    if (isWorld && terrain.world && terrain.world->started() && terrain.world->occupancy().bounds(low, high)) {
        const float size = terrain.world->lattice().tileSize();
        minX = static_cast<float>(low.x) * size;
        minZ = static_cast<float>(low.z) * size;
        maxX = static_cast<float>(high.x + 1) * size;
        maxZ = static_cast<float>(high.z + 1) * size;
        boxHeight = terrain.world->maxHeight();
    }
#endif
    if (hasKey(args, "region")) {
        const Json& region = args["region"];
        if (!region.is_object() || !hasKey(region, "min") || !hasKey(region, "max") || !region["min"].is_array() || !region["max"].is_array() ||
            region["min"].size() != 2 || region["max"].size() != 2) {
            call->fail("region must be {min: [x, z], max: [x, z]}.");
            return false;
        }
        const Vector2 a = space.point(region["min"][0].get<float>(), region["min"][1].get<float>());
        const Vector2 b = space.point(region["max"][0].get<float>(), region["max"][1].get<float>());
        minX = std::max(-half, std::min(a.x, b.x));
        maxX = std::min(half, std::max(a.x, b.x));
        minZ = std::max(-half, std::min(a.y, b.y));
        maxZ = std::min(half, std::max(a.y, b.y));
        if (maxX <= minX || maxZ <= minZ) {
            call->fail("region lies outside the terrain, whose local bounds are [" + std::to_string(-half) + ", " + std::to_string(half) + "].");
            return false;
        }
    }
    const Vector2 t0 = store.texelOfWorld(heightMap, minX, minZ);
    const Vector2 t1 = store.texelOfWorld(heightMap, maxX, maxZ);
    const int x0 = std::clamp(static_cast<int>(std::floor(std::min(t0.x, t1.x) + 0.5f)), 0, format.bounds.w - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(std::max(t0.x, t1.x) - 0.5f)), x0, format.bounds.w - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(std::min(t0.y, t1.y) + 0.5f)), 0, format.bounds.h - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(std::max(t0.y, t1.y) - 0.5f)), y0, format.bounds.h - 1);
    const int stride = std::max(1, (std::max(x1 - x0, y1 - y0) + MAX_STATS_SIDE) / MAX_STATS_SIDE);

    std::vector<float> heights;
    std::vector<float> slopes;
    const int bins = hasKey(args, "slope_bins") && args["slope_bins"].is_number_integer() ? std::clamp(args["slope_bins"].get<int>(), 1, 90) : 9;
    std::vector<int64_t> binCounts(static_cast<size_t>(bins), 0);
    const terrain::SurfaceView view = terrainSurfaceView(terrain);
    int64_t clippedLow = 0, clippedHigh = 0, texels = 0;
    for (int y = y0; y <= y1; y += stride) {
        for (int x = x0; x <= x1; x += stride) {
            const Vector2 local = store.worldOfTexel(heightMap, x, y);
            const terrain::SurfaceSample sample = terrain::sampleSurface(view, local.x, local.y);
            texels++;
            if (wantHeight) {
                const float normalized = terrain.maxHeight != 0.0f ? sample.height / terrain.maxHeight : 0.0f;
                if (normalized <= 0.0f) clippedLow++;
                else if (normalized >= 1.0f) clippedHigh++;
                heights.push_back(space.worldHeight(sample.height));
            }
            if (wantSlope) {
                const float degrees = Angle::radToDeg(std::acos(std::clamp(sample.normal.y, -1.0f, 1.0f)));
                slopes.push_back(degrees);
                binCounts[static_cast<size_t>(std::min(bins - 1, static_cast<int>(degrees / 90.0f * static_cast<float>(bins))))]++;
            }
        }
    }

    Json stats = {{"texels", texels}, {"stride", stride}};
    // T3 R43: the region's hole fraction and drawn-layer histogram
    if (wantCoverage) stats["coverage"] = mcp::terrainCoverageJson(terrain, minX, minZ, maxX, maxZ);
    if (wantHeight && !heights.empty()) {
        double sum = 0.0;
        for (float h : heights) sum += h;
        const float lo = *std::min_element(heights.begin(), heights.end());
        const float hi = *std::max_element(heights.begin(), heights.end());
        stats["height"] = {{"min", lo}, {"max", hi}, {"mean", sum / static_cast<double>(heights.size())},
                           {"p05", percentile(heights, 0.05)}, {"p50", percentile(heights, 0.5)}, {"p95", percentile(heights, 0.95)},
                           {"clipped_low_share", static_cast<double>(clippedLow) / static_cast<double>(texels)},
                           {"clipped_high_share", static_cast<double>(clippedHigh) / static_cast<double>(texels)}};
    }
    if (wantSlope && !slopes.empty()) {
        double sum = 0.0;
        for (float s : slopes) sum += s;
        Json edges = Json::array();
        Json shares = Json::array();
        for (int b = 0; b <= bins; b++) edges.push_back(90.0 * b / bins);
        for (int b = 0; b < bins; b++) shares.push_back(static_cast<double>(binCounts[static_cast<size_t>(b)]) / static_cast<double>(slopes.size()));
        stats["slope"] = {{"min", *std::min_element(slopes.begin(), slopes.end())}, {"max", *std::max_element(slopes.begin(), slopes.end())},
                          {"mean", sum / static_cast<double>(slopes.size())}, {"bins_deg", edges}, {"shares", shares}};
    }
    if (wantLayers) {
        Json layers = Json::array();
        const int layerCount = static_cast<int>(terrain.surfaceLayers.size());
        std::vector<double> coverage(static_cast<size_t>(layerCount), 0.0), weight(static_cast<size_t>(layerCount), 0.0);
        double baseCoverage = 0.0;
        int64_t samples = 0;
        std::vector<float> texel(4);
        for (int y = y0; y <= y1; y += stride) {
            for (int x = x0; x <= x1; x += stride) {
                const Vector2 local = store.worldOfTexel(heightMap, x, y);
                double total = 0.0;
                samples++;
                for (int l = 0; l < layerCount; l++) {
                    const terrain::LayerChannel channel = terrain::terrainLayerChannel(l);
                    const MapId blend{MapKind::Blend, channel.map};
                    const terrain::MapFormat blendFormat = store.format(blend);
                    if (!blendFormat.exists) continue;
                    const Vector2 at = store.texelOfWorld(blend, local.x, local.y);
                    const int bx = std::clamp(static_cast<int>(std::lround(at.x)), 0, blendFormat.bounds.w - 1);
                    const int by = std::clamp(static_cast<int>(std::lround(at.y)), 0, blendFormat.bounds.h - 1);
                    texel.assign(static_cast<size_t>(blendFormat.channels), 0.0f);
                    store.read(blend, TexelRect{bx, by, 1, 1}, texel);
                    const float value = channel.inverted ? 1.0f - texel[channel.channel] : texel[channel.channel];
                    weight[static_cast<size_t>(l)] += value;
                    total += value;
                    if (value > 0.5f) coverage[static_cast<size_t>(l)] += 1.0;
                }
                if (total < 0.5) baseCoverage += 1.0;
            }
        }
        for (int l = 0; l < layerCount; l++) {
            layers.push_back({{"index", l}, {"coverage", samples ? coverage[static_cast<size_t>(l)] / static_cast<double>(samples) : 0.0},
                              {"mean_weight", samples ? weight[static_cast<size_t>(l)] / static_cast<double>(samples) : 0.0}});
        }
        stats["layers"] = layers;
        stats["base_coverage"] = samples ? baseCoverage / static_cast<double>(samples) : 0.0;
    }
    data["stats"] = stats;
    return true;
}

namespace terrain_tools {

void editTerrainTool(const CallPtr& call) {
    TerrainCallTarget target;
    if (!resolveTerrainTarget(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.scene->scene;
    if (const std::string escape = McpRefs::writeEscape(target.project, target.project->getTerrainMapsDir()); !escape.empty()) {
        call->fail(escape);
        return;
    }

    // T4 R32: a World's heightmap makes tiles; one map's import would cover its whole addressable lattice
    if (scene->getComponent<TerrainComponent>(target.entity).world && hasKey(args, "ops") && args["ops"].is_array()) {
        for (size_t i = 0; i < args["ops"].size(); i++) {
            const Json& item = args["ops"][i];
            if (!item.is_object() || !hasKey(item, "op") || item["op"] != "import_heightmap") continue;
            call->fail("ops[" + std::to_string(i) + "]: on a Terrain World import_heightmap is edit_terrain_world's op, which makes the tiles the "
                       "file covers: edit_terrain_world {ops: [{op: \"import_heightmap\", path, origin}]}. Nothing was changed.");
            return;
        }
    }

    const TerrainWorldPlacement transform = terrainWorldPlacement(scene, target.entity);
    const terrain::OpSpace space = spaceOf(transform, !(hasKey(args, "space") && args["space"] == "local"));
    const bool dryRun = hasKey(args, "dry_run") && args["dry_run"].get<bool>();

    CommandHistory* history = CommandHandle::get(target.scene->id);
    const size_t start = history->mark();
    auto rollback = [&]() {
        while (history->position() > start && history->canUndo() && history->undo()) {}
    };

    // create: a terrain with no maps gets Table 6's sizes and a flat height map, in the same step
    if (hasKey(args, "create")) {
        const std::string id = args["create"].is_string() ? args["create"].get<std::string>() : "";
        const TerrainSizePreset* preset = nullptr;
        for (const TerrainSizePreset& p : TERRAIN_SIZE_PRESETS) {
            if (id == p.id) preset = &p;
        }
        if (!preset) {
            call->fail("create: \"" + id + "\" is not a preset: test_patch, mobile, small, medium, large, huge.");
            return;
        }
        if (!scene->getComponent<TerrainComponent>(target.entity).heightMap.empty()) {
            call->fail("create: the terrain already has a height map; leave create out to edit it.");
            return;
        }
        if (dryRun) {
            call->fail("create: a dry run cannot make a terrain; call without dry_run, then undo if needed.");
            return;
        }
        const int resolution = hasKey(args, "map_resolution") && args["map_resolution"].is_number_integer() ? args["map_resolution"].get<int>() : preset->heightmap;
        Json values = {{"terrainSize", preset->size}, {"maxHeight", preset->height}, {"resolution", preset->meshResolution},
                       {"rootGridSize", preset->root}, {"levels", preset->levels}};
        for (auto it = values.begin(); it != values.end(); ++it) {
            std::string error;
            PropertyData property = Catalog::findProperty(scene, target.entity, ComponentType::TerrainComponent, it.key(), true);
            Command* command = property.ref ? McpValues::buildPropertyCommand(target.project, target.scene->id, target.entity,
                                                                              ComponentType::TerrainComponent, it.key(), property, it.value(), error)
                                            : nullptr;
            if (!command) {
                rollback();
                call->fail("create: " + it.key() + ": " + error);
                return;
            }
            command->setNoMerge();
            if (!history->addCommand(command)) {
                rollback();
                call->fail("create: the terrain's sizes could not be set.");
                return;
            }
        }
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(target.entity);
        Texture* texture = TerrainMapUtils::findTexture(terrain, TerrainMapRef(TerrainMapTarget::HeightMap));
        TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(target.project, *texture, true);
        TerrainMapSnapshot after;
        after.empty = false;
        after.path = TerrainMapUtils::makeEditableTexturePath(target.project, target.scene->id, target.entity, TerrainMapRef(TerrainMapTarget::HeightMap));
        after.id = after.path;
        after.colorFormat = ColorFormat::RED16;
        after.channels = 1;
        after.width = resolution;
        after.height = resolution;
        after.pixels.assign(static_cast<size_t>(resolution) * resolution * 2, 0);
        Command* heightStep = new TerrainTextureEditCmd(target.project, target.scene->id, target.entity, TerrainMapRef(TerrainMapTarget::HeightMap),
                                                        std::move(before), std::move(after), "Create Terrain");
        heightStep->setNoMerge();
        if (!history->addCommand(heightStep)) {
            rollback();
            call->fail("create: the height map could not be made.");
            return;
        }
    }

    if (!hasKey(args, "ops") || !args["ops"].is_array() || args["ops"].empty()) {
        if (hasKey(args, "create")) {
            history->groupSince(start);
            call->ok("Created the terrain.", {{"terrain", McpRefs::entityJson(target.scene, target.entity)}, {"dry_run", false}});
            return;
        }
        call->fail("ops is required: a list of operations such as {\"op\": \"raise\", \"delta\": 2, \"shape\": {\"type\": \"circle\", \"center\": [0, 0], \"radius\": 5}}.");
        return;
    }

    HeightImport imported;
    terrain::OpJsonParser parser(space, editorHooks(target.project, imported));
    std::vector<Op> ops;
    if (!parser.parseList(args["ops"], ops)) {
        rollback();
        call->fail(parser.error);
        return;
    }

    // T5's ops write trees as well as density maps: they run as a job on a snapshot, in a call of their own
    if (std::any_of(ops.begin(), ops.end(), [](const Op& op) { return isFoliageOp(op); })) {
        if (hasKey(args, "create")) {
            rollback();
            call->fail("create: foliage ops run in a call of their own; create the terrain first, then send them.");
            return;
        }
        editFoliageFromOps(call, TerrainTarget{target.project, target.scene, target.entity}, std::move(ops), space, dryRun);
        return;
    }

    terrain::OpOptions options;
    if (hasKey(args, "map_resolution") && args["map_resolution"].is_number_integer()) options.newMapResolution = args["map_resolution"].get<int>();

    // Everything runs once as a dry run: the call's answer when dry, else what the real run must
    // prepare (maps to make first, a hole map to remove after)
    const terrain::TerrainOpResult preview = previewTerrainOps(target.project, target.scene, target.entity, ops, options);
    if (!preview.ok) {
        rollback();
        call->fail(preview.error);
        return;
    }

    std::string undoName = "Edit Terrain (" + scene->getEntityName(target.entity) + ")";
    if (dryRun) {
        Json out = terrain::opResultJson(preview, space, ops);
        addImportDetection(out, ops, imported);
        out["terrain"] = McpRefs::entityJson(target.scene, target.entity);
        out["dry_run"] = true;
        out["undo_name"] = undoName;
        out["next"] = "Nothing changed. Call again without dry_run to apply.";
        call->ok("Dry run: " + std::to_string(preview.changed.size()) + " map(s) would change.", out);
        return;
    }

    terrain::TerrainOpResult reported;
    std::string error;
    if (!applyTerrainOpsWithUndo(target.project, target.scene, target.entity, ops, options, preview, "Edit Terrain", reported, error)) {
        rollback();
        call->fail(error);
        return;
    }
    history->groupSince(start);

    Json out = terrain::opResultJson(reported, space, ops);
    addImportDetection(out, ops, imported);
    out["terrain"] = McpRefs::entityJson(target.scene, target.entity);
    out["dry_run"] = false;
    out["undo_name"] = undoName;
    out["next"] = nextFor(reported);
    if (hasKey(args, "hash") && args["hash"].get<bool>()) {
        Json hashes = Json::object();
        TerrainComponentStore written(scene, target.entity, scene->getComponent<TerrainComponent>(target.entity));
        for (const terrain::MapChangeReport& change : reported.changed) {
            hashes[terrain::mapIdName(change.map)] = terrain::hashHex(terrain::mapHash(written, change.map));
        }
        out["hashes"] = hashes;
    }
    int64_t texels = 0;
    for (const terrain::MapChangeReport& change : reported.changed) texels += change.texels;
    call->ok("Edited the terrain: " + std::to_string(texels) + " texel(s) changed in " + std::to_string(reported.changed.size()) + " map(s).", out);
}

void exportTerrainHeightmapTool(const CallPtr& call) {
    TerrainCallTarget target;
    if (!resolveTerrainTarget(call, target)) return;
    const Json& args = call->args();
    TerrainComponent& terrain = target.scene->scene->getComponent<TerrainComponent>(target.entity);
    if (!hasKey(args, "path") || !args["path"].is_string()) {
        call->fail("path is required: where under the project to write, such as \"assets/heights.r16\".");
        return;
    }
    HeightExportOptions options;
    const std::string formatName = hasKey(args, "format") && args["format"].is_string() ? args["format"].get<std::string>() : "r16";
    if (!TerrainRawIO::formatFromName(formatName, options.format) || options.format == HeightFileFormat::Auto) {
        call->fail("format must be r16, r8, r32f, png or exr.");
        return;
    }
    options.byteOrder = hasKey(args, "byte_order") && args["byte_order"] == "big" ? HeightByteOrder::Big : HeightByteOrder::Little;
    options.flipVertically = hasKey(args, "flip_vertically") && args["flip_vertically"].get<bool>();
    options.range = hasKey(args, "range") && args["range"] == "auto" ? HeightRange::Auto : HeightRange::Full;

    TerrainComponentStore store(target.scene->scene, target.entity, terrain);
    const MapId heightMap{MapKind::Height, 0};
    const terrain::MapFormat format = store.format(heightMap);
    if (!format.exists) {
        call->fail("The terrain has no height map to export: edit_terrain create makes one.");
        return;
    }
    std::vector<float> heights(static_cast<size_t>(format.bounds.area()));
    store.read(heightMap, format.bounds, heights);

    const std::string path = args["path"].get<std::string>();
    const fs::path absolute = target.project->resolveAssetPath(path);
    if (const std::string escape = McpRefs::writeEscape(target.project, absolute); !escape.empty()) {
        call->fail(escape);
        return;
    }
    // R30: what waits in memory is what the file shows
    TerrainMapPersistence::get().flushAll();

    double low = 0.0, high = 1.0;
    std::vector<uint8_t> bytes;
    std::string error;
    if (options.format == HeightFileFormat::Exr) {
        FloatImage image;
        image.width = format.bounds.w;
        image.height = format.bounds.h;
        image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
        for (int y = 0; y < image.height; y++) {
            const int row = options.flipVertically ? image.height - 1 - y : y;
            for (int x = 0; x < image.width; x++) {
                const float v = heights[static_cast<size_t>(row) * image.width + x];
                float* texel = image.rgba.data() + (static_cast<size_t>(y) * image.width + x) * 4;
                texel[0] = texel[1] = texel[2] = v;
                texel[3] = 1.0f;
            }
        }
        std::error_code ec;
        FileUtils::createDirectories(absolute.parent_path(), ec);
        if (!writeExr(FileUtils::pathToUtf8(absolute), image, false, &error)) {
            call->fail(error);
            return;
        }
    } else {
        if (!TerrainRawIO::encode(heights, format.bounds.w, format.bounds.h, options, bytes, error, &low, &high)) {
            call->fail(error);
            return;
        }
        if (!safewrite::writeFileResolvedBeneath(target.project->getProjectPath(), absolute, std::string(bytes.begin(), bytes.end()), error)) {
            call->fail(error);
            return;
        }
    }
    const float minHeight = *std::min_element(heights.begin(), heights.end()) * terrain.maxHeight;
    const float maxHeight = *std::max_element(heights.begin(), heights.end()) * terrain.maxHeight;
    Json out = {{"path", path}, {"format", TerrainRawIO::formatName(options.format)}, {"width", format.bounds.w}, {"height", format.bounds.h},
                {"bytes", bytes.size()}, {"min", minHeight}, {"max", maxHeight}, {"hash", terrain::hashHex(terrain::mapHash(store, heightMap))},
                {"warnings", Json::array()},
                {"next", "edit_terrain {op: import_heightmap, path, range: \"" + std::string(options.range == HeightRange::Full ? "full" : "source") +
                         "\"} reads it back" + (options.range == HeightRange::Auto ? " with source_min " + std::to_string(low) + " and source_max " + std::to_string(high) : "") + "."}};
    call->ok("Wrote " + path + " (" + std::to_string(format.bounds.w) + " x " + std::to_string(format.bounds.h) + ").", out);
}


// R65, R71, R90: an overlay on the Scene view, the terrain framed, and the view's image
void visualizeTerrainTool(const CallPtr& call) {
    TerrainCallTarget target;
    if (!resolveTerrainTarget(call, target)) return;
    const Json& args = call->args();
    if (target.scene->sceneType != SceneType::SCENE_3D) {
        call->fail("Terrain overlays are for 3D scenes.");
        return;
    }
    TerrainOverlaySettings settings = target.scene->displaySettings.terrainOverlay;
    const bool isWorld = target.scene->scene->findComponent<TerrainWorldComponent>(target.entity) != nullptr;
    if (hasKey(args, "overlay")) {
        const std::string name = args["overlay"].is_string() ? args["overlay"].get<std::string>() : "";
        bool worldOverlay = false;
#if JASPERENGINE_TERRAIN_WORLD
        TerrainWorldOverlayMode mode;
        worldOverlay = terrainWorldOverlayMode(name, mode);
#endif
        if (name == "none") {
            settings.source.clear();
        } else if (worldOverlay != isWorld && (worldOverlay || TerrainOverlay::hasSource(name))) {
            // T4 R46: a Terrain World draws its tiles' overlays, a Terrain its maps'
            call->fail(isWorld ? "overlay: \"" + name + "\" draws one Terrain's maps; on a Terrain World use tiles, tile_state, lod or collision."
                               : "overlay: \"" + name + "\" is for a Terrain World; on a Terrain use height, slope, contours or layer_weights.");
            return;
        } else if (!worldOverlay && !TerrainOverlay::hasSource(name)) {
            std::string names;
            for (const std::string& source : TerrainOverlay::sourceNames()) names += ", " + source;
            call->fail("overlay: \"" + name + "\" is not an overlay. Overlays: none" + names + "; on a Terrain World tiles, tile_state, lod, collision.");
            return;
        } else {
            settings.source = name;
        }
    }
    if (hasKey(args, "contour_interval")) settings.contourInterval = std::max(0.0f, args["contour_interval"].get<float>());
    if (hasKey(args, "slope_range")) {
        const Json& range = args["slope_range"];
        if (!range.is_array() || range.size() != 2 || !range[0].is_number() || !range[1].is_number() || range[1].get<float>() <= range[0].get<float>()) {
            call->fail("slope_range must be [min, max] degrees, min below max.");
            return;
        }
        settings.slopeMin = range[0].get<float>();
        settings.slopeMax = range[1].get<float>();
    }
    if (hasKey(args, "layer")) settings.layer = args["layer"].get<int>();
    if (hasKey(args, "opacity")) settings.opacity = std::clamp(args["opacity"].get<float>(), 0.0f, 1.0f);
    target.scene->displaySettings.terrainOverlay = settings;
    target.scene->needUpdateRender = true;
    // T4 R42: in Play the running copy's Scene view shows it too
    if (SceneProject* running = McpRefs::runtimeScene(target.project, target.scene); running != target.scene) {
        running->displaySettings.terrainOverlay = settings;
        running->needUpdateRender = true;
    }
    target.project->saveWorkspaceFile();

    // the box to frame: the terrain, or region in the call's space
    Scene* scene = target.scene->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(target.entity);
    const TerrainWorldPlacement transform = terrainWorldPlacement(scene, target.entity);
    const terrain::OpSpace space = spaceOf(transform, !(hasKey(args, "space") && args["space"] == "local"));
    const float half = terrain.terrainSize * 0.5f;
    float minX = -half, minZ = -half, maxX = half, maxZ = half;
    if (hasKey(args, "region")) {
        const Json& region = args["region"];
        if (!region.is_object() || !hasKey(region, "min") || !hasKey(region, "max") || !region["min"].is_array() || !region["max"].is_array() ||
            region["min"].size() != 2 || region["max"].size() != 2) {
            call->fail("region must be {min: [x, z], max: [x, z]}.");
            return;
        }
        const Vector2 a = space.point(region["min"][0].get<float>(), region["min"][1].get<float>());
        const Vector2 b = space.point(region["max"][0].get<float>(), region["max"][1].get<float>());
        minX = std::min(a.x, b.x);
        maxX = std::max(a.x, b.x);
        minZ = std::min(a.y, b.y);
        maxZ = std::max(a.y, b.y);
    }
    AABB box;
    for (int corner = 0; corner < 8; corner++) {
        box.merge(transform.model * Vector3((corner & 1) ? maxX : minX, (corner & 2) ? boxHeight : 0.0f, (corner & 4) ? maxZ : minZ));
    }
    const std::string framing = hasKey(args, "framing") && args["framing"].is_string() ? args["framing"].get<std::string>() : "fit";
    if (framing != "keep" && framing != "fit" && framing != "top" && framing != "orbit") {
        call->fail("framing must be keep, fit, top or orbit.");
        return;
    }
    const float yaw = hasKey(args, "yaw") ? args["yaw"].get<float>() : 45.0f;
    const float pitch = std::clamp(hasKey(args, "pitch") ? args["pitch"].get<float>() : 35.0f, -89.0f, 89.0f);
    const bool screenshot = !hasKey(args, "screenshot") || args["screenshot"].get<bool>();
    const int maxWidth = hasKey(args, "max_width") ? std::clamp(args["max_width"].get<int>(), 0, 4096) : 1024;
    const uint32_t sceneId = target.scene->id;
    const std::string overlayName = settings.source.empty() ? "none" : settings.source;

    w3::whenSceneViewShown(call, sceneId, true, 10000, [call, sceneId, box, framing, yaw, pitch, screenshot, maxWidth, overlayName](bool ready, const std::string& error) {
        if (!ready) {
            call->fail(error);
            return;
        }
        SceneProject* sceneProject = call->ctx().project()->getScene(sceneId);
        SceneWindow* window = call->ctx().sceneWindow();
        if (!sceneProject || !sceneProject->sceneRender || !window) {
            call->fail("The scene closed.");
            return;
        }
        SceneViewController& view = window->getViewController();
        view.ensureApplied(*sceneProject);
        if (framing != "keep") {
            const Vector3 center = box.getCenter();
            const float radius = std::max(box.getSize().length() * 0.5f, 0.5f);
            const float halfFov = sceneProject->viewState.camera.fieldOfView * 0.5f * 3.14159265f / 180.0f;
            const float distance = radius * 1.1f / std::max(0.05f, std::sin(halfFov));
            Vector3 direction;
            Vector3 up(0.0f, 1.0f, 0.0f);
            if (framing == "top") {
                direction = Vector3(0.0f, -1.0f, 0.0f);
                up = Vector3(0.0f, 0.0f, -1.0f);
            } else if (framing == "orbit") {
                const float y = Angle::degToRad(yaw), p = Angle::degToRad(pitch);
                direction = Vector3(-std::sin(y) * std::cos(p), -std::sin(p), -std::cos(y) * std::cos(p));
            } else {
                const SceneCameraPose pose = view.currentPose(*sceneProject);
                direction = pose.target - pose.position;
                if (direction.squaredLength() < 1e-12f) direction = Vector3(0.0f, -0.5f, -1.0f);
                direction.normalize();
            }
            view.lookFrom(*sceneProject, center - direction * distance, center, up);
            view.finishTween(*sceneProject);
        }
        call->afterFrames(2, [call, sceneId, screenshot, maxWidth, overlayName]() {
            SceneProject* shown = call->ctx().project()->getScene(sceneId);
            if (!shown || !shown->sceneRender) {
                call->fail("The scene closed.");
                return;
            }
            // in Play the legend is the running copy's, whose state the overlay shows
            SceneProject* running = McpRefs::runtimeScene(call->ctx().project(), shown);
            SceneRender* legendRender = running && running->sceneRender ? running->sceneRender : shown->sceneRender;
            Json legend = Json::array();
            for (const TerrainOverlayLegendStop& stop : legendRender->getTerrainOverlayLegend()) {
                legend.push_back({{"label", stop.label}, {"color", {stop.color.x, stop.color.y, stop.color.z}}});
            }
            Json data = {{"overlay", overlayName}, {"legend", legend}, {"camera", w3::sceneViewCameraJson(shown)},
                         {"next", "The overlay stays on in the Scene view; overlay none hides it."}};
            const std::string text = overlayName == "none" ? "Terrain overlay off." : "Terrain overlay " + overlayName + " on.";
            if (!screenshot) {
                call->ok(text, data);
                return;
            }
            w3::CaptureRequest request;
            request.view = "scene";
            request.scene = sceneId;
            request.maxWidth = maxWidth;
            w3::captureView(call, request, [call, data, text](w3::CaptureResult result) {
                if (!result.ok) {
                    call->fail("Screenshot failed: " + result.error, data);
                    return;
                }
                Json out = data;
                out["image"] = result.info;
                ToolResult reply = ToolResult::success(text + " Image: the Scene view.", out);
                reply.images.push_back(std::move(result.image));
                call->reply(std::move(reply));
            });
        });
    });
}

} // namespace terrain_tools

void registerTerrainTools(McpRegistry& registry) {
    using namespace schema;
    terrain::registerFoliageOps();
    // T5 R56: editor only, so Lua's applyOps does not know it
    ImpostorBaker::registerOp();

    ToolDef edit;
    edit.name = "edit_terrain";
    edit.title = "Edit Terrain";
    edit.group = "terrain";
    edit.attributesUndo = true;
    edit.allowedWhilePlaying = false;
    edit.description =
        "Declarative terrain edits, all-or-nothing, one undo step per call, no rates: absolute heights or signed deltas, seeded. "
        "dry_run changes nothing and returns the same result. create sizes a terrain with no maps. For numbers use sample_terrain. "
        "Ops and fields: jasper://docs/terrain.";
    edit.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef("Default: the only terrain.")},
        {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
        {"create", string("Size preset for a terrain with no maps.")},
        {"map_resolution", integer("New maps.", 2, 8192)},
        {"ops", Json{{"type", "array"}, {"maxItems", 256}, {"items", anyObject("{op, shape, filter, ...}: raise, lower, set_height, smooth, paint_layer, paint_by_rules, cut_holes, scatter_by_rules...")}}},
        {"dry_run", withDefault(boolean(""), false)},
        {"hash", withDefault(boolean("Hash each changed map."), false)},
        {"wait", withDefault(boolean("Foliage ops are a job: wait for its end."), true)},
        {"timeout_seconds", number("", 0, 3600)}
    });
    edit.handler = terrain_tools::editTerrainTool;
    registry.addTool(std::move(edit));

    ToolDef exportTool;
    exportTool.name = "export_terrain_heightmap";
    exportTool.title = "Export Terrain Heightmap";
    exportTool.group = "terrain";
    exportTool.allowedWhilePlaying = false;
    exportTool.annotations.idempotent = true;
    exportTool.description =
        "Write the heightmap to a raw or 16-bit image file under the project. Returns size, value range and hash. "
        "edit_terrain's import_heightmap with range full reads it back unchanged.";
    exportTool.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef("Default: the only terrain.")},
        {"path", string("Project-relative.")},
        {"format", withDefault(stringEnum("", {"r16", "r8", "r32f", "png", "exr"}), "r16")},
        {"byte_order", withDefault(stringEnum("", {"little", "big"}), "little")},
        {"flip_vertically", withDefault(boolean(""), false)},
        {"range", withDefault(stringEnum("", {"full", "auto"}), "full")}
    }, {"path"});
    exportTool.handler = terrain_tools::exportTerrainHeightmapTool;
    registry.addTool(std::move(exportTool));

    ToolDef visualize;
    visualize.name = "visualize_terrain";
    visualize.title = "Visualize Terrain";
    visualize.group = "terrain";
    visualize.allowedWhilePlaying = false;
    visualize.annotations.readOnly = true;
    visualize.description =
        "Show a terrain overlay in the Scene view (none hides it), frame the terrain and return a Scene view PNG with the "
        "overlay's legend and the camera. The view stays as set; no project data changes. Overlay names: jasper://docs/terrain.";
    visualize.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef("Default: the only terrain.")},
        {"overlay", string("height, slope, contours, layer_weights, none...")},
        {"contour_interval", number("World units; default: auto.", 0, 100000)},
        {"slope_range", Json{{"type", "array"}, {"items", Json{{"type", "number"}}}, {"description", "Degrees."}}},
        {"layer", integer("", 0, 64)},
        {"opacity", withDefault(number("", 0, 1), 0.7)},
        {"framing", withDefault(stringEnum("", {"keep", "fit", "top", "orbit"}), "fit")},
        {"yaw", number("Degrees.")},
        {"pitch", number("Degrees.", -89, 89)},
        {"region", anyObject("{min:[x,z],max:[x,z]}")},
        {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
        {"screenshot", withDefault(boolean(""), true)},
        {"max_width", withDefault(integer("", 0, 4096), 1024)}
    });
    visualize.handler = terrain_tools::visualizeTerrainTool;
    registry.addTool(std::move(visualize));

    registerTerrainWorldTools(registry);
}

}
