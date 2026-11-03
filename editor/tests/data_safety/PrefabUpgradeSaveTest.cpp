// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A prefab file of an older version is written again in the current format the next time a
// scene using it is saved (Prefab::needsUpgrade), so a project upgraded from origin/main (prefab
// version 3) migrates once and stays migrated: tools/ci/upgrade_from_main/check.sh.

#include "support/EditorTestSupport.h"

#include "Project.h"
#include "StreamMigrations.h"
#include "TempDir.h"
#include "command/CommandHandle.h"
#include "util/Prefab.h"

#include "yaml-cpp/yaml.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace prefab_upgrade_save_test {

    int fileVersion(const fs::path& path) {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        return StreamMigrations::readVersion(DocumentKind::Prefab, YAML::Load(text.str()));
    }

}

using namespace prefab_upgrade_save_test;

TEST(PrefabUpgradeSave, MigratedPrefabIsWrittenInTheCurrentVersionWithItsScene) {
    ensureEngine();
    jasperengine::test::TempDir dir;
    fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    const fs::path enemy = dir.path() / "prefabs" / "Enemy.prefab";
    // what origin/main writes (Prefab::FILE_VERSION 3 there): migrated 3 -> current on load
    ASSERT_EQ(fileVersion(enemy), 3);
    ASSERT_GT(StreamMigrations::currentVersion(DocumentKind::Prefab), 3);

    auto project = std::make_unique<Project>();
    project->setSkipProjectLock(true);
    ASSERT_TRUE(project->loadProject(dir.path(), false));
    uint32_t sceneId = 0;
    for (SceneProject& candidate : project->getScenes()) {
        if (candidate.filepath.filename() == "Main Scene.scene") sceneId = candidate.id;
    }
    ASSERT_NE(sceneId, 0u);
    if (!project->getScene(sceneId)->scene) project->loadScene(project->getScene(sceneId)->filepath, true, false, true);
    ASSERT_NE(project->getScene(sceneId)->scene, nullptr);

    const Prefab* loaded = project->getPrefab(fs::path("prefabs/Enemy.prefab"));
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->needsUpgrade) << "a prefab read from an older version is due to be written again";

    std::string error;
    ASSERT_TRUE(project->saveSceneNow(sceneId, {}, &error)) << error;
    EXPECT_EQ(fileVersion(enemy), StreamMigrations::currentVersion(DocumentKind::Prefab))
        << "saving a scene that uses the prefab writes it in the current version";

    for (SceneProject& open : project->getScenes()) CommandHandle::remove(open.id);
    project->releaseScenes();
}
