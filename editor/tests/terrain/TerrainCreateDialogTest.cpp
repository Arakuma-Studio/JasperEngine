// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R58 (AC44): Create Terrain makes the GameObject, its sizes, a height map and a collider as one
// undo step, its Start Shape Import Raw... and Base Layer included; R10, R24: an op list through the
// shared undo helper is one step too.

#include "TerrainProjectFixture.h"
#include "component/RigidbodyComponent.h"

#include "command/CommandHistory.h"
#include "terrain/TerrainOpsUndo.h"
#include "terrain/TerrainRawIO.h"
#include "terrain/TerrainSizePresets.h"
#include "window/dialog/CreateTerrainDialog.h"

using namespace terrainprojecttest;

namespace {

    class TerrainCreateDialogTest : public TerrainProjectFixture {};

    CreateTerrainDialog::Settings smallSettings(const std::string& name, int heightmap) {
        CreateTerrainDialog::Settings settings;
        settings.name = name;
        const TerrainSizePreset& small = TERRAIN_SIZE_PRESETS[2];
        settings.size = small.size;
        settings.height = small.height;
        settings.heightmap = heightmap;
        settings.meshResolution = small.meshResolution;
        settings.rootGrid = small.root;
        settings.levels = small.levels;
        settings.collider = false;
        return settings;
    }

    // A little-endian 16-bit file whose rows and columns differ, so a flip or a transpose shows
    HeightImport rampFile(int width, int height) {
        std::vector<uint8_t> bytes(static_cast<size_t>(width) * height * 2);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const uint16_t value = static_cast<uint16_t>((x * 977 + y * 131) % 65536);
                const size_t at = (static_cast<size_t>(y) * width + x) * 2;
                bytes[at] = static_cast<uint8_t>(value & 0xFF);
                bytes[at + 1] = static_cast<uint8_t>(value >> 8);
            }
        }
        HeightImportOptions options;
        options.width = width;
        options.height = height;
        options.byteOrder = HeightByteOrder::Little;
        options.range = HeightRange::Full;
        return TerrainRawIO::decode("ramp.r16", bytes, options);
    }

    uint64_t heightHash(Scene* scene, Entity entity) {
        TerrainComponentStore store(scene, entity, scene->getComponent<TerrainComponent>(entity));
        return terrain::mapHash(store, HEIGHT_MAP);
    }

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

TEST_F(TerrainCreateDialogTest, ImportRawStartShapeIsTheImportedFile) {
    const HeightImport imported = rampFile(33, 33);
    ASSERT_TRUE(imported.ok) << imported.error;
    CreateTerrainDialog::Settings settings = smallSettings("Imported", 33);
    settings.shape = CreateTerrainDialog::StartShape::ImportRaw;
    settings.imported = imported;
    std::string error;
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t before = history->position();
    const Entity made = CreateTerrainDialog::create(project.get(), scene->id, NULL_ENTITY, settings, error);
    ASSERT_NE(made, NULL_ENTITY) << error;
    EXPECT_EQ(history->position(), before + 1) << "one undo step";

    // the same file through the Terrain window's import (the import_heightmap op) on a flat terrain
    CreateTerrainDialog::Settings flat = smallSettings("Flat", 33);
    const Entity reference = CreateTerrainDialog::create(project.get(), scene->id, NULL_ENTITY, flat, error);
    ASSERT_NE(reference, NULL_ENTITY) << error;
    terrain::Op op;
    op.type = terrain::OpType::ImportHeightmap;
    op.importedWidth = imported.width;
    op.importedHeight = imported.height;
    op.importedHeights = imported.normalized;
    const std::vector<terrain::Op> ops = {op};
    const terrain::TerrainOpResult preview = previewTerrainOps(project.get(), scene, reference, ops, terrain::OpOptions());
    ASSERT_TRUE(preview.ok) << preview.error;
    terrain::TerrainOpResult reported;
    const size_t importStart = history->mark();
    ASSERT_TRUE(applyTerrainOpsWithUndo(project.get(), scene, reference, ops, terrain::OpOptions(), preview, "Import", reported, error)) << error;
    ASSERT_TRUE(history->groupSince(importStart, "Import"));
    EXPECT_EQ(heightHash(scene->scene, made), heightHash(scene->scene, reference)) << "Create's heights are the import's";

    ASSERT_TRUE(history->undo());
    ASSERT_TRUE(history->undo());
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(scene->scene->isEntityCreated(made)) << "undo takes the imported terrain away";
}

TEST_F(TerrainCreateDialogTest, ImportRawRefusesWhatItCannotMake) {
    CreateTerrainDialog::Settings settings = smallSettings("Imported", 33);
    settings.shape = CreateTerrainDialog::StartShape::ImportRaw;
    std::string error;
    EXPECT_EQ(CreateTerrainDialog::create(project.get(), scene->id, NULL_ENTITY, settings, error), NULL_ENTITY);
    EXPECT_NE(error.find("choose a heightmap file"), std::string::npos) << error;

    settings.imported = rampFile(33, 17);
    ASSERT_TRUE(settings.imported.ok) << settings.imported.error;
    error.clear();
    EXPECT_EQ(CreateTerrainDialog::create(project.get(), scene->id, NULL_ENTITY, settings, error), NULL_ENTITY);
    EXPECT_NE(error.find("Terrain window"), std::string::npos) << "names the route that takes any size: " << error;
}

TEST_F(TerrainCreateDialogTest, BaseLayerCoversTheTerrainInTheSameStep) {
    CreateTerrainDialog::Settings settings = smallSettings("Grass", 17);
    settings.control = 32;
    settings.baseLayer = "textures/grass.png";
    std::string error;
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t before = history->position();
    const Entity made = CreateTerrainDialog::create(project.get(), scene->id, NULL_ENTITY, settings, error);
    ASSERT_NE(made, NULL_ENTITY) << error;
    EXPECT_EQ(history->position(), before + 1) << "one undo step";

    // what sample_terrain's layer coverage reads: layer 0 in the first blend map's red channel, everywhere
    auto coverage = [&]() {
        TerrainComponent& terrain = scene->scene->getComponent<TerrainComponent>(made);
        EXPECT_EQ(terrain.surfaceLayers.size(), 1u);
        if (terrain.surfaceLayers.empty()) return -1.0;
        EXPECT_EQ(terrain.surfaceLayers[0].colorTexture.getPath(0), "textures/grass.png");
        TerrainComponentStore store(scene->scene, made, terrain);
        const terrain::MapFormat format = store.format(BLEND_MAP);
        EXPECT_TRUE(format.exists);
        if (!format.exists) return -1.0;
        EXPECT_EQ(format.bounds.w, 32);
        std::vector<float> texels(static_cast<size_t>(format.bounds.area()) * format.channels);
        EXPECT_TRUE(store.read(BLEND_MAP, format.bounds, texels));
        size_t covered = 0;
        for (size_t i = 0; i < texels.size(); i += 4) {
            if (texels[i] > 0.5f && texels[i + 1] < 0.5f && texels[i + 2] < 0.5f) covered++;
        }
        return static_cast<double>(covered) / static_cast<double>(format.bounds.area());
    };
    EXPECT_EQ(coverage(), 1.0) << "Terrain Layer 0 covers the whole terrain";

    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(scene->scene->isEntityCreated(made)) << "one undo takes the terrain, its layer and its blend map away";
    ASSERT_TRUE(history->redo());
    ASSERT_TRUE(scene->scene->isEntityCreated(made));
    EXPECT_EQ(coverage(), 1.0) << "redo brings them back";
}
