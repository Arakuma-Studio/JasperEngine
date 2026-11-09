// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The maps folder's collector (TerrainEditWindow::cleanUnusedTerrainMaps), which every scene save runs:
// it keeps every file an open scene holds, saved or not, with its sidecar, and only those (T1 R32,
// T3 R29, T5 R61)

#include "TerrainProjectFixture.h"
#include "command/type/SetAssetMetaCmd.h"
#include "foliage/TreeDataFile.h"
#include "io/AtomicFile.h"
#include "texture/TextureImportSettings.h"
#include "window/TerrainEditWindow.h"

#include <yaml-cpp/yaml.h>

using namespace terrainprojecttest;

namespace {

    class TerrainMapCleanupTest : public TerrainProjectFixture {
    protected:
        fs::path absoluteOf(const std::string& assetPath) { return project->resolveAssetPath(assetPath); }

        // The database picks the new files up, as the editor's poll does: each gets its meta
        void refreshAssets() { project->getAssetDatabase().refresh(); }
    };

}

// A hole map only an open, unsaved scene names (the save of another scene runs the collector)
TEST_F(TerrainMapCleanupTest, KeepsTheHoleMapOfAnUnsavedScene) {
    giveHeight16(16);
    ASSERT_TRUE(TerrainMapUtils::ensureEditableMap(project.get(), scene, terrainEntity, TerrainMapRef(TerrainMapTarget::HoleMap), 16, false));
    madeIds.push_back(terrain().holeMap.getId());
    settleFiles();
    const fs::path hole = absoluteOf(terrain().holeMap.getPath(0));
    ASSERT_TRUE(fs::exists(hole)) << hole;

    ASSERT_TRUE(TerrainEditWindow::cleanUnusedTerrainMaps(project.get()));

    EXPECT_TRUE(fs::exists(hole)) << "the hole map the scene holds was collected";
    EXPECT_TRUE(fs::exists(fileOf(HEIGHT_MAP)));
}

// A map's .meta holds what is not in the map: its import settings, labels and GUID. A file in use
// keeps it; in-session the database would write a meta again, with the GUID but nothing else.
TEST_F(TerrainMapCleanupTest, KeepsTheSidecarOfAFileInUse) {
    giveHeight16(16);
    settleFiles();
    const fs::path height = fileOf(HEIGHT_MAP);
    const std::string treePath = TreeDataFile::makePath(project.get(), scene->id, terrainEntity);
    const fs::path trees = absoluteOf(treePath);
    ASSERT_EQ(io::writeFileAtomic(trees, std::string_view("trees")).error, io::WriteError::None);
    terrain().treeData = AssetRef(treePath);
    refreshAssets();

    YAML::Node settings;
    settings["mipmaps"] = false;
    SetAssetMetaCmd setImport(project.get(), project->normalizeToProjectRelative(height), TextureImportSettings::metaKey, settings);
    ASSERT_TRUE(setImport.execute());
    const fs::path heightMeta = SetAssetMetaCmd::metaPathOf(height);
    const fs::path treesMeta = SetAssetMetaCmd::metaPathOf(trees);
    ASSERT_TRUE(fs::exists(heightMeta));
    ASSERT_TRUE(fs::exists(treesMeta));
    const std::string treesGuid = project->getAssetDatabase().getGuid(project->normalizeToProjectRelative(trees));
    ASSERT_FALSE(treesGuid.empty());

    ASSERT_TRUE(TerrainEditWindow::cleanUnusedTerrainMaps(project.get()));

    EXPECT_TRUE(fs::exists(height));
    EXPECT_TRUE(fs::exists(trees));
    ASSERT_TRUE(fs::exists(heightMeta)) << "the height map's import settings were deleted";
    ASSERT_TRUE(fs::exists(treesMeta)) << "the tree file's GUID was deleted";
    const YAML::Node kept = SetAssetMetaCmd::read(height, TextureImportSettings::metaKey);
    ASSERT_TRUE(kept.IsMap());
    EXPECT_FALSE(kept["mipmaps"].as<bool>());
    EXPECT_EQ(YAML::LoadFile(treesMeta.string())["guid"].as<std::string>(), treesGuid);
}

// What nothing names goes, its sidecar with it: keeping sidecars is only for files in use
TEST_F(TerrainMapCleanupTest, CollectsAnOrphanWithItsSidecar) {
    giveHeight16(16);
    settleFiles();
    const fs::path orphan = project->getTerrainMapsDir() / "terrain_edit_orphan_height_1.png";
    ASSERT_EQ(io::writeFileAtomic(orphan, std::string_view("orphan")).error, io::WriteError::None);
    refreshAssets();
    const fs::path orphanMeta = SetAssetMetaCmd::metaPathOf(orphan);
    ASSERT_TRUE(fs::exists(orphanMeta));

    ASSERT_TRUE(TerrainEditWindow::cleanUnusedTerrainMaps(project.get()));

    EXPECT_FALSE(fs::exists(orphan));
    EXPECT_FALSE(fs::exists(orphanMeta));
    EXPECT_TRUE(fs::exists(fileOf(HEIGHT_MAP)));
    EXPECT_TRUE(fs::exists(SetAssetMetaCmd::metaPathOf(fileOf(HEIGHT_MAP))));
}
