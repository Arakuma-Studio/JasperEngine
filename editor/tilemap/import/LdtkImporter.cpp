// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The LDtk importer (docs/specs/S50-grid-tilemap.md R45, D11). A level's layer instances are
// listed top first in LDtk, back to front in the plan. Tiles stack in LDtk (several rules can
// paint a cell): one cell holds one tile here, so every tile past a cell's first goes to an
// overlay layer "<layer> (2)", "<layer> (3)", in the order LDtk draws them. Coordinates are y down
// and flip to y up with the layer's row count; a level's Grid sits at its world position.

#include "tilemap/import/LdtkImporter.h"

#include "tilemap/import/ImportJsonReader.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::editor::tilemapimport {

    namespace {

        using nlohmann::json;

        struct LdtkLayerCells {
            std::map<std::pair<int32_t, int32_t>, ImportCell> byPosition;   // keyed (y, x)
        };

        class LdtkParser {
        public:
            LdtkParser(const std::string& fileName, const FileReader& readFile, ImportPlan& plan): fileName(fileName), readFile(readFile), plan(plan) {}

            void run(std::string_view text) {
                detail::SourceContext ctx;
                ctx.file = fileName;
                plan.sourceFile = fileName;
                plan.format = "ldtk";
                const json root = detail::parseJsonDocument(text, ctx);
                if (!root.is_object()) ctx.fail("", "the document is not an LDtk project object");

                readDefinitions(root, ctx);
                defaultGridSize = detail::boundedInt(root, "defaultGridSize", 16, 1, 65536, ctx, "");

                std::vector<std::pair<const json*, std::string>> levels;
                if (const json* worlds = detail::findKey(root, "worlds"); worlds && worlds->is_array() && !worlds->empty()) {
                    for (size_t w = 0; w < worlds->size(); w++) {
                        const std::string worldPath = detail::indexPath("worlds", w);
                        const json* list = detail::findKey((*worlds)[w], "levels");
                        if (!list) continue;
                        collectLevels(*list, detail::keyPath(worldPath, "levels"), levels, ctx);
                    }
                } else {
                    const json* list = detail::findKey(root, "levels");
                    if (!list) ctx.fail("levels", "the project has no levels");
                    collectLevels(*list, "levels", levels, ctx);
                }
                for (const auto& [level, path] : levels) readLevel(*level, path, ctx);
                if (offGridTiles > 0) warn(std::to_string(offGridTiles) + " tiles are off their layer's grid and snap to it");
            }

        private:
            const std::string& fileName;
            const FileReader& readFile;
            ImportPlan& plan;
            int defaultGridSize = 16;

            std::map<int64_t, const json*> tilesetDefs;
            std::map<int64_t, const json*> layerDefs;
            std::map<int64_t, int> tilesetIndices;                // LDtk uid -> plan tileset
            std::map<std::pair<int64_t, int>, std::map<int, uint8_t>> terrainMaps;   // (layer uid, tileset) -> intgrid value -> terrain value
            std::map<int64_t, int> intGridTilesets;               // layer uid -> generated tileset
            int64_t totalCells = 0;

            void warn(std::string message) { plan.warnings.push_back(std::move(message)); }

            void collectLevels(const json& list, const std::string& path, std::vector<std::pair<const json*, std::string>>& out, const detail::SourceContext& ctx) const {
                if (!list.is_array()) ctx.fail(path, "expected an array of levels");
                for (size_t i = 0; i < list.size(); i++) {
                    if (!list[i].is_object()) ctx.fail(detail::indexPath(path, i), "expected a level object");
                    out.emplace_back(&list[i], detail::indexPath(path, i));
                }
            }

            void readDefinitions(const json& root, const detail::SourceContext& ctx) {
                const json* defs = detail::findKey(root, "defs");
                if (!defs || !defs->is_object()) ctx.fail("defs", "the project has no definitions");
                const auto index = [&](const char* key, std::map<int64_t, const json*>& out) {
                    const std::string path = detail::keyPath("defs", key);
                    const json* list = detail::findKey(*defs, key);
                    if (!list) return;
                    if (!list->is_array()) ctx.fail(path, "expected an array");
                    for (size_t i = 0; i < list->size(); i++) {
                        const std::string itemPath = detail::indexPath(path, i);
                        const json* uid = detail::findKey((*list)[i], "uid");
                        if (!uid) ctx.fail(detail::keyPath(itemPath, "uid"), "the definition has no uid");
                        out[detail::asInt(*uid, ctx, detail::keyPath(itemPath, "uid"))] = &(*list)[i];
                    }
                };
                index("tilesets", tilesetDefs);
                index("layers", layerDefs);
            }

            // ---- tilesets

            // The plan's tileset of an LDtk tileset uid, made on first use (unused tilesets make no files)
            int tilesetFor(int64_t uid, const std::string& usePath, const detail::SourceContext& ctx) {
                const auto found = tilesetIndices.find(uid);
                if (found != tilesetIndices.end()) return found->second;
                const auto def = tilesetDefs.find(uid);
                if (def == tilesetDefs.end()) ctx.fail(usePath, "names the tileset uid " + std::to_string(uid) + ", which the project does not define");
                const json& ts = *def->second;
                const std::string path = "defs.tilesets[uid " + std::to_string(uid) + "]";

                ImportTileset out;
                TilesetData& data = out.data;
                data.name = detail::getString(ts, "identifier", "tileset" + std::to_string(uid), ctx, path);
                const int grid = detail::requireInt(ts, "tileGridSize", 1, 65536, ctx, path);
                data.tileWidth = grid;
                data.tileHeight = grid;
                const int spacing = detail::boundedInt(ts, "spacing", 0, 0, 65536, ctx, path);
                const int padding = detail::boundedInt(ts, "padding", 0, 0, 65536, ctx, path);
                const int pxWid = detail::requireInt(ts, "pxWid", 0, 1 << 24, ctx, path);
                const int pxHei = detail::requireInt(ts, "pxHei", 0, 1 << 24, ctx, path);
                const int columns = detail::boundedInt(ts, "__cWid", std::max(0, (pxWid - 2 * padding + spacing) / (grid + spacing)), 0, 1 << 24, ctx, path);
                const int rows = detail::boundedInt(ts, "__cHei", std::max(0, (pxHei - 2 * padding + spacing) / (grid + spacing)), 0, 1 << 24, ctx, path);
                const int64_t count = static_cast<int64_t>(columns) * rows;
                if (count > detail::kMaxTilesetTiles) ctx.fail(path, std::to_string(count) + " tiles is too many for one tileset");

                TilesetSource source;
                source.id = 0;
                source.kind = TilesetSourceKind::Texture;
                source.tileWidth = grid;
                source.tileHeight = grid;
                source.margin = padding;
                source.spacing = spacing;
                source.skipEmpty = false;   // ids are positions (D11)
                const std::string relPath = detail::getString(ts, "relPath", "", ctx, path);
                if (relPath.empty()) warn("Tileset '" + data.name + "' has no image file (an embedded atlas)");
                source.asset = detail::joinMapPath(std::string(), relPath);
                out.images.push_back({0, source.asset.path, pxWid, pxHei});
                data.sources.push_back(std::move(source));

                data.tiles.reserve(static_cast<size_t>(count));
                for (int64_t id = 0; id < count; id++) {
                    TileDef tile;
                    tile.id = static_cast<int32_t>(id);
                    tile.source = 0;
                    tile.atX = static_cast<int>(id % columns);
                    tile.atY = static_cast<int>(id / columns);
                    data.tiles.push_back(std::move(tile));
                }
                data.nextId = static_cast<int32_t>(count);

                readTileMetadata(ts, path, data, ctx);

                const int index = static_cast<int>(plan.tilesets.size());
                plan.tilesets.push_back(std::move(out));
                tilesetIndices[uid] = index;
                return index;
            }

            void readTileMetadata(const json& ts, const std::string& path, TilesetData& data, const detail::SourceContext& ctx) {
                std::map<std::string, PropertySchemaEntry> schema;
                const auto tileAt = [&](int64_t id) -> TileDef* {
                    return id >= 0 && id < static_cast<int64_t>(data.tiles.size()) ? &data.tiles[static_cast<size_t>(id)] : nullptr;
                };
                if (const json* enumTags = detail::findKey(ts, "enumTags")) {
                    if (!enumTags->is_array()) ctx.fail(detail::keyPath(path, "enumTags"), "expected an array");
                    for (size_t i = 0; i < enumTags->size(); i++) {
                        const std::string tagPath = detail::indexPath(detail::keyPath(path, "enumTags"), i);
                        const std::string value = detail::getString((*enumTags)[i], "enumValueId", "", ctx, tagPath);
                        const json* tileIds = detail::findKey((*enumTags)[i], "tileIds");
                        if (value.empty() || !tileIds || !tileIds->is_array()) continue;
                        PropertySchemaEntry& entry = schema[value];
                        entry.name = value;
                        entry.type = TilePropertyType::Bool;
                        entry.defaultValue.type = TilePropertyType::Bool;
                        for (size_t t = 0; t < tileIds->size(); t++) {
                            TileDef* tile = tileAt(detail::asInt((*tileIds)[t], ctx, detail::indexPath(detail::keyPath(tagPath, "tileIds"), t)));
                            if (!tile) continue;
                            TilePropertyValue v;
                            v.type = TilePropertyType::Bool;
                            v.b = true;
                            tile->properties[value] = v;
                        }
                    }
                }
                if (const json* customData = detail::findKey(ts, "customData")) {
                    if (!customData->is_array()) ctx.fail(detail::keyPath(path, "customData"), "expected an array");
                    for (size_t i = 0; i < customData->size(); i++) {
                        const std::string dataPath = detail::indexPath(detail::keyPath(path, "customData"), i);
                        const int64_t tileId = detail::getInt((*customData)[i], "tileId", -1, ctx, dataPath);
                        const std::string text = detail::getString((*customData)[i], "data", "", ctx, dataPath);
                        TileDef* tile = tileAt(tileId);
                        if (!tile || text.empty()) continue;
                        PropertySchemaEntry& entry = schema["data"];
                        entry.name = "data";
                        entry.type = TilePropertyType::String;
                        entry.defaultValue.type = TilePropertyType::String;
                        TilePropertyValue v;
                        v.type = TilePropertyType::String;
                        v.s = text;
                        tile->properties["data"] = std::move(v);
                    }
                }
                for (auto& [name, entry] : schema) data.propertySchema.push_back(std::move(entry));
            }

            // The terrain set "IntGrid <layer>" of a layer's IntGrid values in a tileset, built once
            // per (layer, tileset), and the IntGrid value -> terrain plane value table
            const std::map<int, uint8_t>& terrainsFor(const json& instance, int64_t layerUid, int tilesetIndex, const std::string& path, const detail::SourceContext& ctx) {
                const std::pair<int64_t, int> key{layerUid, tilesetIndex};
                const auto found = terrainMaps.find(key);
                if (found != terrainMaps.end()) return found->second;

                std::map<int, uint8_t>& table = terrainMaps[key];
                TilesetData& data = plan.tilesets[static_cast<size_t>(tilesetIndex)].data;
                const auto def = layerDefs.find(layerUid);
                if (def == layerDefs.end()) ctx.fail(detail::keyPath(path, "layerDefUid"), "names a layer the project does not define");
                const json* values = detail::findKey(*def->second, "intGridValues");
                if (!values || !values->is_array()) return table;
                const std::string layerName = detail::getString(instance, "__identifier", "Layer", ctx, path);

                int terrainCount = 0;
                for (const TerrainSetDef& s : data.terrainSets) terrainCount += static_cast<int>(s.terrains.size());
                if (terrainCount + static_cast<int>(values->size()) > MAX_TILESET_TERRAINS) {
                    warn("The IntGrid values of layer '" + layerName + "' are skipped: tileset '" + data.name + "' would pass " + std::to_string(MAX_TILESET_TERRAINS) + " terrains");
                    return table;
                }

                TerrainSetDef set;
                set.name = "IntGrid " + layerName;
                set.mode = TerrainMode::Blob;
                const int base = terrainCount;
                for (size_t i = 0; i < values->size(); i++) {
                    const std::string valuePath = "defs.layers[uid " + std::to_string(layerUid) + "].intGridValues[" + std::to_string(i) + "]";
                    const json& entry = (*values)[i];
                    const int value = detail::requireInt(entry, "value", 1, 1 << 30, ctx, valuePath);
                    TerrainDef terrain;
                    terrain.name = detail::getString(entry, "identifier", "", ctx, valuePath);
                    if (terrain.name.empty()) terrain.name = "Value " + std::to_string(value);
                    terrain.color = detail::normalizeColor(detail::getString(entry, "color", "", ctx, valuePath));
                    set.terrains.push_back(std::move(terrain));
                    table[value] = static_cast<uint8_t>(base + static_cast<int>(i) + 1);
                }
                data.terrainSets.push_back(std::move(set));
                return table;
            }

            // ---- levels

            void readLevel(const json& inlineLevel, const std::string& inlinePath, const detail::SourceContext& ctx) {
                const json* level = &inlineLevel;
                std::string path = inlinePath;
                json external;
                detail::SourceContext levelCtx = ctx;

                const json* instances = detail::findKey(*level, "layerInstances");
                const std::string relPath = detail::getString(*level, "externalRelPath", "", ctx, path);
                if ((!instances || !instances->is_array()) && !relPath.empty()) {
                    std::string text;
                    const std::string file = detail::joinMapPath(std::string(), relPath);
                    if (!readFile || !readFile(file, text)) ctx.fail(detail::keyPath(path, "externalRelPath"), "cannot read the external level '" + file + "'");
                    detail::SourceContext externalCtx;
                    externalCtx.file = file;
                    external = detail::parseJsonDocument(text, externalCtx);
                    if (!external.is_object()) externalCtx.fail("", "the document is not a level object");
                    level = &external;
                    levelCtx = externalCtx;
                    path = std::string();
                    instances = detail::findKey(*level, "layerInstances");
                }
                if (!instances || !instances->is_array()) {
                    warn("Level '" + detail::getString(*level, "identifier", "", levelCtx, path) + "' has no layer instances and is skipped");
                    return;
                }

                const std::string levelName = detail::getString(*level, "identifier", "Level", levelCtx, path);
                const double worldX = detail::getDouble(*level, "worldX", 0.0, levelCtx, path);
                const double worldY = detail::getDouble(*level, "worldY", 0.0, levelCtx, path);
                const double pxHei = detail::getDouble(*level, "pxHei", 0.0, levelCtx, path);
                const std::string bg = detail::getString(*level, "bgRelPath", "", levelCtx, path);
                if (!bg.empty()) plan.skipped.push_back({"image_layer", levelName + " background"});

                // the world position of the level's bottom-left corner, y up; LDtk's linear layouts
                // leave worldX and worldY at -1
                const bool placed = !(worldX == -1.0 && worldY == -1.0);
                std::vector<std::pair<int, int>> gridOrder;   // (grid size, plan grid index)
                const auto gridFor = [&](int size) -> ImportGrid& {
                    for (const auto& [s, index] : gridOrder) {
                        if (s == size) return plan.grids[static_cast<size_t>(index)];
                    }
                    ImportGrid grid;
                    grid.name = gridOrder.empty() ? levelName : levelName + " @" + std::to_string(size);
                    grid.spec.layout = TileGridLayout::Rectangle;
                    grid.spec.cellWidth = size;
                    grid.spec.cellHeight = size;
                    grid.originX = placed ? worldX : 0.0;
                    grid.originY = placed ? -(worldY + pxHei) : 0.0;
                    gridOrder.emplace_back(size, static_cast<int>(plan.grids.size()));
                    plan.grids.push_back(std::move(grid));
                    return plan.grids.back();
                };

                // instances are listed top first: the plan's layers go back to front
                for (size_t reverse = instances->size(); reverse-- > 0;) {
                    const json& instance = (*instances)[reverse];
                    const std::string layerPath = detail::indexPath(detail::keyPath(path, "layerInstances"), reverse);
                    if (!instance.is_object()) levelCtx.fail(layerPath, "expected a layer instance");
                    readLayerInstance(instance, layerPath, levelName, gridFor, levelCtx);
                }
                if (gridOrder.size() > 1) warn("Level '" + levelName + "' mixes layer grid sizes: it makes one Grid per size");
                if (gridOrder.empty()) gridFor(defaultGridSize);
            }

            template<class GridFor>
            void readLayerInstance(const json& instance, const std::string& path, const std::string& levelName, GridFor&& gridFor, const detail::SourceContext& ctx) {
                const std::string name = detail::getString(instance, "__identifier", "", ctx, path);
                const std::string type = detail::getString(instance, "__type", "", ctx, path);
                if (type == "Entities") {
                    const json* entities = detail::findKey(instance, "entityInstances");
                    plan.skipped.push_back({"entities", levelName + "/" + name, entities && entities->is_array() ? static_cast<int>(entities->size()) : 0});
                    return;
                }
                if (type != "IntGrid" && type != "Tiles" && type != "AutoLayer") {
                    warn("Layer '" + levelName + "/" + name + "' has the unknown type '" + type + "' and is skipped");
                    return;
                }
                const int gridSize = detail::requireInt(instance, "__gridSize", 1, 65536, ctx, path);
                const int cWid = detail::requireInt(instance, "__cWid", 1, 1 << 24, ctx, path);
                const int cHei = detail::requireInt(instance, "__cHei", 1, 1 << 24, ctx, path);
                if (static_cast<int64_t>(cWid) * cHei > detail::kMaxLayerCells) ctx.fail(path, "the layer has too many cells");
                totalCells += static_cast<int64_t>(cWid) * cHei;
                if (totalCells > detail::kMaxMapCells) ctx.fail(path, "the project has too many cells");

                ImportLayer base;
                base.name = name;
                base.visible = detail::getBool(instance, "visible", true, ctx, path);
                base.opacity = static_cast<float>(std::clamp(detail::getDouble(instance, "__opacity", 1.0, ctx, path), 0.0, 1.0));
                base.offsetX = detail::getDouble(instance, "__pxTotalOffsetX", 0.0, ctx, path);
                base.offsetY = -detail::getDouble(instance, "__pxTotalOffsetY", 0.0, ctx, path);

                const int64_t layerUid = detail::getInt(instance, "layerDefUid", 0, ctx, path);
                if (const auto def = layerDefs.find(layerUid); def != layerDefs.end()) {
                    if (detail::getDouble(*def->second, "parallaxFactorX", 0.0, ctx, "") != 0.0 || detail::getDouble(*def->second, "parallaxFactorY", 0.0, ctx, "") != 0.0) {
                        plan.skipped.push_back({"parallax", levelName + "/" + name});
                    }
                }

                // the tiles' tileset: an override wins; an IntGrid layer without one has its generated tileset
                int tilesetIndex = -1;
                const json* overrideUid = detail::findKey(instance, "overrideTilesetUid");
                const json* tilesetUid = overrideUid ? overrideUid : detail::findKey(instance, "__tilesetDefUid");
                if (tilesetUid) tilesetIndex = tilesetFor(detail::asInt(*tilesetUid, ctx, detail::keyPath(path, "__tilesetDefUid")), detail::keyPath(path, "__tilesetDefUid"), ctx);

                const json* intGrid = detail::findKey(instance, "intGridCsv");
                bool hasIntGrid = false;
                if (type == "IntGrid" && intGrid && intGrid->is_array()) {
                    for (size_t i = 0; i < intGrid->size() && !hasIntGrid; i++) hasIntGrid = detail::asInt((*intGrid)[i], ctx, detail::indexPath(detail::keyPath(path, "intGridCsv"), i)) != 0;
                }
                if (hasIntGrid && tilesetIndex < 0) tilesetIndex = generatedIntGridTileset(instance, layerUid, name, gridSize, ctx, path);
                base.tileset = tilesetIndex;

                std::vector<LdtkLayerCells> depths(1);
                std::unordered_map<uint64_t, int> stackDepth;

                // the tiles
                const char* tilesKey = type == "Tiles" ? "gridTiles" : "autoLayerTiles";
                if (const json* tiles = detail::findKey(instance, tilesKey)) {
                    if (!tiles->is_array()) ctx.fail(detail::keyPath(path, tilesKey), "expected an array");
                    for (size_t i = 0; i < tiles->size(); i++) {
                        readTile((*tiles)[i], detail::indexPath(detail::keyPath(path, tilesKey), i), gridSize, cHei, tilesetIndex, depths, stackDepth, name, ctx);
                    }
                }

                // the terrain plane
                if (hasIntGrid) readIntGrid(instance, path, *intGrid, cWid, cHei, tilesetIndex, layerUid, depths[0], name, ctx);

                ImportGrid& grid = gridFor(gridSize);
                for (size_t depth = 0; depth < depths.size(); depth++) {
                    ImportLayer layer = base;
                    if (depth > 0) layer.name = name + " (" + std::to_string(depth + 1) + ")";
                    layer.name = uniqueName(grid, layer.name);
                    layer.cells.reserve(depths[depth].byPosition.size());
                    for (auto& [position, cell] : depths[depth].byPosition) layer.cells.push_back(cell);
                    grid.layers.push_back(std::move(layer));
                }
                if (depths.size() > 1) warn("Layer '" + levelName + "/" + name + "' stacks tiles: it makes " + std::to_string(depths.size()) + " layers");
            }

            int generatedIntGridTileset(const json& instance, int64_t layerUid, const std::string& name, int gridSize, const detail::SourceContext& ctx, const std::string& path) {
                const auto found = intGridTilesets.find(layerUid);
                if (found != intGridTilesets.end()) return found->second;
                ImportTileset out;
                out.data.name = "IntGrid " + detail::getString(instance, "__identifier", name, ctx, path);
                out.data.tileWidth = gridSize;
                out.data.tileHeight = gridSize;
                const int index = static_cast<int>(plan.tilesets.size());
                plan.tilesets.push_back(std::move(out));
                intGridTilesets[layerUid] = index;
                return index;
            }

            void readTile(const json& tile, const std::string& path, int gridSize, int cHei, int tilesetIndex, std::vector<LdtkLayerCells>& depths, std::unordered_map<uint64_t, int>& stackDepth, const std::string& layerName, const detail::SourceContext& ctx) {
                if (tilesetIndex < 0) ctx.fail(path, "the layer has tiles but no tileset");
                const TilesetData& data = plan.tilesets[static_cast<size_t>(tilesetIndex)].data;
                // a generated IntGrid tileset has no image to take tiles from
                if (data.sources.empty()) ctx.fail(path, "the layer has tiles but no tileset");
                const auto point = [&](const char* key, int64_t& x, int64_t& y) {
                    const json* p = detail::findKey(tile, key);
                    if (!p || !p->is_array() || p->size() != 2) ctx.fail(detail::keyPath(path, key), "expected [x, y]");
                    x = detail::asInt((*p)[0], ctx, detail::keyPath(path, key));
                    y = detail::asInt((*p)[1], ctx, detail::keyPath(path, key));
                    if (x < -detail::kMaxCoordinate || x > detail::kMaxCoordinate || y < -detail::kMaxCoordinate || y > detail::kMaxCoordinate) ctx.fail(detail::keyPath(path, key), "is out of range");
                };
                int64_t px = 0, py = 0, sx = 0, sy = 0;
                point("px", px, py);
                point("src", sx, sy);

                const auto floorDiv = [](int64_t a, int64_t b) { return a >= 0 ? a / b : (a - b + 1) / b; };
                if (px % gridSize != 0 || py % gridSize != 0) offGridTiles++;
                const int64_t cellX = floorDiv(px, gridSize);
                const int64_t cellY = floorDiv(py, gridSize);

                const TilesetSource& source = data.sources[0];
                const int stepX = data.tileWidth + source.spacing;
                const int stepY = data.tileHeight + source.spacing;
                const int64_t sourceX = sx - source.margin;
                const int64_t sourceY = sy - source.margin;
                const int columns = data.nextId > 0 && !data.tiles.empty() ? data.tiles.back().atX + 1 : 0;
                const int rows = data.nextId > 0 && !data.tiles.empty() ? data.tiles.back().atY + 1 : 0;
                if (sourceX < 0 || sourceY < 0 || sourceX % stepX != 0 || sourceY % stepY != 0 || sourceX / stepX >= columns || sourceY / stepY >= rows) {
                    warn("Layer '" + layerName + "': a tile's src (" + std::to_string(sx) + ", " + std::to_string(sy) + ") is outside tileset '" + data.name + "' and is skipped");
                    return;
                }
                const int64_t id = sourceY / stepY * columns + sourceX / stepX;

                const int64_t flip = detail::getInt(tile, "f", 0, ctx, path);
                uint32_t flags = 0;
                if (flip & 1) flags |= TILE_FLIP_X;
                if (flip & 2) flags |= TILE_FLIP_Y;

                const int32_t x = static_cast<int32_t>(cellX);
                const int32_t y = static_cast<int32_t>(static_cast<int64_t>(cHei) - 1 - cellY);
                const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
                int& depth = stackDepth[key];
                if (static_cast<size_t>(depth) >= depths.size()) depths.resize(static_cast<size_t>(depth) + 1);
                depths[static_cast<size_t>(depth)].byPosition[{y, x}] = ImportCell{x, y, makeCellValue(static_cast<int32_t>(id), flags), 0};
                depth++;
            }

            void readIntGrid(const json& instance, const std::string& path, const json& csv, int cWid, int cHei, int tilesetIndex, int64_t layerUid, LdtkLayerCells& base, const std::string& layerName, const detail::SourceContext& ctx) {
                const int64_t needed = static_cast<int64_t>(cWid) * cHei;
                if (static_cast<int64_t>(csv.size()) != needed) ctx.fail(detail::keyPath(path, "intGridCsv"), "has " + std::to_string(csv.size()) + " cells, the layer needs " + std::to_string(needed));
                const std::map<int, uint8_t>& table = terrainsFor(instance, layerUid, tilesetIndex, path, ctx);
                int64_t unknown = 0;
                for (int row = 0; row < cHei; row++) {
                    for (int col = 0; col < cWid; col++) {
                        const size_t index = static_cast<size_t>(row) * static_cast<size_t>(cWid) + static_cast<size_t>(col);
                        const int64_t value = detail::asInt(csv[index], ctx, detail::indexPath(detail::keyPath(path, "intGridCsv"), index));
                        if (value == 0) continue;
                        const auto terrain = value >= INT32_MIN && value <= INT32_MAX ? table.find(static_cast<int>(value)) : table.end();
                        if (terrain == table.end()) {
                            unknown++;
                            continue;
                        }
                        const int32_t x = col;
                        const int32_t y = cHei - 1 - row;
                        ImportCell& cell = base.byPosition[{y, x}];
                        cell.x = x;
                        cell.y = y;
                        cell.terrain = terrain->second;
                    }
                }
                if (unknown > 0) warn("Layer '" + layerName + "': " + std::to_string(unknown) + " IntGrid cells have values the layer does not define and get no terrain");
            }

            std::string uniqueName(const ImportGrid& grid, const std::string& name) const {
                const auto taken = [&](const std::string& candidate) {
                    return std::any_of(grid.layers.begin(), grid.layers.end(), [&](const ImportLayer& l) { return l.name == candidate; });
                };
                if (!taken(name)) return name;
                for (int i = 2;; i++) {
                    std::string candidate = name + " (" + std::to_string(i) + ")";
                    if (!taken(candidate)) return candidate;
                }
            }

            int64_t offGridTiles = 0;
        };

    }

    bool parseLdtk(std::string_view text, const std::string& fileName, const FileReader& readFile, ImportPlan& out, std::vector<ImportProblem>& problems) {
        out = ImportPlan();
        try {
            LdtkParser parser(fileName, readFile, out);
            parser.run(text);
            return true;
        } catch (detail::ParseFailure& failure) {
            problems.push_back(std::move(failure.problem));
        } catch (const std::exception& e) {
            ImportProblem p;
            p.message = std::string("the project could not be read: ") + e.what();
            p.file = fileName;
            problems.push_back(std::move(p));
        }
        return false;
    }

}
