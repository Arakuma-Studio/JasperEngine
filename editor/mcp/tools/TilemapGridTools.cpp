// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// edit_tilemap and get_tilemap on grid tilemaps (docs/specs/S50-grid-tilemap.md R75, R76, R79,
// R83; agent guide docs/mcp/guide/tilemaps.md). WorldTools.cpp's handlers call these first and
// keep the legacy Free Tilemap path for what is not a Grid.
//
// A call never writes as it goes: every op runs on a private copy of each layer's store, so a
// failing op, or a preview, leaves nothing to undo. The scene is touched only at the end, in one
// undo step: the Grid and layers the call makes, the tileset it writes, then one TilemapCellsCmd.

#include "McpTools.h"

#include "Engine.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpValues.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "component/GridComponent.h"
#include "component/TilemapComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "io/FileData.h"
#include "tilemap/TilemapMeshAccess.h"
#include "tilemap/TilemapBrush.h"
#include "tilemap/TilemapCellsCmd.h"
#include "tilemap/TilemapComposite.h"
#include "tilemap/TilemapConvert.h"
#include "tilemap/TilesetEdit.h"
#include "tilemap/TilesetPool.h"
#include "util/Base64.h"
#include "util/ProjectFileWrites.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

// file-local, in a namespace of its own: a unity build merges the MCP tool sources
namespace tilemapgridtools {
namespace {

namespace fs = std::filesystem;
namespace tm = jasperengine::editor::tilemap;

using Problem = jasperengine::editor::pixelart::PixelProblem;

constexpr int kDefaultMaxCells = 6000;
constexpr const char* kGridTypedDoc = "jasper://docs/tilemaps";

Problem makeProblem(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
    Problem p;
    p.kind = kind;
    p.field = field;
    p.message = message;
    p.fix = fix;
    return p;
}

Json problemJson(const Problem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix.empty() ? std::string("see ") + kGridTypedDoc : p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    return j;
}

std::string problemText(const Problem& p, const std::string& where) {
    return p.kind + ": " + (where.empty() ? std::string() : where + ": ") + p.message + (p.fix.empty() ? "" : ". Fix: " + p.fix);
}

void failTyped(const CallPtr& call, const Problem& p, const std::string& where = {}) {
    Problem shown = p;
    if (!where.empty()) shown.field = where + (p.field.empty() ? std::string() : "." + p.field);
    const Json typed = problemJson(shown);
    call->fail(problemText(p, where), Json{{"error", typed}, {"errors", Json::array({typed})}});
}

bool hasArg(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

bool flagArg(const Json& args, const char* key, bool fallback = false) {
    return hasArg(args, key) && args[key].is_boolean() ? args[key].get<bool>() : fallback;
}

int intArg(const Json& args, const char* key, int fallback) {
    return hasArg(args, key) && args[key].is_number() ? args[key].get<int>() : fallback;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool cellArg(const Json& value, CellCoord& out) {
    if (!value.is_array() || value.size() < 2 || !value[0].is_number() || !value[1].is_number()) return false;
    out.x = value[0].get<int32_t>();
    out.y = value[1].get<int32_t>();
    return true;
}

// the Grid (self or nearest ancestor) of a layer; NULL_ENTITY when it has none
Entity gridAbove(Scene* scene, Entity entity) {
    Entity current = entity;
    while (current != NULL_ENTITY) {
        if (scene->findComponent<GridComponent>(current)) return current;
        const Transform* transform = scene->findComponent<Transform>(current);
        if (!transform) break;
        current = transform->parent;
    }
    return NULL_ENTITY;
}

// the layers of a Grid, in hierarchy order, not those of a Grid nested below it
std::vector<Entity> gridLayers(SceneProject* sceneProject, Entity grid) {
    std::vector<Entity> layers;
    Scene* scene = sceneProject->scene;
    for (Entity entity : sceneProject->entities) {
        if (scene->findComponent<TilemapLayerComponent>(entity) && gridAbove(scene, entity) == grid) layers.push_back(entity);
    }
    return layers;
}

// a Grid's Cell Size is in world units: a tile's pixels over the project's Pixels Per Unit, as
// the runtime draws it (TilemapSystem) and the importers and the conversion store it
double projectPixelsPerUnit() {
    return static_cast<double>(std::max(Engine::getPixelsPerUnit(), 1));
}

TileGridSpec defaultSpecFor(const TilesetData* data) {
    TileGridSpec spec;
    if (data) {
        spec.cellWidth = data->tileWidth / projectPixelsPerUnit();
        spec.cellHeight = data->tileHeight / projectPixelsPerUnit();
    }
    return spec;
}

// the tileset of a layer path; null when the layer has none or it does not load
std::shared_ptr<const TilesetRuntime> runtimeOf(const std::string& path) {
    return path.empty() ? nullptr : TilesetPool::get(path);
}

// ---------------------------------------------------------------- ASCII

const char* const kGridChars = "#ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@$%&*+=?!";

std::string tileLegend(const TilesetData* data, int32_t tile) {
    if (data) {
        if (const TileDef* def = data->findTile(tile)) {
            if (!def->name.empty()) return def->name;
        }
    }
    return "#" + std::to_string(tile);
}

// the ASCII view of `region`, the same shape as the legacy grid object (lines top = highest row)
Json asciiGrid(const TileCells& cells, const TilesetData* data, const CellRect& region, int maxCells,
               const std::map<int32_t, char>& preferred, const TileGridSpec& spec) {
    Json out = {{"cell_size", Json::array({spec.cellWidth, spec.cellHeight})}, {"origin", Json::array({0, 0})}};
    if (region.isEmpty()) {
        out["tiles_on_grid"] = 0;
        out["tiles_off_grid"] = 0;
        out["lines"] = Json::array();
        return out;
    }
    out["columns"] = {region.minX, region.maxX};
    out["rows"] = {region.minY, region.maxY};
    if (region.exceeds(static_cast<int64_t>(maxCells))) {
        out["tiles_on_grid"] = cells.cellCount();
        out["tiles_off_grid"] = 0;
        out["lines_omitted"] = "The region is " + std::to_string(region.width()) + "x" + std::to_string(region.height()) +
                               " cells; raise max_cells or pass region to see it.";
        return out;
    }
    std::map<int32_t, char> chars = preferred;
    std::set<char> used;
    for (const auto& item : chars) used.insert(item.second);
    size_t tiles = 0;
    cells.forEachCell(region, [&](CellCoord, uint32_t value, uint8_t) {
        if (cellValueEmpty(value)) return;
        tiles++;
        const int32_t tile = cellValueTile(value);
        if (chars.count(tile)) return;
        const std::string name = tileLegend(data, tile);
        const char first = name[0];
        if (first != '#' && std::isalnum(static_cast<unsigned char>(first)) && !used.count(first)) {
            chars[tile] = first;
            used.insert(first);
            return;
        }
        for (const char* p = kGridChars; *p; p++) {
            if (!used.count(*p)) {
                chars[tile] = *p;
                used.insert(*p);
                return;
            }
        }
        chars[tile] = '?';
    });
    Json legend = Json::object();
    for (const auto& item : chars) legend[std::string(1, item.second)] = tileLegend(data, item.first);
    out["tiles_on_grid"] = tiles;
    out["tiles_off_grid"] = 0;
    out["legend"] = legend;
    Json lines = Json::array();
    for (int64_t y = region.maxY; y >= region.minY; y--) {
        std::string line(static_cast<size_t>(region.width()), '.');
        for (int64_t x = region.minX; x <= region.maxX; x++) {
            const uint32_t value = cells.get(CellCoord{static_cast<int32_t>(x), static_cast<int32_t>(y)});
            if (!cellValueEmpty(value)) {
                auto it = chars.find(cellValueTile(value));
                line[static_cast<size_t>(x - region.minX)] = it != chars.end() ? it->second : '?';
            }
        }
        lines.push_back(line);
    }
    out["lines"] = lines;
    out["lines_note"] = "Top line is row " + std::to_string(region.maxY) + ", bottom line row " + std::to_string(region.minY) +
                        "; first character is column " + std::to_string(region.minX) + ". '.' is empty.";
    return out;
}

// the same view of the terrain plane ('.' is no terrain)
Json terrainGrid(const TileCells& cells, const TilesetData* data, const CellRect& region, int maxCells) {
    Json out = {{"legend", Json::object()}};
    if (region.isEmpty()) {
        out["lines"] = Json::array();
        return out;
    }
    if (region.exceeds(static_cast<int64_t>(maxCells))) {
        out["lines_omitted"] = "The region is larger than max_cells.";
        return out;
    }
    std::map<int, char> chars;
    std::set<char> used;
    const auto charFor = [&](int terrain) {
        auto it = chars.find(terrain);
        if (it != chars.end()) return it->second;
        const TerrainDef* def = data ? data->terrain(terrain) : nullptr;
        char chosen = '?';
        if (def && !def->name.empty() && std::isalnum(static_cast<unsigned char>(def->name[0])) && !used.count(def->name[0])) {
            chosen = def->name[0];
        } else {
            for (const char* p = kGridChars; *p; p++) {
                if (!used.count(*p)) {
                    chosen = *p;
                    break;
                }
            }
        }
        chars[terrain] = chosen;
        used.insert(chosen);
        return chosen;
    };
    Json lines = Json::array();
    for (int64_t y = region.maxY; y >= region.minY; y--) {
        std::string line(static_cast<size_t>(region.width()), '.');
        for (int64_t x = region.minX; x <= region.maxX; x++) {
            const int terrain = cells.getTerrain(CellCoord{static_cast<int32_t>(x), static_cast<int32_t>(y)});
            if (terrain != NO_TERRAIN) line[static_cast<size_t>(x - region.minX)] = charFor(terrain);
        }
        lines.push_back(line);
    }
    out["lines"] = lines;
    Json legend = Json::object();
    for (const auto& item : chars) {
        const TerrainDef* def = data ? data->terrain(item.first) : nullptr;
        legend[std::string(1, item.second)] = def ? def->name : "#" + std::to_string(item.first);
    }
    out["legend"] = legend;
    return out;
}

// ---------------------------------------------------------------- images

ToolImage pngOf(const jasperengine::editor::pixelart::PixelImage& image) {
    const std::string png = image.encodePng();
    return ToolImage{Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"};
}

// R76's image: a CPU composite of a region (R83: same arguments, same bytes), at the project's
// Pixels Per Unit, so the image places tiles where the scene draws them
bool compositeImage(const TileCells& cells, const TilesetRuntime* tileset, const TileGridSpec& spec, const CellRect& region, int scale,
                    double anchorX, double anchorY, bool dualGrid, ToolImage& out, Json& info, Problem& error) {
    if (!tileset || !tileset->data) {
        error = makeProblem("tileset_missing", "image", "the layer has no loaded tileset to draw", "assign a .tileset with edit_tilemap tileset");
        return false;
    }
    tm::CompositeParams params;
    params.cells = &cells;
    params.tileset = tileset;
    params.grid = spec;
    params.region = region;
    params.scale = scale;
    params.pixelsPerUnit = projectPixelsPerUnit();
    params.tileAnchorX = anchorX;
    params.tileAnchorY = anchorY;
    params.dualGrid = dualGrid;
    tm::TilesetPagePixels pages;
    tm::CompositeResult result;
    if (!tm::compositeLayer(params, pages, result, error)) return false;
    out = pngOf(result.image);
    info = {{"width", result.image.width()}, {"height", result.image.height()}, {"scale", scale}, {"origin", {result.originX, result.originY}},
            {"region", {region.minX, region.minY, region.maxX, region.maxY}}};
    if (result.missing > 0) info["missing_cells"] = result.missing;
    return true;
}

// ---------------------------------------------------------------- edit_tilemap

struct LayerSlot {
    Entity entity = NULL_ENTITY;          // NULL_ENTITY: made at commit (create, add_layer)
    std::string name;
    int orderInLayer = 0;                 // of a new layer: the next one
    std::string tilesetPath;              // what the layer will hold (stored path)
    bool assignTileset = false;           // the call changed it
    std::shared_ptr<const TilesetRuntime> tileset;
    TileCells cells;                      // a private copy of the layer's store
    bool opened = false;                  // `cells` holds the layer's store
    tm::BrushEdit edit;
    bool dualGrid = false;
    bool setDualGrid = false;             // a new layer made from a dual-grid terrain set
    uint32_t seed = 0;
};

struct GridEdit {
    const CallPtr& call;
    const Json& args;
    Project* project;
    SceneProject* sceneProject;
    Scene* scene;
    bool preview = false;

    Entity gridEntity = NULL_ENTITY;      // NULL_ENTITY: created at commit
    bool createGrid = false;
    TileGridSpec spec;
    bool specFromGrid = false;
    bool linked = false;

    std::vector<LayerSlot> slots;
    int chosen = 0;                       // the slot of the top-level `layer`
    std::map<int32_t, char> preferredChars;
    std::vector<std::string> warnings;
    int painted = 0;
    int erased = 0;

    // the call's new tileset file, written in its step (and the document, for the pool)
    std::shared_ptr<TilesetData> newTilesetData;
    std::string newTilesetPath;
    fs::path newTilesetAbsolute;

    // the top-level tileset of a call on a Grid with no layer: what its first add_layer takes
    std::shared_ptr<const TilesetRuntime> firstLayerTileset;
    std::string firstLayerTilesetPath;

    explicit GridEdit(const CallPtr& c): call(c), args(c->args()), project(c->ctx().project()), sceneProject(nullptr), scene(nullptr) {}
};

void ensureOpened(GridEdit& e, LayerSlot& slot) {
    if (slot.opened || slot.entity == NULL_ENTITY) {
        slot.opened = true;
        return;
    }
    slot.cells = e.scene->getComponent<TilemapLayerComponent>(slot.entity).cells.read();
    slot.opened = true;
}

tm::BrushTarget targetOf(GridEdit& e, LayerSlot& slot) {
    return tm::BrushTarget{slot.cells, slot.tileset ? slot.tileset->data.get() : nullptr, e.spec, slot.dualGrid, slot.seed};
}

// R75: a tile by name or id; null erases; false with unknown_tile
bool resolveTile(const TilesetData* data, const Json& value, int32_t& out, Problem& error, const std::string& field) {
    if (value.is_null()) {
        out = NO_TILE;
        return true;
    }
    const auto listing = [&]() {
        std::string names;
        if (data) {
            size_t shown = 0;
            for (const TileDef& def : data->tiles) {
                if (shown++ == 30) {
                    names += ", ...";
                    break;
                }
                names += (names.empty() ? "" : ", ") + data->tileName(def);
            }
        }
        return names.empty() ? std::string("none") : names;
    };
    if (value.is_number_integer()) {
        const int32_t id = value.get<int32_t>();
        if (data && data->findTile(id)) {
            out = id;
            return true;
        }
        error = makeProblem("unknown_tile", field, "the tileset has no tile " + std::to_string(id), data ? "use a tile id or name of it: " + listing() : "assign a tileset first");
        return false;
    }
    if (value.is_string()) {
        const std::string name = value.get<std::string>();
        const int32_t id = data ? data->findTileByName(name) : NO_TILE;
        if (id != NO_TILE) {
            out = id;
            return true;
        }
        error = makeProblem("unknown_tile", field, "the tileset has no tile named \"" + name + "\"",
                            data ? "use one of: " + listing() : "assign a tileset first (edit_tilemap tileset, or edit_tileset)");
        return false;
    }
    error = makeProblem("unknown_tile", field, "a tile is a name, an id, or null to erase", "pass a tile name or id");
    return false;
}

bool flipsArg(const Json& value, uint32_t& flags, Problem& error, const std::string& field) {
    flags = 0;
    if (value.is_null()) return true;
    if (!value.is_string()) {
        error = makeProblem("invalid_value", field, "flips are a string of the letters x, y and t", "use \"x\", \"y\", \"t\" or a combination like \"xt\"");
        return false;
    }
    for (char c : value.get<std::string>()) {
        if (c == 'x') flags |= TILE_FLIP_X;
        else if (c == 'y') flags |= TILE_FLIP_Y;
        else if (c == 't') flags |= TILE_TRANSPOSE;
        else {
            error = makeProblem("invalid_value", field, std::string("'") + c + "' is not a flip letter", "use x, y and t");
            return false;
        }
    }
    return true;
}

// a brush of a single tile from {tile, flip}; `erase` true means no tile
bool tileBrushOf(const TilesetData* data, const Json& op, tm::BrushStamp& stamp, bool& erase, Problem& error, const std::string& where) {
    erase = flagArg(op, "erase") || (op.contains("tile") && op["tile"].is_null());
    if (erase) return true;
    if (!hasArg(op, "tile")) {
        error = makeProblem("unknown_tile", where + ".tile", "this op needs tile (a name or id), or erase: true", "add tile");
        return false;
    }
    int32_t tile = NO_TILE;
    if (!resolveTile(data, op["tile"], tile, error, where + ".tile")) return false;
    uint32_t flags = 0;
    if (hasArg(op, "flip") && !flipsArg(op["flip"], flags, error, where + ".flip")) return false;
    stamp = tm::BrushStamp::single(tile, flags);
    return true;
}

// the ASCII lines of an op's grid (string or lines); empty leading/trailing lines are not rows
bool gridLines(const Json& op, std::vector<std::string>& lines) {
    if (!hasArg(op, "grid")) return false;
    if (op["grid"].is_string()) {
        std::stringstream stream(op["grid"].get<std::string>());
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        while (!lines.empty() && lines.front().find_first_not_of(' ') == std::string::npos) lines.erase(lines.begin());
        while (!lines.empty() && lines.back().find_first_not_of(' ') == std::string::npos) lines.pop_back();
        return true;
    }
    if (op["grid"].is_array()) {
        for (const Json& line : op["grid"]) lines.push_back(line.is_string() ? line.get<std::string>() : std::string());
        return true;
    }
    return false;
}

// from/to (inclusive), `to` defaulting to `from`
bool boxArg(const Json& op, CellRect& rect, Problem& error, const std::string& where) {
    CellCoord from, to;
    if (!hasArg(op, "from") || !cellArg(op["from"], from)) {
        error = makeProblem("invalid_value", where + ".from", "this op needs from [x, y] (inclusive)", "add from and to, cell coordinates");
        return false;
    }
    to = from;
    if (hasArg(op, "to") && !cellArg(op["to"], to)) {
        error = makeProblem("invalid_value", where + ".to", "to must be [x, y]", "pass cell coordinates");
        return false;
    }
    rect = CellRect::of(from, to);
    return true;
}

// the cells an op targets: cells [[x, y]...], or from/to as a box; unset out means "none given"
bool cellsArg(const Json& op, std::vector<CellCoord>& out, bool& given, Problem& error, const std::string& where) {
    given = false;
    if (hasArg(op, "cells")) {
        given = true;
        if (!op["cells"].is_array()) {
            error = makeProblem("invalid_value", where + ".cells", "cells must be [[x, y], ...]", "pass cell coordinates");
            return false;
        }
        for (const Json& item : op["cells"]) {
            CellCoord cell;
            if (!cellArg(item, cell)) {
                error = makeProblem("invalid_value", where + ".cells", "each cell is [x, y]", "pass cell coordinates");
                return false;
            }
            out.push_back(cell);
        }
        return true;
    }
    if (hasArg(op, "from")) {
        given = true;
        CellRect rect;
        if (!boxArg(op, rect, error, where)) return false;
        return tm::boxCells(rect, out, error);
    }
    return true;
}

const std::map<std::string, bool>& knownOps() {
    static const std::map<std::string, bool> ops = {
        {"paint", false}, {"fill", false}, {"erase", false}, {"set", false}, {"clear", false},
        {"line", true}, {"flood", true}, {"random", true}, {"terrain", true}, {"refresh", true},
        {"stamp", true}, {"add_layer", true}, {"convert", true},
    };
    return ops;
}

bool paintOp(GridEdit& e, LayerSlot& slot, const Json& op, const std::string& where, Problem& error) {
    const TilesetData* data = slot.tileset ? slot.tileset->data.get() : nullptr;
    std::vector<std::string> lines;
    if (!gridLines(op, lines)) {
        error = makeProblem("invalid_value", where + ".grid", "paint needs grid (a multi-line string or an array of lines)", "add grid and legend");
        return false;
    }
    struct Entry {
        int32_t tile = NO_TILE;
        uint32_t flags = 0;
    };
    std::map<char, Entry> legend;
    if (hasArg(op, "legend")) {
        if (!op["legend"].is_object()) {
            error = makeProblem("invalid_value", where + ".legend", "legend must be {\"char\": tile name | id | null}", "use single-character keys");
            return false;
        }
        for (auto it = op["legend"].begin(); it != op["legend"].end(); ++it) {
            if (it.key().size() != 1) {
                error = makeProblem("invalid_value", where + ".legend", "legend keys are single characters, not \"" + it.key() + "\"", "use single-character keys");
                return false;
            }
            Entry entry;
            const Json& value = it.value();
            const Json& tile = value.is_array() && !value.empty() ? value[0] : value;
            if (!resolveTile(data, tile, entry.tile, error, where + ".legend['" + it.key() + "']")) return false;
            if (value.is_array() && value.size() > 1 && !flipsArg(value[1], entry.flags, error, where + ".legend['" + it.key() + "'][1]")) return false;
            legend[it.key()[0]] = entry;
            if (entry.tile != NO_TILE) e.preferredChars[entry.tile] = it.key()[0];
        }
    }
    CellCoord at;
    if (hasArg(op, "at") && !cellArg(op["at"], at)) {
        error = makeProblem("invalid_value", where + ".at", "at must be [x, y]", "pass cell coordinates");
        return false;
    }
    const bool eraseBlank = lowered(hasArg(op, "blank") && op["blank"].is_string() ? op["blank"].get<std::string>() : "skip") == "erase";
    const tm::PaintOptions options{flagArg(op, "keep_terrain")};
    tm::BrushTarget target = targetOf(e, slot);
    const int rows = static_cast<int>(lines.size());
    for (int i = 0; i < rows; i++) {
        const int32_t y = at.y + (rows - 1 - i);
        for (size_t j = 0; j < lines[i].size(); j++) {
            const char ch = lines[i][j];
            const CellCoord cell{at.x + static_cast<int32_t>(j), y};
            auto it = legend.find(ch);
            if (it == legend.end()) {
                if (ch == '.' || ch == ' ') {
                    if (eraseBlank && !tm::eraseCell(target, cell, slot.edit)) {
                        error = slot.edit.error;
                        return false;
                    }
                    continue;
                }
                error = makeProblem("unknown_tile", where + ".legend", std::string("character '") + ch + "' (line " + std::to_string(i + 1) +
                                    ") is not in the legend", "add it to legend; '.' and ' ' are empty cells");
                return false;
            }
            if (it->second.tile == NO_TILE) {
                if (!tm::eraseCell(target, cell, slot.edit)) {
                    error = slot.edit.error;
                    return false;
                }
                e.erased++;
            } else {
                if (!tm::paintStamp(target, tm::BrushStamp::single(it->second.tile, it->second.flags), cell, options, slot.edit)) {
                    error = slot.edit.error;
                    return false;
                }
                e.painted++;
            }
        }
    }
    return true;
}

bool setOp(GridEdit& e, LayerSlot& slot, const Json& op, const std::string& where, Problem& error) {
    const TilesetData* data = slot.tileset ? slot.tileset->data.get() : nullptr;
    if (!hasArg(op, "cells") || !op["cells"].is_array()) {
        error = makeProblem("invalid_value", where + ".cells", "set needs cells [[x, y, tile, flips?], ...]", "tile: name, id or null");
        return false;
    }
    const tm::PaintOptions options{flagArg(op, "keep_terrain")};
    tm::BrushTarget target = targetOf(e, slot);
    for (const Json& item : op["cells"]) {
        if (!item.is_array() || item.size() < 3 || item.size() > 4 || !item[0].is_number() || !item[1].is_number()) {
            error = makeProblem("invalid_value", where + ".cells", "each cell is [x, y, tile] or [x, y, tile, flips]", "flips is a string of x, y and t");
            return false;
        }
        CellCoord cell{item[0].get<int32_t>(), item[1].get<int32_t>()};
        int32_t tile = NO_TILE;
        if (!resolveTile(data, item[2], tile, error, where + ".cells")) return false;
        uint32_t flags = 0;
        if (item.size() == 4 && !flipsArg(item[3], flags, error, where + ".cells")) return false;
        if (tile == NO_TILE) {
            if (!tm::eraseCell(target, cell, slot.edit)) {
                error = slot.edit.error;
                return false;
            }
            e.erased++;
        } else {
            if (!tm::paintStamp(target, tm::BrushStamp::single(tile, flags), cell, options, slot.edit)) {
                error = slot.edit.error;
                return false;
            }
            e.painted++;
        }
    }
    return true;
}

bool terrainOp(GridEdit& e, LayerSlot& slot, const Json& op, const std::string& where, Problem& error) {
    const TilesetData* data = slot.tileset ? slot.tileset->data.get() : nullptr;
    if (!data) {
        error = makeProblem("tileset_missing", where, "the layer has no tileset", "assign a .tileset first");
        return false;
    }
    const auto terrainOf = [&](const Json& value, int& out) {
        if (value.is_null()) {
            out = NO_TERRAIN;
            return true;
        }
        if (value.is_string()) {
            out = data->findTerrain(value.get<std::string>());
            if (out != NO_TERRAIN) return true;
        }
        std::string names;
        for (const TerrainSetDef& set : data->terrainSets) {
            for (const TerrainDef& terrain : set.terrains) names += (names.empty() ? "" : ", ") + terrain.name;
        }
        error = makeProblem("unknown_terrain", where + ".terrain", "no terrain " + value.dump() + " in the tileset",
                            names.empty() ? "add a terrain set with edit_tileset" : "use one of: " + names);
        return false;
    };
    tm::BrushTarget target = targetOf(e, slot);
    const bool erase = flagArg(op, "erase");
    const bool keepTerrain = flagArg(op, "keep_terrain");
    const auto paintCells = [&](std::vector<CellCoord> cells, int terrain) {
        if (keepTerrain) {
            cells.erase(std::remove_if(cells.begin(), cells.end(), [&](const CellCoord& c) { return slot.cells.getTerrain(c) != NO_TERRAIN; }), cells.end());
        }
        if (cells.empty()) return true;
        if (!tm::paintTerrain(target, cells, terrain, slot.edit)) {
            error = slot.edit.error;
            return false;
        }
        return true;
    };
    std::vector<std::string> lines;
    if (gridLines(op, lines)) {
        if (!hasArg(op, "legend") || !op["legend"].is_object()) {
            error = makeProblem("invalid_value", where + ".legend", "a terrain grid needs legend {\"char\": terrain name | null}", "add legend");
            return false;
        }
        std::map<char, int> legend;
        for (auto it = op["legend"].begin(); it != op["legend"].end(); ++it) {
            if (it.key().size() != 1) {
                error = makeProblem("invalid_value", where + ".legend", "legend keys are single characters", "use single-character keys");
                return false;
            }
            int terrain = NO_TERRAIN;
            if (!terrainOf(it.value(), terrain)) return false;
            legend[it.key()[0]] = terrain;
        }
        CellCoord at;
        if (hasArg(op, "at") && !cellArg(op["at"], at)) {
            error = makeProblem("invalid_value", where + ".at", "at must be [x, y]", "pass cell coordinates");
            return false;
        }
        std::map<int, std::vector<CellCoord>> byTerrain;
        const int rows = static_cast<int>(lines.size());
        for (int i = 0; i < rows; i++) {
            for (size_t j = 0; j < lines[i].size(); j++) {
                const char ch = lines[i][j];
                auto it = legend.find(ch);
                if (it == legend.end()) {
                    if (ch == '.' || ch == ' ') continue;
                    error = makeProblem("unknown_terrain", where + ".legend", std::string("character '") + ch + "' (line " + std::to_string(i + 1) + ") is not in the legend",
                                        "add it to legend; '.' and ' ' leave the cell alone");
                    return false;
                }
                byTerrain[erase ? NO_TERRAIN : it->second].push_back(CellCoord{at.x + static_cast<int32_t>(j), at.y + (rows - 1 - i)});
            }
        }
        for (const auto& item : byTerrain) {
            if (!paintCells(item.second, item.first)) return false;
        }
        return true;
    }
    int terrain = NO_TERRAIN;
    if (!erase) {
        if (!hasArg(op, "terrain")) {
            error = makeProblem("unknown_terrain", where + ".terrain", "this op needs terrain (a name), grid + legend, or erase: true", "add terrain");
            return false;
        }
        if (!terrainOf(op["terrain"], terrain)) return false;
    }
    std::vector<CellCoord> cells;
    bool given = false;
    if (!cellsArg(op, cells, given, error, where)) return false;
    if (!given) {
        error = makeProblem("invalid_value", where, "terrain needs cells, from/to, or grid + legend", "add the cells");
        return false;
    }
    return paintCells(std::move(cells), terrain);
}

bool stampOp(GridEdit& e, LayerSlot& slot, const Json& op, const std::string& where, Problem& error) {
    const TilesetData* data = slot.tileset ? slot.tileset->data.get() : nullptr;
    if (!data) {
        error = makeProblem("tileset_missing", where, "the layer has no tileset", "assign a .tileset first");
        return false;
    }
    const std::string name = hasArg(op, "name") && op["name"].is_string() ? op["name"].get<std::string>() : std::string();
    const TileStamp* source = nullptr;
    for (const TileStamp& stamp : data->stamps) {
        if (stamp.name == name) source = &stamp;
    }
    if (!source) {
        std::string names;
        for (const TileStamp& stamp : data->stamps) names += (names.empty() ? "" : ", ") + stamp.name;
        error = makeProblem("unknown_tile", where + ".name", "the tileset has no stamp \"" + name + "\"",
                            names.empty() ? "add stamps with edit_tileset" : "use one of: " + names);
        return false;
    }
    CellCoord at;
    if (hasArg(op, "at") && !cellArg(op["at"], at)) {
        error = makeProblem("invalid_value", where + ".at", "at must be [x, y]", "pass cell coordinates");
        return false;
    }
    tm::BrushStamp stamp;
    stamp.width = source->width;
    stamp.height = source->height;
    for (const TileStampCell& cell : source->cells) stamp.cells.push_back(tm::BrushCell{cell.x, cell.y, cell.tile, cell.flags});
    tm::BrushTarget target = targetOf(e, slot);
    if (!tm::paintStamp(target, stamp, at, tm::PaintOptions{flagArg(op, "keep_terrain")}, slot.edit)) {
        error = slot.edit.error;
        return false;
    }
    return true;
}

// runs one op on the slot; a false leaves the slot's store as it was (brush ops apply or nothing)
bool runOp(GridEdit& e, LayerSlot& slot, const std::string& kind, const Json& op, const std::string& where, Problem& error) {
    const TilesetData* data = slot.tileset ? slot.tileset->data.get() : nullptr;
    tm::BrushTarget target = targetOf(e, slot);
    const tm::PaintOptions options{flagArg(op, "keep_terrain")};
    if (kind == "paint") return paintOp(e, slot, op, where, error);
    if (kind == "set") return setOp(e, slot, op, where, error);
    if (kind == "terrain") return terrainOp(e, slot, op, where, error);
    if (kind == "stamp") return stampOp(e, slot, op, where, error);
    if (kind == "fill" || kind == "line") {
        bool erase = false;
        tm::BrushStamp stamp;
        if (kind == "fill" && !hasArg(op, "tile") && !flagArg(op, "erase")) {
            error = makeProblem("unknown_tile", where + ".tile", "fill needs tile (a name or id)", "add tile");
            return false;
        }
        if (!tileBrushOf(data, op, stamp, erase, error, where)) return false;
        if (kind == "line") {
            CellCoord from, to;
            if (!hasArg(op, "from") || !cellArg(op["from"], from) || !hasArg(op, "to") || !cellArg(op["to"], to)) {
                error = makeProblem("invalid_value", where, "line needs from and to [x, y]", "pass cell coordinates");
                return false;
            }
            const bool ok = erase ? tm::eraseLine(target, from, to, slot.edit) : tm::paintLine(target, stamp, from, to, options, slot.edit);
            if (!ok) error = slot.edit.error;
            return ok;
        }
        CellRect rect;
        if (!boxArg(op, rect, error, where)) return false;
        const bool ok = erase ? tm::eraseBox(target, rect, slot.edit) : tm::boxFill(target, stamp, rect, options, slot.edit);
        if (!ok) {
            error = slot.edit.error;
            return false;
        }
        e.painted += erase ? 0 : static_cast<int>(std::min<int64_t>(rect.area(), 1000000));
        return true;
    }
    if (kind == "erase") {
        if (hasArg(op, "cells")) {
            std::vector<CellCoord> cells;
            bool given = false;
            if (!cellsArg(op, cells, given, error, where)) return false;
            for (const CellCoord& cell : cells) {
                if (!tm::eraseCell(target, cell, slot.edit)) {
                    error = slot.edit.error;
                    return false;
                }
            }
            e.erased += static_cast<int>(cells.size());
            return true;
        }
        CellRect rect;
        if (!boxArg(op, rect, error, where)) return false;
        if (!tm::eraseBox(target, rect, slot.edit)) {
            error = slot.edit.error;
            return false;
        }
        e.erased += static_cast<int>(std::min<int64_t>(rect.area(), 1000000));
        return true;
    }
    if (kind == "clear") {
        std::vector<CellCoord> withTerrain, withTile;
        slot.cells.forEachUsed([&](CellCoord cell, uint32_t value, uint8_t terrain) {
            if (terrain != TERRAIN_NONE_VALUE) withTerrain.push_back(cell);
            if (!cellValueEmpty(value)) withTile.push_back(cell);
        });
        if (!withTerrain.empty() && !tm::paintTerrain(target, withTerrain, NO_TERRAIN, slot.edit)) {
            error = slot.edit.error;
            return false;
        }
        for (const CellCoord& cell : withTile) {
            if (!tm::eraseCell(target, cell, slot.edit)) {
                error = slot.edit.error;
                return false;
            }
        }
        e.erased += static_cast<int>(withTile.size());
        return true;
    }
    if (kind == "flood") {
        bool erase = false;
        tm::BrushStamp stamp;
        if (!tileBrushOf(data, op, stamp, erase, error, where)) return false;
        CellCoord at;
        if (!hasArg(op, "at") || !cellArg(op["at"], at)) {
            error = makeProblem("invalid_value", where + ".at", "flood needs at [x, y]", "pass the cell to start from");
            return false;
        }
        const std::string scope = hasArg(op, "scope") && op["scope"].is_string() ? lowered(op["scope"].get<std::string>()) : "contiguous";
        if (scope != "contiguous" && scope != "every") {
            error = makeProblem("invalid_value", where + ".scope", "scope is contiguous or every", "use one of them");
            return false;
        }
        const tm::FloodScope floodScope = scope == "every" ? tm::FloodScope::EveryEqual : tm::FloodScope::Contiguous;
        const bool ok = erase ? tm::eraseFlood(target, at, floodScope, slot.edit) : tm::floodFill(target, stamp, at, floodScope, options, slot.edit);
        if (!ok) error = slot.edit.error;
        return ok;
    }
    if (kind == "random") {
        if (!hasArg(op, "tiles") || !op["tiles"].is_array() || op["tiles"].empty()) {
            error = makeProblem("unknown_tile", where + ".tiles", "random needs tiles: [name | id | {tile, weight, flip}]", "list at least one tile");
            return false;
        }
        std::vector<tm::RandomEntry> entries;
        for (size_t i = 0; i < op["tiles"].size(); i++) {
            const Json& item = op["tiles"][i];
            tm::RandomEntry entry;
            const std::string field = where + ".tiles[" + std::to_string(i) + "]";
            const Json& tileJson = item.is_object() ? (item.contains("tile") ? item["tile"] : Json()) : item;
            if (!resolveTile(data, tileJson, entry.tile, error, field)) return false;
            if (entry.tile == NO_TILE) {
                error = makeProblem("unknown_tile", field, "a random entry needs a tile", "give a tile name or id");
                return false;
            }
            if (item.is_object()) {
                if (item.contains("weight") && item["weight"].is_number()) entry.weight = item["weight"].get<float>();
                if (hasArg(item, "flip") && !flipsArg(item["flip"], entry.flags, error, field + ".flip")) return false;
            }
            entries.push_back(entry);
        }
        std::vector<CellCoord> cells;
        bool given = false;
        if (!cellsArg(op, cells, given, error, where)) return false;
        if (!given) {
            error = makeProblem("invalid_value", where, "random needs cells or from/to", "add the cells");
            return false;
        }
        const uint32_t strokeSeed = static_cast<uint32_t>(intArg(op, "seed", 0));
        if (!tm::paintRandom(target, entries, strokeSeed, cells, options, slot.edit)) {
            error = slot.edit.error;
            return false;
        }
        e.painted += static_cast<int>(cells.size());
        return true;
    }
    if (kind == "refresh") {
        CellRect rect;
        if (hasArg(op, "from")) {
            if (!boxArg(op, rect, error, where)) return false;
        } else {
            rect = slot.cells.usedRectWithTerrain();
        }
        if (rect.isEmpty()) return true;
        if (!tm::refreshTerrain(target, rect, slot.edit)) {
            error = slot.edit.error;
            return false;
        }
        return true;
    }
    error = makeProblem("invalid_value", where + ".op", "unknown op \"" + kind + "\"", "use one of the ops in " + std::string(kGridTypedDoc));
    return false;
}

// ---- the tileset argument

struct TilesetArg {
    bool given = false;
    bool isSlice = false;
    std::string path;                 // stored path of the .tileset
    std::shared_ptr<TilesetData> data;
    fs::path absolute;
};

// the `tileset` slice object: columns/rows or tile_size, offset (margin), padding (spacing)
bool sliceTileset(GridEdit& e, const Json& spec, const std::string& textureStored, TilesetArg& out, Problem& error) {
    if (textureStored.empty()) {
        error = makeProblem("tileset_missing", "tileset", "a slice needs a texture", "pass texture, or a .tileset path as tileset");
        return false;
    }
    tm::TilesetPagePixels pages;
    const jasperengine::editor::pixelart::PixelImage* image = pages.get(textureStored);
    if (!image) {
        error = makeProblem("tileset_missing", "texture", "can't read the texture: " + pages.lastError(), "pass a readable PNG inside the project");
        return false;
    }
    int w = image->width(), h = image->height();
    const auto vec2Of = [&](const char* key, double& x, double& y, bool& given) {
        given = false;
        if (!hasArg(spec, key)) return true;
        given = true;
        Vector2 value;
        if (!McpValues::parseVector2(spec[key], value)) {
            error = makeProblem("invalid_value", std::string("tileset.") + key, std::string(key) + " must be [x, y]", "pass two numbers");
            return false;
        }
        x = value.x;
        y = value.y;
        return true;
    };
    double offX = 0.0, offY = 0.0, padX = 0.0, padY = 0.0, tileX = 0.0, tileY = 0.0;
    bool hasOff = false, hasPad = false, hasTile = false;
    if (!vec2Of("offset", offX, offY, hasOff) || !vec2Of("padding", padX, padY, hasPad) || !vec2Of("tile_size", tileX, tileY, hasTile)) return false;
    if (static_cast<int>(offX) != static_cast<int>(offY) || static_cast<int>(padX) != static_cast<int>(padY)) {
        error = makeProblem("invalid_value", "tileset.offset", "a .tileset slices with one margin and one spacing, not per axis", "use the same offset and padding on both axes");
        return false;
    }
    const int cols = intArg(spec, "columns", 0), rows = intArg(spec, "rows", 0);
    tm::TextureSlicing slicing;
    slicing.margin = static_cast<int>(offX);
    slicing.spacing = static_cast<int>(padX);
    slicing.skipEmpty = false;   // like the legacy slice: every cell of the grid is a tile
    if (hasTile && tileX > 0.0 && tileY > 0.0) {
        slicing.tileWidth = static_cast<int>(tileX);
        slicing.tileHeight = static_cast<int>(tileY);
    } else if (cols > 0 && rows > 0) {
        slicing.tileWidth = (w - slicing.margin - slicing.spacing * (cols - 1)) / cols;
        slicing.tileHeight = (h - slicing.margin - slicing.spacing * (rows - 1)) / rows;
    } else {
        error = makeProblem("invalid_value", "tileset", "slice with tile_size [w, h] or columns and rows", "add tile_size");
        return false;
    }
    if (slicing.tileWidth < 1 || slicing.tileHeight < 1) {
        error = makeProblem("invalid_value", "tileset", "the slice grid is empty: the texture is " + std::to_string(w) + "x" + std::to_string(h) + " pixels", "use a smaller tile_size");
        return false;
    }
    out.isSlice = true;
    out.path = tm::tilesetPathForTexture(textureStored);
    out.absolute = e.project->resolveAssetPath(out.path);
    auto data = std::make_shared<TilesetData>();
    bool reused = false;
    std::error_code ec;
    if (fs::exists(out.absolute, ec)) {
        std::ifstream file(out.absolute, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        std::vector<TilesetProblem> problems;
        if (tileset::parse(text.str(), *data, problems)) {
            for (const TilesetSource& source : data->sources) {
                if (source.kind == TilesetSourceKind::Texture && source.asset.path == textureStored) {
                    tm::ResliceReport report;
                    if (!tm::resliceTexture(*data, source.id, slicing, *image, report, error)) return false;
                    reused = true;
                    break;
                }
            }
        }
    }
    if (!reused) {
        *data = TilesetData{};
        data->name = fs::path(textureStored).stem().generic_string();
        if (!tm::createFromTexture(data->name, AssetRef(textureStored), slicing, *image, *data, error)) return false;
    }
    // keep the first `count` tiles of the grid, as the legacy slice's count does
    if (hasArg(spec, "count") && spec["count"].is_number()) {
        const size_t count = static_cast<size_t>(std::max(0, spec["count"].get<int>()));
        while (data->tiles.size() > count) data->removeTile(data->tiles.back().id);
    }
    const std::string prefix = hasArg(spec, "prefix") && spec["prefix"].is_string() ? spec["prefix"].get<std::string>() : std::string("tile_");
    const Json names = hasArg(spec, "names") && spec["names"].is_array() ? spec["names"] : Json::array();
    for (size_t i = 0; i < data->tiles.size(); i++) {
        data->tiles[i].name = i < names.size() && names[i].is_string() ? names[i].get<std::string>() : prefix + std::to_string(i);
    }
    out.data = std::move(data);
    return true;
}

// resolves `tileset`: a slice (needs `texture`) or a .tileset path. Returns false after failing
bool resolveTilesetArg(GridEdit& e, TilesetArg& out, Problem& error) {
    const Json& args = e.args;
    std::string textureStored;
    std::string assetError;
    if (hasArg(args, "texture")) {
        if (!args["texture"].is_string() || !McpValues::assetPathFromJson(e.project, args["texture"].get<std::string>(), textureStored, assetError)) {
            error = makeProblem("tileset_missing", "texture", "texture: " + assetError, "pass a project-relative image path");
            return false;
        }
    }
    if (!hasArg(args, "tileset")) {
        if (textureStored.empty()) return true;
        // a texture alone slices by the Grid's cell size in pixels; a create without cell_size by
        // the default tile size
        out.given = true;
        const bool cellKnown = !e.createGrid || hasArg(args, "cell_size");
        const double ppu = cellKnown ? projectPixelsPerUnit() : 1.0;
        Json slice = {{"tile_size", Json::array({std::lround(e.spec.cellWidth * ppu), std::lround(e.spec.cellHeight * ppu)})}};
        return sliceTileset(e, slice, textureStored, out, error);
    }
    out.given = true;
    const Json& value = args["tileset"];
    if (value.is_object()) return sliceTileset(e, value, textureStored, out, error);
    if (!value.is_string()) {
        error = makeProblem("invalid_value", "tileset", "tileset is a .tileset path or a slice object", "see " + std::string(kGridTypedDoc));
        return false;
    }
    if (!McpValues::assetPathFromJson(e.project, value.get<std::string>(), out.path, assetError)) {
        error = makeProblem("tileset_missing", "tileset", assetError, "pass a project-relative .tileset path");
        return false;
    }
    if (!TilesetPool::get(out.path)) {
        const std::string why = TilesetPool::lastError(out.path);
        error = makeProblem("tileset_missing", "tileset", "can't load \"" + value.get<std::string>() + "\"" + (why.empty() ? "" : ": " + why),
                            "create it with edit_tileset, or pass an existing .tileset path");
        return false;
    }
    return true;
}

// ---- commit: everything the call does to the scene, in one undo step

bool commitEdit(GridEdit& e, const TilesetArg& tilesetArg, Problem& error) {
    CommandHistory* history = CommandHandle::get(e.sceneProject->id);
    const size_t mark = history->mark();
    const uint32_t sceneId = e.sceneProject->id;
    const auto rollback = [&]() {
        while (history->position() > mark && history->canUndo() && history->undo()) {}
        history->freeRedo();
    };
    const auto add = [&](Command* command, const char* what) {
        if (history->addCommandNoMerge(command)) return true;
        error = makeProblem("tileset_missing", "", std::string("the editor could not ") + what, "check the project's files, then try again");
        rollback();
        return false;
    };

    // the tileset file (one step with the scene edits)
    if (tilesetArg.given && tilesetArg.isSlice && tilesetArg.data) {
        Project* project = e.project;
        auto writeCommand = std::make_unique<WriteFilesCmd>(
            std::vector<WriteFilesCmd::Item>{tm::tilesetFileItem(project, tilesetArg.absolute, *tilesetArg.data)},
            [project](const std::vector<fs::path>& changed) {
                ProjectFileWrites::filesWritten(project, changed);
                for (const fs::path& path : changed) TilesetPool::invalidate(path.generic_string());
            });
        writeCommand->setName("Create Tileset " + tilesetArg.absolute.filename().generic_string());
        if (!add(writeCommand.release(), "write the tileset file")) return false;
        // the pool is keyed by the stored path: the call's document is what later reads see
        TilesetPool::set(tilesetArg.path, tilesetArg.data);
    }

    if (e.createGrid) {
        const Json& create = e.args["create"];
        const std::string name = hasArg(create, "name") && create["name"].is_string() ? create["name"].get<std::string>() : std::string("Grid");
        Entity parent = NULL_ENTITY;
        if (hasArg(create, "parent")) {
            std::string refError;
            parent = McpRefs::resolveEntity(e.sceneProject, create["parent"], refError);
            if (parent == NULL_ENTITY) {
                rollback();
                error = makeProblem("unknown_layer", "create.parent", refError, "give an existing GameObject as parent");
                return false;
            }
        }
        auto* gridCmd = parent != NULL_ENTITY ? new CreateEntityCmd(e.project, sceneId, name, EntityCreationType::TILEMAP_GRID, parent)
                                              : new CreateEntityCmd(e.project, sceneId, name, EntityCreationType::TILEMAP_GRID);
        if (hasArg(create, "position")) {
            Vector3 position;
            if (!McpValues::parseVector3(create["position"], position)) {
                delete gridCmd;
                rollback();
                error = makeProblem("invalid_value", "create.position", "position must be [x, y, z]", "pass three numbers");
                return false;
            }
            gridCmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
        }
        if (!add(gridCmd, "create the Grid")) return false;
        e.gridEntity = gridCmd->getEntity();
        auto* gridProps = new MultiPropertyCmd();
        gridProps->addPropertyCmd<TileGridLayout>(e.project, sceneId, e.gridEntity, ComponentType::GridComponent, "layout", e.spec.layout);
        gridProps->addPropertyCmd<Vector2>(e.project, sceneId, e.gridEntity, ComponentType::GridComponent, "cellSize",
                                           Vector2(static_cast<float>(e.spec.cellWidth), static_cast<float>(e.spec.cellHeight)));
        if (!add(gridProps, "set the Grid")) return false;
    }

    // layers the call adds, then the tilesets assigned, then the cells in one command
    int nextOrder = 0;
    for (const LayerSlot& slot : e.slots) {
        if (slot.entity != NULL_ENTITY) nextOrder = std::max(nextOrder, (int)tilemapmesh::sortingOrder(e.scene, slot.entity) + 1);
    }
    std::vector<tm::TilemapCellsCmd::LayerChanges> changes;
    for (LayerSlot& slot : e.slots) {
        bool isNew = false;
        if (slot.entity == NULL_ENTITY) {
            auto* layerCmd = new CreateEntityCmd(e.project, sceneId, slot.name, EntityCreationType::TILEMAP_LAYER, e.gridEntity);
            if (!add(layerCmd, "create the layer")) return false;
            slot.entity = layerCmd->getEntity();
            isNew = true;
        }
        auto* props = new MultiPropertyCmd();
        bool any = false;
        if (slot.assignTileset) {
            props->addPropertyCmd<std::string>(e.project, sceneId, slot.entity, ComponentType::TilemapLayerComponent, "tileset", slot.tilesetPath);
            any = true;
        }
        if (slot.setDualGrid) {
            props->addPropertyCmd<bool>(e.project, sceneId, slot.entity, ComponentType::TilemapLayerComponent, "dualGrid", true);
            any = true;
        }
        if (isNew && nextOrder > 0) {
            props->addPropertyCmd<int>(e.project, sceneId, slot.entity, ComponentType::MeshComponent, "sortingOrder", nextOrder++);
            any = true;
        } else if (isNew) {
            nextOrder++;
        }
        if (any) {
            if (!add(props, "set the layer")) return false;
        } else {
            delete props;
        }
        std::vector<TileCellChange> list = slot.edit.patch.changes();
        if (!list.empty()) {
            tm::TilemapCellsCmd::LayerChanges item;
            item.layer = slot.entity;
            item.changes = std::move(list);
            changes.push_back(std::move(item));
        }
    }
    if (!changes.empty() && !add(new tm::TilemapCellsCmd(e.project, sceneId, std::move(changes), "Edit Tilemap"), "write the cells")) return false;
    history->groupSince(mark);
    return true;
}

// the Grid + layer picture of a Grid or layer GameObject; false after failing. `allowNoLayer`: a
// Grid without layers is kept (edit_tilemap's add_layer op gives it one)
bool resolveGridTarget(GridEdit& e, Entity entity, Problem& error, bool allowNoLayer = false) {
    Scene* scene = e.scene;
    Entity grid = gridAbove(scene, entity);
    e.gridEntity = grid;
    if (grid != NULL_ENTITY) {
        const GridComponent& gridComponent = scene->getComponent<GridComponent>(grid);
        e.spec = gridSpecOf(gridComponent);
        e.linked = gridComponent.linked;
    }
    std::vector<Entity> layers = grid != NULL_ENTITY ? gridLayers(e.sceneProject, grid) : std::vector<Entity>{entity};
    if (layers.empty() && !allowNoLayer) {
        error = makeProblem("unknown_layer", "entity", "\"" + scene->getEntityName(entity) + "\" has no Tilemap Layer",
                            "add one: edit_tilemap ops [{\"op\": \"add_layer\"}]");
        return false;
    }
    for (Entity layer : layers) {
        const TilemapLayerComponent& component = scene->getComponent<TilemapLayerComponent>(layer);
        LayerSlot slot;
        slot.entity = layer;
        slot.name = scene->getEntityName(layer);
        slot.tilesetPath = component.tileset.path;
        slot.tileset = runtimeOf(slot.tilesetPath);
        slot.dualGrid = component.dualGrid;
        slot.seed = component.seed;
        e.slots.push_back(std::move(slot));
    }
    e.specFromGrid = grid != NULL_ENTITY;
    if (grid == NULL_ENTITY && !e.slots.empty()) {
        const std::shared_ptr<const TilesetRuntime>& first = e.slots.front().tileset;
        e.spec = defaultSpecFor(first ? first->data.get() : nullptr);
    }
    // the layer the top-level arguments name: the default is the given layer GameObject, else the first
    e.chosen = 0;
    for (size_t i = 0; i < layers.size(); i++) {
        if (layers[i] == entity) e.chosen = static_cast<int>(i);
    }
    return error.kind.empty();
}

// picks the slot of `value` (name or index), including layers this call added
bool pickSlot(GridEdit& e, const Json& value, int& out, Problem& error, const std::string& field) {
    if (value.is_number_integer()) {
        const int index = value.get<int>();
        if (index >= 0 && index < static_cast<int>(e.slots.size())) {
            out = index;
            return true;
        }
    } else if (value.is_string()) {
        const std::string name = value.get<std::string>();
        for (size_t i = 0; i < e.slots.size(); i++) {
            if (e.slots[i].name == name) {
                out = static_cast<int>(i);
                return true;
            }
        }
    }
    std::string names;
    for (const LayerSlot& slot : e.slots) names += (names.empty() ? "" : ", ") + slot.name;
    error = makeProblem("unknown_layer", field, "no layer " + value.dump() + " on this Grid", names.empty() ? "add one with the add_layer op" : "use a name or index of: " + names);
    return false;
}

// R3: the tileset a new layer starts with: its own, else the chosen layer's
void addLayerSlot(GridEdit& e, const Json& op) {
    LayerSlot slot;
    slot.name = hasArg(op, "name") && op["name"].is_string() ? op["name"].get<std::string>() : "Layer " + std::to_string(e.slots.size());
    slot.opened = true;
    if (hasArg(op, "tileset") && op["tileset"].is_string()) {
        std::string stored, assetError;
        if (McpValues::assetPathFromJson(e.project, op["tileset"].get<std::string>(), stored, assetError)) {
            slot.tilesetPath = stored;
            slot.assignTileset = true;
            slot.tileset = runtimeOf(stored);
        }
    } else if (!e.slots.empty()) {
        const LayerSlot& chosen = e.slots[static_cast<size_t>(e.chosen)];
        slot.tilesetPath = chosen.tilesetPath;
        slot.assignTileset = !slot.tilesetPath.empty();
        slot.tileset = chosen.tileset;
    } else if (e.firstLayerTileset) {
        // a Grid with no layer yet: the call's top-level tileset
        slot.tilesetPath = e.firstLayerTilesetPath;
        slot.assignTileset = true;
        slot.tileset = e.firstLayerTileset;
    }
    slot.dualGrid = flagArg(op, "dual_grid", slot.tileset && slot.tileset->data && !slot.tileset->data->terrainSets.empty() &&
                                                 slot.tileset->data->terrainSets[0].mode == TerrainMode::DualGrid);
    slot.setDualGrid = slot.dualGrid;
    e.slots.push_back(std::move(slot));
}

Json slotEntityJson(GridEdit& e, const LayerSlot& slot) {
    return slot.entity != NULL_ENTITY ? McpRefs::entityJson(e.sceneProject, slot.entity) : Json{{"id", nullptr}, {"name", slot.name}};
}

Json legacyRects(const TilesetData& data) {
    Json rects = Json::array();
    for (const TileDef& def : data.tiles) {
        rects.push_back({{"index", def.id}, {"name", data.tileName(def)}, {"rect", {def.atX, def.atY}}, {"submesh", def.source}});
    }
    return rects;
}

// R75's result; the `grid` key holds the Grid's GameObject fields and the ASCII view of the touched
// region, so old readers of grid.lines and new readers of grid.id both find what they expect
Json editResult(GridEdit& e, CellRect touched, const Json& ascii, bool created) {
    LayerSlot& slot = e.slots[static_cast<size_t>(e.chosen)];
    const TilesetData* data = slot.tileset ? slot.tileset->data.get() : nullptr;
    const Json layerJson = slotEntityJson(e, slot);
    size_t changedCells = 0;
    bool noTile = false;
    std::set<ChunkCoord> dirty;
    for (LayerSlot& layer : e.slots) {
        for (const TileCellChange& change : layer.edit.patch.changes()) {
            changedCells++;
            dirty.insert(layer.cells.chunkOf(change.cell));
        }
        if (!layer.edit.terrain.noTile.empty()) {
            noTile = true;
            e.warnings.push_back("no_tile_for_terrain: " + std::to_string(layer.edit.terrain.noTile.size()) +
                                 " cell(s) of \"" + layer.name + "\" have a terrain but no tile matches them (edit_tileset check)");
        }
    }
    Json grid = e.gridEntity != NULL_ENTITY ? McpRefs::entityJson(e.sceneProject, e.gridEntity) : Json{{"id", nullptr}, {"name", "Grid"}};
    for (auto it = ascii.begin(); it != ascii.end(); ++it) grid[it.key()] = it.value();
    Json result = {{"kind", "grid"},
                  {"grid", grid},
                  {"tilemap", layerJson},
                  {"layer", layerJson},
                  {"created", created},
                  {"tile_rects", data ? static_cast<int>(data->tiles.size()) : 0},
                  {"tiles", slot.cells.cellCount()},
                  {"painted", e.painted},
                  {"erased", e.erased},
                  {"changed_cells", changedCells},
                  {"dirty_chunks", dirty.size()},
                  {"warnings", e.warnings},
                  {"preview", e.preview}};
    result["layer"]["index"] = e.chosen;
    if (data && hasArg(e.args, "tileset")) result["rects"] = legacyRects(*data);
    if (!touched.isEmpty()) result["region"] = {touched.minX, touched.minY, touched.maxX, touched.maxY};
    if (e.preview) {
        result["next"] = "A preview: nothing was written. Call again without preview to apply it.";
    } else if (noTile) {
        result["next"] = "Check the terrains: edit_tileset {path, check: true} lists the masks without a tile (tool_groups: tilemaps).";
    }
    return result;
}

} // namespace

// The route: a Grid, a layer, a `create` without the legacy-only arguments, or a `convert` on a
// Free Tilemap. Legacy-only arguments on a create keep making a Free Tilemap: rects can be any
// pixel rectangle, which no .tileset source can hold
bool editTilemapGrid(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string refError;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), refError);
    if (!sceneProject || !sceneProject->scene) return false;
    Scene* scene = sceneProject->scene;

    const bool wantsOps = hasArg(args, "ops") && args["ops"].is_array();
    const auto opKind = [&](size_t i) { return lowered(args["ops"][i].is_object() && args["ops"][i].contains("op") && args["ops"][i]["op"].is_string() ? args["ops"][i]["op"].get<std::string>() : ""); };
    bool convertFirst = false, newVocabulary = false;
    if (wantsOps) {
        for (size_t i = 0; i < args["ops"].size(); i++) {
            const std::string kind = opKind(i);
            auto it = knownOps().find(kind);
            if (it != knownOps().end() && it->second) newVocabulary = true;
            if (i == 0 && kind == "convert") convertFirst = true;
        }
    }
    const bool newArgs = hasArg(args, "layer") || hasArg(args, "layout") || hasArg(args, "preview") || hasArg(args, "image");
    const bool legacyOnly = hasArg(args, "rects") || hasArg(args, "replace_rects") || hasArg(args, "origin");

    Entity entity = NULL_ENTITY;
    if (hasArg(args, "entity")) {
        entity = McpRefs::resolveEntity(sceneProject, args["entity"], refError);
        if (entity == NULL_ENTITY) return false;
        const bool isGrid = scene->findComponent<GridComponent>(entity) || scene->findComponent<TilemapLayerComponent>(entity);
        if ((isGrid || (convertFirst && scene->findComponent<TilemapComponent>(entity))) && legacyOnly) {
            // a Grid's tiles are its .tileset's: these fields would be dropped without a word
            const char* field = hasArg(args, "rects") ? "rects" : hasArg(args, "replace_rects") ? "replace_rects" : "origin";
            failTyped(call, makeProblem("invalid_value", field, std::string(field) + " is a Free Tilemap field; a Grid's tiles come from its .tileset",
                                        "drop it: add tiles with edit_tileset (tool_groups: tilemaps), or pass tileset as a slice object"));
            return true;
        }
        if (!isGrid) {
            if (!scene->findComponent<TilemapComponent>(entity)) return false;
            if (!convertFirst) {
                if (newVocabulary || newArgs || (hasArg(args, "tileset") && args["tileset"].is_string())) {
                    failTyped(call, makeProblem("layout_mismatch", "entity", "\"" + scene->getEntityName(entity) + "\" is a Free Tilemap: this argument or op needs a Grid",
                                                "convert it first: ops [{\"op\": \"convert\"}]"));
                    return true;
                }
                return false;
            }
        }
    } else if (hasArg(args, "create")) {
        if (legacyOnly) return false;
    } else {
        return false;
    }

    GridEdit e(call);
    e.preview = flagArg(args, "preview");
    e.sceneProject = sceneProject;
    e.scene = scene;
    if (sceneProject->playState != ScenePlayState::STOPPED && !e.preview) {
        failTyped(call, makeProblem("play_mode", "scene", "Scene " + std::to_string(sceneProject->id) + " is playing", "stop play mode, or pass preview: true"));
        return true;
    }

    Problem error;
    const bool wantImage = flagArg(args, "image") || e.preview;

    // ---- a legacy Free Tilemap becomes a Grid and a layer first (S50 R72). The conversion is
    // its own step; until the ops commit, any failure undoes it too, so the call stays all or nothing
    bool converted = false;
    std::vector<std::string> convertWarnings;
    struct ConvertRollback {
        CommandHistory* history = nullptr;
        size_t mark = 0;
        bool armed = false;
        ~ConvertRollback() {
            if (!armed) return;
            while (history->position() > mark && history->canUndo() && history->undo()) {}
            // a refused call leaves no step behind, not even one to redo
            history->freeRedo();
        }
    } convertRollback;
    if (hasArg(args, "entity") && convertFirst && !scene->findComponent<GridComponent>(entity) && !scene->findComponent<TilemapLayerComponent>(entity)) {
        if (e.preview) {
            const TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(entity);
            const tm::ConvertCheck check = tm::check(tilemap, tilemapmesh::submeshTextures(scene, entity));
            if (!check.ok) {
                failTyped(call, check.problem);
                return true;
            }
            call->ok("Convert preview: " + std::to_string(check.cells.size()) + " cells, tile " + std::to_string(check.tileWidth) + "x" + std::to_string(check.tileHeight) + ".",
                     Json{{"preview", true}, {"converted", false}, {"cells", check.cells.size()}, {"warnings", check.warnings}});
            return true;
        }
        convertRollback.history = CommandHandle::get(sceneProject->id);
        convertRollback.mark = convertRollback.history->mark();
        const tm::ConvertResult converting = tm::convert(project, sceneProject->id, entity);
        if (!converting.ok) {
            failTyped(call, converting.problem);
            return true;
        }
        convertRollback.armed = true;
        converted = true;
        convertWarnings = converting.warnings;
    }
    Json ops = wantsOps ? args["ops"] : Json::array();
    if (converted && !ops.empty()) ops.erase(static_cast<size_t>(0));   // convert ran; the other ops go on the new layer

    // ---- the target
    const bool createNew = !hasArg(args, "entity");
    if (createNew) {
        e.createGrid = true;
        if (hasArg(args, "layout")) {
            const std::string layout = args["layout"].is_string() ? lowered(args["layout"].get<std::string>()) : "";
            if (layout == "rectangle") e.spec.layout = TileGridLayout::Rectangle;
            else if (layout == "isometric") e.spec.layout = TileGridLayout::Isometric;
            else if (layout == "isometric_staggered") e.spec.layout = TileGridLayout::IsometricStaggered;
            else if (layout == "hexagon") e.spec.layout = TileGridLayout::Hexagon;
            else {
                failTyped(call, makeProblem("layout_mismatch", "layout", "unknown layout " + args["layout"].dump(), "use rectangle, isometric, isometric_staggered or hexagon"));
                return true;
            }
        }
    } else {
        if (!resolveGridTarget(e, entity, error, true)) {
            failTyped(call, error);
            return true;
        }
        if (e.linked) {
            failTyped(call, tm::lockedLinkedError());
            return true;
        }
    }
    if (hasArg(args, "cell_size")) {
        Vector2 size;
        if (!McpValues::parseVector2(args["cell_size"], size) || size.x <= 0.0f || size.y <= 0.0f) {
            failTyped(call, makeProblem("invalid_value", "cell_size", "cell_size must be [width, height] > 0", "pass two positive numbers"));
            return true;
        }
        if (createNew) {
            e.spec.cellWidth = size.x;
            e.spec.cellHeight = size.y;
        } else if (static_cast<float>(e.spec.cellWidth) != size.x || static_cast<float>(e.spec.cellHeight) != size.y) {
            failTyped(call, makeProblem("layout_mismatch", "cell_size", "the Grid's cell size is [" + std::to_string(e.spec.cellWidth) + ", " + std::to_string(e.spec.cellHeight) + "]",
                                        "omit cell_size, or set the Grid's cellSize with set_properties"));
            return true;
        }
    }
    if (!createNew && hasArg(args, "layout")) {
        failTyped(call, makeProblem("layout_mismatch", "layout", "layout applies with create; this Grid already has one", "edit the Grid's layout with set_properties"));
        return true;
    }

    // ---- the tileset (a slice object writes <texture>.tileset at commit)
    TilesetArg tilesetArg;
    if (!resolveTilesetArg(e, tilesetArg, error)) {
        failTyped(call, error);
        return true;
    }
    std::shared_ptr<const TilesetRuntime> tilesetRuntime;
    if (tilesetArg.given) {
        tilesetRuntime = tilesetArg.isSlice ? TilesetPool::resolve(tilesetArg.path, tilesetArg.data) : TilesetPool::get(tilesetArg.path);
        if (createNew && tilesetRuntime && tilesetRuntime->data && !hasArg(args, "cell_size")) {
            e.spec.cellWidth = tilesetRuntime->data->tileWidth / projectPixelsPerUnit();
            e.spec.cellHeight = tilesetRuntime->data->tileHeight / projectPixelsPerUnit();
        }
    }
    if (createNew) {
        LayerSlot slot;
        slot.name = "Tilemap";
        slot.opened = true;
        if (tilesetRuntime) {
            slot.tilesetPath = tilesetArg.path;
            slot.assignTileset = true;
            slot.tileset = tilesetRuntime;
            if (tilesetRuntime->data && !tilesetRuntime->data->terrainSets.empty() && tilesetRuntime->data->terrainSets[0].mode == TerrainMode::DualGrid) {
                slot.dualGrid = true;
                slot.setDualGrid = true;
            }
        }
        e.slots.push_back(std::move(slot));
        e.chosen = 0;
    } else if (hasArg(args, "layer")) {
        if (!pickSlot(e, args["layer"], e.chosen, error, "layer")) {
            failTyped(call, error);
            return true;
        }
    }
    if (tilesetRuntime && !createNew) {
        if (e.slots.empty()) {
            e.firstLayerTileset = tilesetRuntime;
            e.firstLayerTilesetPath = tilesetArg.path;
        } else {
            LayerSlot& slot = e.slots[static_cast<size_t>(e.chosen)];
            slot.tileset = tilesetRuntime;
            slot.tilesetPath = tilesetArg.path;
            slot.assignTileset = true;
        }
    }
    const std::string targetName = entity != NULL_ENTITY ? "\"" + scene->getEntityName(entity) + "\"" : std::string("the Grid");
    const Problem noLayer = makeProblem("unknown_layer", "entity", targetName + " has no Tilemap Layer to edit", "add one first: ops [{\"op\": \"add_layer\"}, ...]");

    // ---- run the ops on private copies; any failure just stops here, nothing was written
    for (size_t i = 0; i < ops.size(); i++) {
        const std::string where = "ops[" + std::to_string(i) + "]";
        const Json& op = ops[i];
        if (!op.is_object()) {
            failTyped(call, makeProblem("invalid_value", where, "an op is an object {op, ...}", "see " + std::string(kGridTypedDoc)));
            return true;
        }
        const std::string kind = lowered(op.contains("op") && op["op"].is_string() ? op["op"].get<std::string>() : "");
        if (kind == "add_layer") {
            addLayerSlot(e, op);
            continue;
        }
        if (kind == "convert") {
            failTyped(call, makeProblem("invalid_value", where, "convert runs alone on a Free Tilemap, as the first op", "move it to the first op, on the legacy GameObject"));
            return true;
        }
        if (e.slots.empty()) {
            failTyped(call, noLayer);
            return true;
        }
        int slotIndex = e.chosen;
        if (hasArg(op, "layer") && !pickSlot(e, op["layer"], slotIndex, error, where + ".layer")) {
            failTyped(call, error);
            return true;
        }
        LayerSlot& slot = e.slots[static_cast<size_t>(slotIndex)];
        ensureOpened(e, slot);
        if (!runOp(e, slot, kind, op, where, error)) {
            failTyped(call, error, where);
            return true;
        }
    }
    if (e.slots.empty()) {
        failTyped(call, noLayer);
        return true;
    }
    for (LayerSlot& slot : e.slots) ensureOpened(e, slot);

    LayerSlot& chosenSlot = e.slots[static_cast<size_t>(e.chosen)];
    std::vector<TileCellChange> changesOfChosen = chosenSlot.edit.patch.changes();
    CellRect touched;
    for (const TileCellChange& change : changesOfChosen) touched.merge(change.cell);
    if (touched.isEmpty()) touched = chosenSlot.cells.usedRectWithTerrain();

    // ---- commit (nothing to undo on preview)
    if (!e.preview) {
        if (!commitEdit(e, tilesetArg, error)) {
            failTyped(call, error);
            return true;
        }
        if (converted) {
            convertRollback.armed = false;
            convertRollback.history->groupSince(convertRollback.mark, "Convert to Grid Tilemap");
        }
    }
    for (const std::string& warning : convertWarnings) e.warnings.push_back(warning);
    if (tilesetRuntime) {
        for (const std::string& warning : tilesetRuntime->warnings) e.warnings.push_back(warning);
    }

    const TilesetData* data = chosenSlot.tileset ? chosenSlot.tileset->data.get() : nullptr;
    const Json ascii = flagArg(args, "return_grid", true) ? asciiGrid(chosenSlot.cells, data, touched, intArg(args, "max_cells", kDefaultMaxCells), e.preferredChars, e.spec) : Json::object();
    const bool wasCreated = createNew;
    Json result = editResult(e, touched, ascii, wasCreated);
    if (converted) result["converted"] = true;

    ToolResult reply;
    const std::string name = chosenSlot.name;
    reply.text = std::string(e.preview ? "Preview only (nothing written). " : "") + "Layer \"" + name + "\": " +
                 std::to_string(chosenSlot.cells.cellCount()) + " cells, " + std::to_string(result["changed_cells"].get<size_t>()) + " changed" +
                 (e.preview ? "." : " (one undo step).");
    if (wantImage && !touched.isEmpty()) {
        ToolImage image;
        Json info;
        Problem imageError;
        const TilemapLayerComponent* layerComponent = chosenSlot.entity != NULL_ENTITY ? scene->findComponent<TilemapLayerComponent>(chosenSlot.entity) : nullptr;
        const double anchorX = layerComponent ? layerComponent->tileAnchor.x : 0.0;
        const double anchorY = layerComponent ? layerComponent->tileAnchor.y : 0.0;
        if (compositeImage(chosenSlot.cells, chosenSlot.tileset.get(), e.spec, touched, 1, anchorX, anchorY, chosenSlot.dualGrid, image, info, imageError)) {
            reply.images.push_back(std::move(image));
            result["image"] = info;
        } else {
            result["warnings"].push_back(imageError.message);
        }
    }
    reply.structured = result;
    call->reply(std::move(reply));
    return true;
}

// ---------------------------------------------------------------- get_tilemap

bool getTilemapGrid(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string refError;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), refError);
    if (!sceneProject || !sceneProject->scene || !hasArg(args, "entity")) return false;
    Entity entity = McpRefs::resolveEntity(sceneProject, args["entity"], refError);
    if (entity == NULL_ENTITY) return false;
    Scene* scene = sceneProject->scene;
    if (!scene->findComponent<GridComponent>(entity) && !scene->findComponent<TilemapLayerComponent>(entity)) return false;

    GridEdit e(call);
    e.sceneProject = sceneProject;
    e.scene = scene;
    Problem error;
    if (!resolveGridTarget(e, entity, error)) {
        failTyped(call, error);
        return true;
    }
    if (hasArg(args, "layer")) {
        if (!pickSlot(e, args["layer"], e.chosen, error, "layer")) {
            failTyped(call, error);
            return true;
        }
    }
    LayerSlot& slot = e.slots[static_cast<size_t>(e.chosen)];
    ensureOpened(e, slot);
    const TilesetData* data = slot.tileset ? slot.tileset->data.get() : nullptr;

    CellRect region = slot.cells.usedRectWithTerrain();
    if (hasArg(args, "region")) {
        const Json& given = args["region"];
        if (!given.is_array() || given.size() != 4 || !given[0].is_number() || !given[1].is_number() || !given[2].is_number() || !given[3].is_number()) {
            failTyped(call, makeProblem("invalid_value", "region", "region is [x0, y0, x1, y1], inclusive", "pass four integers"));
            return true;
        }
        region = CellRect::of(CellCoord{given[0].get<int32_t>(), given[1].get<int32_t>()}, CellCoord{given[2].get<int32_t>(), given[3].get<int32_t>()});
    }
    if (region.exceeds(MAX_TILE_REGION_CELLS)) {
        failTyped(call, makeProblem("region_too_large", "region", "the region has " + std::to_string(region.area()) + " cells (limit " + std::to_string(MAX_TILE_REGION_CELLS) + ")",
                                    "read a smaller region"));
        return true;
    }
    const int maxCells = intArg(args, "max_cells", kDefaultMaxCells);

    Json layers = Json::array();
    for (size_t i = 0; i < e.slots.size(); i++) {
        LayerSlot& layer = e.slots[i];
        const TilemapLayerComponent& component = scene->getComponent<TilemapLayerComponent>(layer.entity);
        const TileCells& cells = component.cells.read();
        const CellRect used = cells.usedRect();
        Json usedRect = used.isEmpty() ? Json(nullptr) : Json::array({used.minX, used.minY, used.maxX, used.maxY});
        layers.push_back({{"entity", McpRefs::entityJson(sceneProject, layer.entity)},
                          {"name", layer.name},
                          {"tileset", component.tileset.path.empty() ? std::string() : McpValues::assetPathToJson(project, component.tileset.path)},
                          {"sorting_layer", tilemapmesh::sortingLayer(scene, layer.entity)},
                          {"order_in_layer", tilemapmesh::sortingOrder(scene, layer.entity)},
                          {"cells", cells.cellCount()},
                          {"used_rect", usedRect},
                          {"dual_grid", component.dualGrid}});
    }

    const Json ascii = asciiGrid(slot.cells, data, region, maxCells, {}, e.spec);
    Json grid = e.gridEntity != NULL_ENTITY ? McpRefs::entityJson(sceneProject, e.gridEntity) : Json{{"id", nullptr}};
    for (auto it = ascii.begin(); it != ascii.end(); ++it) grid[it.key()] = it.value();

    std::string texture;
    if (data && !data->sources.empty() && data->sources.front().kind == TilesetSourceKind::Texture) {
        texture = McpValues::assetPathToJson(project, data->sources.front().asset.path);
    }
    static const char* const kLayoutNames[] = {"rectangle", "isometric", "isometric_staggered", "hexagon"};
    Json result = {{"grid", grid},
                   {"tilemap", McpRefs::entityJson(sceneProject, slot.entity)},
                   {"layer", McpRefs::entityJson(sceneProject, slot.entity)},
                   {"layers", layers},
                   {"layout", kLayoutNames[static_cast<int>(e.spec.layout)]},
                   {"cell_size", Json::array({e.spec.cellWidth, e.spec.cellHeight})},
                   {"texture", texture},
                   {"rects", data ? legacyRects(*data) : Json::array()},
                   {"tiles", slot.cells.cellCount()},
                   {"region", region.isEmpty() ? Json(nullptr) : Json::array({region.minX, region.minY, region.maxX, region.maxY})}};
    result["layer"]["index"] = e.chosen;
    if (!data && !slot.tilesetPath.empty()) {
        const std::string why = TilesetPool::lastError(slot.tilesetPath);
        result["warnings"] = Json::array({"tileset_missing: " + (why.empty() ? std::string("the tileset does not load") : why)});
    }

    if (flagArg(args, "include_terrain")) result["terrain"] = terrainGrid(slot.cells, data, region, maxCells);
    if (flagArg(args, "missing_tiles")) {
        Json missing = Json::array();
        size_t missingCount = 0;
        const int limit = intArg(args, "limit", 200);
        slot.cells.forEachCell(region, [&](CellCoord cell, uint32_t value, uint8_t) {
            if (cellValueEmpty(value)) return;
            const int32_t tile = cellValueTile(value);
            if (data && data->findTile(tile)) return;
            missingCount++;
            if (static_cast<int>(missing.size()) < limit) missing.push_back({cell.x, cell.y, tile});
        });
        result["missing_tiles"] = missing;
        result["missing_count"] = missingCount;
    }
    if (flagArg(args, "include_tiles")) {
        Json list = Json::array(), tileList = Json::array();
        const size_t limit = static_cast<size_t>(std::max(1, intArg(args, "limit", 200)));
        size_t index = 0;
        slot.cells.forEachCell(region, [&](CellCoord cell, uint32_t value, uint8_t) {
            if (cellValueEmpty(value) || list.size() >= limit) return;
            const int32_t tile = cellValueTile(value);
            const uint32_t flags = cellValueFlags(value) & (TILE_FLIP_X | TILE_FLIP_Y | TILE_TRANSPOSE);
            list.push_back({cell.x, cell.y, tile, flags});
            const TilePoint point = tilegrid::cellToLocal(e.spec, cell);
            tileList.push_back({{"index", index++}, {"rect", tile}, {"name", tileLegend(data, tile)}, {"position", {point.x, point.y}},
                                {"size", {e.spec.cellWidth, e.spec.cellHeight}}});
        });
        result["cells"] = list;
        result["tile_list"] = tileList;
    }

    ToolResult reply;
    reply.text = "Layer \"" + slot.name + "\": " + std::to_string(slot.cells.cellCount()) + " cells.";
    if (flagArg(args, "image") && !region.isEmpty()) {
        ToolImage image;
        Json info;
        Problem imageError;
        const TilemapLayerComponent& layer = scene->getComponent<TilemapLayerComponent>(slot.entity);
        const int scale = intArg(args, "scale", 1);
        if (!compositeImage(slot.cells, slot.tileset.get(), e.spec, region, scale, layer.tileAnchor.x, layer.tileAnchor.y, slot.dualGrid, image, info, imageError)) {
            failTyped(call, imageError);
            return true;
        }
        reply.images.push_back(std::move(image));
        result["image"] = info;
    }
    reply.structured = result;
    call->reply(std::move(reply));
    return true;
}

} // namespace tilemapgridtools

} // namespace jasperengine::editor::mcp
