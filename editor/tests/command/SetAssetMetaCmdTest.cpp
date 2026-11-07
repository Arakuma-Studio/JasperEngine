// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S53 R14 (AC9): SetAssetMetaCmd tells every write listener, so a texture's and a model's
// import settings applied in one session both queue their cooks again (one listener slot let the
// queue attached last silence the other), and one listener going leaves the others.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "model/ModelCookQueue.h"
#include "texture/TextureCookQueue.h"

#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    // the texture fixture project (a glTF model and PNG textures), loaded headless
    std::unique_ptr<Project> loadFixture(const fs::path& dir){
        fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/textures", dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        auto project = std::make_unique<Project>();
        project->setSkipProjectLock(true);
        if (!project->loadProject(dir, false))
            return nullptr;
        return project;
    }

}

TEST(SetAssetMetaCmd, EveryWriteListenerHearsEveryWrite){
    jasperengine::test::TestTempDir dir{"meta-listeners"};
    std::unique_ptr<Project> project = loadFixture(dir.path());
    ASSERT_TRUE(project);
    int first = 0, second = 0;
    const int a = 0, b = 0;
    SetAssetMetaCmd::addWriteListener(&a, [&](Project*, const fs::path&, const std::string&){ first++; });
    SetAssetMetaCmd::addWriteListener(&b, [&](Project*, const fs::path&, const std::string&){ second++; });
    SetAssetMetaCmd command(project.get(), "assets/textures/ao.png", "import.textureImporter", YAML::Load("{version: 1, maxSize: 512}"));
    ASSERT_TRUE(command.execute());
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 1);
    // undo is a write too
    command.undo();
    EXPECT_EQ(first, 2);
    EXPECT_EQ(second, 2);
    // a listener added again under its key replaces itself; removing one keeps the other
    SetAssetMetaCmd::addWriteListener(&a, [&](Project*, const fs::path&, const std::string&){ first += 10; });
    SetAssetMetaCmd::removeWriteListener(&b);
    ASSERT_TRUE(command.execute());
    EXPECT_EQ(first, 12);
    EXPECT_EQ(second, 2);
    SetAssetMetaCmd::removeWriteListener(&a);
    closeProject(*project);
}

TEST(SetAssetMetaCmd, TextureAndModelAppliesBothRecook){
    jasperengine::test::TestTempDir dir{"meta-two-queues"};
    std::unique_ptr<Project> project = loadFixture(dir.path());
    ASSERT_TRUE(project);
    project->getAssetIndex().waitUntilReady();
    TextureCookQueue& textures = TextureCookQueue::get();
    ModelCookQueue& models = ModelCookQueue::get();
    textures.attach(project.get());
    models.attach(project.get());
    const std::string texture = "textures/gradient.png", model = "models/panel.gltf";
    textures.request(texture, TextureFamily::Desktop);
    models.request(model, ModelFamily::Desktop);
    // nothing pending: what a settings write queues again is its own doing
    textures.cancelAll();
    models.cancelAll();
    ASSERT_FALSE(textures.isPending(texture, TextureFamily::Desktop));
    ASSERT_FALSE(models.isPending(model, ModelFamily::Desktop));

    // one Apply of each importer, in one undo step
    CommandHistory* history = project->getProjectCommandHistory();
    InteractiveEdit edit = history->beginInteractive("Set Import Settings");
    history->addCommand(new SetAssetMetaCmd(project.get(), "assets/" + texture, "import.textureImporter", YAML::Load("{version: 1, maxSize: 512}")));
    history->addCommand(new SetAssetMetaCmd(project.get(), "assets/" + model, "import.modelImporter", YAML::Load("{version: 1, meshCompression: medium}")));
    edit.commit();
    EXPECT_TRUE(textures.isPending(texture, TextureFamily::Desktop)) << "the texture queue heard its write";
    EXPECT_TRUE(models.isPending(model, ModelFamily::Desktop)) << "the model queue heard its write";

    // undo of the Apply restores the sidecars and recooks both again
    textures.cancelAll();
    models.cancelAll();
    ASSERT_TRUE(history->undo());
    const YAML::Node restored = SetAssetMetaCmd::read(dir.path() / "assets" / model, "import.modelImporter");
    EXPECT_TRUE(!restored || restored.IsNull()) << "the model had no settings before the Apply";
    EXPECT_TRUE(textures.isPending(texture, TextureFamily::Desktop));
    EXPECT_TRUE(models.isPending(model, ModelFamily::Desktop));

    textures.attach(nullptr);
    models.attach(nullptr);
    closeProject(*project);
}
