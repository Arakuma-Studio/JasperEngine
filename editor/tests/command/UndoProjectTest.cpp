// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S24 on a headless Project (tools/fixtures/prefab-runtime copied to a temp dir): no-op drops of
// real commands, selection steps, the prefab override matrix (R31, AC14) and the Play Mode
// Changes review (R26-R29, AC13).

#include "UndoFakeCommand.h"
#include "component/Transform.h"

#include "Project.h"
#include "command/PlayChangeReview.h"
#include "command/PrefabEditUndo.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/SetAssetMetaCmd.h"
#include "Stream.h"
#include "subsystem/MeshSystem.h"
#include "support/EditorTestSupport.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    constexpr Entity GAME = 1001;       // a plain GameObject
    constexpr Entity SPAWNER = 1002;    // a plain GameObject
    constexpr Entity ENEMY = 1003;      // a prefab instance root (Enemy.prefab)
    constexpr Entity BODY = 1004;       // a GameObject of that instance, from the prefab

    // The fixture project, loaded as the CLI's scene-dump loads one
    class UndoProjectTest : public ::testing::Test {
    protected:
        UndoStatics statics;
        jasperengine::test::TestTempDir dir{"undo"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) {
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
            project->setSelectedSceneId(sceneId);
            scene->isModified = false;
        }
        void TearDown() override {
            PlayChangeReview::get().discardAll();
            // closes the scenes as a project switch does: a Project frees none itself
            if (project) closeProject(*project);
            project.reset();
        }

        CommandHistory* history() { return CommandHandle::get(sceneId); }
        Vector3 position(Entity entity) { return scene->scene->getComponent<Transform>(entity).position; }
        Prefab::ObjectOverrides overrides(Entity entity) {
            Prefab::ObjectOverrides out;
            project->getPrefabEditOverrides(sceneId, entity, out);
            return out;
        }
        static bool same(const Prefab::ObjectOverrides& a, const Prefab::ObjectOverrides& b) {
            return a.properties == b.properties && a.state == b.state && a.addedComponents == b.addedComponents &&
                a.removedComponents == b.removedComponents;
        }
        std::string encoded(Entity entity) {
            // without the project context: a prefab member is encoded too (with it, its prefab holds it)
            return YAML::Dump(Stream::encodeEntity(entity, scene->scene, nullptr, nullptr));
        }

        // Play as the CLI's playStart/playStop emulate it: the snapshot, the history's floor,
        // and Stop's capture, restore and review
        void beginPlay() {
            scene->playStateSnapshot = Stream::encodeSceneProject(nullptr, scene);
            scene->playPrefabState = project->capturePlayPrefabState(scene);
            const std::vector<Entity> alive = scene->scene->getEntityList();
            scene->playStateEntities = std::set<Entity>(alive.begin(), alive.end());
            scene->playStateModified = scene->isModified;
            history()->beginPlaySession();
            scene->playState = ScenePlayState::PLAYING;
        }
        void stopPlay() {
            PlayChangeReview::get().capture(project.get(), scene);
            scene->isModified = scene->playStateModified;
            const bool restored = project->restorePlaySnapshot(scene);
            scene->playState = ScenePlayState::STOPPED;
            history()->endPlaySession(restored);
            ASSERT_TRUE(restored);
            PlayChangeReview::get().afterRestore(project.get(), scene);
        }
    };

    // the suites the spec names (AC14, AC13), on the same loaded project
    class PrefabUndoTests : public UndoProjectTest {};
    class PlayChangeReviewTests : public UndoProjectTest {};

}

// S71: Stop puts back a Mesh only when Play changed it; its decode reloads it at the next frame
TEST_F(UndoProjectTest, StopKeepsAMeshAPlayOnlyMoved) {
    scene->scene->addComponent<MeshComponent>(GAME, MeshComponent{});
    const Vector3 edit = position(GAME);
    MeshComponent& mesh = scene->scene->getComponent<MeshComponent>(GAME);
    // what create_entity's box holds: the geometry the encoding writes
    scene->scene->getSystem<MeshSystem>()->createBox(mesh, 1, 1, 1);
    mesh.worldAABB = AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1));
    beginPlay();
    scene->scene->getComponent<Transform>(GAME).position = edit + Vector3(5, 5, 5);
    mesh.worldAABB = AABB(Vector3(4, 4, 4), Vector3(6, 6, 6));
    mesh.needReload = false;
    stopPlay();
    EXPECT_EQ(position(GAME), edit);
    const MeshComponent& restored = scene->scene->getComponent<MeshComponent>(GAME);
    EXPECT_FALSE(restored.needReload);
    EXPECT_EQ(YAML::Dump(Stream::encodeAABB(restored.worldAABB)), YAML::Dump(Stream::encodeAABB(AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1)))));
}

TEST_F(UndoProjectTest, StopDecodesAMeshAPlayChanged) {
    scene->scene->addComponent<MeshComponent>(GAME, MeshComponent{});
    beginPlay();
    MeshComponent& mesh = scene->scene->getComponent<MeshComponent>(GAME);
    mesh.castShadows = !mesh.castShadows;
    const bool played = mesh.castShadows;
    mesh.needReload = false;
    stopPlay();
    const MeshComponent& restored = scene->scene->getComponent<MeshComponent>(GAME);
    EXPECT_NE(restored.castShadows, played);
    EXPECT_TRUE(restored.needReload);
}

TEST_F(UndoProjectTest, NoOpDropped) {
    const size_t before = history()->entries().size();
    EXPECT_TRUE(history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, SPAWNER, ComponentType::Transform,
                                                              "position", position(SPAWNER))));
    EXPECT_EQ(history()->entries().size(), before);
    EXPECT_FALSE(scene->isModified);

    // a real change is a step, named after the inspector label
    EXPECT_TRUE(history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, SPAWNER, ComponentType::Transform,
                                                              "position", Vector3(4, 5, 6))));
    ASSERT_NE(history()->top(), nullptr);
    EXPECT_EQ(history()->top()->name(), "Set Position (Spawner)");
    EXPECT_TRUE(scene->isModified);
    history()->undo();
    EXPECT_FALSE(scene->isModified);

    // the same TRS, name and state again are no steps either
    const size_t count = history()->entries().size();
    Transform& transform = scene->scene->getComponent<Transform>(SPAWNER);
    history()->addCommand(new ObjectTransformCmd(project.get(), sceneId, SPAWNER, transform.position, transform.rotation, transform.scale));
    history()->addCommand(new EntityNameCmd(project.get(), sceneId, SPAWNER, "Spawner"));
    history()->addCommand(EntityStateCmd::active(project.get(), sceneId, {SPAWNER}, true));
    EXPECT_EQ(history()->entries().size(), count);
    EXPECT_FALSE(scene->isModified);
}

TEST_F(UndoProjectTest, CommandNames) {
    history()->addCommand(new ObjectTransformCmd(project.get(), sceneId, SPAWNER, Vector3(1, 1, 1), Quaternion(), Vector3(1, 1, 1)));
    EXPECT_EQ(history()->top()->name(), "Move Spawner");
    history()->addCommand(new EntityNameCmd(project.get(), sceneId, SPAWNER, "Hero"));
    EXPECT_EQ(history()->top()->name(), "Rename Spawner \xE2\x86\x92 Hero");
    auto* multi = new MultiPropertyCmd();
    multi->addPropertyCmd<Vector3>(project.get(), sceneId, GAME, ComponentType::Transform, "position", Vector3(7, 0, 0));
    multi->addPropertyCmd<Vector3>(project.get(), sceneId, SPAWNER, ComponentType::Transform, "scale", Vector3(2, 2, 2));
    history()->addCommand(multi);
    EXPECT_EQ(history()->top()->name(), "Set Properties (2 Objects)");
    history()->addCommand(new DeleteEntityCmd(project.get(), sceneId, std::vector<Entity>{GAME, SPAWNER}));
    EXPECT_EQ(history()->top()->name(), "Delete 2 Objects");
    history()->undo();
    history()->addCommand(new AddComponentCmd(project.get(), sceneId, SPAWNER, ComponentType::LightComponent));
    EXPECT_EQ(history()->top()->name(), "Add Component Light (Hero)");
}

TEST_F(UndoProjectTest, SelectionSteps) {
    const size_t start = history()->entries().size();
    project->setSelectedEntity(sceneId, GAME);
    project->nextSelectionFrame();
    project->setSelectedEntity(sceneId, SPAWNER);
    // two standalone selections: two steps
    ASSERT_EQ(history()->entries().size(), start + 2);
    EXPECT_EQ(history()->top()->name(), "Select Spawner");
    EXPECT_TRUE(history()->top()->info.selectionOnly);
    EXPECT_FALSE(scene->isModified);

    // changes of one frame are one step (a click: clear, then add)
    project->nextSelectionFrame();
    project->clearSelectedEntities(sceneId);
    project->addSelectedEntity(sceneId, GAME);
    project->addSelectedEntity(sceneId, SPAWNER);
    ASSERT_EQ(history()->entries().size(), start + 3);
    EXPECT_EQ(history()->top()->name(), "Select 2 Objects");

    // undo gives the previous selection back
    history()->undo();
    EXPECT_EQ(project->getSelectedEntities(sceneId), std::vector<Entity>{SPAWNER});

    // nothing is recorded while there is a redo tail
    project->nextSelectionFrame();
    project->setSelectedEntity(sceneId, GAME);
    EXPECT_EQ(history()->entries().size(), start + 3);
    EXPECT_TRUE(history()->canRedo());
    history()->redo();

    // a selection made inside a command is that command's: undoing a delete re-selects
    project->nextSelectionFrame();
    project->replaceSelectedEntities(sceneId, {GAME, SPAWNER});
    const size_t beforeDelete = history()->entries().size();
    history()->addCommand(new DeleteEntityCmd(project.get(), sceneId, std::vector<Entity>{GAME, SPAWNER}));
    EXPECT_EQ(history()->entries().size(), beforeDelete + 1);
    EXPECT_TRUE(project->getSelectedEntities(sceneId).empty());
    history()->undo();
    EXPECT_EQ(project->getSelectedEntities(sceneId), (std::vector<Entity>{GAME, SPAWNER}));
    history()->redo();
    history()->undo();

    // the setting off: no standalone steps; the commands still keep their selections
    Project::recordSelectionInUndo = false;
    project->nextSelectionFrame();
    const size_t off = history()->entries().size();
    project->setSelectedEntity(sceneId, ENEMY);
    EXPECT_EQ(history()->entries().size(), off);
    Project::recordSelectionInUndo = true;
}

// --- R31 / AC14: every value-changing command on a GameObject of a prefab instance restores
// the value and its override record on undo, and redo puts the edit back

namespace {

    struct OverrideCase {
        const char* name;
        std::function<Command*(Project*, uint32_t)> make;
    };

}

TEST_F(PrefabUndoTests, ValueCommandsRestoreValuesAndOverrides) {
    const std::vector<OverrideCase> cases = {
        {"PropertyCmd", [](Project* p, uint32_t s) -> Command* {
            return new PropertyCmd<Vector3>(p, s, BODY, ComponentType::Transform, "position", Vector3(9, 8, 7)); }},
        {"ObjectTransformCmd", [](Project* p, uint32_t s) -> Command* {
            return new ObjectTransformCmd(p, s, BODY, Vector3(3, 3, 3), Quaternion(), Vector3(2, 2, 2)); }},
        {"ComponentValuesCmd", [](Project* p, uint32_t s) -> Command* {
            return ComponentValuesCmd::buildReset(p, s, {BODY}, ComponentType::Transform); }},
        {"AddComponentCmd", [](Project* p, uint32_t s) -> Command* {
            return new AddComponentCmd(p, s, BODY, ComponentType::LightComponent); }},
        {"EntityNameCmd", [](Project* p, uint32_t s) -> Command* { return new EntityNameCmd(p, s, BODY, "Renamed"); }},
        {"EntityStateCmd", [](Project* p, uint32_t s) -> Command* {
            return EntityStateCmd::tag(p, s, {BODY}, "Tagged"); }},
        {"MultiPropertyCmd", [](Project* p, uint32_t s) -> Command* {
            auto* multi = new MultiPropertyCmd();
            multi->addPropertyCmd<Vector3>(p, s, BODY, ComponentType::Transform, "scale", Vector3(5, 5, 5));
            return multi; }},
    };
    for (const OverrideCase& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        const std::string valueBefore = encoded(BODY);
        const Prefab::ObjectOverrides overridesBefore = overrides(BODY);
        Command* cmd = testCase.make(project.get(), sceneId);
        ASSERT_NE(cmd, nullptr);
        ASSERT_TRUE(history()->addCommandNoMerge(cmd));
        const std::string valueEdited = encoded(BODY);
        const Prefab::ObjectOverrides overridesEdited = overrides(BODY);
        EXPECT_NE(valueEdited, valueBefore);
        EXPECT_FALSE(same(overridesEdited, overridesBefore)) << "the edit records an override";

        history()->undo();
        EXPECT_EQ(encoded(BODY), valueBefore);
        EXPECT_TRUE(same(overrides(BODY), overridesBefore));

        history()->redo();
        EXPECT_EQ(encoded(BODY), valueEdited);
        EXPECT_TRUE(same(overrides(BODY), overridesEdited));
        history()->undo();
    }

    // removing a component the prefab gave the GameObject: a removed component override
    {
        history()->addCommandNoMerge(new AddComponentCmd(project.get(), sceneId, ENEMY, ComponentType::LightComponent));
        const std::string valueBefore = encoded(ENEMY);
        const Prefab::ObjectOverrides overridesBefore = overrides(ENEMY);
        ASSERT_TRUE(history()->addCommandNoMerge(new RemoveComponentCmd(project.get(), sceneId, ENEMY, ComponentType::LightComponent)));
        history()->undo();
        EXPECT_EQ(encoded(ENEMY), valueBefore);
        EXPECT_TRUE(same(overrides(ENEMY), overridesBefore));
        history()->undo();
    }
}

TEST_F(PrefabUndoTests, InteractiveEditCancel) {
    const std::string valueBefore = encoded(BODY);
    const Prefab::ObjectOverrides overridesBefore = overrides(BODY);
    const bool modifiedBefore = scene->isModified;
    InteractiveEdit edit = history()->beginInteractive("Move Body");
    for (int i = 1; i <= 10; i++) {
        history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, BODY, ComponentType::Transform, "position",
                                                       Vector3((float)i, 0, 0)));
    }
    EXPECT_FALSE(same(overrides(BODY), overridesBefore));
    edit.cancel();
    EXPECT_EQ(encoded(BODY), valueBefore);
    EXPECT_TRUE(same(overrides(BODY), overridesBefore));
    EXPECT_EQ(scene->isModified, modifiedBefore);
}

// --- R26-R29 / AC13: fresh commands from the final play values, undone to the edit values

TEST_F(PlayChangeReviewTests, FreshCommandsUndoToEditValues) {
    const Vector3 editGame = position(GAME);
    const Vector3 editSpawner = position(SPAWNER);
    beginPlay();
    // two play-time edits, then a GameObject made by a command and one a script made
    history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, GAME, ComponentType::Transform, "position", Vector3(1, 1, 1)));
    history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, GAME, ComponentType::Transform, "position", Vector3(2, 2, 2)));
    history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, SPAWNER, ComponentType::Transform, "position", Vector3(5, 5, 5)));
    auto* create = new CreateEntityCmd(project.get(), sceneId, "Made In Play");
    history()->addCommand(create);
    const Entity made = history()->top()->cmd ? static_cast<CreateEntityCmd*>(history()->top()->cmd.get())->getEntity() : NULL_ENTITY;
    ASSERT_NE(made, NULL_ENTITY);
    const Entity scripted = scene->scene->createEntity();
    scene->scene->addComponent<Transform>(scripted);
    history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, scripted, ComponentType::Transform, "position", Vector3(3, 3, 3)));
    stopPlay();

    // back to the edit values; four rows (the two GAME edits are one), the script's disabled
    EXPECT_EQ(position(GAME), editGame);
    EXPECT_EQ(position(SPAWNER), editSpawner);
    const PlayChangeReview::Pending* pending = PlayChangeReview::get().pending(sceneId);
    ASSERT_NE(pending, nullptr);
    ASSERT_EQ(pending->rows.size(), 4u);
    uint64_t gameRow = 0;
    for (const KeepRow& row : pending->rows) {
        if (row.entity == GAME) {
            gameRow = row.id;
            EXPECT_EQ(row.playValue, "(2, 2, 2)");
            EXPECT_TRUE(row.keepable);
        }
        if (row.entity == scripted) {
            EXPECT_FALSE(row.keepable);
            EXPECT_EQ(row.reason, "Created by a script during Play");
        }
        if (row.entity == made) {
            EXPECT_EQ(row.kind, KeepRow::Kind::Created);
            EXPECT_TRUE(row.keepable);
        }
    }
    ASSERT_NE(gameRow, 0u);

    // keep only GAME's: it gets its play value, SPAWNER keeps its edit value, one step
    const size_t steps = history()->entries().size();
    std::vector<uint64_t> ids = {gameRow};
    PlayChangeReview::ApplyResult result = PlayChangeReview::get().apply(project.get(), sceneId, &ids);
    EXPECT_TRUE(result.applied);
    EXPECT_EQ(result.kept, ids);
    EXPECT_EQ(position(GAME), Vector3(2, 2, 2));
    EXPECT_EQ(position(SPAWNER), editSpawner);
    EXPECT_EQ(history()->entries().size(), steps + 1);
    EXPECT_EQ(history()->top()->name(), "Apply Play Mode Changes (1)");
    EXPECT_EQ(history()->top()->info.origin.kind, UndoOriginKind::PlayReview);
    EXPECT_EQ(PlayChangeReview::get().pending(sceneId), nullptr);

    // one undo: the edit value (the fresh command captured it on the restored scene)
    history()->undo();
    EXPECT_EQ(position(GAME), editGame);
}

TEST_F(PlayChangeReviewTests, KeepAllRestoresCreatedGameObjects) {
    beginPlay();
    history()->addCommand(new CreateEntityCmd(project.get(), sceneId, "Kept Object"));
    const Entity made = static_cast<CreateEntityCmd*>(history()->top()->cmd.get())->getEntity();
    stopPlay();
    EXPECT_FALSE(scene->scene->isEntityCreated(made));
    PlayChangeReview::ApplyResult result = PlayChangeReview::get().apply(project.get(), sceneId, nullptr);
    EXPECT_TRUE(result.applied);
    ASSERT_TRUE(scene->scene->isEntityCreated(made));
    EXPECT_EQ(scene->scene->getEntityName(made), "Kept Object");
    history()->undo();
    EXPECT_FALSE(scene->scene->isEntityCreated(made));
}

TEST_F(PlayChangeReviewTests, OverrideRecordedOnTheRestoredInstance) {
    const Prefab::ObjectOverrides overridesBefore = overrides(BODY);
    beginPlay();
    history()->addCommand(new PropertyCmd<Vector3>(project.get(), sceneId, BODY, ComponentType::Transform, "scale", Vector3(4, 4, 4)));
    stopPlay();
    EXPECT_TRUE(same(overrides(BODY), overridesBefore));
    PlayChangeReview::get().apply(project.get(), sceneId, nullptr);
    EXPECT_FALSE(same(overrides(BODY), overridesBefore)) << "kept on the restored instance, an override";
    history()->undo();
    EXPECT_TRUE(same(overrides(BODY), overridesBefore)) << "undo removes it";
}

TEST_F(PlayChangeReviewTests, KeepValuesAfterPlay) {
    beginPlay();
    // a script's change: no command, only the mark
    scene->scene->getComponent<Transform>(SPAWNER).position = Vector3(20, 0, 0);
    PlayChangeReview::get().markComponent(sceneId, SPAWNER, ComponentType::Transform);
    stopPlay();
    const PlayChangeReview::Pending* pending = PlayChangeReview::get().pending(sceneId);
    ASSERT_NE(pending, nullptr);
    ASSERT_EQ(pending->rows.size(), 1u);
    EXPECT_EQ(pending->rows[0].kind, KeepRow::Kind::Component);
    EXPECT_NE(pending->rows[0].property.find("position"), std::string::npos);
    PlayChangeReview::get().apply(project.get(), sceneId, nullptr);
    EXPECT_EQ(position(SPAWNER), Vector3(20, 0, 0));
}

// --- R22: asset metadata edits are steps of the project history

TEST_F(UndoProjectTest, SetAssetMetaCmdIsAProjectStep) {
    const fs::path asset = project->getProjectPath() / "prefabs/Enemy.prefab";
    const fs::path meta = SetAssetMetaCmd::metaPathOf(asset);
    ASSERT_FALSE(fs::exists(meta));
    CommandHistory* projectHistory = project->getProjectCommandHistory();
    ASSERT_TRUE(projectHistory->addCommand(new SetAssetMetaCmd(project.get(), "prefabs/Enemy.prefab", "import.compression", YAML::Node("astc"))));
    EXPECT_EQ(projectHistory->top()->name(), "Set Import Compression (Enemy.prefab)");
    EXPECT_EQ(SetAssetMetaCmd::read(asset, "import.compression").as<std::string>(), "astc");
    // the same value again is no step
    const size_t steps = projectHistory->entries().size();
    projectHistory->addCommand(new SetAssetMetaCmd(project.get(), "prefabs/Enemy.prefab", "import.compression", YAML::Node("astc")));
    EXPECT_EQ(projectHistory->entries().size(), steps);
    // on the one timeline, after the scene's steps
    EXPECT_EQ(UndoTimeline::undoLabel(sceneId), "Undo Set Import Compression (Enemy.prefab)");
    ASSERT_TRUE(UndoTimeline::undo(sceneId));
    EXPECT_FALSE(fs::exists(meta)) << "the sidecar the step made goes with its undo";
    ASSERT_TRUE(UndoTimeline::redo(sceneId));
    EXPECT_EQ(SetAssetMetaCmd::read(asset, "import.compression").as<std::string>(), "astc");
}
