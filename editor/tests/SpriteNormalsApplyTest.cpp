// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Wiring generated normal maps onto GameObjects (spritenormals/SpriteNormalsApply, Phase 2 spec
// 9): a still map onto a Sprite (filter and wrap copied, one undo step), a tilemap's matching
// submesh only, several maps in one step, the refusals (size_mismatch, not_a_sprite,
// scene_playing, too_many_frames), an animated GIF's atlas, frames and Sprite Animation (made,
// or the existing one updated), undo/redo of the whole step. Runs a headless project, as
// VatEditorTest does.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "component/ActionComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "spritenormals/SpriteNormalsApply.h"
#include "texsynth/SynthImage.h"
#include "ScriptDataTestSupport.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
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
            const fs::path root = fs::temp_directory_path() / ("jasper-spritenormals-apply-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: spritenormalsapply\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            // the editor's Platform as System::instance() for this suite's tests (the binary's engine
            // is TestMain's: a second Engine::systemInit would restart its jobs and logger)
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
    const fs::path& root() { return headless().dir; }

    std::string folder(const std::string& name) {
        fs::remove_all(root() / "assets" / name);
        fs::create_directories(root() / "assets" / name);
        return "assets/" + name;
    }

    void writePng(const std::string& rel, int w, int h) {
        ts::Image image = ts::Image::make(w, h, 4, 8);
        for (size_t i = 0; i < image.data.size(); i++) image.data[i] = static_cast<uint8_t>(i * 13);
        std::vector<uint8_t> png;
        ASSERT_TRUE(ts::encodePng(image, png));
        const fs::path path = root() / fs::u8path(rel);
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Apply", SceneType::SCENE_2D);
        return p->getSelectedSceneId();
    }

    SceneProject* sceneProject(uint32_t sceneId) { return project()->getScene(sceneId); }
    Scene* sceneOf(uint32_t sceneId) { return sceneProject(sceneId)->scene; }
    CommandHistory* history(uint32_t sceneId) { return CommandHandle::get(sceneId); }

    Entity create(uint32_t sceneId, const std::string& name, EntityCreationType type) {
        auto* cmd = new CreateEntityCmd(project(), sceneId, name, type);
        cmd->setQuiet(true);
        history(sceneId)->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

    // A Sprite drawing `texture` sampled Nearest, clamped
    Entity sprite(uint32_t sceneId, const std::string& texture, const std::string& name = "Hero") {
        const Entity e = create(sceneId, name, EntityCreationType::SPRITE);
        Texture& base = sceneOf(sceneId)->getComponent<MeshComponent>(e).submeshes[0].material.baseColorTexture;
        base = Texture(texture);
        base.setMinFilter(TextureFilter::NEAREST);
        base.setMagFilter(TextureFilter::NEAREST);
        base.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        base.setWrapV(TextureWrap::MIRRORED_REPEAT);
        return e;
    }

    const Material& material(uint32_t sceneId, Entity e, int submesh = 0) {
        return sceneOf(sceneId)->getComponent<MeshComponent>(e).submeshes[submesh].material;
    }

    sn::ApplyRequest still(uint32_t sceneId, Entity e, const std::string& normal, const std::string& color) {
        sn::ApplyRequest r;
        r.sceneId = sceneId;
        r.entity = e;
        r.normalPath = normal;
        r.colorSourcePath = color;
        return r;
    }

    // the Sprite Animations targeting `sprite` (actions have no Transform: CreateEntityCmd's
    // parent becomes their target)
    std::vector<Entity> animationsOf(Scene* scene, Entity sprite) {
        std::vector<Entity> out;
        auto animations = scene->getComponentArray<SpriteAnimationComponent>();
        for (size_t i = 0; animations && i < animations->size(); i++) {
            const Entity e = animations->getEntity(i);
            const ActionComponent* action = scene->findComponent<ActionComponent>(e);
            if (scene->isEntityCreated(e) && action && action->target == sprite) out.push_back(e);
        }
        return out;
    }

}

TEST(SpriteNormalsApply, StillMapOntoASprite) {
    const std::string dir = folder("still");
    writePng(dir + "/hero.png", 32, 32);
    writePng(dir + "/hero_normal.png", 32, 32);
    const uint32_t sceneId = newScene();
    const Entity hero = sprite(sceneId, dir + "/hero.png");
    sceneProject(sceneId)->isModified = false;
    const size_t before = history(sceneId)->position();

    const sn::ApplyResult r = sn::applyToSprite(project(), still(sceneId, hero, dir + "/hero_normal.png", dir + "/hero.png"));
    ASSERT_TRUE(r.ok) << r.error.message;
    EXPECT_EQ(r.submeshes, std::vector<int>{0});
    EXPECT_EQ(r.undoName, "Assign Normal Map");
    EXPECT_TRUE(r.warnings.empty());
    EXPECT_EQ(r.animation, NULL_ENTITY);
    const Texture& normal = material(sceneId, hero).normalTexture;
    EXPECT_EQ(normal.getPath(), dir + "/hero_normal.png");
    EXPECT_EQ(normal.getMinFilter(), TextureFilter::NEAREST);
    EXPECT_EQ(normal.getMagFilter(), TextureFilter::NEAREST);
    EXPECT_EQ(normal.getWrapU(), TextureWrap::CLAMP_TO_EDGE);
    EXPECT_EQ(normal.getWrapV(), TextureWrap::MIRRORED_REPEAT);
    EXPECT_TRUE(sceneProject(sceneId)->isModified);
    EXPECT_EQ(history(sceneId)->position(), before + 1) << "one undo step";

    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(material(sceneId, hero).normalTexture.getPath().empty());
    EXPECT_EQ(history(sceneId)->position(), before);
    ASSERT_TRUE(history(sceneId)->redo());
    EXPECT_EQ(material(sceneId, hero).normalTexture.getPath(), dir + "/hero_normal.png");
}

TEST(SpriteNormalsApply, OnlyTheMatchingSubmeshesAndSeveralMapsInOneStep) {
    const std::string dir = folder("tilemap");
    writePng(dir + "/grass.png", 32, 32);
    writePng(dir + "/rock.png", 16, 16);
    writePng(dir + "/grass_normal.png", 32, 32);
    writePng(dir + "/rock_normal.png", 16, 16);
    const uint32_t sceneId = newScene();
    const Entity map = create(sceneId, "Tilemap", EntityCreationType::TILEMAP);
    MeshComponent& mesh = sceneOf(sceneId)->getComponent<MeshComponent>(map);
    mesh.numSubmeshes = 2;
    mesh.submeshes[0].material.baseColorTexture = Texture(dir + "/grass.png");
    mesh.submeshes[1].material.baseColorTexture = Texture(dir + "/rock.png");

    sn::ApplyResult r = sn::applyToSprite(project(), still(sceneId, map, dir + "/rock_normal.png", dir + "/rock.png"));
    ASSERT_TRUE(r.ok) << r.error.message;
    EXPECT_EQ(r.submeshes, std::vector<int>{1});
    EXPECT_TRUE(material(sceneId, map, 0).normalTexture.getPath().empty());
    EXPECT_EQ(material(sceneId, map, 1).normalTexture.getPath(), dir + "/rock_normal.png");
    ASSERT_TRUE(history(sceneId)->undo());

    // both tilesets' maps in one step
    sn::ApplyRequest both;
    both.sceneId = sceneId;
    both.entity = map;
    both.maps = {{dir + "/grass_normal.png", dir + "/grass.png"}, {dir + "/rock_normal.png", dir + "/rock.png"}};
    const size_t before = history(sceneId)->position();
    r = sn::applyToSprite(project(), both);
    ASSERT_TRUE(r.ok) << r.error.message;
    EXPECT_EQ(r.submeshes, (std::vector<int>{0, 1}));
    EXPECT_EQ(material(sceneId, map, 0).normalTexture.getPath(), dir + "/grass_normal.png");
    EXPECT_EQ(material(sceneId, map, 1).normalTexture.getPath(), dir + "/rock_normal.png");
    EXPECT_EQ(history(sceneId)->position(), before + 1);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(material(sceneId, map, 0).normalTexture.getPath().empty());
    EXPECT_TRUE(material(sceneId, map, 1).normalTexture.getPath().empty());

    // one pair that does not match: nothing is applied
    both.maps[1].colorSourcePath = dir + "/sand.png";
    r = sn::applyToSprite(project(), both);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.kind, "not_a_sprite");
    EXPECT_TRUE(material(sceneId, map, 0).normalTexture.getPath().empty());
    EXPECT_EQ(history(sceneId)->position(), before);
}

TEST(SpriteNormalsApply, ApplyToSpritesIsOneStepAllOrNothing) {
    const std::string dir = folder("many");
    writePng(dir + "/a.png", 16, 16);
    writePng(dir + "/a_normal.png", 16, 16);
    writePng(dir + "/b.png", 16, 16);
    writePng(dir + "/b_normal.png", 8, 8);
    const uint32_t sceneId = newScene();
    const Entity a = sprite(sceneId, dir + "/a.png", "A");
    const Entity b = sprite(sceneId, dir + "/b.png", "B");
    const size_t before = history(sceneId)->position();

    // b's map has the wrong size: a's assignment is undone too
    sn::ApplyResult r = sn::applyToSprites(project(), {still(sceneId, a, dir + "/a_normal.png", dir + "/a.png"),
                                                       still(sceneId, b, dir + "/b_normal.png", dir + "/b.png")});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.kind, "size_mismatch");
    EXPECT_TRUE(material(sceneId, a).normalTexture.getPath().empty());
    EXPECT_EQ(history(sceneId)->position(), before);
    EXPECT_FALSE(history(sceneId)->canRedo()) << "a rolled back step leaves nothing to redo";

    writePng(dir + "/b_normal.png", 16, 16);
    r = sn::applyToSprites(project(), {still(sceneId, a, dir + "/a_normal.png", dir + "/a.png"), still(sceneId, b, dir + "/b_normal.png", dir + "/b.png")});
    ASSERT_TRUE(r.ok) << r.error.message;
    EXPECT_EQ(history(sceneId)->position(), before + 1);
    EXPECT_EQ(material(sceneId, b).normalTexture.getPath(), dir + "/b_normal.png");
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(material(sceneId, a).normalTexture.getPath().empty());
    EXPECT_TRUE(material(sceneId, b).normalTexture.getPath().empty());

    // two scenes: refused (the first made non-transient, so the new scene does not replace it)
    sceneProject(sceneId)->transientId = false;
    sceneProject(sceneId)->isModified = false;
    project()->createNewScene("Other", SceneType::SCENE_2D);
    const uint32_t other = project()->getSelectedSceneId();
    ASSERT_NE(other, sceneId);
    const Entity c = sprite(other, dir + "/a.png", "C");
    r = sn::applyToSprites(project(), {still(sceneId, a, dir + "/a_normal.png", dir + "/a.png"), still(other, c, dir + "/a_normal.png", dir + "/a.png")});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.kind, "not_a_sprite");
    EXPECT_NE(r.error.message.find("scenes"), std::string::npos) << r.error.message;
    EXPECT_FALSE(sn::applyToSprites(project(), {}).ok);
}

TEST(SpriteNormalsApply, Refusals) {
    const std::string dir = folder("refusals");
    writePng(dir + "/hero.png", 32, 32);
    writePng(dir + "/small_normal.png", 16, 16);
    writePng(dir + "/hero_normal.png", 32, 32);
    const uint32_t sceneId = newScene();
    const Entity hero = sprite(sceneId, dir + "/hero.png");
    const size_t before = history(sceneId)->position();

    sn::ApplyResult r = sn::applyToSprite(project(), still(sceneId, hero, dir + "/small_normal.png", dir + "/hero.png"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.kind, "size_mismatch");
    EXPECT_EQ(r.error.message, "The sprite's texture is 32x32 but the normal map is 16x16; they must match pixel for pixel.");

    r = sn::applyToSprite(project(), still(sceneId, hero, dir + "/hero_normal.png", dir + "/other.png"));
    EXPECT_EQ(r.error.kind, "not_a_sprite");
    EXPECT_EQ(r.error.message, "Its texture is " + dir + "/hero.png, not " + dir + "/other.png.");

    const Entity empty = create(sceneId, "Empty", EntityCreationType::GAMEOBJECT);
    r = sn::applyToSprite(project(), still(sceneId, empty, dir + "/hero_normal.png", dir + "/hero.png"));
    EXPECT_EQ(r.error.kind, "not_a_sprite");

    r = sn::applyToSprite(project(), still(sceneId, hero, dir + "/missing_normal.png", dir + "/hero.png"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.kind, "invalid_source");

    sceneProject(sceneId)->playState = ScenePlayState::PLAYING;
    r = sn::applyToSprite(project(), still(sceneId, hero, dir + "/hero_normal.png", dir + "/hero.png"));
    sceneProject(sceneId)->playState = ScenePlayState::STOPPED;
    EXPECT_EQ(r.error.kind, "scene_playing");
    EXPECT_FALSE(r.error.fix.empty());

    EXPECT_EQ(history(sceneId)->position(), before + 1) << "only the Empty GameObject was added";
    // a refusal keeps the user's redo tail
    ASSERT_TRUE(history(sceneId)->undo());
    ASSERT_TRUE(history(sceneId)->canRedo());
    r = sn::applyToSprite(project(), still(sceneId, hero, dir + "/small_normal.png", dir + "/hero.png"));
    EXPECT_EQ(r.error.kind, "size_mismatch");
    EXPECT_TRUE(history(sceneId)->canRedo()) << "nothing was added, so nothing is dropped";
    ASSERT_TRUE(history(sceneId)->redo());
    EXPECT_TRUE(material(sceneId, hero).normalTexture.getPath().empty());
    EXPECT_FALSE(sn::applyToSprite(nullptr, still(sceneId, hero, "", "")).ok);
}

TEST(SpriteNormalsApply, LinkedMaterialWarns) {
    const std::string dir = folder("linked");
    writePng(dir + "/hero.png", 8, 8);
    writePng(dir + "/hero_normal.png", 8, 8);
    const uint32_t sceneId = newScene();
    const Entity hero = sprite(sceneId, dir + "/hero.png");
    project()->linkMaterialFile(sceneId, hero, 0, dir + "/hero.material");
    const sn::ApplyResult r = sn::applyToSprite(project(), still(sceneId, hero, dir + "/hero_normal.png", dir + "/hero.png"));
    ASSERT_TRUE(r.ok) << r.error.message;
    ASSERT_EQ(r.warnings.size(), 1u);
    EXPECT_EQ(r.warnings[0].kind, "material_linked");
    EXPECT_EQ(r.warnings[0].message, "The sprite's material comes from " + dir + "/hero.material; the scene's copy changed, the file did not.");
    project()->unlinkMaterialFile(sceneId, hero, 0);
}

TEST(SpriteNormalsApply, AnimatedSourceMakesTheAnimation) {
    const std::string dir = folder("animated");
    writePng(dir + "/old.png", 16, 16);
    writePng(dir + "/walk_sheet.png", 64, 32);
    writePng(dir + "/walk_sheet_normal.png", 64, 32);
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = sprite(sceneId, dir + "/old.png");
    SpriteComponent& before = scene->getComponent<SpriteComponent>(hero);
    before.framesRect[0] = {"x", Rect(0, 0, 4, 4)};
    before.framesRect[1] = {"y", Rect(4, 0, 4, 4)};
    before.framesRect[2] = {"z", Rect(8, 0, 4, 4)};
    before.numFramesRect = 3;
    const size_t start = history(sceneId)->position();

    sn::ApplyRequest r;
    r.sceneId = sceneId;
    r.entity = hero;
    r.normalPath = dir + "/walk_sheet_normal.png";
    r.colorSourcePath = dir + "/walk.gif";
    r.atlasPath = dir + "/walk_sheet.png";
    r.frames = {{0, 0, 32, 32}, {32, 0, 32, 32}};
    r.frameMs = {80, 120};
    r.loopCount = 0;
    sn::ApplyResult result = sn::applyToSprite(project(), r);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.undoName, "Assign Animated Sprite");
    EXPECT_TRUE(result.createdAnimation);
    EXPECT_EQ(history(sceneId)->position(), start + 1) << "one undo step, the animation included";

    const Material& m = material(sceneId, hero);
    EXPECT_EQ(m.baseColorTexture.getPath(), dir + "/walk_sheet.png");
    EXPECT_EQ(m.baseColorTexture.getMinFilter(), TextureFilter::NEAREST) << "the sampling is kept";
    EXPECT_EQ(m.normalTexture.getPath(), dir + "/walk_sheet_normal.png");
    EXPECT_EQ(m.normalTexture.getWrapV(), TextureWrap::MIRRORED_REPEAT);
    const SpriteComponent& sc = scene->getComponent<SpriteComponent>(hero);
    ASSERT_EQ(sc.numFramesRect, 2u);
    EXPECT_EQ(sc.framesRect[0].name, "frame_0");
    EXPECT_EQ(sc.framesRect[1].name, "frame_1");
    EXPECT_FLOAT_EQ(sc.framesRect[1].rect.getX(), 32.0f);
    EXPECT_TRUE(sc.framesRect[2].name.empty()) << "old frames past the new ones are cleared";
    EXPECT_EQ(sc.width, 32u);
    EXPECT_EQ(sc.height, 32u);
    const Rect shown = scene->getComponent<MeshComponent>(hero).submeshes[0].textureRect;
    EXPECT_FLOAT_EQ(shown.getWidth(), 0.5f);
    EXPECT_FLOAT_EQ(shown.getHeight(), 1.0f);

    const Entity animation = result.animation;
    ASSERT_NE(animation, NULL_ENTITY);
    EXPECT_EQ(animationsOf(scene, hero), std::vector<Entity>{animation}) << "made for the sprite";
    const SpriteAnimationComponent& anim = scene->getComponent<SpriteAnimationComponent>(animation);
    EXPECT_EQ(anim.framesSize, 2u);
    EXPECT_EQ(anim.framesTimeSize, 2u);
    EXPECT_EQ(anim.frames[1], 1);
    EXPECT_EQ(anim.framesTime[0], 80);
    EXPECT_EQ(anim.framesTime[1], 120);
    EXPECT_TRUE(anim.loop);
    const ActionComponent& action = scene->getComponent<ActionComponent>(animation);
    EXPECT_EQ(action.target, hero);
    EXPECT_EQ(action.state, ActionState::Running) << "autoplay when created";

    // undo: everything back, the animation gone; redo: all again
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_EQ(material(sceneId, hero).baseColorTexture.getPath(), dir + "/old.png");
    EXPECT_TRUE(material(sceneId, hero).normalTexture.getPath().empty());
    EXPECT_EQ(scene->getComponent<SpriteComponent>(hero).numFramesRect, 3u);
    EXPECT_EQ(scene->getComponent<SpriteComponent>(hero).framesRect[2].name, "z");
    EXPECT_TRUE(animationsOf(scene, hero).empty());
    ASSERT_TRUE(history(sceneId)->redo());
    EXPECT_EQ(material(sceneId, hero).baseColorTexture.getPath(), dir + "/walk_sheet.png");
    ASSERT_EQ(animationsOf(scene, hero).size(), 1u);

    // again: the existing animation is updated, not a second one; loop count 1 plays once
    const Entity existing = animationsOf(scene, hero)[0];
    r.frames = {{0, 0, 32, 32}};
    r.frameMs = {50};
    r.loopCount = 1;
    result = sn::applyToSprite(project(), r);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.createdAnimation);
    EXPECT_EQ(result.animation, existing);
    EXPECT_EQ(animationsOf(scene, hero).size(), 1u);
    const SpriteAnimationComponent& updated = scene->getComponent<SpriteAnimationComponent>(existing);
    EXPECT_EQ(updated.framesSize, 1u);
    EXPECT_EQ(updated.framesTime[0], 50);
    EXPECT_EQ(updated.framesTime[1], 0);
    EXPECT_FALSE(updated.loop);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_EQ(scene->getComponent<SpriteAnimationComponent>(existing).framesSize, 2u);
    EXPECT_TRUE(scene->getComponent<SpriteAnimationComponent>(existing).loop);
}

TEST(SpriteNormalsApply, AnimatedRefusals) {
    const std::string dir = folder("animrefuse");
    writePng(dir + "/sheet.png", 64, 32);
    writePng(dir + "/sheet_normal.png", 64, 32);
    const uint32_t sceneId = newScene();
    const Entity hero = sprite(sceneId, dir + "/sheet.png");
    sn::ApplyRequest r;
    r.sceneId = sceneId;
    r.entity = hero;
    r.normalPath = dir + "/sheet_normal.png";
    r.atlasPath = dir + "/sheet.png";
    // (a sprite holds any number of frames: only past ARRAY_INDEX_LIMIT is a count refused)
    r.frames.assign(ARRAY_INDEX_LIMIT + 1, {0, 0, 1, 1});
    sn::ApplyResult result = sn::applyToSprite(project(), r);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.kind, "too_many_frames");
    EXPECT_EQ(result.error.message, "The animation has 65537 frames; a Sprite holds at most 65536.");

    // frames outside the atlas
    r.frames = {{0, 0, 32, 32}, {48, 0, 32, 32}};
    result = sn::applyToSprite(project(), r);
    EXPECT_EQ(result.error.kind, "frames_mismatch");
    EXPECT_EQ(result.error.message, "Frame 1 [48, 0, 32, 32] is not inside the 64x32 atlas.");

    // a mesh that is not a Sprite can't hold frames
    const Entity plane = create(sceneId, "Plane", EntityCreationType::PLANE);
    r.entity = plane;
    r.frames = {{0, 0, 32, 32}};
    result = sn::applyToSprite(project(), r);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.kind, "not_a_sprite");
    EXPECT_TRUE(animationsOf(sceneOf(sceneId), hero).empty());
}
