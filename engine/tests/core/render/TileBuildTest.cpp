// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The screen-tile light lists' CPU build (render/TiledLights.h, docs/specs/S57-forge-lighting.md
// R1): the projection of a light's sphere to a screen rect, the grid, the caps and the order the
// tiles list their lights in.

#include "render/TiledLights.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine;

namespace {

    // a 1600x900 perspective camera at the origin looking down -z (60 degrees of vertical field)
    TiledLights::View perspectiveView(int columns = 16, int rows = 9){
        TiledLights::View view;
        view.viewMatrix = Matrix4();
        view.projectionMatrix = Matrix4::perspectiveMatrix(1.0471976f, 1600.0f / 900.0f, 0.1f, 100.0f);
        view.perspective = true;
        view.nearClip = 0.1f;
        view.farClip = 100.0f;
        view.cameraPosition = Vector3(0.0f, 0.0f, 0.0f);
        view.width = 1600;
        view.height = 900;
        view.fragYUp = false;
        view.grid = TiledLights::makeGrid(view.width, view.height, columns, rows);
        return view;
    }

    TiledLights::Light pointLight(const Vector3& position, float range, int sceneIndex){
        TiledLights::Light light;
        light.position = position;
        light.range = range;
        light.sceneIndex = sceneIndex;
        // a record that names it: position_type.x carries its scene index
        light.record[0] = Vector4((float)sceneIndex, 0.0f, 0.0f, 1.0f + range);
        return light;
    }

    // the tile's count and its view-local indices
    std::vector<int> tileList(const std::vector<float>& texels, const TiledLights::Grid& grid, int x, int y){
        const size_t header = ((size_t)TiledLights::LIGHT_TEXELS + (size_t)(x + y * grid.columns) * TiledLights::TILE_TEXELS) * 4;
        const int count = (int)texels[header];
        std::vector<int> list;
        for (int i = 0; i < count; i++)
            list.push_back((int)texels[header + 4 + (size_t)i]);
        return list;
    }

}

TEST(TileBuild, GridTilesAreEvenAndCoverTheViewport){
    for (int width : {1, 2, 641, 1600, 1919, 3840}){
        for (int height : {1, 3, 360, 899, 2160}){
            const TiledLights::Grid grid = TiledLights::makeGrid(width, height, 16, 9);
            EXPECT_EQ(grid.tileWidth % 2, 0) << width << "x" << height;
            EXPECT_EQ(grid.tileHeight % 2, 0) << width << "x" << height;
            EXPECT_GE(grid.tileWidth * grid.columns, width);
            EXPECT_GE(grid.tileHeight * grid.rows, height);
        }
    }
    EXPECT_EQ(TiledLights::textureRows(TiledLights::makeGrid(1600, 900, 16, 9)), 6);
    EXPECT_EQ(TiledLights::textureRows(TiledLights::makeGrid(1600, 900, 32, 18)), 14);
}

TEST(TileBuild, SphereInFrontProjectsAroundItsCentre){
    const TiledLights::View view = perspectiveView();
    float rect[4];
    ASSERT_TRUE(TiledLights::projectSphere(view, Vector3(0.0f, 0.0f, -10.0f), 1.0f, rect));
    // centred, a bit over 2 / (2 * 10 * tan 30) of the height (the tangents, not the radius)
    EXPECT_NEAR((rect[0] + rect[2]) * 0.5f, 800.0f, 1.0f);
    EXPECT_NEAR((rect[1] + rect[3]) * 0.5f, 450.0f, 1.0f);
    const float expected = 900.0f * (1.0f / (10.0f * 0.57735f));
    EXPECT_GT(rect[3] - rect[1], expected * 0.99f);
    EXPECT_LT(rect[3] - rect[1], expected * 1.05f);
}

TEST(TileBuild, SphereRightOfCentreProjectsRight){
    const TiledLights::View view = perspectiveView();
    float rect[4];
    ASSERT_TRUE(TiledLights::projectSphere(view, Vector3(5.0f, 0.0f, -10.0f), 0.5f, rect));
    EXPECT_GT(rect[0], 800.0f);
    // with fragYUp the rows count from the bottom: a sphere above the centre has rows over half
    TiledLights::View up = view;
    up.fragYUp = true;
    ASSERT_TRUE(TiledLights::projectSphere(up, Vector3(0.0f, 3.0f, -10.0f), 0.5f, rect));
    EXPECT_GT(rect[1], 450.0f);
    ASSERT_TRUE(TiledLights::projectSphere(view, Vector3(0.0f, 3.0f, -10.0f), 0.5f, rect));
    EXPECT_LT(rect[3], 450.0f);
}

TEST(TileBuild, SphereBehindTheCameraOrOutsideMisses){
    const TiledLights::View view = perspectiveView();
    float rect[4];
    EXPECT_FALSE(TiledLights::projectSphere(view, Vector3(0.0f, 0.0f, 10.0f), 1.0f, rect));
    EXPECT_FALSE(TiledLights::projectSphere(view, Vector3(-200.0f, 0.0f, -10.0f), 1.0f, rect))
        << "rect " << rect[0] << " " << rect[1] << " " << rect[2] << " " << rect[3];
    EXPECT_FALSE(TiledLights::projectSphere(view, Vector3(0.0f, 0.0f, -150.0f), 1.0f, rect));
}

TEST(TileBuild, SphereReachingTheNearPlaneCoversTheView){
    const TiledLights::View view = perspectiveView();
    float rect[4];
    // the camera inside its range
    ASSERT_TRUE(TiledLights::projectSphere(view, Vector3(0.5f, 0.0f, -1.0f), 3.0f, rect));
    EXPECT_FLOAT_EQ(rect[0], 0.0f);
    EXPECT_FLOAT_EQ(rect[1], 0.0f);
    EXPECT_FLOAT_EQ(rect[2], 1600.0f);
    EXPECT_FLOAT_EQ(rect[3], 900.0f);
}

TEST(TileBuild, OrthographicSphereIsItsSquare){
    TiledLights::View view = perspectiveView();
    view.perspective = false;
    view.projectionMatrix = Matrix4::orthoMatrix(-8.0f, 8.0f, -4.5f, 4.5f, 0.1f, 100.0f);
    float rect[4];
    ASSERT_TRUE(TiledLights::projectSphere(view, Vector3(4.0f, 0.0f, -10.0f), 1.0f, rect));
    EXPECT_NEAR(rect[0], 1100.0f, 0.5f);
    EXPECT_NEAR(rect[2], 1300.0f, 0.5f);
    EXPECT_NEAR(rect[1], 350.0f, 0.5f);
    EXPECT_NEAR(rect[3], 550.0f, 0.5f);
}

TEST(TileBuild, TilesListTheLightsReachingThemInSceneOrder){
    const TiledLights::View view = perspectiveView();
    std::vector<TiledLights::Light> lights;
    // a small light at the centre, a directional (global) one, one off screen
    lights.push_back(pointLight(Vector3(0.0f, 0.0f, -20.0f), 0.5f, 3));
    TiledLights::Light sun;
    sun.global = true;
    sun.sceneIndex = 1;
    sun.record[0] = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
    lights.push_back(sun);
    lights.push_back(pointLight(Vector3(0.0f, 0.0f, 20.0f), 0.5f, 5));

    TiledLights builder;
    std::vector<float> texels;
    TiledLights::Stats stats;
    builder.build(view, lights, texels, stats);

    EXPECT_EQ(stats.lights, 2);
    ASSERT_EQ(builder.getViewLights(), (std::vector<int>{1, 3}));
    // view-local records in scene order: 0 the sun, 1 the point light
    EXPECT_FLOAT_EQ(texels[0], 1.0f);
    EXPECT_FLOAT_EQ(texels[(size_t)TiledLights::LIGHT_RECORD_TEXELS * 4], 3.0f);
    // the centre tile has both, ascending; a corner the sun alone
    EXPECT_EQ(tileList(texels, view.grid, 8, 4), (std::vector<int>{0, 1}));
    EXPECT_EQ(tileList(texels, view.grid, 0, 0), (std::vector<int>{0}));
    EXPECT_EQ(stats.maxPerTile, 2);
    EXPECT_GT(stats.averagePerTile, 1.0f);
    EXPECT_LT(stats.averagePerTile, 1.1f);
}

TEST(TileBuild, PastTheTileCapTheNearestWin){
    const TiledLights::View view = perspectiveView();
    std::vector<TiledLights::Light> lights;
    // 40 lights reaching the centre tile, the farther the higher their scene index
    for (int i = 0; i < 40; i++)
        lights.push_back(pointLight(Vector3(0.0f, 0.0f, -5.0f - (float)i), 1.0f, i));
    TiledLights builder;
    std::vector<float> texels;
    TiledLights::Stats stats;
    builder.build(view, lights, texels, stats);

    const std::vector<int> centre = tileList(texels, view.grid, 8, 4);
    ASSERT_EQ((int)centre.size(), TiledLights::MAX_LIGHTS_PER_TILE);
    EXPECT_GT(stats.droppedFromTiles, 0);
    // the 32 nearest: scene indices 0..31 (view-local indices are scene order)
    for (int i = 0; i < TiledLights::MAX_LIGHTS_PER_TILE; i++)
        EXPECT_EQ(builder.getViewLights()[(size_t)centre[(size_t)i]], i);
    EXPECT_TRUE(std::is_sorted(centre.begin(), centre.end()));
}

TEST(TileBuild, PastTheViewCapTheNearestWin){
    const TiledLights::View view = perspectiveView(32, 18);
    std::vector<TiledLights::Light> lights;
    // spread over the view, each farther from the camera than the one before
    for (int i = 0; i < 300; i++){
        const Vector3 direction = Vector3((float)(i % 20) - 10.0f, (float)((i / 20) % 15) * 0.5f - 3.5f, -10.0f).normalized();
        lights.push_back(pointLight(direction * (10.0f + (float)i * 0.1f), 0.3f, i));
    }
    TiledLights builder;
    std::vector<float> texels;
    TiledLights::Stats stats;
    builder.build(view, lights, texels, stats);
    EXPECT_EQ(stats.lights, TiledLights::MAX_LIGHTS_PER_VIEW);
    EXPECT_EQ(stats.droppedFromView, 300 - TiledLights::MAX_LIGHTS_PER_VIEW);
    // the farthest went: the view lists the 256 nearest, in scene order
    ASSERT_EQ((int)builder.getViewLights().size(), TiledLights::MAX_LIGHTS_PER_VIEW);
    for (int k = 0; k < TiledLights::MAX_LIGHTS_PER_VIEW; k++)
        EXPECT_EQ(builder.getViewLights()[(size_t)k], k);
    EXPECT_EQ(texels.size(), (size_t)TiledLights::TEXTURE_WIDTH * (size_t)TiledLights::textureRows(view.grid) * 4);
}
