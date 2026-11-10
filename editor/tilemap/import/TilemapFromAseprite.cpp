// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/import/TilemapFromAseprite.h"

#include "tilemap/import/ImportJsonReader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <utility>

namespace jasperengine::editor::tilemapimport {

    namespace {

        using nlohmann::json;
        using pixelart::aseprite::TilemapCelOutput;
        using pixelart::aseprite::TilesetOutput;

        bool asepriteColor(const std::string& text, Vector4& out) {
            unsigned r = 0, g = 0, b = 0, a = 255;
            if (text.size() != 9 || text[0] != '#' || std::sscanf(text.c_str() + 1, "%02x%02x%02x%02x", &r, &g, &b, &a) != 4) return false;
            out = Vector4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
            return true;
        }

        // Adds a user data value to the tile and its tileset's schema; the first tile with a name
        // decides its type (a tile with the other type keeps none, reported)
        void addUserProperty(TileDef& tile, const std::string& name, TilePropertyValue value, std::map<std::string, PropertySchemaEntry>& schema, const std::string& tilesetName, ImportPlan& plan) {
            auto [it, inserted] = schema.try_emplace(name);
            if (inserted) {
                it->second.name = name;
                it->second.type = value.type;
                it->second.defaultValue.type = value.type;
            } else if (it->second.type != value.type) {
                plan.warnings.push_back("Tileset '" + tilesetName + "': tile " + std::to_string(tile.id) + "'s user data '" + name + "' changes type and is skipped");
                return;
            }
            tile.properties[name] = std::move(value);
        }

        void readUserData(const json& userData, TileDef& tile, std::map<std::string, PropertySchemaEntry>& schema, const std::string& tilesetName, ImportPlan& plan) {
            if (!userData.is_object()) return;
            if (const json* text = detail::findKey(userData, "text"); text && text->is_string()) {
                TilePropertyValue v;
                v.type = TilePropertyType::String;
                v.s = text->get<std::string>();
                addUserProperty(tile, "text", std::move(v), schema, tilesetName, plan);
            }
            if (const json* color = detail::findKey(userData, "color"); color && color->is_string()) {
                TilePropertyValue v;
                v.type = TilePropertyType::Color;
                if (asepriteColor(color->get<std::string>(), v.color)) addUserProperty(tile, "color", std::move(v), schema, tilesetName, plan);
            }
            const json* properties = detail::findKey(userData, "properties");
            if (!properties || !properties->is_object()) return;
            for (const auto& [name, value] : properties->items()) {
                TilePropertyValue v;
                if (value.is_boolean()) {
                    v.type = TilePropertyType::Bool;
                    v.b = value.get<bool>();
                } else if (value.is_number_integer() || value.is_number_unsigned()) {
                    v.type = TilePropertyType::Int;
                    v.i = value.get<int64_t>();
                } else if (value.is_number_float()) {
                    v.type = TilePropertyType::Float;
                    v.f = value.get<double>();
                } else if (value.is_string()) {
                    v.type = TilePropertyType::String;
                    v.s = value.get<std::string>();
                } else {
                    plan.warnings.push_back("Tileset '" + tilesetName + "': tile " + std::to_string(tile.id) + "'s user data '" + name + "' is not a simple value and is skipped");
                    continue;
                }
                addUserProperty(tile, name, std::move(v), schema, tilesetName, plan);
            }
        }

        // Aseprite tile i >= 1 is our tile i - 1 (R12): tile 0 is Aseprite's empty tile
        void buildTileset(const TilesetOutput& ts, size_t index, const std::string& asepritePath, ImportPlan& plan) {
            ImportTileset out;
            TilesetData& data = out.data;
            data.name = ts.name.empty() ? "tileset" + std::to_string(index) : ts.name;
            data.tileWidth = ts.tileWidth;
            data.tileHeight = ts.tileHeight;
            TilesetSource source;
            source.id = 0;
            source.kind = TilesetSourceKind::Aseprite;
            source.asset = asepritePath;
            source.asepriteTileset = static_cast<int>(index);
            data.sources.push_back(std::move(source));
            std::map<std::string, PropertySchemaEntry> schema;
            const int count = ts.tileCount > 1 ? ts.tileCount - 1 : 0;
            data.tiles.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; i++) {
                TileDef tile;
                tile.id = i;
                tile.source = 0;
                tile.atX = i;
                tile.atY = 0;
                if (static_cast<size_t>(i + 1) < ts.tileUserData.size()) readUserData(ts.tileUserData[static_cast<size_t>(i + 1)], tile, schema, data.name, plan);
                data.tiles.push_back(std::move(tile));
            }
            data.nextId = count;
            for (auto& [name, entry] : schema) data.propertySchema.push_back(std::move(entry));
            plan.tilesets.push_back(std::move(out));
        }

        int floorDivide(int value, int divisor) {
            int q = value / divisor;
            if (value % divisor != 0 && value < 0) q--;
            return q;
        }

        void buildLayer(const TilemapCelOutput& cel, const TilesetOutput& tileset, int tilesetIndex, int canvasHeight, ImportGrid& grid, ImportPlan& plan) {
            ImportLayer layer;
            layer.name = cel.layer;
            layer.tileset = tilesetIndex;
            // the cel's bottom edge in canvas pixels from the bottom (y up), its top-left in pixels
            const int tw = tileset.tileWidth, th = tileset.tileHeight;
            const int columns = cel.tilemap.columns, rows = cel.tilemap.rows;
            const int bottom = canvasHeight - cel.y - rows * th;
            const int baseX = floorDivide(cel.x, tw);
            const int baseY = floorDivide(bottom, th);
            // a cel off the tile grid keeps its exact place through the layer's offset
            layer.offsetX = cel.x - baseX * tw;
            layer.offsetY = bottom - baseY * th;
            int64_t missing = 0;
            for (int row = 0; row < rows; row++) {
                for (int column = 0; column < columns; column++) {
                    const uint32_t raw = cel.tilemap.at(column, row);
                    const uint32_t id = raw & cel.tilemap.idMask;
                    if (id == 0) continue;
                    uint32_t flags = 0;
                    if (raw & cel.tilemap.xFlipMask) flags |= TILE_FLIP_X;
                    if (raw & cel.tilemap.yFlipMask) flags |= TILE_FLIP_Y;
                    if (raw & cel.tilemap.diagonalMask) flags |= TILE_TRANSPOSE;
                    if (id > static_cast<uint32_t>(MAX_TILE_ID)) {
                        missing++;
                        continue;
                    }
                    if (static_cast<int>(id) >= tileset.tileCount) missing++;
                    ImportCell cell;
                    cell.x = baseX + column;
                    cell.y = baseY + rows - 1 - row;
                    cell.value = makeCellValue(static_cast<int32_t>(id) - 1, flags);
                    layer.cells.push_back(cell);
                }
            }
            if (missing > 0) plan.warnings.push_back("Layer '" + cel.layer + "': " + std::to_string(missing) + " cells name tiles past the end of tileset '" + tileset.name + "'");
            std::sort(layer.cells.begin(), layer.cells.end(), [](const ImportCell& a, const ImportCell& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
            grid.layers.push_back(std::move(layer));
        }

    }

    bool tilemapFromAseprite(const pixelart::aseprite::AsepriteImport& import, const std::string& asepritePath, int canvasHeight, int frame, ImportPlan& out, std::vector<ImportProblem>& problems) {
        out = ImportPlan();
        out.sourceFile = asepritePath;
        out.format = "aseprite";
        const std::vector<TilemapCelOutput>& cels = import.tilemapCels();
        const std::vector<TilesetOutput>& tilesets = import.tilesets();
        const auto fail = [&](const std::string& message) {
            ImportProblem p;
            p.kind = "parse";
            p.field = "tilemapCels";
            p.message = message;
            p.fix = "add a tilemap layer in Aseprite, or import this file as a sprite";
            p.file = asepritePath;
            problems.push_back(std::move(p));
            return false;
        };
        if (canvasHeight <= 0) return fail("the canvas height is unknown");
        if (cels.empty()) return fail("the file has no tilemap layers");
        std::map<uint32_t, int> tilesetById;
        for (size_t i = 0; i < tilesets.size(); i++) tilesetById[tilesets[i].id] = static_cast<int>(i);

        std::vector<std::pair<std::pair<int, int>, int>> gridsBySize;   // (tile size, plan grid)
        std::map<int, int> planTileset;                                 // tilesets() index -> plan tileset
        const std::string stem = std::filesystem::path(asepritePath).stem().generic_string();
        for (const TilemapCelOutput& cel : cels) {
            if (cel.frame != frame) continue;
            const auto found = tilesetById.find(cel.tileset);
            if (found == tilesetById.end() || cel.tilemap.columns <= 0 || cel.tilemap.rows <= 0) {
                out.warnings.push_back("Layer '" + cel.layer + "' has no usable tileset and is skipped");
                continue;
            }
            const TilesetOutput& tileset = tilesets[static_cast<size_t>(found->second)];
            if (tileset.tileWidth <= 0 || tileset.tileHeight <= 0) {
                out.warnings.push_back("Layer '" + cel.layer + "' has a tileset with no tile size and is skipped");
                continue;
            }
            const auto emplaced = planTileset.try_emplace(found->second, static_cast<int>(out.tilesets.size()));
            if (emplaced.second) buildTileset(tileset, static_cast<size_t>(found->second), asepritePath, out);

            const std::pair<int, int> size{tileset.tileWidth, tileset.tileHeight};
            int gridIndex = -1;
            for (const auto& [gridSize, index] : gridsBySize) {
                if (gridSize == size) gridIndex = index;
            }
            if (gridIndex < 0) {
                gridIndex = static_cast<int>(out.grids.size());
                ImportGrid grid;
                grid.name = gridsBySize.empty() ? stem : stem + " @" + std::to_string(size.first) + "x" + std::to_string(size.second);
                grid.spec.layout = TileGridLayout::Rectangle;
                grid.spec.cellWidth = size.first;
                grid.spec.cellHeight = size.second;
                gridsBySize.emplace_back(size, gridIndex);
                out.grids.push_back(std::move(grid));
            }
            buildLayer(cel, tileset, emplaced.first->second, canvasHeight, out.grids[static_cast<size_t>(gridIndex)], out);
        }
        for (const TilemapCelOutput& cel : cels) {
            if (cel.frame != frame) {
                out.warnings.push_back("Tilemap cels of frame " + std::to_string(cel.frame) + " are not imported (frame " + std::to_string(frame) + " only)");
                break;
            }
        }
        if (out.grids.empty()) return fail("no tilemap cel on frame " + std::to_string(frame) + " has a usable tileset");
        return true;
    }

}
