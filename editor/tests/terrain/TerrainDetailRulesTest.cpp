// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Terrain detail layers (Unity's Details, TerrainComponent::foliageLayers): the ranges the engine
// can draw, shared by edit_terrain_details, set_properties and the scene decoder. A hand-edited or
// agent-written layer with a negative density or inverted ranges used to load as written.

#include <gtest/gtest.h>

#include <algorithm>

#include "Stream.h"
#include "util/TerrainDetailRules.h"

#include <yaml-cpp/yaml.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(TerrainDetailRules, DefaultsAreValid){
    EXPECT_EQ(terraindetail::checkDetailLayer(TerrainFoliageLayer()), "");
}

TEST(TerrainDetailRules, EveryRangeIsChecked){
    for (const terraindetail::DetailRange& range : terraindetail::detailRanges()){
        TerrainFoliageLayer below;
        below.*range.field = range.min - 1.0f;
        if (range.min - 1.0f >= range.min) continue;
        const std::string why = terraindetail::checkDetailLayer(below);
        EXPECT_NE(why.find(range.key), std::string::npos) << range.key << ": " << why;
        TerrainFoliageLayer above;
        above.*range.field = range.max + 1.0f;
        EXPECT_NE(terraindetail::checkDetailLayer(above).find(range.key), std::string::npos) << range.key;
    }
    TerrainFoliageLayer dense;
    dense.density = 500.0f;
    EXPECT_NE(terraindetail::checkDetailLayer(dense).find("8192 per 8 m cell"), std::string::npos);
}

TEST(TerrainDetailRules, PairsAndMeshes){
    TerrainFoliageLayer layer;
    layer.minScale = 3.0f;
    layer.maxScale = 1.0f;
    EXPECT_NE(terraindetail::checkDetailLayer(layer).find("min_scale"), std::string::npos);
    layer = TerrainFoliageLayer();
    layer.minSlope = 50.0f;
    EXPECT_NE(terraindetail::checkDetailLayer(layer).find("max_slope"), std::string::npos);
    layer = TerrainFoliageLayer();
    layer.meshPath = "models/grass.png";
    EXPECT_NE(terraindetail::checkDetailLayer(layer).find(".glb"), std::string::npos);
    EXPECT_TRUE(terraindetail::isDetailMeshPath("models/Grass.GLB"));
    EXPECT_TRUE(terraindetail::isDetailMeshPath("models/rock.obj"));
    EXPECT_FALSE(terraindetail::isDetailMeshPath("models/rock"));
    // every model the import pipeline loads: an FBX draws from its import (the Terrain window assigns them)
    EXPECT_TRUE(terraindetail::isDetailMeshPath("models/tree.FBX"));
    EXPECT_FALSE(terraindetail::isDetailMeshPath("models/tree.png"));
    layer = TerrainFoliageLayer();
    layer.meshPath = "models/tree.fbx";
    EXPECT_EQ(terraindetail::checkDetailLayer(layer), "");
    layer = TerrainFoliageLayer();
    layer.minDrawDistance = 80.0f;   // past drawDistance (50)
    EXPECT_NE(terraindetail::checkDetailLayer(layer).find("min_draw_distance"), std::string::npos);
}

TEST(TerrainDetailRules, ClampReportsWhatChanged){
    TerrainFoliageLayer layer;
    layer.density = -5.0f;
    layer.minScale = 3.0f;
    layer.maxScale = 1.0f;
    const std::vector<std::string> changed = terraindetail::clampDetailLayer(layer);
    EXPECT_EQ(layer.density, 0.0f);
    EXPECT_EQ(layer.minScale, 1.0f);
    EXPECT_EQ(layer.maxScale, 3.0f);
    ASSERT_EQ(changed.size(), 2u);
    EXPECT_EQ(terraindetail::checkDetailLayer(layer), "");
}

// the decoder clamps what the renderer cannot use (red before the fix: kept as written)
TEST(TerrainDetailRules, DecoderClampsOutOfRangeValues){
    const YAML::Node node = YAML::Load("{density: -5, minScale: 3, maxScale: 1, maxSlope: 400, drawDistance: 0}");
    const TerrainFoliageLayer layer = Stream::decodeTerrainFoliageLayer(node);
    EXPECT_EQ(layer.density, 0.0f);
    EXPECT_EQ(layer.minScale, 1.0f);
    EXPECT_EQ(layer.maxScale, 3.0f);
    EXPECT_EQ(layer.maxSlope, 90.0f);
    EXPECT_EQ(layer.drawDistance, 1.0f);
}

// T5 AC42: the foliage fields are in the table, so edit_terrain_details, set_properties and the load
// clamp all hold them
TEST(TerrainDetailRules, FoliageFieldsAreInTheTable){
    std::vector<std::string> keys;
    for (const terraindetail::DetailRange& range : terraindetail::detailRanges()) keys.push_back(range.key);
    for (const char* key : {"min_height_ratio", "max_height_ratio", "noise_spread", "wind_bend", "density_falloff_start", "far_density", "density_scale"}){
        EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end()) << key;
    }
    TerrainFoliageLayer layer;
    layer.minHeightRatio = 1.5f;
    layer.maxHeightRatio = 0.5f;
    EXPECT_NE(terraindetail::checkDetailLayer(layer).find("min_height_ratio"), std::string::npos);
    const TerrainFoliageLayer loaded = Stream::decodeTerrainFoliageLayer(YAML::Load("{windBend: 5, farDensity: -1}"));
    EXPECT_EQ(loaded.windBend, 1.0f);
    EXPECT_EQ(loaded.farDensity, 0.0f);
}
