// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One file per scene and one id per scene file, scenes inside the project, references that mean
// one GameObject, the tracked list's order (docs/superpowers/specs/2026-11-04-scene-integrity-
// design.md), on a headless Project (tools/fixtures/prefab-runtime copied to a temp dir).

#include "command/UndoFakeCommand.h"

#include <algorithm>
#include <map>

#include "Factory.h"
#include "Project.h"
#include "Stream.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetOps.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "component/CameraComponent.h"
#include "component/SkyboxComponent.h"
#include "component/Transform.h"
#include "project/SceneFileIdentity.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"
#include "util/EntityPayload.h"
#include "util/ProjectUtils.h"
#include "window/dialog/ProjectSaveDialog.h"
#include "window/dialog/SceneSaveDialog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    constexpr Entity PLAIN_OBJECT = 1001;   // a GameObject with a Transform

    std::string fileText(const fs::path& path) {
        std::string text;
        assetio::readFile(path, text);
        return text;
    }

    uint32_t fileSceneId(const fs::path& path) {
        uint32_t id = 0;
        std::string name;
        sceneid::readSceneHeader(fileText(path), id, name);
        return id;
    }

    std::string fileSceneName(const fs::path& path) {
        uint32_t id = 0;
        std::string name;
        sceneid::readSceneHeader(fileText(path), id, name);
        return name;
    }

    class SceneIntegrityTest : public ::testing::Test {
    protected:
        UndoStatics statics;
        jasperengine::test::TestTempDir dir{"scene-integrity"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = openProject();
            ASSERT_TRUE(project);
            scene = findScene("Main Scene");
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
            project->setSelectedSceneId(sceneId);
            scene->isModified = false;
        }
        void TearDown() override {
            if (project) closeProject(*project);
            project.reset();
        }

        std::unique_ptr<Project> openProject() {
            auto opened = std::make_unique<Project>();
            opened->setSkipProjectLock(true);
            if (!opened->loadProject(dir.path(), false)) return nullptr;
            for (SceneProject& candidate : opened->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) {
                    opened->loadScene(candidate.filepath, false, false, true);
                }
            }
            return opened;
        }

        SceneProject* findScene(const std::string& name) {
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.name == name) return &candidate;
            }
            return nullptr;
        }

        CommandHistory* history() { return CommandHandle::get(sceneId); }
        fs::path mainFile() const { return dir.path() / "Main Scene.scene"; }
    };

}

// ---- the identity a scene file carries -------------------------------------------------------

TEST(SceneFileIdentity, ReplacementIdIsStableAndFree) {
    const uint32_t first = sceneid::replacementId("Levels/A.scene", nullptr);
    EXPECT_EQ(sceneid::replacementId("Levels/./A.scene", nullptr), first);
    EXPECT_NE(first, 0u);
    EXPECT_NE(sceneid::replacementId("Levels/B.scene", nullptr), first);
    // probed past the ids in use
    const uint32_t free = sceneid::replacementId("Levels/A.scene", [first](uint32_t id) { return id == first || id == first + 1; });
    EXPECT_NE(free, first);
    EXPECT_NE(free, first + 1);
}

TEST(SceneFileIdentity, RewriteTouchesOnlyTheRootIdAndName) {
    std::string text = "formatVersion: 5\r\nid: 7\r\nname: Level\r\nchildScenes:\r\n  - id: 7\r\n    startActive: true\r\n"
                       "entities:\r\n  - name: Level\r\n";
    ASSERT_TRUE(sceneid::rewriteSceneHeaderText(text, 42u, std::string("Level \xC3\xA9 1")));
    uint32_t id = 0;
    std::string name;
    ASSERT_TRUE(sceneid::readSceneHeader(text, id, name));
    EXPECT_EQ(id, 42u);
    EXPECT_EQ(name, "Level \xC3\xA9 1");
    EXPECT_NE(text.find("  - id: 7\r\n"), std::string::npos) << "a nested id is not the scene's";
    EXPECT_NE(text.find("  - name: Level\r\n"), std::string::npos);
    EXPECT_NE(text.find("id: 42\r\n"), std::string::npos) << "line endings kept";
    std::string noId = "name: A\n";
    EXPECT_FALSE(sceneid::rewriteSceneHeaderText(noId, 1u, std::nullopt));
}

TEST(SceneFileIdentity, KeysFoldCaseOnCaseInsensitiveVolumes) {
    EXPECT_EQ(Project::sceneFileKeyOf("Levels/./A.scene", false), "Levels/A.scene");
    EXPECT_NE(Project::sceneFileKeyOf("levels/a.scene", false), Project::sceneFileKeyOf("Levels/A.scene", false));
    EXPECT_EQ(Project::sceneFileKeyOf("levels/a.scene", true), Project::sceneFileKeyOf("Levels/A.scene", true));
}

// ---- the Save Scene dialog's names --------------------------------------------------------------

TEST(SceneSaveDialog, NameProblems) {
    const fs::path project = fs::path("/tmp/project");
    auto problem = [&](const std::string& name) { return SceneSaveDialog::nameProblem(project, project / "Levels", name, nullptr); };
    EXPECT_EQ(problem("Level1"), "");
    EXPECT_EQ(problem("Level1.scene"), "");
    for (const char* bad : {"", "../x", "a/b", "a\\b", "/abs", "C:x", "con", "Con.scene", "a?b", "a*b", "name.", "name "}) {
        EXPECT_NE(problem(bad), "") << bad;
    }
    EXPECT_EQ(SceneSaveDialog::withSceneExtension("a.scene.bak"), "a.scene.bak.scene");
    EXPECT_EQ(SceneSaveDialog::withSceneExtension("a.scene"), "a.scene");
    // then the project's own rule for the file
    EXPECT_EQ(SceneSaveDialog::nameProblem(project, project, "Main", [](const fs::path& path) {
        return path.filename() == "Main.scene" ? std::string("taken") : std::string();
    }), "taken");
}

TEST(SceneSaveDialog, CloseRunsCancelOnceAndReleasesTheClosures) {
    auto token = std::make_shared<int>(0);
    int saved = 0;
    int cancelled = 0;
    SceneSaveDialog dialog;
    dialog.open("/tmp", "/tmp", "A.scene", [token, &saved](const fs::path&) { saved++; },
                [token, &cancelled]() { cancelled++; });
    EXPECT_GT(token.use_count(), 1);
    dialog.close();
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(saved, 0);
    EXPECT_FALSE(dialog.isOpen());
    EXPECT_EQ(token.use_count(), 1) << "an ended dialog keeps no closure";
    dialog.close();
    EXPECT_EQ(cancelled, 1) << "once";

    dialog.open("/tmp", "/tmp", "A.scene", [token, &saved](const fs::path&) { saved++; }, [token, &cancelled]() { cancelled++; });
    dialog.abandon();
    EXPECT_EQ(cancelled, 1) << "abandon runs nothing";
    EXPECT_EQ(token.use_count(), 1);
}

TEST(ProjectSaveDialog, CloseRunsCancelOnceAndReleasesTheClosures) {
    auto token = std::make_shared<int>(0);
    int cancelled = 0;
    ProjectSaveDialog dialog;
    dialog.open("P", [token](const std::string&, const fs::path&) {}, [token, &cancelled]() { cancelled++; });
    dialog.close();
    dialog.close();
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(token.use_count(), 1);
}

// ---- generated scene code ------------------------------------------------------------------------

TEST(GeneratedScenes, ParentsAreCreatedBeforeChildren) {
    Scene scene;
    const Entity parent = scene.createUserEntity();
    scene.addComponent<Transform>(parent, {});
    scene.setEntityName(parent, "OrderParent");
    const Entity child = scene.createUserEntity();
    scene.addComponent<Transform>(child, {});
    scene.setEntityName(child, "OrderChild");
    scene.addEntityChild(parent, child, false);

    // the list a model import left: the child first
    const std::string code = Factory::createScene(0, &scene, 77, "Order", {child, parent}, NULL_ENTITY, fs::path(), fs::path());
    const size_t parentAt = code.find("OrderParent");
    const size_t childAt = code.find("OrderChild");
    ASSERT_NE(parentAt, std::string::npos) << code;
    ASSERT_NE(childAt, std::string::npos) << code;
    EXPECT_LT(parentAt, childAt) << code;
    EXPECT_NE(std::string(Factory::sceneSourceHeader).find("scene source 4"), std::string::npos);
}

// ---- one file per scene --------------------------------------------------------------------------

TEST_F(SceneIntegrityTest, SaveOverAnotherScenesFileIsRefused) {
    const std::string before = fileText(mainFile());
    const uint32_t other = project->createSceneNow("Other", SceneType::SCENE_3D);
    ASSERT_NE(other, NULL_PROJECT_SCENE);

    EXPECT_NE(project->sceneSavePathProblem(other, "Main Scene.scene"), "");
    EXPECT_FALSE(project->saveSceneToPath(other, "Main Scene.scene"));
    EXPECT_EQ(fileText(mainFile()), before) << "the other scene's file is untouched";
    ASSERT_NE(project->getScene(other), nullptr);
    EXPECT_TRUE(project->getScene(other)->filepath.empty()) << "a refused save gives no path";

    // its own new file is fine, and saving the scene again over its own file too
    EXPECT_EQ(project->sceneSavePathProblem(other, "Levels/Other.scene"), "");
    EXPECT_TRUE(project->saveSceneToPath(other, "Levels/Other.scene"));
    EXPECT_TRUE(project->saveSceneToPath(other, "Levels/Other.scene"));
    EXPECT_EQ(project->findSceneByPath("Levels/./Other.scene"), other);
}

TEST_F(SceneIntegrityTest, NewScenePathsStayInsideTheProject) {
    const uint32_t other = project->createSceneNow("Other", SceneType::SCENE_3D);
    EXPECT_NE(project->sceneSavePathProblem(other, "../Outside.scene"), "");
    EXPECT_FALSE(project->saveSceneToPath(other, "../Outside.scene"));
    EXPECT_FALSE(fs::exists(dir.path().parent_path() / "Outside.scene"));

    // a scene file from outside is not taken into the project
    const fs::path outside = dir.path().parent_path() / (dir.path().filename().string() + "-outside.scene");
    fs::copy_file(mainFile(), outside, fs::copy_options::overwrite_existing);
    const size_t count = project->getScenes().size();
    EXPECT_FALSE(project->loadScene(outside, false, true, true));
    EXPECT_EQ(project->getScenes().size(), count);
    fs::remove(outside);
}

TEST_F(SceneIntegrityTest, ProjectListsEachSceneFileOnce) {
    // a second scene on Main Scene's file, as a save over it used to leave
    SceneProject twin;
    twin.id = 4242;
    twin.name = "Twin";
    twin.filepath = scene->filepath;
    project->getScenes().push_back(twin);
    const YAML::Node encoded = Stream::encodeProject(project.get());
    size_t listed = 0;
    for (const YAML::Node& entry : encoded["scenes"]) {
        if (entry["filepath"].as<std::string>() == "Main Scene.scene") listed++;
    }
    EXPECT_EQ(listed, 1u);
    project->getScenes().pop_back();
}

// ---- one id per scene file -----------------------------------------------------------------------

TEST_F(SceneIntegrityTest, ACopiedSceneGetsItsOwnIdAndName) {
    std::map<std::string, std::string> guids;
    std::map<std::string, AssetOps::SceneCopy> scenes;
    const AssetOps::Result result = AssetOps::copy(*project, mainFile(), dir.path() / "Main Copy.scene", &guids, &scenes);
    ASSERT_TRUE(result.ok) << result.error;
    const uint32_t copyId = fileSceneId(dir.path() / "Main Copy.scene");
    EXPECT_NE(copyId, scene->id);
    EXPECT_NE(copyId, 0u);
    EXPECT_EQ(fileSceneName(dir.path() / "Main Copy.scene"), "Main Copy") << "the name followed the file";
    ASSERT_EQ(result.copiedScenes.size(), 1u);
    EXPECT_EQ(result.copiedScenes[0].second.id, copyId);

    // a redo gives the copy the same identity
    AssetOps::removeCopy(*project, dir.path() / "Main Copy.scene");
    ASSERT_TRUE(AssetOps::copy(*project, mainFile(), dir.path() / "Main Copy.scene", &guids, &scenes).ok);
    EXPECT_EQ(fileSceneId(dir.path() / "Main Copy.scene"), copyId);

    // and it opens as a scene of its own
    ASSERT_TRUE(project->loadScene(dir.path() / "Main Copy.scene", false, true, true));
    EXPECT_EQ(project->findSceneByPath("Main Copy.scene"), copyId);
}

TEST_F(SceneIntegrityTest, AClashAtLoadGetsTheSameReplacementEveryTime) {
    // a copy made outside the editor carries Main Scene's id
    fs::copy_file(mainFile(), dir.path() / "Twin.scene");
    ASSERT_TRUE(project->loadScene(dir.path() / "Twin.scene", false, true, true));
    const uint32_t twin = project->findSceneByPath("Twin.scene");
    ASSERT_NE(twin, NULL_PROJECT_SCENE);
    EXPECT_NE(twin, sceneId) << "the scene listed first keeps the id";
    EXPECT_TRUE(project->getScene(twin)->isModified) << "its save writes the new id";

    // the next load of the same files picks the same id (no save in between)
    closeProject(*project);
    project = openProject();
    ASSERT_TRUE(project);
    ASSERT_TRUE(project->loadScene(dir.path() / "Twin.scene", false, true, true));
    EXPECT_EQ(project->findSceneByPath("Twin.scene"), twin);
}

// ---- the tracked list and its commands -----------------------------------------------------------

TEST_F(SceneIntegrityTest, ATransformNextToATransformlessAnchorIsRefused) {
    CreateEntityCmd* sky = new CreateEntityCmd(project.get(), sceneId, "Sky", EntityCreationType::SKYBOX);
    ASSERT_TRUE(history()->addCommand(sky));
    const Entity skybox = sky->getEntity();
    const std::vector<Entity> before = scene->entities;

    MoveEntityOrderCmd move(project.get(), sceneId, PLAIN_OBJECT, skybox, InsertionType::AFTER);
    EXPECT_FALSE(move.execute()) << "only the list would change";
    EXPECT_EQ(scene->entities, before);
    EXPECT_NE(MoveEntityOrderCmd::transformAnchorProblem(scene->scene, PLAIN_OBJECT, skybox, InsertionType::BEFORE), "");
    // the other way stays allowed (a Skybox keeps its place in the gaps)
    EXPECT_EQ(MoveEntityOrderCmd::transformAnchorProblem(scene->scene, skybox, PLAIN_OBJECT, InsertionType::AFTER), "");
}

TEST_F(SceneIntegrityTest, MultiDeleteUndoKeepsTheSky) {
    CreateEntityCmd* first = new CreateEntityCmd(project.get(), sceneId, "Sky1", EntityCreationType::SKYBOX);
    ASSERT_TRUE(history()->addCommand(first));
    CreateEntityCmd* second = new CreateEntityCmd(project.get(), sceneId, "Sky2", EntityCreationType::SKYBOX);
    ASSERT_TRUE(history()->addCommand(second));
    const Entity sky1 = first->getEntity();
    const Entity sky2 = second->getEntity();
    auto skies = scene->scene->getComponentArray<SkyboxComponent>();
    ASSERT_EQ(skies->getEntity(0), sky1) << "the first Skybox is the sky";
    const std::vector<Entity> before = scene->entities;

    // deleted in the opposite order, so the undo restores Sky2 first
    ASSERT_TRUE(history()->addCommand(new DeleteEntityCmd(project.get(), sceneId, std::vector<Entity>{sky2, sky1, PLAIN_OBJECT})));
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(scene->entities, before);
    skies = scene->scene->getComponentArray<SkyboxComponent>();
    ASSERT_EQ(skies->size(), 2u);
    EXPECT_EQ(skies->getEntity(0), sky1) << "the sky is the one it was";
}

TEST_F(SceneIntegrityTest, CreatedChildrenJoinTheirParentsBranch) {
    // a child created under PLAIN_OBJECT is listed after PLAIN_OBJECT's branch, not at the end
    CreateEntityCmd* child = new CreateEntityCmd(project.get(), sceneId, "Child", EntityCreationType::EMPTY, PLAIN_OBJECT);
    ASSERT_TRUE(history()->addCommand(child));
    std::vector<Entity> sorted = scene->entities;
    ProjectUtils::sortEntitiesByTransformOrder(scene->scene, sorted);
    EXPECT_EQ(scene->entities, sorted);
}

// ---- cameras -------------------------------------------------------------------------------------

TEST_F(SceneIntegrityTest, ARemovedCameraIsNoMainCamera) {
    CreateEntityCmd* camera = new CreateEntityCmd(project.get(), sceneId, "Cam", EntityCreationType::CAMERA);
    ASSERT_TRUE(history()->addCommand(camera));
    scene->mainCamera = camera->getEntity();
    EXPECT_EQ(project->getGameCamera(sceneId), camera->getEntity());
    scene->scene->removeComponent<CameraComponent>(camera->getEntity());
    EXPECT_NE(project->getGameCamera(sceneId), camera->getEntity());
}

// ---- references across scenes --------------------------------------------------------------------

TEST_F(SceneIntegrityTest, RelatedScenesAndDropRules) {
    const uint32_t child = project->createSceneNow("HUD", SceneType::SCENE_UI);
    const uint32_t sibling = project->createSceneNow("Audio", SceneType::SCENE_3D);
    const uint32_t stranger = project->createSceneNow("Elsewhere", SceneType::SCENE_3D);
    ASSERT_TRUE(project->addChildScene(sceneId, child));
    ASSERT_TRUE(project->addChildScene(sceneId, sibling));

    const std::vector<uint32_t> related = project->getRelatedScenes(child);
    ASSERT_FALSE(related.empty());
    EXPECT_EQ(related.front(), child);
    EXPECT_NE(std::find(related.begin(), related.end(), sceneId), related.end()) << "its parent";
    EXPECT_NE(std::find(related.begin(), related.end(), sibling), related.end()) << "its sibling";
    EXPECT_EQ(std::find(related.begin(), related.end(), stranger), related.end());

    // a child scene's GameObject: a reference, never moved by a same-scene target
    EntityPayload payload{};
    payload.entity = PLAIN_OBJECT;
    payload.entitySceneId = child;
    payload.referenceOnly = true;
    EXPECT_NE(sameSceneDropProblem(*project, payload, sceneId), "");
    EXPECT_NE(sameSceneDropProblem(*project, payload, child, true), "");
    EXPECT_EQ(sameSceneDropProblem(*project, payload, child), "");
    payload.entitySceneId = sceneId;
    payload.referenceOnly = false;
    EXPECT_EQ(sameSceneDropProblem(*project, payload, sceneId, true), "");

    // a reference from the HUD to the parent's GameObject works, from an unrelated scene not
    EXPECT_EQ(referenceDropProblem(*project, payload, child), "");
    EXPECT_NE(referenceDropProblem(*project, payload, stranger), "");
}
