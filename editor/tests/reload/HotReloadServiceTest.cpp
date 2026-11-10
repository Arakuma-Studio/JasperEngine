// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// HotReloadService::apply on a headless project (S05 AC10), fed change sets directly (no
// watcher): a texture that fails to decode keeps the old one and is tried once more; a shader
// include reaches only the forks that include it; an open scene changed on disk gets its bar
// and is left as it is; the file kinds the funnel dispatches on.

#include "support/EditorTestSupport.h"
#include "TempDir.h"

#include "Project.h"
#include "graph/GraphDocument.h"
#include "graph/GraphRegistry.h"
#include "io/watch/FileWatchService.h"
#include "pool/TextureDataPool.h"
#include "reload/HotReloadService.h"
#include "reload/handlers/ReloadHandlers.h"
#include "subsystem/MeshSystem.h"
#include "texture/Texture.h"
#include "util/PngWriter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    void writeSolidPng(const fs::path& path, unsigned char r, unsigned char g, unsigned char b) {
        std::vector<unsigned char> pixels;
        for (int i = 0; i < 4 * 4; i++) pixels.insert(pixels.end(), {r, g, b, 255});
        ASSERT_TRUE(writePng(path, 4, 4, 4, pixels.data(), 4 * 4));
    }

    class HotReloadServiceTests : public ::testing::Test {
    protected:
        TempDir dir;
        Project project;
        HotReloadService& service = HotReloadService::get();

        void SetUp() override {
            editor::test::ensureEngine();
            std::string error;
            ASSERT_TRUE(project.createProjectAt(dir.path() / "HotReload", "HotReload", &error)) << error;
            // (a project resets the engine's settings: textures load on this thread from here)
            Engine::setAsyncLoading(false);
            service.attach(project);
        }
        void TearDown() override {
            service.detach();
            // closes the project's scenes as a project switch does (a Project frees none itself)
            project.releaseScenes();
        }
        // what the editor's loop does with the watcher running (App::pollExternalChanges),
        // until `done` or `limit`
        bool pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds limit) {
            const auto end = std::chrono::steady_clock::now() + limit;
            while (std::chrono::steady_clock::now() < end) {
                FileWatchService::get().pump(50.0);
                service.update();
                project.updateAssetScan();
                if (done()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return done();
        }
        // what the editor's frames do: adopt finished reloads, until none is left
        void settle(std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
            const auto end = std::chrono::steady_clock::now() + limit;
            do {
                service.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (!service.idle() && std::chrono::steady_clock::now() < end);
            service.update();
        }
    };

}

TEST_F(HotReloadServiceTests, TextureFailureKeepsTheOldOneAndRetriesOnce) {
    const fs::path file = project.getProjectPath() / "assets" / "hr" / "tex.png";
    writeSolidPng(file, 255, 0, 0);
    Texture texture(file.string());
    ASSERT_TRUE(texture.load());
    const std::string id = texture.getId();
    const uint32_t generation = TextureDataPool::generation(id);

    // a file that is not an image any more
    writeText(file, "not a png");
    const auto started = std::chrono::steady_clock::now();
    ApplyResult result = service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    ASSERT_EQ(result.pending.size(), 1u);
    settle();
    HotReloadService::Record record;
    ASSERT_TRUE(service.lastRecordOf("assets/hr/tex.png", record));
    EXPECT_FALSE(record.ok);
    EXPECT_EQ(record.attempts, 2) << "a failure is tried once more";
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500)) << "after 500 ms";
    EXPECT_EQ(TextureDataPool::generation(id), generation) << "the old pixels stay";
    EXPECT_EQ(static_cast<unsigned char*>(texture.getData().getData())[0], 255);
    bool problem = false;
    for (const Json& item : service.takeLateProblems()) problem = problem || item.value("path", std::string()) == "assets/hr/tex.png";
    EXPECT_TRUE(problem) << "a problem names the file";

    // and a good file after it reloads (generation up, every copy sees it)
    writeSolidPng(file, 0, 255, 0);
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    settle();
    ASSERT_TRUE(service.lastRecordOf("assets/hr/tex.png", record));
    EXPECT_TRUE(record.ok);
    EXPECT_EQ(TextureDataPool::generation(id), generation + 1);
    ASSERT_TRUE(texture.load());
    EXPECT_EQ(static_cast<unsigned char*>(texture.getData().getData())[1], 255);
}

namespace {

    // the journal change `sequence` of `file` (what the watcher delivers and McpAutoSync passes on)
    ChangeSet journalChange(const fs::path& file, uint64_t sequence) {
        ChangeSet set = ChangeSet::fromPaths({file});
        set.changes[0].sequence = sequence;
        return set;
    }

    void setMtime(const fs::path& file, fs::file_time_type time) {
        std::error_code ec;
        fs::last_write_time(file, time, ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    unsigned char firstTexel(Texture& texture) {
        return static_cast<unsigned char*>(texture.getData().getData())[0];
    }

}

TEST_F(HotReloadServiceTests, AppliedOnceThenCurrent) {
    const fs::path file = project.getProjectPath() / "assets" / "hr" / "once.png";
    writeSolidPng(file, 10, 20, 30);
    Texture texture(file.string());
    ASSERT_TRUE(texture.load());
    writeSolidPng(file, 40, 50, 60);
    const uint64_t before = service.reloadCount();
    service.apply(journalChange(file, 7), ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 1);
    // the same journal change again (an MCP call's pick-up of what the watcher reloaded, just
    // after the save): reported, not redone
    ApplyResult again = service.apply(journalChange(file, 7), ApplyOptions());
    EXPECT_TRUE(again.pending.empty());
    ASSERT_EQ(again.reloaded.size(), 1u);
    EXPECT_EQ(again.reloaded[0]["path"], "assets/hr/once.png");
    settle();
    EXPECT_EQ(service.reloadCount(), before + 1);
}

TEST_F(HotReloadServiceTests, SettledStampSaysCurrent) {
    const fs::path file = project.getProjectPath() / "assets" / "hr" / "settled.png";
    writeSolidPng(file, 10, 20, 30);
    Texture texture(file.string());
    ASSERT_TRUE(texture.load());
    writeSolidPng(file, 40, 50, 60);
    setMtime(file, fs::file_time_type::clock::now() - std::chrono::seconds(10));
    const uint64_t before = service.reloadCount();
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 1);
    // the file as reloaded, stamped long after its write: a path with no journal change is current
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 1);
}

// Two saves in one timestamp tick keep the mtime and the size: a stamp taken that close to the
// write does not say the second one is the first; the bytes do
TEST_F(HotReloadServiceTests, RacyStampComparesTheBytes) {
    const fs::path file = project.getProjectPath() / "assets" / "hr" / "racy.png";
    writeSolidPng(file, 10, 20, 30);
    Texture texture(file.string());
    ASSERT_TRUE(texture.load());

    // one tick for every save, ahead of the clock: racy however slowly the test runs (sanitizers)
    const fs::file_time_type tick = fs::file_time_type::clock::now() + std::chrono::hours(1);
    writeSolidPng(file, 40, 50, 60);
    setMtime(file, tick);
    service.apply(journalChange(file, 11), ApplyOptions());
    settle();
    ASSERT_TRUE(texture.load());
    EXPECT_EQ(firstTexel(texture), 40);

    // the next save, same size, same tick, read later through a path (no journal change) and
    // through the watcher's next journal change
    const uintmax_t size = fs::file_size(file);
    writeSolidPng(file, 70, 80, 90);
    setMtime(file, tick);
    ASSERT_EQ(fs::file_size(file), size) << "the same stat (time and size) as the save reloaded";
    const uint64_t before = service.reloadCount();
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 1) << "a racy stamp with other bytes is not current";
    ASSERT_TRUE(texture.load());
    EXPECT_EQ(firstTexel(texture), 70);

    writeSolidPng(file, 100, 110, 120);
    setMtime(file, tick);
    ASSERT_EQ(fs::file_size(file), size);
    service.apply(journalChange(file, 12), ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 2) << "a new journal change of a racy stamp reloads";
    ASSERT_TRUE(texture.load());
    EXPECT_EQ(firstTexel(texture), 100);

    // the same bytes again (the watcher's change for the save a path reload already read): current
    service.apply(journalChange(file, 13), ApplyOptions());
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 2) << "a racy stamp with the same bytes is current";

    // the editor's own write is the one it reloaded (it knows what it wrote)
    ChangeSet own = ChangeSet::fromPaths({file});
    own.changes[0].origin = ChangeOrigin::Editor;
    service.apply(own, ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 2);
}

TEST_F(HotReloadServiceTests, ShaderIncludeReachesOnlyItsForks) {
    const fs::path root = project.getProjectPath();
    writeText(root / "shaders" / "lib" / "common.glsl", "float common() { return 1.0; }\n");
    writeText(root / "shaders" / "a.vert", "#version 450\nvoid main() {}\n");
    writeText(root / "shaders" / "a.frag", "#version 450\n#include \"lib/common.glsl\"\nvoid main() {}\n");
    writeText(root / "shaders" / "b.vert", "#version 450\nvoid main() {}\n");
    writeText(root / "shaders" / "b.frag", "#version 450\nvoid main() {}\n");
    const std::set<std::string> changed = {"shaders/lib/common.glsl"};
    EXPECT_TRUE(reload::shaderSourcesReach(&project, "shaders/a", 0, changed)) << "a includes it";
    EXPECT_FALSE(reload::shaderSourcesReach(&project, "shaders/b", 0, changed)) << "b does not";
    EXPECT_TRUE(reload::shaderSourcesReach(&project, "shaders/b", 0, {"shaders/b.frag"})) << "its own entry point";
}

TEST_F(HotReloadServiceTests, OpenSceneChangedOnDiskShowsTheBarAndStays) {
    SceneProject* scene = nullptr;
    for (SceneProject& candidate : project.getScenes()) {
        if (candidate.scene && !candidate.filepath.empty()) scene = &candidate;
    }
    if (!scene) {
        // a new project's scene is saved on its first save
        ASSERT_TRUE(project.saveSceneNow(project.getSelectedSceneId(), "scenes/Main.scene"));
        scene = project.getSelectedScene();
    }
    ASSERT_NE(scene, nullptr);
    const size_t entities = scene->entities.size();
    const fs::path file = project.getProjectPath() / scene->filepath;
    // another program appends to it
    {
        std::ofstream out(file, std::ios::binary | std::ios::app);
        out << "\n# edited outside\n";
    }
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    EXPECT_TRUE(service.hasExternalChange(scene->id)) << "the Scene view shows Reload / Keep Mine";
    EXPECT_EQ(scene->entities.size(), entities) << "never reloaded on its own";
    service.keepMine(scene->id);
    EXPECT_FALSE(service.hasExternalChange(scene->id));
}

TEST_F(HotReloadServiceTests, EditorWritesFanOutButDoNotTouchScenes) {
    SceneProject* scene = project.getSelectedScene();
    ASSERT_NE(scene, nullptr);
    if (scene->filepath.empty()) {
        ASSERT_TRUE(project.saveSceneNow(scene->id, "scenes/Main.scene"));
    }
    FileChange change;
    change.kind = FileChangeKind::Modified;
    change.path = project.getProjectPath() / project.getScene(scene->id)->filepath;
    change.origin = ChangeOrigin::Editor;
    ChangeSet set;
    set.changes.push_back(change);
    service.apply(set, ApplyOptions());
    EXPECT_FALSE(service.hasExternalChange(scene->id)) << "the editor's own save is no external change";
}

// S90 review I4: the editor's own writes of a graph file (an agent's write_file, a delete in the Project
// window) reach its open document, which then never writes a deleted file back
TEST_F(HotReloadServiceTests, EditorWritesAndDeletesReachAnOpenGraph) {
    graph::GraphKindDesc kind;
    kind.id = "reload_test";
    kind.fileExtension = ".rtgraph";
    graph::NodeTypeDesc box;
    box.id = "box";
    kind.nodeTypes = {box};
    graph::GraphRegistry::get().registerKind(kind);
    const fs::path file = project.getProjectPath() / "assets" / "flow.rtgraph";
    auto graphText = [](const std::string& nodes) {
        return R"({"formatVersion": 1, "kind": "reload_test", "kindVersion": 1, "nextId": 9, "nodes": [)" + nodes + R"(], "edges": []})";
    };
    writeText(file, graphText(R"({"id": "n1", "type": "box", "position": [0, 0]})"));
    std::string error;
    std::shared_ptr<GraphDocument> document = GraphDocument::open(&project, file.string(), &error);
    ASSERT_TRUE(document) << error;

    auto apply = [&](FileChangeKind changeKind) {
        FileChange change;
        change.kind = changeKind;
        change.path = file;
        change.origin = ChangeOrigin::Editor;
        ChangeSet set;
        set.changes.push_back(change);
        service.apply(set, ApplyOptions());
    };
    writeText(file, graphText(R"({"id": "n1", "type": "box", "position": [0, 0]}, {"id": "n2", "type": "box", "position": [9, 9]})"));
    apply(FileChangeKind::Modified);
    EXPECT_EQ(document->graph().nodes.size(), 2u) << "write_file's content reached the document";

    fs::remove(file);
    apply(FileChangeKind::Removed);
    EXPECT_FALSE(document->editable()) << "a deleted file's document is read-only";
    EXPECT_FALSE(document->apply({{"moveNodes", {{"positions", {{"n1", {5, 5}}}}}}}, "", &error));
    EXPECT_FALSE(fs::exists(file)) << "never written back";
    GraphDocument::closeAll();
    graph::GraphRegistry::get().unregisterKind("reload_test");
}

TEST(ModelReloadTests, HierarchyMismatchIsReportedNotRebuilt) {
    editor::test::ensureEngine();
    TempDir dir;
    const fs::path file = dir.path() / "model.gltf";
    auto gltf = [](const std::string& nodes, const std::string& roots) {
        return "{\"asset\": {\"version\": \"2.0\"}, \"scene\": 0, \"scenes\": [{\"nodes\": [" + roots + "]}], \"nodes\": [" + nodes + "]}";
    };
    writeText(file, gltf("{\"name\": \"Root\", \"children\": [1]}, {\"name\": \"Arm\"}", "0"));
    std::shared_ptr<MeshSystem::ModelReload> first = MeshSystem::parseModelForReload(file.string());
    MeshSystem::adoptModelReload(first);   // what the scene loaded

    // the same nodes with other contents: rebuilt in place
    writeText(file, gltf("{\"name\": \"Root\", \"children\": [1], \"translation\": [1, 0, 0]}, {\"name\": \"Arm\"}", "0"));
    std::string why;
    EXPECT_TRUE(MeshSystem::modelHierarchyMatches(*MeshSystem::parseModelForReload(file.string()), why)) << why;

    // a node more: no rebuild, a reason that names it
    writeText(file, gltf("{\"name\": \"Root\", \"children\": [1, 2]}, {\"name\": \"Arm\"}, {\"name\": \"Leg\"}", "0"));
    EXPECT_FALSE(MeshSystem::modelHierarchyMatches(*MeshSystem::parseModelForReload(file.string()), why));
    EXPECT_NE(why.find("nodes"), std::string::npos) << why;

    // a broken file throws with the parser's message (the reload keeps the old one)
    writeText(file, "{ not json");
    EXPECT_THROW(MeshSystem::parseModelForReload(file.string()), std::runtime_error);
}

TEST(HotReloadKinds, FilesByKind) {
    EXPECT_EQ(reload::kindOf("a/b.PNG"), "texture");
    EXPECT_EQ(reload::kindOf("a/b.svg"), "texture");
    EXPECT_EQ(reload::kindOf("a/b.cube"), "texture");
    EXPECT_EQ(reload::kindOf("a/b.glb"), "model");
    EXPECT_EQ(reload::kindOf("a/b.material"), "material");
    EXPECT_EQ(reload::kindOf("a/b.glsl"), "shader");
    EXPECT_EQ(reload::kindOf("a/b.ogg"), "audio");
    EXPECT_EQ(reload::kindOf("a/b.ttf"), "font");
    EXPECT_EQ(reload::kindOf("a/b.lua"), "script");
    EXPECT_EQ(reload::kindOf("a/b.css"), "ui");
    EXPECT_EQ(reload::kindOf("a/b.scene"), "scene");
    EXPECT_EQ(reload::kindOf("a/b.prefab"), "prefab");
    EXPECT_EQ(reload::kindOf("a/b.shake"), "animation") << "Shake Presets: read again, their problems reported";
    EXPECT_EQ(reload::kindOf("project.yaml"), "project");
    EXPECT_EQ(reload::kindOf("a/b.bin"), "");
    EXPECT_TRUE(reload::isEditorOwnedFile("assets/terrain_maps/terrain_edit_height.png"));
    EXPECT_FALSE(reload::isEditorOwnedFile("assets/maps/terrain_edit_height.png"));
}

TEST(HotReloadKinds, ChangeSetFromPathsAsksTheDisk) {
    TempDir dir;
    writeText(dir.path() / "there.txt", "x");
    ChangeSet set = ChangeSet::fromPaths({dir.path() / "there.txt", dir.path() / "gone.txt"});
    ASSERT_EQ(set.changes.size(), 2u);
    EXPECT_EQ(set.changes[0].kind, FileChangeKind::Modified);
    EXPECT_EQ(set.changes[1].kind, FileChangeKind::Removed);
    EXPECT_EQ(set.changes[0].origin, ChangeOrigin::External);
}

// S04's identity follows a rename made by another program through the watcher alone: the
// database looks at the disk when a batch arrives (no poll), the GUID stays with the file and
// the old path no longer resolves. With the meta carried along (git mv), and without it (a file
// manager renaming the image only: the orphan meta is adopted, S04 R5).
TEST_F(HotReloadServiceTests, ExternalRenameKeepsTheGuidThroughTheWatcher) {
    AssetDatabase& database = project.getAssetDatabase();
    const fs::path folder = project.getProjectPath() / "assets" / "hr";
    writeSolidPng(folder / "hero.png", 10, 20, 30);
    writeSolidPng(folder / "villain.png", 40, 50, 60);
    project.refreshAssetDatabase(true);
    // R5 gives the orphan meta to the renamed file by the content hash the index has of the old
    // one (without a hash it needs the same file name, which a rename never keeps). The hashes
    // are computed in the background after the refresh: a loaded machine had not done the
    // villain's yet when it was renamed, and the villain got a new identity.
    database.waitForBackgroundWork();
    const std::string hero = database.getGuid("assets/hr/hero.png");
    const std::string villain = database.getGuid("assets/hr/villain.png");
    ASSERT_FALSE(hero.empty());
    ASSERT_FALSE(villain.empty());
    ASSERT_TRUE(fs::exists(folder / "hero.png.meta"));

    service.startWatching(project);
    ASSERT_TRUE(FileWatchService::get().isAttached());
    ASSERT_TRUE(FileWatchService::get().flush(std::chrono::milliseconds(5000)));

    fs::rename(folder / "hero.png", folder / "hero_moved.png");
    fs::rename(folder / "hero.png.meta", folder / "hero_moved.png.meta");
    fs::rename(folder / "villain.png", folder / "villain_moved.png");
    const bool followed = pumpUntil([&]() {
        return database.getPath(hero).generic_string() == "assets/hr/hero_moved.png" &&
               database.getPath(villain).generic_string() == "assets/hr/villain_moved.png";
    }, std::chrono::milliseconds(10000));
    service.stopWatching();
    ASSERT_TRUE(followed) << "hero at " << database.getPath(hero) << ", villain at " << database.getPath(villain);
    EXPECT_EQ(database.getGuid("assets/hr/hero_moved.png"), hero);
    EXPECT_EQ(database.getGuid("assets/hr/villain_moved.png"), villain);
    EXPECT_TRUE(database.getGuid("assets/hr/hero.png").empty());
    EXPECT_TRUE(database.getGuid("assets/hr/villain.png").empty());
    // the orphan meta went with its file
    EXPECT_TRUE(fs::exists(folder / "villain_moved.png.meta"));
    EXPECT_FALSE(fs::exists(folder / "villain.png.meta"));
}

// Without the watcher nothing looks at the disk on its own any more (S04's 2 s poll is gone):
// only a forced refresh (Assets > Refresh, a tool answering from the database) sees a rename
TEST_F(HotReloadServiceTests, NoAssetPollWithoutTheWatcher) {
    AssetDatabase& database = project.getAssetDatabase();
    const fs::path folder = project.getProjectPath() / "assets" / "hr";
    writeSolidPng(folder / "still.png", 1, 2, 3);
    project.refreshAssetDatabase(true);
    const std::string guid = database.getGuid("assets/hr/still.png");
    ASSERT_FALSE(guid.empty());

    fs::rename(folder / "still.png", folder / "still_moved.png");
    fs::rename(folder / "still.png.meta", folder / "still_moved.png.meta");
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < end) {
        project.updateAssetScan();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(database.getPath(guid).generic_string(), "assets/hr/still.png") << "something polled";
    project.refreshAssetDatabase(true);
    EXPECT_EQ(database.getPath(guid).generic_string(), "assets/hr/still_moved.png");
}

