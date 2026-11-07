// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R65, R90 (AC71): the overlay sources bake from the terrain's maps through one interface.

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "terrain/TerrainOverlay.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr int OVERLAY_MAP = 17;

    // A ramp along X: the left edge at 0, the right edge at the top
    Entity overlayTerrain(Scene& scene) {
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        TerrainComponent component;
        component.terrainSize = 16.0f;
        component.maxHeight = 8.0f;
        scene.addComponent<TerrainComponent>(entity, component);
        const size_t bytes = static_cast<size_t>(OVERLAY_MAP) * OVERLAY_MAP * 2;
        uint16_t* raw = static_cast<uint16_t*>(std::malloc(bytes));
        for (int y = 0; y < OVERLAY_MAP; y++) {
            for (int x = 0; x < OVERLAY_MAP; x++) raw[y * OVERLAY_MAP + x] = static_cast<uint16_t>(x * 65535 / (OVERLAY_MAP - 1));
        }
        TextureData data(OVERLAY_MAP, OVERLAY_MAP, static_cast<unsigned int>(bytes), ColorFormat::RED16, 1, raw);
        data.setDataOwned(false);
        Texture& texture = scene.getComponent<TerrainComponent>(entity).heightMap;
        texture.setData("t1-overlay-ramp", data);
        texture.getData().setDataOwned(true);
        return entity;
    }

}

TEST(TerrainOverlay, BuiltInSourcesBakeTheMaps) {
    Scene scene;
    const Entity entity = overlayTerrain(scene);
    for (const char* name : {"height", "slope", "contours", "layer_weights"}) EXPECT_TRUE(TerrainOverlay::hasSource(name)) << name;

    TerrainOverlaySettings settings;
    settings.source = "height";
    std::vector<uint8_t> rgba;
    std::vector<TerrainOverlayLegendStop> legend;
    std::string error;
    ASSERT_TRUE(TerrainOverlay::bake(&scene, entity, settings, 0, 0, rgba, legend, error)) << error;
    ASSERT_EQ(rgba.size(), static_cast<size_t>(OVERLAY_MAP) * OVERLAY_MAP * 4) << "the image follows the height map";
    EXPECT_NE(std::memcmp(&rgba[0], &rgba[(OVERLAY_MAP - 1) * 4], 3), 0) << "low and high ends differ in colour";
    ASSERT_EQ(legend.size(), 5u);
    EXPECT_EQ(legend.front().label, "0 m");
    EXPECT_EQ(legend.back().label, "8 m");

    // a 30 degree ramp (8 m over 16 m is 26.6 degrees): the second band everywhere
    settings.source = "slope";
    ASSERT_TRUE(TerrainOverlay::bake(&scene, entity, settings, 8, 8, rgba, legend, error)) << error;
    EXPECT_EQ(legend.size(), 4u);
    const size_t middle = (4 * 8 + 4) * 4;
    EXPECT_GT(rgba[middle + 0], 200) << "yellow: 15 to 30 degrees";
    EXPECT_GT(rgba[middle + 1], 180);

    settings.source = "contours";
    settings.contourInterval = 2.0f;
    ASSERT_TRUE(TerrainOverlay::bake(&scene, entity, settings, 64, 1, rgba, legend, error)) << error;
    int lines = 0;
    for (int x = 0; x < 64; x++) lines += rgba[static_cast<size_t>(x) * 4 + 3] > 0 ? 1 : 0;
    EXPECT_GE(lines, 3) << "a line every 2 m over 8 m";
    EXPECT_LE(lines, 5);
}

// AC71 (R90): a source registered by another spec bakes and has a legend through the same call
TEST(TerrainOverlay, RegisteredSourceBakesAndHasLegend) {
    TerrainOverlay::registerSource("t1_test_checker",
        [](const TerrainOverlayBake& context, std::vector<uint8_t>& rgba) {
            for (int y = 0; y < context.height; y++) {
                for (int x = 0; x < context.width; x++) rgba[(static_cast<size_t>(y) * context.width + x) * 4 + 3] = (x + y) % 2 ? 255 : 0;
            }
        },
        [](const TerrainOverlayBake&) { return std::vector<TerrainOverlayLegendStop>{{"checked", Vector4(1, 1, 1, 1)}}; });
    EXPECT_TRUE(TerrainOverlay::hasSource("t1_test_checker"));

    Scene scene;
    const Entity entity = overlayTerrain(scene);
    TerrainOverlaySettings settings;
    settings.source = "t1_test_checker";
    std::vector<uint8_t> rgba;
    std::vector<TerrainOverlayLegendStop> legend;
    std::string error;
    ASSERT_TRUE(TerrainOverlay::bake(&scene, entity, settings, 4, 4, rgba, legend, error)) << error;
    EXPECT_EQ(rgba[3], 0);
    EXPECT_EQ(rgba[7], 255);
    ASSERT_EQ(legend.size(), 1u);
    EXPECT_EQ(legend[0].label, "checked");

    settings.source = "t1_no_such_source";
    EXPECT_FALSE(TerrainOverlay::bake(&scene, entity, settings, 4, 4, rgba, legend, error));
    EXPECT_NE(error.find("t1_no_such_source"), std::string::npos);
}

TEST(TerrainOverlay, SettingsRoundTrip) {
    TerrainOverlaySettings settings;
    EXPECT_EQ(encodeTerrainOverlaySettings(settings).size(), 0u) << "defaults write nothing";
    settings.source = "contours";
    settings.contourInterval = 5.0f;
    settings.opacity = 0.4f;
    TerrainOverlaySettings back;
    decodeTerrainOverlaySettings(encodeTerrainOverlaySettings(settings), back);
    EXPECT_EQ(back, settings);
}
