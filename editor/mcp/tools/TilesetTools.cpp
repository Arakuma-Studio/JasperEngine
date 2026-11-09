// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The tilemaps group (docs/specs/S50-grid-tilemap.md R77-R79, R81; agent guide
// docs/mcp/guide/tilemaps.md): edit_tileset creates or edits a .tileset (preview by default, a
// report and a contact sheet; output writes one undo step; check returns only the report),
// import_tilemap previews or imports a Tiled or LDtk map (editor/tilemap/import).

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/W4Shared.h"

#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "tilemap/TerrainTemplates.h"
#include "tilemap/TilesetEdit.h"
#include "tilemap/TilesetPool.h"
#include "tilemap/import/TilemapImportApply.h"
#include "util/Base64.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::mcp {

// file-local, in a namespace of its own: a unity build merges the MCP tool sources
namespace tilesettools {
namespace {

namespace fs = std::filesystem;
namespace tm = jasperengine::editor::tilemap;
namespace ti = jasperengine::editor::tilemapimport;

using Problem = jasperengine::editor::pixelart::PixelProblem;

constexpr const char* kTilesetDoc = "jasper://docs/tilemaps";
// a map with this many tile cells imports as a job (about 1 s of apply)
constexpr int64_t kImportJobCells = 200000;
constexpr double kImportJobWaitSeconds = 600.0;
constexpr size_t kMissingMasksListed = 32;

Problem tilesetProblem(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
    Problem p;
    p.kind = kind;
    p.field = field;
    p.message = message;
    p.fix = fix;
    return p;
}

Problem tilesetProblemOf(const TilesetProblem& p) {
    return tilesetProblem(p.kind, p.field, p.message, p.fix);
}

Json tilesetProblemJson(const Problem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix.empty() ? std::string("see ") + kTilesetDoc : p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    return j;
}

std::string problemsLines(const std::string& headline, const std::vector<Problem>& problems) {
    std::string text = headline;
    for (const Problem& p : problems) text += "\n- " + p.kind + ": " + p.message + (p.fix.empty() ? "" : " Fix: " + p.fix);
    return text;
}

Json problemsJsonOf(const std::vector<Problem>& problems) {
    Json list = Json::array();
    for (const Problem& p : problems) list.push_back(tilesetProblemJson(p));
    return list;
}

void failProblems(const CallPtr& call, const std::string& headline, const std::vector<Problem>& problems) {
    call->fail(problemsLines(headline, problems), Json{{"errors", problemsJsonOf(problems)}});
}

ToolImage pngImageOf(const jasperengine::editor::pixelart::PixelImage& image) {
    const std::string png = image.encodePng();
    return ToolImage{Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"};
}

bool argIn(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

bool boolIn(const Json& args, const char* key, bool fallback = false) {
    return argIn(args, key) && args[key].is_boolean() ? args[key].get<bool>() : fallback;
}

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<int> tileIdOf(const TilesetData& data, const Json& value) {
    if (value.is_number_integer()) {
        const int id = value.get<int>();
        return data.findTile(id) ? std::optional<int>(id) : std::nullopt;
    }
    if (value.is_string()) {
        const int32_t id = data.findTileByName(value.get<std::string>());
        return id != NO_TILE ? std::optional<int>(id) : std::nullopt;
    }
    return std::nullopt;
}

Problem unknownTileProblem(const std::string& field, const Json& value) {
    return tilesetProblem("unknown_tile", field, "the tileset has no tile " + value.dump(), "use a tile id or name of the tileset (see the contact sheet)");
}

// ---- property values

std::vector<std::string> declaredProperties(const TilesetData& data) {
    std::vector<std::string> names;
    for (const PropertySchemaEntry& entry : data.propertySchema) names.push_back(entry.name);
    return names;
}

std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) text += (text.empty() ? "" : ", ") + name;
    return text;
}

bool parsePropertyValue(const Json& value, TilePropertyType type, TilePropertyValue& out, Problem& error, const std::string& field) {
    out.type = type;
    const std::string wanted = tileset::propertyTypeName(type);
    switch (type) {
        case TilePropertyType::Bool:
            if (value.is_boolean()) {
                out.b = value.get<bool>();
                return true;
            }
            break;
        case TilePropertyType::Int:
            if (value.is_number_integer()) {
                out.i = value.get<int64_t>();
                return true;
            }
            break;
        case TilePropertyType::Float:
            if (value.is_number()) {
                out.f = value.get<double>();
                return true;
            }
            break;
        case TilePropertyType::String:
            if (value.is_string()) {
                out.s = value.get<std::string>();
                return true;
            }
            break;
        case TilePropertyType::Color: {
            Vector4 color;
            if (value.is_array() && value.size() >= 3 && McpValues::parseVector4(value.size() == 3 ? Json::array({value[0], value[1], value[2], 1.0}) : value, color)) {
                out.color = color;
                return true;
            }
            if (value.is_string() && value.get<std::string>().rfind('#', 0) == 0 && (value.get<std::string>().size() == 7 || value.get<std::string>().size() == 9)) {
                const std::string text = value.get<std::string>();
                const auto byte = [&](size_t at) { return static_cast<float>(std::stoi(text.substr(at, 2), nullptr, 16)) / 255.0f; };
                out.color = Vector4(byte(1), byte(3), byte(5), text.size() == 9 ? byte(7) : 1.0f);
                return true;
            }
            break;
        }
    }
    error = tilesetProblem("unknown_property", field, "the value " + value.dump() + " is not a " + wanted, "pass a " + wanted);
    return false;
}

// ---- the edits

bool applySources(Project* project, TilesetData& data, const Json& sources, std::vector<std::string>& warnings, Problem& error) {
    if (!sources.is_array()) {
        error = tilesetProblem("invalid_value", "sources", "sources must be an array of {texture|sheet|file, ...}", "see " + std::string(kTilesetDoc));
        return false;
    }
    for (size_t i = 0; i < sources.size(); i++) {
        const Json& source = sources[i];
        const std::string where = "sources[" + std::to_string(i) + "]";
        if (!source.is_object()) {
            error = tilesetProblem("invalid_value", where, "a source is an object", "e.g. {\"texture\": \"assets/tiles.png\", \"tile_size\": [16, 16]}");
            return false;
        }
        std::string stored, pathError;
        const auto assetArg = [&](const char* key) {
            return source[key].is_string() && McpValues::assetPathFromJson(project, source[key].get<std::string>(), stored, pathError);
        };
        int sourceId = 0;
        if (argIn(source, "texture")) {
            if (!assetArg("texture")) {
                error = tilesetProblem("tileset_missing", where + ".texture", "texture: " + pathError, "pass a project-relative image path");
                return false;
            }
            tm::TilesetPagePixels pages;
            const auto* image = pages.get(stored);
            if (!image) {
                error = tilesetProblem("tileset_missing", where + ".texture", "can't read the texture: " + pages.lastError(), "pass a readable image of the project");
                return false;
            }
            tm::TextureSlicing slicing;
            slicing.tileWidth = data.tileWidth;
            slicing.tileHeight = data.tileHeight;
            if (argIn(source, "tile_size")) {
                Vector2 size;
                if (!McpValues::parseVector2(source["tile_size"], size) || size.x < 1.0f || size.y < 1.0f) {
                    error = tilesetProblem("invalid_value", where + ".tile_size", "tile_size must be [w, h]", "pass two positive pixel sizes");
                    return false;
                }
                slicing.tileWidth = static_cast<int>(size.x);
                slicing.tileHeight = static_cast<int>(size.y);
            }
            slicing.margin = source.value("margin", 0);
            slicing.spacing = source.value("spacing", 0);
            slicing.skipEmpty = boolIn(source, "skip_empty", true);
            bool existing = false;
            for (const TilesetSource& known : data.sources) {
                if (known.kind == TilesetSourceKind::Texture && known.asset.path == stored) {
                    tm::ResliceReport report;
                    if (!tm::resliceTexture(data, known.id, slicing, *image, report, error)) return false;
                    existing = true;
                    break;
                }
            }
            if (!existing) {
                if (data.sources.empty() && data.tiles.empty()) {
                    TilesetData created;
                    if (!tm::createFromTexture(data.name, AssetRef(stored), slicing, *image, created, error)) return false;
                    created.propertySchema = data.propertySchema;
                    created.terrainSets = data.terrainSets;
                    created.stamps = data.stamps;
                    data = std::move(created);
                } else if (!tm::addTextureSource(data, AssetRef(stored), slicing, *image, sourceId, error)) {
                    return false;
                }
            }
        } else if (argIn(source, "sheet")) {
            if (!assetArg("sheet")) {
                error = tilesetProblem("tileset_missing", where + ".sheet", "sheet: " + pathError, "pass a project-relative sheet path");
                return false;
            }
            std::vector<std::string> frames;
            if (argIn(source, "frames")) frames = source["frames"].get<std::vector<std::string>>();
            if (!tm::addSheetSource(data, AssetRef(stored), frames, sourceId, error)) return false;
        } else if (argIn(source, "file")) {
            if (!assetArg("file")) {
                error = tilesetProblem("tileset_missing", where + ".file", "file: " + pathError, "pass a project-relative .aseprite path");
                return false;
            }
            fs::path absolute = project->resolveAssetPath(stored);
            if (!tm::addAsepriteSourceFromFile(data, AssetRef(stored), absolute, source.value("tileset", 0), warnings, sourceId, error)) return false;
        } else {
            error = tilesetProblem("invalid_value", where, "a source names texture, sheet or file", "add one of them");
            return false;
        }
    }
    return true;
}

bool applyPropertySchema(TilesetData& data, const Json& schema, Problem& error) {
    if (!schema.is_array()) {
        error = tilesetProblem("invalid_value", "property_schema", "property_schema must be an array of {name, type, default}", "see " + std::string(kTilesetDoc));
        return false;
    }
    for (size_t i = 0; i < schema.size(); i++) {
        const Json& item = schema[i];
        const std::string where = "property_schema[" + std::to_string(i) + "]";
        PropertySchemaEntry entry;
        entry.name = item.is_object() ? item.value("name", std::string()) : std::string();
        if (entry.name.empty() || !tileset::parsePropertyType(item.value("type", std::string()), entry.type)) {
            error = tilesetProblem("unknown_property", where, "a schema entry needs name and type (bool, int, float, string, color)", "add name and type");
            return false;
        }
        entry.defaultValue.type = entry.type;
        if (argIn(item, "default") && !parsePropertyValue(item["default"], entry.type, entry.defaultValue, error, where + ".default")) return false;
        bool replaced = false;
        for (PropertySchemaEntry& known : data.propertySchema) {
            if (known.name == entry.name) {
                known = entry;
                replaced = true;
            }
        }
        if (!replaced) data.propertySchema.push_back(std::move(entry));
    }
    return true;
}

bool resolveTerrainSet(const TilesetData& data, const Json& value, int& out) {
    if (value.is_number_integer()) {
        out = value.get<int>();
        return out >= 0 && out < static_cast<int>(data.terrainSets.size());
    }
    if (value.is_string()) {
        for (size_t i = 0; i < data.terrainSets.size(); i++) {
            if (data.terrainSets[i].name == value.get<std::string>()) {
                out = static_cast<int>(i);
                return true;
            }
        }
    }
    return false;
}

// a peering value: "any", "none", or a terrain of the set, by name or index
bool peeringValue(const TerrainSetDef& set, const Json& value, int16_t& out) {
    if (value.is_string()) {
        const std::string text = lowerText(value.get<std::string>());
        if (text == "any") {
            out = PEERING_ANY;
            return true;
        }
        if (text == "none") {
            out = PEERING_NONE;
            return true;
        }
        for (size_t i = 0; i < set.terrains.size(); i++) {
            if (set.terrains[i].name == value.get<std::string>()) {
                out = static_cast<int16_t>(i);
                return true;
            }
        }
        return false;
    }
    if (value.is_number_integer() && value.get<int>() >= 0 && value.get<int>() < static_cast<int>(set.terrains.size())) {
        out = static_cast<int16_t>(value.get<int>());
        return true;
    }
    return false;
}

bool applyTileTerrain(const TilesetData& data, TileDef& tile, const Json& terrain, const std::string& where, Problem& error) {
    int set = -1;
    if (!argIn(terrain, "set") || !resolveTerrainSet(data, terrain["set"], set)) {
        error = tilesetProblem("invalid_peering", where + ".set", "terrain.set names no terrain set of this tileset", "add the set in terrain_sets first");
        return false;
    }
    const TerrainSetDef& setDef = data.terrainSets[static_cast<size_t>(set)];
    TileTerrain value;
    value.set = set;
    if (argIn(terrain, "center")) {
        int16_t center = PEERING_ANY;
        if (!peeringValue(setDef, terrain["center"], center) || center < 0) {
            error = tilesetProblem("invalid_peering", where + ".center", "center must be a terrain of the set", "use a terrain name of the set");
            return false;
        }
        value.center = center;
    }
    if (argIn(terrain, "peering")) {
        if (!terrain["peering"].is_object()) {
            error = tilesetProblem("invalid_peering", where + ".peering", "peering is an object {direction: value}", "see " + std::string(kTilesetDoc));
            return false;
        }
        const std::vector<int> slots = tileset::peeringSlots(data.shape, setDef.mode);
        std::string keys;
        for (int slot : slots) keys += (keys.empty() ? "" : " ") + std::string(tileset::peeringKey(data.shape, slot));
        for (auto it = terrain["peering"].begin(); it != terrain["peering"].end(); ++it) {
            const int slot = tileset::peeringSlotOfKey(data.shape, it.key());
            if (slot < 0 || std::find(slots.begin(), slots.end(), slot) == slots.end()) {
                error = tilesetProblem("invalid_peering", where + ".peering." + it.key(), "\"" + it.key() + "\" is not a peering direction of this set's mode",
                                       "use the directions: " + keys);
                return false;
            }
            int16_t peering = PEERING_ANY;
            if (!peeringValue(setDef, it.value(), peering)) {
                std::string names;
                for (const TerrainDef& terrainDef : setDef.terrains) names += (names.empty() ? "" : ", ") + terrainDef.name;
                error = tilesetProblem("invalid_peering", where + ".peering." + it.key(), "the value " + it.value().dump() + " is not any, none or a terrain of the set",
                                       "use any, none" + (names.empty() ? std::string() : " or: " + names));
                return false;
            }
            value.peering[static_cast<size_t>(slot)] = peering;
        }
    }
    tile.terrain = value;
    return true;
}

bool applyTileEdit(TilesetData& data, const Json& edit, const std::string& where, std::vector<int32_t>& selected, Problem& error) {
    selected.clear();
    if (argIn(edit, "range")) {
        const Json& range = edit["range"];
        if (!range.is_array() || range.size() != 2 || !range[0].is_number_integer() || !range[1].is_number_integer()) {
            error = tilesetProblem("invalid_value", where + ".range", "range is [first id, last id] inclusive", "pass two tile ids");
            return false;
        }
        for (const TileDef& tile : data.tiles) {
            if (tile.id >= range[0].get<int>() && tile.id <= range[1].get<int>()) selected.push_back(tile.id);
        }
    } else {
        const Json selector = argIn(edit, "id") ? edit["id"] : argIn(edit, "tile") ? edit["tile"] : Json();
        const std::optional<int> id = tileIdOf(data, selector);
        if (!id) {
            error = unknownTileProblem(where, selector.is_null() ? Json("(no id, tile or range)") : selector);
            return false;
        }
        selected.push_back(*id);
    }
    for (int32_t id : selected) {
        TileDef& tile = *data.findTile(id);
        if (argIn(edit, "name")) tile.name = edit["name"].get<std::string>();
        if (argIn(edit, "probability")) tile.probability = edit["probability"].get<float>();
        if (argIn(edit, "offset")) {
            Vector2 offset;
            if (!McpValues::parseVector2(edit["offset"], offset)) {
                error = tilesetProblem("invalid_value", where + ".offset", "offset must be [x, y]", "pass two numbers");
                return false;
            }
            tile.offsetX = static_cast<int>(offset.x);
            tile.offsetY = static_cast<int>(offset.y);
        }
        if (argIn(edit, "random_start")) tile.randomStart = edit["random_start"].get<bool>();
        if (argIn(edit, "nav_area")) tile.navArea = edit["nav_area"].get<int>();
        if (argIn(edit, "properties")) {
            if (!edit["properties"].is_object()) {
                error = tilesetProblem("unknown_property", where + ".properties", "properties is an object", "pass {name: value}");
                return false;
            }
            for (auto it = edit["properties"].begin(); it != edit["properties"].end(); ++it) {
                const PropertySchemaEntry* entry = data.findProperty(it.key());
                if (!entry) {
                    error = tilesetProblem("unknown_property", where + ".properties." + it.key(), "\"" + it.key() + "\" is not declared in the property schema",
                                           data.propertySchema.empty() ? "declare it first in property_schema" : "declared: " + joined(declaredProperties(data)) + "; add it to property_schema");
                    return false;
                }
                TilePropertyValue value;
                if (!parsePropertyValue(it.value(), entry->type, value, error, where + ".properties." + it.key())) return false;
                tile.properties[it.key()] = value;
            }
        }
        if (argIn(edit, "animation")) {
            if (!edit["animation"].is_array()) {
                error = tilesetProblem("invalid_value", where + ".animation", "animation is an array of {tile, ms}", "see " + std::string(kTilesetDoc));
                return false;
            }
            tile.animation.clear();
            for (const Json& frame : edit["animation"]) {
                TileAnimationFrame value;
                const std::optional<int> frameId = tileIdOf(data, frame.value("tile", Json()));
                if (!frameId) {
                    error = unknownTileProblem(where + ".animation", frame.value("tile", Json("?")));
                    return false;
                }
                value.tile = *frameId;
                value.ms = frame.value("ms", 100);
                tile.animation.push_back(value);
            }
        }
        if (argIn(edit, "collision")) {
            const Json& collision = edit["collision"];
            const std::string mode = lowerText(collision.value("mode", std::string("full")));
            if (mode == "none") tile.collision.mode = TileCollisionMode::None;
            else if (mode == "full") tile.collision.mode = TileCollisionMode::Full;
            else if (mode == "alpha") tile.collision.mode = TileCollisionMode::Alpha;
            else if (mode == "custom" || mode == "polygons") tile.collision.mode = TileCollisionMode::Custom;
            else {
                error = tilesetProblem("invalid_value", where + ".collision.mode", "mode is none, full, alpha or custom", "use one of them");
                return false;
            }
            tile.collision.threshold = collision.value("threshold", tile.collision.threshold);
            const std::string kind = lowerText(collision.value("kind", std::string(tileset::collisionKindName(tile.collision.kind))));
            tile.collision.kind = kind == "one_way" ? TileCollisionKind::OneWay : kind == "sensor" ? TileCollisionKind::Sensor : TileCollisionKind::Solid;
            if (argIn(collision, "polygons")) {
                tile.collision.polygons.clear();
                for (const Json& polygon : collision["polygons"]) {
                    TilePolygon value;
                    for (const Json& point : polygon) value.points.push_back({point[0].get<int32_t>(), point[1].get<int32_t>()});
                    tile.collision.polygons.push_back(std::move(value));
                }
            }
        }
        if (argIn(edit, "terrain") && !applyTileTerrain(data, tile, edit["terrain"], where + ".terrain", error)) return false;
    }
    return true;
}

bool applyTerrainSets(TilesetData& data, const Json& sets, Problem& error) {
    if (!sets.is_array()) {
        error = tilesetProblem("invalid_value", "terrain_sets", "terrain_sets must be an array of {name, mode, terrains, template?, origin?}", "see " + std::string(kTilesetDoc));
        return false;
    }
    for (size_t i = 0; i < sets.size(); i++) {
        const Json& item = sets[i];
        const std::string where = "terrain_sets[" + std::to_string(i) + "]";
        const std::string name = item.value("name", std::string());
        if (name.empty()) {
            error = tilesetProblem("invalid_value", where + ".name", "a terrain set needs name", "add name");
            return false;
        }
        int setIndex = -1;
        for (size_t s = 0; s < data.terrainSets.size(); s++) {
            if (data.terrainSets[s].name == name) setIndex = static_cast<int>(s);
        }
        if (setIndex < 0) {
            TerrainSetDef created;
            created.name = name;
            data.terrainSets.push_back(std::move(created));
            setIndex = static_cast<int>(data.terrainSets.size()) - 1;
        }
        TerrainSetDef& set = data.terrainSets[static_cast<size_t>(setIndex)];
        if (argIn(item, "mode") && !tileset::parseTerrainMode(item["mode"].get<std::string>(), set.mode)) {
            error = tilesetProblem("invalid_peering", where + ".mode", "mode is blob, sides or dual_grid", "use one of them");
            return false;
        }
        if (argIn(item, "two_surface")) set.twoSurface = item["two_surface"].get<bool>();
        if (argIn(item, "terrains")) {
            set.terrains.clear();
            for (const Json& terrain : item["terrains"]) {
                TerrainDef def;
                def.name = terrain.is_string() ? terrain.get<std::string>() : terrain.value("name", std::string());
                if (terrain.is_object()) def.color = terrain.value("color", def.color);
                set.terrains.push_back(std::move(def));
            }
        }
        if (argIn(item, "template")) {
            TerrainTemplate value;
            if (!terrain::parseTemplate(item["template"].get<std::string>(), value)) {
                error = tilesetProblem("invalid_peering", where + ".template", "template is blob_47, blob_47_godot, sides_16 or dual_grid_4x4", "use one of them");
                return false;
            }
            const std::optional<int> origin = tileIdOf(data, item.value("origin", Json()));
            if (!origin) {
                error = unknownTileProblem(where + ".origin", item.value("origin", Json("?")));
                return false;
            }
            TerrainTemplateOptions options;
            options.terrain = item.value("terrain", 0);
            options.secondTerrain = item.value("second_terrain", -1);
            options.assignNames = item.value("assign_names", true);
            std::vector<TilesetProblem> templateProblems;
            if (!terrain::applyTemplate(data, setIndex, value, *origin, options, templateProblems)) {
                error = templateProblems.empty() ? tilesetProblem("invalid_peering", where + ".template", "the template does not fit", "check the set's mode and the tile block")
                                                  : tilesetProblemOf(templateProblems.front());
                if (error.field.empty()) error.field = where + ".template";
                return false;
            }
        }
    }
    return true;
}

bool applyStamps(TilesetData& data, const Json& stamps, Problem& error) {
    if (!stamps.is_array()) {
        error = tilesetProblem("invalid_value", "stamps", "stamps is an array of {name, size, cells}", "see " + std::string(kTilesetDoc));
        return false;
    }
    for (size_t i = 0; i < stamps.size(); i++) {
        const Json& item = stamps[i];
        const std::string where = "stamps[" + std::to_string(i) + "]";
        TileStamp stamp;
        stamp.name = item.value("name", std::string());
        if (stamp.name.empty() || !argIn(item, "cells") || !item["cells"].is_array()) {
            error = tilesetProblem("invalid_value", where, "a stamp needs name and cells [[x, y, tile, flips?], ...]", "add name and cells");
            return false;
        }
        int maxX = 0, maxY = 0;
        for (const Json& cell : item["cells"]) {
            if (!cell.is_array() || cell.size() < 3) {
                error = tilesetProblem("invalid_value", where + ".cells", "each cell is [x, y, tile] or [x, y, tile, flips]", "pass cell offsets");
                return false;
            }
            const std::optional<int> id = tileIdOf(data, cell[2]);
            if (!id) {
                error = unknownTileProblem(where + ".cells", cell[2]);
                return false;
            }
            TileStampCell value;
            value.x = cell[0].get<int32_t>();
            value.y = cell[1].get<int32_t>();
            value.tile = *id;
            if (cell.size() > 3 && cell[3].is_string()) {
                for (char c : cell[3].get<std::string>()) {
                    if (c == 'x') value.flags |= TILE_FLIP_X;
                    if (c == 'y') value.flags |= TILE_FLIP_Y;
                    if (c == 't') value.flags |= TILE_TRANSPOSE;
                }
            }
            maxX = std::max(maxX, value.x);
            maxY = std::max(maxY, value.y);
            stamp.cells.push_back(value);
        }
        if (argIn(item, "size")) {
            Vector2 size;
            if (McpValues::parseVector2(item["size"], size)) {
                stamp.width = static_cast<int>(size.x);
                stamp.height = static_cast<int>(size.y);
            }
        } else {
            stamp.width = maxX + 1;
            stamp.height = maxY + 1;
        }
        bool replaced = false;
        for (TileStamp& known : data.stamps) {
            if (known.name == stamp.name) {
                known = stamp;
                replaced = true;
            }
        }
        if (!replaced) data.stamps.push_back(std::move(stamp));
    }
    return true;
}

Json maskJson(const TilesetData& data, int set, const TerrainMissingMask& mask) {
    Json peering = Json::object();
    const TerrainSetDef& setDef = data.terrainSets[static_cast<size_t>(set)];
    for (int slot : tileset::peeringSlots(data.shape, setDef.mode)) {
        const int16_t value = mask.peering[static_cast<size_t>(slot)];
        peering[tileset::peeringKey(data.shape, slot)] =
            value == PEERING_ANY ? std::string("any") : value == PEERING_NONE ? std::string("none") : setDef.terrains[static_cast<size_t>(value)].name;
    }
    Json out = {{"peering", peering}};
    if (mask.center >= 0) out["center"] = setDef.terrains[static_cast<size_t>(mask.center)].name;
    return out;
}

// R77's report: per terrain set the masks no tile matches; FAIL on a validity problem, WARN on a
// missing mask, else PASS
Json coverageReport(const TilesetData& data, bool allowTransforms, std::vector<Problem>& problems, std::string& verdict) {
    std::vector<TilesetProblem> found;
    tileset::validate(data, found);
    for (const TilesetProblem& problem : found) problems.push_back(tilesetProblemOf(problem));
    Json sets = Json::array();
    size_t totalMissing = 0;
    for (size_t i = 0; i < data.terrainSets.size(); i++) {
        const TerrainCoverage coverage = terrain::coverage(data, static_cast<int>(i), allowTransforms);
        Json missing = Json::array();
        for (size_t m = 0; m < coverage.missing.size() && m < kMissingMasksListed; m++) missing.push_back(maskJson(data, static_cast<int>(i), coverage.missing[m]));
        sets.push_back({{"name", data.terrainSets[i].name},
                        {"mode", tileset::terrainModeName(data.terrainSets[i].mode)},
                        {"required", coverage.required},
                        {"missing_count", coverage.missing.size()},
                        {"missing", missing}});
        totalMissing += coverage.missing.size();
    }
    verdict = !problems.empty() ? "FAIL" : totalMissing > 0 ? "WARN" : "PASS";
    return {{"verdict", verdict},
            {"tiles", data.tiles.size()},
            {"sources", data.sources.size()},
            {"terrain_sets", sets},
            {"missing_masks", totalMissing},
            {"problems", problemsJsonOf(problems)}};
}

void editTilesetTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const bool check = boolIn(args, "check");
    const bool output = boolIn(args, "output");
    const std::string given = argIn(args, "path") && args["path"].is_string() ? args["path"].get<std::string>() : std::string();
    if (given.empty() || FileUtils::pathFromUtf8(given).extension().generic_string() != ".tileset") {
        failProblems(call, "Which tileset?", {tilesetProblem("invalid_value", "path", "path is a project-relative .tileset path", "e.g. assets/Tilesets/overworld.tileset")});
        return;
    }
    if (output && call->ctx().isPlaying()) {
        failProblems(call, "Nothing was written:", {tilesetProblem("play_mode", "output", "writes are refused while a scene plays (a preview works)", "stop play, then call again with output")});
        return;
    }
    fs::path absolute;
    std::string pathError;
    if (!McpRefs::resolveProjectWritePath(project, given, absolute, pathError)) {
        failProblems(call, "Which tileset?", {tilesetProblem("invalid_value", "path", pathError, "give a path inside the project")});
        return;
    }
    std::string stored;
    if (!McpValues::assetPathFromJson(project, given, stored, pathError)) {
        failProblems(call, "Which tileset?", {tilesetProblem("invalid_value", "path", pathError, "give a path inside the project's assets")});
        return;
    }

    TilesetData data;
    bool created = false;
    std::vector<Problem> problems;
    std::error_code ec;
    if (fs::exists(absolute, ec)) {
        std::ifstream file(absolute, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        std::vector<TilesetProblem> parseProblems;
        if (!tileset::parse(text.str(), data, parseProblems)) {
            std::vector<Problem> typed;
            for (const TilesetProblem& p : parseProblems) typed.push_back(tilesetProblemOf(p));
            failProblems(call, "Can't read " + given + ":", typed);
            return;
        }
    } else {
        created = true;
        data.name = FileUtils::pathToUtf8(absolute.stem());
    }

    Problem error;
    std::vector<std::string> warnings;
    if (argIn(args, "name") && args["name"].is_string()) data.name = args["name"].get<std::string>();
    if (argIn(args, "tile_size")) {
        Vector2 size;
        if (!McpValues::parseVector2(args["tile_size"], size) || size.x < 1.0f || size.y < 1.0f) {
            failProblems(call, "Can't edit the tileset:", {tilesetProblem("invalid_value", "tile_size", "tile_size must be [w, h]", "pass two positive pixel sizes")});
            return;
        }
        data.tileWidth = static_cast<int>(size.x);
        data.tileHeight = static_cast<int>(size.y);
    }
    if (argIn(args, "tile_shape") && !tileset::parseShape(args["tile_shape"].get<std::string>(), data.shape)) {
        failProblems(call, "Can't edit the tileset:", {tilesetProblem("invalid_value", "tile_shape", "tile_shape is square, isometric, hex_pointy or hex_flat", "use one of them")});
        return;
    }
    if (argIn(args, "sources") && !applySources(project, data, args["sources"], warnings, error)) {
        failProblems(call, "Can't edit the tileset:", {error});
        return;
    }
    if (created && data.sources.empty() && !check) {
        failProblems(call, "Can't create the tileset:", {tilesetProblem("tileset_missing", "sources", "a new tileset needs a source", "add sources: [{\"texture\": <path>, \"tile_size\": [16, 16]}]")});
        return;
    }
    if (argIn(args, "property_schema") && !applyPropertySchema(data, args["property_schema"], error)) {
        failProblems(call, "Can't edit the tileset:", {error});
        return;
    }
    if (argIn(args, "terrain_sets") && !applyTerrainSets(data, args["terrain_sets"], error)) {
        failProblems(call, "Can't edit the tileset:", {error});
        return;
    }
    if (argIn(args, "tiles")) {
        if (!args["tiles"].is_array()) {
            failProblems(call, "Can't edit the tileset:", {tilesetProblem("invalid_value", "tiles", "tiles is an array of edits {id|tile|range, ...}", "see " + std::string(kTilesetDoc))});
            return;
        }
        for (size_t i = 0; i < args["tiles"].size(); i++) {
            std::vector<int32_t> selected;
            if (!applyTileEdit(data, args["tiles"][i], "tiles[" + std::to_string(i) + "]", selected, error)) {
                failProblems(call, "Can't edit the tileset:", {error});
                return;
            }
        }
    }
    if (argIn(args, "stamps") && !applyStamps(data, args["stamps"], error)) {
        failProblems(call, "Can't edit the tileset:", {error});
        return;
    }

    // alpha collision outlines are traced here so the runtime never reads pixels (R15)
    auto shared = std::make_shared<TilesetData>(data);
    std::shared_ptr<const TilesetRuntime> runtime = TilesetPool::resolve(stored, shared);
    tm::TilesetPagePixels pages;
    bool hasAlpha = false;
    for (const TileDef& tile : data.tiles) hasAlpha = hasAlpha || tile.collision.mode == TileCollisionMode::Alpha;
    if (hasAlpha && runtime) {
        size_t filled = 0;
        if (!tm::fillAlphaCollisions(data, *runtime, pages, {}, filled, error)) {
            failProblems(call, "Can't trace the alpha collisions:", {error});
            return;
        }
        *shared = data;
        runtime = TilesetPool::resolve(stored, shared);
    }

    std::vector<Problem> reportProblems;
    std::string verdict;
    const Json report = coverageReport(data, boolIn(args, "allow_transforms"), reportProblems, verdict);
    if (check) {
        ToolResult reply = ToolResult::success(verdict + ": " + std::to_string(data.tiles.size()) + " tiles, " + std::to_string(report["missing_masks"].get<size_t>()) +
                                                   " missing masks, " + std::to_string(reportProblems.size()) + " problems.", report);
        call->reply(std::move(reply));
        return;
    }
    if (!reportProblems.empty()) {
        failProblems(call, "The tileset is not valid:", reportProblems);
        return;
    }

    Json result = {{"path", w4::projectRelative(project, absolute)},
                   {"created", created},
                   {"tiles", data.tiles.size()},
                   {"sources", data.sources.size()},
                   {"terrain_sets", data.terrainSets.size()},
                   {"stamps", data.stamps.size()},
                   {"warnings", warnings},
                   {"report", report}};
    ToolResult reply;
    if (output) {
        std::string writeError;
        if (!tm::writeTilesetFile(project, absolute, data, writeError, "Edit Tileset " + FileUtils::pathToUtf8(absolute.filename()))) {
            failProblems(call, "Nothing was written:", {tilesetProblem("tileset_missing", "path", writeError, "check the path is writable")});
            return;
        }
        TilesetPool::set(stored, shared);
        CommandHistory* history = project->getProjectCommandHistory();
        const UndoEntry* top = history ? history->top() : nullptr;
        if (top) result["undo"] = Json{{"id", top->info.id}, {"name", top->name()}};
        result["written"] = w4::projectRelative(project, absolute);
        reply.text = "Wrote " + w4::projectRelative(project, absolute) + ": " + std::to_string(data.tiles.size()) + " tiles (" + verdict + ", one undo step).";
    } else {
        result["next"] = "A preview: nothing was written. Pass output: true to write it.";
        reply.text = "Preview of " + w4::projectRelative(project, absolute) + ": " + std::to_string(data.tiles.size()) + " tiles (" + verdict + "). output: true writes it.";
    }
    if (runtime) {
        tm::ContactSheetOptions options;
        options.columns = argIn(args, "columns") && args["columns"].is_number() ? args["columns"].get<int>() : options.columns;
        options.scale = argIn(args, "scale") && args["scale"].is_number() ? args["scale"].get<int>() : options.scale;
        reply.images.push_back(pngImageOf(tm::contactSheet(*runtime, pages, options)));
        reply.text += " Image: the tiles and their ids.";
    }
    reply.structured = result;
    call->reply(std::move(reply));
}

// ---------------------------------------------------------------- import_tilemap

Problem importProblem(const ti::ImportProblem& p) {
    Problem out = tilesetProblem(p.kind, p.field, p.message + (p.line > 0 ? " (line " + std::to_string(p.line) + ")" : ""), p.fix);
    if (out.field.empty() && !p.file.empty()) out.field = p.file;
    return out;
}

Json reportJson(const ti::ImportReport& report) {
    Json grids = Json::array();
    for (const ti::ImportReport::Grid& grid : report.grids) grids.push_back({{"name", grid.name}, {"layers", grid.layers}});
    Json tilesets = Json::array();
    for (const ti::ImportReport::Tileset& tileset : report.tilesets) tilesets.push_back({{"name", tileset.name}, {"file", tileset.file}});
    Json skipped = Json::array();
    for (const ti::ImportSkipped& item : report.skipped) skipped.push_back({{"kind", item.kind}, {"name", item.name}, {"count", item.count}});
    const ti::ImportCounts& c = report.counts;
    return {{"grids", grids},
            {"tilesets", tilesets},
            {"skipped", skipped},
            {"warnings", report.warnings},
            {"counts", {{"grids", c.grids}, {"layers", c.layers}, {"tilesets", c.tilesets}, {"images", c.images}, {"tiles", c.tiles}, {"terrain_cells", c.terrainCells},
                         {"skipped", c.skipped}, {"warnings", c.warnings}}}};
}

// runs the apply step and writes its result; runs on the main thread
Json importApplyResult(const ti::ImportResult& result, SceneProject* sceneProject) {
    Json grids = Json::array();
    for (Entity grid : result.grids) grids.push_back(McpRefs::entityJson(sceneProject, grid));
    Json layers = Json::array();
    for (Entity layer : result.layers) layers.push_back(McpRefs::entityJson(sceneProject, layer));
    Json out = {{"grids", grids}, {"layers", layers}, {"tileset_files", result.tilesetFiles}, {"warnings", result.warnings}};
    if (!result.prefab.empty()) out["prefab"] = result.prefab;
    return out;
}

void importTilemapTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string fileArg = argIn(args, "file") && args["file"].is_string() ? args["file"].get<std::string>() : std::string();
    fs::path absolute;
    std::string pathError;
    if (fileArg.empty() || !McpRefs::resolveProjectPath(project, fileArg, absolute, pathError, true)) {
        failProblems(call, "Can't read the map:", {tilesetProblem("parse", "file", fileArg.empty() ? "file is required" : pathError, "pass a project-relative .tmj, .tmx or .ldtk path")});
        return;
    }

    std::vector<ti::ImportProblem> parseProblems;
    auto plan = std::make_shared<ti::ImportPlan>();
    if (!ti::loadPlan(project, absolute, *plan, parseProblems)) {
        std::vector<Problem> problems;
        for (const ti::ImportProblem& p : parseProblems) problems.push_back(importProblem(p));
        failProblems(call, "Can't parse " + fileArg + ":", problems);
        return;
    }

    std::string refError;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), refError);
    const bool toPrefab = argIn(args, "prefab");
    if (!sceneProject && !toPrefab) {
        failProblems(call, "Which scene?", {tilesetProblem("invalid_value", "scene", refError.empty() ? "no scene is open" : refError, "open a scene first, or pass prefab")});
        return;
    }
    ti::ImportTarget target;
    target.project = project;
    target.sceneId = sceneProject ? sceneProject->id : 0;
    target.mapFile = absolute;
    target.linked = boolIn(args, "linked");
    if (argIn(args, "parent")) {
        if (!sceneProject) {
            failProblems(call, "Which scene?", {tilesetProblem("invalid_value", "parent", "parent needs a scene", "pass scene")});
            return;
        }
        target.parent = McpRefs::resolveEntity(sceneProject, args["parent"], refError);
        if (target.parent == NULL_ENTITY) {
            failProblems(call, "Which parent?", {tilesetProblem("invalid_value", "parent", refError, "give an existing GameObject")});
            return;
        }
    }
    if (argIn(args, "tilesets_dir") && !McpRefs::resolveProjectWritePath(project, args["tilesets_dir"].get<std::string>(), target.tilesetsDir, pathError)) {
        failProblems(call, "Where do the tilesets go?", {tilesetProblem("invalid_value", "tilesets_dir", pathError, "give a project-relative folder")});
        return;
    }
    if (toPrefab && !McpRefs::resolveProjectWritePath(project, args["prefab"].get<std::string>(), target.prefab, pathError)) {
        failProblems(call, "Where does the prefab go?", {tilesetProblem("invalid_value", "prefab", pathError, "give a project-relative .prefab path")});
        return;
    }
    if (argIn(args, "level")) {
        target.grid = args["level"].get<std::string>();
        const bool found = std::any_of(plan->grids.begin(), plan->grids.end(), [&](const ti::ImportGrid& grid) { return grid.name == target.grid; });
        if (!found) {
            std::string names;
            for (const ti::ImportGrid& grid : plan->grids) names += (names.empty() ? "" : ", ") + grid.name;
            failProblems(call, "Which level?", {tilesetProblem("parse", "level", "the map has no level \"" + target.grid + "\"", "use one of: " + names)});
            return;
        }
    }

    const ti::ImportReport report = ti::previewReport(*plan, target);
    if (!boolIn(args, "output")) {
        Json data = reportJson(report);
        data["preview"] = true;
        data["next"] = "A preview: nothing was written. Pass output: true to import it (one undo step).";
        const ti::ImportCounts& c = report.counts;
        call->ok("Preview of " + fileArg + ": " + std::to_string(c.grids) + " grid(s), " + std::to_string(c.layers) + " layer(s), " + std::to_string(c.tilesets) +
                     " tileset(s), " + std::to_string(c.tiles) + " tiles. output: true imports it.", data);
        return;
    }
    if (call->ctx().isPlaying()) {
        failProblems(call, "Nothing was imported:", {tilesetProblem("play_mode", "output", "imports are refused while a scene plays (a preview works)", "stop play, then call again with output")});
        return;
    }

    const auto finish = [plan, target, sceneProject, project, fileArg](std::string& text, Json& data, Problem& error) {
        const ti::ImportResult result = ti::apply(*plan, target);
        if (!result.ok) {
            error = importProblem(result.problem);
            return false;
        }
        data = importApplyResult(result, target.prefab.empty() ? sceneProject : nullptr);
        text = "Imported " + fileArg + ": " + std::to_string(result.grids.size()) + " grid(s), " + std::to_string(result.layers.size()) + " layer(s) (one undo step).";
        return true;
    };

    if (report.counts.tiles <= kImportJobCells) {
        std::string text;
        Json data;
        Problem error;
        if (!finish(text, data, error)) {
            failProblems(call, "Nothing was imported:", {error});
            return;
        }
        call->ok(text, data);
        return;
    }
    McpContext& ctx = call->ctx();
    JobPtr job = ctx.jobs().create("import_tilemap", "Import " + fileArg);
    job->setProgress(-1.0, "importing");
    auto state = std::make_shared<std::string>();
    const std::optional<UndoOrigin> origin = call->attributed() ? std::optional<UndoOrigin>(call->getUndoOrigin()) : std::nullopt;
    getEditorHost().enqueueMainThreadTask([job, finish, state, origin]() {
        McpWriteScope writeScope;
        std::optional<UndoOriginScope> attribution;
        if (origin) attribution.emplace(*origin);
        Json data;
        Problem error;
        if (finish(*state, data, error)) job->succeed(*state, data);
        else job->fail(problemsLines("Nothing was imported:", {error}), Json{{"errors", problemsJsonOf({error})}});
    });
    call->detach();
    replyWhenJobDone(call, job, true, kImportJobWaitSeconds, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        return ToolResult::success(state->empty() ? finished.message() : *state, result);
    });
}

} // namespace
} // namespace tilesettools

void registerTilesetTools(McpRegistry& registry) {
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "edit_tileset";
        tool.title = "Edit Tileset";
        tool.description =
            "Creates or edits a .tileset: sources, tile edits by id, tile (name) or range (name, probability, collision, "
            "animation, properties, terrain), property_schema, terrain_sets (template + origin, or peering), stamps "
            "(fields: jasper://docs/tilemaps). A preview (report, contact sheet with ids) until output: true writes it "
            "in one undo step. check: true returns only the coverage report.";
        tool.inputSchema = object({
            {"path", string("Project-relative .tileset.")},
            {"name", string("")},
            {"tile_size", vec2("")},
            {"tile_shape", stringEnum("", {"square", "isometric", "hex_pointy", "hex_flat"})},
            {"sources", Json{{"type", "array"}}},
            {"tiles", Json{{"type", "array"}}},
            {"property_schema", Json{{"type", "array"}}},
            {"terrain_sets", Json{{"type", "array"}}},
            {"stamps", Json{{"type", "array"}}},
            {"allow_transforms", boolean("")},
            {"check", boolean("")},
            {"output", boolean("")},
            {"columns", integer("", 1, 256)},
            {"scale", integer("", 1, 8)}
        }, {"path"});
        tool.annotations.idempotent = false;
        tool.writesFiles = [](const Json& args) { return tilesettools::boolIn(args, "output"); };
        tool.allowedWhilePlaying = true;   // previews and checks; the handler refuses writes (play_mode)
        tool.activity = ToolActivity::Edit;
        tool.handler = tilesettools::editTilesetTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "import_tilemap";
        tool.title = "Import Tilemap";
        tool.description =
            "Imports a Tiled (.tmj, .tmx) or LDtk (.ldtk) map: a preview (grids, layers, tilesets, skipped, warnings) until "
            "output: true writes the .tileset files and GameObjects (or prefab) in one undo step. linked re-imports "
            "on file change. Mappings: jasper://docs/tilemaps.";
        tool.inputSchema = object({
            {"file", string("Project-relative map.")},
            {"level", string("LDtk level name.")},
            {"scene", sceneRef()},
            {"parent", entityRef()},
            {"prefab", assetPath("")},
            {"tilesets_dir", string("")},
            {"linked", boolean("")},
            {"output", boolean("")}
        }, {"file"});
        tool.annotations.idempotent = false;
        tool.writesFiles = [](const Json& args) { return tilesettools::boolIn(args, "output"); };
        tool.allowedWhilePlaying = true;   // previews; the handler refuses imports (play_mode)
        tool.activity = ToolActivity::Edit;
        tool.handler = tilesettools::importTilemapTool;
        registry.addTool(std::move(tool));
    }
}

}
