// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector and Animation window parts of animation layers (upstream anim layers spec 9,
// 11.4, 14 editor tests; docs/specs/S21-animation.md "Upstream absorption"), through the
// functions their clicks run, on S21's masks (AvatarMaskDef) and documents
// (AnimatorAssetDocument): the Avatar Mask figure (a click on the right arm writes right_arm,
// undo puts it back), the layer edits and the clip Layer combo with undo, Create Mask from
// Selection and Create Animator Controller from Clips (one scene step each, with the new file;
// the controller plays on an Animator the player hands over to), the mask coverage line, the
// preview hierarchy of a model (humanoid detection for the "Not humanoid" note), and the
// Animation window's Preview over (the arm from the layer through its mask, the leg from the
// base) and its masked-out rows.

#include <gtest/gtest.h>
#include "component/Transform.h"

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationPlayerRuntime.h"
#include "command/CommandHandle.h"
#include "component/AnimatorComponent.h"
#include "command/type/PlayerLayerActions.h"
#include "command/type/PropertyCmd.h"
#include "window/InspectorAnimLayers.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct LayersHost : public EditorHost {
        void stopTransientPreviews() override {
            if (animationClipAppHooks().stopTransientPreviews) animationClipAppHooks().stopTransientPreviews();
        }
        void clearSceneWindowState(uint32_t sceneId) override {
            if (animationClipAppHooks().sceneClosed) animationClipAppHooks().sceneClosed(sceneId);
        }
        void prepareForProjectSwitch() override {
            if (animationClipAppHooks().projectClosed) animationClipAppHooks().projectClosed();
        }
    };

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-layers-ui-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: layersui\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the test binary's TestSystem stays the engine's System (a second
            // Engine::systemInit would break its log capture): the project's paths go in
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    bool started = false;
    Project* project() {
        started = true;
        return headless().project.get();
    }
    fs::path assets() { return project()->getAssetsPath(); }

    struct Cleanup : public ::testing::Environment {
        void TearDown() override {
            if (!started) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const cleanup = ::testing::AddGlobalTestEnvironment(new Cleanup);

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    ClipKey key(float time, std::vector<float> value) {
        ClipKey out;
        out.time = time;
        out.value = std::move(value);
        return out;
    }

    ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<ClipKey> keys) {
        ClipTrackData out;
        out.path = path;
        out.channel = channel;
        out.keys = std::move(keys);
        return out;
    }

    // walk: Leg x 0 -> 2 and Arm y 0 -> 4 over a second; hold: Arm y 3 and Leg x 9 (the mask hides the leg)
    void writeClips() {
        ClipData walk;
        walk.wrap = ClipWrapMode::Loop;
        walk.tracks = {track("Arm", ClipChannel::Position, {key(0, {5, 0, 0}), key(1, {5, 4, 0})}),
                       track("Leg", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {2, 0, 0})})};
        ClipData hold;
        hold.wrap = ClipWrapMode::Loop;
        hold.tracks = {track("Arm", ClipChannel::Position, {key(0, {5, 3, 0}), key(1, {5, 3, 0})}),
                       track("Leg", ClipChannel::Position, {key(0, {9, 0, 0}), key(1, {9, 0, 0})})};
        writeAll(assets() / "layers/walk.anim", AnimationClipDocument::fileText(walk));
        writeAll(assets() / "layers/hold.anim", AnimationClipDocument::fileText(hold));
        anim::AvatarMaskDef arm;
        arm.defaultWeight = 0.0f;
        arm.paths.push_back({"Arm", 1.0f, true});
        writeAll(assets() / "layers/arm.mask", anim::writeMask(arm));
        AnimationClipDocument::checkDiskChanges(project());
    }

    // Hero (walk on the base, hold on Holding with the arm mask) > Arm > Hand, Leg
    struct LayerScene {
        uint32_t id = 0;
        SceneProject* sceneProject = nullptr;
        Scene* scene = nullptr;
        Entity hero = NULL_ENTITY, arm = NULL_ENTITY, hand = NULL_ENTITY, leg = NULL_ENTITY;
        AnimationPlayerComponent& player() const { return scene->getComponent<AnimationPlayerComponent>(hero); }
        Transform& transform(Entity entity) const { return scene->getComponent<Transform>(entity); }
    };

    Entity addObject(LayerScene& s, const std::string& name, Entity parent, const Vector3& position) {
        const Entity entity = s.scene->createUserEntity();
        s.scene->setEntityName(entity, name);
        Transform transform;
        transform.position = position;
        s.scene->addComponent<Transform>(entity, transform);
        if (parent != NULL_ENTITY) s.scene->addEntityChild(parent, entity, false);
        s.sceneProject->entities.push_back(entity);
        return entity;
    }

    LayerScene layerScene(bool withHoldingLayer = true) {
        LayerScene s;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Layers", SceneType::SCENE_3D);
        s.id = p->getSelectedSceneId();
        s.sceneProject = p->getScene(s.id);
        s.scene = s.sceneProject->scene;
        s.hero = addObject(s, "Hero", NULL_ENTITY, Vector3(0, 0, 0));
        s.arm = addObject(s, "Arm", s.hero, Vector3(5, 0, 0));
        s.hand = addObject(s, "Hand", s.arm, Vector3(0, 7, 0));
        s.leg = addObject(s, "Leg", s.hero, Vector3(0, -3, 0));
        AnimationPlayerComponent player;
        AnimationPlayerClip walk;
        walk.clip = "layers/walk.anim";
        walk.name = "walk";
        AnimationPlayerClip hold;
        hold.clip = "layers/hold.anim";
        hold.name = "hold";
        hold.layer = withHoldingLayer ? 1 : 0;
        player.clips = {walk, hold};
        player.defaultClip = "walk";
        if (withHoldingLayer){
            AnimationPlayerLayer base;
            base.name = anim::BASE_LAYER_NAME;
            AnimationPlayerLayer holding;
            holding.name = "Holding";
            holding.weight = 0.5f;
            holding.mask = "layers/arm.mask";
            player.layers = {base, holding};
        }
        s.scene->addComponent<AnimationPlayerComponent>(s.hero, player);
        s.sceneProject->isModified = false;
        return s;
    }

    class AnimationLayersUi : public ::testing::Test {
    protected:
        LayersHost host;
        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            setEditorHost(&host);
            fs::remove_all(assets() / "layers");
            writeClips();
        }
        void TearDown() override {
            AnimationClipController::release(project());
            setEditorHost(nullptr);
        }
    };

}

TEST(AnimationLayersFigure, EveryPartIsClickableAtItsCentre) {
    for (int p = 0; p < AVATAR_BODY_PART_COUNT; p++){
        float x = 0, y = 0;
        animlayersui::figurePartCenter((AvatarBodyPart)p, x, y);
        EXPECT_EQ(animlayersui::figurePartAt(x, y), p) << animlayersui::partLabel((AvatarBodyPart)p);
    }
    EXPECT_EQ(animlayersui::figurePartAt(0.02f, 0.02f), -1) << "outside the figure";
    EXPECT_STREQ(animlayersui::partLabel(AvatarBodyPart::RightFingers), "Right Fingers");
    EXPECT_STREQ(animlayersui::partLabel(AvatarBodyPart::RightHand), "Right Hand");
}

TEST_F(AnimationLayersUi, AFigureClickWritesThePartAndUndoPutsItBack) {
    std::string error;
    std::shared_ptr<AnimatorAssetDocument> document =
        AnimatorAssetDocument::create(project(), "assets/layers/figure.mask", AnimatorAssetDocument::Kind::Mask, "Create Avatar Mask", &error);
    ASSERT_TRUE(document) << error;
    anim::AvatarMaskDef initial = document->mask();
    for (auto& part : initial.bodyParts) part = 1.0f;
    ASSERT_TRUE(document->commit(initial, "Edit Avatar Mask", "", &error)) << error;
    // the click: the part under the right arm's centre, toggled, committed
    float x = 0, y = 0;
    animlayersui::figurePartCenter(AvatarBodyPart::RightArm, x, y);
    const int part = animlayersui::figurePartAt(x, y);
    ASSERT_EQ(part, (int)AvatarBodyPart::RightArm);
    ASSERT_TRUE(document->commit(animlayersui::togglePart(document->mask(), (AvatarBodyPart)part), "Edit Avatar Mask", "", &error)) << error;
    EXPECT_EQ(document->mask().partWeight(AvatarBodyPart::RightArm), 0.0f);
    EXPECT_EQ(document->mask().partWeight(AvatarBodyPart::RightHand), 1.0f) << "the hand is a part of its own";
    EXPECT_EQ(document->mask().partWeight(AvatarBodyPart::LeftArm), 1.0f);
    const anim::MaskParseResult written = anim::parseMask(readAll(assets() / "layers/figure.mask"));
    ASSERT_TRUE(written.ok) << written.error.toString();
    ASSERT_TRUE(written.data.hasPart(AvatarBodyPart::RightArm));
    EXPECT_EQ(written.data.partWeight(AvatarBodyPart::RightArm), 0.0f);
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_EQ(document->mask().partWeight(AvatarBodyPart::RightArm), 1.0f);
    // a fraction toggles to 1, a part that is not set (the default) toggles from what it weighs
    anim::AvatarMaskDef half = document->mask();
    half.bodyParts[(size_t)AvatarBodyPart::Head] = 0.5f;
    EXPECT_EQ(animlayersui::togglePart(half, AvatarBodyPart::Head).partWeight(AvatarBodyPart::Head), 1.0f);
    anim::AvatarMaskDef unset;
    unset.defaultWeight = 0.0f;
    EXPECT_EQ(animlayersui::togglePart(unset, AvatarBodyPart::Head).partWeight(AvatarBodyPart::Head), 1.0f);
    EXPECT_TRUE(animlayersui::togglePart(unset, AvatarBodyPart::Head).hasPart(AvatarBodyPart::Head));

    // Shift-click in the transform tree: a subtree on, then off
    const std::vector<std::string> paths = {"Arm", "Arm/Hand", "Arm/Hand/Finger", "Leg"};
    anim::AvatarMaskDef tree = animlayersui::setTransform(anim::AvatarMaskDef(), "Arm", true, true, paths);
    ASSERT_EQ(tree.paths.size(), 3u);
    EXPECT_EQ(tree.findPath("Leg"), -1);
    EXPECT_TRUE(tree.paths[(size_t)tree.findPath("Arm")].children);
    tree = animlayersui::setTransform(tree, "Arm/Hand", false, true, paths);
    EXPECT_EQ(tree.paths.size(), 1u);
}

TEST_F(AnimationLayersUi, LayerEditsAndTheClipLayerComboAreSceneSteps) {
    LayerScene s = layerScene(false);
    std::string error;
    AnimationPlayerLayer added;
    added.name = "Upper";
    int index = -1;
    ASSERT_TRUE(PlayerLayerActions::addLayer(project(), s.id, s.hero, added, &error, &index)) << error;
    EXPECT_EQ(index, 1);
    ASSERT_EQ(s.player().layers.size(), 2u);
    // the clip row's Layer combo
    CommandHandle::get(s.id)->addCommandNoMerge(new PropertyCmd<int>(project(), s.id, s.hero, ComponentType::AnimationPlayerComponent, "clips[1].layer", 1));
    EXPECT_EQ(s.player().clips[1].layer, 1);
    ASSERT_TRUE(PlayerLayerActions::renameLayer(project(), s.id, s.hero, 1, "Holding", &error)) << error;
    EXPECT_EQ(s.player().layers[1].name, "Holding");
    CommandHistory* history = CommandHandle::get(s.id);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(s.player().layers[1].name, "Upper");
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(s.player().clips[1].layer, 0);
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(s.player().layers.empty());
}

TEST_F(AnimationLayersUi, CreateMaskFromSelectionIsOneStepWithItsFile) {
    LayerScene s = layerScene();
    std::string error;
    ASSERT_FALSE(animlayersui::createMaskFromSelection(project(), s.id, s.hero, 1, "assets/layers/upper.mask", {}, error));
    EXPECT_NE(error.find("Select"), std::string::npos) << error;
    ASSERT_TRUE(animlayersui::createMaskFromSelection(project(), s.id, s.hero, 1, "assets/layers/upper.mask", {s.arm, s.leg}, error)) << error;
    const fs::path file = assets() / "layers/upper.mask";
    ASSERT_TRUE(fs::exists(file));
    anim::MaskParseResult parsed = anim::parseMask(readAll(file));
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    EXPECT_EQ(parsed.dialect, anim::AnimatorFileDialect::Current);
    ASSERT_EQ(parsed.data.paths.size(), 2u);
    EXPECT_EQ(parsed.data.paths[0].path, "Arm");
    EXPECT_TRUE(parsed.data.paths[0].children);
    EXPECT_EQ(parsed.data.paths[1].path, "Leg");
    EXPECT_EQ(parsed.data.defaultWeight, 0.0f);
    EXPECT_EQ(s.player().layers[1].mask, "layers/upper.mask");
    EXPECT_EQ(s.player().layers[1].maskGuid, parsed.data.guid);
    EXPECT_EQ(project()->getAssetDatabase().getGuid(file), parsed.data.guid) << "the asset database knows the new file";
    ASSERT_TRUE(CommandHandle::get(s.id)->undo());
    EXPECT_EQ(s.player().layers[1].mask, "layers/arm.mask");
    EXPECT_FALSE(fs::exists(file)) << "one step: the file goes with the undo";
    ASSERT_TRUE(CommandHandle::get(s.id)->redo());
    EXPECT_TRUE(fs::exists(file));
    EXPECT_EQ(s.player().layers[1].mask, "layers/upper.mask");

    // on a player without layers: its whole list (one Base Layer) gets the mask
    LayerScene plain = layerScene(false);
    ASSERT_TRUE(animlayersui::createMaskFromSelection(project(), plain.id, plain.hero, 0, "assets/layers/base.mask", {plain.arm}, error)) << error;
    ASSERT_EQ(plain.player().layers.size(), 1u);
    EXPECT_EQ(plain.player().layers[0].mask, "layers/base.mask");
}

TEST_F(AnimationLayersUi, CreatedMasksAndControllersStayInTheAssetsFolder) {
    LayerScene s = layerScene();
    std::string error;
    const std::pair<const char*, const char*> refused[] = {{"../outside.mask", "outside the project"}, {"loose.mask", "outside the assets folder"},
                                                           {"assets/layers/upper.anim", "not a .mask path"}};
    for (const auto& [path, why] : refused){
        EXPECT_FALSE(animlayersui::createMaskFromSelection(project(), s.id, s.hero, 1, path, {s.arm}, error)) << path;
        EXPECT_NE(error.find(why), std::string::npos) << path << ": " << error;
    }
    EXPECT_FALSE(fs::exists(project()->getProjectPath() / "loose.mask"));
    EXPECT_FALSE(animlayersui::createControllerFromClips(project(), s.id, s.hero, "../outside.controller", error));
    EXPECT_NE(error.find("outside the project"), std::string::npos) << error;
    EXPECT_FALSE(animlayersui::createControllerFromClips(project(), s.id, s.hero, "loose.controller", error));
    EXPECT_NE(error.find("outside the assets folder"), std::string::npos) << error;
    EXPECT_EQ(s.player().layers[1].mask, "layers/arm.mask") << "nothing changed";
    EXPECT_FALSE(s.scene->findComponent<AnimatorComponent>(s.hero));
    EXPECT_TRUE(s.player().enabled);
}

TEST_F(AnimationLayersUi, CreateControllerFromClips) {
    LayerScene s = layerScene();
    std::string error;
    ASSERT_TRUE(animlayersui::createControllerFromClips(project(), s.id, s.hero, "assets/layers/hero.controller", error)) << error;
    const fs::path file = assets() / "layers/hero.controller";
    anim::ControllerParseResult parsed = anim::parseController(readAll(file));
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    EXPECT_EQ(parsed.dialect, anim::AnimatorFileDialect::Current);
    const anim::ControllerDef& data = parsed.data;
    ASSERT_EQ(data.layers.size(), 2u);
    EXPECT_EQ(data.layers[0].name, anim::BASE_LAYER_NAME);
    ASSERT_EQ(data.layers[0].machine.states.size(), 1u);
    EXPECT_EQ(data.layers[0].machine.states[0].name, "walk");
    EXPECT_EQ(data.layers[0].machine.states[0].motion.clip.path, "layers/walk.anim");
    EXPECT_EQ(data.layers[0].machine.defaultState, "walk");
    EXPECT_EQ(data.layers[1].name, "Holding");
    EXPECT_FLOAT_EQ(data.layers[1].weight, 0.5f);
    EXPECT_EQ(data.layers[1].blending, AnimatorLayerBlending::Override);
    EXPECT_EQ(data.layers[1].mask.path, "layers/arm.mask");
    ASSERT_EQ(data.layers[1].machine.states.size(), 1u);
    EXPECT_EQ(data.layers[1].machine.states[0].name, "hold");
    EXPECT_TRUE(data.layers[0].machine.states[0].transitions.empty());
    EXPECT_TRUE(data.layers[0].machine.anyState.empty());
    EXPECT_TRUE(data.parameters.empty());

    // an Animator plays it; the player is disabled (it hands over to the Animator), its clips and
    // layers kept
    const AnimatorComponent* animator = s.scene->findComponent<AnimatorComponent>(s.hero);
    ASSERT_TRUE(animator);
    EXPECT_EQ(animator->controller.path, "layers/hero.controller");
    EXPECT_EQ(animator->controller.guid, Guid::parse(data.guid));
    EXPECT_FALSE(s.player().enabled);
    EXPECT_EQ(s.player().clips.size(), 2u);
    EXPECT_EQ(s.player().layers.size(), 2u);

    // one step: the undo takes the Animator, the player's state and the file
    ASSERT_TRUE(CommandHandle::get(s.id)->undo());
    EXPECT_FALSE(s.scene->findComponent<AnimatorComponent>(s.hero));
    EXPECT_TRUE(s.player().enabled);
    EXPECT_FALSE(fs::exists(file));
    ASSERT_TRUE(CommandHandle::get(s.id)->redo());
    EXPECT_TRUE(fs::exists(file));
    ASSERT_TRUE(s.scene->findComponent<AnimatorComponent>(s.hero));
    EXPECT_FALSE(s.player().enabled);
}

TEST_F(AnimationLayersUi, MaskCoverageCountsTheLayersBoundEntities) {
    LayerScene s = layerScene();
    anim::AvatarMaskDef data;
    data.defaultWeight = 0.0f;
    data.paths.push_back({"Arm", 1.0f, true});
    const animlayersui::MaskCoverage coverage = animlayersui::maskCoverage(s.scene, s.hero, data, {s.arm, s.leg, s.arm, s.hand});
    EXPECT_EQ(coverage.total, 3u);
    EXPECT_EQ(coverage.covered, 2u) << "the arm and its hand";
    EXPECT_FALSE(coverage.humanoidWithoutRig);
    data.paths[0].children = false;
    EXPECT_EQ(animlayersui::maskCoverage(s.scene, s.hero, data, {s.arm, s.hand}).covered, 1u) << "the arm alone";
    data.bodyParts[(size_t)AvatarBodyPart::RightArm] = 1.0f;
    EXPECT_TRUE(animlayersui::maskCoverage(s.scene, s.hero, data, {s.arm}).humanoidWithoutRig);
}

TEST_F(AnimationLayersUi, ThePreviewHierarchyOfAModelKnowsItsHumanoidRig) {
    const fs::path fixture = fs::path(JASPER_EDITOR_TEST_FIXTURES).parent_path().parent_path().parent_path() /
                             "tools/fixtures/pose-animation/assets/models/hero_tpose.gltf";
    ASSERT_TRUE(fs::exists(fixture)) << fixture;
    fs::create_directories(assets() / "layers");
    fs::copy_file(fixture, assets() / "layers/hero.gltf", fs::copy_options::overwrite_existing);
    animlayersui::PreviewHierarchy preview;
    ASSERT_TRUE(animlayersui::previewHierarchyFromModel(project(), "layers/hero.gltf", preview)) << preview.error;
    EXPECT_FALSE(preview.paths.empty());
    EXPECT_TRUE(preview.humanoid.valid) << preview.humanoid.error;
    // a right arm and hand mask covers the right hand, not the left; the arm part alone stops at
    // the wrist (the hand is a part of its own, upstream's arm masks convert to both)
    anim::AvatarMaskDef data;
    data.defaultWeight = 0.0f;
    data.bodyParts[(size_t)AvatarBodyPart::RightArm] = 1.0f;
    const Entity rightHand = preview.humanoid.entities[(size_t)pose::HumanBone::RightHand];
    const Entity rightForearm = preview.humanoid.entities[(size_t)pose::HumanBone::RightLowerArm];
    const Entity leftHand = preview.humanoid.entities[(size_t)pose::HumanBone::LeftHand];
    ASSERT_NE(rightHand, NULL_ENTITY);
    ASSERT_NE(rightForearm, NULL_ENTITY);
    EXPECT_EQ(anim::maskWeightOf(data, preview.registry.get(), preview.root, rightForearm, &preview.humanoid), 1.0f);
    EXPECT_EQ(anim::maskWeightOf(data, preview.registry.get(), preview.root, rightHand, &preview.humanoid), 0.0f);
    data.bodyParts[(size_t)AvatarBodyPart::RightHand] = 1.0f;
    EXPECT_EQ(anim::maskWeightOf(data, preview.registry.get(), preview.root, rightHand, &preview.humanoid), 1.0f);
    EXPECT_EQ(anim::maskWeightOf(data, preview.registry.get(), preview.root, leftHand, &preview.humanoid), 0.0f);

    // a GameObject without a rig: no humanoid (the "Not humanoid" note)
    LayerScene s = layerScene();
    animlayersui::PreviewHierarchy plain;
    ASSERT_TRUE(animlayersui::previewHierarchyFromEntity(s.scene, s.hero, plain));
    EXPECT_EQ(plain.paths.size(), 3u);
    EXPECT_FALSE(plain.humanoid.valid);
}

TEST_F(AnimationLayersUi, PreviewOverComposesTheLayerOverABaseState) {
    LayerScene s = layerScene();
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    ASSERT_TRUE(c.openState("hold", &error)) << error;
    EXPECT_EQ(c.getStateLayer(), 1);
    EXPECT_EQ(c.getLayerName(1), "Holding");
    const auto groups = c.getStatesByLayer();
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[1].second, std::vector<std::string>{"hold"});
    EXPECT_EQ(c.previewOverChoices(), std::vector<std::string>{"walk"});

    // the hold clip's Leg track is hidden by the arm mask (dimmed, with the reason)
    const std::vector<std::string> masked = c.maskedTracks();
    ASSERT_EQ(masked.size(), 2u);
    EXPECT_TRUE(masked[0].empty()) << "Arm is in the mask";
    EXPECT_EQ(masked[1], "masked out by arm.mask on layer Holding");

    c.setPreviewOver("walk");
    ASSERT_TRUE(c.startPreview(&error)) << error;
    c.seek(0.5f);
    EXPECT_NEAR(s.transform(s.arm).position.y, 3.0f, 1e-4f) << "the arm: the edited clip on its layer at weight 1";
    EXPECT_NEAR(s.transform(s.leg).position.x, 1.0f, 1e-4f) << "the leg: the base state (the mask hides the layer's)";
    c.setPreviewOver("");
    EXPECT_NEAR(s.transform(s.leg).position.x, 9.0f, 1e-4f) << "without Preview over the clip plays alone";
    c.stopPreview();
    EXPECT_NEAR(s.transform(s.leg).position.x, 0.0f, 1e-4f);
    EXPECT_NEAR(s.transform(s.arm).position.y, 0.0f, 1e-4f);
}
