// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// AC3 (docs/specs/S50-grid-tilemap.md R11-R20, §6.2). fixtures/tilemap/terrain_templates.json was
// generated outside the repository from the references, not from this code: cr31's and Godot's
// layouts from the pixels of their sheet images, dual_grid_4x4 from SpriteCook's frameByMask text,
// sides_16 from its one-line rule. Its tile names are this implementation's naming scheme.

#include "tilemap/TerrainTemplates.h"
#include "tilemap/TileAlphaTrace.h"
#include "tilemap/Tileset.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;

namespace {

    std::string tilesetFixtureText(const std::string& name){
        const std::filesystem::path file = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "tilemap" / name;
        std::ifstream in(file);
        EXPECT_TRUE(in.good()) << file;
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    TilesetData richTileset(){
        TilesetData d;
        d.name = "Overworld \"quoted\"";
        d.tileWidth = 16;
        d.tileHeight = 8;
        d.shape = TileShape::Isometric;

        TilesetSource texture;
        texture.id = 0;
        texture.kind = TilesetSourceKind::Texture;
        texture.asset.set("tiles/a.png", Guid::parse("9f3c00000000000000000000000000aa"));
        texture.tileWidth = 16;
        texture.tileHeight = 8;
        texture.margin = 1;
        texture.spacing = 2;
        texture.skipEmpty = false;
        d.sources.push_back(texture);
        TilesetSource sheet;
        sheet.id = 1;
        sheet.kind = TilesetSourceKind::Sheet;
        sheet.asset = AssetRef("tiles/water.spriteatlas");
        sheet.frames = {"water_0", "water_1"};
        d.sources.push_back(sheet);
        TilesetSource aseprite;
        aseprite.id = 2;
        aseprite.kind = TilesetSourceKind::Aseprite;
        aseprite.asset = AssetRef("tiles/hero.aseprite");
        aseprite.asepriteTileset = 3;
        d.sources.push_back(aseprite);

        PropertySchemaEntry damage;
        damage.name = "damage";
        damage.type = TilePropertyType::Int;
        damage.defaultValue.type = TilePropertyType::Int;
        damage.defaultValue.i = 2;
        d.propertySchema.push_back(damage);
        PropertySchemaEntry slippery;
        slippery.name = "friction";
        slippery.type = TilePropertyType::Float;
        slippery.defaultValue.type = TilePropertyType::Float;
        slippery.defaultValue.f = 0.25;
        d.propertySchema.push_back(slippery);
        PropertySchemaEntry tag;
        tag.name = "tag";
        tag.type = TilePropertyType::String;
        tag.defaultValue.type = TilePropertyType::String;
        tag.defaultValue.s = "ground";
        d.propertySchema.push_back(tag);
        PropertySchemaEntry solid;
        solid.name = "solid";
        solid.type = TilePropertyType::Bool;
        solid.defaultValue.type = TilePropertyType::Bool;
        d.propertySchema.push_back(solid);
        PropertySchemaEntry tint;
        tint.name = "tint";
        tint.type = TilePropertyType::Color;
        tint.defaultValue.type = TilePropertyType::Color;
        tint.defaultValue.color = Vector4(1.0f, 0.0f, 51.0f / 255.0f, 1.0f);
        d.propertySchema.push_back(tint);

        TerrainSetDef blob;
        blob.name = "Ground";
        blob.mode = TerrainMode::Blob;
        blob.terrains = {{"Grass", "#5fa04e"}, {"Dirt", "#8a6a3f"}};
        d.terrainSets.push_back(blob);
        TerrainSetDef sides;
        sides.name = "Road";
        sides.mode = TerrainMode::Sides;
        sides.terrains = {{"Road", "#404040"}};
        d.terrainSets.push_back(sides);
        TerrainSetDef dual;
        dual.name = "Shore";
        dual.mode = TerrainMode::DualGrid;
        dual.twoSurface = true;
        dual.terrains = {{"Sand", "#e3c27a"}, {"Water", "#3a6fd8"}};
        d.terrainSets.push_back(dual);

        TileDef& a = d.addTile(0, 0, 0);
        a.name = "grass";
        TileDef& b = d.addTile(0, 7, 0);
        b.name = "lava";
        b.offsetX = 3;
        b.offsetY = -2;
        b.probability = 0.3f;
        b.collision.mode = TileCollisionMode::Alpha;
        b.collision.threshold = 200;
        b.collision.kind = TileCollisionKind::OneWay;
        b.collision.oneWay = TileOneWayDirection::Left;
        b.collision.polygons.push_back(TilePolygon{{{0, 0}, {16, 0}, {16, 8}, {0, 8}}});
        b.animation = {TileAnimationFrame{0, 120}, TileAnimationFrame{1, 80}};
        b.randomStart = true;
        b.navArea = 2;
        b.properties["damage"].type = TilePropertyType::Int;
        b.properties["damage"].i = -3;
        b.properties["friction"].type = TilePropertyType::Float;
        b.properties["friction"].f = 0.125;
        b.properties["tag"].type = TilePropertyType::String;
        b.properties["tag"].s = "h\"ot\n";
        b.properties["solid"].type = TilePropertyType::Bool;
        b.properties["solid"].b = true;
        b.properties["tint"].type = TilePropertyType::Color;
        b.properties["tint"].color = Vector4(0.0f, 1.0f, 102.0f / 255.0f, 128.0f / 255.0f);
        TileDef& c = d.addTile(0, 4, 1);
        c.probability = 0.25f;
        c.terrain.set = 0;
        c.terrain.center = 1;
        c.terrain.peering[0] = 1;
        c.terrain.peering[1] = PEERING_ANY;
        c.terrain.peering[2] = 0;
        c.terrain.peering[3] = PEERING_NONE;
        TileDef& e = d.addTile(1, 0, 0);
        e.frame = "water_0";
        e.terrain.set = 2;
        e.terrain.peering[1] = 0;
        e.terrain.peering[3] = 1;
        e.terrain.peering[5] = PEERING_NONE;
        e.collision.mode = TileCollisionMode::Custom;
        e.collision.polygons.push_back(TilePolygon{{{1, 1}, {2, 1}, {2, 2}}});
        e.collision.polygons.push_back(TilePolygon{{{3, 3}, {4, 3}, {4, 4}}});
        TileDef& f = d.addTile(2, 4, 0);
        f.collision.mode = TileCollisionMode::Full;
        f.collision.kind = TileCollisionKind::Sensor;
        d.removeTile(d.tiles[4].id);
        d.addTile(2, 5, 0);

        TileStamp stamp;
        stamp.name = "house";
        stamp.width = 3;
        stamp.height = 2;
        stamp.cells = {TileStampCell{0, 0, 0, 0}, TileStampCell{1, 0, 1, TILE_FLIP_X}, TileStampCell{2, 1, 3, TILE_FLIP_X | TILE_FLIP_Y | TILE_TRANSPOSE}};
        d.stamps.push_back(stamp);
        return d;
    }

    std::vector<std::string> problemKinds(const std::vector<TilesetProblem>& problems){
        std::vector<std::string> kinds;
        for (const TilesetProblem& p : problems)
            kinds.push_back(p.kind);
        return kinds;
    }

    bool hasProblem(const std::vector<TilesetProblem>& problems, const std::string& kind, const std::string& field){
        for (const TilesetProblem& p : problems){
            if (p.kind == kind && p.field == field)
                return true;
        }
        return false;
    }

    const char* const SPEC_EXAMPLE = R"({
  "version": 1,
  "name": "Overworld",
  "tile_size": [16, 16],
  "tile_shape": "square",
  "sources": [
    {"id": 0, "kind": "texture", "texture": "guid:9f3c0000000000000000000000000001", "tile_size": [16, 16], "margin": 0, "spacing": 0, "skip_empty": true},
    {"id": 1, "kind": "sheet", "sheet": "guid:9f3c0000000000000000000000000002", "frames": ["water_0", "water_1"]},
    {"id": 2, "kind": "aseprite", "file": "tiles/x.aseprite", "tileset": 0}
  ],
  "property_schema": [{"name": "damage", "type": "int", "default": 0}],
  "terrain_sets": [
    {"name": "Ground", "mode": "blob", "terrains": [{"name": "Grass", "color": "#5fa04e"}, {"name": "Dirt", "color": "#8a6a3f"}]},
    {"name": "Shore", "mode": "dual_grid", "two_surface": true, "terrains": [{"name": "Sand", "color": "#e3c27a"}, {"name": "Water", "color": "#3a6fd8"}]}
  ],
  "tiles": [
    {"id": 0, "source": 0, "at": [0, 0], "name": "grass"},
    {"id": 7, "source": 0, "at": [7, 0], "name": "lava", "properties": {"damage": 3},
     "collision": {"mode": "alpha", "threshold": 128, "kind": "solid"},
     "animation": [{"tile": 7, "ms": 120}, {"tile": 0, "ms": 120}], "random_start": true},
    {"id": 12, "source": 0, "at": [4, 1], "probability": 0.25,
     "terrain": {"set": 0, "center": 0, "peering": {"n": 0, "ne": "any", "e": 0, "se": "none", "s": 0, "sw": 0, "w": 0, "nw": 0}}}
  ],
  "stamps": [{"name": "house", "size": [3, 2], "cells": [[0, 0, 12], [1, 0, 7, "x"]]}]
}
)";

    TilesetData blobBlock(int columns, int rows){
        TilesetData d;
        TilesetSource source;
        source.id = 0;
        source.kind = TilesetSourceKind::Texture;
        d.sources.push_back(source);
        TerrainSetDef set;
        set.name = "Ground";
        set.mode = TerrainMode::Blob;
        set.terrains = {{"Grass", "#00ff00"}};
        d.terrainSets.push_back(set);
        for (int y = 0; y < rows; y++){
            for (int x = 0; x < columns; x++)
                d.addTile(0, x, y);
        }
        return d;
    }

    void addCompleteBlob(TilesetData& d){
        ASSERT_EQ(d.tiles.size(), 49u);
        std::vector<TilesetProblem> problems;
        ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Blob47, 0, TerrainTemplateOptions{}, problems)) << (problems.empty() ? "" : problems[0].message);
    }

}

TEST(Tileset, SaveLoadIsByteIdentical){
    const TilesetData original = richTileset();
    const std::string first = tileset::write(original);
    TilesetData parsed;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(first, parsed, problems)) << (problems.empty() ? "" : problems[0].message);
    EXPECT_TRUE(problems.empty()) << problems[0].field << " " << problems[0].message;
    EXPECT_EQ(parsed, original);
    EXPECT_EQ(tileset::write(parsed), first);
    TilesetData again;
    ASSERT_TRUE(tileset::parse(tileset::write(parsed), again, problems));
    EXPECT_EQ(again, original);
}

TEST(Tileset, WritesKeysInTheDocumentedOrderOneTilePerLine){
    TilesetData d;
    d.name = "A";
    TilesetSource source;
    source.asset = AssetRef("a.png");
    source.tileWidth = 16;
    source.tileHeight = 16;
    d.sources.push_back(source);
    TileDef& tile = d.addTile(0, 2, 1);
    tile.name = "grass";
    tile.probability = 0.25f;
    TileDef& second = d.addTile(0, 3, 1);
    second.navArea = 4;
    const std::string expected =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"A\",\n"
        "  \"tile_size\": [16, 16],\n"
        "  \"tile_shape\": \"square\",\n"
        "  \"next_id\": 2,\n"
        "  \"sources\": [\n"
        "    {\"id\": 0, \"kind\": \"texture\", \"texture\": {\"path\": \"a.png\"}, \"tile_size\": [16, 16], \"margin\": 0, \"spacing\": 0, \"skip_empty\": true}\n"
        "  ],\n"
        "  \"property_schema\": [],\n"
        "  \"terrain_sets\": [],\n"
        "  \"tiles\": [\n"
        "    {\"id\": 0, \"source\": 0, \"at\": [2, 1], \"name\": \"grass\", \"probability\": 0.25},\n"
        "    {\"id\": 1, \"source\": 0, \"at\": [3, 1], \"nav_area\": 4}\n"
        "  ],\n"
        "  \"stamps\": []\n"
        "}\n";
    EXPECT_EQ(tileset::write(d), expected);
}

TEST(Tileset, UnsetTileFieldsAreLeftOut){
    TilesetData d;
    d.addTile(0, 1, 2);
    const std::string text = tileset::write(d);
    EXPECT_NE(text.find("{\"id\": 0, \"source\": 0, \"at\": [1, 2]}"), std::string::npos) << text;
}

TEST(Tileset, ParsesTheSpecExample){
    TilesetData d;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(SPEC_EXAMPLE, d, problems)) << (problems.empty() ? "" : problems[0].message);
    EXPECT_TRUE(problems.empty());
    EXPECT_EQ(d.tiles.size(), 3u);
    EXPECT_EQ(d.findTile(12)->probability, 0.25f);
    EXPECT_EQ(d.findTile(12)->terrain.peering[1], PEERING_ANY);
    EXPECT_EQ(d.findTile(12)->terrain.peering[3], PEERING_NONE);
    EXPECT_EQ(d.findTile(7)->properties.at("damage").i, 3);
    EXPECT_EQ(d.nextId, 13);
    EXPECT_EQ(d.sources[0].asset.guid.str(), "9f3c0000000000000000000000000001");
    EXPECT_TRUE(d.sources[0].asset.path.empty());
    EXPECT_EQ(d.sources[2].asset.path, "tiles/x.aseprite");
    EXPECT_EQ(d.terrainCount(), 4);
    EXPECT_EQ(d.findTerrain("Water"), 3);
    EXPECT_EQ(d.stamps[0].cells[1].flags, TILE_FLIP_X);
}

TEST(Tileset, ReadsEveryAssetReferenceForm){
    const auto sourceOf = [](const std::string& reference){
        TilesetData d;
        std::vector<TilesetProblem> problems;
        const std::string text = R"({"version":1,"sources":[{"id":0,"kind":"texture","texture":)" + reference + R"(}]})";
        EXPECT_TRUE(tileset::parse(text, d, problems)) << text;
        return d.sources.empty() ? TilesetSource() : d.sources[0];
    };
    EXPECT_EQ(sourceOf("\"a/b.png\"").asset.path, "a/b.png");
    EXPECT_TRUE(sourceOf("\"a/b.png\"").asset.guid.isNull());
    const AssetRef guidOnly = sourceOf("\"guid:0000000000000000000000000000abcd\"").asset;
    EXPECT_EQ(guidOnly.guid.str(), "0000000000000000000000000000abcd");
    const AssetRef both = sourceOf(R"({"path":"c.png","guid":"0000000000000000000000000000abcd"})").asset;
    EXPECT_EQ(both.path, "c.png");
    EXPECT_FALSE(both.guid.isNull());
}

TEST(Tileset, MalformedJsonReportsTheLine){
    TilesetData d;
    std::vector<TilesetProblem> problems;
    EXPECT_FALSE(tileset::parse("{\n  \"version\": 1,\n  oops\n}", d, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "parse");
    EXPECT_EQ(problems[0].field, "line 3");
}

TEST(Tileset, FatalProblemsNameTheirJsonPath){
    struct Case{ const char* text; const char* field; };
    for (const Case& c : {
             Case{R"({"name":"a"})", "version"},
             Case{R"({"version":2})", "version"},
             Case{R"({"version":1,"tile_shape":"round"})", "tile_shape"},
             Case{R"({"version":1,"sources":[{"id":0,"kind":"movie"}]})", "sources[0].kind"},
             Case{R"({"version":1,"sources":[{"id":0,"kind":"texture"}]})", "sources[0].texture"},
             Case{R"({"version":1,"tiles":[{"id":0,"source":0,"at":[1]}]})", "tiles[0].at"},
             Case{R"({"version":1,"tiles":[{"id":"x","source":0}]})", "tiles[0].id"},
             Case{R"({"version":1,"tiles":[{"id":0,"source":0,"probability":"x"}]})", "tiles[0].probability"},
             Case{R"({"version":1,"terrain_sets":[{"name":"a","mode":"wang"}]})", "terrain_sets[0].mode"},
             Case{R"({"version":1,"tiles":[{"id":0,"source":0,"collision":{"mode":"magic"}}]})", "tiles[0].collision.mode"},
         }){
        TilesetData d;
        std::vector<TilesetProblem> problems;
        EXPECT_FALSE(tileset::parse(c.text, d, problems)) << c.text;
        EXPECT_TRUE(hasProblem(problems, "parse", c.field)) << c.text << " got " << (problems.empty() ? std::string("nothing") : problems.back().field);
    }
}

TEST(Tileset, UnknownKeysAreReportedAndSkipped){
    TilesetData d;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(R"({"version":1,"future":true,"tiles":[{"id":3,"source":0,"sparkle":1}]})", d, problems));
    EXPECT_TRUE(hasProblem(problems, "parse", "future"));
    EXPECT_TRUE(hasProblem(problems, "parse", "tiles[0].sparkle"));
    EXPECT_EQ(d.tiles.size(), 1u);
}

TEST(Tileset, DanglingAnimationFramesAreReportedAndSkipped){
    TilesetData d;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(R"({"version":1,"tiles":[{"id":0,"source":0,"animation":[{"tile":0,"ms":100},{"tile":44,"ms":100}]}]})", d, problems));
    EXPECT_TRUE(hasProblem(problems, "unknown_tile", "tiles[0].animation[1]"));
    ASSERT_EQ(d.tiles[0].animation.size(), 1u);
    EXPECT_EQ(d.tiles[0].animation[0].tile, 0);
}

TEST(Tileset, TilesSortByIdAndNextIdCoversThem){
    TilesetData d;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(R"({"version":1,"next_id":2,"tiles":[{"id":9,"source":0},{"id":1,"source":0}]})", d, problems));
    EXPECT_EQ(d.tiles[0].id, 1);
    EXPECT_EQ(d.tiles[1].id, 9);
    EXPECT_EQ(d.nextId, 10);
    EXPECT_TRUE(hasProblem(problems, "parse", "next_id"));
}

TEST(Tileset, IdsAreNeverReused){
    TilesetData d;
    const int32_t a = d.addTile(0, 0, 0).id;
    const int32_t b = d.addTile(0, 1, 0).id;
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
    ASSERT_TRUE(d.removeTile(b));
    EXPECT_EQ(d.findTile(b), nullptr);
    const int32_t c = d.addTile(0, 2, 0).id;
    EXPECT_EQ(c, 2);
    EXPECT_EQ(d.nextId, 3);
    // the survivors keep their ids and stay sorted
    EXPECT_NE(d.findTile(a), nullptr);
    EXPECT_NE(d.findTile(c), nullptr);
    EXPECT_FALSE(d.removeTile(b));
    // and a removed id stays burnt across a save and load
    TilesetData loaded;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(tileset::write(d), loaded, problems));
    EXPECT_EQ(loaded.nextId, 3);
    EXPECT_EQ(loaded.addTile(0, 3, 0).id, 3);
}

TEST(Tileset, DefaultNamesFollowThePositionInTheSource){
    TilesetData d;
    d.addTile(0, 0, 0);
    TileDef& named = d.addTile(0, 1, 0);
    named.name = "stone";
    d.addTile(1, 0, 0);
    d.addTile(0, 2, 0);
    EXPECT_EQ(d.tileName(d.tiles[0]), "0_0");
    EXPECT_EQ(d.tileName(d.tiles[1]), "stone");
    EXPECT_EQ(d.tileName(d.tiles[2]), "1_0");
    EXPECT_EQ(d.tileName(d.tiles[3]), "0_2");
    EXPECT_EQ(d.findTileByName("0_2"), 3);
    EXPECT_EQ(d.findTileByName("stone"), 1);
    EXPECT_EQ(d.findTileByName("nope"), NO_TILE);
}

TEST(Tileset, TerrainIndicesAreGlobalAcrossSets){
    const TilesetData d = richTileset();
    EXPECT_EQ(d.terrainCount(), 5);
    EXPECT_EQ(d.terrainIndex(2, 1), 4);
    int set = 0, local = 0;
    ASSERT_TRUE(d.terrainOf(4, set, local));
    EXPECT_EQ(set, 2);
    EXPECT_EQ(local, 1);
    EXPECT_FALSE(d.terrainOf(5, set, local));
    EXPECT_EQ(d.terrainIndex(2, 2), NO_TERRAIN);
    ASSERT_NE(d.terrain(3), nullptr);
    EXPECT_EQ(d.terrain(3)->name, "Sand");
}

TEST(Tileset, ValidateNamesTheDeclaredPropertiesInItsFix){
    TilesetData d = richTileset();
    TilePropertyValue value;
    d.tiles[0].properties["undeclared"] = value;
    std::vector<TilesetProblem> problems;
    tileset::validate(d, problems);
    ASSERT_TRUE(hasProblem(problems, "unknown_property", "tiles[0].properties.undeclared"));
    EXPECT_NE(problems[0].fix.find("damage, friction, tag, solid, tint"), std::string::npos) << problems[0].fix;
}

TEST(Tileset, ValidateFlagsATypeMismatchedProperty){
    TilesetData d = richTileset();
    d.tiles[1].properties["damage"].type = TilePropertyType::Float;
    std::vector<TilesetProblem> problems;
    tileset::validate(d, problems);
    EXPECT_TRUE(hasProblem(problems, "invalid_property", "tiles[1].properties.damage"));
}

TEST(Tileset, ValidateReportsInvalidPeering){
    TilesetData d = richTileset();
    d.tiles[2].terrain.center = 9;
    d.tiles[2].terrain.peering[2] = 7;      // no terrain 7 in the set
    d.tiles[3].terrain.peering[0] = 0;      // dual grid has no side 'n' slot
    std::vector<TilesetProblem> problems;
    tileset::validate(d, problems);
    EXPECT_TRUE(hasProblem(problems, "invalid_peering", "tiles[2].terrain.center"));
    EXPECT_TRUE(hasProblem(problems, "invalid_peering", "tiles[2].terrain.peering.e"));
    EXPECT_TRUE(hasProblem(problems, "invalid_peering", "tiles[3].terrain.peering.n"));
}

TEST(Tileset, ValidateReportsDanglingFramesAndDuplicateIds){
    TilesetData d = richTileset();
    d.tiles[0].animation.push_back(TileAnimationFrame{999, 50});
    d.tiles.push_back(d.tiles[0]);
    std::vector<TilesetProblem> problems;
    tileset::validate(d, problems);
    EXPECT_TRUE(hasProblem(problems, "unknown_tile", "tiles[0].animation[0]"));
    EXPECT_TRUE(hasProblem(problems, "duplicate_id", "tiles[" + std::to_string(d.tiles.size() - 1) + "].id"));
}

TEST(Tileset, ValidateCapsTheTerrains){
    TilesetData d;
    TerrainSetDef set;
    set.terrains.resize((size_t)MAX_TILESET_TERRAINS + 1);
    d.terrainSets.push_back(set);
    std::vector<TilesetProblem> problems;
    tileset::validate(d, problems);
    EXPECT_EQ(problemKinds(problems), std::vector<std::string>{"too_many_terrains"});
}

TEST(Tileset, ValidateAcceptsAWellFormedTileset){
    const TilesetData d = richTileset();
    std::vector<TilesetProblem> problems;
    tileset::validate(d, problems);
    EXPECT_TRUE(problems.empty()) << problems[0].field << ": " << problems[0].message;
}

TEST(Tileset, PeeringSlotsAndKeysFollowTheMode){
    using V = std::vector<int>;
    EXPECT_EQ(tileset::peeringSlots(TileShape::Square, TerrainMode::Blob), (V{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(tileset::peeringSlots(TileShape::Square, TerrainMode::Sides), (V{0, 2, 4, 6}));
    EXPECT_EQ(tileset::peeringSlots(TileShape::Isometric, TerrainMode::DualGrid), (V{1, 3, 5, 7}));
    EXPECT_TRUE(tileset::peeringSlots(TileShape::HexPointy, TerrainMode::DualGrid).empty());
    EXPECT_EQ(tileset::peeringSlots(TileShape::HexFlat, TerrainMode::Blob).size(), 12u);
    EXPECT_STREQ(tileset::peeringKey(TileShape::HexFlat, 6), "c_e");
    EXPECT_STREQ(tileset::peeringKey(TileShape::HexPointy, 6), "c_ne");
    EXPECT_EQ(tileset::peeringSlotOfKey(TileShape::Square, "se"), 3);
    EXPECT_EQ(tileset::peeringSlotOfKey(TileShape::Square, "c_n"), -1);
}

// reference values from an independent implementation of the documented mix (MurmurHash3's block
// step and finaliser over seed, stroke, x, y): they pin the hash across compilers and platforms
TEST(Tileset, CellHashHasFixedValues){
    EXPECT_EQ(tileset::cellHash(0, 0, 0, 0), 3187378880u);
    EXPECT_EQ(tileset::cellHash(1, 0, 0, 0), 1392314761u);
    EXPECT_EQ(tileset::cellHash(1, 2, 3, 4), 2528703238u);
    EXPECT_EQ(tileset::cellHash(0xdeadbeefu, 7, -5, INT32_MIN), 667992247u);
}

TEST(Tileset, WeightedPickFollowsTheWeights){
    const float weights[3] = {1.0f, 0.0f, 3.0f};
    int counts[3] = {0, 0, 0};
    for (uint32_t i = 0; i < 40000; i++)
        counts[tileset::weightedPick(weights, 3, tileset::cellHash(9, 0, (int32_t)i, 0))]++;
    EXPECT_EQ(counts[1], 0);
    EXPECT_NEAR((double)counts[2] / (double)(counts[0] + counts[2]), 0.75, 0.02);
}

TEST(Tileset, WeightedPickNeedsAPositiveWeight){
    const float none[2] = {0.0f, -1.0f};
    EXPECT_EQ(tileset::weightedPick(none, 2, 12345), -1);
    EXPECT_EQ(tileset::weightedPick(nullptr, 0, 12345), -1);
    const float tiny[2] = {1e-12f, 0.0f};
    EXPECT_EQ(tileset::weightedPick(tiny, 2, 12345), 0);
}

// ---- alpha tracing: hand-made images, expected outlines written by hand

namespace {

    using TraceLoop = std::vector<std::array<int32_t, 2>>;

    // a little RGBA image from rows of '#', '.', '+' (solid, transparent, alpha 128); row 0 is the top
    std::vector<uint8_t> traceImage(const std::vector<std::string>& rows){
        std::vector<uint8_t> out;
        for (const std::string& row : rows){
            for (char c : row){
                out.push_back(0);
                out.push_back(0);
                out.push_back(0);
                out.push_back(c == '#' ? 255 : (c == '+' ? 128 : 0));
            }
        }
        return out;
    }

}

TEST(TileAlphaTrace, AnOpaqueTileIsOneRectangle){
    const std::vector<uint8_t> image = traceImage({"####", "####", "####", "####"});
    const std::vector<TilePolygon> polygons = traceAlpha(image.data(), 4, 4, 0, 0, 4, 4, 128);
    ASSERT_EQ(polygons.size(), 1u);
    EXPECT_EQ(polygons[0].points, (TraceLoop{{0, 0}, {4, 0}, {4, 4}, {0, 4}}));
}

TEST(TileAlphaTrace, AnEmptyTileHasNoPolygons){
    const std::vector<uint8_t> image = traceImage({"..", ".."});
    EXPECT_TRUE(traceAlpha(image.data(), 2, 2, 0, 0, 2, 2, 128).empty());
}

TEST(TileAlphaTrace, YIsUpFromTheBottomLeft){
    // the single solid pixel is in the image's top-left, so the tile's top-left
    const std::vector<uint8_t> image = traceImage({"#.", ".."});
    const std::vector<TilePolygon> polygons = traceAlpha(image.data(), 2, 2, 0, 0, 2, 2, 128);
    ASSERT_EQ(polygons.size(), 1u);
    EXPECT_EQ(polygons[0].points, (TraceLoop{{0, 1}, {1, 1}, {1, 2}, {0, 2}}));
}

TEST(TileAlphaTrace, CollinearPointsAreMerged){
    // an L made of two rectangles: 6 corners, none along a straight run
    const std::vector<uint8_t> image = traceImage({"#..", "#..", "###"});
    const std::vector<TilePolygon> polygons = traceAlpha(image.data(), 3, 3, 0, 0, 3, 3, 128);
    ASSERT_EQ(polygons.size(), 1u);
    EXPECT_EQ(polygons[0].points, (TraceLoop{{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}}));
}

TEST(TileAlphaTrace, HolesAreClockwiseAfterTheirSolid){
    const std::vector<uint8_t> image = traceImage({"####", "#..#", "#..#", "####"});
    const std::vector<TilePolygon> polygons = traceAlpha(image.data(), 4, 4, 0, 0, 4, 4, 128);
    ASSERT_EQ(polygons.size(), 2u);
    EXPECT_EQ(polygons[0].points, (TraceLoop{{0, 0}, {4, 0}, {4, 4}, {0, 4}}));
    EXPECT_EQ(polygons[1].points, (TraceLoop{{1, 1}, {1, 3}, {3, 3}, {3, 1}}));
}

TEST(TileAlphaTrace, PixelsTouchingAtACornerAreSeparate){
    const std::vector<uint8_t> image = traceImage({"#.", ".#"});
    const std::vector<TilePolygon> polygons = traceAlpha(image.data(), 2, 2, 0, 0, 2, 2, 128);
    ASSERT_EQ(polygons.size(), 2u);
    EXPECT_EQ(polygons[0].points, (TraceLoop{{1, 0}, {2, 0}, {2, 1}, {1, 1}}));
    EXPECT_EQ(polygons[1].points, (TraceLoop{{0, 1}, {1, 1}, {1, 2}, {0, 2}}));
}

TEST(TileAlphaTrace, TheThresholdIsInclusive){
    const std::vector<uint8_t> image = traceImage({"+"});
    EXPECT_EQ(traceAlpha(image.data(), 1, 1, 0, 0, 1, 1, 128).size(), 1u);
    EXPECT_TRUE(traceAlpha(image.data(), 1, 1, 0, 0, 1, 1, 129).empty());
}

TEST(TileAlphaTrace, TracesARectangleOfALargerImage){
    const std::vector<uint8_t> image = traceImage({"......", "..##..", "..##..", "......"});
    const std::vector<TilePolygon> polygons = traceAlpha(image.data(), 6, 4, 2, 1, 2, 2, 128);
    ASSERT_EQ(polygons.size(), 1u);
    EXPECT_EQ(polygons[0].points, (TraceLoop{{0, 0}, {2, 0}, {2, 2}, {0, 2}}));
}

TEST(TileAlphaTrace, PixelsOutsideTheImageCountAsTransparent){
    const std::vector<uint8_t> image = traceImage({"##", "##"});
    const std::vector<TilePolygon> polygons = traceAlpha(image.data(), 2, 2, 1, 0, 2, 2, 128);
    ASSERT_EQ(polygons.size(), 1u);
    EXPECT_EQ(polygons[0].points, (TraceLoop{{0, 0}, {1, 0}, {1, 2}, {0, 2}}));
    EXPECT_TRUE(traceAlpha(nullptr, 2, 2, 0, 0, 2, 2, 128).empty());
}

// ---- templates: the golden comes from the references (see the top of the file)

namespace {

    void checkGolden(const TilesetData& d, const nlohmann::json& entries, int set, int columns){
        const std::vector<int> slots = tileset::peeringSlots(d.shape, d.terrainSets[(size_t)set].mode);
        std::set<int32_t> expectedTerrain;
        for (const nlohmann::json& entry : entries){
            const int column = entry["column"].get<int>();
            const int row = entry["row"].get<int>();
            const int32_t id = row * columns + column;
            expectedTerrain.insert(id);
            const TileDef* tile = d.findTile(id);
            ASSERT_NE(tile, nullptr);
            const std::string where = "cell (" + std::to_string(column) + ", " + std::to_string(row) + ")";
            EXPECT_EQ(tile->name, entry["name"].get<std::string>()) << where;
            EXPECT_EQ(tile->terrain.set, set) << where;
            EXPECT_EQ(tile->terrain.center, entry["center"].get<int>()) << where;
            for (int slot : slots){
                const nlohmann::json& value = entry["peering"][tileset::peeringKey(d.shape, slot)];
                const int16_t expected = value.is_string() ? PEERING_NONE : (int16_t)value.get<int>();
                EXPECT_EQ(tile->terrain.peering[(size_t)slot], expected) << where << " " << tileset::peeringKey(d.shape, slot);
            }
        }
        for (const TileDef& tile : d.tiles){
            if (expectedTerrain.count(tile.id) == 0){
                EXPECT_EQ(tile.terrain.set, -1) << "tile " << tile.id << " should have no terrain";
            }
        }
    }

    nlohmann::json templatesGolden(){
        return nlohmann::json::parse(tilesetFixtureText("terrain_templates.json"));
    }

}

TEST(TerrainTemplates, Blob47EqualsCr31Sheet){
    TilesetData d = blobBlock(7, 7);
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Blob47, 0, TerrainTemplateOptions{}, problems));
    checkGolden(d, templatesGolden()["blob_47"], 0, 7);
}

TEST(TerrainTemplates, Blob47GodotEqualsTheGodotTemplate){
    TilesetData d = blobBlock(12, 4);
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Blob47Godot, 0, TerrainTemplateOptions{}, problems));
    checkGolden(d, templatesGolden()["blob_47_godot"], 0, 12);
}

TEST(TerrainTemplates, Sides16EqualsItsRule){
    TilesetData d = blobBlock(4, 4);
    d.terrainSets[0].mode = TerrainMode::Sides;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Sides16, 0, TerrainTemplateOptions{}, problems));
    checkGolden(d, templatesGolden()["sides_16"], 0, 4);
}

TEST(TerrainTemplates, DualGridEqualsFrameByMask){
    TilesetData d = blobBlock(4, 4);
    d.terrainSets[0].mode = TerrainMode::DualGrid;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::DualGrid4x4, 0, TerrainTemplateOptions{}, problems));
    checkGolden(d, templatesGolden()["dual_grid_4x4"], 0, 4);
    // frame 12 is left alone on a single-surface set
    EXPECT_EQ(d.findTile(12)->terrain.set, -1);
}

TEST(TerrainTemplates, TwoSurfaceDualGridAssignsFrame12){
    TilesetData d = blobBlock(4, 4);
    d.terrainSets[0].mode = TerrainMode::DualGrid;
    d.terrainSets[0].twoSurface = true;
    d.terrainSets[0].terrains = {{"Sand", "#fff000"}, {"Water", "#0000ff"}};
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::DualGrid4x4, 0, TerrainTemplateOptions{}, problems));
    checkGolden(d, templatesGolden()["dual_grid_4x4_two_surface"], 0, 4);
}

TEST(TerrainTemplates, RefusesAMismatchedSetAndStaysUnchanged){
    TilesetData d = blobBlock(4, 4);
    const TilesetData before = d;
    std::vector<TilesetProblem> problems;
    EXPECT_FALSE(terrain::applyTemplate(d, 0, TerrainTemplate::Sides16, 0, TerrainTemplateOptions{}, problems));
    EXPECT_FALSE(problems.empty());
    EXPECT_EQ(d, before);
}

TEST(TerrainTemplates, RefusesAnIncompleteBlockAndStaysUnchanged){
    TilesetData d = blobBlock(7, 6);
    const TilesetData before = d;
    std::vector<TilesetProblem> problems;
    EXPECT_FALSE(terrain::applyTemplate(d, 0, TerrainTemplate::Blob47, 0, TerrainTemplateOptions{}, problems));
    ASSERT_FALSE(problems.empty());
    EXPECT_EQ(problems[0].kind, "unknown_tile");
    EXPECT_EQ(d, before);
}

TEST(TerrainTemplates, AnOffsetOriginFindsTilesByGridPosition){
    TilesetData d = blobBlock(9, 9);
    std::vector<TilesetProblem> problems;
    const int32_t origin = d.tiles[9 * 2 + 1].id;     // column 1, row 2
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Blob47, origin, TerrainTemplateOptions{}, problems));
    const TileDef* tile = d.findTile(9 * 3 + 2);      // block (1, 1) is cr31 tile 20
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(tile->name, "blob_20");
    EXPECT_EQ(tile->terrain.peering[2], 0);    // e
    EXPECT_EQ(tile->terrain.peering[4], 0);    // s
    EXPECT_EQ(tile->terrain.peering[0], PEERING_NONE);
}

TEST(TerrainTemplates, NonGridSourcesTakeTilesInIdOrder){
    TilesetData d = blobBlock(4, 4);
    d.sources[0].kind = TilesetSourceKind::Sheet;
    d.terrainSets[0].mode = TerrainMode::Sides;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Sides16, 0, TerrainTemplateOptions{}, problems));
    EXPECT_EQ(d.findTile(5)->name, "sides_5");
}

TEST(TerrainTemplates, NamesAreOptional){
    TilesetData d = blobBlock(4, 4);
    d.terrainSets[0].mode = TerrainMode::Sides;
    d.findTile(3)->name = "keep";
    TerrainTemplateOptions options;
    options.assignNames = false;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Sides16, 0, options, problems));
    EXPECT_EQ(d.findTile(3)->name, "keep");
    EXPECT_EQ(d.findTile(3)->terrain.set, 0);
}

TEST(TerrainCoverageCheck, ACompleteBlobHasNothingMissing){
    TilesetData d = blobBlock(7, 7);
    addCompleteBlob(d);
    const TerrainCoverage coverage = terrain::coverage(d, 0, false);
    EXPECT_EQ(coverage.required, 47u);
    EXPECT_TRUE(coverage.missing.empty());
}

TEST(TerrainCoverageCheck, ListsExactlyTheMasksOfTheRemovedTiles){
    TilesetData d = blobBlock(7, 7);
    addCompleteBlob(d);
    std::set<std::array<int16_t, MAX_PEERING_SLOTS>> expected;
    // tiles 24 (cr31 28), 25 (cr31 127) and 35 (cr31 5) of the 7x7 block, each unique
    for (int32_t id : {24, 25, 35}){
        const TileDef* tile = d.findTile(id);
        ASSERT_NE(tile, nullptr);
        expected.insert(tile->terrain.peering);
        d.removeTile(id);
    }
    const TerrainCoverage coverage = terrain::coverage(d, 0, false);
    EXPECT_EQ(coverage.required, 47u);
    std::set<std::array<int16_t, MAX_PEERING_SLOTS>> missing;
    for (const TerrainMissingMask& mask : coverage.missing){
        EXPECT_EQ(mask.center, 0);
        missing.insert(mask.peering);
    }
    EXPECT_EQ(missing, expected);
    EXPECT_EQ(coverage.missing.size(), 3u);
}

TEST(TerrainCoverageCheck, AllowTransformsCountsFlippedVariants){
    TilesetData d = blobBlock(7, 7);
    addCompleteBlob(d);
    ASSERT_EQ(d.findTile(1)->name, "blob_4");
    d.removeTile(1);       // tile 4 (east only); its mirror tile 64 (west only) remains
    EXPECT_EQ(terrain::coverage(d, 0, false).missing.size(), 1u);
    EXPECT_TRUE(terrain::coverage(d, 0, true).missing.empty());
}

TEST(TerrainCoverageCheck, AnyMatchesEveryValue){
    TilesetData d;
    TerrainSetDef set;
    set.mode = TerrainMode::Blob;
    set.terrains = {{"Grass", "#00ff00"}};
    d.terrainSets.push_back(set);
    TileDef& tile = d.addTile(0, 0, 0);
    tile.terrain.set = 0;
    tile.terrain.center = 0;
    EXPECT_TRUE(terrain::coverage(d, 0, false).missing.empty());
}

TEST(TerrainCoverageCheck, DualGridNeedsFifteenMasksPerTerrainAgainstNone){
    TilesetData d = blobBlock(4, 4);
    d.terrainSets[0].mode = TerrainMode::DualGrid;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::DualGrid4x4, 0, TerrainTemplateOptions{}, problems));
    const TerrainCoverage coverage = terrain::coverage(d, 0, false);
    EXPECT_EQ(coverage.required, 15u);
    EXPECT_TRUE(coverage.missing.empty());
}

TEST(TerrainCoverageCheck, TwoSurfaceNeedsSixteenMasks){
    TilesetData d = blobBlock(4, 4);
    d.terrainSets[0].mode = TerrainMode::DualGrid;
    d.terrainSets[0].twoSurface = true;
    d.terrainSets[0].terrains = {{"Sand", "#fff000"}, {"Water", "#0000ff"}};
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::DualGrid4x4, 0, TerrainTemplateOptions{}, problems));
    TerrainCoverage coverage = terrain::coverage(d, 0, false);
    EXPECT_EQ(coverage.required, 16u);
    EXPECT_TRUE(coverage.missing.empty());
    d.removeTile(12);
    coverage = terrain::coverage(d, 0, false);
    ASSERT_EQ(coverage.missing.size(), 1u);
    EXPECT_EQ(coverage.missing[0].peering[1], 1);
    EXPECT_EQ(coverage.missing[0].peering[7], 1);
}

TEST(TerrainCoverageCheck, SidesNeedsSixteenMasks){
    TilesetData d = blobBlock(4, 4);
    d.terrainSets[0].mode = TerrainMode::Sides;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(terrain::applyTemplate(d, 0, TerrainTemplate::Sides16, 0, TerrainTemplateOptions{}, problems));
    const TerrainCoverage coverage = terrain::coverage(d, 0, false);
    EXPECT_EQ(coverage.required, 16u);
    EXPECT_TRUE(coverage.missing.empty());
}
