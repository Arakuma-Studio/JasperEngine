// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R58 (AC44): Create Terrain makes the GameObject, its sizes, a height map and a collider as one
// undo step; R10, R24: an op list through the shared undo helper is one step too.

#include "TerrainProjectFixture.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"

#include "command/CommandHistory.h"
#include "terrain/TerrainOpsUndo.h"
#include "terrain/TerrainSizePresets.h"
#include "window/dialog/CreateTerrainDialog.h"

using namespace terrainprojecttest;

namespace {

    class TerrainCreateDialogTest : public TerrainProjectFixture {};

}

TEST_F(TerrainCreateDialogTest, CreateIsOneUndoStep) {
    CreateTerrainDialog::Settings settings;
    settings.name = "Island";
    const TerrainSizePreset& small = TERRAIN_SIZE_PRESETS[2];
    settings.size = small.size;
    settings.height = small.height;
    settings.heightmap = 65;
    settings.meshResolution = small.meshResolution;
    settings.rootGrid = small.root;
    settings.levels = small.levels;
    settings.baseHeight = 16.0f;
    std::string error;
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t before = history->position();
    const Entity island = CreateTerrainDialog::create(project.get(), scene->id, NULL_ENTITY, settings, error);
    ASSERT_NE(island, NULL_ENTITY) << error;
    EXPECT_EQ(history->position(), before + 1) << "one undo step";
    EXPECT_EQ(scene->scene->getEntityName(island), "Island");

    TerrainComponent& made = scene->scene->getComponent<TerrainComponent>(island);
    EXPECT_EQ(made.terrainSize, 256.0f);
    EXPECT_EQ(made.maxHeight, 64.0f);
    TerrainComponentStore store(scene->scene, island, made);
    const terrain::MapFormat format = store.format(HEIGHT_MAP);
    ASSERT_TRUE(format.exists);
    EXPECT_EQ(format.bounds.w, 65);
    EXPECT_EQ(format.bitsPerChannel, 16);
    EXPECT_NEAR(querySurface(made, 0.0f, 0.0f).height, 16.0f, 0.01f) << "flat at Base Height";
    const RigidbodyComponent* body = scene->scene->findComponent<RigidbodyComponent>(island);
    ASSERT_NE(body, nullptr);
    ASSERT_EQ(body->colliders.size(), 1u);
    EXPECT_EQ(body->colliders[0].type, Collider3DType::HEIGHTFIELD);
    EXPECT_EQ(body->colliders[0].sourceEntity, island);

    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(scene->scene->isEntityCreated(island)) << "undo takes the whole terrain away";
    ASSERT_TRUE(history->redo());
    EXPECT_TRUE(scene->scene->isEntityCreated(island));
}

TEST_F(TerrainCreateDialogTest, MemoryLineWarnsPerPlatform) {
    CreateTerrainDialog::Settings settings;
    settings.heightmap = 4097;
    const std::string line = CreateTerrainDialog::memoryLine(settings);
    EXPECT_NE(line.find("mobile and web"), std::string::npos) << line;
    EXPECT_NE(line.find("Collision samples 1025"), std::string::npos) << line;
    settings.heightmap = 513;
    EXPECT_EQ(CreateTerrainDialog::memoryLine(settings).find("mobile"), std::string::npos);
}

// R10, R24, R28: a list that makes a map and writes heights is undone and redone as a whole
TEST_F(TerrainCreateDialogTest, OpsWithUndoMakeMapsAndOneGroup) {
    giveHeight16(33);
    settleFiles();
    const std::vector<unsigned char> heightsBefore = bytesOf(HEIGHT_MAP);
    terrain::Op raise;
    raise.type = terrain::OpType::Raise;
    raise.delta = 1.0f;
    terrain::Op paint;
    paint.type = terrain::OpType::PaintLayer;
    paint.layer = 1;
    const std::vector<terrain::Op> ops = {raise, paint};
    const terrain::TerrainOpResult preview = previewTerrainOps(project.get(), scene, terrainEntity, ops, terrain::OpOptions());
    ASSERT_TRUE(preview.ok) << preview.error;
    ASSERT_FALSE(preview.createdMaps.empty()) << "painting makes the blend map";

    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t start = history->mark();
    terrain::TerrainOpResult reported;
    std::string error;
    ASSERT_TRUE(applyTerrainOpsWithUndo(project.get(), scene, terrainEntity, ops, terrain::OpOptions(), preview, "Edit Terrain", reported, error)) << error;
    ASSERT_TRUE(history->groupSince(start, "Edit Terrain"));
    EXPECT_EQ(reported.changed.size(), preview.changed.size()) << "the numbers are the preview's";
    EXPECT_NE(bytesOf(HEIGHT_MAP), heightsBefore);
    TerrainComponentStore store(scene->scene, terrainEntity, terrain());
    EXPECT_TRUE(store.format(BLEND_MAP).exists);

    ASSERT_TRUE(history->undo());
    EXPECT_EQ(bytesOf(HEIGHT_MAP), heightsBefore);
    EXPECT_FALSE(TerrainComponentStore(scene->scene, terrainEntity, terrain()).format(BLEND_MAP).exists) << "the made map goes with the step";
    ASSERT_TRUE(history->redo());
    EXPECT_NE(bytesOf(HEIGHT_MAP), heightsBefore);
}
