// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Duplicate Tile (ProjectUtils::buildDuplicateTileCmd) on a tilemap of any size up to the tiles a
// Tilemap draws (mesh-memory spec D7): it refused at the old 2,048-tile capacity.

#include "Project.h"
#include "Scene.h"
#include "command/Command.h"
#include "component/TilemapComponent.h"
#include "util/ProjectUtils.h"

#include <gtest/gtest.h>

#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct TilemapProject {
        Project project;
        Entity entity = NULL_ENTITY;

        explicit TilemapProject(unsigned int tiles){
            SceneProject scene;
            scene.id = 1;
            scene.name = "Tiles";
            scene.scene = new Scene();
            project.getScenes().push_back(scene);
            Scene* registry = project.getScenes().back().scene;
            entity = registry->createEntity();
            TilemapComponent tilemap;
            tilemap.numTilesRect = 1;
            tilemap.tilesRect[0] = {"rect", 0, Rect(0.0f, 0.0f, 16.0f, 16.0f)};
            tilemap.numTiles = tiles;
            for (unsigned int i = 0; i < tiles; i++)
                tilemap.tiles[i] = {"tile" + std::to_string(i), 0, Vector2((float)i, 0.0f), 16.0f, 16.0f};
            registry->addComponent<TilemapComponent>(entity, tilemap);
        }

        ~TilemapProject(){
            for (SceneProject& scene : project.getScenes()){
                delete scene.scene;
                scene.scene = nullptr;
            }
        }

        TilemapComponent& tilemap(){
            return project.getScenes().back().scene->getComponent<TilemapComponent>(entity);
        }
    };

}

TEST(ProjectUtilsTile, DuplicateTilePastTheOldCapacity){
    TilemapProject fixture(2048);
    std::unique_ptr<Command> duplicate(ProjectUtils::buildDuplicateTileCmd(&fixture.project, 1, fixture.entity, 5));
    ASSERT_NE(duplicate, nullptr) << "Duplicate Tile is refused at 2,048 tiles";
    ASSERT_TRUE(duplicate->execute());
    EXPECT_EQ(fixture.tilemap().numTiles, 2049u);
    EXPECT_EQ(fixture.tilemap().tiles[2048].name, "tile5");
    duplicate->undo();
    EXPECT_EQ(fixture.tilemap().numTiles, 2048u);
}

// up to the tiles a Tilemap draws (MAX_TILEMAP_RENDERED_TILES): one more would not draw
TEST(ProjectUtilsTile, DuplicateTileRefusedAtTheDrawLimit){
    TilemapProject fixture(MAX_TILEMAP_RENDERED_TILES);
    EXPECT_EQ(ProjectUtils::buildDuplicateTileCmd(&fixture.project, 1, fixture.entity, 5), nullptr);
    TilemapProject below(MAX_TILEMAP_RENDERED_TILES - 1);
    std::unique_ptr<Command> duplicate(ProjectUtils::buildDuplicateTileCmd(&below.project, 1, below.entity, 5));
    ASSERT_NE(duplicate, nullptr);
    ASSERT_TRUE(duplicate->execute());
    EXPECT_EQ(below.tilemap().numTiles, (unsigned int)MAX_TILEMAP_RENDERED_TILES);
}
