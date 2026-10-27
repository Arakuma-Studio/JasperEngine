// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Vertex Animation assets, editor side (vertex animation spec 2.1, 3.3, 5.2-5.4): the .vat
// document (commit, undo/redo on the project history, merge keys, generated-field refusal, outside
// changes, broken files, the pool invalidated on every revision), AssetDatabase GUIDs in the .vat
// JSON, the .vat + .vat.glb pair through rename / move / copy / delete (the "mesh" field rewritten,
// open scenes follow, a closed scene and a prefab follow by GUID, an undone delete relinks the
// Model too), the Project window's file helpers, the GameObject a dropped .vat makes, Create Crowd
// (layout determinism, spacing, counts, weights, validation, one undo step) and the Inspector's
// pure helpers. Runs a headless project, as AnimationClipAssetsTest does.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "animation/AnimationClipIO.h"
#include "animation/VertexAnimation.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CopyFileCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/RenameFileCmd.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/VertexAnimationPool.h"
#include "util/AssetDatabase.h"
#include "vat/VatAssetFiles.h"
#include "vat/VatAssetWriter.h"
#include "vat/VatCrowd.h"
#include "vat/VatDocument.h"
#include "vat/VatSceneActions.h"
#include "window/InspectorWindowVat.h"

#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace jv = jasperengine::vat;
namespace ev = jasperengine::editor::vat;

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
            const fs::path root = fs::temp_directory_path() / ("jasper-vat-editor-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: vateditor\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            static char arg0[] = "jasperengine-editor-tests";
            static char* argv[] = {arg0, nullptr};
            Engine::systemInit(1, argv, out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            // the project's settings turn it on again: models load synchronously here
            Engine::setAsyncLoading(false);
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
        std::error_code ec;
        const bool existed = fs::exists(path, ec);
        const fs::file_time_type before = existed ? fs::last_write_time(path, ec) : fs::file_time_type();
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << content;
        }
        // an outside edit of the same size within the clock's tick must still look changed
        if (existed && fs::last_write_time(path, ec) <= before) {
            fs::last_write_time(path, before + std::chrono::milliseconds(10), ec);
        }
    }

    void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    }

    std::string folder(const std::string& name) {
        fs::remove_all(root() / name);
        fs::create_directories(root() / name);
        return name;
    }

    // A baked quad (4 vertices): Walk (frames 0-1) and Idle (frames 2-3), one socket
    struct Pair {
        fs::path vat;
        fs::path mesh;
        std::string relative;       // the .vat, asset-relative (= project-relative here)
        std::string meshRelative;
        std::string guid;
    };

    jv::VatData quadData(const std::string& guid) {
        jv::VatData data;
        data.guid = guid;
        data.mode = jv::VatMode::Vertices;
        data.layout.precision = jv::VatPrecision::Half;
        data.layout.width = 8;
        data.layout.height = 4;
        data.layout.samples = 4;
        data.layout.vertices = 4;
        data.layout.normals = true;
        jv::VatClip walk;
        walk.name = "Walk";
        walk.start = 0;
        walk.frames = 2;
        walk.fps = 2.0f;
        walk.bounds.extend(Vector3(0, 0, 0));
        walk.bounds.extend(Vector3(1, 1, 0));
        ClipEventData step;
        step.time = 0.25f;
        step.function = "onStep";
        step.args = {AnimationEvent::stringArg("left")};
        walk.events.push_back(step);
        jv::VatClip idle = walk;
        idle.name = "Idle";
        idle.start = 2;
        idle.events.clear();
        data.clips = {walk, idle};
        data.bounds.extend(walk.bounds);
        data.sockets.push_back({"Tip", "Quad/Tip"});
        data.recipe.sockets = {"Quad/Tip"};
        jv::VatRecipeSource source;
        source.kind = "model";
        source.path = "models/quad.gltf";
        source.clips = {"Walk", "Idle"};
        data.recipe.sources.push_back(source);
        data.stats.maxErrorMm = 0.25f;
        data.stats.meanErrorMm = 0.05f;
        return data;
    }

    // the files of the quad bake at `vatPath`
    ev::VatEncodedFiles encodeQuad(const fs::path& vatPath, const std::string& guid) {
        jv::VatData data = quadData(guid);
        ev::VatMeshData mesh;
        mesh.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        mesh.uv0 = {0, 0, 1, 0, 1, 1, 0, 1};
        mesh.vatIndex = {0, 1, 2, 3};
        mesh.submeshes.push_back({{0, 1, 2, 0, 2, 3}, -1});
        jv::VatPayload payload;
        payload.precision = jv::VatPrecision::Half;
        payload.width = 8;
        payload.height = 4;
        payload.bytes.assign(8 * 4 * 8, 0);
        jv::VatSocketTracks sockets;
        sockets.sockets = 1;
        sockets.frames = 4;
        for (int f = 0; f < 4; f++) {
            const float row[7] = {(float)f, 0, 0, 0, 0, 0, 1};
            sockets.data.insert(sockets.data.end(), row, row + 7);
        }
        ev::VatEncodedFiles files;
        std::string error;
        EXPECT_TRUE(ev::encodeVatFiles(vatPath.generic_string(), data, mesh, payload, sockets, files, &error)) << error;
        return files;
    }

    Pair writePair(const std::string& dir, const std::string& stem, const std::string& guid = "") {
        Pair pair;
        pair.vat = (root() / dir / (stem + ".vat")).lexically_normal();
        pair.mesh = (root() / dir / (stem + ".vat.glb")).lexically_normal();
        pair.relative = dir + "/" + stem + ".vat";
        pair.meshRelative = dir + "/" + stem + ".vat.glb";
        pair.guid = guid.empty() ? AssetDatabase::generateGuid() : guid;
        const ev::VatEncodedFiles files = encodeQuad(pair.vat, pair.guid);
        writeAll(pair.vat, files.vatText);
        writeBytes(pair.mesh, files.glbBytes);
        project()->importPrefabFiles(pair.vat);
        return pair;
    }

    // A scene with one GameObject playing `pair`: Model = its mesh, Vertex Animator = the .vat
    struct VatScene {
        SceneProject* sceneProject = nullptr;
        Entity entity = NULL_ENTITY;

        VertexAnimatorComponent& animator() const { return sceneProject->scene->getComponent<VertexAnimatorComponent>(entity); }
        ModelComponent& model() const { return sceneProject->scene->getComponent<ModelComponent>(entity); }
    };

    VatScene vatScene(const Pair& pair, const std::string& name = "Vat") {
        VatScene out;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene(name, SceneType::SCENE_3D);
        out.sceneProject = p->getScene(p->getSelectedSceneId());
        Scene* scene = out.sceneProject->scene;
        out.entity = scene->createEntity();
        scene->addComponent<Transform>(out.entity);
        out.sceneProject->entities.push_back(out.entity);
        scene->addComponent<MeshComponent>(out.entity, {});
        ModelComponent model;
        model.filename = pair.meshRelative;
        model.needUpdateModel = false;
        scene->addComponent<ModelComponent>(out.entity, model);
        VertexAnimatorComponent animator;
        animator.animation = pair.relative;
        animator.guid = pair.guid;
        scene->addComponent<VertexAnimatorComponent>(out.entity, animator);
        out.sceneProject->isModified = false;
        return out;
    }

    std::shared_ptr<VatDocument> openDocument(const std::string& path) {
        std::string error;
        std::shared_ptr<VatDocument> document = VatDocument::open(project(), path, &error);
        EXPECT_TRUE(document) << error;
        return document;
    }

    class VatEditor : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready) << "the headless project did not open";
        }
    };

}

// The MCP UI test (tools/mcp/tests/test_vat_inspector.py) plays editor/tests/fixtures/vat/quad.vat:
// exactly what the writer makes of the quad bake. JASPER_WRITE_VAT_FIXTURE=<dir> writes it there.
TEST(VatEditorFixture, TheCommittedQuadIsWhatTheWriterMakes) {
    const fs::path fixture = fs::path(JASPER_EDITOR_TEST_FIXTURES) / "vat";
    const ev::VatEncodedFiles files = encodeQuad(fixture / "quad.vat", "5a7c0de0b1a5e5c0ffee00112233aabb");
    if (const char* out = std::getenv("JASPER_WRITE_VAT_FIXTURE")) {
        writeAll(fs::path(out) / "quad.vat", files.vatText);
        writeBytes(fs::path(out) / "quad.vat.glb", files.glbBytes);
    }
    EXPECT_EQ(readAll(fixture / "quad.vat"), files.vatText);
    EXPECT_EQ(readAll(fixture / "quad.vat.glb"), std::string(files.glbBytes.begin(), files.glbBytes.end()));
}

// ---- the document -------------------------------------------------------------------------

TEST_F(VatEditor, DocumentCommitUndoRedoAndThePool) {
    const std::string dir = folder("doc");
    const Pair pair = writePair(dir, "hero");
    std::shared_ptr<VatDocument> document = openDocument(pair.relative);
    ASSERT_TRUE(document);
    EXPECT_EQ(document->path(), pair.relative);
    EXPECT_EQ(document->absolutePath(), pair.vat);
    EXPECT_EQ(document->assetPath(), pair.relative);
    // shared per path, in every form, and the mesh names its .vat
    std::string error;
    EXPECT_EQ(VatDocument::open(project(), pair.vat.string(), &error), document) << error;
    EXPECT_EQ(VatDocument::open(project(), pair.meshRelative, &error), document) << error;
    EXPECT_FALSE(VatDocument::open(project(), "../outside.vat", &error));
    EXPECT_FALSE(VatDocument::open(project(), dir + "/hero.anim", &error));

    std::vector<VatDocument::Change> changes;
    const int listener = document->addListener([&](VatDocument&, VatDocument::Change change) { changes.push_back(change); });
    const std::string original = readAll(pair.vat);
    const uint64_t generation = VertexAnimationPool::getGeneration(pair.vat.generic_string());
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(pair.vat.generic_string(), &error)) << error;
    const uint64_t loaded = VertexAnimationPool::getGeneration(pair.vat.generic_string());
    EXPECT_NE(loaded, generation);

    // an editable change: one project step, canonical text, the GUID kept, the pool dropped
    jv::VatData edited = document->data();
    edited.clips[0].name = "Stroll";
    edited.clips[0].wrap = ClipWrapMode::PingPong;
    edited.clips[0].speed = 1.5f;
    edited.clips[1].interpolate = false;
    edited.sockets[0].name = "Hand";
    edited.guid = AssetDatabase::generateGuid();
    const size_t steps = history()->position();
    ASSERT_TRUE(document->commit(edited, "Edit", "", &error)) << error;
    EXPECT_EQ(history()->position(), steps + 1);
    EXPECT_EQ(document->data().clips[0].name, "Stroll");
    EXPECT_EQ(document->data().guid, pair.guid) << "the file keeps its GUID";
    EXPECT_EQ(readAll(pair.vat), jv::writeVat(document->data()));
    EXPECT_EQ(anim::readClipGuid(readAll(pair.vat)), pair.guid);
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes.back(), VatDocument::Change::Edited);
    EXPECT_FALSE(VertexAnimationPool::get(pair.vat.generic_string())) << "playing animators reload the edit";
    EXPECT_GT(VertexAnimationPool::getGeneration(pair.vat.generic_string()), loaded);
    // the edited file still loads (the payload is the same)
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(pair.vat.generic_string(), &error)) << error;
    EXPECT_EQ(VertexAnimationPool::get(pair.vat.generic_string())->getData().clips[0].name, "Stroll");

    // nothing changed: nothing written
    ASSERT_TRUE(document->commit(document->data(), "Nothing", "", &error)) << error;
    EXPECT_EQ(history()->position(), steps + 1);

    // undo, redo reload it
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(readAll(pair.vat), original);
    EXPECT_EQ(document->data().clips[0].name, "Walk");
    EXPECT_EQ(changes.back(), VatDocument::Change::Reloaded);
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(document->data().clips[0].name, "Stroll");
    ASSERT_TRUE(history()->undo());
    document->removeListener(listener);
}

TEST_F(VatEditor, DocumentMergeKeysMakeOneStepPerDrag) {
    const std::string dir = folder("merge");
    const Pair pair = writePair(dir, "hero");
    std::shared_ptr<VatDocument> document = openDocument(pair.relative);
    ASSERT_TRUE(document);
    const size_t steps = history()->position();
    std::string error;
    jv::VatData data = document->data();
    for (float speed : {1.1f, 1.2f, 1.3f}) {
        data.clips[0].speed = speed;
        ASSERT_TRUE(document->commit(data, "Clip Speed", "speed#0#1", &error)) << error;
    }
    EXPECT_EQ(history()->position(), steps + 1) << "one drag, one step";
    data.clips[0].speed = 2.0f;
    ASSERT_TRUE(document->commit(data, "Clip Speed", "speed#0#2", &error)) << error;
    EXPECT_EQ(history()->position(), steps + 2) << "a new drag, a new step";
    ASSERT_TRUE(history()->undo());
    EXPECT_FLOAT_EQ(document->data().clips[0].speed, 1.3f);
    ASSERT_TRUE(history()->undo());
    EXPECT_FLOAT_EQ(document->data().clips[0].speed, 1.0f) << "the first drag undone at once";
}

TEST_F(VatEditor, DocumentRefusesGeneratedFieldsAndBadNames) {
    const std::string dir = folder("refuse");
    const Pair pair = writePair(dir, "hero");
    std::shared_ptr<VatDocument> document = openDocument(pair.relative);
    ASSERT_TRUE(document);
    const size_t steps = history()->position();
    std::string error;
    auto refused = [&](const std::function<void(jv::VatData&)>& edit, const std::string& message) {
        jv::VatData data = document->data();
        edit(data);
        error.clear();
        EXPECT_FALSE(document->commit(data, "Edit", "", &error)) << message;
        EXPECT_NE(error.find(message), std::string::npos) << error;
    };
    refused([](jv::VatData& d) { d.mode = jv::VatMode::Bones; }, "generated field mode");
    refused([](jv::VatData& d) { d.mesh = "other.vat.glb"; }, "generated field mesh");
    refused([](jv::VatData& d) { d.layout.width = 16; }, "generated field layout");
    refused([](jv::VatData& d) { d.clips[0].frames = 3; }, "generated field clips[0].frames");
    refused([](jv::VatData& d) { d.clips[1].fps = 5.0f; }, "generated field clips[1].fps");
    refused([](jv::VatData& d) { d.clips.push_back(d.clips[0]); d.clips.back().name = "Copy"; }, "a clip the bake did not make");
    refused([](jv::VatData& d) { d.sockets.clear(); }, "sockets (count)");
    refused([](jv::VatData& d) { d.recipe.fps = 60.0f; }, "generated field recipe");
    refused([](jv::VatData& d) { d.stats.bytes = 1; }, "generated field stats");
    refused([](jv::VatData& d) { d.clips[1].name = "Walk"; }, "two clips are named 'Walk'");
    refused([](jv::VatData& d) { d.clips[0].name = ""; }, "a clip needs a name");
    refused([](jv::VatData& d) { d.sockets[0].name = ""; }, "a socket needs a name");
    refused([](jv::VatData& d) { d.clips[0].events[0].function = ""; }, "an event calls a function");
    EXPECT_EQ(history()->position(), steps) << "nothing written";

    // removing a whole clip is an edit: its rows stay until a rebake (and it still loads)
    jv::VatData data = document->data();
    data.clips.erase(data.clips.begin());
    ASSERT_TRUE(document->commit(data, "Remove Clip", "", &error)) << error;
    EXPECT_EQ(VatDocument::unusedSamples(document->data()), 2);
    EXPECT_NE(inspectorvat::removedClipsNote(document->data()).find("Removed clips keep their rows until the next Rebake (2 unused frames"),
              std::string::npos) << inspectorvat::removedClipsNote(document->data());
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(pair.vat.generic_string(), &error)) << error;
    // events are kept sorted
    data = document->data();
    ClipEventData late;
    late.time = 0.9f;
    late.function = "onLate";
    ClipEventData early;
    early.time = 0.1f;
    early.function = "onEarly";
    data.clips[0].events = {late, early};
    ASSERT_TRUE(document->commit(data, "Events", "", &error)) << error;
    ASSERT_EQ(document->data().clips[0].events.size(), 2u);
    EXPECT_EQ(document->data().clips[0].events[0].function, "onEarly");
    ASSERT_TRUE(history()->undo());
    ASSERT_TRUE(history()->undo());
}

TEST_F(VatEditor, DocumentReloadsOutsideChangesAndKeepsTheLastValidOne) {
    const std::string dir = folder("outside");
    const Pair pair = writePair(dir, "hero");
    std::shared_ptr<VatDocument> document = openDocument(pair.relative);
    ASSERT_TRUE(document);
    std::string error;
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(pair.vat.generic_string(), &error)) << error;

    // edited outside the editor: the poll reloads it and drops the pool's copy
    jv::VatData data = document->data();
    data.clips[1].name = "Rest";
    writeAll(pair.vat, jv::writeVat(data));
    VatDocument::checkDiskChanges(project());
    EXPECT_EQ(document->data().clips[1].name, "Rest");
    EXPECT_FALSE(VertexAnimationPool::get(pair.vat.generic_string()));

    // a commit made on old content is refused (the file changed since it was read)
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(pair.vat.generic_string(), &error)) << error;
    jv::VatData stale = document->data();
    data.clips[1].name = "Sleep";
    writeAll(pair.vat, jv::writeVat(data) + "\n");
    stale.clips[0].speed = 3.0f;
    EXPECT_FALSE(document->commit(stale, "Edit", "", &error));
    EXPECT_NE(error.find("changed on disk"), std::string::npos) << error;
    EXPECT_EQ(document->data().clips[1].name, "Sleep");

    // broken JSON: the last valid content stays (and the pool keeps the asset), with a warning
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(pair.vat.generic_string(), &error)) << error;
    writeAll(pair.vat, "{ \"format\": \"jasperengine.vat\", ");
    VatDocument::filesChanged({pair.vat}, project());
    EXPECT_EQ(document->data().clips[1].name, "Sleep");
    ASSERT_FALSE(document->loadWarnings().empty());
    EXPECT_NE(document->loadWarnings()[0].message.find("not a valid Vertex Animation"), std::string::npos);
    EXPECT_TRUE(VertexAnimationPool::get(pair.vat.generic_string())) << "animators keep the last valid asset";

    // a .vat no document has open: the pool hears of it all the same (a new mesh too)
    const Pair other = writePair(dir, "other");
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(other.vat.generic_string(), &error)) << error;
    VatDocument::filesChanged({other.mesh}, project());
    EXPECT_FALSE(VertexAnimationPool::get(other.vat.generic_string()));

    // deleted: Removed
    fs::remove(pair.vat);
    VatDocument::filesChanged({pair.vat}, project());
    EXPECT_FALSE(document->exists());
    EXPECT_FALSE(document->commit(document->data(), "Edit", "", &error));
}

TEST_F(VatEditor, ThePollWatchesTheAnimatorsFiles) {
    const std::string dir = folder("poll");
    const Pair pair = writePair(dir, "hero");
    VatScene scene = vatScene(pair);
    std::string error;
    VatDocument::checkDiskChanges(project());     // first look: stamps taken
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(pair.vat.generic_string(), &error)) << error;
    scene.animator().needsLoad = false;
    // a rebake rewrites the mesh: the asset reloads
    const std::string glb = readAll(pair.mesh);
    writeAll(pair.mesh, glb + std::string(4, '\0'));
    fs::last_write_time(pair.mesh, fs::last_write_time(pair.mesh) + std::chrono::seconds(2));
    VatDocument::checkDiskChanges(project());
    EXPECT_FALSE(VertexAnimationPool::get(pair.vat.generic_string()));
    EXPECT_TRUE(scene.animator().needsLoad);
}

// ---- AssetDatabase ----------------------------------------------------------------------

TEST_F(VatEditor, AssetDatabaseTracksVatGuidsInTheJson) {
    const std::string dir = folder("guids");
    EXPECT_TRUE(AssetDatabase::isTrackedFile("a/hero.vat"));
    EXPECT_FALSE(AssetDatabase::isTrackedFile("a/hero.vat.glb"));
    const Pair pair = writePair(dir, "hero");
    AssetDatabase& database = project()->getAssetDatabase();
    EXPECT_EQ(database.getGuid(pair.vat), pair.guid);
    EXPECT_EQ(database.getPath(pair.guid), fs::path(pair.relative));
    EXPECT_EQ(AssetDatabase::readGuid(pair.vat), pair.guid);

    // written without a GUID (by hand): the database gives it one, in the JSON
    jv::VatData data = jv::parseVat(readAll(pair.vat)).data.value();
    const std::string without = dir + "/bare.vat";
    std::string text = jv::writeVat(data);
    const size_t at = text.find("\"guid\"");
    ASSERT_NE(at, std::string::npos);
    text.erase(at, text.find('\n', at) - at + 1);
    writeAll(root() / without, text);
    project()->refreshAssetDatabase(true);
    const std::string given = database.getGuid(root() / without);
    EXPECT_TRUE(AssetDatabase::isGuid(given));
    EXPECT_NE(given, pair.guid);
    EXPECT_EQ(AssetDatabase::readGuid(root() / without), given) << "written into the file";
    EXPECT_TRUE(jv::parseVat(readAll(root() / without)).ok()) << "still a valid .vat";

    // a broken .vat is left alone (its error keeps its line)
    writeAll(root() / dir / "broken.vat", "{ not json");
    project()->refreshAssetDatabase(true);
    EXPECT_EQ(readAll(root() / dir / "broken.vat"), "{ not json");
}

// ---- the pair through rename, move, copy, delete -------------------------------------------

TEST_F(VatEditor, RenameTakesThePairAndRewritesTheMeshField) {
    const std::string dir = folder("rename");
    const Pair pair = writePair(dir, "hero");
    VatScene scene = vatScene(pair);
    std::shared_ptr<VatDocument> document = openDocument(pair.relative);
    ASSERT_TRUE(document);
    project()->setInspectedAsset(pair.vat);

    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "hero.vat", "crowd.vat", (root() / dir).string())));
    const fs::path vat = root() / dir / "crowd.vat";
    const fs::path mesh = root() / dir / "crowd.vat.glb";
    EXPECT_TRUE(fs::exists(vat));
    EXPECT_TRUE(fs::exists(mesh)) << "the mesh went with it";
    EXPECT_FALSE(fs::exists(pair.vat));
    EXPECT_FALSE(fs::exists(pair.mesh));
    const jv::VatParseResult parsed = jv::parseVat(readAll(vat));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->mesh, "crowd.vat.glb");
    EXPECT_EQ(parsed.data->guid, pair.guid);
    EXPECT_EQ(project()->getAssetDatabase().getPath(pair.guid), fs::path(dir + "/crowd.vat"));
    // the open scene follows: the animator by GUID, the Model by path
    EXPECT_EQ(scene.animator().animation, dir + "/crowd.vat");
    EXPECT_EQ(scene.animator().guid, pair.guid);
    EXPECT_EQ(scene.model().filename, dir + "/crowd.vat.glb");
    EXPECT_TRUE(scene.sceneProject->isModified);
    // the document and the Inspector follow
    EXPECT_EQ(document->path(), dir + "/crowd.vat");
    EXPECT_EQ(document->data().mesh, "crowd.vat.glb");
    EXPECT_EQ(project()->getInspectedAsset(), vat.lexically_normal());
    std::string error;
    EXPECT_TRUE(VertexAnimationPool::getOrLoad(vat.generic_string(), &error)) << error;

    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(fs::exists(pair.vat));
    EXPECT_TRUE(fs::exists(pair.mesh));
    EXPECT_FALSE(fs::exists(vat));
    EXPECT_FALSE(fs::exists(mesh));
    EXPECT_EQ(jv::parseVat(readAll(pair.vat)).data->mesh, "hero.vat.glb");
    EXPECT_EQ(scene.animator().animation, pair.relative);
    EXPECT_EQ(scene.model().filename, pair.meshRelative);
    EXPECT_EQ(document->path(), pair.relative);

    // renaming the generated mesh renames the pair too
    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "hero.vat.glb", "walker.vat.glb", (root() / dir).string())));
    EXPECT_TRUE(fs::exists(root() / dir / "walker.vat"));
    EXPECT_TRUE(fs::exists(root() / dir / "walker.vat.glb"));
    EXPECT_EQ(jv::parseVat(readAll(root() / dir / "walker.vat")).data->mesh, "walker.vat.glb");
    EXPECT_EQ(scene.animator().animation, dir + "/walker.vat");
    EXPECT_EQ(scene.model().filename, dir + "/walker.vat.glb");
    ASSERT_TRUE(history()->undo());

    // a rename onto an existing pair file is refused, nothing moves
    writePair(dir, "taken");
    fs::remove(root() / dir / "taken.vat");
    EXPECT_FALSE(history()->addCommand(new RenameFileCmd(project(), "hero.vat", "taken.vat", (root() / dir).string())));
    EXPECT_TRUE(fs::exists(pair.vat));
    EXPECT_TRUE(fs::exists(pair.mesh));
}

TEST_F(VatEditor, AClosedSceneAndAPrefabFollowByGuid) {
    const std::string dir = folder("closed");
    const Pair pair = writePair(dir, "hero");
    // what a closed scene's file holds: the old paths and the GUID
    Scene closed;
    const Entity entity = closed.createEntity();
    closed.addComponent<Transform>(entity, {});
    closed.addComponent<MeshComponent>(entity, {});
    ModelComponent model;
    model.filename = pair.meshRelative;
    closed.addComponent<ModelComponent>(entity, model);
    VertexAnimatorComponent animator;
    animator.animation = pair.relative;
    animator.guid = pair.guid;
    closed.addComponent<VertexAnimatorComponent>(entity, animator);

    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "hero.vat", "knight.vat", (root() / dir).string())));
    EXPECT_EQ(closed.getComponent<VertexAnimatorComponent>(entity).animation, pair.relative) << "a closed scene is not rewritten";
    // opened: the reference resolves by GUID and the Model follows its .vat
    closed.getComponent<ModelComponent>(entity).needUpdateModel = false;
    EXPECT_TRUE(project()->resolveRegistryClipRefs(&closed));
    EXPECT_EQ(closed.getComponent<VertexAnimatorComponent>(entity).animation, dir + "/knight.vat");
    EXPECT_EQ(closed.getComponent<ModelComponent>(entity).filename, dir + "/knight.vat.glb");
    EXPECT_TRUE(closed.getComponent<ModelComponent>(entity).needUpdateModel);
    EXPECT_FALSE(project()->resolveRegistryClipRefs(&closed)) << "resolved: nothing changes the second time";

    // a Model that plays another mesh is not touched
    closed.getComponent<VertexAnimatorComponent>(entity).animation = pair.relative;
    closed.getComponent<ModelComponent>(entity).filename = "models/other.glb";
    EXPECT_TRUE(project()->resolveRegistryClipRefs(&closed));
    EXPECT_EQ(closed.getComponent<ModelComponent>(entity).filename, "models/other.glb");
    ASSERT_TRUE(history()->undo());
}

TEST_F(VatEditor, MoveAndCopyTakeThePair) {
    const std::string dir = folder("move");
    const Pair pair = writePair(dir, "hero");
    VatScene scene = vatScene(pair);
    fs::create_directories(root() / dir / "sub");

    // moving the .vat alone moves both
    ASSERT_TRUE(history()->addCommand(new CopyFileCmd(project(), {pair.vat.string()}, (root() / dir / "sub").string(), false)));
    EXPECT_TRUE(fs::exists(root() / dir / "sub" / "hero.vat"));
    EXPECT_TRUE(fs::exists(root() / dir / "sub" / "hero.vat.glb"));
    EXPECT_FALSE(fs::exists(pair.mesh));
    EXPECT_EQ(scene.animator().animation, dir + "/sub/hero.vat");
    EXPECT_EQ(scene.model().filename, dir + "/sub/hero.vat.glb");
    EXPECT_EQ(project()->getAssetDatabase().getPath(pair.guid), fs::path(dir + "/sub/hero.vat"));
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(fs::exists(pair.vat));
    EXPECT_TRUE(fs::exists(pair.mesh));
    EXPECT_EQ(scene.animator().animation, pair.relative);
    EXPECT_EQ(scene.model().filename, pair.meshRelative);

    // a copy (of the mesh alone) copies both; the copy is a new asset with its own GUID
    ASSERT_TRUE(history()->addCommand(new CopyFileCmd(project(), {pair.mesh.string()}, (root() / dir / "sub").string(), true)));
    const fs::path copied = root() / dir / "sub" / "hero.vat";
    EXPECT_TRUE(fs::exists(copied));
    EXPECT_TRUE(fs::exists(root() / dir / "sub" / "hero.vat.glb"));
    const std::string copyGuid = project()->getAssetDatabase().getGuid(copied);
    EXPECT_TRUE(AssetDatabase::isGuid(copyGuid));
    EXPECT_NE(copyGuid, pair.guid);
    EXPECT_EQ(AssetDatabase::readGuid(copied), copyGuid);
    EXPECT_EQ(scene.animator().animation, pair.relative) << "the users keep the original";
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(fs::exists(copied));
    EXPECT_FALSE(fs::exists(root() / dir / "sub" / "hero.vat.glb"));
}

TEST_F(VatEditor, DeleteTakesThePairAndUndoRelinks) {
    const std::string dir = folder("delete");
    const Pair pair = writePair(dir, "hero");
    VatScene scene = vatScene(pair);
    EXPECT_EQ(ev::withPairs({pair.vat}), (std::vector<fs::path>{pair.vat, pair.mesh}));
    EXPECT_EQ(ev::withPairs({pair.mesh, pair.vat}), (std::vector<fs::path>{pair.mesh, pair.vat}));

    ASSERT_TRUE(history()->addCommand(new DeleteFileCmd(project(), {pair.vat}, project()->getProjectPath())));
    EXPECT_FALSE(fs::exists(pair.vat));
    EXPECT_FALSE(fs::exists(pair.mesh)) << "one delete for both";
    EXPECT_EQ(scene.animator().guid, pair.guid) << "the reference keeps its GUID (Missing)";
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(fs::exists(pair.vat));
    EXPECT_TRUE(fs::exists(pair.mesh));
    EXPECT_EQ(scene.animator().animation, pair.relative) << "relinked by GUID";
    EXPECT_EQ(scene.model().filename, pair.meshRelative) << "and the Model plays its mesh again";
    EXPECT_EQ(project()->getAssetDatabase().getPath(pair.guid), fs::path(pair.relative));
}

TEST_F(VatEditor, ProjectWindowFileHelpers) {
    const std::string dir = folder("helpers");
    const Pair pair = writePair(dir, "hero");
    EXPECT_TRUE(ev::isVatMeshFile("a/hero.vat.glb"));
    EXPECT_FALSE(ev::isVatMeshFile("a/.vat.glb"));
    EXPECT_FALSE(ev::isVatMeshFile("a/hero.glb"));
    EXPECT_EQ(ev::pairPathOf("a/hero.vat"), fs::path("a/hero.vat.glb"));
    EXPECT_EQ(ev::pairPathOf("a/hero.vat.glb"), fs::path("a/hero.vat"));
    EXPECT_TRUE(ev::pairPathOf("a/hero.glb").empty());
    EXPECT_EQ(ev::existingPairOf(pair.vat), pair.mesh);
    EXPECT_TRUE(ev::existingPairOf(root() / dir / "lonely.vat").empty());
    fs::path from, to;
    EXPECT_TRUE(ev::renamedPair(pair.vat, root() / dir / "x.vat", from, to));
    EXPECT_EQ(from, pair.mesh);
    EXPECT_EQ(to, root() / dir / "x.vat.glb");
    EXPECT_FALSE(ev::renamedPair(pair.vat, root() / dir / "x.json", from, to)) << "leaving the type: alone";

    // what the context menu offers
    EXPECT_TRUE(ev::isVatBakeSource("m/hero.gltf"));
    EXPECT_TRUE(ev::isVatBakeSource("m/hero.FBX"));
    EXPECT_TRUE(ev::isVatBakeSource("m/walk.anim"));
    EXPECT_FALSE(ev::isVatBakeSource("m/hero.vat.glb"));
    EXPECT_FALSE(ev::isVatBakeSource("m/hero.vat"));
    EXPECT_TRUE(ev::isVatImportSource("x/cloth-remap_info.json"));
    EXPECT_TRUE(ev::isVatImportSource("x/explosion_data.json"));
    EXPECT_FALSE(ev::isVatImportSource("x/settings.json"));
    const fs::path houdini = root() / dir / "houdini";
    writeAll(houdini / "geo" / "rock_mesh.fbx", "fbx");
    writeAll(houdini / "tex" / "rock_pos.exr", "exr");
    writeAll(houdini / "unity" / "rock_mat.mat", "mat");
    writeAll(houdini / "plain" / "box.fbx", "fbx");
    writeAll(houdini / "open" / "flag.glb", "glb");
    writeAll(houdini / "open" / "flag_vat.exr", "exr");
    EXPECT_TRUE(ev::isVatImportSource(houdini / "geo" / "rock_mesh.fbx"));
    EXPECT_TRUE(ev::isVatImportSource(houdini / "unity" / "rock_mat.mat"));
    EXPECT_TRUE(ev::isVatImportSource(houdini / "open" / "flag.glb"));
    EXPECT_FALSE(ev::isVatImportSource(houdini / "plain" / "box.fbx"));
}

// ---- GameObjects: a dropped .vat, Create Crowd --------------------------------------------

TEST_F(VatEditor, ADroppedVatMakesOneGameObjectInOneStep) {
    const std::string dir = folder("drop");
    const Pair pair = writePair(dir, "hero");
    Project* p = project();
    p->createNewScene("Drop", SceneType::SCENE_3D);
    SceneProject* sceneProject = p->getScene(p->getSelectedSceneId());
    CommandHistory* sceneHistory = CommandHandle::get(sceneProject->id);
    const size_t steps = sceneHistory->position();
    const size_t entitiesBefore = sceneProject->entities.size();
    std::string error;
    const Entity entity = createVatObject(p, sceneProject->id, pair.vat.string(), Vector3(1, 0, 2), NULL_ENTITY, &error);
    ASSERT_NE(entity, NULL_ENTITY) << error;
    EXPECT_EQ(sceneHistory->position(), steps + 1) << "one undo step";
    Scene* scene = sceneProject->scene;
    EXPECT_EQ(scene->getEntityName(entity), "hero");
    ASSERT_TRUE(scene->findComponent<ModelComponent>(entity));
    EXPECT_EQ(scene->getComponent<ModelComponent>(entity).filename, pair.meshRelative);
    ASSERT_TRUE(scene->findComponent<VertexAnimatorComponent>(entity));
    EXPECT_EQ(scene->getComponent<VertexAnimatorComponent>(entity).animation, pair.relative);
    EXPECT_EQ(scene->getComponent<VertexAnimatorComponent>(entity).guid, pair.guid);
    EXPECT_FLOAT_EQ(scene->getComponent<Transform>(entity).position.x, 1.0f);
    EXPECT_FLOAT_EQ(scene->getComponent<Transform>(entity).position.z, 2.0f);
    EXPECT_TRUE(sceneProject->isModified);

    ASSERT_TRUE(sceneHistory->undo());
    EXPECT_FALSE(scene->isEntityCreated(entity));
    EXPECT_EQ(sceneProject->entities.size(), entitiesBefore);
    ASSERT_TRUE(sceneHistory->redo());
    EXPECT_TRUE(scene->isEntityCreated(entity));
    ASSERT_TRUE(scene->findComponent<VertexAnimatorComponent>(entity));
    EXPECT_EQ(scene->getComponent<ModelComponent>(entity).filename, pair.meshRelative);

    // refused: not a .vat, a missing mesh, outside the assets
    EXPECT_EQ(createVatObject(p, sceneProject->id, (root() / dir / "hero.vat.glb").string(), Vector3(), NULL_ENTITY, &error), NULL_ENTITY);
    fs::remove(root() / dir / "lone.vat");
    const Pair lone = writePair(dir, "lone");
    fs::remove(lone.mesh);
    EXPECT_EQ(createVatObject(p, sceneProject->id, lone.vat.string(), Vector3(), NULL_ENTITY, &error), NULL_ENTITY);
    EXPECT_NE(error.find("is missing"), std::string::npos) << error;
    EXPECT_EQ(sceneHistory->position(), steps + 1);
}

TEST(VatCrowdLayout, DeterministicBySeedWithSpacingAndCounts) {
    ev::CrowdSettings settings;
    settings.count = 200;
    settings.width = 20.0f;
    settings.depth = 10.0f;
    settings.minSpacing = 0.5f;
    settings.scaleMin = 0.8f;
    settings.scaleMax = 1.2f;
    settings.speedMin = 0.9f;
    settings.speedMax = 1.1f;
    settings.center = Vector3(5, 2, -3);
    settings.seed = 42;
    const ev::CrowdLayout a = ev::layoutCrowd(settings, nullptr);
    const ev::CrowdLayout b = ev::layoutCrowd(settings, nullptr);
    ASSERT_EQ(a.placed, 200);
    ASSERT_EQ(a.instances.size(), b.instances.size());
    for (size_t i = 0; i < a.instances.size(); i++) {
        EXPECT_EQ(a.instances[i].position, b.instances[i].position);
        EXPECT_EQ(a.instances[i].rotation, b.instances[i].rotation);
        EXPECT_EQ(a.instances[i].speed, b.instances[i].speed);
    }
    settings.seed = 43;
    const ev::CrowdLayout c = ev::layoutCrowd(settings, nullptr);
    EXPECT_NE(a.instances[0].position, c.instances[0].position) << "another seed, another crowd";

    // inside the area, on the plane through the center, spaced, in the ranges
    for (size_t i = 0; i < a.instances.size(); i++) {
        const ev::CrowdInstanceSpec& instance = a.instances[i];
        EXPECT_LE(std::fabs(instance.position.x), 10.0f);
        EXPECT_LE(std::fabs(instance.position.z), 5.0f);
        EXPECT_FLOAT_EQ(instance.position.y, 0.0f);
        EXPECT_GE(instance.scale.x, 0.8f);
        EXPECT_LE(instance.scale.x, 1.2f);
        EXPECT_GE(instance.speed, 0.9f);
        EXPECT_LE(instance.speed, 1.1f);
        EXPECT_TRUE(instance.clip.empty());
        for (size_t j = i + 1; j < a.instances.size(); j++) {
            const float dx = instance.position.x - a.instances[j].position.x;
            const float dz = instance.position.z - a.instances[j].position.z;
            EXPECT_GE(dx * dx + dz * dz, 0.25f - 1e-4f) << i << " and " << j;
        }
    }

    // a disc keeps its radius; no room: fewer placed
    settings.area = ev::CrowdArea::Disc;
    settings.width = 4.0f;
    settings.minSpacing = 1.5f;
    settings.count = 100;
    const ev::CrowdLayout tight = ev::layoutCrowd(settings, nullptr);
    EXPECT_LT(tight.placed, 100);
    EXPECT_GT(tight.placed, 0);
    EXPECT_EQ(tight.requested, 100);
    for (const ev::CrowdInstanceSpec& instance : tight.instances) {
        EXPECT_LE(instance.position.x * instance.position.x + instance.position.z * instance.position.z, 4.0f + 1e-3f);
    }

    // the ground: a height function lifts them
    settings.minSpacing = 0.0f;
    settings.count = 10;
    const ev::CrowdLayout hill = ev::layoutCrowd(settings, [](float, float, float& y) { y = 7.0f; return true; });
    for (const ev::CrowdInstanceSpec& instance : hill.instances) EXPECT_FLOAT_EQ(instance.position.y, 5.0f) << "7 - center 2";
}

TEST(VatCrowdLayout, ClipWeightsAndValidation) {
    ev::CrowdSettings settings;
    settings.count = 4000;
    settings.minSpacing = 0.0f;
    settings.clipWeights = {{"Walk", 70.0f}, {"Idle", 30.0f}, {"Run", 0.0f}};
    const ev::CrowdLayout layout = ev::layoutCrowd(settings, nullptr);
    std::map<std::string, int> counts;
    for (const ev::CrowdInstanceSpec& instance : layout.instances) counts[instance.clip]++;
    EXPECT_EQ(counts["Run"], 0);
    EXPECT_NEAR(counts["Walk"] / 4000.0, 0.7, 0.03);
    EXPECT_NEAR(counts["Idle"] / 4000.0, 0.3, 0.03);

    const std::vector<std::string> clips = {"Walk", "Idle", "Run"};
    EXPECT_EQ(ev::validateCrowd(settings, clips), "");
    ev::CrowdSettings bad = settings;
    bad.count = 0;
    EXPECT_NE(ev::validateCrowd(bad, clips).find("count must be"), std::string::npos);
    bad = settings;
    bad.clipWeights = {{"Fly", 1.0f}};
    EXPECT_NE(ev::validateCrowd(bad, clips).find("no clip 'Fly'"), std::string::npos);
    bad.clipWeights = {{"Walk", 0.0f}};
    EXPECT_NE(ev::validateCrowd(bad, clips).find("at least one clip"), std::string::npos);
    bad.clipWeights = {{"Walk", -1.0f}};
    EXPECT_NE(ev::validateCrowd(bad, clips).find("0 or more"), std::string::npos);
    bad.clipWeights = {{"Walk", 1.0f}, {"Walk", 1.0f}};
    EXPECT_NE(ev::validateCrowd(bad, clips).find("weighted twice"), std::string::npos);
    bad = settings;
    bad.scaleMin = 2.0f;
    bad.scaleMax = 1.0f;
    EXPECT_NE(ev::validateCrowd(bad, clips).find("scale range"), std::string::npos);
    bad = settings;
    bad.speedMin = 2.0f;
    bad.speedMax = 1.0f;
    EXPECT_NE(ev::validateCrowd(bad, clips).find("speed range"), std::string::npos);
    bad = settings;
    bad.minSpacing = -1.0f;
    EXPECT_NE(ev::validateCrowd(bad, clips).find("spacing"), std::string::npos);
}

TEST_F(VatEditor, CreateCrowdIsOneGameObjectInOneStep) {
    const std::string dir = folder("crowd");
    const Pair pair = writePair(dir, "hero");
    Project* p = project();
    p->createNewScene("Crowd", SceneType::SCENE_3D);
    SceneProject* sceneProject = p->getScene(p->getSelectedSceneId());
    CommandHistory* sceneHistory = CommandHandle::get(sceneProject->id);
    const size_t steps = sceneHistory->position();

    ev::CrowdSettings settings;
    settings.vat = pair.relative;
    settings.count = 50;
    settings.width = 12.0f;
    settings.depth = 12.0f;
    settings.minSpacing = 0.4f;
    settings.clipWeights = {{"Walk", 3.0f}, {"Idle", 1.0f}};
    settings.speedMin = 0.8f;
    settings.speedMax = 1.2f;
    settings.center = Vector3(0, 0, 4);
    settings.seed = 7;
    settings.instanceEvents = true;
    const ev::CrowdResult result = ev::createCrowd(p, sceneProject->id, settings);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(result.placed, 50);
    EXPECT_EQ(result.message, "Placed 50 of 50");
    EXPECT_EQ(sceneHistory->position(), steps + 1);
    Scene* scene = sceneProject->scene;
    const Entity entity = result.entity;
    EXPECT_EQ(scene->getEntityName(entity), "hero Crowd");
    ASSERT_TRUE(scene->findComponent<InstancedMeshComponent>(entity));
    ASSERT_TRUE(scene->findComponent<VertexAnimatorComponent>(entity));
    const InstancedMeshComponent& instanced = scene->getComponent<InstancedMeshComponent>(entity);
    const VertexAnimatorComponent& animator = scene->getComponent<VertexAnimatorComponent>(entity);
    EXPECT_EQ(instanced.instances.size(), 50u);
    EXPECT_GE(instanced.maxInstances, 50u);
    EXPECT_EQ(animator.animation, pair.relative);
    EXPECT_EQ(animator.guid, pair.guid);
    EXPECT_TRUE(animator.instanceEvents);
    ASSERT_EQ(animator.instanceClips.size(), 50u);
    ASSERT_EQ(animator.instanceSpeeds.size(), 50u);
    EXPECT_EQ(scene->getComponent<ModelComponent>(entity).filename, pair.meshRelative);
    EXPECT_FLOAT_EQ(scene->getComponent<Transform>(entity).position.z, 4.0f);
    // the same as the pure layout
    const ev::CrowdLayout layout = ev::layoutCrowd(settings, nullptr);
    for (size_t i = 0; i < 50; i++) {
        EXPECT_EQ(instanced.instances[i].position, layout.instances[i].position);
        EXPECT_EQ(animator.instanceClips[i], layout.instances[i].clip);
        EXPECT_FLOAT_EQ(animator.instanceSpeeds[i], layout.instances[i].speed);
    }

    ASSERT_TRUE(sceneHistory->undo());
    EXPECT_FALSE(scene->isEntityCreated(entity));
    ASSERT_TRUE(sceneHistory->redo());
    ASSERT_TRUE(scene->findComponent<InstancedMeshComponent>(entity));
    EXPECT_EQ(scene->getComponent<InstancedMeshComponent>(entity).instances.size(), 50u);
    EXPECT_EQ(scene->getComponent<VertexAnimatorComponent>(entity).instanceClips.size(), 50u);

    // a crowd that doesn't fit says how many it placed; a bad clip is refused with no step
    settings.count = 200;
    settings.width = 2.0f;
    settings.depth = 2.0f;
    settings.minSpacing = 0.5f;
    const ev::CrowdResult tight = ev::createCrowd(p, sceneProject->id, settings);
    ASSERT_TRUE(tight.ok) << tight.message;
    EXPECT_LT(tight.placed, 200);
    EXPECT_NE(tight.message.find("of 200 (minimum spacing 0.5 m left no room for"), std::string::npos) << tight.message;
    const size_t now = sceneHistory->position();
    settings.clipWeights = {{"Fly", 1.0f}};
    const ev::CrowdResult refused = ev::createCrowd(p, sceneProject->id, settings);
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.message.find("no clip 'Fly'"), std::string::npos) << refused.message;
    EXPECT_EQ(sceneHistory->position(), now);
    // every instance on the default clip at speed 1: nothing per instance is stored
    settings.clipWeights.clear();
    settings.speedMin = settings.speedMax = 1.0f;
    settings.count = 3;
    const ev::CrowdResult plain = ev::createCrowd(p, sceneProject->id, settings);
    ASSERT_TRUE(plain.ok) << plain.message;
    EXPECT_TRUE(scene->getComponent<VertexAnimatorComponent>(plain.entity).instanceClips.empty());
    EXPECT_TRUE(scene->getComponent<VertexAnimatorComponent>(plain.entity).instanceSpeeds.empty());
}

// ---- the Inspector's pure helpers ------------------------------------------------------------

TEST(VatInspectorHelpers, TextsFixesAndLimits) {
    EXPECT_EQ(inspectorvat::formatBytes(812), "812 B");
    EXPECT_EQ(inspectorvat::formatBytes(14540), "14.2 KB");
    EXPECT_EQ(inspectorvat::formatBytes(1814036), "1.73 MB");
    jv::VatData data = quadData("00112233445566778899aabbccddeeff");
    data.stats.bytes = 256;
    EXPECT_EQ(inspectorvat::modeLine(data), "Vertices \xC2\xB7 4 vertices, normals");
    EXPECT_EQ(inspectorvat::textureLine(data), "8 \xC3\x97 4 RGBA16F \xC2\xB7 256 B");
    EXPECT_EQ(inspectorvat::errorLine(data), "Error: max 0.25 mm, mean 0.05 mm");
    EXPECT_EQ(inspectorvat::clipBytes(data, data.clips[0]), 2u * 4u * 2u * 8u);
    data.mode = jv::VatMode::Bones;
    data.layout.bones = 41;
    EXPECT_EQ(inspectorvat::modeLine(data), "Bones \xC2\xB7 41 bones");

    const std::string mismatch = "mesh_mismatch: the Model of 'Hero' is models/hero.gltf, but vat/hero.vat plays on its baked mesh vat/hero.vat.glb";
    EXPECT_EQ(inspectorvat::meshMismatchFix(mismatch, "vat/hero.vat", "hero.vat.glb"), "vat/hero.vat.glb");
    EXPECT_EQ(inspectorvat::meshMismatchFix(mismatch, "vat/hero.vat", ""), "vat/hero.vat.glb");
    EXPECT_EQ(inspectorvat::meshMismatchFix("vat/hero.vat: cannot read", "vat/hero.vat", "hero.vat.glb"), "");
    EXPECT_EQ(inspectorvat::meshFixLabel("vat/hero.vat.glb"), "Use hero.vat.glb");

    EXPECT_EQ(inspectorvat::deviceLimitWarning(2048, 4096, "vat/hero.vat"), "");
    EXPECT_EQ(inspectorvat::deviceLimitWarning(5000, 0, "vat/hero.vat"), "") << "unknown limit";
    EXPECT_NE(inspectorvat::deviceLimitWarning(5000, 4096, "vat/hero.vat").find("hero.vat needs a texture 5000 rows tall; this device allows 4096"),
              std::string::npos);

    // the scrubber: a clip's time at a local time, offsets kept
    jv::VatClip loop = data.clips[0];   // 2 frames at 2 fps, loop: 1 s
    EXPECT_NEAR(inspectorvat::clipTimeAt(loop, 0.25), 0.25f, 1e-5f);
    EXPECT_NEAR(inspectorvat::clipTimeAt(loop, 2.25), 0.25f, 1e-5f);
    const std::vector<double> shifted = inspectorvat::shiftedTimes({0.1, 0.7, 3.0}, 0.5, 0.75);
    EXPECT_NEAR(shifted[0], 0.35, 1e-9);
    EXPECT_NEAR(shifted[2], 3.25, 1e-9);
    EXPECT_STREQ(inspectorvat::wrapLabel(ClipWrapMode::PingPong), "Ping Pong");
    EXPECT_EQ(inspectorvat::stalenessText("vat-source-changed", "'m/hero.gltf' changed since 'v/hero.vat' was baked", "rebake it"),
              "Source changed since baking: 'm/hero.gltf' changed since 'v/hero.vat' was baked. Rebake it.");
    EXPECT_EQ(inspectorvat::stalenessText("vat-mesh-mismatch", "the mesh 'hero.vat.glb' is missing", ""),
              "the mesh 'hero.vat.glb' is missing.");
    EXPECT_EQ(inspectorvat::removedClipsNote(quadData("")), "");
}

TEST(VatInspectorHelpers, EventArgsReadBackWhatTheyShow) {
    std::vector<AnimationEventArg> args = {AnimationEvent::stringArg("left"), AnimationEvent::numberArg(1.5),
                                           AnimationEvent::boolArg(true), AnimationEvent::stringArg("a, b"),
                                           AnimationEvent::stringArg("2"), AnimationEvent::stringArg("")};
    const std::string text = inspectorvat::eventArgsText(args);
    EXPECT_EQ(text, "left, 1.5, true, \"a, b\", \"2\", \"\"");
    std::vector<AnimationEventArg> back;
    std::string error;
    ASSERT_TRUE(inspectorvat::parseEventArgs(text, back, &error)) << error;
    ASSERT_EQ(back.size(), args.size());
    for (size_t i = 0; i < args.size(); i++) EXPECT_EQ(back[i], args[i]) << i;
    ASSERT_TRUE(inspectorvat::parseEventArgs("", back, &error));
    EXPECT_TRUE(back.empty());
    EXPECT_FALSE(inspectorvat::parseEventArgs("a,,b", back, &error));
    EXPECT_FALSE(inspectorvat::parseEventArgs("\"open", back, &error));
    EXPECT_FALSE(inspectorvat::parseEventArgs("\"a\" b", back, &error));
    EXPECT_TRUE(inspectorvat::isLuaName("onStep_2"));
    EXPECT_FALSE(inspectorvat::isLuaName("2step"));
    EXPECT_FALSE(inspectorvat::isLuaName("on step"));
}
