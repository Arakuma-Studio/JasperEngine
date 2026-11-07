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
#include "command/type/CopyFileCmd.h"
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
#include <fstream>
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

        // loadClosed: every listed scene's data too, as the CLI's scene-dump loads them
        std::unique_ptr<Project> openProject(bool loadClosed = true) {
            auto opened = std::make_unique<Project>();
            opened->setSkipProjectLock(true);
            if (!opened->loadProject(dir.path(), false)) return nullptr;
            for (SceneProject& candidate : opened->getScenes()) {
                if (loadClosed && !candidate.filepath.empty() && !candidate.scene) {
                    opened->loadScene(candidate.filepath, false, false, true);
                }
            }
            return opened;
        }

        void reopen(bool loadClosed = true) {
            closeProject(*project);
            project = openProject(loadClosed);
            ASSERT_TRUE(project);
            scene = findScene("Main Scene");
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
        }

        // project.yaml with one more scenes entry
        void listScene(const std::string& filepath) {
            const fs::path yaml = dir.path() / "project.yaml";
            std::string text = fileText(yaml);
            const size_t at = text.find("  - filepath: Main Scene.scene");
            ASSERT_NE(at, std::string::npos) << text;
            const size_t end = text.find('\n', at);
            text.insert(end == std::string::npos ? text.size() : end + 1, "  - filepath: " + filepath + "\n");
            std::ofstream(yaml, std::ios::binary) << text;
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

    // the extension in any case, as the asset database reads it
    EXPECT_EQ(project->sceneSavePathProblem(other, "Levels/Other.Scene"), "");

    // a listed scene that did not load (a merge conflict in its file) still owns its file
    project->addUnresolvedScene(project->getScenes().size(), "Broken.scene");
    EXPECT_NE(project->sceneSavePathProblem(other, "Broken.scene"), "");
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

// A file copied outside the editor (a merge) can claim the id of a scene listed before it. Unity
// regenerates a duplicate GUID; here the later scene gets a fresh id, written into its file at once,
// so a closed scene, a reload, a rename and an inline load all see the same id.
TEST_F(SceneIntegrityTest, AClashWithAClosedSceneIsWrittenIntoItsFile) {
    fs::copy_file(mainFile(), dir.path() / "Twin.scene");
    listScene("Twin.scene");
    reopen(false);
    const uint32_t twin = project->findSceneByPath("Twin.scene");
    ASSERT_NE(twin, NULL_PROJECT_SCENE);
    EXPECT_EQ(project->getScene(twin)->scene, nullptr) << "closed: no data loaded";
    EXPECT_NE(twin, sceneId) << "the scene listed first keeps the id";
    EXPECT_EQ(fileSceneId(dir.path() / "Twin.scene"), twin) << "the fresh id is in the file";

    // a reference to it, saved, survives the reload
    ASSERT_TRUE(project->addChildScene(sceneId, twin));
    ASSERT_TRUE(project->saveSceneToPath(sceneId, scene->filepath));
    reopen(false);
    EXPECT_EQ(project->findSceneByPath("Twin.scene"), twin) << "the same id at the next load";
    EXPECT_TRUE(project->hasChildScene(sceneId, twin)) << "the child-scene reference holds";

    // loaded inline, it keeps it
    ASSERT_TRUE(project->loadChildSceneInline(twin));
    EXPECT_EQ(project->findSceneByPath("Twin.scene"), twin);
    EXPECT_NE(project->getScene(twin)->scene, nullptr);
}

TEST_F(SceneIntegrityTest, AClashWithAnOpenedFileIsWrittenIntoItsFile) {
    fs::copy_file(mainFile(), dir.path() / "Twin.scene");
    ASSERT_TRUE(project->loadScene(dir.path() / "Twin.scene", true, true, true));
    const uint32_t twin = project->findSceneByPath("Twin.scene");
    ASSERT_NE(twin, NULL_PROJECT_SCENE);
    EXPECT_NE(twin, sceneId);
    EXPECT_EQ(fileSceneId(dir.path() / "Twin.scene"), twin);
    EXPECT_EQ(project->getSelectedSceneId(), twin) << "selected under its new id";

    // renamed after the fix, the file carries its id
    const AssetOps::Result moved = AssetOps::move(*project, dir.path() / "Twin.scene", dir.path() / "Renamed.scene");
    ASSERT_TRUE(moved.ok) << moved.error;
    ASSERT_TRUE(project->saveProjectFile());
    reopen();
    EXPECT_EQ(project->findSceneByPath("Renamed.scene"), twin);
}

TEST_F(SceneIntegrityTest, ADuplicateProjectEntryLoadsOnce) {
    listScene("Main Scene.scene");
    reopen();
    size_t listed = 0;
    for (const SceneProject& candidate : project->getScenes()) {
        if (candidate.filepath == "Main Scene.scene") listed++;
    }
    EXPECT_EQ(listed, 1u);
}

TEST_F(SceneIntegrityTest, AnOldProjectsOutsideSceneLoadsAndSavesInPlace) {
    // a guard, not a red control: an older editor let a project list a scene outside its folder
    const std::string outsideName = dir.path().filename().string() + "-shared.scene";
    const fs::path outside = dir.path().parent_path() / outsideName;
    fs::copy_file(mainFile(), outside, fs::copy_options::overwrite_existing);
    sceneid::rewriteSceneHeader(outside, 777001u, std::string("Shared"), nullptr);
    listScene("../" + outsideName);
    reopen();
    SceneProject* shared = findScene("Shared");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(project->sceneSavePathProblem(shared->id, shared->filepath), "");
    EXPECT_TRUE(project->saveSceneToPath(shared->id, shared->filepath));
    fs::remove(outside);
}

TEST_F(SceneIntegrityTest, ACopyOverAnExistingSceneKeepsItsIdentity) {
    const uint32_t other = project->createSceneNow("Other", SceneType::SCENE_3D);
    ASSERT_TRUE(project->saveSceneToPath(other, "Other.scene"));
    const fs::path outsideDir = dir.path().parent_path() / (dir.path().filename().string() + "-incoming");
    fs::create_directories(outsideDir);
    fs::copy_file(mainFile(), outsideDir / "Other.scene", fs::copy_options::overwrite_existing);

    CopyFileCmd copy(project.get(), std::vector<std::string>{(outsideDir / "Other.scene").string()}, dir.path().string(), true);
    copy.setOverwrite(true);
    ASSERT_TRUE(copy.execute());
    EXPECT_EQ(fileSceneId(dir.path() / "Other.scene"), other) << "replaced in place, it stays the scene it was";
    EXPECT_EQ(fileSceneName(dir.path() / "Other.scene"), "Other");
    fs::remove_all(outsideDir);
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

// A guard, not a red control: CreateEntityCmd already lists a created child in its parent's branch
TEST_F(SceneIntegrityTest, CreatedChildrenJoinTheirParentsBranch) {
    // a child created under First goes into First's branch (before Second), not to the list's end
    CreateEntityCmd* first = new CreateEntityCmd(project.get(), sceneId, "First", EntityCreationType::EMPTY);
    ASSERT_TRUE(history()->addCommand(first));
    ASSERT_TRUE(history()->addCommand(new CreateEntityCmd(project.get(), sceneId, "Second", EntityCreationType::EMPTY)));
    CreateEntityCmd* child = new CreateEntityCmd(project.get(), sceneId, "Child", EntityCreationType::EMPTY, first->getEntity());
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
