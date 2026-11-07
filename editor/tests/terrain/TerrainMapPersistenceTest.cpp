// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// When an edited map reaches its file (T1 R29-R32; AC21, AC22)

#include "TerrainProjectFixture.h"
#include "component/TerrainComponent.h"

#include "command/type/TerrainMapDeltaCmd.h"
#include "util/PngWriter.h"
#include "util/TerrainMapFileWriter.h"
#include "window/TerrainEditWindow.h"

#include <fstream>
#include <sstream>

using namespace terrainprojecttest;

namespace {

    std::string fileText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    class TerrainMapPersistenceTest : public TerrainProjectFixture {
    protected:
        void SetUp() override {
            TerrainProjectFixture::SetUp();
            giveHeight16(64);
            settleFiles();
            onDisk = fileText(fileOf(HEIGHT_MAP));
            ASSERT_FALSE(onDisk.empty());
        }

        std::string onDisk;

        // A step that changed the map in memory
        void commitEdit(float delta = 3.0f) {
            EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
            terrain::OverlayMapStore overlay(store);
            ASSERT_TRUE(runOn(overlay, raiseOp(delta)).ok);
            auto* step = new TerrainMapDeltaCmd(project.get(), scene->id, terrainEntity, terrainEntity, overlay, false);
            overlay.discard();
            ASSERT_TRUE(history()->addCommandNoMerge(step));
        }

        // The file as the map's pixels would encode now
        std::vector<unsigned char> decodedFile() {
            TextureData data;
            EXPECT_TRUE(data.loadTextureFromFile(fileOf(HEIGHT_MAP).string().c_str()));
            data.setDataOwned(true);
            const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
            return std::vector<unsigned char>(pixels, pixels + data.getSize());
        }
    };

}

// AC21: a commit changes memory only; the writer sees nothing until the debounce
TEST_F(TerrainMapPersistenceTest, CommitDoesNotEncode) {
    TerrainMapPersistence& persistence = TerrainMapPersistence::get();
    const std::vector<unsigned char> before = bytesOf(HEIGHT_MAP);
    commitEdit();
    EXPECT_NE(bytesOf(HEIGHT_MAP), before);
    EXPECT_EQ(persistence.pendingCount(), 1u);

    // draining the writer alone finds no job: the file has not moved
    EXPECT_TRUE(TerrainMapFileWriter::get().flushAll());
    EXPECT_EQ(fileText(fileOf(HEIGHT_MAP)), onDisk);

    const double now = TerrainMapPersistence::clockSeconds();
    persistence.update(now);
    EXPECT_EQ(persistence.pendingCount(), 1u) << "a new edit restarted the second";
    EXPECT_FALSE(persistence.isDue(now));

    EXPECT_TRUE(persistence.isDue(now + TerrainMapPersistence::DEBOUNCE_SECONDS + 0.5));
    persistence.update(now + TerrainMapPersistence::DEBOUNCE_SECONDS + 0.5);
    EXPECT_EQ(persistence.pendingCount(), 0u);
    EXPECT_TRUE(TerrainMapFileWriter::get().flushAll());
    EXPECT_EQ(decodedFile(), bytesOf(HEIGHT_MAP));
}

TEST_F(TerrainMapPersistenceTest, UndoDoesNotEncode) {
    commitEdit();
    settleFiles();
    const std::string edited = fileText(fileOf(HEIGHT_MAP));
    ASSERT_NE(edited, onDisk);

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(TerrainMapPersistence::get().pendingCount(), 1u);
    EXPECT_TRUE(TerrainMapFileWriter::get().flushAll());
    EXPECT_EQ(fileText(fileOf(HEIGHT_MAP)), edited) << "undo changed memory only";

    settleFiles();
    EXPECT_EQ(decodedFile(), bytesOf(HEIGHT_MAP));
}

// R29: no write during a stroke, however long it holds
TEST_F(TerrainMapPersistenceTest, AStrokeHoldsTheFile) {
    TerrainMapPersistence& persistence = TerrainMapPersistence::get();
    {
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        store.beginStroke();
        terrain::OverlayMapStore overlay(store);
        ASSERT_TRUE(runOn(overlay, raiseOp(2.0f)).ok);
        ASSERT_TRUE(overlay.flushToBase().ok);
        ASSERT_EQ(persistence.pendingCount(), 1u);
        const double later = TerrainMapPersistence::clockSeconds() + 60.0;
        EXPECT_FALSE(persistence.isDue(later));
        persistence.update(later);
        EXPECT_EQ(persistence.pendingCount(), 1u);
        overlay.discard();
    }
    const double later = TerrainMapPersistence::clockSeconds() + 60.0;
    EXPECT_TRUE(persistence.isDue(later)) << "the store's end of the stroke let it go";
}

// R30: the forced points write the current pixels
TEST_F(TerrainMapPersistenceTest, FlushPoints) {
    // scene save
    commitEdit();
    EXPECT_TRUE(project->saveSceneToPath(scene->id, project->getProjectPath() / "t1_flush.scene"));
    EXPECT_EQ(decodedFile(), bytesOf(HEIGHT_MAP));
    EXPECT_EQ(TerrainMapPersistence::get().pendingCount(), 0u);

    // a reader that goes to disk
    commitEdit(1.0f);
    TextureData read;
    ASSERT_TRUE(TerrainMapUtils::loadTextureDataFromPath(project.get(), terrain().heightMap.getPath(0), read));
    const unsigned char* pixels = static_cast<const unsigned char*>(read.getData());
    EXPECT_EQ(std::vector<unsigned char>(pixels, pixels + read.getSize()), bytesOf(HEIGHT_MAP));

    // closing the project's writers
    commitEdit(1.0f);
    EXPECT_TRUE(TerrainMapPersistence::get().flushAll());
    EXPECT_EQ(decodedFile(), bytesOf(HEIGHT_MAP));
}

// AC22 (R31): a failed write stays with the writer, keeps the scene modified, and says which file
TEST_F(TerrainMapPersistenceTest, FailedWriteKeepsSceneModifiedAndRaisesIssue) {
    commitEdit();
    const fs::path target = fileOf(HEIGHT_MAP);
    // a directory where the file goes: no atomic replace can land (permissions would not stop root)
    fs::remove(target);
    fs::create_directories(target / "in-the-way");

    EXPECT_FALSE(TerrainMapPersistence::get().flushAll());
    const std::vector<std::string> failed = TerrainMapPersistence::get().failedPaths();
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(fs::path(failed[0]).filename(), target.filename());

    EXPECT_FALSE(project->saveSceneToPath(scene->id, project->getProjectPath() / "t1_failed.scene")) << "the save says the data is not durable";
    EXPECT_TRUE(scene->isModified);

    // made writable again: the next flush retries, and the issue clears
    fs::remove_all(target);
    EXPECT_TRUE(TerrainMapPersistence::get().flushAll());
    EXPECT_TRUE(TerrainMapPersistence::get().failedPaths().empty());
    EXPECT_EQ(decodedFile(), bytesOf(HEIGHT_MAP));
}
