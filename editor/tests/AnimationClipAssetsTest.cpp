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

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHistory.h"
#include "command/type/CopyFileCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/RenameFileCmd.h"
#include "pool/AnimationClipPool.h"
#include "util/AssetDatabase.h"
#include "util/Util.h"

#include <unistd.h>

#include <cstdlib>
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
        std::unique_ptr<editor::Platform> platform;
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
            out.platform.reset(new editor::Platform(out.project.get()));
            static char arg0[] = "jasperengine-editor-tests";
            static char* argv[] = {arg0, nullptr};
            Engine::systemInit(1, argv, out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
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

    // the first fixture model with animations, seen inside the project through a link (a .gltf
    // keeps its .bin next to it)
    std::error_code ec;
    const fs::path fixtures = fs::weakly_canonical(fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "tools" / "fixtures", ec);
    fs::create_directory_symlink(fixtures, root() / dir / "fixtures", ec);
    std::string model;
    std::vector<anim::ModelClipInfo> clips;
    for (fs::recursive_directory_iterator it(fixtures, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path candidate = it->path();
        if (!it->is_regular_file(ec) || !Util::isModelFile(candidate.string())) continue;
        const std::string inProject = dir + "/fixtures/" + candidate.lexically_relative(fixtures).generic_string();
        std::vector<anim::ModelClipInfo> found;
        std::string listError;
        if (anim::listModelFileClips(project()->normalizeToAssetsRelative(root() / inProject).generic_string(), found, &listError) &&
            !found.empty()) {
            model = inProject;
            clips = found;
            break;
        }
        error = listError;
    }
    if (model.empty()) {
        // the sub-clip builder (T1c) is not there yet, or no fixture has an animation
        fs::remove(root() / dir / "fixtures", ec);
        GTEST_SKIP() << "no model sub-clips to open: " << error;
    }
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project(), model + "#" + clips[0].name, &error);
    ASSERT_TRUE(document) << error;
    EXPECT_TRUE(document->isReadOnly());
    EXPECT_EQ(document->path(), model + "#" + clips[0].name);
    EXPECT_FALSE(document->commit(document->data(), "Edit", "", &error));
    EXPECT_NE(error.find("read-only"), std::string::npos) << error;
    // by index: the same document (the canonical form is the name)
    EXPECT_EQ(AnimationClipDocument::open(project(), model + "#" + std::to_string(clips[0].index), &error), document);
    fs::remove(root() / dir / "fixtures", ec);
}

// ---- the asset database ------------------------------------------------------------------

TEST_F(AnimationClipAssets, AssetDatabaseReadsAndWritesTheJsonGuid) {
    const std::string dir = folder("database");
    EXPECT_TRUE(AssetDatabase::isTrackedFile("a/b/walk.anim"));
    EXPECT_TRUE(AssetDatabase::isTrackedFile("a/b/WALK.ANIM"));
    EXPECT_FALSE(AssetDatabase::isTrackedFile("a/hero.gltf#Walk"));

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

    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "run.anim", "sprint.anim", (root() / dir).string())));
    const fs::path renamed = root() / dir / "sprint.anim";
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/sprint.anim");
    EXPECT_EQ(scene.player().clips[0].guid, guid);
    EXPECT_TRUE(scene.player().needsLoad);
    EXPECT_TRUE(scene.sceneProject->isModified);
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/sprint.anim"));
    EXPECT_EQ(anim::readClipGuid(readAll(renamed)), guid) << "the file keeps its GUID";
    EXPECT_FALSE(document->exists()) << "the document of the old path is Removed";
    std::string error;
    std::shared_ptr<AnimationClipDocument> moved = AnimationClipDocument::open(project(), dir + "/sprint.anim", &error);
    ASSERT_TRUE(moved) << error;
    EXPECT_EQ(moved->data().guid, guid);

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(scene.player().clips[0].clip, dir + "/run.anim");
    EXPECT_EQ(project()->getAssetDatabase().getPath(guid), fs::path(dir + "/run.anim"));
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
