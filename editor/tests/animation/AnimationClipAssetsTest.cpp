// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animation clip assets, editor side (animation clips spec 4.1, 9.R4/R8/R26/R27): the clip
// document (create, commit, undo/redo on the project history, merge keys as drags use them,
// reloads of outside changes, broken and deleted files, read-only sub-clips, listeners), the
// AssetDatabase tracking .anim GUIDs in the JSON, and what rename, move, copy and delete do to
// the Animation Player references: an open scene follows by path, a closed one by GUID when it
// is opened, a deletion keeps the references and its undo relinks them, a copy is a new asset
// with a new GUID, a renamed model takes its sub-clip references and the clips' preview/source
// hints along. Runs a headless project, as PlacementSessionTest does.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/CopyFileCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/RenameFileCmd.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimationClipPoolTesting.h"
#include "pool/ModelPool.h"
#include "pool/TextureDataPool.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseAnimationIO.h"
#include "subsystem/MeshSystem.h"
#include "Stream.h"
#include "util/AssetDatabase.h"
#include "util/Util.h"

#include "support/PosixCompat.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // ---- one headless project for this file

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-clip-assets-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            // the editor's own settings (last project, ...) stay out of the user's
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: clipassets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        // every test of the project (the paths end with each test)
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }
    const fs::path& root() { return headless().dir; }
    CommandHistory* history() { return project()->getProjectCommandHistory(); }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    // a fresh folder per test, so the tests do not see each other's files
    std::string folder(const std::string& name) {
        fs::remove_all(root() / name);
        fs::create_directories(root() / name);
        return name;
    }

    ClipData positionClip(float y = 1.0f) {
        ClipData data;
        ClipTrackData track;
        track.path = "Arm";
        track.channel = ClipChannel::Position;
        ClipKey first;
        first.time = 0.0f;
        first.value = {0.0f, 0.0f, 0.0f};
        ClipKey second;
        second.time = 1.0f;
        second.value = {0.0f, y, 0.0f};
        track.keys = {first, second};
        data.tracks.push_back(track);
        return data;
    }

    float lastKeyY(const ClipData& data) {
        return data.tracks.at(0).keys.back().value.at(1);
    }

    // A loaded scene with one Animation Player listing `entries`
    struct PlayerScene {
        SceneProject* sceneProject = nullptr;
        Entity entity = NULL_ENTITY;

        AnimationPlayerComponent& player() const {
            return sceneProject->scene->getComponent<AnimationPlayerComponent>(entity);
        }
    };

    PlayerScene playerScene(const std::vector<AnimationPlayerClip>& entries) {
        PlayerScene out;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Clips", SceneType::SCENE_3D);
        out.sceneProject = p->getScene(p->getSelectedSceneId());
        Scene* scene = out.sceneProject->scene;
        out.entity = scene->createEntity();
        scene->addComponent<Transform>(out.entity);
        out.sceneProject->entities.push_back(out.entity);   // a GameObject of the scene (saved)
        AnimationPlayerComponent component;
        component.clips = entries;
        scene->addComponent<AnimationPlayerComponent>(out.entity, component);
        out.sceneProject->isModified = false;
        return out;
    }

    AnimationPlayerClip entry(const std::string& clip, const std::string& guid = "") {
        AnimationPlayerClip e;
        e.clip = clip;
        e.guid = guid;
        return e;
    }

    std::shared_ptr<AnimationClipDocument> createClip(const std::string& relative, const ClipData& data = positionClip()) {
        std::string error;
        std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::create(project(), relative, data, "Create", &error);
        EXPECT_TRUE(document) << error;
        return document;
    }

    class AnimationClipAssets : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready) << "the headless project did not open";
        }
    };

}

// ---- the document -------------------------------------------------------------------------

TEST_F(AnimationClipAssets, CreateWritesTheCanonicalFileWithANewGuid) {
    const std::string dir = folder("create");
    const size_t steps = history()->position();
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/walk.anim");
    ASSERT_TRUE(document);
    EXPECT_EQ(history()->position(), steps + 1) << "one project undo step";

    const fs::path file = root() / dir / "walk.anim";
    EXPECT_EQ(document->path(), dir + "/walk.anim");
    EXPECT_EQ(document->absolutePath(), file.lexically_normal());
    const std::string text = readAll(file);
    EXPECT_EQ(text, AnimationClipDocument::fileText(document->data()));
    EXPECT_TRUE(anim::isClipGuid(document->data().guid));
    EXPECT_EQ(anim::readClipGuid(text), document->data().guid);
    EXPECT_EQ(project()->getAssetDatabase().getGuid(file), document->data().guid) << "registered in the asset database";
    EXPECT_FALSE(document->isReadOnly());
    EXPECT_TRUE(document->exists());

    // shared per path, in any accepted form
    std::string error;
    EXPECT_EQ(AnimationClipDocument::open(project(), dir + "/walk.anim", &error), document) << error;
    EXPECT_EQ(AnimationClipDocument::open(project(), file.string(), &error), document) << error;

    // an existing file, a sub-clip, a path outside the project or of another type: refused
    EXPECT_FALSE(AnimationClipDocument::create(project(), dir + "/walk.anim", positionClip(), "Create", &error));
    EXPECT_NE(error.find("already exists"), std::string::npos) << error;
    EXPECT_FALSE(AnimationClipDocument::open(project(), "../outside.anim", &error));
    EXPECT_NE(error.find("outside the project"), std::string::npos) << error;
    EXPECT_FALSE(AnimationClipDocument::open(project(), dir + "/walk.txt", &error));
    EXPECT_FALSE(AnimationClipDocument::create(project(), "models/hero.gltf#Walk", positionClip(), "Create", &error));

    // undo of the creation removes the file (the document says Removed), redo brings it back
    int removed = 0;
    const int listener = document->addListener([&](AnimationClipDocument&, AnimationClipDocument::Change change) {
        if (change == AnimationClipDocument::Change::Removed) removed++;
    });
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(fs::exists(file));
    EXPECT_FALSE(document->exists());
    EXPECT_EQ(removed, 1);
    EXPECT_TRUE(project()->getAssetDatabase().getGuid(file).empty());
    ASSERT_TRUE(history()->redo());
    EXPECT_TRUE(document->exists());
    EXPECT_EQ(readAll(file), text) << "the same GUID again";
    document->removeListener(listener);
}

TEST_F(AnimationClipAssets, CommitUndoRedoAndListeners) {
    const std::string dir = folder("commit");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/jump.anim");
    ASSERT_TRUE(document);
    const fs::path file = document->absolutePath();
    const std::string guid = document->data().guid;
    const std::string original = readAll(file);

    std::vector<AnimationClipDocument::Change> changes;
    const int listener = document->addListener([&](AnimationClipDocument&, AnimationClipDocument::Change change) {
        changes.push_back(change);
    });

    // the edit keeps the file's GUID whatever the data says
    ClipData edited = document->data();
    edited.tracks[0].keys.back().value[1] = 2.0f;
    edited.guid = anim::generateClipGuid();
    const uint64_t revision = document->revision();
    const size_t steps = history()->position();
    std::string error;
    ASSERT_TRUE(document->commit(edited, "Move Key", "", &error)) << error;
    EXPECT_EQ(history()->position(), steps + 1);
    EXPECT_GT(document->revision(), revision);
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 2.0f);
    EXPECT_EQ(document->data().guid, guid);
    EXPECT_EQ(anim::readClipGuid(readAll(file)), guid);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], AnimationClipDocument::Change::Edited);

    // nothing changed: nothing written, no step
    ASSERT_TRUE(document->commit(document->data(), "Nothing", "", &error)) << error;
    EXPECT_EQ(history()->position(), steps + 1);
    EXPECT_EQ(changes.size(), 1u);

    // normalize() reports what it changed (unsorted keys)
    ClipData unsorted = document->data();
    std::swap(unsorted.tracks[0].keys[0], unsorted.tracks[0].keys[1]);
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(document->commit(unsorted, "Sort", "", &error, &warnings)) << error;
    EXPECT_FALSE(warnings.empty());
    EXPECT_EQ(history()->position(), steps + 1) << "sorted, it is the same clip";

    // undo and redo reload the document (Reloaded)
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(readAll(file), original);
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 1.0f);
    EXPECT_EQ(changes.back(), AnimationClipDocument::Change::Reloaded);
    ASSERT_TRUE(history()->redo());
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 2.0f);

    // a listener removed while the listeners run is not called again
    int second = 0;
    int secondId = 0;
    secondId = document->addListener([&](AnimationClipDocument& d, AnimationClipDocument::Change) {
        second++;
        d.removeListener(secondId);
    });
    edited.tracks[0].keys.back().value[1] = 3.0f;
    ASSERT_TRUE(document->commit(edited, "Move Key", "", &error)) << error;
    edited.tracks[0].keys.back().value[1] = 4.0f;
    ASSERT_TRUE(document->commit(edited, "Move Key", "", &error)) << error;
    EXPECT_EQ(second, 1);
    document->removeListener(listener);
}

// The controller's key drags (beginKeyDrag / updateKeyDrag / endKeyDrag) commit every update
// with the drag's merge key: one undo step per drag, the next drag is another
TEST_F(AnimationClipAssets, MergeKeysMakeOneStepPerDrag) {
    const std::string dir = folder("merge");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/drag.anim");
    ASSERT_TRUE(document);
    const size_t steps = history()->position();
    std::string error;

    ClipData data = document->data();
    for (float y : {1.5f, 2.0f, 2.5f}) {
        data.tracks[0].keys.back().value[1] = y;
        ASSERT_TRUE(document->commit(data, "Drag Keys", "drag#1", &error)) << error;
    }
    EXPECT_EQ(history()->position(), steps + 1) << "the updates of one drag merged";
    for (float y : {3.0f, 3.5f}) {
        data.tracks[0].keys.back().value[1] = y;
        ASSERT_TRUE(document->commit(data, "Drag Keys", "drag#2", &error)) << error;
    }
    EXPECT_EQ(history()->position(), steps + 2) << "a new drag, a new step";
    // a discrete edit in between does not merge with either
    data.tracks[0].keys.back().value[1] = 4.0f;
    ASSERT_TRUE(document->commit(data, "Set Value", "", &error)) << error;
    data.tracks[0].keys.back().value[1] = 4.5f;
    ASSERT_TRUE(document->commit(data, "Drag Keys", "drag#2", &error)) << error;
    EXPECT_EQ(history()->position(), steps + 4);

    ASSERT_TRUE(history()->undo());
    ASSERT_TRUE(history()->undo());
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 3.5f);
    ASSERT_TRUE(history()->undo());
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 2.5f) << "the second drag undone at once";
    ASSERT_TRUE(history()->undo());
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 1.0f) << "the first drag undone at once";
}

TEST_F(AnimationClipAssets, OutsideChangesReloadAndThePoolDropsTheClip) {
    const std::string dir = folder("reload");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/idle.anim");
    ASSERT_TRUE(document);
    const fs::path file = document->absolutePath();

    // a player loaded it through the pool
    std::string error;
    ASSERT_TRUE(AnimationClipPool::getOrLoad(file.string(), &error)) << error;
    const uint64_t generation = AnimationClipPool::getGeneration(file.string());

    std::vector<AnimationClipDocument::Change> changes;
    const int listener = document->addListener([&](AnimationClipDocument&, AnimationClipDocument::Change change) {
        changes.push_back(change);
    });

    // another program writes it: the poll (or auto-sync's filesChanged) reloads it
    ClipData outside = positionClip(5.0f);
    outside.guid = document->data().guid;
    writeAll(file, anim::writeClip(outside) + "\n");
    AnimationClipDocument::filesChanged({file}, project());
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], AnimationClipDocument::Change::Reloaded);
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 5.0f);
    EXPECT_GT(AnimationClipPool::getGeneration(file.string()), generation) << "the pool dropped the old clip";
    std::shared_ptr<const AnimationClip> reloaded = AnimationClipPool::getOrLoad(file.string(), &error);
    ASSERT_TRUE(reloaded) << error;
    EXPECT_FLOAT_EQ(reloaded->getLength(), 1.0f);

    // broken: the last valid content stays, with a warning that says so (line:col)
    const uint64_t revision = document->revision();
    writeAll(file, "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  oops\n}\n");
    AnimationClipDocument::checkDiskChanges();
    EXPECT_EQ(document->revision(), revision);
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 5.0f);
    ASSERT_FALSE(document->loadWarnings().empty());
    EXPECT_NE(document->loadWarnings()[0].message.find("not a valid animation clip"), std::string::npos);
    EXPECT_NE(document->loadWarnings()[0].message.find("4:"), std::string::npos) << document->loadWarnings()[0].message;
    // an edit through the document replaces the broken file
    ClipData edited = document->data();
    edited.tracks[0].keys.back().value[1] = 6.0f;
    ASSERT_TRUE(document->commit(edited, "Fix", "", &error)) << error;
    EXPECT_TRUE(anim::parseClip(readAll(file)).ok());

    // deleted outside: Removed, and commits fail until it is back
    fs::remove(file);
    AnimationClipDocument::checkDiskChanges();
    EXPECT_EQ(changes.back(), AnimationClipDocument::Change::Removed);
    EXPECT_FALSE(document->exists());
    EXPECT_FALSE(document->commit(edited, "Edit", "", &error));
    EXPECT_FALSE(AnimationClipDocument::open(project(), dir + "/idle.anim", &error));
    document->removeListener(listener);
}

TEST_F(AnimationClipAssets, AReloadMarksThePlayersUsingTheClip) {
    const std::string dir = folder("players");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/wave.anim");
    ASSERT_TRUE(document);
    PlayerScene scene = playerScene({entry(dir + "/wave.anim", document->data().guid)});
    scene.player().needsLoad = false;

    ClipData edited = document->data();
    edited.tracks[0].keys.back().value[1] = 9.0f;
    std::string error;
    ASSERT_TRUE(document->commit(edited, "Edit", "", &error)) << error;
    EXPECT_TRUE(scene.player().needsLoad) << "edit preview and play see the edit";

    std::vector<ClipUser> users = findClipUsers(project(), dir + "/wave.anim");
    ASSERT_EQ(users.size(), 1u);
    EXPECT_EQ(users[0].sceneId, scene.sceneProject->id);
    EXPECT_EQ(users[0].entity, scene.entity);
    EXPECT_EQ(users[0].entry, 0u);
    EXPECT_FALSE(users[0].prefabAsset);
    EXPECT_EQ(findClipUsers(project(), document->data().guid).size(), 1u) << "by GUID";
    EXPECT_EQ(findClipUsers(project(), document->absolutePath().string()).size(), 1u) << "by absolute path";
    EXPECT_TRUE(findClipUsers(project(), dir + "/other.anim").empty());
}

TEST_F(AnimationClipAssets, SubClipDocumentsAreReadOnly) {
    const std::string dir = folder("subclip");
    std::string error;
    // not a model, no model file, no such animation: refused
    EXPECT_FALSE(AnimationClipDocument::open(project(), dir + "/hero.gltf#Walk", &error));
    EXPECT_NE(error.find("does not exist"), std::string::npos) << error;

    // the pose fixture's animated hero (T1c; buffers embedded)
    const fs::path fixture = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" /
                             "assets" / "models" / "hero_animated.gltf";
    ASSERT_TRUE(fs::is_regular_file(fixture)) << fixture;
    fs::copy_file(fixture, root() / dir / "hero_animated.gltf", fs::copy_options::overwrite_existing);
    const std::string model = dir + "/hero_animated.gltf";
    std::vector<anim::ModelClipInfo> clips;
    ASSERT_TRUE(anim::listModelFileClips(project()->normalizeToAssetsRelative(root() / model).generic_string(), clips, &error)) << error;
    ASSERT_FALSE(clips.empty());
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project(), model + "#" + clips[0].name, &error);
    ASSERT_TRUE(document) << error;
    EXPECT_TRUE(document->isReadOnly());
    EXPECT_EQ(document->path(), model + "#" + clips[0].name);
    EXPECT_FALSE(document->commit(document->data(), "Edit", "", &error));
    EXPECT_NE(error.find("read-only"), std::string::npos) << error;
    // by index: the same document (the canonical form is the name)
    EXPECT_EQ(AnimationClipDocument::open(project(), model + "#" + std::to_string(clips[0].index), &error), document);
    EXPECT_FALSE(document->data().tracks.empty());
}

// ---- the asset database ------------------------------------------------------------------

TEST_F(AnimationClipAssets, AssetDatabaseReadsAndWritesTheJsonGuid) {
    const std::string dir = folder("database");
    EXPECT_TRUE(project()->getAssetDatabase().isTrackedFile("a/b/walk.anim"));
    EXPECT_TRUE(project()->getAssetDatabase().isTrackedFile("a/b/WALK.ANIM"));
    EXPECT_FALSE(project()->getAssetDatabase().isTrackedFile("a/hero.gltf#Walk"));

    // a clip written without a GUID gets one (inserted after "version", the rest unchanged)
    ClipData data = positionClip();
    const std::string text = anim::writeClip(data);
    const fs::path file = root() / dir / "legacy.anim";
    writeAll(file, text);
    EXPECT_TRUE(AssetDatabase::readGuid(file).empty());
    const std::string guid = AssetDatabase::generateGuid();
    ASSERT_TRUE(AssetDatabase::writeGuid(file, guid));
    EXPECT_EQ(AssetDatabase::readGuid(file), guid);
    anim::ClipParseResult parsed = anim::parseClip(readAll(file));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->guid, guid);
    data.guid = guid;
    EXPECT_EQ(readAll(file), anim::writeClip(data)) << "the canonical text with the GUID";

    // a Unity YAML .anim or broken JSON is left alone
    const fs::path unity = root() / dir / "unity.anim";
    const std::string yaml = "%YAML 1.1\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Walk\n";
    writeAll(unity, yaml);
    EXPECT_FALSE(AssetDatabase::writeGuid(unity, guid));
    EXPECT_EQ(readAll(unity), yaml);

    // the refresh finds a clip made outside the editor
    const fs::path outside = root() / dir / "outside.anim";
    ClipData other = positionClip();
    other.guid = anim::generateClipGuid();
    writeAll(outside, anim::writeClip(other));
    project()->refreshAssetDatabase(true);
    EXPECT_EQ(project()->getAssetDatabase().getGuid(outside), other.guid);
    fs::remove(unity);
    project()->refreshAssetDatabase(true);
}

// ---- rename, move, copy, delete ----------------------------------------------------------

TEST_F(AnimationClipAssets, RenameUpdatesOpenScenesAndKeepsTheGuid) {
    const std::string dir = folder("rename");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/run.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;
    PlayerScene scene = playerScene({entry(dir + "/run.anim", guid)});
    scene.player().needsLoad = false;
    project()->setInspectedAsset(document->absolutePath());
    std::vector<AnimationClipDocument::Change> changes;
    const int listener = document->addListener([&](AnimationClipDocument&, AnimationClipDocument::Change change) {
        changes.push_back(change);
    });

    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "run.anim", "sprint.anim", (root() / dir).string())));
    const fs::path renamed = root() / dir / "sprint.anim";
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/sprint.anim");
    EXPECT_EQ(scene.player().clips[0].guid, guid);
    EXPECT_TRUE(scene.player().needsLoad);
    EXPECT_TRUE(scene.sceneProject->isModified);
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/sprint.anim"));
    EXPECT_EQ(anim::readClipGuid(readAll(renamed)), guid) << "the file keeps its GUID";
    // the open document follows the file (the Animation window and the Inspector keep it)
    EXPECT_TRUE(document->exists());
    EXPECT_EQ(document->path(), dir + "/sprint.anim");
    EXPECT_EQ(document->absolutePath(), renamed.lexically_normal());
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes.back(), AnimationClipDocument::Change::Reloaded);
    std::string error;
    EXPECT_EQ(AnimationClipDocument::open(project(), dir + "/sprint.anim", &error), document) << error;
    EXPECT_EQ(project()->getInspectedAsset(), renamed.lexically_normal()) << "the inspected asset follows";
    // and edits keep working at the new path
    ClipData edited = document->data();
    edited.tracks[0].keys.back().value[1] = 7.0f;
    ASSERT_TRUE(document->commit(edited, "Edit", "", &error)) << error;
    ASSERT_TRUE(history()->undo());

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/run.anim");
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/run.anim"));
    EXPECT_EQ(document->path(), dir + "/run.anim");
    EXPECT_EQ(project()->getInspectedAsset(), (root() / dir / "run.anim").lexically_normal());
    document->removeListener(listener);

    // deleting it clears the Inspector's asset
    ASSERT_TRUE(history()->addCommand(new DeleteFileCmd(project(), {document->absolutePath()}, project()->getProjectPath())));
    EXPECT_TRUE(project()->getInspectedAsset().empty());
    ASSERT_TRUE(history()->undo());
}

// spec 9.R8: rename a clip while the scene using it is closed, open the scene: the reference
// resolves by GUID (its hint repaired) and the clip loads from the new path
TEST_F(AnimationClipAssets, AClosedSceneFindsARenamedClipByGuid) {
    const std::string dir = folder("closed");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/kick.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;

    // what the closed scene's file holds: the old path and the GUID (decoded on open)
    Scene closed;
    const Entity entity = closed.createEntity();
    AnimationPlayerComponent stored;
    stored.clips = {entry(dir + "/kick.anim", guid)};
    closed.addComponent<AnimationPlayerComponent>(entity, stored);

    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "kick.anim", "roundhouse.anim", (root() / dir).string())));
    EXPECT_EQ(closed.getComponent<AnimationPlayerComponent>(entity).clips[0].clip, dir + "/kick.anim") << "a closed scene is not rewritten";

    // opening it (Project::loadSceneProjectData, the play copy, prefab loads) resolves it
    closed.getComponent<AnimationPlayerComponent>(entity).needsLoad = false;
    EXPECT_TRUE(project()->resolveRegistryClipRefs(&closed));
    const AnimationPlayerComponent& player = closed.getComponent<AnimationPlayerComponent>(entity);
    EXPECT_EQ(player.clips[0].clip, dir + "/roundhouse.anim");
    EXPECT_EQ(player.clips[0].guid, guid);
    EXPECT_TRUE(player.needsLoad);
    std::string error;
    EXPECT_TRUE(AnimationClipPool::getOrLoad(project()->resolveAssetPath(player.clips[0].clip).string(), &error)) << error;
    EXPECT_FALSE(project()->resolveRegistryClipRefs(&closed)) << "resolved: nothing changes the second time";

    // a reference without a GUID (hand-written) gets it filled in, where it points unchanged
    AnimationPlayerClip byPath = entry(dir + "/roundhouse.anim");
    EXPECT_FALSE(project()->resolveClipReference(byPath));
    EXPECT_EQ(byPath.guid, guid);
    // a GUID no file has: Missing, the hint stays
    AnimationPlayerClip missing = entry(dir + "/gone.anim", anim::generateClipGuid());
    EXPECT_FALSE(project()->resolveClipReference(missing));
    EXPECT_EQ(missing.clip, dir + "/gone.anim");
    // sub-clips are path-only
    AnimationPlayerClip subClip = entry(dir + "/hero.gltf#Walk");
    EXPECT_FALSE(project()->resolveClipReference(subClip));
    EXPECT_TRUE(subClip.guid.empty());
}

TEST_F(AnimationClipAssets, AnOutsideMoveIsFollowedByGuid) {
    const std::string dir = folder("outsidemove");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/swim.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;
    PlayerScene scene = playerScene({entry(dir + "/swim.anim", guid)});

    fs::create_directories(root() / dir / "water");
    fs::rename(root() / dir / "swim.anim", root() / dir / "water" / "swim.anim");
    project()->refreshAssetDatabase(true);
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/water/swim.anim");
    EXPECT_TRUE(scene.sceneProject->isModified);
}

TEST_F(AnimationClipAssets, DeleteKeepsTheReferencesAndUndoRelinksThem) {
    const std::string dir = folder("delete");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/fall.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;
    const fs::path file = document->absolutePath();
    PlayerScene scene = playerScene({entry(dir + "/fall.anim", guid)});
    scene.player().needsLoad = false;

    ASSERT_TRUE(history()->addCommand(new DeleteFileCmd(project(), {file}, project()->getProjectPath())));
    EXPECT_FALSE(fs::exists(file));
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/fall.anim") << "Missing, not cleared";
    EXPECT_EQ(scene.player().clips[0].guid, guid);
    EXPECT_TRUE(scene.player().needsLoad);
    EXPECT_TRUE(project()->getAssetDatabase().getPath(guid).empty());
    EXPECT_FALSE(document->exists());
    EXPECT_EQ(findClipUsers(project(), dir + "/fall.anim").size(), 1u) << "the Inspector still lists the user";
    std::string error;
    EXPECT_FALSE(AnimationClipPool::getOrLoad(file.string(), &error));

    scene.player().needsLoad = false;
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(fs::exists(file));
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/fall.anim")) << "back with its GUID";
    EXPECT_TRUE(scene.player().needsLoad) << "relinked";
    EXPECT_TRUE(document->exists());
    EXPECT_TRUE(AnimationClipPool::getOrLoad(file.string(), &error)) << error;
    // the entry is a user of the GUID again, and its resolved path loads
    EXPECT_EQ(findClipUsers(project(), guid).size(), 1u);
    AnimationPlayerClip resolved = scene.player().clips[0];
    EXPECT_FALSE(project()->resolveClipReference(resolved)) << "nothing to repair";
    EXPECT_TRUE(AnimationClipPool::getOrLoad(project()->resolveAssetPath(resolved.clip).string(), &error)) << error;
}

TEST_F(AnimationClipAssets, ACopyIsANewAssetWithANewGuid) {
    const std::string dir = folder("copy");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/a/roll.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;
    fs::create_directories(root() / dir / "b");

    ASSERT_TRUE(history()->addCommand(new CopyFileCmd(project(), std::vector<std::string>{document->absolutePath().string()},
                                                      (root() / dir / "b").string(), true)));
    const fs::path copy = root() / dir / "b" / "roll.anim";
    const std::string copyGuid = AssetDatabase::readGuid(copy);
    EXPECT_TRUE(anim::isClipGuid(copyGuid));
    EXPECT_NE(copyGuid, guid);
    EXPECT_EQ(project()->getAssetDatabase().getGuid(copy), copyGuid);
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/a/roll.anim")) << "the original keeps its GUID";
    EXPECT_TRUE(anim::parseClip(readAll(copy)).ok());

    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(fs::exists(copy));
    EXPECT_TRUE(project()->getAssetDatabase().getPath(copyGuid).empty());
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(AssetDatabase::readGuid(copy), copyGuid) << "a redo gives the copy the same GUID";
}

TEST_F(AnimationClipAssets, AMoveIntoAnotherFolderKeepsTheGuid) {
    const std::string dir = folder("move");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/a/climb.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;
    PlayerScene scene = playerScene({entry(dir + "/a/climb.anim", guid)});
    fs::create_directories(root() / dir / "b");

    ASSERT_TRUE(history()->addCommand(new CopyFileCmd(project(), std::vector<std::string>{document->absolutePath().string()},
                                                      (root() / dir / "b").string(), false)));
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/b/climb.anim"));
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/b/climb.anim");
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/a/climb.anim"));
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/a/climb.anim");

    // a whole folder moves the same way
    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "a", "c", (root() / dir).string())));
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/c/climb.anim"));
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/c/climb.anim");
}

// spec 9.R26: a renamed model takes its sub-clip references along, and the clips previewing it
// (or extracted from it) name it again, in the rename's own undo step
TEST_F(AnimationClipAssets, AModelRenameRemapsSubClipsAndClipHints) {
    const std::string dir = folder("model");
    writeAll(root() / dir / "hero.gltf", "{\"asset\": {\"version\": \"2.0\"}}\n");
    // an asset of the database (S04: a rename moves what it tracks; the watcher or a refresh
    // imports a file written outside the editor)
    project()->importPrefabFiles(root() / dir / "hero.gltf");
    ClipData data = positionClip();
    data.previewModel = dir + "/hero.gltf";
    data.source.kind = "model";
    data.source.path = dir + "/hero.gltf";
    data.source.name = "Walk";
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/walk.anim", data);
    ASSERT_TRUE(document);
    const std::string before = readAll(document->absolutePath());
    PlayerScene scene = playerScene({entry(dir + "/hero.gltf#Walk"), entry(dir + "/walk.anim", document->data().guid)});

    const size_t steps = history()->position();
    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "hero.gltf", "knight.gltf", (root() / dir).string())));
    EXPECT_EQ(history()->position(), steps + 1);
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/knight.gltf#Walk");
    EXPECT_EQ(scene.player().clips[1].clip, dir + "/walk.anim");
    EXPECT_EQ(document->data().previewModel, dir + "/knight.gltf") << "the open document reloaded";
    EXPECT_EQ(document->data().source.path, dir + "/knight.gltf");
    EXPECT_EQ(document->data().source.name, "Walk");

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/hero.gltf#Walk");
    EXPECT_EQ(readAll(document->absolutePath()), before) << "the clip's own bytes back";
    EXPECT_EQ(document->data().previewModel, dir + "/hero.gltf");

    // deleting the model keeps the sub-clip reference (Missing)
    ASSERT_TRUE(history()->addCommand(new DeleteFileCmd(project(), {root() / dir / "hero.gltf"}, project()->getProjectPath())));
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/hero.gltf#Walk");
    ASSERT_TRUE(history()->undo());
}

// A model edited or reimported outside the editor (its .gltf, or the .bin buffer next to it):
// the pool drops its sub-clips and the players using them reload, through the editor's poll
// and through filesChanged (auto-sync, sync_files, an FBX reimport in Project::reloadModelFile)
TEST_F(AnimationClipAssets, AChangedModelDropsItsSubClips) {
    const std::string dir = folder("modelchange");
    const fs::path gltf = root() / dir / "hero.gltf";
    const fs::path bin = root() / dir / "hero.bin";
    writeAll(gltf, "{\"asset\": {\"version\": \"2.0\"}, \"buffers\": [{\"uri\": \"hero.bin\", \"byteLength\": 4}]}\n");
    writeAll(bin, "abcd");

    // sub-clips without the model task's builder: the pool's test seam
    AnimationClipPoolTestHooks hooks;
    hooks.listModelClips = [](const std::string&, std::vector<anim::ModelClipInfo>& out, std::string*) {
        anim::ModelClipInfo walk;
        walk.index = 0;
        walk.name = "Walk";
        out = {walk};
        return true;
    };
    hooks.buildSubClip = [](const std::string&, ClipData& out, std::vector<ClipWarning>&, std::string*) {
        out = positionClip();
        return true;
    };
    setAnimationClipPoolTestHooks(hooks);
    const std::string reference = gltf.generic_string() + "#Walk";
    auto loadSubClip = [&]() {
        std::string error;
        EXPECT_TRUE(AnimationClipPool::getOrLoad(reference, &error)) << error;
    };

    PlayerScene scene = playerScene({entry(dir + "/hero.gltf#Walk")});
    loadSubClip();
    AnimationClipDocument::checkDiskChanges(project());   // the poll starts watching the model
    scene.player().needsLoad = false;
    AnimationClipDocument::checkDiskChanges(project());
    EXPECT_FALSE(scene.player().needsLoad) << "nothing changed";
    EXPECT_TRUE(AnimationClipPool::get(reference));

    // the buffer changes: the poll sees it
    writeAll(bin, "abcdefgh");
    AnimationClipDocument::checkDiskChanges(project());
    EXPECT_TRUE(scene.player().needsLoad);
    EXPECT_FALSE(AnimationClipPool::get(reference)) << "the sub-clip was dropped";

    // the paths auto-sync and sync_files report: the .bin, the .gltf itself
    for (const fs::path& changed : {bin, gltf}) {
        loadSubClip();
        scene.player().needsLoad = false;
        AnimationClipDocument::filesChanged({changed}, project());
        EXPECT_TRUE(scene.player().needsLoad) << changed;
        EXPECT_FALSE(AnimationClipPool::get(reference)) << changed;
    }
    // another model's buffer: nothing
    writeAll(root() / dir / "other.bin", "x");
    loadSubClip();
    scene.player().needsLoad = false;
    AnimationClipDocument::filesChanged({root() / dir / "other.bin"}, project());
    EXPECT_FALSE(scene.player().needsLoad);
    EXPECT_TRUE(AnimationClipPool::get(reference));

    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
    AnimationClipPool::invalidate(gltf.generic_string());
}

// The same through real files (Stream encodes the player since T2a): save a scene, close it,
// rename its clip, open it again: the reference resolves by GUID (the scene opens modified with
// the new path), and the closed scene's file was never rewritten
TEST_F(AnimationClipAssets, ASavedSceneOpensWithARenamedClip) {
    const std::string dir = folder("sceneroundtrip");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/dash.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;
    PlayerScene scene = playerScene({entry(dir + "/dash.anim", guid)});
    const uint32_t sceneId = scene.sceneProject->id;
    const fs::path sceneFile = root() / dir / "Level.scene";
    ASSERT_TRUE(project()->saveSceneToPath(sceneId, sceneFile));
    const std::string saved = readAll(sceneFile);
    ASSERT_NE(saved.find(dir + "/dash.anim"), std::string::npos) << saved;
    ASSERT_NE(saved.find(guid), std::string::npos) << "the GUID is in the file";
    project()->getScene(sceneId)->isModified = false;
    project()->closeScene(sceneId, true);
    ASSERT_FALSE(project()->getScene(sceneId) && project()->getScene(sceneId)->scene) << "closed";

    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "dash.anim", "sprint.anim", (root() / dir).string())));
    EXPECT_EQ(readAll(sceneFile), saved) << "no closed file is rewritten";

    project()->openScene(sceneFile, false);
    SceneProject* opened = nullptr;
    for (SceneProject& candidate : project()->getScenes()) {
        if (candidate.scene && (root() / candidate.filepath).lexically_normal() == sceneFile.lexically_normal()) opened = &candidate;
    }
    ASSERT_TRUE(opened) << "the scene opened";
    auto players = opened->scene->getComponentArray<AnimationPlayerComponent>();
    ASSERT_EQ(players->size(), 1u);
    const AnimationPlayerComponent& player = players->getComponentFromIndex(0);
    ASSERT_EQ(player.clips.size(), 1u);
    EXPECT_EQ(player.clips[0].clip, dir + "/sprint.anim");
    EXPECT_EQ(player.clips[0].guid, guid);
    EXPECT_TRUE(opened->isModified) << "saved with the repaired path next time";
    std::string error;
    EXPECT_TRUE(AnimationClipPool::getOrLoad(project()->resolveAssetPath(player.clips[0].clip).string(), &error)) << error;
    opened->isModified = false;
}

// Reverting a prefab instance's override of its clips reloads the player (as a decode does)
TEST_F(AnimationClipAssets, RevertingAClipOverrideReloadsThePlayer) {
    const std::string dir = folder("revert");
    PlayerScene scene = playerScene({entry(dir + "/a.anim")});
    const uint32_t sceneId = scene.sceneProject->id;
    CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project(), sceneId, fs::path(dir) / "Hero.prefab",
        Stream::encodeEntity(scene.entity, scene.sceneProject->scene, project(), scene.sceneProject)));
    ASSERT_TRUE(project()->isPrefabInstanceRoot(sceneId, scene.entity));

    for (const std::vector<std::string>& properties : {std::vector<std::string>{"clips"}, std::vector<std::string>{}}) {
        scene.player().clips = {entry(dir + "/b.anim")};
        ASSERT_TRUE(project()->recordPrefabEdit(sceneId, scene.entity, ComponentType::AnimationPlayerComponent, {"clips"}));
        scene.player().needsLoad = false;
        ASSERT_TRUE(project()->revertPropertyOverride(sceneId, scene.entity, ComponentType::AnimationPlayerComponent, properties));
        ASSERT_EQ(scene.player().clips.size(), 1u);
        EXPECT_EQ(scene.player().clips[0].clip, dir + "/a.anim");
        EXPECT_TRUE(scene.player().needsLoad) << (properties.empty() ? "whole component" : "clips");
    }
    scene.sceneProject->isModified = false;
}

namespace {

    std::string base64(const std::vector<uint8_t>& bytes) {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const uint32_t n = (uint32_t(bytes[i]) << 16) | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) |
                               (i + 2 < bytes.size() ? uint32_t(bytes[i + 2]) : 0);
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? table[n & 63] : '=';
        }
        return out;
    }

    // A one-node glTF whose animation "Move" lifts "Box" to y in one second (buffer in a data URI)
    std::string movingBoxGltf(float y) {
        const float data[8] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, y, 0.0f};
        std::vector<uint8_t> bytes(sizeof(data));
        std::memcpy(bytes.data(), data, sizeof(data));
        return std::string("{\"asset\": {\"version\": \"2.0\"}, \"scene\": 0, \"scenes\": [{\"nodes\": [0]}], \"nodes\": [{\"name\": \"Box\"}],") +
               "\"buffers\": [{\"byteLength\": 32, \"uri\": \"data:application/octet-stream;base64," + base64(bytes) + "\"}]," +
               "\"bufferViews\": [{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 8}, {\"buffer\": 0, \"byteOffset\": 8, \"byteLength\": 24}]," +
               "\"accessors\": [{\"bufferView\": 0, \"componentType\": 5126, \"count\": 2, \"type\": \"SCALAR\", \"min\": [0], \"max\": [1]}," +
               "{\"bufferView\": 1, \"componentType\": 5126, \"count\": 2, \"type\": \"VEC3\"}]," +
               "\"animations\": [{\"name\": \"Move\", \"samplers\": [{\"input\": 0, \"output\": 1, \"interpolation\": \"LINEAR\"}]," +
               "\"channels\": [{\"sampler\": 0, \"target\": {\"node\": 0, \"path\": \"translation\"}}]}]}\n";
    }

}

// A .gltf overwritten on disk with a changed animation: its sub-clip (document and pool) is built
// again from the new file, not from the pooled parse of the old one (MeshSystem::invalidateModelFile)
TEST_F(AnimationClipAssets, AnOverwrittenModelRebuildsItsSubClips) {
    const std::string dir = folder("overwrite");
    const fs::path gltf = root() / dir / "mover.gltf";
    writeAll(gltf, movingBoxGltf(1.0f));
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project(), dir + "/mover.gltf#Move", &error);
    ASSERT_TRUE(document) << error;
    ASSERT_TRUE(document->isReadOnly());
    ASSERT_EQ(document->data().tracks.size(), 1u);
    EXPECT_EQ(document->data().tracks[0].path, "Box");
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 1.0f);
    const std::string reference = gltf.generic_string() + "#Move";
    std::shared_ptr<const AnimationClip> first = AnimationClipPool::getOrLoad(reference, &error);
    ASSERT_TRUE(first) << error;
    EXPECT_FLOAT_EQ(lastKeyY(first->toData()), 1.0f);

    // auto-sync / sync_files report the path
    writeAll(gltf, movingBoxGltf(3.0f));
    AnimationClipDocument::filesChanged({gltf}, project());
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 3.0f) << "the document rebuilt from the new file";
    std::shared_ptr<const AnimationClip> second = AnimationClipPool::getOrLoad(reference, &error);
    ASSERT_TRUE(second) << error;
    EXPECT_FLOAT_EQ(lastKeyY(second->toData()), 3.0f) << "the pool rebuilt from the new file";

    // the editor's poll (a later write time)
    writeAll(gltf, movingBoxGltf(5.0f));
    std::error_code ec;
    fs::last_write_time(gltf, fs::last_write_time(gltf, ec) + std::chrono::seconds(2), ec);
    AnimationClipDocument::checkDiskChanges(project());
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 5.0f);
    std::shared_ptr<const AnimationClip> third = AnimationClipPool::getOrLoad(reference, &error);
    ASSERT_TRUE(third) << error;
    EXPECT_FLOAT_EQ(lastKeyY(third->toData()), 5.0f);
    AnimationClipPool::invalidate(gltf.generic_string());
}

namespace {

    // An indexed one-triangle glTF with a 2x2 PNG base color texture (buffer and image in data URIs)
    std::string texturedTriangleGltf() {
        const float data[15] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,   // POSITION
                                0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};                     // TEXCOORD_0
        const uint16_t indices[3] = {0, 1, 2};
        std::vector<uint8_t> bytes(sizeof(data) + sizeof(indices));
        std::memcpy(bytes.data(), data, sizeof(data));
        std::memcpy(bytes.data() + sizeof(data), indices, sizeof(indices));
        const std::string png = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGP4z8AAQv8ZYAwAQ84H+VjtZqAAAAAASUVORK5CYII=";
        return std::string("{\"asset\": {\"version\": \"2.0\"}, \"scene\": 0, \"scenes\": [{\"nodes\": [0]}], \"nodes\": [{\"name\": \"Tri\", \"mesh\": 0}],") +
               "\"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, \"TEXCOORD_0\": 1}, \"indices\": 2, \"material\": 0}]}]," +
               "\"materials\": [{\"pbrMetallicRoughness\": {\"baseColorTexture\": {\"index\": 0}}}]," +
               "\"textures\": [{\"source\": 0}], \"images\": [{\"uri\": \"data:image/png;base64," + png + "\"}]," +
               "\"buffers\": [{\"byteLength\": 66, \"uri\": \"data:application/octet-stream;base64," + base64(bytes) + "\"}]," +
               "\"bufferViews\": [{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36}, {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 24}," +
               "{\"buffer\": 0, \"byteOffset\": 60, \"byteLength\": 6}]," +
               "\"accessors\": [{\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]}," +
               "{\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC2\"}," +
               "{\"bufferView\": 2, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\"}]}\n";
    }

}

// A model reported changed (auto-sync picks up every written model, a bake's new .glb that the
// editor already loaded included; sync_files; the poll): the parse is dropped so its sub-clips
// rebuild, but the entities showing it are not reloaded, so the images they show stay in the pools
// (they were invalidated with the parse: the baked Ball drew without its texture). A reload's
// MeshSystem::invalidateModelFile (an FBX reimport) still drops them afterwards.
TEST_F(AnimationClipAssets, AChangedModelKeepsTheImagesOfTheEntitiesShowingIt) {
    const std::string dir = folder("textured");
    const fs::path gltf = root() / dir / "tri.gltf";
    writeAll(gltf, texturedTriangleGltf());
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), gltf.string()));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    ASSERT_GE(mesh.numSubmeshes, 1u);
    const std::string id = mesh.submeshes[0].material.baseColorTexture.getId();
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(TextureDataPool::get(id)) << "the load pooled the image";
    const std::string key = MeshSystem::getModelFilenameKey(gltf.string());
    ASSERT_TRUE(ModelPool::getGLTF(key));

    AnimationClipDocument::filesChanged({gltf}, project());
    EXPECT_FALSE(ModelPool::getGLTF(key)) << "the parse is dropped: sub-clips are built again from the file";
    EXPECT_TRUE(TextureDataPool::get(id)) << "the image the Model shows stays";

    MeshSystem::invalidateModelFile(gltf.string());
    EXPECT_FALSE(TextureDataPool::get(id)) << "a reload's invalidation still finds and drops it without the parse";
}

// ---- review round 1 ----------------------------------------------------------------------

// A model and the clip previewing it moved together (one CopyFileCmd, either order): the clip's
// hint names the moved model, and the undo puts the clip's own bytes back
TEST_F(AnimationClipAssets, AModelAndItsClipMovedTogetherUndoInBothOrders) {
    for (bool modelFirst : {true, false}) {
        const std::string dir = folder(modelFirst ? "pairmove1" : "pairmove2");
        fs::create_directories(root() / dir / "a");
        fs::create_directories(root() / dir / "b");
        writeAll(root() / dir / "a" / "hero.gltf", "{\"asset\": {\"version\": \"2.0\"}}\n");
        ClipData data = positionClip();
        data.previewModel = dir + "/a/hero.gltf";
        std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/a/walk.anim", data);
        ASSERT_TRUE(document);
        const std::string before = readAll(document->absolutePath());

        std::vector<std::string> files = {(root() / dir / "a" / "hero.gltf").string(), (root() / dir / "a" / "walk.anim").string()};
        if (!modelFirst) std::swap(files[0], files[1]);
        ASSERT_TRUE(history()->addCommand(new CopyFileCmd(project(), files, (root() / dir / "b").string(), false)));
        const fs::path movedClip = root() / dir / "b" / "walk.anim";
        ASSERT_TRUE(fs::exists(movedClip));
        anim::ClipParseResult parsed = anim::parseClip(readAll(movedClip));
        ASSERT_TRUE(parsed.ok());
        EXPECT_EQ(parsed.data->previewModel, dir + "/b/hero.gltf") << (modelFirst ? "model first" : "clip first");
        EXPECT_EQ(document->data().previewModel, dir + "/b/hero.gltf") << "the open document followed and reloaded";

        ASSERT_TRUE(history()->undo());
        EXPECT_EQ(readAll(root() / dir / "a" / "walk.anim"), before) << (modelFirst ? "model first" : "clip first");
        EXPECT_TRUE(fs::exists(root() / dir / "a" / "hero.gltf"));
    }
}

// A clip edited outside the history after the rename keeps what it holds when the rename is undone
TEST_F(AnimationClipAssets, AHintEditedSinceTheRenameIsKept) {
    const std::string dir = folder("hintkept");
    writeAll(root() / dir / "hero.gltf", "{\"asset\": {\"version\": \"2.0\"}}\n");
    ClipData data = positionClip();
    data.previewModel = dir + "/hero.gltf";
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/walk.anim", data);
    ASSERT_TRUE(document);
    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "hero.gltf", "knight.gltf", (root() / dir).string())));
    ClipData outside = document->data();
    outside.previewModel = dir + "/other.gltf";
    writeAll(document->absolutePath(), anim::writeClip(outside));
    const std::string edited = readAll(document->absolutePath());
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(readAll(document->absolutePath()), edited) << "not overwritten by the undo";
    EXPECT_TRUE(fs::exists(root() / dir / "hero.gltf"));
}

// A broken write reported through filesChanged (auto-sync, sync_files) keeps the last valid clip
// for the players, as the poll and an open document do
TEST_F(AnimationClipAssets, ABrokenClipKeepsTheLastValidOneForThePlayers) {
    const std::string dir = folder("broken");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/idle.anim");
    ASSERT_TRUE(document);
    const fs::path file = document->absolutePath();
    const std::string guid = document->data().guid;
    PlayerScene scene = playerScene({entry(dir + "/idle.anim", guid)});
    std::string error;
    ASSERT_TRUE(AnimationClipPool::getOrLoad(file.string(), &error)) << error;
    const uint64_t generation = AnimationClipPool::getGeneration(file.string());

    for (bool documentOpen : {true, false}) {
        scene.player().needsLoad = false;
        writeAll(file, documentOpen ? "{ broken" : "{ broken again");
        if (!documentOpen) AnimationClipDocument::closeAll();
        AnimationClipDocument::filesChanged({file}, project());
        EXPECT_TRUE(AnimationClipPool::get(file.string())) << (documentOpen ? "document open" : "no document");
        EXPECT_EQ(AnimationClipPool::getGeneration(file.string()), generation);
        EXPECT_FALSE(scene.player().needsLoad);
    }
    // fixed again: the players reload it
    ClipData fixed = positionClip(4.0f);
    fixed.guid = guid;
    writeAll(file, anim::writeClip(fixed));
    AnimationClipDocument::filesChanged({file}, project());
    EXPECT_TRUE(scene.player().needsLoad);
    EXPECT_FALSE(AnimationClipPool::get(file.string()));
}

// A commit made before the poll saw an outside edit does not overwrite it; an outside change also
// ends a drag's merge, so undoing the next drag keeps the outside edit
TEST_F(AnimationClipAssets, ACommitNeverOverwritesAnUnseenOutsideEdit) {
    const std::string dir = folder("unseen");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/wave.anim");
    ASSERT_TRUE(document);
    const fs::path file = document->absolutePath();
    std::string error;
    ClipData data = document->data();
    data.tracks[0].keys.back().value[1] = 2.0f;
    ASSERT_TRUE(document->commit(data, "Drag", "drag#1", &error)) << error;
    const size_t steps = history()->position();

    // another program adds a track; nobody told the document yet
    ClipData outside = document->data();
    ClipTrackData second = outside.tracks[0];
    second.path = "Leg";
    outside.tracks.push_back(second);
    writeAll(file, anim::writeClip(outside) + "\n");
    std::error_code ec;
    fs::last_write_time(file, fs::last_write_time(file, ec) + std::chrono::seconds(2), ec);

    data.tracks[0].keys.back().value[1] = 3.0f;
    EXPECT_FALSE(document->commit(data, "Drag", "drag#1", &error));
    EXPECT_NE(error.find("changed on disk"), std::string::npos) << error;
    ASSERT_EQ(document->data().tracks.size(), 2u) << "reloaded";
    EXPECT_EQ(history()->position(), steps);

    // the edit made again on the new content: a step of its own (not merged into the drag)
    ClipData again = document->data();
    again.tracks[0].keys.back().value[1] = 3.0f;
    ASSERT_TRUE(document->commit(again, "Drag", "drag#1", &error)) << error;
    EXPECT_EQ(history()->position(), steps + 1);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(document->data().tracks.size(), 2u) << "the outside edit survives the undo";
    ASSERT_TRUE(history()->undo());
    EXPECT_FLOAT_EQ(lastKeyY(document->data()), 1.0f);
}

// The pose animation document has the same rule: a commit over an outside edit it has not seen
// yet is refused (the document reloads), the edit made again on the new content is written
TEST_F(AnimationClipAssets, APoseAnimationCommitNeverOverwritesAnUnseenOutsideEdit) {
    const std::string dir = folder("unseenpose");
    pose::PoseAnimationData initial;
    initial.poses.resize(2);
    initial.poses[0].name = "Rest";
    initial.poses[1].name = "Raise";
    std::string error;
    std::shared_ptr<PoseAnimationDocument> document =
        PoseAnimationDocument::create(project(), dir + "/wave.poseanim", initial, "Create", nullptr, &error);
    ASSERT_TRUE(document) << error;
    const fs::path file = document->absolutePath();
    pose::PoseAnimationData data = document->data();
    data.poses[1].hold = 0.5f;
    ASSERT_TRUE(document->commit(data, "Hold", "", nullptr, &error)) << error;
    const size_t steps = history()->position();

    // another program adds a pose; nobody told the document yet
    pose::PoseAnimationData outside = document->data();
    outside.poses.push_back(outside.poses[0]);
    outside.poses.back().name = "Wave";
    writeAll(file, pose::writePoseAnimation(outside));
    std::error_code ec;
    fs::last_write_time(file, fs::last_write_time(file, ec) + std::chrono::seconds(2), ec);

    data.poses[1].hold = 0.75f;
    EXPECT_FALSE(document->commit(data, "Hold", "", nullptr, &error));
    EXPECT_NE(error.find("changed on disk"), std::string::npos) << error;
    ASSERT_EQ(document->data().poses.size(), 3u) << "reloaded";
    EXPECT_EQ(readAll(file), pose::writePoseAnimation(outside)) << "the outside edit is not overwritten";
    EXPECT_EQ(history()->position(), steps);

    pose::PoseAnimationData again = document->data();
    again.poses[1].hold = 0.75f;
    ASSERT_TRUE(document->commit(again, "Hold", "", nullptr, &error)) << error;
    EXPECT_EQ(history()->position(), steps + 1);
    EXPECT_EQ(document->data().poses.size(), 3u);
    EXPECT_FLOAT_EQ(document->data().poses[1].hold, 0.75f);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(document->data().poses.size(), 3u) << "the outside edit survives the undo";
}

TEST_F(AnimationClipAssets, AFailedWriteReportsItsReason) {
    const std::string dir = folder("writefail");
    writeAll(root() / dir / "file.txt", "a file, not a folder");
    std::string error;
    EXPECT_FALSE(AnimationClipDocument::create(project(), dir + "/file.txt/clip.anim", positionClip(), "Create", &error));
    EXPECT_NE(error.find("writing"), std::string::npos) << error;
    // a later create at a good path works (the failed one left no document behind)
    EXPECT_TRUE(createClip(dir + "/clip.anim"));
}

// A saved scene opened again with nothing to repair is not modified; the play-startup form of the
// resolver never imports a file the database does not know
TEST_F(AnimationClipAssets, NothingToRepairLeavesTheSceneUnmodified) {
    const std::string dir = folder("unmodified");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/stay.anim");
    ASSERT_TRUE(document);
    PlayerScene scene = playerScene({entry(dir + "/stay.anim", document->data().guid)});
    const uint32_t sceneId = scene.sceneProject->id;
    const fs::path sceneFile = root() / dir / "Stay.scene";
    ASSERT_TRUE(project()->saveSceneToPath(sceneId, sceneFile));
    project()->getScene(sceneId)->isModified = false;
    project()->closeScene(sceneId, true);
    project()->openScene(sceneFile, false);
    SceneProject* opened = nullptr;
    for (SceneProject& candidate : project()->getScenes()) {
        if (candidate.scene && (root() / candidate.filepath).lexically_normal() == sceneFile.lexically_normal()) opened = &candidate;
    }
    ASSERT_TRUE(opened);
    EXPECT_FALSE(opened->isModified);

    // made outside, the database has not seen it: the read-only resolver leaves it alone
    ClipData outside = positionClip();
    outside.guid = anim::generateClipGuid();
    writeAll(root() / dir / "outside.anim", anim::writeClip(outside));
    Scene registry;
    const Entity entity = registry.createEntity();
    AnimationPlayerComponent stored;
    stored.clips = {entry(dir + "/outside.anim")};
    registry.addComponent<AnimationPlayerComponent>(entity, stored);
    EXPECT_FALSE(project()->resolveRegistryClipRefs(&registry, false));
    EXPECT_TRUE(registry.getComponent<AnimationPlayerComponent>(entity).clips[0].guid.empty());
    EXPECT_TRUE(project()->getAssetDatabase().getPath(outside.guid).empty()) << "not imported";
    // a path outside the project is never imported either
    AnimationPlayerClip far = entry((fs::temp_directory_path() / "far-away.anim").string());
    EXPECT_FALSE(project()->resolveClipReference(far));
    EXPECT_TRUE(far.guid.empty());
}

// Prefab assets: their players are clip users, follow an outside move in memory, and a clip
// rename never rewrites the prefab file (the GUID finds the clip when it is read)
TEST_F(AnimationClipAssets, PrefabAssetsUseClipsByGuid) {
    const std::string dir = folder("prefabasset");
    std::shared_ptr<AnimationClipDocument> document = createClip(dir + "/jog.anim");
    ASSERT_TRUE(document);
    const std::string guid = document->data().guid;
    PlayerScene scene = playerScene({entry(dir + "/jog.anim", guid)});
    const uint32_t sceneId = scene.sceneProject->id;
    const fs::path prefabFile = fs::path(dir) / "Runner.prefab";
    CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project(), sceneId, prefabFile,
        Stream::encodeEntity(scene.entity, scene.sceneProject->scene, project(), scene.sceneProject)));
    ASSERT_TRUE(project()->isPrefabInstanceRoot(sceneId, scene.entity));
    size_t prefabUsers = 0;
    for (const ClipUser& user : findClipUsers(project(), guid)) {
        if (user.prefabAsset) {
            prefabUsers++;
            EXPECT_EQ(user.prefabPath, prefabFile.generic_string());
        }
    }
    EXPECT_EQ(prefabUsers, 1u);

    const std::string prefabBytes = readAll(root() / prefabFile);
    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "jog.anim", "trot.anim", (root() / dir).string())));
    EXPECT_EQ(readAll(root() / prefabFile), prefabBytes) << "the prefab file is not rewritten";
    auto prefabEntry = [&]() {
        std::string clip;
        project()->forEachPrefabAsset([&](const fs::path& path, Prefab& prefab) {
            if (path.generic_string() != prefabFile.generic_string()) return;
            auto players = prefab.registry->getComponentArray<AnimationPlayerComponent>();
            if (players->size() > 0) clip = players->getComponentFromIndex(0).clips.at(0).clip;
        });
        return clip;
    };
    EXPECT_EQ(prefabEntry(), dir + "/trot.anim") << "in memory";
    ASSERT_TRUE(history()->undo());

    // moved outside the editor: the refresh relinks the prefab asset's player by GUID
    fs::create_directories(root() / dir / "moves");
    fs::rename(root() / dir / "jog.anim", root() / dir / "moves" / "jog.anim");
    project()->refreshAssetDatabase(true);
    EXPECT_EQ(prefabEntry(), dir + "/moves/jog.anim");
    EXPECT_EQ(readAll(root() / prefabFile), prefabBytes);
    scene.sceneProject->isModified = false;
}

// A clip rewritten outside with another clip's GUID (JSON copied from it) stays the same asset
TEST_F(AnimationClipAssets, AClipRewrittenWithAnotherClipsGuidKeepsItsOwn) {
    const std::string dir = folder("guidcopy");
    std::shared_ptr<AnimationClipDocument> first = createClip(dir + "/one.anim");
    std::shared_ptr<AnimationClipDocument> second = createClip(dir + "/two.anim");
    ASSERT_TRUE(first && second);
    const std::string secondGuid = second->data().guid;
    writeAll(second->absolutePath(), readAll(first->absolutePath()) + "\n");
    project()->refreshAssetDatabase(true);
    EXPECT_EQ(project()->getAssetDatabase().getPath(secondGuid), fs::path(dir + "/two.anim"));
    EXPECT_EQ(AssetDatabase::readGuid(second->absolutePath()), secondGuid);
    EXPECT_EQ(project()->getAssetDatabase().getPath(first->data().guid), fs::path(dir + "/one.anim"));
}

// Sub-clip users by name and by index are the same animation
TEST_F(AnimationClipAssets, SubClipUsersByNameAndIndex) {
    const std::string dir = folder("subusers");
    const fs::path fixture = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" /
                             "assets" / "models" / "hero_animated.gltf";
    fs::copy_file(fixture, root() / dir / "hero.gltf", fs::copy_options::overwrite_existing);
    std::vector<anim::ModelClipInfo> clips;
    std::string error;
    ASSERT_TRUE(anim::listModelFileClips(project()->normalizeToAssetsRelative(root() / dir / "hero.gltf").generic_string(), clips, &error)) << error;
    ASSERT_FALSE(clips.empty());
    const std::string model = dir + "/hero.gltf";
    PlayerScene scene = playerScene({entry(model + "#" + clips[0].name), entry(model + "#" + std::to_string(clips[0].index))});
    EXPECT_EQ(findClipUsers(project(), model + "#" + clips[0].name).size(), 2u);
    EXPECT_EQ(findClipUsers(project(), model + "#" + std::to_string(clips[0].index)).size(), 2u);
    scene.sceneProject->isModified = false;
}

// ---- review round 2 ----------------------------------------------------------------------

// A database opened read-only (CLI check, headless analysis) gives GUIDs in memory only: no clip,
// no prefab and no cache is written. In either mode a clip that does not parse is never touched.
TEST_F(AnimationClipAssets, AReadOnlyDatabaseWritesNoFile) {
    const fs::path dir = fs::temp_directory_path() / ("jasper-clip-readonly-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const std::string clip = anim::writeClip(positionClip());   // no "guid"
    const std::string broken = "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  oops\n}\n";
    const std::string prefab = "type: Prefab\nversion: 3\nroot: 1\nmembers: []\n";   // no guid either
    writeAll(dir / "clips" / "walk.anim", clip);
    writeAll(dir / "clips" / "broken.anim", broken);
    writeAll(dir / "Thing.prefab", prefab);

    AssetDatabase readOnly;
    readOnly.setReadOnly(true);
    readOnly.open(dir);
    EXPECT_TRUE(anim::isClipGuid(readOnly.getGuid(dir / "clips" / "walk.anim"))) << "a GUID in memory";
    EXPECT_FALSE(readOnly.getGuid(dir / "Thing.prefab").empty());
    writeAll(dir / "clips" / "new.anim", clip);
    readOnly.refresh();
    EXPECT_FALSE(readOnly.getGuid(dir / "clips" / "new.anim").empty());
    EXPECT_EQ(readAll(dir / "clips" / "walk.anim"), clip);
    EXPECT_EQ(readAll(dir / "clips" / "new.anim"), clip);
    EXPECT_EQ(readAll(dir / "clips" / "broken.anim"), broken);
    EXPECT_EQ(readAll(dir / "Thing.prefab"), prefab);
    EXPECT_FALSE(fs::exists(dir / ".jasperengine" / "assetdb")) << "no cache written";
    readOnly.close();

    // a normal database upgrades the good clip, never the broken one (its error keeps its line)
    AssetDatabase writable;
    writable.open(dir);
    EXPECT_EQ(anim::readClipGuid(readAll(dir / "clips" / "walk.anim")), writable.getGuid(dir / "clips" / "walk.anim"));
    EXPECT_EQ(readAll(dir / "clips" / "broken.anim"), broken);
    EXPECT_FALSE(AssetDatabase::writeGuid(dir / "clips" / "broken.anim", AssetDatabase::generateGuid()));
    EXPECT_EQ(readAll(dir / "clips" / "broken.anim"), broken);
    writable.close();
    fs::remove_all(dir);
}
