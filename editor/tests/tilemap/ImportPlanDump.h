// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The JSON the tilemap importer tests compare with their goldens
// (editor/tests/fixtures/tilemaps/*.expected.json), and reading the fixtures and goldens.

#pragma once

#include "tilemap/import/ImportPlan.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace jasperengine::editor::tilemapimport::testing {

    inline std::filesystem::path tilemapFixtures() {
        return std::filesystem::path(JASPER_TEST_FIXTURES_DIR) / "tilemaps";
    }

    inline std::string readFixture(const std::string& name) {
        std::ifstream in(tilemapFixtures() / name, std::ios::binary);
        EXPECT_TRUE(in.good()) << name;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    inline FileReader fixtureReader() {
        return [](const std::string& path, std::string& out) {
            std::ifstream in(tilemapFixtures() / path, std::ios::binary);
            if (!in) return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        };
    }

    inline nlohmann::json goldenJson(const std::string& name) {
        return nlohmann::json::parse(readFixture(name));
    }

    inline double roundedForGolden(double value) {
        return std::round(value * 1.0e7) / 1.0e7;
    }

    inline std::string flagText(uint32_t flags) {
        std::string text;
        if (flags & TILE_FLIP_X) text += 'x';
        if (flags & TILE_FLIP_Y) text += 'y';
        if (flags & TILE_TRANSPOSE) text += 't';
        return text;
    }

    inline nlohmann::json propertyJson(const TilePropertyValue& v) {
        switch (v.type) {
            case TilePropertyType::Bool: return v.b;
            case TilePropertyType::Int: return v.i;
            case TilePropertyType::Float: return roundedForGolden(v.f);
            case TilePropertyType::Color: return {roundedForGolden(v.color.x), roundedForGolden(v.color.y), roundedForGolden(v.color.z), roundedForGolden(v.color.w)};
            case TilePropertyType::String: return v.s;
        }
        return nullptr;
    }

    inline const char* typeName(TilePropertyType type) {
        switch (type) {
            case TilePropertyType::Bool: return "bool";
            case TilePropertyType::Int: return "int";
            case TilePropertyType::Float: return "float";
            case TilePropertyType::String: return "string";
            case TilePropertyType::Color: return "color";
        }
        return "";
    }

    inline nlohmann::json planJson(const ImportPlan& plan) {
        using nlohmann::json;
        json out;
        out["grids"] = json::array();
        for (const ImportGrid& g : plan.grids) {
            json grid;
            const char* layouts[] = {"rectangle", "isometric", "staggered", "hexagon"};
            grid["layout"] = layouts[static_cast<int>(g.spec.layout)];
            grid["cell"] = {g.spec.cellWidth, g.spec.cellHeight};
            grid["origin"] = {g.originX, g.originY};
            if (g.spec.layout == TileGridLayout::IsometricStaggered || g.spec.layout == TileGridLayout::Hexagon) {
                grid["stagger_axis"] = g.spec.staggerAxis == StaggerAxis::X ? "x" : "y";
                grid["stagger_index"] = g.spec.staggerIndex == StaggerIndex::Odd ? "odd" : "even";
                if (g.spec.layout == TileGridLayout::Hexagon) grid["hex_side"] = g.spec.hexSideLength;
            }
            grid["layers"] = json::array();
            for (const ImportLayer& l : g.layers) {
                json layer;
                layer["name"] = l.name;
                layer["tileset"] = l.tileset;
                layer["visible"] = l.visible;
                layer["opacity"] = roundedForGolden(l.opacity);
                layer["offset"] = {l.offsetX, l.offsetY};
                layer["chunks"] = l.chunks;
                layer["cells"] = json::array();
                for (const ImportCell& c : l.cells) {
                    json cell = {c.x, c.y, cellValueTile(c.value), flagText(cellValueFlags(c.value))};
                    if (c.terrain != TERRAIN_NONE_VALUE) cell.push_back(c.terrain);
                    layer["cells"].push_back(std::move(cell));
                }
                grid["layers"].push_back(std::move(layer));
            }
            out["grids"].push_back(std::move(grid));
        }

        out["tilesets"] = json::array();
        for (const ImportTileset& t : plan.tilesets) {
            json tileset;
            tileset["name"] = t.data.name;
            tileset["tile_size"] = {t.data.tileWidth, t.data.tileHeight};
            const char* shapes[] = {"square", "isometric", "hex_pointy", "hex_flat"};
            tileset["shape"] = shapes[static_cast<int>(t.data.shape)];
            tileset["external"] = t.externalFile;
            tileset["images"] = json::array();
            for (const ImportImage& image : t.images) tileset["images"].push_back({{"source", image.source}, {"path", image.path}, {"size", {image.width, image.height}}});
            tileset["sources"] = json::array();
            for (const TilesetSource& s : t.data.sources) {
                tileset["sources"].push_back({{"id", s.id}, {"kind", s.kind == TilesetSourceKind::Texture ? "texture" : s.kind == TilesetSourceKind::Sheet ? "sheet" : "aseprite"},
                                              {"path", s.asset.path}, {"tile_size", {s.tileWidth, s.tileHeight}}, {"margin", s.margin}, {"spacing", s.spacing}, {"skip_empty", s.skipEmpty}});
            }
            tileset["tile_total"] = t.data.tiles.size();
            tileset["next_id"] = t.data.nextId;
            tileset["schema"] = json::array();
            for (const PropertySchemaEntry& e : t.data.propertySchema) {
                tileset["schema"].push_back({{"name", e.name}, {"type", typeName(e.type)}, {"default", propertyJson(e.defaultValue)}});
            }
            tileset["terrain_sets"] = json::array();
            for (const TerrainSetDef& s : t.data.terrainSets) {
                json terrains = json::array();
                for (const TerrainDef& d : s.terrains) terrains.push_back({d.name, d.color});
                tileset["terrain_sets"].push_back({{"name", s.name}, {"mode", s.mode == TerrainMode::Sides ? "sides" : s.mode == TerrainMode::Blob ? "blob" : "dual_grid"}, {"terrains", terrains}});
            }
            tileset["tiles"] = json::array();
            for (const TileDef& d : t.data.tiles) {
                json tile = {{"id", d.id}, {"at", {d.atX, d.atY}}};
                bool defaults = true;
                if (t.data.sources.size() > 1) {
                    tile["source"] = d.source;
                    defaults = false;
                }
                if (d.offsetX != 0 || d.offsetY != 0) {
                    tile["offset"] = {d.offsetX, d.offsetY};
                    defaults = false;
                }
                if (d.probability != 1.0f) {
                    tile["probability"] = roundedForGolden(d.probability);
                    defaults = false;
                }
                if (d.collision.mode != TileCollisionMode::None) {
                    json polygons = json::array();
                    for (const TilePolygon& p : d.collision.polygons) {
                        json points = json::array();
                        for (const auto& point : p.points) points.push_back({point[0], point[1]});
                        polygons.push_back(std::move(points));
                    }
                    tile["collision"] = {{"mode", "custom"}, {"polygons", std::move(polygons)}};
                    defaults = false;
                }
                if (!d.animation.empty()) {
                    json frames = json::array();
                    for (const TileAnimationFrame& f : d.animation) frames.push_back({f.tile, f.ms});
                    tile["animation"] = std::move(frames);
                    defaults = false;
                }
                if (d.terrain.set >= 0) {
                    const char* slots[] = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};
                    json peering = json::object();
                    for (int s = 0; s < 8; s++) {
                        const int16_t value = d.terrain.peering[static_cast<size_t>(s)];
                        if (value == PEERING_ANY) continue;
                        if (value == PEERING_NONE) peering[slots[s]] = "none";
                        else peering[slots[s]] = value;
                    }
                    tile["terrain"] = {{"set", d.terrain.set}, {"center", d.terrain.center}, {"peering", std::move(peering)}};
                    defaults = false;
                }
                if (!d.properties.empty()) {
                    json properties = json::object();
                    for (const auto& [name, value] : d.properties) properties[name] = propertyJson(value);
                    tile["properties"] = std::move(properties);
                    defaults = false;
                }
                if (!defaults) tileset["tiles"].push_back(std::move(tile));
            }
            out["tilesets"].push_back(std::move(tileset));
        }

        out["warnings"] = plan.warnings;
        out["skipped"] = json::array();
        for (const ImportSkipped& s : plan.skipped) out["skipped"].push_back({{"kind", s.kind}, {"name", s.name}, {"count", s.count}});
        const ImportCounts c = plan.counts();
        out["counts"] = {{"grids", c.grids}, {"layers", c.layers}, {"tilesets", c.tilesets}, {"images", c.images}, {"tiles", c.tiles}, {"terrain_cells", c.terrainCells}, {"skipped", c.skipped}, {"warnings", c.warnings}};
        return out;
    }

}
