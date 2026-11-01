// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator window (anim layers spec 11.4, 14 editor tests): the controller edits and the
// graph geometry it runs (AnimatorEdits), and the window driven through the functions its
// buttons, menus, drags and shortcuts call: create states, make a transition, set a condition,
// undo and redo (the file checked), a deleted state's transitions, a parameter rename that
// follows its conditions, a drag as one undo step, layers, warnings that select their item, the
// edit-mode preview on the bound GameObject, and play mode's live parameters (the runtime, not the
// file).

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "Scene.h"
#include "animation/AnimAssetDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorEdits.h"
#include "subsystem/ActionSystem.h"
#include "window/AnimatorWindow.h"

#include "imgui.h"

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

    anim::ControllerData twoStates(){
        anim::ControllerData data;
        data.parameters.push_back({"Speed", anim::ControllerParameterType::Float, 0.0f});
        data.parameters.push_back({"Jump", anim::ControllerParameterType::Trigger, 0.0f});
        anim::ControllerLayer layer;
        layer.name = anim::BASE_LAYER_NAME;
        anim::ControllerState idle;
        idle.name = "Idle";
        idle.clip = "idle.anim";
        anim::ControllerState run;
        run.name = "Run";
        run.hasBlendTree = true;
        run.blendTree.parameter = "Speed";
        run.speedParameter = "Speed";
        layer.states = {idle, run};
        anim::ControllerTransition go;
        go.from = "Idle";
        go.to = "Run";
        go.conditions.push_back({"Speed", anim::ConditionMode::Greater, 0.1f});
        go.conditions.push_back({"Jump", anim::ConditionMode::If, 0.0f});
        anim::ControllerTransition back;
        back.from = "Run";
        back.to = "Idle";
        anim::ControllerTransition any;
        any.from = anim::ANY_STATE;
        any.to = "Idle";
        layer.transitions = {go, back, any};
        layer.defaultState = "Idle";
        data.layers.push_back(layer);
        return data;
    }

}

TEST(AnimatorEdits, JsonPathsSelectTheirItems){
    using animator::ItemKind;
    EXPECT_EQ(animator::itemOfPath("layers[1].states[2].clip"), (animator::Item{ItemKind::State, 1, 2}));
    EXPECT_EQ(animator::itemOfPath("layers[0].transitions[3].conditions[0].mode"), (animator::Item{ItemKind::Transition, 0, 3}));
    EXPECT_EQ(animator::itemOfPath("parameters[4]"), (animator::Item{ItemKind::Parameter, -1, 4}));
    EXPECT_EQ(animator::itemOfPath("layers[2].weight"), (animator::Item{ItemKind::Layer, 2, -1}));
    EXPECT_EQ(animator::itemOfPath("layers[0].any_state.position"), (animator::Item{ItemKind::AnyState, 0, -1}));
    EXPECT_EQ(animator::itemOfPath("").kind, ItemKind::None);
    EXPECT_EQ(animator::itemOfPath("layers[x]").kind, ItemKind::None);
}

TEST(AnimatorEdits, RenamesAndRemovalsFollowTheirReferences){
    anim::ControllerData data = twoStates();
    std::string error;
    ASSERT_TRUE(animator::renameState(data, 0, 0, "Rest", &error)) << error;
    EXPECT_EQ(data.layers[0].transitions[0].from, "Rest");
    EXPECT_EQ(data.layers[0].transitions[1].to, "Rest");
    EXPECT_EQ(data.layers[0].transitions[2].to, "Rest");
    EXPECT_EQ(data.layers[0].defaultState, "Rest");
    EXPECT_FALSE(animator::renameState(data, 0, 0, "Run", &error)) << "taken";
    EXPECT_FALSE(animator::renameState(data, 0, 0, "any", &error)) << "reserved";

    ASSERT_TRUE(animator::renameParameter(data, 0, "Velocity", &error)) << error;
    EXPECT_EQ(data.layers[0].transitions[0].conditions[0].parameter, "Velocity");
    EXPECT_EQ(data.layers[0].states[1].blendTree.parameter, "Velocity");
    EXPECT_EQ(data.layers[0].states[1].speedParameter, "Velocity");
    EXPECT_FALSE(animator::renameParameter(data, 0, "Jump", &error));

    animator::removeParameter(data, 1);
    ASSERT_EQ(data.layers[0].transitions[0].conditions.size(), 1u) << "the conditions reading Jump go";

    animator::removeStates(data, 0, {0});
    ASSERT_EQ(data.layers[0].states.size(), 1u);
    EXPECT_TRUE(data.layers[0].transitions.empty()) << "every transition from or to the state goes";
    EXPECT_EQ(data.layers[0].defaultState, "Run") << "the default moves to the first state left";
    std::vector<ClipWarning> warnings;
    anim::ControllerData normalized = data;
    anim::normalizeController(normalized, &warnings);
    EXPECT_TRUE(warnings.empty()) << warnings[0].path << ": " << warnings[0].message;
}

TEST(AnimatorEdits, StatesTransitionsLayersAndPaste){
    anim::ControllerData data;
    data.layers.push_back(anim::ControllerLayer());
    data.layers[0].name = anim::BASE_LAYER_NAME;
    const int a = animator::addState(data, 0, anim::ControllerState());
    const int b = animator::addState(data, 0, anim::ControllerState());
    EXPECT_EQ(data.layers[0].states[(size_t)a].name, "New State");
    EXPECT_EQ(data.layers[0].states[(size_t)b].name, "New State 1");
    EXPECT_TRUE(data.layers[0].states[(size_t)b].hasPosition) << "a grid cell";
    EXPECT_FALSE(animator::stateRect(data.layers[0], a).overlaps(animator::stateRect(data.layers[0], b)));
    EXPECT_FALSE(animator::stateRect(data.layers[0], a).overlaps(animator::anyStateRect(data.layers[0])));
    const int t = animator::addTransition(data, 0, a, b);
    EXPECT_TRUE(data.layers[0].transitions[(size_t)t].hasExitTime) << "Unity's defaults";
    EXPECT_FLOAT_EQ(data.layers[0].transitions[(size_t)t].exitTime, 0.75f);
    const int any = animator::addTransition(data, 0, -1, b);
    EXPECT_EQ(data.layers[0].transitions[(size_t)any].from, anim::ANY_STATE);
    EXPECT_FALSE(data.layers[0].transitions[(size_t)any].hasExitTime);
    EXPECT_EQ(animator::addTransition(data, 0, a, 9), -1);

    std::string error;
    const int upper = animator::addLayer(data, "");
    EXPECT_EQ(data.layers[(size_t)upper].name, "New Layer");
    animator::addLayer(data, "Second");
    EXPECT_FALSE(animator::moveLayer(data, 1, 0, &error)) << "the base stays first";
    ASSERT_TRUE(animator::moveLayer(data, 2, 1, &error)) << error;
    EXPECT_EQ(data.layers[1].name, "Second");
    EXPECT_FALSE(animator::removeLayer(data, 0, &error));
    ASSERT_TRUE(animator::removeLayer(data, 2, &error));
    EXPECT_EQ(data.layers.size(), 2u);

    const animator::Clipboard clipboard = animator::copyStates(data, 0, {a, b});
    EXPECT_EQ(clipboard.transitions.size(), 1u) << "the Any State transition is not between copied states";
    const std::vector<int> pasted = animator::pasteStates(data, 0, clipboard, {600.0f, 400.0f});
    ASSERT_EQ(pasted.size(), 2u);
    EXPECT_EQ(data.layers[0].states[(size_t)pasted[0]].name, "New State 2");
    EXPECT_FLOAT_EQ(data.layers[0].states[(size_t)pasted[0]].position[0], 600.0f);
    EXPECT_EQ(data.layers[0].transitions.back().from, "New State 2");
    EXPECT_EQ(data.layers[0].transitions.back().to, "New State 3");
}

TEST(AnimatorEdits, GraphGeometry){
    anim::ControllerData data = twoStates();
    anim::ControllerLayer& layer = data.layers[0];
    layer.states[0].hasPosition = true;
    layer.states[0].position[0] = 300.0f;
    layer.states[0].position[1] = 100.0f;
    layer.states[1].hasPosition = true;
    layer.states[1].position[0] = 600.0f;
    layer.states[1].position[1] = 100.0f;
    layer.transitions.push_back(layer.transitions[0]);      // a second Idle -> Run
    const std::vector<animator::Arrow> arrows = animator::arrows(layer);
    ASSERT_EQ(arrows.size(), 3u);
    EXPECT_EQ(arrows[0].transitions.size(), 2u) << "one arrow with a x2 badge";
    // both directions: shifted apart
    EXPECT_NE(arrows[0].ay, arrows[1].ay);
    EXPECT_NEAR(arrows[0].ax, 300.0f + animator::STATE_WIDTH, 1e-3f) << "from the box's right edge";
    EXPECT_EQ(animator::hitState(layer, 310.0f, 110.0f), 0);
    EXPECT_EQ(animator::hitState(layer, animator::ANY_STATE_X + 5.0f, animator::ANY_STATE_Y + 5.0f), -2);
    EXPECT_EQ(animator::hitState(layer, 5.0f, 300.0f), -1);
    const float midX = (arrows[0].ax + arrows[0].bx) * 0.5f, midY = (arrows[0].ay + arrows[0].by) * 0.5f;
    EXPECT_EQ(animator::hitArrow(arrows, midX, midY, 3.0f), 0);
    EXPECT_EQ(animator::hitArrow(arrows, midX, midY + 60.0f, 3.0f), -1);
    EXPECT_EQ(animator::statesIn(layer, {250.0f, 50.0f, 200.0f, 200.0f}), std::vector<int>({0}));

    float panX = 0.0f, panY = 0.0f, zoom = 1.0f;
    animator::frame({0.0f, 0.0f, 1000.0f, 500.0f}, 500.0f, 500.0f, 0.0f, 0.2f, 2.0f, panX, panY, zoom);
    EXPECT_FLOAT_EQ(zoom, 0.5f);
    EXPECT_FLOAT_EQ(panX, 0.0f);
    EXPECT_FLOAT_EQ(panY, -250.0f) << "centred";

    anim::ControllerTransition transition;
    transition.exitTime = 0.5f;
    transition.duration = 0.25f;
    transition.fixedDuration = false;
    const animator::Timeline timeline = animator::transitionTimeline(transition, 2.0f, 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(timeline.exitStart, 1.0f);
    EXPECT_FLOAT_EQ(timeline.blend, 0.5f) << "a fraction of the source";
}

// ---- the window on a project ----------------------------------------------------------------

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
            const fs::path root = fs::temp_directory_path() / ("jasper-animator-window-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: animatorproject\nassetsDir: assets\n";
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
    fs::path assets() { return project()->getAssetsPath(); }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // Leg x 0 -> x over 1 s
    std::string writeClip(const std::string& name, float x) {
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        ClipTrackData track;
        track.path = "Leg";
        track.channel = ClipChannel::Position;
        ClipKey a;
        a.time = 0.0f;
        a.value = {0, 0, 0};
        ClipKey b;
        b.time = 1.0f;
        b.value = {x, 0, 0};
        track.keys = {a, b};
        data.tracks = {track};
        writeAll(assets() / ("animator/" + name + ".anim"), anim::writeClip(data));
        return "animator/" + name + ".anim";
    }

    // a fresh controller file (one Base Layer) and the window on it
    std::shared_ptr<AnimatorControllerDocument> freshController(AnimatorWindow& window, const std::string& name) {
        fs::remove(assets() / ("animator/" + name + ".controller"));
        anim::ControllerData data;
        data.layers.push_back(anim::ControllerLayer());
        data.layers[0].name = anim::BASE_LAYER_NAME;
        std::string error;
        std::shared_ptr<AnimatorControllerDocument> doc =
            AnimatorControllerDocument::create(project(), "assets/animator/" + name + ".controller", data, &error);
        EXPECT_TRUE(doc) << error;
        EXPECT_TRUE(window.openController("animator/" + name + ".controller", 0, NULL_ENTITY, &error)) << error;
        return doc;
    }

    struct PlayerScene {
        SceneProject* sceneProject = nullptr;
        Entity root = NULL_ENTITY;
        Entity leg = NULL_ENTITY;
    };

    PlayerScene playerScene(const std::string& controller) {
        PlayerScene out;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Animator", SceneType::SCENE_3D);
        out.sceneProject = p->getScene(p->getSelectedSceneId());
        Scene* scene = out.sceneProject->scene;
        out.root = scene->createUserEntity();
        scene->setEntityName(out.root, "Granjero");
        scene->addComponent<Transform>(out.root, {});
        out.leg = scene->createUserEntity();
        scene->setEntityName(out.leg, "Leg");
        scene->addComponent<Transform>(out.leg, {});
        scene->addEntityChild(out.root, out.leg, false);
        AnimationPlayerComponent player;
        player.controller = controller;
        scene->addComponent<AnimationPlayerComponent>(out.root, player);
        out.sceneProject->entities.push_back(out.root);
        out.sceneProject->entities.push_back(out.leg);
        return out;
    }

}

TEST(AnimatorWindow, CreateStatesMakeATransitionSetAConditionUndoRedo){
    ASSERT_TRUE(headless().ready);
    const std::string idle = writeClip("idle", 0.0f);
    const std::string run = writeClip("run", 4.0f);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "flow");
    ASSERT_TRUE(doc);
    const fs::path file = assets() / "animator/flow.controller";
    const std::string empty = readAll(file);

    const std::array<float, 2> at = {300.0f, 120.0f};
    ASSERT_EQ(window.createState(idle, false, &at), 0);
    ASSERT_EQ(window.createState(run, false, nullptr), 1);
    EXPECT_EQ(window.selectedStates(), std::vector<int>({1}));
    EXPECT_EQ(doc->data().layers[0].states[0].name, "idle");
    EXPECT_EQ(doc->data().layers[0].states[0].position[0], 300.0f - animator::STATE_WIDTH * 0.5f) << "centred on the point";
    EXPECT_TRUE(anim::isClipGuid(doc->data().layers[0].states[0].clipGuid)) << "an .anim gets its GUID";
    const int parameter = window.addParameter(anim::ControllerParameterType::Float);
    ASSERT_EQ(parameter, 0);
    ASSERT_TRUE(window.renameParameter(parameter, "Speed"));
    ASSERT_TRUE(window.makeTransition(0, 1));
    EXPECT_EQ(window.selection().kind, animator::ItemKind::Transition);
    ASSERT_TRUE(window.editTransition(0, [](anim::ControllerTransition& t){
        t.conditions.push_back({"Speed", anim::ConditionMode::Greater, 0.01f});
        t.hasExitTime = false;
        t.duration = 0.15f;
    }));
    const std::string done = readAll(file);
    EXPECT_NE(done.find("{\"from\": \"idle\", \"to\": \"run\", \"conditions\": [{\"parameter\": \"Speed\", \"mode\": \"greater\", \"threshold\": 0.01}], "
                        "\"has_exit_time\": false"), std::string::npos) << done;
    EXPECT_EQ(done, anim::writeController(doc->data()));

    // every edit was one project step: six undos go back to the empty controller
    CommandHistory* history = project()->getProjectCommandHistory();
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(doc->data().layers[0].transitions[0].conditions.empty());
    for (int i = 0; i < 5; i++) ASSERT_TRUE(history->undo()) << i;
    EXPECT_EQ(readAll(file), empty);
    EXPECT_TRUE(doc->data().layers[0].states.empty());
    for (int i = 0; i < 6; i++) ASSERT_TRUE(history->redo()) << i;
    EXPECT_EQ(readAll(file), done);
}

TEST(AnimatorWindow, DeletesRenamesDragsAndLayers){
    ASSERT_TRUE(headless().ready);
    const std::string idle = writeClip("idle", 0.0f);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "edits");
    ASSERT_TRUE(doc);
    window.createState(idle, false, nullptr);
    window.createState("", false, nullptr);
    window.createState("", true, nullptr);
    ASSERT_EQ(doc->data().layers[0].states.size(), 3u);
    EXPECT_EQ(doc->data().parameters.size(), 1u) << "a new blend tree makes a Blend parameter";
    EXPECT_EQ(doc->data().layers[0].states[2].blendTree.parameter, "Blend");
    window.makeTransition(0, 1);
    window.makeTransition(1, 0);
    window.makeTransition(-1, 2);
    ASSERT_TRUE(window.editTransition(2, [](anim::ControllerTransition& t){ t.conditions.push_back({"Blend", anim::ConditionMode::Greater, 0.5f}); }));

    // a parameter rename follows into the conditions and the tree
    ASSERT_TRUE(window.renameParameter(0, "Speed"));
    EXPECT_EQ(doc->data().layers[0].transitions[2].conditions[0].parameter, "Speed");
    EXPECT_EQ(doc->data().layers[0].states[2].blendTree.parameter, "Speed");
    EXPECT_FALSE(window.renameParameter(0, ""));
    EXPECT_FALSE(window.notice().empty());

    // a drag is one undo step, however many moves it has
    const float x0 = doc->data().layers[0].states[0].position[0];
    window.selectStates({0});
    window.beginDrag();
    ASSERT_TRUE(window.dragStates({0}, 10.0f, 0.0f));
    ASSERT_TRUE(window.dragStates({0}, 40.0f, 5.0f));
    window.endDrag();
    EXPECT_EQ(doc->data().layers[0].states[0].position[0], x0 + 50.0f);
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_EQ(doc->data().layers[0].states[0].position[0], x0);
    ASSERT_TRUE(project()->getProjectCommandHistory()->redo());

    // deleting a state removes its transitions
    window.selectStates({1});
    ASSERT_TRUE(window.deleteSelection());
    EXPECT_EQ(doc->data().layers[0].states.size(), 2u);
    ASSERT_EQ(doc->data().layers[0].transitions.size(), 1u);
    EXPECT_EQ(doc->data().layers[0].transitions[0].from, anim::ANY_STATE);
    window.select({animator::ItemKind::Transition, 0, 0});
    ASSERT_TRUE(window.deleteSelection());
    EXPECT_TRUE(doc->data().layers[0].transitions.empty());

    // copy and paste
    window.selectStates({0});
    ASSERT_TRUE(window.copySelection());
    ASSERT_TRUE(window.paste({900.0f, 300.0f}));
    EXPECT_EQ(doc->data().layers[0].states.size(), 3u);
    EXPECT_EQ(doc->data().layers[0].states[2].name, "idle 1");

    // layers
    const int upper = window.addLayer();
    ASSERT_EQ(upper, 1);
    EXPECT_EQ(window.currentLayer(), 1);
    ASSERT_TRUE(window.renameLayer(1, "Holding"));
    ASSERT_TRUE(window.editLayer(1, [](anim::ControllerLayer& layer){ layer.weight = 0.5f; }));
    EXPECT_EQ(doc->data().layers[1].weight, 0.5f);
    EXPECT_FALSE(window.removeLayer(0));
    ASSERT_TRUE(window.removeLayer(1));
    EXPECT_EQ(window.currentLayer(), 0);
    ASSERT_TRUE(window.setDefaultState(1));
    EXPECT_EQ(doc->data().layers[0].defaultState, doc->data().layers[0].states[1].name);
}

TEST(AnimatorWindow, WarningsSelectTheirItem){
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "warned");
    ASSERT_TRUE(doc);
    window.createState("animator/missing.anim", false, nullptr);
    window.createState("", false, nullptr);
    const std::vector<ClipWarning> warnings = window.warnings();
    const ClipWarning* missing = nullptr;
    for (const ClipWarning& warning : warnings){
        if (warning.message.find("does not load") != std::string::npos) missing = &warning;
    }
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->path, "layers[0].states[0].clip");
    window.clearSelection();
    window.selectPath(missing->path);
    EXPECT_EQ(window.selection(), (animator::Item{animator::ItemKind::State, 0, 0}));
    EXPECT_EQ(window.selectedStates(), std::vector<int>({0}));
}

TEST(AnimatorWindow, PreviewsOnTheBoundGameObjectAndDrivesLiveParameters){
    ASSERT_TRUE(headless().ready);
    const std::string run = writeClip("run", 4.0f);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "live");
    ASSERT_TRUE(doc);
    window.createState(run, false, nullptr);
    window.addParameter(anim::ControllerParameterType::Float);
    const std::string text = readAll(assets() / "animator/live.controller");

    PlayerScene s = playerScene("animator/live.controller");
    std::string error;
    ASSERT_TRUE(window.openController("animator/live.controller", s.sceneProject->id, s.root, &error)) << error;
    EXPECT_EQ(window.boundEntity(), s.root);

    // edit-mode preview: the state's clip on the GameObject, restored on stop
    window.selectStates({0});
    ASSERT_TRUE(window.startPreview()) << window.notice();
    window.updatePreview(0.5f);
    EXPECT_NEAR(s.sceneProject->scene->getComponent<Transform>(s.leg).position.x, 2.0f, 1e-4f);
    window.stopPreview();
    EXPECT_NEAR(s.sceneProject->scene->getComponent<Transform>(s.leg).position.x, 0.0f, 1e-5f);

    // play mode: the parameters are the runtime's; editing them does not touch the file
    AnimationPlayerComponent& player = s.sceneProject->scene->getComponent<AnimationPlayerComponent>(s.root);
    ASSERT_TRUE(anim::ensureController(player, "Granjero", 0.0f, false));
    s.sceneProject->playState = ScenePlayState::PLAYING;
    EXPECT_TRUE(window.isLive());
    const std::string name = doc->data().parameters[0].name;
    ASSERT_TRUE(window.setLiveParameter(name, 2.5f));
    EXPECT_FLOAT_EQ(anim::getControllerParameter(player, name, anim::ControllerParameterType::Float, "Granjero"), 2.5f);
    EXPECT_FALSE(window.startPreview()) << "no edit-mode preview while it plays";
    s.sceneProject->playState = ScenePlayState::STOPPED;
    EXPECT_EQ(readAll(assets() / "animator/live.controller"), text);
    EXPECT_FALSE(window.isLive());

    // following the selection: the selected GameObject's controller
    AnimatorWindow follower(project());
    project()->setSelectedEntity(s.sceneProject->id, s.leg);
    follower.followSelection();
    ASSERT_TRUE(follower.document());
    EXPECT_EQ(follower.document()->path(), doc->path());
    EXPECT_EQ(follower.boundEntity(), s.root) << "the player of the selection's ancestor";
}

// every panel and selection draws in a frame (edit mode, a preview, play mode's live view)
TEST(AnimatorWindow, EveryPanelDrawsInAFrame){
    ASSERT_TRUE(headless().ready);
    const std::string idle = writeClip("idle", 0.0f);
    const std::string run = writeClip("run", 4.0f);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "drawn");
    ASSERT_TRUE(doc);
    window.createState(idle, false, nullptr);
    window.createState(run, false, nullptr);
    const int tree = window.createState("", true, nullptr);
    ASSERT_TRUE(window.editState(tree, [&](anim::ControllerState& state){
        state.blendTree.children = {anim::BlendTreeChild(), anim::BlendTreeChild()};
        state.blendTree.children[0].clip = idle;
        state.blendTree.children[1].clip = run;
        state.blendTree.children[1].threshold = 4.0f;
    }));
    window.addParameter(anim::ControllerParameterType::Bool);
    window.addParameter(anim::ControllerParameterType::Trigger);
    window.addParameter(anim::ControllerParameterType::Int);
    window.makeTransition(0, 1);
    window.makeTransition(0, 1);
    window.makeTransition(1, 0);
    window.makeTransition(-1, 2);
    window.makeTransition(2, 2);
    window.editTransition(0, [](anim::ControllerTransition& t){ t.conditions.push_back({"Blend", anim::ConditionMode::Greater, 1.0f}); });
    window.addLayer();
    window.setCurrentLayer(0);

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1400, 900);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&](){
        ImGui::NewFrame();
        window.show();
        ImGui::EndFrame();
    };
    window.setOpen(true);
    frame();
    frame();
    EXPECT_GT(window.zoom(), 0.0f);
    const std::vector<animator::Item> items = {
        {animator::ItemKind::State, 0, 0}, {animator::ItemKind::State, 0, tree}, {animator::ItemKind::Transition, 0, 0},
        {animator::ItemKind::Transition, 0, 3}, {animator::ItemKind::AnyState, 0, -1}, {animator::ItemKind::Parameter, -1, 1}};
    for (const animator::Item& item : items){
        window.select(item);
        frame();
    }
    window.frameAll();
    window.frameSelection();
    window.setCurrentLayer(1);
    frame();
    window.setCurrentLayer(0);

    // bound: a preview, then the live view in play mode
    PlayerScene s = playerScene("animator/drawn.controller");
    std::string error;
    ASSERT_TRUE(window.openController("animator/drawn.controller", s.sceneProject->id, s.root, &error)) << error;
    window.select({animator::ItemKind::State, 0, tree});
    window.setBlendValue(2.0f);
    ASSERT_TRUE(window.startPreview()) << window.notice();
    frame();
    frame();
    EXPECT_GT(s.sceneProject->scene->getComponent<Transform>(s.leg).position.x, 0.0f) << "the tree previews at its value";
    window.stopPreview();
    AnimationPlayerComponent& player = s.sceneProject->scene->getComponent<AnimationPlayerComponent>(s.root);
    ASSERT_TRUE(anim::ensureController(player, "Granjero", 0.0f, false));
    s.sceneProject->scene->getSystem<ActionSystem>()->setPaused(false);
    s.sceneProject->scene->getSystem<ActionSystem>()->update(0.1);
    s.sceneProject->playState = ScenePlayState::PLAYING;
    ASSERT_TRUE(window.isLive());
    frame();
    window.select({animator::ItemKind::Parameter, -1, 0});
    frame();
    s.sceneProject->playState = ScenePlayState::STOPPED;
    s.sceneProject->scene->getSystem<ActionSystem>()->setPaused(true);
    window.setOpen(false);
    frame();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}

// a threshold moved past its neighbour: the child keeps its clip and lands at its sorted place;
// typed paths are checked and normalized
TEST(AnimatorWindow, ThresholdsReSortByChildAndTypedPathsAreChecked){
    ASSERT_TRUE(headless().ready);
    const std::string a = writeClip("a", 1.0f);
    const std::string b = writeClip("b", 2.0f);
    const std::string c = writeClip("c", 3.0f);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "thresholds");
    ASSERT_TRUE(doc);
    const int tree = window.createState("", true, nullptr);
    ASSERT_TRUE(window.editState(tree, [&](anim::ControllerState& state){
        state.blendTree.children = {anim::BlendTreeChild(), anim::BlendTreeChild(), anim::BlendTreeChild()};
        state.blendTree.children[0].clip = a;
        state.blendTree.children[1].clip = b;
        state.blendTree.children[1].threshold = 1.0f;
        state.blendTree.children[2].clip = c;
        state.blendTree.children[2].threshold = 2.0f;
    }));
    EXPECT_EQ(window.setBlendThreshold(tree, 0, 1.5f), 1);
    const std::vector<anim::BlendTreeChild>& children = doc->data().layers[0].states[(size_t)tree].blendTree.children;
    ASSERT_EQ(children.size(), 3u);
    EXPECT_EQ(children[0].clip, b);
    EXPECT_EQ(children[1].clip, a) << "the moved child, past its neighbour";
    EXPECT_FLOAT_EQ(children[1].threshold, 1.5f);
    EXPECT_EQ(window.setBlendThreshold(tree, 1, 9.0f), 2);
    EXPECT_EQ(doc->data().layers[0].states[(size_t)tree].blendTree.children[2].clip, a);

    std::string value, guid;
    ASSERT_TRUE(window.resolveTypedAsset("assets/" + a, true, value, guid)) << window.notice();
    EXPECT_EQ(value, a) << "a project-relative path is stored asset-relative";
    EXPECT_TRUE(anim::isClipGuid(guid));
    EXPECT_FALSE(window.resolveTypedAsset("animator/nope.anim", true, value, guid));
    EXPECT_FALSE(window.notice().empty());
    EXPECT_FALSE(window.resolveTypedAsset(a, false, value, guid)) << "a clip is not a mask";
    EXPECT_TRUE(window.resolveTypedAsset("", true, value, guid));
    EXPECT_TRUE(value.empty());
}

// Delete acts only when the Animator window has the focus (not when another window does)
TEST(AnimatorWindow, ShortcutsNeedTheWindowFocused){
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "keys");
    ASSERT_TRUE(doc);
    window.createState("", false, nullptr);
    window.createState("", false, nullptr);
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1400, 900);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool focusOther = false;
    auto frame = [&](){
        ImGui::NewFrame();
        window.show();
        if (focusOther) ImGui::SetNextWindowFocus();
        focusOther = false;
        ImGui::Begin("Other");
        ImGui::End();
        ImGui::EndFrame();
    };
    auto pressDelete = [&](){
        io.AddKeyEvent(ImGuiKey_Delete, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Delete, false);
        frame();
    };
    window.setOpen(true);
    frame();
    focusOther = true;
    frame();
    frame();
    EXPECT_FALSE(window.isFocused());
    window.selectStates({0});
    pressDelete();
    EXPECT_EQ(doc->data().layers[0].states.size(), 2u) << "another window has the focus";
    window.requestFocus();
    frame();
    frame();
    EXPECT_TRUE(window.isFocused());
    window.selectStates({0});
    pressDelete();
    EXPECT_EQ(doc->data().layers[0].states.size(), 1u);
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}

// a layer list change (remove, move, their undo and redo) keeps the current layer by name and drops
// a selection of the old indices: Delete never reaches another layer
TEST(AnimatorWindow, LayerListChangesDropStaleSelections){
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "layerlist");
    ASSERT_TRUE(doc);
    window.createState("", false, nullptr);                 // Base Layer: New State
    ASSERT_EQ(window.addLayer(), 1);
    window.createState("", false, nullptr);                 // New Layer: New State
    ASSERT_EQ(window.addLayer(), 2);
    ASSERT_TRUE(window.renameLayer(2, "Upper"));
    EXPECT_EQ(window.currentLayer(), 2) << "a rename keeps the layer";
    window.createState("", false, nullptr);                 // Upper: New State
    window.selectStates({0});
    EXPECT_EQ(window.selection().layer, 2);

    ASSERT_TRUE(window.removeLayer(1));
    EXPECT_EQ(window.currentLayer(), 1) << "Upper, now index 1, stays current";
    EXPECT_EQ(window.selection().kind, animator::ItemKind::None) << "the old indices' selection goes";
    window.selectStates({0});
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_EQ(window.currentLayer(), 2) << "after the undo Upper is index 2 again";
    EXPECT_EQ(window.selection().kind, animator::ItemKind::None);
    EXPECT_FALSE(window.deleteSelection()) << "nothing selected: nothing deleted";
    for (const anim::ControllerLayer& layer : doc->data().layers) EXPECT_EQ(layer.states.size(), 1u) << layer.name;
    ASSERT_TRUE(project()->getProjectCommandHistory()->redo());
    EXPECT_EQ(window.currentLayer(), 1);

    // removing the current layer falls back to the base
    ASSERT_TRUE(window.removeLayer(1));
    EXPECT_EQ(window.currentLayer(), 0);
    EXPECT_EQ(doc->data().layers.size(), 1u);
}

// the bound player's controller field changes (the Inspector, MCP, an undo): the window follows it
TEST(AnimatorWindow, FollowsTheBoundPlayersControllerField){
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> first = freshController(window, "fieldA");
    std::shared_ptr<AnimatorControllerDocument> second = freshController(window, "fieldB");
    ASSERT_TRUE(first && second);
    PlayerScene s = playerScene("animator/fieldA.controller");
    std::string error;
    ASSERT_TRUE(window.openController("animator/fieldA.controller", s.sceneProject->id, s.root, &error)) << error;
    window.followBoundController();
    EXPECT_EQ(window.document(), first);
    AnimationPlayerComponent& player = s.sceneProject->scene->getComponent<AnimationPlayerComponent>(s.root);
    player.controller = "animator/fieldB.controller";
    window.followBoundController();
    EXPECT_EQ(window.document(), second);
    EXPECT_EQ(window.boundEntity(), s.root);
    player.controller.clear();
    window.followBoundController();
    EXPECT_FALSE(window.document());
    EXPECT_NE(window.openError().find("no Animator Controller"), std::string::npos) << window.openError();
    player.controller = "animator/fieldA.controller";
    window.followBoundController();
    EXPECT_EQ(window.document(), first);
}

// parameter names are Lua identifiers: the window refuses others with a notice, never a silent drop
TEST(AnimatorWindow, ParameterNamesAreLuaIdentifiers){
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project());
    std::shared_ptr<AnimatorControllerDocument> doc = freshController(window, "names");
    ASSERT_TRUE(doc);
    ASSERT_EQ(window.addParameter(anim::ControllerParameterType::Float), 0);
    ASSERT_EQ(window.addParameter(anim::ControllerParameterType::Float), 1);
    EXPECT_EQ(doc->data().parameters[0].name, "NewFloat");
    EXPECT_EQ(doc->data().parameters[1].name, "NewFloat1") << "numbered without a space";
    const std::string before = doc->data().parameters[0].name;
    EXPECT_FALSE(window.renameParameter(0, "Move Speed"));
    EXPECT_NE(window.notice().find("not a valid parameter name"), std::string::npos) << window.notice();
    EXPECT_FALSE(window.renameParameter(0, "2fast"));
    EXPECT_FALSE(window.renameParameter(0, "end")) << "a Lua keyword";
    ASSERT_EQ(doc->data().parameters.size(), 2u) << "nothing dropped";
    EXPECT_EQ(doc->data().parameters[0].name, before);
    EXPECT_TRUE(window.renameParameter(0, "move_speed"));
    anim::ControllerData data = doc->data();
    EXPECT_EQ(animator::addParameter(data, anim::ControllerParameterType::Bool, "not valid"), -1);
}
