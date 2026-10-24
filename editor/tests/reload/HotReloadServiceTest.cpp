// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// HotReloadService::apply on a headless project (S05 AC10), fed change sets directly (no
// watcher): a texture that fails to decode keeps the old one and is tried once more; a shader
// include reaches only the forks that include it; an open scene changed on disk gets its bar
// and is left as it is; the file kinds the funnel dispatches on.

#include "support/EditorTestSupport.h"

#include "Project.h"
#include "pool/TextureDataPool.h"
#include "reload/HotReloadService.h"
#include "reload/handlers/ReloadHandlers.h"
#include "subsystem/MeshSystem.h"
#include "texture/Texture.h"
#include "util/PngWriter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct TempDir {
        fs::path path;
        TempDir() {
            path = fs::temp_directory_path() / ("je-watch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

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
            ASSERT_TRUE(project.createProjectAt(dir.path / "HotReload", "HotReload", &error)) << error;
            // (a project resets the engine's settings: textures load on this thread from here)
            Engine::setAsyncLoading(false);
            service.attach(project);
        }
        void TearDown() override {
            service.detach();
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

TEST_F(HotReloadServiceTests, AppliedOnceThenCurrent) {
    const fs::path file = project.getProjectPath() / "assets" / "hr" / "once.png";
    writeSolidPng(file, 10, 20, 30);
    Texture texture(file.string());
    ASSERT_TRUE(texture.load());
    writeSolidPng(file, 40, 50, 60);
    const uint64_t before = service.reloadCount();
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    settle();
    EXPECT_EQ(service.reloadCount(), before + 1);
    // the same bytes again (the watcher's batch after an MCP call's sync): reported, not redone
    ApplyResult again = service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    EXPECT_TRUE(again.pending.empty());
    ASSERT_EQ(again.reloaded.size(), 1u);
    EXPECT_EQ(again.reloaded[0]["path"], "assets/hr/once.png");
    settle();
    EXPECT_EQ(service.reloadCount(), before + 1);
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

TEST(ModelReloadTests, HierarchyMismatchIsReportedNotRebuilt) {
    editor::test::ensureEngine();
    TempDir dir;
    const fs::path file = dir.path / "model.gltf";
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
    EXPECT_EQ(reload::kindOf("a/b.glb"), "model");
    EXPECT_EQ(reload::kindOf("a/b.material"), "material");
    EXPECT_EQ(reload::kindOf("a/b.glsl"), "shader");
    EXPECT_EQ(reload::kindOf("a/b.ogg"), "audio");
    EXPECT_EQ(reload::kindOf("a/b.ttf"), "font");
    EXPECT_EQ(reload::kindOf("a/b.lua"), "script");
    EXPECT_EQ(reload::kindOf("a/b.css"), "ui");
    EXPECT_EQ(reload::kindOf("a/b.scene"), "scene");
    EXPECT_EQ(reload::kindOf("a/b.prefab"), "prefab");
    EXPECT_EQ(reload::kindOf("project.yaml"), "project");
    EXPECT_EQ(reload::kindOf("a/b.bin"), "");
    EXPECT_TRUE(reload::isEditorOwnedFile("assets/terrain_maps/terrain_edit_height.png"));
    EXPECT_FALSE(reload::isEditorOwnedFile("assets/maps/terrain_edit_height.png"));
}

TEST(HotReloadKinds, ChangeSetFromPathsAsksTheDisk) {
    TempDir dir;
    writeText(dir.path / "there.txt", "x");
    ChangeSet set = ChangeSet::fromPaths({dir.path / "there.txt", dir.path / "gone.txt"});
    ASSERT_EQ(set.changes.size(), 2u);
    EXPECT_EQ(set.changes[0].kind, FileChangeKind::Modified);
    EXPECT_EQ(set.changes[1].kind, FileChangeKind::Removed);
    EXPECT_EQ(set.changes[0].origin, ChangeOrigin::External);
}
