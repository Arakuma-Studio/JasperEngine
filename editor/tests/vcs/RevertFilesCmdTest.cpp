// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The undoable revert (docs/specs/S29-version-control.md R11, AC10): tools/fixtures/prefab-runtime
// committed to a temp repository, a texture with its .meta and the open scene changed (the scene
// dirty too), reverted, undone, redone.

#include "VcsTestSupport.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/RevertFilesCmd.h"
#include "vcs/VcsEditorOps.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    class RevertFilesTest : public ::testing::Test {
    protected:
        VcsSandbox box;
        fs::path dir;
        std::unique_ptr<Project> project;

        void SetUp() override {
            dir = box.path("game");
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir, fs::copy_options::recursive);
            writeFile(dir / "assets/hero.png", "png at HEAD");
            writeFile(dir / "assets/hero.png.meta", "guid: 0123456789abcdef0123456789abcdef\n");
            box.gitOk(dir, {"init", "-q"});
            writeFile(dir / ".gitignore", ".jasperengine/\n");
            box.gitOk(dir, {"add", "-A"});
            box.gitOk(dir, {"commit", "-q", "-m", "initial"});
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir, false));
            project->getVcs().open(dir, project->getProjectInternalPath() / "vcs", box.settings(), false);
            ASSERT_TRUE(pumpUntil(project->getVcs(), [this]() { return project->getVcs().snapshot() != nullptr; }));
        }

        void TearDown() override {
            if (project) {
                for (SceneProject& scene : project->getScenes()) CommandHandle::remove(scene.id);
                project->releaseScenes();
            }
            project.reset();
        }

        SceneProject* mainScene() {
            for (SceneProject& scene : project->getScenes()) {
                if (scene.filepath.filename() == "Main Scene.scene") return &scene;
            }
            return nullptr;
        }
    };

}

TEST_F(RevertFilesTest, UndoRedo) {
    VcsService& service = project->getVcs();
    // what HEAD holds (the project's load already wrote its asset identity into the files)
    const std::string headScene = box.gitOk(dir, {"show", "HEAD:Main Scene.scene"});
    const std::string headPng = box.gitOk(dir, {"show", "HEAD:assets/hero.png"});
    const std::string headMeta = box.gitOk(dir, {"show", "HEAD:assets/hero.png.meta"});

    // the scene open and saved with a change, then changed again without a save
    SceneProject* scene = mainScene();
    ASSERT_NE(scene, nullptr);
    if (!scene->scene) {
        project->loadScene(scene->filepath, true, false, true);
        scene = mainScene();
    }
    ASSERT_NE(scene->scene, nullptr);
    const uint32_t sceneId = scene->id;
    Entity game = NULL_ENTITY;
    for (Entity entity : scene->entities) {
        if (scene->scene->getEntityName(entity) == "Game") game = entity;
    }
    ASSERT_NE(game, NULL_ENTITY);
    ASSERT_TRUE(CommandHandle::get(sceneId)->addCommand(new EntityNameCmd(project.get(), sceneId, game, "Saved Name")));
    std::string error;
    ASSERT_TRUE(project->saveSceneNow(sceneId, {}, &error)) << error;
    ASSERT_TRUE(CommandHandle::get(sceneId)->addCommand(new EntityNameCmd(project.get(), sceneId, game, "Unsaved Name")));
    ASSERT_TRUE(mainScene()->isModified);
    const std::string modifiedScene = readFile(dir / "Main Scene.scene");
    ASSERT_NE(modifiedScene, headScene);
    writeFile(dir / "assets/hero.png", "png edited");
    writeFile(dir / "assets/hero.png.meta", "guid: 0123456789abcdef0123456789abcdef\nimport: {compression: high}\n");
    ASSERT_TRUE(refreshed(service));

    // R11: an open dirty document is named before the revert (the UI asks; auto-accepted here)
    const std::vector<fs::path> files = {dir / "assets/hero.png", dir / "Main Scene.scene"};
    EXPECT_EQ(dirtyDocumentsAmong(*project, files), std::vector<std::string>{"Main Scene"});
    std::string undoName;
    const VcsResult reverted = await(service, [&](VcsService::Done done) {
        revertFiles(*project, files, [done, &undoName](const VcsResult& result, const std::string& name) {
            undoName = name;
            done(result);
        });
    });
    ASSERT_TRUE(reverted.ok) << reverted.message;
    EXPECT_EQ(undoName, "Revert 2 files");
    EXPECT_EQ(readFile(dir / "assets/hero.png"), headPng);
    EXPECT_EQ(readFile(dir / "assets/hero.png.meta"), headMeta);
    EXPECT_EQ(readFile(dir / "Main Scene.scene"), headScene);
    // the scene read its file again: neither name, nothing unsaved
    scene = mainScene();
    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->scene, nullptr);
    EXPECT_FALSE(scene->isModified);
    EXPECT_EQ(scene->scene->getEntityName(game), "Game");
    const CommandHistory* history = project->getProjectCommandHistory();
    ASSERT_NE(history->top(), nullptr);
    EXPECT_EQ(history->top()->name(), "Revert 2 files");
    const auto* command = dynamic_cast<const RevertFilesCmd*>(history->top()->cmd.get());
    ASSERT_NE(command, nullptr);
    const fs::path backup = command->getBackupDir();
    EXPECT_TRUE(fs::exists(backup));

    // undo: the changed files come back byte for byte; redo: HEAD again
    ASSERT_TRUE(project->getProjectCommandHistory()->undo());
    EXPECT_EQ(readFile(dir / "assets/hero.png"), "png edited");
    EXPECT_EQ(readFile(dir / "assets/hero.png.meta"), "guid: 0123456789abcdef0123456789abcdef\nimport: {compression: high}\n");
    EXPECT_EQ(readFile(dir / "Main Scene.scene"), modifiedScene);
    EXPECT_EQ(mainScene()->scene->getEntityName(game), "Saved Name");
    ASSERT_TRUE(project->getProjectCommandHistory()->redo());
    EXPECT_EQ(readFile(dir / "assets/hero.png"), headPng);
    EXPECT_EQ(readFile(dir / "assets/hero.png.meta"), headMeta);
    EXPECT_EQ(readFile(dir / "Main Scene.scene"), headScene);

    // the backups go with the project
    for (SceneProject& open : project->getScenes()) CommandHandle::remove(open.id);
    project->releaseScenes();
    EXPECT_FALSE(fs::exists(backup));
    EXPECT_FALSE(fs::exists(project->getVcsBackupDir()));
}

TEST_F(RevertFilesTest, NewFilesAndFailuresPushNothing) {
    VcsService& service = project->getVcs();
    writeFile(dir / "assets/new.png", "new");
    ASSERT_TRUE(refreshed(service));
    const size_t steps = project->getProjectCommandHistory()->entries().size();
    const VcsResult refused = await(service, [&](VcsService::Done done) {
        revertFiles(*project, {dir / "assets/new.png"}, [done](const VcsResult& result, const std::string&) { done(result); });
    });
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.message.find("Delete it instead"), std::string::npos) << refused.message;
    const VcsResult unchanged = await(service, [&](VcsService::Done done) {
        revertFiles(*project, {dir / "project.yaml"}, [done](const VcsResult& result, const std::string&) { done(result); });
    });
    EXPECT_FALSE(unchanged.ok);
    EXPECT_EQ(project->getProjectCommandHistory()->entries().size(), steps);
    EXPECT_TRUE(fs::exists(dir / "assets/new.png"));
}
