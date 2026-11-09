// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T4 R43-R46: edit_terrain_world (create a Terrain World; its tiles, seams, overview, streaming, focuses
// and collision; validated all-or-nothing, one undo step a call) and what sample_terrain adds for a
// World: its streaming status, a wait for an area as a job, and each point's state.

#include "McpTools.h"
#include "TerrainToolsSupport.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "EditorHost.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/PropertyCmd.h"
#include "command/type/TerrainWorldTilesCmd.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrainworld/TerrainWorldConvert.h"
#include "terrainworld/TerrainWorldEditor.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "terrainworld/TerrainWorldImport.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace terrain_world_tools {

using terraincore::TileCoord;
using terraincore::TileState;
using Clock = std::chrono::steady_clock;

constexpr double DEFAULT_TIMEOUT_SECONDS = 30.0;
constexpr size_t MAX_LISTED_TILES = 64;
constexpr size_t MAX_CALL_TILES = 4096;
constexpr size_t MAX_OPS = 64;
constexpr size_t MAX_TEXT_TILES = 16;

const char* const FREEZE_NEXT =
    "For screenshots: edit_terrain_world {ops: [{op: \"set_streaming\", freeze: true}]} stops loads, evictions and uploads so frames "
    "repeat; freeze false resumes streaming.";

bool given(const Json& object, const char* key) {
    return object.is_object() && object.contains(key) && !object[key].is_null();
}

std::string tileText(TileCoord tile) {
    return "(" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ")";
}

Json tileJson(TileCoord tile) {
    return Json::array({static_cast<int>(tile.x), static_cast<int>(tile.z)});
}

std::string tilesText(const std::vector<TileCoord>& tiles) {
    std::string text;
    for (size_t i = 0; i < tiles.size() && i < MAX_TEXT_TILES; i++) text += (i ? ", " : "") + tileText(tiles[i]);
    if (tiles.size() > MAX_TEXT_TILES) text += " and " + std::to_string(tiles.size() - MAX_TEXT_TILES) + " more";
    return text;
}

// T1 R95: create and the ops' items are open in the listed schema and closed here
bool checkKeys(const Json& object, const std::vector<std::string>& keys, const std::string& where, std::string& error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(keys.begin(), keys.end(), it.key()) != keys.end()) continue;
        std::string valid;
        for (const std::string& key : keys) {
            if (key != "op") valid += (valid.empty() ? "" : ", ") + key;
        }
        error = where + "." + it.key() + " is not a field" + (valid.empty() ? std::string("; it takes none.") : ": " + valid + ".");
        return false;
    }
    return true;
}

std::string numberText(double value) {
    if (value == std::floor(value) && std::fabs(value) < 1.0e9) return std::to_string(static_cast<long long>(value));
    std::string text = std::to_string(value);
    while (!text.empty() && text.back() == '0') text.pop_back();
    return text;
}

bool readNumber(const Json& object, const char* key, double low, double high, const std::string& where, std::optional<double>& out,
                std::string& error) {
    if (!given(object, key)) return true;
    const Json& value = object[key];
    if (!value.is_number() || !(value.get<double>() >= low && value.get<double>() <= high)) {
        error = where + "." + key + " must be a number from " + numberText(low) + " to " + numberText(high) + ".";
        return false;
    }
    out = value.get<double>();
    return true;
}

bool parseTiles(const Json& value, const std::string& where, std::vector<TileCoord>& out, std::string& error) {
    if (!value.is_array() || value.empty() || value.size() > MAX_CALL_TILES) {
        error = where + " must list 1 to " + std::to_string(MAX_CALL_TILES) + " tiles as [[x, z], ...].";
        return false;
    }
    for (size_t i = 0; i < value.size(); i++) {
        const Json& item = value[i];
        const std::string at = where + "[" + std::to_string(i) + "]";
        if (!item.is_array() || item.size() != 2 || !item[0].is_number_integer() || !item[1].is_number_integer()) {
            error = at + " must be [x, z] tile indices (integers).";
            return false;
        }
        const long long x = item[0].get<long long>(), z = item[1].get<long long>();
        if (x < terraincore::WORLD_TILE_MIN || x > terraincore::WORLD_TILE_MAX || z < terraincore::WORLD_TILE_MIN || z > terraincore::WORLD_TILE_MAX) {
            error = at + " is outside the tile range " + std::to_string(terraincore::WORLD_TILE_MIN) + " to " + std::to_string(terraincore::WORLD_TILE_MAX) + ".";
            return false;
        }
        out.emplace_back(static_cast<int>(x), static_cast<int>(z));
    }
    return true;
}

enum class WorldOpKind { AddTiles, RemoveTiles, ImportHeightmap, RepairSeams, RebuildOverview, SetStreaming, SetFocus, RemoveFocus, SetCollision };

struct WorldOpSpec {
    const char* name;
    WorldOpKind kind;
    std::vector<std::string> keys;
    bool inPlay;
};

const std::vector<WorldOpSpec>& worldOpSpecs() {
    static const std::vector<WorldOpSpec> specs = {
        {"add_tiles", WorldOpKind::AddTiles, {"op", "tiles"}, false},
        {"remove_tiles", WorldOpKind::RemoveTiles, {"op", "tiles"}, false},
        {"import_heightmap", WorldOpKind::ImportHeightmap, {"op", "path", "width", "height", "flip_vertically", "origin"}, false},
        {"repair_seams", WorldOpKind::RepairSeams, {"op", "tiles"}, false},
        {"rebuild_overview", WorldOpKind::RebuildOverview, {"op"}, false},
        {"set_streaming", WorldOpKind::SetStreaming, {"op", "pool_size", "lod_distance", "upload_budget", "freeze"}, true},
        {"set_focus", WorldOpKind::SetFocus, {"op", "id", "x", "z", "radius_scale", "velocity"}, true},
        {"remove_focus", WorldOpKind::RemoveFocus, {"op", "id"}, true},
        {"set_collision", WorldOpKind::SetCollision, {"op", "radius", "chunk_quads"}, false},
    };
    return specs;
}

struct WorldOp {
    WorldOpKind kind = WorldOpKind::AddTiles;
    std::string name;
    bool inPlay = false;
    std::vector<TileCoord> tiles;
    std::optional<double> poolSize, lodDistance, uploadBudget, radiusScale, collisionRadius, chunkQuads;
    std::optional<bool> freeze;
    std::optional<int> focusId;
    double x = 0.0, z = 0.0, velocityX = 0.0, velocityZ = 0.0;
    // import_heightmap: the file, its size when it does not say, and the tile its first sample lands on
    std::string path;
    int width = 0, height = 0;
    bool flipVertically = false;
    TileCoord origin;
    TerrainWorldImportPlan importPlan;
};

bool parseOp(const Json& item, size_t index, WorldOp& op, std::string& error) {
    const std::string where = "ops[" + std::to_string(index) + "]";
    std::string names;
    for (const WorldOpSpec& spec : worldOpSpecs()) names += (names.empty() ? "" : ", ") + std::string(spec.name);
    if (!item.is_object() || !given(item, "op") || !item["op"].is_string()) {
        error = where + " must be {op, ...}; ops: " + names + ".";
        return false;
    }
    op.name = item["op"].get<std::string>();
    const WorldOpSpec* spec = nullptr;
    for (const WorldOpSpec& candidate : worldOpSpecs()) {
        if (op.name == candidate.name) spec = &candidate;
    }
    if (!spec) {
        error = where + ".op: \"" + op.name + "\" is not an op: " + names + ".";
        return false;
    }
    op.kind = spec->kind;
    op.inPlay = spec->inPlay;
    if (!checkKeys(item, spec->keys, where, error)) return false;

    switch (op.kind) {
        case WorldOpKind::AddTiles:
        case WorldOpKind::RemoveTiles:
            if (!given(item, "tiles")) {
                error = where + ".tiles is required: [[x, z], ...].";
                return false;
            }
            return parseTiles(item["tiles"], where + ".tiles", op.tiles, error);
        case WorldOpKind::ImportHeightmap: {
            if (!given(item, "path") || !item["path"].is_string() || item["path"].get<std::string>().empty()) {
                error = where + ".path is required: the heightmap file in the project (.r16, .raw, .png or .exr).";
                return false;
            }
            op.path = item["path"].get<std::string>();
            for (const char* key : {"width", "height"}) {
                if (!given(item, key)) continue;
                if (!item[key].is_number_integer() || item[key].get<long long>() < 2 || item[key].get<long long>() > 65537) {
                    error = where + "." + key + " must be the file's samples a side, from 2 (0 or absent: detected).";
                    return false;
                }
                (std::string(key) == "width" ? op.width : op.height) = static_cast<int>(item[key].get<long long>());
            }
            if (given(item, "flip_vertically")) {
                if (!item["flip_vertically"].is_boolean()) {
                    error = where + ".flip_vertically must be true or false.";
                    return false;
                }
                op.flipVertically = item["flip_vertically"].get<bool>();
            }
            if (given(item, "origin")) {
                const Json& origin = item["origin"];
                if (!origin.is_array() || origin.size() != 2 || !origin[0].is_number_integer() || !origin[1].is_number_integer() ||
                    !TileCoord(static_cast<int>(origin[0].get<long long>()), static_cast<int>(origin[1].get<long long>())).addressable()) {
                    error = where + ".origin must be [x, z]: the tile the file's first sample lands on, from " + std::to_string(terraincore::WORLD_TILE_MIN) +
                            " to " + std::to_string(terraincore::WORLD_TILE_MAX) + ".";
                    return false;
                }
                op.origin = TileCoord(static_cast<int>(origin[0].get<long long>()), static_cast<int>(origin[1].get<long long>()));
            }
            return true;
        }
        case WorldOpKind::RepairSeams:
            return !given(item, "tiles") || parseTiles(item["tiles"], where + ".tiles", op.tiles, error);
        case WorldOpKind::RebuildOverview:
            return true;
        case WorldOpKind::SetStreaming:
            if (!readNumber(item, "pool_size", 0, 512, where, op.poolSize, error) || !readNumber(item, "lod_distance", 0, 16, where, op.lodDistance, error) ||
                !readNumber(item, "upload_budget", 0, 256, where, op.uploadBudget, error)) {
                return false;
            }
            if (op.poolSize && *op.poolSize != std::floor(*op.poolSize)) {
                error = where + ".pool_size must be a whole number of pages (0: the platform's).";
                return false;
            }
            if (given(item, "freeze")) {
                if (!item["freeze"].is_boolean()) {
                    error = where + ".freeze must be true or false.";
                    return false;
                }
                op.freeze = item["freeze"].get<bool>();
            }
            if (!op.poolSize && !op.lodDistance && !op.uploadBudget && !op.freeze) {
                error = where + " sets nothing: give pool_size, lod_distance, upload_budget or freeze.";
                return false;
            }
            return true;
        case WorldOpKind::SetFocus: {
            if (given(item, "id")) {
                if (!item["id"].is_number_integer() || item["id"].get<long long>() < 1 || item["id"].get<long long>() > 1000000000) {
                    error = where + ".id must be a focus id an earlier set_focus returned.";
                    return false;
                }
                op.focusId = static_cast<int>(item["id"].get<long long>());
            }
            if (!given(item, "x") || !given(item, "z") || !item["x"].is_number() || !item["z"].is_number()) {
                error = where + " needs x and z: the focus's world position.";
                return false;
            }
            op.x = item["x"].get<double>();
            op.z = item["z"].get<double>();
            if (!readNumber(item, "radius_scale", 0.01, 16, where, op.radiusScale, error)) return false;
            if (given(item, "velocity")) {
                const Json& velocity = item["velocity"];
                if (!velocity.is_array() || velocity.size() != 2 || !velocity[0].is_number() || !velocity[1].is_number()) {
                    error = where + ".velocity must be [x, z] world units a second.";
                    return false;
                }
                op.velocityX = velocity[0].get<double>();
                op.velocityZ = velocity[1].get<double>();
            }
            return true;
        }
        case WorldOpKind::RemoveFocus:
            if (!given(item, "id") || !item["id"].is_number_integer()) {
                error = where + ".id is required: the focus id set_focus returned.";
                return false;
            }
            op.focusId = static_cast<int>(item["id"].get<long long>());
            return true;
        case WorldOpKind::SetCollision:
            if (!readNumber(item, "radius", 0, 4096, where, op.collisionRadius, error) ||
                !readNumber(item, "chunk_quads", 32, 1024, where, op.chunkQuads, error)) {
                return false;
            }
            if (op.chunkQuads && !terraincore::isPowerOfTwo(static_cast<int>(*op.chunkQuads))) {
                error = where + ".chunk_quads must be a power of two from 32 to 1024.";
                return false;
            }
            if (!op.collisionRadius && !op.chunkQuads) {
                error = where + " sets nothing: give radius or chunk_quads.";
                return false;
            }
            return true;
    }
    return true;
}

bool parseCreate(const Json& value, TerrainWorldSettings& settings, std::string& error) {
    if (!value.is_object()) {
        error = "create must be {name, asset_path, tile_quads, spacing, max_height, base_height, grid: {width, length, origin: [x, z]}}.";
        return false;
    }
    if (!checkKeys(value, {"name", "asset_path", "tile_quads", "spacing", "max_height", "base_height", "grid"}, "create", error)) return false;
    if (given(value, "name")) {
        if (!value["name"].is_string() || value["name"].get<std::string>().empty()) {
            error = "create.name must be a non-empty string.";
            return false;
        }
        settings.name = value["name"].get<std::string>();
    }
    if (given(value, "asset_path")) {
        if (!value["asset_path"].is_string() || value["asset_path"].get<std::string>().empty()) {
            error = "create.asset_path must be a folder path under assets, such as \"Worlds/Island.terrainworld\".";
            return false;
        }
        settings.assetPath = value["asset_path"].get<std::string>();
    }
    if (given(value, "tile_quads")) {
        if (!value["tile_quads"].is_number_integer()) {
            error = "create.tile_quads must be 64, 128, 256, 512 or 1024.";
            return false;
        }
        settings.tileQuads = static_cast<int>(value["tile_quads"].get<long long>());
    }
    std::optional<double> number;
    if (!readNumber(value, "spacing", 0.25, 8, "create", number, error)) return false;
    if (number) settings.spacing = static_cast<float>(*number);
    number.reset();
    if (!readNumber(value, "max_height", 0.01, 100000, "create", number, error)) return false;
    if (number) settings.maxHeight = static_cast<float>(*number);
    number.reset();
    if (!readNumber(value, "base_height", 0, 100000, "create", number, error)) return false;
    if (number) settings.baseHeight = static_cast<float>(*number);
    if (given(value, "grid")) {
        const Json& grid = value["grid"];
        if (!grid.is_object() || !checkKeys(grid, {"width", "length", "origin"}, "create.grid", error)) {
            if (error.empty()) error = "create.grid must be {width, length, origin: [x, z]} in tiles.";
            return false;
        }
        for (const char* key : {"width", "length"}) {
            if (given(grid, key) && (!grid[key].is_number_integer() || grid[key].get<long long>() < 1 || grid[key].get<long long>() > 512)) {
                error = std::string("create.grid.") + key + " must be a whole number of tiles from 1.";
                return false;
            }
        }
        if (given(grid, "width")) settings.gridWidth = static_cast<int>(grid["width"].get<long long>());
        if (given(grid, "length")) settings.gridLength = static_cast<int>(grid["length"].get<long long>());
        // centred on the GameObject unless placed
        settings.originX = -settings.gridWidth / 2;
        settings.originZ = -settings.gridLength / 2;
        if (given(grid, "origin")) {
            const Json& origin = grid["origin"];
            if (!origin.is_array() || origin.size() != 2 || !origin[0].is_number_integer() || !origin[1].is_number_integer()) {
                error = "create.grid.origin must be [x, z]: the grid's first tile.";
                return false;
            }
            settings.originX = static_cast<int>(origin[0].get<long long>());
            settings.originZ = static_cast<int>(origin[1].get<long long>());
        }
    }
    if (!validateTerrainWorldSettings(settings, error)) {
        error = "create: " + error + ".";
        return false;
    }
    return true;
}

// R33: create {from_terrain}: the Terrain to convert and what the call chooses; the rest comes from it
bool parseConvert(const Json& value, SceneProject* sceneProject, Entity& source, TerrainConvertOptions& options, std::string& error) {
    for (const char* key : {"grid", "max_height", "base_height"}) {
        if (given(value, key)) {
            error = "create.from_terrain takes its grid and heights from the Terrain: leave out grid, max_height and base_height.";
            return false;
        }
    }
    if (!checkKeys(value, {"from_terrain", "name", "asset_path", "tile_quads", "spacing"}, "create", error)) return false;
    source = McpRefs::resolveEntity(sceneProject, value["from_terrain"], error);
    if (source == NULL_ENTITY) {
        error = "create.from_terrain: " + error;
        return false;
    }
    if (given(value, "name")) {
        if (!value["name"].is_string() || value["name"].get<std::string>().empty()) {
            error = "create.name must be a non-empty string.";
            return false;
        }
        options.name = value["name"].get<std::string>();
    }
    if (given(value, "asset_path")) {
        if (!value["asset_path"].is_string() || value["asset_path"].get<std::string>().empty()) {
            error = "create.asset_path must be a folder path under assets, such as \"Worlds/Island.terrainworld\".";
            return false;
        }
        options.assetPath = value["asset_path"].get<std::string>();
        const std::string suffix = ".terrainworld";
        if (options.assetPath.size() < suffix.size() || options.assetPath.compare(options.assetPath.size() - suffix.size(), suffix.size(), suffix) != 0) {
            options.assetPath += suffix;
        }
    }
    if (given(value, "tile_quads")) {
        if (!value["tile_quads"].is_number_integer()) {
            error = "create.tile_quads must be 64, 128, 256, 512 or 1024.";
            return false;
        }
        options.tileQuads = static_cast<int>(value["tile_quads"].get<long long>());
    }
    std::optional<double> number;
    if (!readNumber(value, "spacing", 0.25, 8, "create", number, error)) return false;
    if (number) options.spacing = static_cast<float>(*number);
    return true;
}

// The runtime for the scene and GameObject a job watches, or null once it is gone
TerrainWorldRuntime* liveRuntime(McpContext* context, uint32_t sceneId, Entity entity) {
    Project* project = context->project();
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene) return nullptr;
    std::shared_ptr<TerrainWorldSystem> system = sceneProject->scene->findSystem<TerrainWorldSystem>();
    TerrainWorldRuntime* runtime = system ? system->getRuntime(entity) : nullptr;
    return runtime && runtime->started() ? runtime : nullptr;
}

Vector3 worldOrigin(Scene* scene, Entity entity) {
    const Transform* transform = scene->findComponent<Transform>(entity);
    return transform ? transform->worldPosition : Vector3(0.0f, 0.0f, 0.0f);
}

uint64_t predictedBytes(const TerrainWorldRuntime& runtime, int tiles) {
    TerrainWorldSettings settings;
    settings.tileQuads = runtime.lattice().tileQuads;
    settings.spacing = runtime.lattice().spacing;
    settings.gridWidth = std::max(0, tiles);
    settings.gridLength = 1;
    return predictTerrainWorldBytes(settings);
}

// R45's numbers
Json worldStatus(const TerrainWorldRuntime& runtime, const std::vector<TileCoord>& asked) {
    const TerrainWorldStats stats = runtime.stats();
    Json counts = {{"total", stats.tiles}};
    for (int i = 0; i < 8; i++) counts[terraincore::tileStateName(static_cast<TileState>(i))] = stats.states[i];
    std::vector<TileCoord> listed = asked;
    bool truncated = false;
    if (listed.empty()) {
        listed = runtime.occupancy().presentTiles();
        if (listed.size() > MAX_LISTED_TILES) {
            listed.resize(MAX_LISTED_TILES);
            truncated = true;
        }
    }
    Json tiles = Json::array();
    for (const TileCoord& tile : listed) tiles.push_back({{"tile", tileJson(tile)}, {"state", terraincore::tileStateName(runtime.tileState(tile))}});
    Json draws = Json::array();
    for (int view = 0; view < 4; view++) {
        draws.push_back({{"view", view}, {"colour", stats.draws[view][0]}, {"depth", stats.draws[view][1]}, {"gbuffer", stats.draws[view][2]}});
    }
    Json out = {{"tile_counts", counts},
                {"tiles", tiles},
                {"ready", stats.ready},
                {"frozen", stats.frozen},
                {"gpu_bytes", stats.gpuBytes},
                {"cpu_bytes", stats.cpuBytes},
                {"budgets", {{"gpu_bytes", stats.gpuCap}, {"cpu_bytes", stats.cpuCap}, {"pool_size", stats.poolSize}}},
                {"queue", stats.queue},
                {"in_flight", stats.inFlight},
                {"uploads", {{"last_frame_bytes", stats.uploadBytesLastFrame}, {"total_bytes", stats.uploadBytesTotal}, {"retries", stats.uploadRetries}}},
                {"main_thread_ms", {{"last", stats.mainThreadMs}, {"worst", stats.worstMainThreadMs}}},
                {"draws", draws},
                {"nodes", {{"visited", stats.nodesVisited}, {"selected", stats.nodesSelected}}},
                {"collision", {{"fine", stats.collisionFine}, {"coarse", stats.collisionCoarse}, {"frozen", stats.frozenBodies}}},
                {"seam_errors", stats.seamErrors},
                {"pool_exhausted", stats.poolExhausted},
                {"late_pages", stats.latePages},
                {"errors", stats.errors}};
    if (truncated) out["tiles_listed"] = MAX_LISTED_TILES;
    return out;
}

// A job finished by a main-thread check run once an editor frame; check returns true once it has
// finished the job. A cancel_job reaches the check through cancelRequested().
void pollJob(const JobPtr& job, std::function<bool()> check) {
    getEditorHost().enqueueMainThreadTask([job, check]() {
        if (job->finished()) return;
        if (check()) return;
        pollJob(job, check);
    });
}

Entity onlyWorld(SceneProject* sceneProject, std::string& error) {
    std::vector<Entity> worlds;
    for (Entity entity : sceneProject->entities) {
        if (sceneProject->scene->findComponent<TerrainWorldComponent>(entity)) worlds.push_back(entity);
    }
    if (worlds.size() == 1) return worlds[0];
    error = worlds.empty() ? "The scene has no Terrain World: make one with edit_terrain_world {create}." : "The scene has several Terrain Worlds: pass entity.";
    return NULL_ENTITY;
}

// The tiles of a list (or the whole world) whose seams disagree with a neighbour now
std::vector<TileCoord> seamTiles(TerrainWorldRuntime& runtime, const std::vector<TileCoord>& asked, int& errors) {
    errors = 0;
    std::vector<TileCoord> found;
    for (const TileCoord& tile : asked.empty() ? runtime.occupancy().presentTiles() : asked) {
        const int count = runtime.store().seamErrors(tile, false);
        if (count > 0) {
            errors += count;
            found.push_back(tile);
        }
    }
    return found;
}

// R32: the file's size and the tiles it reaches, before anything changes
bool checkImport(Project* project, TerrainWorldRuntime& runtime, WorldOp& op, std::vector<TileCoord>& tiles, std::string& error) {
    TerrainWorldImportRequest request;
    request.path = op.path;
    request.width = op.width;
    request.height = op.height;
    request.flipVertically = op.flipVertically;
    request.origin = op.origin;
    if (!planTerrainWorldImport(project, runtime, request, op.importPlan, error)) {
        error = "path: " + error + ". Nothing was changed.";
        return false;
    }
    tiles = op.importPlan.covered;
    return true;
}

// R32: the import as a job (progress, cancel_job); the call waits for it unless wait is false
void importHeightmapJob(const CallPtr& call, Project* project, SceneProject* sceneProject, Entity entity, TerrainWorldRuntime& runtime, const WorldOp& op,
                        const Json& base, bool wait, double timeout) {
    const std::string name = sceneProject->scene->getEntityName(entity);
    JobPtr job = call->ctx().jobs().create("terrain_world_import", "Import Heightmap (" + name + ")");
    job->setProgress(0.0, "Reading the file");
    Json imported = Json::array();
    for (const TileCoord& tile : op.importPlan.covered) imported.push_back(tileJson(tile));
    Json warnings = Json::array();
    if (op.importPlan.sizeGuessed) {
        warnings.push_back("ops[0]: the file's size was detected as " + std::to_string(op.importPlan.width) + " x " + std::to_string(op.importPlan.height) +
                           " samples from its length: pass width and height when that is wrong.");
    }
    const int tiles = runtime.occupancy().tileCount() + op.importPlan.made;
    std::shared_ptr<TerrainWorldImportTask> task = startTerrainWorldImport(project, sceneProject->id, entity, op.importPlan,
        [job, imported, warnings, base, name, tiles](const TerrainWorldImportResult& result) {
            if (result.cancelled) {
                job->markCancelled("Cancelled; the Terrain World is as it was.");
                return;
            }
            if (!result.ok) {
                job->fail("ops[0]: " + result.error + ". Nothing was changed.");
                return;
            }
            Json data = base;
            data["tile_count"] = tiles;
            data["changed_tiles"] = {{"added", Json::array()}, {"removed", Json::array()}, {"imported", imported}, {"repaired", Json::array()}};
            data["warnings"] = warnings;
            data["dry_run"] = false;
            data["undo_name"] = "Import Heightmap (" + name + ")";
            data["next"] = "sample_terrain checks the heights; undo takes the whole import back.";
            job->succeed("Imported the heightmap into Terrain World \"" + name + "\": " + std::to_string(imported.size()) + " tile(s).", data);
        });
    job->setCancelHandler([task]() { task->cancel(); });
    pollJob(job, [job, task]() {
        job->setProgress(task->progress(), task->phase());
        return false;
    });
    call->detach();
    replyWhenJobDone(call, job, wait, timeout, [](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        return ToolResult::success(finished.message(), result);
    });
}

// R33: the conversion's job; its result is the GameObject made, or why nothing was
void convertFromTerrainJob(const CallPtr& call, Project* project, uint32_t sceneId, Entity source, const TerrainConvertOptions& options,
                           const std::string& name, bool wait, double timeout) {
    JobPtr job = call->ctx().jobs().create("terrain_world_convert", "Convert to Terrain World (" + name + ")");
    job->setProgress(-1.0, "Resampling the Terrain into tiles");
    std::function<void()> cancel = convertTerrainLater(project, sceneId, source, options, [job, project, sceneId, name](const TerrainConvertResult& result) {
        if (result.cancelled) {
            job->markCancelled("Cancelled; nothing changed.");
            return;
        }
        if (result.entity == NULL_ENTITY) {
            job->fail("create.from_terrain: " + result.error + ". Nothing was changed.");
            return;
        }
        SceneProject* sceneProject = project->getScene(sceneId);
        Json data = {{"entity", sceneProject ? McpRefs::entityJson(sceneProject, result.entity) : Json()}, {"asset_path", result.assetPath},
                     {"tile_count", result.tiles}, {"undo_name", "Convert to Terrain World (" + name + ")"},
                     {"next", "Sculpt and paint across tiles with edit_terrain or sculpt_terrain on this entity; sample_terrain {world} reports streaming."}};
        job->succeed("Converted into Terrain World \"" + name + "\": " + std::to_string(result.tiles) + " tile(s).", data);
    });
    job->setCancelHandler(cancel);
    call->detach();
    replyWhenJobDone(call, job, wait, timeout, [](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        return ToolResult::success(finished.message(), result);
    });
}

void editTerrainWorldTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sceneProject = McpRefs::resolveScene(project, given(args, "scene") ? args["scene"] : Json(), error);
    if (!sceneProject || !sceneProject->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return;
    }
    const bool playing = project->isAnyScenePlaying();
    const bool dryRun = given(args, "dry_run") && args["dry_run"].is_boolean() && args["dry_run"].get<bool>();
    const bool wait = !given(args, "wait") || !args["wait"].is_boolean() || args["wait"].get<bool>();
    const double timeout = given(args, "timeout_seconds") && args["timeout_seconds"].is_number()
                               ? std::clamp(args["timeout_seconds"].get<double>(), 0.0, 3600.0)
                               : DEFAULT_TIMEOUT_SECONDS;
    const bool creating = given(args, "create");
    if (!creating && !given(args, "ops")) {
        call->fail("Pass create to make a Terrain World, ops to change one, or both.");
        return;
    }
    if (creating && given(args, "entity")) {
        call->fail("create makes a new Terrain World: leave entity out, or drop create to change entity.");
        return;
    }

    // everything is checked before anything runs (programme D15)
    TerrainWorldSettings settings;
    const bool fromTerrain = creating && args["create"].is_object() && given(args["create"], "from_terrain");
    Entity source = NULL_ENTITY;
    TerrainConvertOptions convert;
    TerrainConvertPlan plan;
    if (fromTerrain) {
        if (!parseConvert(args["create"], sceneProject, source, convert, error)) {
            call->fail(error + " Nothing was changed.");
            return;
        }
        if (!planTerrainConversion(sceneProject->scene, source, convert, plan, error)) {
            call->fail("create.from_terrain: " + error + ". Nothing was changed.");
            return;
        }
        settings = plan.world;
        if (convert.assetPath.empty()) convert.assetPath = freeTerrainWorldAssetPath(project, settings.name);
        settings.assetPath = convert.assetPath;
    } else if (creating && !parseCreate(args["create"], settings, error)) {
        call->fail(error + " Nothing was changed.");
        return;
    }
    std::vector<WorldOp> ops;
    if (given(args, "ops")) {
        if (!args["ops"].is_array() || args["ops"].empty() || args["ops"].size() > MAX_OPS) {
            call->fail("ops must list 1 to " + std::to_string(MAX_OPS) + " operations such as {\"op\": \"add_tiles\", \"tiles\": [[0, 0]]}.");
            return;
        }
        for (size_t i = 0; i < args["ops"].size(); i++) {
            WorldOp op;
            if (!parseOp(args["ops"][i], i, op, error)) {
                call->fail(error + " Nothing was changed.");
                return;
            }
            ops.push_back(std::move(op));
        }
    }
    for (size_t i = 0; i < ops.size(); i++) {
        if (ops[i].kind != WorldOpKind::ImportHeightmap || (ops.size() == 1 && !creating)) continue;
        call->fail("ops[" + std::to_string(i) + "]: import_heightmap runs as a job: send it alone, in a call of its own on the World. Nothing was changed.");
        return;
    }
    if (playing) {
        if (creating) {
            call->fail("create does not run in Play; only the ops set_streaming, set_focus and remove_focus do. Stop Play first "
                       "(play {action: \"stop\"}). Nothing was changed.");
            return;
        }
        for (size_t i = 0; i < ops.size(); i++) {
            if (ops[i].inPlay) continue;
            call->fail("ops[" + std::to_string(i) + "]: " + ops[i].name + " does not run in Play; only set_streaming, set_focus and remove_focus do. "
                       "Stop Play first (play {action: \"stop\"}). Nothing was changed.");
            return;
        }
    }

    std::string assetPath;
    if (creating) {
        assetPath = settings.assetPath.empty() ? settings.name + ".terrainworld" : settings.assetPath;
        const std::string suffix = ".terrainworld";
        if (assetPath.size() < suffix.size() || assetPath.compare(assetPath.size() - suffix.size(), suffix.size(), suffix) != 0) assetPath += suffix;
        if (const std::string escape = McpRefs::writeEscape(project, project->resolveAssetPath(FileUtils::pathFromUtf8(assetPath))); !escape.empty()) {
            call->fail("create.asset_path: " + escape);
            return;
        }
        if (dryRun) {
            const int tiles = settings.gridWidth * settings.gridLength;
            Json out = {{"dry_run", true}, {"asset_path", assetPath}, {"tile_count", tiles}, {"predicted_bytes", predictTerrainWorldBytes(settings)},
                        {"tile_size", static_cast<double>(settings.tileQuads) * settings.spacing}, {"ops_checked", ops.size()},
                        {"warnings", Json::array()}, {"next", "Nothing changed. Call again without dry_run to create it."}};
            if (fromTerrain) {
                out["tile_quads"] = settings.tileQuads;
                out["spacing"] = settings.spacing;
                out["job"] = plan.samples > TERRAIN_CONVERT_JOB_SAMPLES;
                out["next"] = "Nothing changed. Call again without dry_run to convert; the Terrain is deactivated, not deleted.";
            }
            call->ok("Dry run: a Terrain World of " + std::to_string(tiles) + " tile(s) at " + assetPath + ".", out);
            return;
        }
    }

    // R33, R43: a large Terrain resamples on a job; the call waits for it unless wait is false
    if (fromTerrain && plan.samples > TERRAIN_CONVERT_JOB_SAMPLES) {
        if (!ops.empty()) {
            call->fail("create.from_terrain: a Terrain this large converts as a job, so send ops in a second call once it is done. Nothing was changed.");
            return;
        }
        convertFromTerrainJob(call, project, sceneProject->id, source, convert, settings.name, wait, timeout);
        return;
    }

    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t start = history->mark();
    auto rollback = [history, start]() {
        while (history->position() > start && history->canUndo() && history->undo()) {}
    };

    Entity entity = NULL_ENTITY;
    if (fromTerrain) {
        const TerrainConvertResult converted = convertTerrainNow(project, sceneProject->id, source, convert);
        entity = converted.entity;
        if (entity == NULL_ENTITY) {
            call->fail("create.from_terrain: " + converted.error + ". Nothing was changed.");
            return;
        }
    } else if (creating) {
        entity = createTerrainWorld(project, sceneProject->id, NULL_ENTITY, settings, error);
        if (entity == NULL_ENTITY) {
            call->fail("create: " + error);
            return;
        }
    } else if (given(args, "entity")) {
        entity = McpRefs::resolveEntity(sceneProject, args["entity"], error);
    } else {
        entity = onlyWorld(sceneProject, error);
    }
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    // Play changes the running copy's world, which never writes (R31)
    SceneProject* running = McpRefs::runtimeScene(project, sceneProject);
    Scene* scene = running->scene;
    TerrainWorldRuntime* runtime = terrainWorldRuntimeOf(scene, entity, error);
    if (!runtime) {
        rollback();
        call->fail(error);
        return;
    }
    const std::string name = scene->getEntityName(entity);

    // the checks that need the world as it is now
    std::vector<std::vector<TileCoord>> changes(ops.size());
    int newFocuses = 0;
    for (size_t i = 0; i < ops.size(); i++) {
        WorldOp& op = ops[i];
        const std::string where = "ops[" + std::to_string(i) + "]";
        bool ok = true;
        if (op.kind == WorldOpKind::AddTiles || op.kind == WorldOpKind::RemoveTiles) {
            ok = checkTerrainWorldTiles(*runtime, op.tiles, op.kind == WorldOpKind::RemoveTiles, changes[i], error);
            if (!ok) error = where + ": " + error;
        } else if (op.kind == WorldOpKind::ImportHeightmap) {
            ok = checkImport(project, *runtime, op, changes[i], error);
            if (!ok) error = where + "." + error;
        } else if (op.kind == WorldOpKind::RepairSeams) {
            for (const TileCoord& tile : op.tiles) {
                if (runtime->present(tile)) continue;
                error = where + ".tiles: tile " + tileText(tile) + " is not in the world: sample_terrain {world} lists its tiles. Nothing was changed.";
                ok = false;
                break;
            }
        } else if (op.kind == WorldOpKind::SetFocus && !op.focusId) {
            newFocuses++;
        }
        if (!ok) {
            rollback();
            call->fail(error);
            return;
        }
    }
    if (runtime->focusCount() + newFocuses > terraincore::WORLD_MAX_FOCUSES) {
        rollback();
        call->fail("A Terrain World streams round at most " + std::to_string(terraincore::WORLD_MAX_FOCUSES) + " focuses and this one has " +
                   std::to_string(runtime->focusCount()) + ": remove_focus frees one. Nothing was changed.");
        return;
    }

    const TerrainWorldComponent& component = scene->getComponent<TerrainWorldComponent>(entity);
    Json warnings = Json::array();
    Json added = Json::array(), removed = Json::array(), imported = Json::array(), repaired = Json::array(), focusIds = Json::array();
    auto base = [&](int tiles) {
        return Json{{"entity", McpRefs::entityJson(sceneProject, entity)}, {"guid", runtime->manifest().guid}, {"asset_path", component.worldData.path},
                    {"tile_count", tiles}, {"predicted_bytes", predictedBytes(*runtime, tiles)}, {"tile_size", runtime->lattice().tileSize()}};
    };

    if (dryRun) {
        int tiles = runtime->occupancy().tileCount();
        Json opsOut = Json::array();
        for (size_t i = 0; i < ops.size(); i++) {
            Json entry = {{"op", ops[i].name}};
            if (ops[i].kind == WorldOpKind::AddTiles || ops[i].kind == WorldOpKind::RemoveTiles) {
                const bool remove = ops[i].kind == WorldOpKind::RemoveTiles;
                Json list = Json::array();
                for (const TileCoord& tile : changes[i]) list.push_back(tileJson(tile));
                entry[remove ? "removed" : "added"] = list;
                tiles += remove ? -static_cast<int>(changes[i].size()) : static_cast<int>(changes[i].size());
            } else if (ops[i].kind == WorldOpKind::ImportHeightmap) {
                Json list = Json::array();
                for (const TileCoord& tile : changes[i]) list.push_back(tileJson(tile));
                tiles += ops[i].importPlan.made;
                entry["imported"] = list;
                entry["detected"] = {{"width", ops[i].importPlan.width}, {"height", ops[i].importPlan.height}, {"size_guessed", ops[i].importPlan.sizeGuessed}};
            } else if (ops[i].kind == WorldOpKind::RepairSeams) {
                int errors = 0;
                const std::vector<TileCoord> found = seamTiles(*runtime, ops[i].tiles, errors);
                entry["seam_errors"] = errors;
                entry["tiles"] = static_cast<int>(found.size());
            }
            opsOut.push_back(entry);
        }
        Json out = base(tiles);
        out["dry_run"] = true;
        out["ops"] = opsOut;
        out["warnings"] = warnings;
        out["next"] = "Nothing changed. Call again without dry_run to apply.";
        call->ok("Dry run of " + std::to_string(ops.size()) + " op(s) on Terrain World \"" + name + "\": nothing changed.", out);
        return;
    }

    auto addProperty = [&](Command* command) {
        command->setNoMerge();
        return history->addCommandNoMerge(command);
    };
    for (size_t i = 0; i < ops.size(); i++) {
        const WorldOp& op = ops[i];
        const std::string where = "ops[" + std::to_string(i) + "]: ";
        switch (op.kind) {
            case WorldOpKind::AddTiles:
            case WorldOpKind::RemoveTiles: {
                const bool remove = op.kind == WorldOpKind::RemoveTiles;
                std::vector<TileCoord> done;
                if (!editTerrainWorldTiles(project, sceneProject->id, entity, op.tiles, remove, error, &done)) {
                    rollback();
                    call->fail(where + error);
                    return;
                }
                for (const TileCoord& tile : done) {
                    (remove ? removed : added).push_back(tileJson(tile));
                    if (!remove && terraincore::tilePastWarning(runtime->lattice(), tile)) {
                        warnings.push_back("Tile " + tileText(tile) + " reaches past " + std::to_string(static_cast<int>(terraincore::WORLD_EXTENT_WARN)) +
                                           " m from the GameObject: float precision drops out there.");
                    }
                }
                if (!remove && done.size() < op.tiles.size()) {
                    warnings.push_back("ops[" + std::to_string(i) + "]: " + std::to_string(op.tiles.size() - done.size()) + " tile(s) were in the world already.");
                }
                break;
            }
            case WorldOpKind::ImportHeightmap:
                // alone in its call (checked above): the job replies
                importHeightmapJob(call, project, sceneProject, entity, *runtime, op, base(runtime->occupancy().tileCount()), wait, timeout);
                return;
            case WorldOpKind::RepairSeams: {
                int errors = 0;
                const std::vector<TileCoord> found = seamTiles(*runtime, op.tiles, errors);
                if (found.empty()) break;
                // the repair writes the tile with the higher (x, z) of each pair: the neighbours are kept too
                std::set<TileCoord> keep;
                for (const TileCoord& tile : found) {
                    for (int dz = -1; dz <= 1; dz++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const TileCoord near(tile.x + dx, tile.z + dz);
                            if (near.addressable() && runtime->present(near)) keep.insert(near);
                        }
                    }
                }
                auto change = [found](TerrainWorldRuntime& world, std::string&) {
                    for (const TileCoord& tile : found) world.store().seamErrors(tile, true);
                    return true;
                };
                auto* command = new TerrainWorldTilesCmd(project, sceneProject->id, entity, "Repair Seams",
                                                         std::vector<TileCoord>(keep.begin(), keep.end()), change);
                if (!addProperty(command)) {
                    rollback();
                    call->fail(where + "the seams could not be repaired. Nothing was changed.");
                    return;
                }
                for (const TileCoord& tile : found) repaired.push_back(tileJson(tile));
                break;
            }
            case WorldOpKind::RebuildOverview: {
                // a cache: no undo step
                runtime->rebuildOverview(true);
                std::string why;
                if (!TerrainWorldFiles::flushWorld(*runtime, &why) && !why.empty()) warnings.push_back("The overview cache was not saved: " + why);
                break;
            }
            case WorldOpKind::SetStreaming: {
                if (playing) {
                    // the running copy only: Play's changes go with it at Stop
                    TerrainWorldComponent& live = scene->getComponent<TerrainWorldComponent>(entity);
                    if (op.poolSize) live.poolSize = static_cast<int>(*op.poolSize);
                    if (op.lodDistance) live.lodDistance = static_cast<float>(*op.lodDistance);
                    if (op.uploadBudget) live.uploadBudget = static_cast<float>(*op.uploadBudget);
                } else {
                    const uint32_t sceneId = sceneProject->id;
                    const ComponentType type = ComponentType::TerrainWorldComponent;
                    if ((op.poolSize && !addProperty(new PropertyCmd<int>(project, sceneId, entity, type, "poolSize", static_cast<int>(*op.poolSize)))) ||
                        (op.lodDistance && !addProperty(new PropertyCmd<float>(project, sceneId, entity, type, "lodDistance", static_cast<float>(*op.lodDistance)))) ||
                        (op.uploadBudget && !addProperty(new PropertyCmd<float>(project, sceneId, entity, type, "uploadBudget", static_cast<float>(*op.uploadBudget))))) {
                        rollback();
                        call->fail(where + "the streaming settings could not be set. Nothing was changed.");
                        return;
                    }
                }
                const TerrainWorldComponent& now = scene->getComponent<TerrainWorldComponent>(entity);
                runtime->applySettings(now.poolSize, now.lodDistance, now.uploadBudget, terrainWorldPlatformBudget().nearTiles);
                if (op.freeze) runtime->setFrozen(*op.freeze);
                break;
            }
            case WorldOpKind::SetFocus: {
                const Vector3 origin = worldOrigin(scene, entity);
                const float x = static_cast<float>(op.x) - origin.x, z = static_cast<float>(op.z) - origin.z;
                const float vx = static_cast<float>(op.velocityX), vz = static_cast<float>(op.velocityZ);
                int id = 0;
                if (op.focusId && runtime->setFocus(*op.focusId, x, z, vx, vz)) {
                    id = *op.focusId;
                    if (op.radiusScale) warnings.push_back(where + "radius_scale applies to a new focus; focus " + std::to_string(id) + " kept its own.");
                } else {
                    id = runtime->addFocus(x, z, op.radiusScale ? static_cast<float>(*op.radiusScale) : 1.0f, vx, vz);
                    if (op.focusId) warnings.push_back(where + "there was no focus " + std::to_string(*op.focusId) + ": it made focus " + std::to_string(id) + ".");
                }
                if (id == 0) {
                    rollback();
                    call->fail(where + "the world has " + std::to_string(terraincore::WORLD_MAX_FOCUSES) + " focuses already: remove_focus frees one.");
                    return;
                }
                focusIds.push_back(id);
                break;
            }
            case WorldOpKind::RemoveFocus:
                if (!runtime->removeFocus(*op.focusId)) warnings.push_back(where + "there was no focus " + std::to_string(*op.focusId) + "; nothing was removed.");
                break;
            case WorldOpKind::SetCollision: {
                const uint32_t sceneId = sceneProject->id;
                const ComponentType type = ComponentType::TerrainWorldComponent;
                if ((op.collisionRadius && !addProperty(new PropertyCmd<float>(project, sceneId, entity, type, "collisionRadius", static_cast<float>(*op.collisionRadius)))) ||
                    (op.chunkQuads && !addProperty(new PropertyCmd<int>(project, sceneId, entity, type, "collisionChunkQuads", static_cast<int>(*op.chunkQuads))))) {
                    rollback();
                    call->fail(where + "the collision settings could not be set. Nothing was changed.");
                    return;
                }
                break;
            }
        }
    }
    std::string undoName;
    if (history->position() > start) {
        undoName = (fromTerrain ? "Convert to Terrain World (" : creating ? "Create Terrain World (" : "Edit Terrain World (") + name + ")";
        history->groupSince(start, undoName);
    }

    Json out = base(runtime->occupancy().tileCount());
    out["changed_tiles"] = {{"added", added}, {"removed", removed}, {"imported", imported}, {"repaired", repaired}};
    if (!focusIds.empty()) out["focus_ids"] = focusIds;
    out["frozen"] = runtime->frozen();
    out["warnings"] = warnings;
    out["dry_run"] = false;
    if (!undoName.empty()) out["undo_name"] = undoName;
    const std::string next = "Sculpt and paint across tiles with edit_terrain or sculpt_terrain on this entity; sample_terrain {world} reports streaming.";
    std::string text = (fromTerrain ? "Converted into Terrain World \"" : creating ? "Created Terrain World \"" : "Edited Terrain World \"") + name + "\": " +
                       std::to_string(runtime->occupancy().tileCount()) + " tile(s), " + std::to_string(added.size()) + " added, " +
                       std::to_string(removed.size()) + " removed, " + std::to_string(repaired.size()) + " repaired.";
    if (runtime->isReady() || !wait) {
        out["ready"] = runtime->isReady();
        out["next"] = runtime->isReady() ? next : "The world is still assembling its overview: sample_terrain {world} shows ready.";
        call->ok(text, out);
        return;
    }
    // R43: a new world's overview loads as a job; the call waits for it unless wait is false
    JobPtr job = call->ctx().jobs().create("terrain_world_ready", "Load Terrain World \"" + name + "\"");
    job->setProgress(-1.0, "Assembling the overview");
    job->setCancelHandler([]() {});
    McpContext* context = &call->ctx();
    const uint32_t sceneId = running->id;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(static_cast<long long>(timeout * 1000.0));
    pollJob(job, [job, context, sceneId, entity, deadline]() {
        TerrainWorldRuntime* live = liveRuntime(context, sceneId, entity);
        if (job->cancelRequested()) {
            job->markCancelled();
            return true;
        }
        if (!live) {
            job->fail("The Terrain World stopped before it was ready (its scene closed or its data changed).");
            return true;
        }
        if (live->isReady()) {
            job->succeed("The Terrain World is ready.");
            return true;
        }
        if (Clock::now() < deadline) return false;
        job->fail("The Terrain World was not ready before timeout_seconds.");
        return true;
    });
    replyWhenJobDone(call, job, true, timeout + 10.0, [out, text, next](const McpJob& finished) {
        Json result = out;
        result["ready"] = finished.status() == JobStatus::Succeeded;
        result["next"] = finished.status() == JobStatus::Succeeded ? next : "The world is still assembling its overview: sample_terrain {world} shows ready.";
        // the edit is made either way: only the wait ended early
        return ToolResult::success(text + (finished.status() == JobStatus::Succeeded ? "" : " " + finished.message()), result);
    });
}

} // namespace terrain_world_tools

void addTerrainWorldPointState(Scene* scene, Entity entity, float localX, float localZ, Json& sample) {
    using namespace terrain_world_tools;
    if (!scene->findComponent<TerrainWorldComponent>(entity)) return;
    std::string error;
    TerrainWorldRuntime* runtime = terrainWorldRuntimeOf(scene, entity, error);
    sample["state"] = runtime ? terrainWorldPointStateName(runtime->pointState(localX, localZ)) : "absent";
}

void sampleTerrainWorld(const CallPtr& call, Project* project, SceneProject* sceneProject, Entity entity, Json data) {
    using namespace terrain_world_tools;
    const Json& args = call->args();
    const Json& world = args["world"];
    std::string error;
    if (!world.is_object() || !checkKeys(world, {"tiles", "wait_for"}, "world", error)) {
        call->fail(error.empty() ? "world must be {tiles?: [[x, z], ...], wait_for?: {x, z, radius, level}}." : error);
        return;
    }
    SceneProject* running = McpRefs::runtimeScene(project, sceneProject);
    Scene* scene = running->scene;
    TerrainWorldRuntime* runtime = terrainWorldRuntimeOf(scene, entity, error);
    if (!runtime) {
        call->fail("world: " + error);
        return;
    }
    std::vector<TileCoord> tiles;
    if (given(world, "tiles") && !parseTiles(world["tiles"], "world.tiles", tiles, error)) {
        call->fail(error);
        return;
    }
    data["world"] = worldStatus(*runtime, tiles);
    const std::string name = scene->getEntityName(entity);
    if (!given(world, "wait_for")) {
        call->ok("Sampled Terrain World \"" + name + "\": " + std::to_string(runtime->occupancy().tileCount()) + " tile(s).", data);
        return;
    }

    const Json& area = world["wait_for"];
    if (!area.is_object() || !checkKeys(area, {"x", "z", "radius", "level"}, "world.wait_for", error) || !given(area, "x") || !given(area, "z") ||
        !area["x"].is_number() || !area["z"].is_number()) {
        call->fail(error.empty() ? "world.wait_for must be {x, z, radius, level}: x and z are world coordinates." : error);
        return;
    }
    std::optional<double> radiusValue;
    if (!readNumber(area, "radius", 0, terraincore::WORLD_EXTENT_LIMIT * 2.0, "world.wait_for", radiusValue, error)) {
        call->fail(error);
        return;
    }
    TerrainWorldReadyLevel level = TerrainWorldReadyLevel::Render;
    if (given(area, "level")) {
        const std::string levelName = area["level"].is_string() ? area["level"].get<std::string>() : "";
        if (levelName == "collision") level = TerrainWorldReadyLevel::Collision;
        else if (levelName != "render") {
            call->fail("world.wait_for.level must be render or collision.");
            return;
        }
    }
    const double timeout = given(args, "timeout_seconds") && args["timeout_seconds"].is_number()
                               ? std::clamp(args["timeout_seconds"].get<double>(), 0.0, 3600.0)
                               : DEFAULT_TIMEOUT_SECONDS;
    const float worldX = area["x"].get<float>(), worldZ = area["z"].get<float>();
    const float radius = radiusValue ? static_cast<float>(*radiusValue) : runtime->lattice().tileSize() * 0.5f;
    const Vector3 origin = worldOrigin(scene, entity);
    const float x = worldX - origin.x, z = worldZ - origin.z;
    if (runtime->isAreaReady(x, z, radius, level)) {
        data["wait_for"] = {{"ready", true}, {"seconds", 0.0}};
        data["next"] = FREEZE_NEXT;
        call->ok("The area of Terrain World \"" + name + "\" is ready.", data);
        return;
    }

    // R18, R45: the area is a focus until it is ready or the wait ends
    JobPtr job = call->ctx().jobs().create("terrain_world_wait", "Wait for Terrain World \"" + name + "\" round (" + std::to_string(worldX) + ", " +
                                                                    std::to_string(worldZ) + ")");
    job->setProgress(-1.0, "Streaming");
    job->setCancelHandler([]() {});
    const int waitId = runtime->waitForArea(x, z, radius, level);
    McpContext* context = &call->ctx();
    const uint32_t sceneId = running->id;
    const Clock::time_point started = Clock::now();
    const Clock::time_point deadline = started + std::chrono::milliseconds(static_cast<long long>(timeout * 1000.0));
    pollJob(job, [job, context, sceneId, entity, waitId, x, z, radius, level, started, deadline, tiles, timeout]() {
        TerrainWorldRuntime* live = liveRuntime(context, sceneId, entity);
        if (!live) {
            job->fail("The Terrain World stopped before the area was ready (its scene closed, Play started or stopped, or its data changed): "
                      "call sample_terrain again.");
            return true;
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        if (job->cancelRequested()) {
            live->dropAreaWait(waitId);
            job->markCancelled();
            return true;
        }
        if (live->isAreaReady(x, z, radius, level)) {
            live->dropAreaWait(waitId);
            job->succeed("The area is ready.", Json{{"world", worldStatus(*live, tiles)}, {"seconds", seconds}});
            return true;
        }
        if (Clock::now() < deadline) return false;
        const std::vector<TileCoord> missing = live->missingTiles(x, z, radius, level);
        live->dropAreaWait(waitId);
        Json list = Json::array();
        for (const TileCoord& tile : missing) list.push_back(tileJson(tile));
        const TerrainWorldStats stats = live->stats();
        std::string message = "The area was not ready after " + std::to_string(static_cast<int>(timeout)) + " s; tiles not ready: " +
                              (missing.empty() ? std::string("none (the overview is still loading)") : tilesText(missing)) + ". Raise timeout_seconds";
        if (stats.poolExhausted > 0) message += ", or raise pool_size with edit_terrain_world set_streaming (the pool ran out)";
        if (stats.frozen) message += ", or unfreeze streaming with edit_terrain_world set_streaming {freeze: false} (it is frozen)";
        job->fail(message + ".", Json{{"world", worldStatus(*live, tiles)}, {"missing_tiles", list}, {"seconds", seconds}});
        return true;
    });
    replyWhenJobDone(call, job, true, timeout + 10.0, [data, name](const McpJob& finished) {
        Json out = data;
        const Json result = finished.result();
        if (result.contains("world")) out["world"] = result["world"];
        if (finished.status() == JobStatus::Succeeded) {
            out["wait_for"] = {{"ready", true}, {"seconds", result.value("seconds", 0.0)}};
            out["next"] = FREEZE_NEXT;
            return ToolResult::success("The area of Terrain World \"" + name + "\" is ready.", out);
        }
        out["wait_for"] = {{"ready", false}, {"missing_tiles", result.contains("missing_tiles") ? result["missing_tiles"] : Json::array()}};
        return ToolResult::error(finished.message(), out);
    });
}

void registerTerrainWorldTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "edit_terrain_world";
    tool.title = "Edit Terrain World";
    tool.group = "terrain";
    tool.attributesUndo = true;
    // its streaming ops run in Play; the handler refuses the others by name
    tool.allowedWhilePlaying = true;
    tool.description =
        "Create a Terrain World (one streamed terrain of many tiles: for land past about 2 km, never many Terrain GameObjects) and run ops "
        "on it, all checked before any runs, one undo step. create makes the asset and GameObject; create.from_terrain converts a Terrain, "
        "which stays, inactive. Ops: add_tiles, remove_tiles, "
        "import_heightmap, repair_seams, rebuild_overview, set_streaming (freeze holds tiles still for screenshots), set_focus, remove_focus, set_collision. "
        "In Play only set_streaming, set_focus and remove_focus run. Heights and paint: edit_terrain with entity. Status: sample_terrain "
        "world. Fields: jasper://docs/terrain.";
    tool.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef("Default: the only Terrain World.")},
        {"create", anyObject("{name, asset_path, tile_quads, spacing, max_height, base_height, grid: {width, length, origin} | from_terrain}")},
        {"ops", Json{{"type", "array"}, {"maxItems", 64}, {"items", anyObject("{op, ...}")}}},
        {"dry_run", withDefault(boolean(""), false)},
        {"wait", withDefault(boolean("Wait until the world draws."), true)},
        {"timeout_seconds", number("", 0, 3600)}
    });
    tool.handler = terrain_world_tools::editTerrainWorldTool;
    registry.addTool(std::move(tool));
}

}
