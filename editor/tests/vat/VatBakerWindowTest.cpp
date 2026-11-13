// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vertex Animation Baker window (vertex animation spec 5.1, 5.2 first bullet) through the
// functions its fields, buttons and the Inspector's Bake entries run (VertexAnimationBakerWindow's
// scripted API) and VatBakerWindowTestAccess, the friend the window declares for its internals (the
// AnimationWindowTestAccess pattern: no test-only member). A headless project as VatBakerTest's
// (the T1c hero, rigid_arm, morph_blob), the window drawn in a headless ImGui frame where the frame
// matters. Covered: source detection -> clip list and summary, settings -> estimate, fix buttons
// (too_large's fps, output_exists's overwrite, interpolation off, clips, preset, mode, normals),
// the bake time-sliced to completion and written through its command (one undo step), cancel
// writing nothing, a preview run then Bake writing it without a second bake, Rebake from a .vat
// keeping its GUID and edits, staleness notes, the import flow on a synthetic Houdini soft export
// (the importer tests' fixture writers), the Inspector entries' hooks prefilling the window (and
// Shift-click baking placed beside), the scene playing refusing, the preview views loading the
// source and the in-memory VAT, and every section drawn in an ImGui frame.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "vat/VatImportFixtures.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "render/preview/VatPreviewRender.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "vat/VatAppHooks.h"
#include "vat/VatBakerHooks.h"
#include "window/VertexAnimationBakerWindow.h"

#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "component/AnimationPlayerComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/AnimationClipPool.h"
#include "subsystem/MeshSystem.h"

#include "imgui.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vat;
namespace fs = std::filesystem;

namespace jasperengine::editor {

    // the window's internals the tests read and drive
    struct VatBakerWindowTestAccess {
        static VatBakeSession* run(VertexAnimationBakerWindow& w) { return w.run.get(); }
        static VatPreviewView* sourceView(VertexAnimationBakerWindow& w) { return w.sourceView.get(); }
        static VatPreviewView* vatView(VertexAnimationBakerWindow& w) { return w.vatView.get(); }
        static const std::string& previewNote(VertexAnimationBakerWindow& w) { return w.previewNote; }
        static void preview(VertexAnimationBakerWindow& w, float dt) { w.updatePreview(dt); }
        static float& previewTime(VertexAnimationBakerWindow& w) { return w.previewTime; }
        static bool& previewPlaying(VertexAnimationBakerWindow& w) { return w.previewPlaying; }
        static int& previewClip(VertexAnimationBakerWindow& w) { return w.previewClip; }
        static const std::vector<std::string>& notes(VertexAnimationBakerWindow& w) { return w.sourceNotes; }
        static const std::string& rebakePath(VertexAnimationBakerWindow& w) { return w.rebakePath; }
        static const std::string& sourceLabel(VertexAnimationBakerWindow& w) { return w.sourceLabel; }
        static const VatImportDetection& detection(VertexAnimationBakerWindow& w) { return w.detection; }
        static bool noneChecked(VertexAnimationBakerWindow& w) { return w.noneChecked; }
        // the next tick checks the source now (watchSource runs a few times a second)
        static void checkSourceNow(VertexAnimationBakerWindow& w) { w.sourceCheckedAt = -1.0; }
        // writes the stored in-memory result as finishRun does, past Bake's checks (what a write
        // throws must end in the window's error, not in the editor)
        static void writeStored(VertexAnimationBakerWindow& w) {
            w.writeWhenDone = true;
            w.runFinished = false;
            w.tick(0.0);
        }
        static uint32_t crowdSeed(VertexAnimationBakerWindow& w) { return w.crowdSeedValue(); }
        // the Place: Crowd... fields
        static void crowd(VertexAnimationBakerWindow& w, int count, float area, int seed) {
            w.crowdCount = count;
            w.crowdArea = area;
            w.crowdSeed = seed;
        }
    };

}

namespace {

    using Access = VatBakerWindowTestAccess;

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    fs::path repoFile(const fs::path& relative) {
        return (fs::path(JASPER_TEST_FIXTURES_DIR) / ".." / ".." / ".." / relative).lexically_normal();
    }

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-vat-window-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "models");
            std::ofstream(out.dir / "project.yaml") << "name: vatwindow\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        // every test of the project (the paths end with each test)
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    bool headlessStarted = false;
    Project* project() {
        headlessStarted = true;
        return headless().project.get();
    }
    const fs::path& root() { return headless().dir; }
    CommandHistory* history(uint32_t sceneId) { return CommandHandle::get(sceneId); }
    Scene* sceneOf(uint32_t sceneId) { return project()->getScene(sceneId)->scene; }

    struct TempProjectCleanup : public ::testing::Environment {
        void TearDown() override {
            if (!headlessStarted) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const tempProjectCleanup = ::testing::AddGlobalTestEnvironment(new TempProjectCleanup);

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    void resetAssets() {
        const fs::path models = root() / "assets" / "models";
        fs::remove_all(root() / "assets");
        fs::create_directories(models);
        const fs::path pose = fs::path("tools") / "fixtures" / "pose-animation" / "assets" / "models";
        const fs::path vat = fs::path("tools") / "fixtures" / "vertex-animation" / "assets" / "models";
        fs::copy_file(repoFile(pose / "hero_animated.gltf"), models / "hero.gltf", fs::copy_options::overwrite_existing);
        fs::copy_file(repoFile(vat / "rigid_arm.gltf"), models / "rigid_arm.gltf", fs::copy_options::overwrite_existing);
        fs::copy_file(repoFile(vat / "morph_blob.gltf"), models / "morph_blob.gltf", fs::copy_options::overwrite_existing);
        for (const char* name : {"hero.gltf", "rigid_arm.gltf", "morph_blob.gltf"}) MeshSystem::invalidateModelFile((models / name).string());
        AnimationClipPool::clear();
    }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("VatWindow", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Entity loadModel(uint32_t sceneId, const std::string& assetsRelative, const std::string& name) {
        CreateEntityCmd* create = new CreateEntityCmd(project(), sceneId, name, EntityCreationType::MODEL);
        create->setQuiet(true);
        history(sceneId)->addCommandNoMerge(create);
        const Entity entity = create->getEntity();
        history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, entity, assetsRelative));
        EXPECT_FALSE(sceneOf(sceneId)->getComponent<ModelComponent>(entity).loadedFilename.empty()) << assetsRelative;
        return entity;
    }

    bool hasKind(const std::vector<VatIssue>& issues, const std::string& kind) {
        for (const VatIssue& issue : issues) {
            if (issue.kind == kind) return true;
        }
        return false;
    }

    const VatIssue* findIssue(const std::vector<VatIssue>& issues, const std::string& kind) {
        for (const VatIssue& issue : issues) {
            if (issue.kind == kind) return &issue;
        }
        return nullptr;
    }

    std::string kinds(const std::vector<VatIssue>& issues) {
        std::string out;
        for (const VatIssue& issue : issues) out += issue.kind + "(" + issue.message + ") ";
        return out;
    }

    // the entity named `name` below `root` (not the root itself)
    Entity findBelow(Scene* scene, Entity root, const std::string& name) {
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity entity = transforms->getEntity(i);
            if (entity == root || scene->getEntityName(entity) != name) continue;
            for (Entity parent = transforms->getComponentFromIndex(i).parent; parent != NULL_ENTITY;
                 parent = scene->getComponent<Transform>(parent).parent) {
                if (parent == root) return entity;
            }
        }
        return NULL_ENTITY;
    }

    std::vector<std::string> clipNames(const VertexAnimationBakerWindow& window) {
        std::vector<std::string> names;
        for (const auto& row : window.getClips()) names.push_back(row.name);
        return names;
    }

    // the window's per-frame tick until the bake or import ends (time-sliced: several slices)
    // (the encoding runs on a worker: the ticks wait for it, as the editor's frames do)
    int runToEnd(VertexAnimationBakerWindow& window, double budgetMs = 2.0) {
        int slices = 0;
        const auto start = std::chrono::steady_clock::now();
        while (window.isRunning() && std::chrono::steady_clock::now() - start < std::chrono::seconds(300)) {
            window.tick(budgetMs);
            slices++;
            if (Access::run(window) && Access::run(window)->state() == VatBakeState::Encoding) usleep(500);
        }
        window.tick(budgetMs);
        EXPECT_FALSE(window.isRunning()) << "the bake ended";
        return slices;
    }

    class VatBakerWindow_ : public ::testing::Test {
    protected:
        std::unique_ptr<VertexAnimationBakerWindow> window;

        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            resetAssets();
            window.reset(new VertexAnimationBakerWindow(project()));
        }
        void TearDown() override {
            window.reset();
        }
    };

}

// ==================================================================== sources

TEST_F(VatBakerWindow_, SourceDetectionListsClipsAndSummary) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    EXPECT_EQ(window->getMode(), VertexAnimationBakerWindow::Mode::Bake);
    EXPECT_EQ(window->getSpec().source.kind, VatSourceKind::Model);
    EXPECT_EQ(clipNames(*window), (std::vector<std::string>{"Swing", "Stretch"}));
    EXPECT_NE(window->getSummary().find("rigid_arm.gltf: "), std::string::npos) << window->getSummary();
    EXPECT_NE(window->getSummary().find("2 clips"), std::string::npos) << window->getSummary();
    EXPECT_FALSE(hasErrors(window->getIssues())) << kinds(window->getIssues());
    EXPECT_EQ(window->plannedOutput(), "assets/models/rigid_arm.vat");
    EXPECT_GT(window->getEstimate().width, 0);

    // an absolute path works as well; a file of another kind and a missing one are refused
    ASSERT_TRUE(window->setSourcePath((root() / "assets/models/morph_blob.gltf").string(), error)) << error;
    EXPECT_EQ(clipNames(*window), std::vector<std::string>{"Pulse"});
    writeAll(root() / "assets/notes.txt", "hello");
    EXPECT_FALSE(window->setSourcePath("assets/notes.txt", error));
    EXPECT_NE(error.find("not a model"), std::string::npos) << error;
    EXPECT_FALSE(window->setSourcePath("assets/models/missing.gltf", error));
    EXPECT_EQ(window->getSpec().source.path, "assets/models/morph_blob.gltf") << "a refused path keeps the source";

    // an .anim: played on its preview model, else the Play on field asks for one
    ClipData clip;
    clip.guid = "0123456789abcdef0123456789abcdef";
    clip.wrap = ClipWrapMode::Loop;
    ClipTrackData track;
    track.path = "Rig/Base/Arm";
    track.channel = ClipChannel::Rotation;
    for (int i = 0; i < 2; i++) {
        ClipKey key;
        key.time = (float)i;
        key.value = {0.0f, 0.0f, i ? 0.5f : 0.0f, i ? 0.866f : 1.0f};
        track.keys.push_back(key);
    }
    clip.tracks.push_back(track);
    anim::normalize(clip);
    writeAll(root() / "assets/anim/bend.anim", anim::writeClip(clip));
    ASSERT_TRUE(window->setSourcePath("assets/anim/bend.anim", error)) << error;
    EXPECT_EQ(window->getSpec().source.kind, VatSourceKind::Anim);
    EXPECT_TRUE(window->getSpec().source.path.empty());
    EXPECT_TRUE(hasKind(window->getIssues(), "invalid_source")) << kinds(window->getIssues());
    ASSERT_TRUE(window->setPlayOnPath("assets/models/rigid_arm.gltf", error)) << error;
    EXPECT_EQ(window->getSpec().source.path, "assets/models/rigid_arm.gltf");
    EXPECT_EQ(clipNames(*window), std::vector<std::string>{"bend"});
    EXPECT_EQ(window->plannedOutput(), "assets/anim/bend.vat");

    // a prefab source lists its root's animations (an Animation Player state here)
    const uint32_t sceneId = newScene();
    const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", "Robot");
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "models/rigid_arm.gltf#Swing";
    player.clips.push_back(entry);
    sceneOf(sceneId)->addComponent<AnimationPlayerComponent>(arm, player);
    ASSERT_TRUE(window->setSourceEntity(sceneId, arm, error)) << error;
    EXPECT_EQ(window->getSpec().source.kind, VatSourceKind::Entity);
    EXPECT_EQ(window->getSpec().source.sceneId, sceneId);
    // the player's state first, then the model's entity Animations (as the baker lists them)
    ASSERT_FALSE(clipNames(*window).empty());
    EXPECT_EQ(clipNames(*window)[0], "Swing");
    EXPECT_NE(Access::sourceLabel(*window).find(": Robot"), std::string::npos) << Access::sourceLabel(*window);
}

// ==================================================================== settings and estimate

TEST_F(VatBakerWindow_, SettingsChangeTheEstimate) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/morph_blob.gltf", error)) << error;
    const VatEstimate first = window->getEstimate();
    ASSERT_EQ(first.mode, jasperengine::vat::VatMode::Vertices) << first.modeReason;
    EXPECT_NE(first.modeReason.find("morph"), std::string::npos) << first.modeReason;
    ASSERT_EQ(first.clips.size(), 1u);

    VatBakeSpec spec = window->getSpec();
    spec.fps = 60.0f;
    window->setSpec(spec);
    window->refreshEstimate();
    EXPECT_GT(window->getEstimate().texels, first.texels) << "twice the samples";
    EXPECT_GT(window->getEstimate().clips[0].frames, first.clips[0].frames);

    spec = window->getSpec();
    spec.normals = false;
    window->setSpec(spec);
    window->refreshEstimate();
    EXPECT_LT(window->getEstimate().fetchesPerVertex, first.fetchesPerVertex);

    // a per-clip fps override and the checklist
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    const int bothSamples = window->getEstimate().samples;
    window->setCheckedClips({"Stretch"});
    window->refreshEstimate();
    EXPECT_EQ(window->getSpec().clips, std::vector<std::string>{"Stretch"});
    ASSERT_EQ(window->getEstimate().clips.size(), 1u);
    EXPECT_LT(window->getEstimate().samples, bothSamples);
    spec = window->getSpec();
    VatClipOverride override;
    override.name = "Stretch";
    override.fps = 10.0f;
    override.wrap = ClipWrapMode::Once;
    spec.clipOverrides = {override};
    window->setSpec(spec);
    window->refreshEstimate();
    EXPECT_FLOAT_EQ(window->getEstimate().clips[0].requestedFps, 10.0f);
    EXPECT_EQ(window->getEstimate().clips[0].wrap, ClipWrapMode::Once);
    // None: nothing to bake
    window->setCheckedClips({});
    window->refreshEstimate();
    EXPECT_TRUE(Access::noneChecked(*window));
    EXPECT_TRUE(hasErrors(window->getIssues()));
    EXPECT_FALSE(window->bake(true, error));
    EXPECT_NE(error.find("no clip"), std::string::npos) << error;
    window->setCheckedClips({"Swing", "Stretch"});
    window->refreshEstimate();
    EXPECT_TRUE(window->getSpec().clips.empty()) << "all checked is every clip";
    EXPECT_FALSE(hasErrors(window->getIssues())) << kinds(window->getIssues());
}

TEST_F(VatBakerWindow_, FixButtonsApplyToTheSettings) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/hero.gltf", error)) << error;
    VatBakeSpec spec = window->getSpec();
    spec.mode = VatModeChoice::Vertices;
    spec.fps = 240.0f;
    window->setSpec(spec);
    window->refreshEstimate();
    const VatIssue* tooLarge = findIssue(window->getIssues(), "too_large");
    ASSERT_TRUE(tooLarge) << kinds(window->getIssues());
    ASSERT_FALSE(tooLarge->fixes.empty());
    // every fix of too_large is one the window applies, and each one brings the bake under the limit
    const std::vector<VatFix> fixes = tooLarge->fixes;
    bool lowered = false;
    for (const VatFix& fix : fixes) {
        window->setSpec(spec);
        if (fix.setting == "clips") window->setCheckedClips(clipNames(*window));
        window->refreshEstimate();
        ASSERT_TRUE(window->applyFix(fix, error)) << fix.label << ": " << error;
        EXPECT_NE(window->getStatus().find(fix.label), std::string::npos);
        if (fix.setting == "fps") {
            lowered = true;
            EXPECT_LT(window->getSpec().fps, 240.0f);
            EXPECT_NE(fix.label.find("Lower to"), std::string::npos) << fix.label;
        }
        if (fix.setting == "preset") {
            EXPECT_EQ(window->getSpec().preset, VatPreset::Desktop);
        }
        if (fix.setting == "normals") {
            EXPECT_FALSE(window->getSpec().normals);
        }
        if (fix.setting == "mode") {
            EXPECT_EQ(window->getSpec().mode, VatModeChoice::Bones);
        }
        if (fix.setting == "fps" || fix.setting == "preset" || fix.setting == "mode") {
            EXPECT_FALSE(hasKind(window->getIssues(), "too_large")) << fix.label << ": " << kinds(window->getIssues());
        }
    }
    EXPECT_TRUE(lowered);

    // output_exists -> "Overwrite it"
    window->setSpec(spec);
    writeAll(root() / "assets/models/taken.vat", "{}");
    spec = window->getSpec();
    spec.fps = 30.0f;
    spec.mode = VatModeChoice::Auto;
    spec.output = "assets/models/taken.vat";
    window->setSpec(spec);
    window->refreshEstimate();
    const VatIssue* exists = findIssue(window->getIssues(), "output_exists");
    ASSERT_TRUE(exists) << kinds(window->getIssues());
    ASSERT_EQ(exists->fixes.size(), 1u);
    ASSERT_TRUE(window->applyFix(exists->fixes[0], error)) << error;
    EXPECT_TRUE(window->getSpec().overwrite);
    EXPECT_FALSE(hasKind(window->getIssues(), "output_exists")) << kinds(window->getIssues());

    // the bake's own fixes: interpolation off for a clip, precision
    ASSERT_TRUE(window->applyFix({"Interpolation off for Walk", "interpolate", "{\"clip\": \"Walk\", \"value\": false}"}, error)) << error;
    ASSERT_EQ(window->getSpec().clipOverrides.size(), 1u);
    EXPECT_EQ(window->getSpec().clipOverrides[0].name, "Walk");
    EXPECT_FALSE(window->getSpec().clipOverrides[0].interpolate);
    ASSERT_TRUE(window->applyFix({"Keep half precision", "precision", "\"half\""}, error)) << error;
    EXPECT_EQ(window->getSpec().precision, VatPrecisionChoice::Half);
    // what the window can't apply is said
    EXPECT_FALSE(window->applyFix({"Nonsense", "teleport", "true"}, error));
    EXPECT_FALSE(window->applyFix({"Broken", "fps", "not json"}, error));
}

// ==================================================================== baking

TEST_F(VatBakerWindow_, BakeRunsTimeSlicedAndWritesOneUndoStep) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    ASSERT_TRUE(window->bake(true, error)) << error;
    EXPECT_TRUE(window->isRunning());
    EXPECT_NE(window->stageLine().find("Stage 1 of 2: Posing clip 1 of 2"), std::string::npos) << window->stageLine();
    const int slices = runToEnd(*window, 0.01);
    EXPECT_GT(slices, 2) << "the bake ran over several frames";
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    const VatWriteResult& written = window->lastWrite();
    ASSERT_FALSE(written.vat.empty()) << window->getStatus();
    EXPECT_EQ(written.vat, "assets/models/rigid_arm.vat");
    EXPECT_TRUE(fs::exists(root() / written.vat));
    EXPECT_TRUE(fs::exists(root() / written.mesh));
    EXPECT_NE(window->getStatus().find("Baked 'assets/models/rigid_arm.vat' (Vertices, 2 clips: Swing, Stretch"), std::string::npos) << window->getStatus();
    EXPECT_NE(window->getStatus().find(":getVertexAnimator():play(\"Swing\")"), std::string::npos) << window->getStatus();
    // the recipe holds the window's settings
    const jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(readAll(root() / written.vat));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->recipe.sources[0].kind, "model");
    EXPECT_EQ(parsed.data->recipe.sources[0].path, "assets/models/rigid_arm.gltf");
    // the default output now names this bake: baking again rewrites it in place
    window->refreshEstimate();
    EXPECT_EQ(window->plannedOutput(), "assets/models/rigid_arm.vat");
    // one project undo step takes both files back
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(fs::exists(root() / written.vat));
    EXPECT_FALSE(fs::exists(root() / written.mesh));
}

TEST_F(VatBakerWindow_, CancelWritesNothing) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/hero.gltf", error)) << error;
    ASSERT_TRUE(window->bake(true, error)) << error;
    window->tick(0.01);
    ASSERT_TRUE(window->isRunning());
    window->cancel();
    window->tick(0.01);
    EXPECT_FALSE(window->isRunning());
    EXPECT_EQ(window->getStatus(), "Cancelled: nothing was written.");
    EXPECT_FALSE(fs::exists(root() / "assets/models/hero.vat"));
    EXPECT_FALSE(fs::exists(root() / "assets/models/hero.vat.glb"));
    EXPECT_EQ(Access::run(*window)->state(), VatBakeState::Cancelled);
}

TEST_F(VatBakerWindow_, PreviewRunThenBakeWritesWithoutBakingAgain) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    ASSERT_TRUE(window->bake(false, error)) << error;
    runToEnd(*window);
    VatBakeSession* first = Access::run(*window);
    ASSERT_TRUE(first && first->result());
    EXPECT_FALSE(fs::exists(root() / "assets/models/rigid_arm.vat"));
    EXPECT_NE(window->getStatus().find("Baked in memory"), std::string::npos) << window->getStatus();
    EXPECT_FALSE(window->isStale());

    // the preview: the source model posed by its clip, the VAT on the in-memory asset
    Access::preview(*window, 0.0f);
    VatPreviewView* source = Access::sourceView(*window);
    VatPreviewView* vat = Access::vatView(*window);
    ASSERT_TRUE(source && vat);
    EXPECT_TRUE(source->hasSubject()) << Access::previewNote(*window);
    EXPECT_TRUE(vat->hasSubject()) << Access::previewNote(*window);
    EXPECT_TRUE(vat->isVat());
    EXPECT_EQ(source->getFile(), "assets/models/rigid_arm.gltf");
    EXPECT_TRUE(vat->vatError().empty()) << vat->vatError();
    Scene* posed = source->getScene();
    const Entity armNode = findBelow(posed, source->getRoot(), "Arm");
    ASSERT_NE(armNode, NULL_ENTITY);
    const Quaternion atStart = posed->getComponent<Transform>(armNode).rotation;
    Access::previewPlaying(*window) = true;
    Access::preview(*window, 0.25f);
    EXPECT_NEAR(Access::previewTime(*window), 0.25f, 1e-4f);
    const Quaternion later = posed->getComponent<Transform>(armNode).rotation;
    EXPECT_FALSE(atStart == later) << "the source view follows the playhead";

    // Bake with the same settings writes that result
    ASSERT_TRUE(window->bake(true, error)) << error;
    EXPECT_FALSE(window->isRunning());
    EXPECT_EQ(Access::run(*window), first) << "no second bake";
    EXPECT_TRUE(fs::exists(root() / "assets/models/rigid_arm.vat"));
    // another setting: the result is stale, Bake bakes again
    VatBakeSpec spec = window->getSpec();
    spec.fps = 20.0f;
    window->setSpec(spec);
    EXPECT_TRUE(window->isStale());
}

TEST_F(VatBakerWindow_, HeatmapSwitchesThePreviewMeshAndBack) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    VatBakeSpec spec = window->getSpec();
    spec.fps = 8.0f;
    window->setSpec(spec);
    ASSERT_TRUE(window->bake(false, error)) << error;
    runToEnd(*window);
    const VatBakeResult* result = Access::run(*window)->result();
    ASSERT_TRUE(result);
    ASSERT_EQ(result->vertexErrorMm.size(), result->mesh.vertexCount());
    EXPECT_FLOAT_EQ(*std::max_element(result->vertexErrorMm.begin(), result->vertexErrorMm.end()), result->report.maxErrorMm);
    EXPECT_TRUE(window->heatmapAvailable());

    Access::preview(*window, 0.0f);
    VatPreviewView* vat = Access::vatView(*window);
    ASSERT_TRUE(vat && vat->hasSubject());
    const std::string plain = vat->getFile();
    EXPECT_FALSE(vat->isUnlit());

    // on: the VAT side loads the heat-coloured preview mesh, unlit; the asset is untouched
    window->setHeatmap(true);
    EXPECT_TRUE(window->isHeatmap());
    ASSERT_TRUE(vat->hasSubject()) << Access::previewNote(*window);
    EXPECT_NE(vat->getFile(), plain);
    EXPECT_NE(vat->getFile().find("-heatmap"), std::string::npos) << vat->getFile();
    EXPECT_TRUE(vat->isUnlit());
    const MeshComponent& heatMesh = vat->getScene()->getComponent<MeshComponent>(vat->getRoot());
    EXPECT_FALSE(heatMesh.receiveLights);
    ASSERT_GT(heatMesh.numSubmeshes, 0u);
    EXPECT_TRUE(heatMesh.submeshes[0].hasVertexColor4) << "the error colours";
    EXPECT_TRUE(vat->vatError().empty()) << vat->vatError();
    Access::preview(*window, 0.1f);
    EXPECT_FALSE(fs::exists(root() / "assets/models/rigid_arm.vat")) << "a preview writes no asset";

    // off: back to the plain mesh
    window->setHeatmap(false);
    EXPECT_EQ(vat->getFile(), plain);
    EXPECT_FALSE(vat->isUnlit());
    EXPECT_TRUE(vat->getScene()->getComponent<MeshComponent>(vat->getRoot()).receiveLights);
    // on again, and a new result starts plain until the toggle applies to it
    window->setHeatmap(true);
    EXPECT_TRUE(vat->isUnlit());

    // imports carry no per-vertex error
    const fs::path exportDir = vatimportfixtures::freshDir("window-heat");
    vatimportfixtures::writeSoft(exportDir, "Wave", vatimportfixtures::SoftOptions());
    ASSERT_TRUE(window->setSourcePath(exportDir.string(), error)) << error;
    EXPECT_FALSE(window->heatmapAvailable());
}

TEST_F(VatBakerWindow_, ReplaceFilesIsAskedThenWrittenOver) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    writeAll(root() / "assets/models/arm.vat", "{}");
    VatBakeSpec spec = window->getSpec();
    spec.output = "assets/models/arm.vat";
    window->setSpec(spec);
    // the editor host's "Replace Files?" (the default host answers yes)
    ASSERT_TRUE(window->bake(true, error)) << error;
    EXPECT_TRUE(window->getSpec().overwrite);
    runToEnd(*window);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    EXPECT_NE(readAll(root() / "assets/models/arm.vat"), "{}");
}

TEST_F(VatBakerWindow_, RebakeFromAVatKeepsItsGuidAndEdits) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    ASSERT_TRUE(window->bake(true, error)) << error;
    runToEnd(*window);
    const VatWriteResult first = window->lastWrite();
    ASSERT_FALSE(first.vat.empty()) << window->getStatus() << " running " << window->isRunning();

    // a hand edit, then the source changes
    const fs::path vatFile = root() / first.vat;
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(readAll(vatFile));
    ASSERT_TRUE(parsed.ok());
    jasperengine::vat::VatData edited = *parsed.data;
    edited.clips[0].speed = 1.75f;
    writeAll(vatFile, jasperengine::vat::writeVat(edited));
    writeAll(root() / "assets/models/rigid_arm.gltf", readAll(root() / "assets/models/rigid_arm.gltf") + "\n");
    MeshSystem::invalidateModelFile((root() / "assets/models/rigid_arm.gltf").string());

    // "Open in Baker" / double-click: the recipe, Rebake, the staleness note
    VertexAnimationBakerWindow other(project());
    ASSERT_TRUE(other.openVat(first.vat, error)) << error;
    EXPECT_TRUE(other.isOpen());
    EXPECT_EQ(other.getMode(), VertexAnimationBakerWindow::Mode::Bake);
    EXPECT_EQ(Access::rebakePath(other), first.vat);
    EXPECT_EQ(other.getSpec().output, first.vat);
    EXPECT_TRUE(other.getSpec().overwrite);
    EXPECT_EQ(other.getSpec().source.path, "assets/models/rigid_arm.gltf");
    bool staleShown = false;
    for (const std::string& note : Access::notes(other)) staleShown |= note.find("Source changed since baking") != std::string::npos;
    EXPECT_TRUE(staleShown);
    ASSERT_TRUE(other.bake(true, error)) << error;
    runToEnd(other);
    ASSERT_FALSE(other.statusIsError()) << other.getStatus();
    EXPECT_EQ(other.lastWrite().vat, first.vat);
    EXPECT_TRUE(other.lastWrite().rewritten);
    EXPECT_EQ(other.lastWrite().guid, first.guid);
    parsed = jasperengine::vat::parseVat(readAll(vatFile));
    ASSERT_TRUE(parsed.ok());
    EXPECT_FLOAT_EQ(parsed.data->clips[0].speed, 1.75f) << "the hand edit stays";
    EXPECT_TRUE(checkVatStaleness(project(), first.vat).empty());
    // opened again: up to date
    ASSERT_TRUE(other.openVat(first.vat, error)) << error;
    ASSERT_FALSE(Access::notes(other).empty());
    EXPECT_NE(Access::notes(other).back().find("up to date"), std::string::npos) << Access::notes(other).back();
    // a .vat given as the source does the same
    ASSERT_TRUE(window->setSourcePath(first.vat, error)) << error;
    EXPECT_EQ(Access::rebakePath(*window), first.vat);
}

TEST_F(VatBakerWindow_, PlaceCrowdBakesACrowd) {
    const uint32_t sceneId = newScene();
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    VatBakeSpec spec = window->getSpec();
    spec.place = VatPlace::Crowd;
    spec.crowd.count = 40;
    spec.crowd.area = 8.0f;
    spec.crowd.seed = 2;
    window->setSpec(spec);
    window->refreshEstimate();
    EXPECT_FALSE(hasErrors(window->getIssues())) << kinds(window->getIssues());
    ASSERT_TRUE(window->bake(true, error)) << error;
    runToEnd(*window);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    const VatWriteResult& written = window->lastWrite();
    ASSERT_NE(written.entity, NULL_ENTITY);
    EXPECT_EQ(written.sceneId, sceneId);
    const InstancedMeshComponent* instanced = sceneOf(sceneId)->findComponent<InstancedMeshComponent>(written.entity);
    ASSERT_TRUE(instanced);
    EXPECT_EQ(instanced->instances.size(), 40u);
    EXPECT_NE(window->getStatus().find("placed in the scene"), std::string::npos) << window->getStatus();
    // a count the crowd can't hold is a validation row
    spec = window->getSpec();
    spec.crowd.count = 0;
    window->setSpec(spec);
    window->refreshEstimate();
    EXPECT_TRUE(hasKind(window->getIssues(), "invalid_settings")) << kinds(window->getIssues());
}

TEST_F(VatBakerWindow_, PlayingSceneRefusesTheBake) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    const uint32_t sceneId = newScene();
    SceneProject* sceneProject = project()->getScene(sceneId);
    sceneProject->playState = ScenePlayState::PLAYING;
    EXPECT_FALSE(window->bake(true, error));
    EXPECT_NE(error.find("Stop the scene"), std::string::npos) << error;
    sceneProject->playState = ScenePlayState::STOPPED;
    EXPECT_TRUE(window->bake(false, error)) << error;
    runToEnd(*window);
}

// ==================================================================== the Inspector's entries

TEST_F(VatBakerWindow_, BakeEntriesPrefillTheWindow) {
    ASSERT_TRUE(vatBakerHooks().bake) << "the window installs the hook";
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", "Robot");
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "models/rigid_arm.gltf#Stretch";
    player.clips.push_back(entry);
    scene->addComponent<AnimationPlayerComponent>(arm, player);

    // a clip row of the model asset view
    VatBakeEntry modelClip;
    modelClip.kind = VatBakeEntry::Kind::ModelClip;
    modelClip.asset = (root() / "assets/models/rigid_arm.gltf").string();
    modelClip.clip = "Stretch";
    vatBakerHooks().bake(project(), modelClip, false);
    EXPECT_TRUE(window->isOpen());
    EXPECT_EQ(window->getSpec().source.kind, VatSourceKind::Model);
    EXPECT_EQ(window->getSpec().source.path, "assets/models/rigid_arm.gltf");
    EXPECT_EQ(window->getSpec().clips, std::vector<std::string>{"Stretch"});
    EXPECT_FALSE(window->isRunning());

    // the Animation Player section
    VatBakeEntry playerEntry;
    playerEntry.kind = VatBakeEntry::Kind::AnimationPlayer;
    playerEntry.sceneId = sceneId;
    playerEntry.entity = arm;
    vatBakerHooks().bake(project(), playerEntry, false);
    EXPECT_EQ(window->getSpec().source.kind, VatSourceKind::Entity);
    EXPECT_EQ(window->getSpec().source.path, "Robot");
    ASSERT_FALSE(clipNames(*window).empty());
    EXPECT_EQ(clipNames(*window)[0], "Stretch");

    // the Lattice Modifier section of a child: the nearest animated GameObject above it
    const Entity forearm = findBelow(scene, arm, "Arm");
    ASSERT_NE(forearm, NULL_ENTITY);
    scene->addComponent<LatticeModifierComponent>(forearm, LatticeModifierComponent());
    VatBakeEntry latticeEntry;
    latticeEntry.kind = VatBakeEntry::Kind::LatticeModifier;
    latticeEntry.sceneId = sceneId;
    latticeEntry.entity = forearm;
    vatBakerHooks().bake(project(), latticeEntry, false);
    EXPECT_EQ(window->getSpec().source.kind, VatSourceKind::Entity);
    EXPECT_EQ(window->getSpec().source.path, "Robot");

    // the Pose Animator section: its pose animation on the GameObject
    VatBakeEntry poseEntry;
    poseEntry.kind = VatBakeEntry::Kind::PoseAnimator;
    poseEntry.sceneId = sceneId;
    poseEntry.entity = arm;
    poseEntry.asset = "anim/raise.poseanim";
    vatBakerHooks().bake(project(), poseEntry, false);
    EXPECT_EQ(window->getSpec().source.kind, VatSourceKind::Pose);
    EXPECT_EQ(window->getSpec().source.path, "Robot");
    EXPECT_EQ(window->getSpec().source.sceneId, sceneId);
    EXPECT_EQ(window->getSpec().source.anims, std::vector<std::string>{"assets/anim/raise.poseanim"});

    // Shift-click: bakes now with the last settings, placed beside the source (one scene step)
    VatBakeSpec spec = window->getSpec();
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/morph_blob.gltf", error)) << error;
    spec = window->getSpec();
    spec.fps = 12.0f;
    spec.normals = false;
    window->setSpec(spec);
    ASSERT_TRUE(window->bake(false, error)) << error;
    runToEnd(*window);
    vatBakerHooks().bake(project(), playerEntry, true);
    EXPECT_TRUE(window->isRunning());
    EXPECT_EQ(window->getSpec().place, VatPlace::Beside);
    EXPECT_FLOAT_EQ(window->getSpec().fps, 12.0f) << "the last-used settings";
    EXPECT_FALSE(window->getSpec().normals);
    runToEnd(*window);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    const VatWriteResult& written = window->lastWrite();
    ASSERT_NE(written.entity, NULL_ENTITY);
    EXPECT_EQ(written.sceneId, sceneId);
    EXPECT_EQ(scene->getEntityName(written.entity), "Robot (VAT)");
    ASSERT_TRUE(scene->findComponent<VertexAnimatorComponent>(written.entity));
    // one scene undo step: the GameObject and the files
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(fs::exists(root() / written.vat));
}

TEST_F(VatBakerWindow_, DeletedSourceNeverWritesTheStoredResult) {
    // the Inspector's Shift-click bake places "Robot (VAT)"; Robot is deleted; Bake (the stored
    // result's settings) used to write it, naming the gone GameObject: a crash
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", "Robot");
    AnimationPlayerComponent player;
    AnimationPlayerClip clip;
    clip.clip = "models/rigid_arm.gltf#Swing";
    player.clips.push_back(clip);
    scene->addComponent<AnimationPlayerComponent>(arm, player);
    VatBakeEntry entry;
    entry.kind = VatBakeEntry::Kind::AnimationPlayer;
    entry.sceneId = sceneId;
    entry.entity = arm;
    vatBakerHooks().bake(project(), entry, true);
    runToEnd(*window);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    ASSERT_NE(window->lastWrite().entity, NULL_ENTITY);
    const std::string firstVat = window->lastWrite().vat;
    ASSERT_TRUE(Access::run(*window) && Access::run(*window)->state() == VatBakeState::Done);

    history(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, arm));
    ASSERT_FALSE(scene->isEntityCreated(arm));
    std::string error;
    EXPECT_FALSE(window->bake(true, error)) << "nothing to bake: the source is gone";
    EXPECT_NE(error.find("Robot"), std::string::npos) << error;
    EXPECT_TRUE(window->statusIsError()) << window->getStatus();
    EXPECT_FALSE(window->isRunning());
    VatBakeSession* after = Access::run(*window);
    EXPECT_TRUE(!after || after->state() != VatBakeState::Done) << "no stored result names the gone GameObject";

    // a preview run, then the source deleted: the next ticks drop the stored result
    const Entity back = loadModel(sceneId, "models/rigid_arm.gltf", "Robot");
    ASSERT_TRUE(window->setSourceEntity(sceneId, back, error)) << error;
    ASSERT_TRUE(window->bake(false, error)) << error;
    runToEnd(*window);
    ASSERT_TRUE(Access::run(*window) && Access::run(*window)->state() == VatBakeState::Done) << window->getStatus();
    history(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, back));
    Access::checkSourceNow(*window);
    window->tick(1.0);
    EXPECT_EQ(Access::run(*window), nullptr) << "the stored result is dropped";
    EXPECT_TRUE(window->statusIsError());
    EXPECT_NE(window->getStatus().find("The baked result was dropped: the GameObject 'Robot' is gone"), std::string::npos) << window->getStatus();
    EXPECT_FALSE(window->bake(true, error));

    // what a write throws ends in the window's error (with the Console line), never in a crash
    const Entity again = loadModel(sceneId, "models/rigid_arm.gltf", "Robot");
    ASSERT_TRUE(window->setSourceEntity(sceneId, again, error)) << error;
    ASSERT_TRUE(window->bake(false, error)) << error << " / " << window->getStatus();
    runToEnd(*window);
    ASSERT_TRUE(Access::run(*window) && Access::run(*window)->state() == VatBakeState::Done);
    history(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, again));
    Access::writeStored(*window);
    EXPECT_TRUE(window->statusIsError()) << window->getStatus();
    EXPECT_FALSE(window->isRunning());
    (void)firstVat;
}

TEST_F(VatBakerWindow_, ASourceStillLoadingIsProbedAgainOnceItsMeshesAreBuilt) {
    // a GameObject brought back by undo builds its meshes in the next MeshSystem update: the
    // window says it is loading (not "no POSITION"), then lists its clips with no new pick
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", "Robot");
    history(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, arm));
    ASSERT_TRUE(history(sceneId)->undo());
    ASSERT_TRUE(scene->isEntityCreated(arm));
    ASSERT_TRUE(scene->getComponent<ModelComponent>(arm).needUpdateModel);
    std::string error;
    ASSERT_TRUE(window->setSourceEntity(sceneId, arm, error)) << error;
    auto has = [&](const std::string& kind) {
        for (const VatIssue& issue : window->getIssues()) {
            if (issue.kind == kind) return true;
        }
        return false;
    };
    EXPECT_TRUE(has("source_loading"));
    EXPECT_FALSE(has("invalid_source"));
    EXPECT_FALSE(window->bake(false, error)) << "not before it has loaded";
    // still loading: a tick changes nothing
    Access::checkSourceNow(*window);
    window->tick(1.0);
    EXPECT_TRUE(has("source_loading"));

    scene->getSystem<MeshSystem>()->update(0);
    Access::checkSourceNow(*window);
    window->tick(1.0);
    EXPECT_FALSE(has("source_loading"));
    for (const VatIssue& issue : window->getIssues()) EXPECT_FALSE(issue.error) << issue.kind << ": " << issue.message;
    ASSERT_TRUE(window->bake(false, error)) << error << " / " << window->getStatus();
    runToEnd(*window);
    ASSERT_TRUE(Access::run(*window) && Access::run(*window)->state() == VatBakeState::Done) << window->getStatus();
}

TEST_F(VatBakerWindow_, ClosingTheSourcesSceneResetsTheWindow) {
    const uint32_t sceneId = newScene();
    const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", "Robot");
    std::string error;
    ASSERT_TRUE(window->setSourceEntity(sceneId, arm, error)) << error;
    ASSERT_TRUE(window->bake(false, error)) << error;
    runToEnd(*window);
    ASSERT_TRUE(Access::run(*window) && Access::run(*window)->result());
    project()->getScene(sceneId)->isModified = false;
    project()->closeScene(sceneId, true);
    ASSERT_TRUE(!project()->getScene(sceneId) || !project()->getScene(sceneId)->scene);
    Access::checkSourceNow(*window);
    window->tick(1.0);
    EXPECT_EQ(window->getMode(), VertexAnimationBakerWindow::Mode::None);
    EXPECT_EQ(Access::run(*window), nullptr);
    EXPECT_TRUE(window->getSpec().source.path.empty());
    EXPECT_NE(window->getStatus().find("was closed"), std::string::npos) << window->getStatus();
    EXPECT_FALSE(window->bake(true, error));
    EXPECT_NE(error.find("choose a source"), std::string::npos) << error;
}

namespace {
    // an editor host that records the confirm alerts and answers no (the user hasn't clicked yet)
    struct AskingHost : public EditorHost {
        std::vector<std::string> asked;
        std::function<void()> yes;
        void registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()>) override {
            asked.push_back(title + ": " + message);
            yes = onYes;
        }
    };
}

TEST_F(VatBakerWindow_, OpeningAnotherFileAsksBeforeDroppingAnUnwrittenResult) {
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    ASSERT_TRUE(window->bake(false, error)) << error;
    runToEnd(*window);
    ASSERT_TRUE(window->holdsUnsavedResult());
    AskingHost host;
    setEditorHost(&host);
    // the Project window's double-click (a .vat or a model): asked first, nothing replaced yet
    vatAppHooks().openBaker(project(), root() / "assets/models/morph_blob.gltf", VatBakerSource::Bake);
    ASSERT_EQ(host.asked.size(), 1u);
    EXPECT_NE(host.asked[0].find("Replace the Baker's Result?"), std::string::npos) << host.asked[0];
    EXPECT_NE(host.asked[0].find("morph_blob.gltf"), std::string::npos) << host.asked[0];
    EXPECT_EQ(window->getSpec().source.path, "assets/models/rigid_arm.gltf") << "kept until the user says yes";
    ASSERT_TRUE(Access::run(*window) && Access::run(*window)->state() == VatBakeState::Done);
    // yes: the other file opens
    host.yes();
    EXPECT_EQ(window->getSpec().source.path, "assets/models/morph_blob.gltf");
    // no unwritten result: no question
    EXPECT_FALSE(window->holdsUnsavedResult());
    vatAppHooks().openBaker(project(), root() / "assets/models/rigid_arm.gltf", VatBakerSource::Bake);
    EXPECT_EQ(host.asked.size(), 1u);
    EXPECT_EQ(window->getSpec().source.path, "assets/models/rigid_arm.gltf");
    // a written result is no reason to ask either
    ASSERT_TRUE(window->bake(true, error)) << error;
    runToEnd(*window);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    EXPECT_FALSE(window->holdsUnsavedResult());
    setEditorHost(nullptr);
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
}

TEST_F(VatBakerWindow_, CrowdSeedIsNeverNegative) {
    Access::crowd(*window, 10, 5.0f, -7);
    EXPECT_EQ(Access::crowdSeed(*window), 0u);
    Access::crowd(*window, 10, 5.0f, 42);
    EXPECT_EQ(Access::crowdSeed(*window), 42u);
    // the window's import Upsample allows what the CLI and the MCP tool allow
    EXPECT_EQ(VAT_IMPORT_MAX_UPSAMPLE, 1024);
}

TEST_F(VatBakerWindow_, AppHooksOpenTheBakerAndRebake) {
    ASSERT_TRUE(vatAppHooks().openBaker && vatAppHooks().rebake) << "the window installs the .vat view's and Project window's hooks";
    // "Bake Vertex Animation..." on a model
    vatAppHooks().openBaker(project(), root() / "assets/models/rigid_arm.gltf", VatBakerSource::Bake);
    EXPECT_TRUE(window->isOpen());
    EXPECT_EQ(window->getSpec().source.path, "assets/models/rigid_arm.gltf");
    std::string error;
    ASSERT_TRUE(window->bake(true, error)) << error;
    runToEnd(*window);
    const VatWriteResult first = window->lastWrite();
    ASSERT_FALSE(first.vat.empty()) << window->getStatus();

    // "Open in Baker" / double-click on the .vat: its recipe, Rebake
    window->setOpen(false);
    vatAppHooks().openBaker(project(), root() / first.vat, VatBakerSource::VatAsset);
    EXPECT_TRUE(window->isOpen());
    EXPECT_EQ(Access::rebakePath(*window), first.vat);

    // the .vat view's Rebake: runs in the window (progress), rewrites the pair keeping its GUID
    vatAppHooks().rebake(project(), root() / first.vat);
    EXPECT_TRUE(window->isRunning());
    runToEnd(*window);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    EXPECT_EQ(window->lastWrite().vat, first.vat);
    EXPECT_TRUE(window->lastWrite().rewritten);
    EXPECT_EQ(window->lastWrite().guid, first.guid);

    // "Import as Vertex Animation..." on an export
    const fs::path exportDir = vatimportfixtures::freshDir("window-hook");
    vatimportfixtures::writeSoft(exportDir, "Flag", vatimportfixtures::SoftOptions());
    vatAppHooks().openBaker(project(), exportDir / "geo" / "Flag_mesh.fbx", VatBakerSource::Import);
    EXPECT_EQ(window->getMode(), VertexAnimationBakerWindow::Mode::Import);
    EXPECT_EQ(Access::detection(*window).assetName, "Flag");

    // a second window (a test's) answers the hooks while it lives, then the first one again
    {
        VertexAnimationBakerWindow other(project());
        vatAppHooks().openBaker(project(), root() / "assets/models/morph_blob.gltf", VatBakerSource::Bake);
        EXPECT_EQ(other.getSpec().source.path, "assets/models/morph_blob.gltf");
    }
    ASSERT_TRUE(vatAppHooks().openBaker);
    vatAppHooks().openBaker(project(), root() / "assets/models/morph_blob.gltf", VatBakerSource::Bake);
    EXPECT_EQ(window->getSpec().source.path, "assets/models/morph_blob.gltf");
}

// ==================================================================== import

TEST_F(VatBakerWindow_, InspectorRebakeOfAnImportRunsOnAWorker) {
    // the Inspector's Rebake (the app hook) of an imported .vat: the import runs on the window's
    // worker with its stage line (the SideFX destruction takes seconds), never on the main thread
    const fs::path exportDir = vatimportfixtures::freshDir("window-rebake-import");
    vatimportfixtures::writeSoft(exportDir, "Wave", vatimportfixtures::SoftOptions());
    std::string error;
    ASSERT_TRUE(window->setSourcePath(exportDir.string(), error)) << error;
    window->importOutput().output = "assets/RebakeWave.vat";
    ASSERT_TRUE(window->bake(true, error)) << error;
    window->tick(0.0);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    const VatWriteResult first = window->lastWrite();
    ASSERT_EQ(first.vat, "assets/RebakeWave.vat");

    ASSERT_TRUE(vatAppHooks().rebake);
    window->setOpen(false);
    vatAppHooks().rebake(project(), root() / first.vat);
    EXPECT_TRUE(window->isOpen()) << "the progress shows";
    EXPECT_TRUE(window->isRunning()) << "the import is on the worker when the hook returns";
    EXPECT_EQ(window->stageLine(), "Stage 1 of 1: Importing Wave");
    window->tick(0.0);
    EXPECT_FALSE(window->isRunning());
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    EXPECT_EQ(window->lastWrite().vat, first.vat);
    EXPECT_EQ(window->lastWrite().guid, first.guid) << "written over, GUID kept";

    // a failing import (the export's textures gone) ends in the window's error, nothing written
    const std::string before = readAll(root() / first.vat);
    for (const auto& file : fs::recursive_directory_iterator(exportDir)) {
        if (file.path().extension() == ".exr") fs::remove(file.path());
    }
    vatAppHooks().rebake(project(), root() / first.vat);
    window->tick(0.0);
    EXPECT_FALSE(window->isRunning());
    EXPECT_TRUE(window->statusIsError()) << window->getStatus();
    EXPECT_EQ(readAll(root() / first.vat), before);
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
}

TEST_F(VatBakerWindow_, ImportsAHoudiniExport) {
    // a synthetic soft-body export (the importer tests' writer), outside the assets folder
    const fs::path exportDir = vatimportfixtures::freshDir("window-soft");
    vatimportfixtures::SoftOptions options;
    vatimportfixtures::writeSoft(exportDir, "Wave", options);
    std::string error;
    ASSERT_TRUE(window->setSourcePath(exportDir.string(), error)) << error;
    EXPECT_EQ(window->getMode(), VertexAnimationBakerWindow::Mode::Import);
    const VatImportDetection& detection = Access::detection(*window);
    EXPECT_EQ(detection.kind, VatImportKind::HoudiniVat3);
    EXPECT_EQ(detection.mode, HoudiniMode::Soft);
    EXPECT_EQ(detection.assetName, "Wave");
    EXPECT_FALSE(window->getSummary().empty());
    EXPECT_EQ(window->importSettings().clipName, "Wave");

    // the options go to the importer (fps overrides the export's)
    window->importSettings().fps = 12.0f;
    window->importSettings().precision = "float";
    ASSERT_TRUE(window->bake(true, error)) << error;
    EXPECT_TRUE(window->isRunning());
    EXPECT_EQ(window->stageLine(), "Stage 1 of 1: Importing Wave");
    window->tick(0.0);
    EXPECT_FALSE(window->isRunning());
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    const VatWriteResult written = window->lastWrite();
    EXPECT_EQ(written.vat, "assets/Wave.vat");
    EXPECT_TRUE(fs::exists(root() / written.vat));
    EXPECT_TRUE(fs::exists(root() / written.mesh));
    EXPECT_NE(window->getStatus().find("Imported 'assets/Wave.vat' (Vertices"), std::string::npos) << window->getStatus();
    ASSERT_TRUE(window->lastImport() && window->lastImport()->ok());
    EXPECT_FLOAT_EQ(window->lastImport()->data.clips[0].fps, 12.0f);
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(readAll(root() / written.vat));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->recipe.sources[0].kind, "houdini_vat3");

    // the preview shows the imported VAT (no source view)
    Access::preview(*window, 0.0f);
    ASSERT_TRUE(Access::vatView(*window));
    EXPECT_TRUE(Access::vatView(*window)->hasSubject());
    EXPECT_FALSE(Access::sourceView(*window)->hasSubject());
    EXPECT_NE(Access::previewNote(*window).find("Houdini"), std::string::npos);

    // Re-import from the .vat keeps the GUID (the same export)
    const std::string importedBytes = readAll(root() / written.vat);
    VertexAnimationBakerWindow other(project());
    ASSERT_TRUE(other.openVat(written.vat, error)) << error;
    EXPECT_EQ(other.getMode(), VertexAnimationBakerWindow::Mode::Import);
    EXPECT_EQ(other.importOutput().output, written.vat);
    ASSERT_TRUE(other.bake(true, error)) << error;
    other.tick(0.0);
    ASSERT_FALSE(other.statusIsError()) << other.getStatus();
    EXPECT_EQ(other.lastWrite().guid, written.guid);

    // the re-import of the same export writes the same bytes: a step that changed nothing is
    // not kept (S24, CommandHistory drops a no-op), so one project undo step takes back the import
    EXPECT_EQ(readAll(root() / written.vat), importedBytes);
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(fs::exists(root() / written.vat));

    // placed as a crowd (the window's Place: Crowd... count, area, seed) in the selected scene
    const uint32_t sceneId = newScene();
    window->importOutput().place = VatPlace::Crowd;
    window->importOutput().output = "assets/WaveCrowd.vat";
    VatBakerWindowTestAccess::crowd(*window, 12, 4.0f, 5);
    ASSERT_TRUE(window->bake(true, error)) << error;
    window->tick(0.0);
    ASSERT_FALSE(window->statusIsError()) << window->getStatus();
    ASSERT_NE(window->lastWrite().entity, NULL_ENTITY);
    EXPECT_EQ(window->lastWrite().sceneId, sceneId);
    const InstancedMeshComponent* instanced = sceneOf(sceneId)->findComponent<InstancedMeshComponent>(window->lastWrite().entity);
    ASSERT_TRUE(instanced);
    EXPECT_EQ(instanced->instances.size(), 12u);
    EXPECT_EQ(sceneOf(sceneId)->getEntityName(window->lastWrite().entity), "Wave (VAT)");
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(fs::exists(root() / "assets/WaveCrowd.vat"));
    window->importOutput().place = VatPlace::None;
    window->importOutput().output.clear();

    // a cancelled import writes nothing
    ASSERT_TRUE(window->bake(true, error)) << error;
    window->cancel();
    EXPECT_FALSE(window->isRunning());
    for (int i = 0; i < 200; i++) {
        window->tick(1.0);
        usleep(5000);
    }
    EXPECT_FALSE(fs::exists(root() / written.vat));
    EXPECT_EQ(window->getStatus(), "Import cancelled: nothing was written.");
}

// ==================================================================== drawn

TEST_F(VatBakerWindow_, EverySectionDrawsInAFrame) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 900);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&]() {
        ImGui::NewFrame();
        window->show();
        ImGui::EndFrame();
    };
    window->open();
    frame();
    std::string error;
    ASSERT_TRUE(window->setSourcePath("assets/models/rigid_arm.gltf", error)) << error;
    frame();
    ASSERT_TRUE(window->bake(false, error)) << error;
    for (int i = 0; i < 400 && window->isRunning(); i++) frame();
    EXPECT_FALSE(window->isRunning()) << "the window's frames advance the bake";
    frame();
    frame();
    EXPECT_TRUE(Access::vatView(*window) && Access::vatView(*window)->hasSubject()) << "the Preview section loaded the result";
    VatBakeSpec spec = window->getSpec();
    spec.place = VatPlace::Crowd;
    window->setSpec(spec);
    frame();
    const fs::path exportDir = vatimportfixtures::freshDir("window-draw");
    vatimportfixtures::writeSoft(exportDir, "Wave", vatimportfixtures::SoftOptions());
    ASSERT_TRUE(window->setSourcePath(exportDir.string(), error)) << error;
    frame();
    window->setOpen(false);
    frame();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}
