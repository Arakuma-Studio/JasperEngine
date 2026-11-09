// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pixel-art check's input and fixes on a headless project (docs/specs/S42-pixel-art-mode.md R9,
// R13, R16, R19): a closed scene's file is decoded once while it is unchanged (the cache, R16); a
// layer scene without a camera drawn under a Pixel Perfect Camera is a pixel-art scene (R9); a later
// spec's drawables (S52's Sprite Batch) come in through registerDrawableCollector; the window's
// Project scope reads scene files the build list does not name; a fix planned before an edit is
// refused as stale rather than overwriting it (R13). Runs a headless project, as
// SpriteNormalsApplyTest does.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelArtFix.h"
#include "pixelart/PixelArtInput.h"
#include "pixelart/PixelArtMode.h"
#include "ScriptDataTestSupport.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace px = jasperengine::editor::pixelart;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-pixelart-input-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: pixelartinput\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            headlesssystem::use(out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        headlesssystem::use(h.platform.get());
        return h;
    }

    Project* project() { return headless().project.get(); }

    uint32_t newScene(const std::string& name) {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene(name, SceneType::SCENE_2D);
        return p->getSelectedSceneId();
    }

    Entity sprite(uint32_t sceneId, const std::string& name, Vector3 position, TextureFilter filter = TextureFilter::NEAREST) {
        auto* cmd = new CreateEntityCmd(project(), sceneId, name, EntityCreationType::SPRITE);
        cmd->setQuiet(true);
        CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
        const Entity e = cmd->getEntity();
        Scene* scene = project()->getScene(sceneId)->scene;
        Texture& base = scene->getComponent<MeshComponent>(e).submeshes[0].material.baseColorTexture;
        base = Texture("sprites/missing.png");
        base.setMinFilter(filter);
        base.setMagFilter(filter);
        scene->getComponent<Transform>(e).position = position;
        return e;
    }

    // saved under scenes/ and closed: the check reads it from its file
    fs::path saveAndClose(uint32_t sceneId, const std::string& file) {
        const fs::path path = headless().dir / "scenes" / file;
        fs::create_directories(path.parent_path());
        EXPECT_TRUE(project()->saveSceneToPath(sceneId, path));
        // the last open scene does not close: another one stays open
        newScene("Open");
        std::string error;
        EXPECT_TRUE(project()->closeSceneNow(sceneId, UnsavedChoice::Discard, &error)) << error;
        return path;
    }

    const px::PixelArtScene* sceneNamed(const px::PixelArtCheckInput& input, const std::string& path) {
        for (const px::PixelArtScene& scene : input.scenes)
            if (scene.path == path) return &scene;
        return nullptr;
    }

}

TEST(PixelArtInput, ClosedScenesAreDecodedOnceWhileUnchanged) {
    ASSERT_TRUE(headless().ready);
    const uint32_t id = newScene("Cached");
    sprite(id, "Hero", Vector3(4, 4, 0));
    const fs::path file = saveAndClose(id, "Cached.scene");

    px::Scope scope;
    scope.scenes = {"scenes/Cached.scene"};
    std::vector<px::PixelProblem> errors;
    const px::PixelArtCheckInput cold = px::inputFromProject(*project(), scope, errors);
    ASSERT_TRUE(errors.empty()) << errors.front().message;
    ASSERT_TRUE(sceneNamed(cold, "scenes/Cached.scene"));
    EXPECT_GE(px::lastInputReads().scenesDecoded, 1);
    // warm: the same input, no file decoded again
    const px::PixelArtCheckInput warm = px::inputFromProject(*project(), scope, errors);
    EXPECT_EQ(px::lastInputReads().scenesDecoded, 0);
    ASSERT_EQ(cold.scenes.size(), 1u);
    ASSERT_EQ(warm.scenes.size(), 1u);
    EXPECT_EQ(cold.scenes.front().drawables.size(), warm.scenes.front().drawables.size());
    // a newer file is decoded again
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    px::inputFromProject(*project(), scope, errors);
    EXPECT_EQ(px::lastInputReads().scenesDecoded, 1);
    // and an asset batch that moves files forgets it all
    px::forgetInputCache();
    px::inputFromProject(*project(), scope, errors);
    EXPECT_GE(px::lastInputReads().scenesDecoded, 1);
}

TEST(PixelArtInput, ALayerSceneUnderAPixelPerfectCameraIsPixelArt) {
    ASSERT_TRUE(headless().ready);
    const uint32_t parent = newScene("Parent");
    px::PixelArtMode::setupNewScene(*project(), parent);   // a Main Camera with a Pixel Perfect Camera
    const uint32_t layer = newScene("Layer");
    sprite(layer, "Ui", Vector3(2, 2, 0), TextureFilter::LINEAR);

    px::Scope scope;
    scope.scenes = {"Layer"};
    std::vector<px::PixelProblem> errors;
    // red control: not a layer of the camera's scene, not pixel art (Pixel Art Mode is off)
    const px::PixelArtCheckInput alone = px::inputFromProject(*project(), scope, errors);
    ASSERT_EQ(alone.scenes.size(), 1u);
    EXPECT_FALSE(alone.scenes.front().pixelArt);

    ASSERT_TRUE(project()->addChildScene(parent, layer, true));
    const px::PixelArtCheckInput layered = px::inputFromProject(*project(), scope, errors);
    ASSERT_EQ(layered.scenes.size(), 1u);
    EXPECT_TRUE(layered.scenes.front().pixelArt);
    ASSERT_EQ(layered.scenes.front().drawables.size(), 1u);
    // so its bilinear sprite is a filtering finding
    const px::CheckResult result = px::run(layered, {"filtering"});
    EXPECT_EQ(result.findings.size(), 1u);
}

TEST(PixelArtInput, CollectorsAddALaterSpecsDrawables) {
    ASSERT_TRUE(headless().ready);
    const uint32_t id = newScene("Batch");
    px::PixelArtMode::setupNewScene(*project(), id);
    const Entity batch = sprite(id, "Batch", Vector3(0, 0, 0));
    px::Scope scope;
    scope.scenes = {"Batch"};
    std::vector<px::PixelProblem> errors;
    const px::PixelArtCheckInput first = px::inputFromProject(*project(), scope, errors);
    ASSERT_EQ(first.scenes.size(), 1u);
    const size_t before = first.scenes.front().drawables.size();

    // S52's Sprite Batch: one more sprite drawable per batch, with the GameObject's Transform
    px::registerDrawableCollector("sprite_batch", [batch](const SceneProject&, const px::DrawableBase& base,
                                                          std::vector<px::PixelArtDrawable>& out) {
        px::PixelArtDrawable d = base(batch, px::DrawableKind::Sprite);
        d.texture = "sprites/batch.png";
        out.push_back(d);
    });
    const px::PixelArtCheckInput input = px::inputFromProject(*project(), scope, errors);
    px::unregisterDrawableCollector("sprite_batch");
    ASSERT_EQ(input.scenes.size(), 1u);
    ASSERT_EQ(input.scenes.front().drawables.size(), before + 1);
    const px::PixelArtDrawable& added = input.scenes.front().drawables.back();
    EXPECT_EQ(added.texture, "sprites/batch.png");
    EXPECT_EQ(added.entity, "/Batch");
    EXPECT_TRUE(added.hasTransform);
    EXPECT_TRUE(input.textures.count("sprites/batch.png")) << "its texture is a pixel-art texture of the scene";
    EXPECT_EQ(px::inputFromProject(*project(), scope, errors).scenes.front().drawables.size(), before);
}

TEST(PixelArtInput, ProjectScopeReadsUnlistedSceneFiles) {
    ASSERT_TRUE(headless().ready);
    const uint32_t id = newScene("Listed");
    sprite(id, "Hero", Vector3(4, 4, 0));
    const fs::path listed = saveAndClose(id, "Listed.scene");
    // a scene file the project does not list (copied in by hand)
    const fs::path unlisted = headless().dir / "scenes" / "Unlisted.scene";
    fs::copy_file(listed, unlisted, fs::copy_options::overwrite_existing);
    project()->getAssetDatabase().refresh();

    std::vector<px::PixelProblem> errors;
    const px::PixelArtCheckInput build = px::inputFromProject(*project(), px::Scope(), errors);
    EXPECT_FALSE(sceneNamed(build, "scenes/Unlisted.scene")) << "the build scenes do not name it";
    px::Scope all;
    all.allScenes = true;
    const px::PixelArtCheckInput everything = px::inputFromProject(*project(), all, errors);
    const px::PixelArtScene* found = sceneNamed(everything, "scenes/Unlisted.scene");
    ASSERT_TRUE(found) << "the Project scope reads every scene file";
    EXPECT_TRUE(sceneNamed(everything, "scenes/Listed.scene"));
    for (const px::PixelArtDrawable& d : found->drawables)
        EXPECT_EQ(d.scene, "scenes/Unlisted.scene");
}

TEST(PixelArtFix, AFixPlannedBeforeAnEditIsRefused) {
    ASSERT_TRUE(headless().ready);
    const uint32_t id = newScene("Stale");
    px::PixelArtMode::setupNewScene(*project(), id);
    const Entity hero = sprite(id, "Hero", Vector3(4.5f, 4.0f, 0.0f));
    px::Scope scope;
    scope.scenes = {"Stale"};
    std::vector<px::PixelProblem> errors;
    const px::CheckResult checked = px::run(px::inputFromProject(*project(), scope, errors), {"off_grid"});
    const px::FixPlan plan = px::plan(checked, {"off_grid"});
    ASSERT_EQ(plan.fixes.size(), 1u);

    // the sprite moves after the check: the planned move would put it back where the check saw it
    Scene* scene = project()->getScene(id)->scene;
    scene->getComponent<Transform>(hero).position = Vector3(10.25f, 4.0f, 0.0f);
    px::FixApplyReport report;
    std::string error;
    EXPECT_FALSE(px::applyFixPlan(*project(), plan, px::fixStepName(plan), report, error, &scope));
    EXPECT_EQ(error.rfind("stale: ", 0), 0u) << error;
    EXPECT_FLOAT_EQ(scene->getComponent<Transform>(hero).position.x, 10.25f) << "the newer edit stays";

    // a plan from the current state applies
    const px::FixPlan fresh = px::plan(px::run(px::inputFromProject(*project(), scope, errors), {"off_grid"}), {"off_grid"});
    ASSERT_EQ(fresh.fixes.size(), 1u);
    EXPECT_TRUE(px::applyFixPlan(*project(), fresh, px::fixStepName(fresh), report, error, &scope)) << error;
    const float x = scene->getComponent<Transform>(hero).position.x;
    EXPECT_NE(x, 10.25f);
    EXPECT_NEAR(x, 10.25f, 0.5f) << "moved onto whole pixels from where it is now";
}
