// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator window, driven through the functions its buttons, menus, fields, drags and
// shortcuts call (upstream anim layers spec 11.4, 14 editor tests, adapted to S21's window:
// docs/specs/S21-animation.md "Upstream absorption"): create states, make a transition, set a
// condition, undo and redo (the file checked); a deleted state's transitions; renames that follow
// their references (states, parameters, layers); a drag as one undo step; copy and paste; layers;
// problems that select their item; the edit-mode preview on the bound Animator and play mode's
// live parameters (the runtime, not the file); every panel drawn in a frame; 1D thresholds kept
// in order with their motions; typed paths checked; Delete only with the window focused; layer
// list changes dropping stale selections; the pinned Animator's controller field followed; and
// parameter names (any name, a non-Lua one warned).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorInstance.h"
#include "command/CommandHistory.h"
#include "component/AnimatorComponent.h"
#include "component/Transform.h"
#include "subsystem/AnimatorSystem.h"
#include "window/AnimatorWindow.h"

#include "imgui.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
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
    CommandHistory* history() { return project()->getProjectCommandHistory(); }

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

    // Leg x 0 -> x over 1 s (looping), asset-relative
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
        // the database gives the new file its GUID (written into it)
        project()->refreshAssetDatabase(true);
        return "animator/" + name + ".anim";
    }

    anim::MotionDef clipMotion(AnimatorWindow& window, const std::string& clip) {
        anim::MotionDef motion;
        EXPECT_TRUE(window.resolveTypedAsset(clip, true, motion.clip)) << window.getStatus();
        return motion;
    }

    anim::MotionDef treeMotion(const std::string& parameter) {
        anim::MotionDef motion;
        anim::BlendTreeDef tree;
        tree.parameter = parameter;
        motion.tree.push_back(tree);
        return motion;
    }

    // a fresh controller file (one Base Layer) and the window on it
    std::shared_ptr<AnimatorAssetDocument> freshController(AnimatorWindow& window, const std::string& name) {
        // (the window follows a selected GameObject's Animator: an earlier test's selection goes)
        if (SceneProject* selected = project()->getSelectedScene()) project()->clearSelectedEntities(selected->id);
        fs::remove(assets() / ("animator/" + name + ".controller"));
        std::string error;
        std::shared_ptr<AnimatorAssetDocument> doc = AnimatorAssetDocument::create(
            project(), "assets/animator/" + name + ".controller", AnimatorAssetDocument::Kind::Controller, "Create Animator Controller", &error);
        EXPECT_TRUE(doc) << error;
        window.openController("animator/" + name + ".controller", 0, NULL_ENTITY);
        EXPECT_EQ(window.getDocument(), doc);
        return doc;
    }

    const anim::StateMachineDef& machine(const std::shared_ptr<AnimatorAssetDocument>& doc, size_t layer = 0) {
        return doc->controller().layers[layer].machine;
    }

    // Granjero (an Animator playing `controller`) > Leg
    struct AnimatorScene {
        SceneProject* sceneProject = nullptr;
        Entity root = NULL_ENTITY;
        Entity leg = NULL_ENTITY;
        Scene* scene() const { return sceneProject->scene; }
        AnimatorComponent& animator() const { return scene()->getComponent<AnimatorComponent>(root); }
        float legX() const { return scene()->getComponent<Transform>(leg).position.x; }
    };

    AnimatorScene animatorScene(const std::string& controller) {
        AnimatorScene out;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()){
            current->isModified = false;
            current->playState = ScenePlayState::STOPPED;      // (a failed test may have left it playing)
        }
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
        AnimatorComponent animator;
        animator.controller = AssetRef(controller);
        scene->addComponent<AnimatorComponent>(out.root, animator);
        out.sceneProject->entities.push_back(out.root);
        out.sceneProject->entities.push_back(out.leg);
        return out;
    }

    // an ImGui frame loop of its own (the window's show(), an Inspector, another window)
    struct Frames {
        ImGuiContext* previous = nullptr;
        ImGuiContext* context = nullptr;
        Frames() {
            previous = ImGui::GetCurrentContext();
            context = ImGui::CreateContext();
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1400, 900);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ~Frames() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }
        void frame(AnimatorWindow& window, const std::function<void()>& extra = nullptr) {
            ImGui::NewFrame();
            window.show();
            ImGui::Begin("Inspector");
            window.drawSelectionInspector();
            ImGui::End();
            if (extra) extra();
            ImGui::EndFrame();
        }
    };

}

TEST(AnimatorWindow, CreateStatesMakeATransitionSetAConditionUndoRedo) {
    ASSERT_TRUE(headless().ready);
    const std::string idle = writeClip("idle", 0.0f);
    const std::string run = writeClip("run", 4.0f);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "flow");
    ASSERT_TRUE(doc);
    const fs::path file = assets() / "animator/flow.controller";
    const std::string empty = readAll(file);
    const size_t start = history()->position();

    ASSERT_TRUE(window.addState("idle", ImVec2(300, 120), clipMotion(window, idle)));
    ASSERT_TRUE(window.addState("run", ImVec2(600, 120), clipMotion(window, run)));
    EXPECT_EQ(window.describeSelection(), "state:run");
    EXPECT_EQ(machine(doc).states[0].position.x, 300.0f);
    EXPECT_TRUE(anim::isClipGuid(machine(doc).states[0].motion.clip.guid)) << "an .anim gets its GUID";
    EXPECT_EQ(machine(doc).defaultState, "idle") << "the first state is the default";
    ASSERT_TRUE(window.addParameter("Speed", AnimatorParameterType::Float));
    ASSERT_TRUE(window.addTransition("idle", "run", false, false));
    EXPECT_EQ(window.describeSelection(), "transition:state:idle:0");
    // the Transition Inspector's edit (one step)
    anim::ControllerDef edited = doc->controller();
    anim::TransitionDef& t = edited.layers[0].machine.states[0].transitions[0];
    anim::ConditionDef condition;
    condition.parameter = "Speed";
    condition.mode = AnimatorConditionMode::Greater;
    condition.threshold = 0.01f;
    t.conditions.push_back(condition);
    t.hasExitTime = false;
    t.duration = 0.15f;
    std::string error;
    ASSERT_TRUE(doc->commit(edited, "Edit Transition idle -> run", "", &error)) << error;
    const std::string done = readAll(file);
    const anim::ControllerParseResult parsed = anim::parseController(done);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    ASSERT_EQ(parsed.data.layers[0].machine.states[0].transitions.size(), 1u);
    EXPECT_EQ(parsed.data.layers[0].machine.states[0].transitions[0].conditions[0].parameter, "Speed");
    EXPECT_FALSE(parsed.data.layers[0].machine.states[0].transitions[0].hasExitTime);
    EXPECT_EQ(done, anim::writeController(doc->controller()));
    EXPECT_TRUE(window.problems().empty()) << window.problems()[0].path << ": " << window.problems()[0].message;

    // every edit was one project step: five undos go back to the empty controller
    ASSERT_EQ(history()->position(), start + 5);
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(machine(doc).states[0].transitions[0].conditions.empty());
    for (int i = 0; i < 4; i++) ASSERT_TRUE(history()->undo()) << i;
    EXPECT_EQ(readAll(file), empty);
    EXPECT_TRUE(machine(doc).states.empty());
    for (int i = 0; i < 5; i++) ASSERT_TRUE(history()->redo()) << i;
    EXPECT_EQ(readAll(file), done);
}

TEST(AnimatorWindow, DeletesRenamesDragsCopyPasteAndLayers) {
    ASSERT_TRUE(headless().ready);
    const std::string idle = writeClip("idle", 0.0f);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "edits");
    ASSERT_TRUE(doc);
    ASSERT_TRUE(window.addParameter("Blend", AnimatorParameterType::Float));
    ASSERT_TRUE(window.addState("idle", ImVec2(200, 100), clipMotion(window, idle)));
    ASSERT_TRUE(window.addState("", ImVec2(400, 100), anim::MotionDef()));
    ASSERT_TRUE(window.addState("Blend Tree", ImVec2(600, 100), treeMotion("Blend")));
    ASSERT_EQ(machine(doc).states.size(), 3u);
    EXPECT_EQ(machine(doc).states[1].name, "New State");
    ASSERT_TRUE(window.addTransition("idle", "New State", false, false));
    ASSERT_TRUE(window.addTransition("New State", "idle", false, false));
    ASSERT_TRUE(window.addTransition("", "Blend Tree", true, false));
    anim::ControllerDef edited = doc->controller();
    anim::ConditionDef condition;
    condition.parameter = "Blend";
    condition.mode = AnimatorConditionMode::Greater;
    condition.threshold = 0.5f;
    edited.layers[0].machine.anyState[0].conditions.push_back(condition);
    edited.layers[0].machine.states[0].speedParameter = "Blend";
    std::string error;
    ASSERT_TRUE(doc->commit(edited, "Edit", "", &error)) << error;

    // a parameter rename follows into the conditions, the tree and the state's parameters
    ASSERT_TRUE(window.renameParameter("Blend", "Speed"));
    EXPECT_EQ(machine(doc).anyState[0].conditions[0].parameter, "Speed");
    EXPECT_EQ(machine(doc).states[2].motion.tree[0].parameter, "Speed");
    EXPECT_EQ(machine(doc).states[0].speedParameter, "Speed");
    EXPECT_FALSE(window.renameParameter("Speed", ""));
    EXPECT_FALSE(window.getStatus().empty());
    ASSERT_TRUE(window.addParameter("Jump", AnimatorParameterType::Trigger));
    EXPECT_FALSE(window.renameParameter("Speed", "Jump")) << "taken";
    // a state rename follows into the transitions and the default
    ASSERT_TRUE(window.renameState("idle", "Rest"));
    EXPECT_EQ(machine(doc).states[1].transitions[0].to, "Rest");
    EXPECT_EQ(machine(doc).defaultState, "Rest");
    EXPECT_FALSE(window.renameState("Rest", "Any State")) << "reserved";
    ASSERT_TRUE(window.renameState("Rest", "idle"));
    // deleting a parameter takes the conditions that read it
    anim::ControllerDef withJump = doc->controller();
    anim::ConditionDef jump;
    jump.parameter = "Jump";
    jump.mode = AnimatorConditionMode::If;
    withJump.layers[0].machine.anyState[0].conditions.push_back(jump);
    ASSERT_TRUE(doc->commit(withJump, "Edit", "", &error)) << error;
    ASSERT_TRUE(window.removeParameter("Jump"));
    ASSERT_EQ(machine(doc).anyState[0].conditions.size(), 1u) << "the conditions reading Jump go";

    // a drag is one undo step, however many moves it has
    const size_t beforeDrag = history()->position();
    const float x0 = machine(doc).states[0].position.x;
    ASSERT_TRUE(window.moveNodes({{"idle", ImVec2(x0 + 10.0f, 100.0f)}}));
    ASSERT_TRUE(window.moveNodes({{"idle", ImVec2(x0 + 50.0f, 105.0f)}, {"Any State", ImVec2(60, 50)}}));
    EXPECT_EQ(machine(doc).states[0].position.x, x0 + 50.0f);
    EXPECT_EQ(history()->position(), beforeDrag + 1);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(machine(doc).states[0].position.x, x0);
    ASSERT_TRUE(history()->redo());

    // deleting a state removes its transitions and the ones into it
    window.selectState("New State");
    ASSERT_TRUE(window.removeSelection());
    EXPECT_EQ(machine(doc).states.size(), 2u);
    EXPECT_TRUE(machine(doc).states[0].transitions.empty()) << "idle -> New State went with it";
    ASSERT_EQ(machine(doc).anyState.size(), 1u);
    window.selectTransition("", 0, true);
    ASSERT_TRUE(window.removeSelection());
    EXPECT_TRUE(machine(doc).anyState.empty());
    EXPECT_FALSE(window.removeSelection()) << "nothing selected";

    // copy and paste (a self transition comes along, the others do not)
    ASSERT_TRUE(window.addTransition("idle", "idle", false, false));
    ASSERT_TRUE(window.addTransition("idle", "Blend Tree", false, false));
    window.selectState("idle");
    ASSERT_TRUE(window.copySelection());
    ASSERT_TRUE(window.paste(ImVec2(900, 300)));
    ASSERT_EQ(machine(doc).states.size(), 3u);
    EXPECT_EQ(machine(doc).states[2].name, "idle 1");
    EXPECT_EQ(machine(doc).states[2].position.x, 900.0f);
    EXPECT_EQ(machine(doc).states[2].motion.clip.path, idle);
    ASSERT_EQ(machine(doc).states[2].transitions.size(), 1u);
    EXPECT_EQ(machine(doc).states[2].transitions[0].to, "idle 1");
    EXPECT_EQ(window.describeSelection(), "state:idle 1");

    // layers
    ASSERT_TRUE(window.addLayer("New Layer"));
    EXPECT_EQ(window.getLayer(), 1);
    ASSERT_TRUE(window.renameLayer(1, "Holding"));
    EXPECT_FALSE(window.renameLayer(1, anim::BASE_LAYER_NAME)) << "taken";
    anim::ControllerDef weighted = doc->controller();
    weighted.layers[1].weight = 0.5f;
    ASSERT_TRUE(doc->commit(weighted, "Edit Layer Holding", "", &error)) << error;
    EXPECT_EQ(doc->controller().layers[1].weight, 0.5f);
    EXPECT_EQ(window.getLayer(), 1) << "an edit of the layer keeps it current";
    ASSERT_TRUE(window.removeLayer(1));
    EXPECT_EQ(window.getLayer(), 0);
    EXPECT_FALSE(window.removeLayer(0)) << "a controller keeps one layer";
    ASSERT_TRUE(window.setDefaultState("Blend Tree"));
    EXPECT_EQ(machine(doc).defaultState, "Blend Tree");
}

TEST(AnimatorWindow, ProblemsSelectTheirItem) {
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "warned");
    ASSERT_TRUE(doc);
    anim::MotionDef missing;
    missing.clip.path = "animator/missing.anim";
    ASSERT_TRUE(window.addState("Missing", ImVec2(200, 100), missing));
    ASSERT_TRUE(window.addState("Other", ImVec2(400, 100), anim::MotionDef()));
    ASSERT_TRUE(window.addTransition("Missing", "Other", false, false));
    anim::ControllerDef edited = doc->controller();
    edited.layers[0].machine.states[0].transitions[0].hasExitTime = false;
    std::string error;
    ASSERT_TRUE(doc->commit(edited, "Edit", "", &error)) << error;

    const std::vector<ClipWarning> problems = window.problems();
    const ClipWarning* notLoading = nullptr;
    const ClipWarning* neverTaken = nullptr;
    for (const ClipWarning& warning : problems){
        if (warning.message.find("does not load") != std::string::npos) notLoading = &warning;
        if (warning.message.find("never taken") != std::string::npos) neverTaken = &warning;
    }
    ASSERT_TRUE(notLoading);
    EXPECT_EQ(notLoading->path, "layers[0].state_machine.states[0].motion.clip");
    ASSERT_TRUE(neverTaken);
    EXPECT_EQ(neverTaken->path, "layers[0].state_machine.states[0].transitions[0]");
    window.selectState("Other");
    ASSERT_TRUE(window.inspect(notLoading->path));
    EXPECT_EQ(window.describeSelection(), "state:Missing");
    ASSERT_TRUE(window.inspect(neverTaken->path));
    EXPECT_EQ(window.describeSelection(), "transition:state:Missing:0");
    // other paths: a layer, an Any State transition, a parameter, a sub-machine's state
    ASSERT_TRUE(window.addLayer("Upper"));
    ASSERT_TRUE(window.selectPath("layers[1].weight"));
    EXPECT_EQ(window.getLayer(), 1);
    EXPECT_EQ(window.describeSelection(), "layer");
    ASSERT_TRUE(window.selectPath("layers[0].state_machine.any_state[0].conditions[0].mode"));
    EXPECT_EQ(window.getLayer(), 0);
    EXPECT_EQ(window.describeSelection(), "transition:any_state::0");
    ASSERT_TRUE(window.addSubStateMachine("Combat", ImVec2(300, 300)));
    anim::ControllerDef nested = doc->controller();
    anim::StateDef inside;
    inside.name = "Punch";
    nested.layers[0].machine.subMachines[0].states.push_back(inside);
    ASSERT_TRUE(doc->commit(nested, "Edit", "", &error)) << error;
    ASSERT_TRUE(window.selectPath("layers[0].state_machine.sub_state_machines[0].states[0].motion"));
    EXPECT_EQ(window.getMachinePath(), std::vector<std::string>{"Combat"});
    EXPECT_EQ(window.describeSelection(), "state:Punch");
    EXPECT_FALSE(window.selectPath("layers[9].weight"));
}

TEST(AnimatorWindow, PreviewsOnTheBoundAnimatorAndDrivesLiveParameters) {
    ASSERT_TRUE(headless().ready);
    const std::string run = writeClip("liverun", 4.0f);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "live");
    ASSERT_TRUE(doc);
    ASSERT_TRUE(window.addState("run", ImVec2(300, 100), clipMotion(window, run)));
    ASSERT_TRUE(window.addParameter("Speed", AnimatorParameterType::Float));
    const std::string text = readAll(assets() / "animator/live.controller");

    AnimatorScene s = animatorScene("animator/live.controller");
    window.openController("animator/live.controller", s.sceneProject->id, s.root);
    EXPECT_EQ(window.getEntity(), s.root);

    // edit-mode preview: the state's clip on the GameObject, restored on stop
    ASSERT_TRUE(window.startPreview()) << window.getStatus();
    EXPECT_TRUE(window.isPreviewing());
    ASSERT_TRUE(window.stepPreview(0.5f)) << window.getStatus();
    EXPECT_NEAR(s.legX(), 2.0f, 1e-4f);
    window.stopPreview();
    EXPECT_FALSE(window.isPreviewing());
    EXPECT_NEAR(s.legX(), 0.0f, 1e-5f);

    // play mode: the parameters are the runtime's; editing them does not touch the file
    std::string error;
    ASSERT_TRUE(s.scene()->getSystem<AnimatorSystem>()->preview(s.root, 0.0f, &error)) << error << " (the runtime, bound)";
    s.sceneProject->playState = ScenePlayState::PLAYING;
    EXPECT_TRUE(window.isLive());
    ASSERT_TRUE(window.setLiveParameter("Speed", 2.5f));
    anim::AnimatorInstance* runtime = s.animator().runtime.get();
    ASSERT_TRUE(runtime);
    EXPECT_FLOAT_EQ(runtime->parameters[(size_t)runtime->findParameter("Speed")], 2.5f);
    EXPECT_FALSE(window.setLiveParameter("Nope", 1.0f));
    EXPECT_FALSE(window.startPreview()) << "no edit-mode preview while it plays";
    s.sceneProject->playState = ScenePlayState::STOPPED;
    EXPECT_EQ(readAll(assets() / "animator/live.controller"), text);
    s.scene()->getSystem<AnimatorSystem>()->restorePreview(s.root);
    EXPECT_FALSE(window.isLive());

    // following the selection: the selected GameObject's Animator
    AnimatorWindow follower(project(), nullptr);
    follower.setOpen(true);
    project()->setSelectedEntity(s.sceneProject->id, s.root);
    {
        Frames frames;
        frames.frame(follower);
    }
    ASSERT_TRUE(follower.getDocument());
    EXPECT_EQ(follower.getDocument(), doc);
    EXPECT_EQ(follower.getEntity(), s.root);
}

// a 1D threshold moved past its neighbour: the child keeps its clip and lands at its sorted
// place; typed paths are checked and stored asset-relative with their GUID
TEST(AnimatorWindow, ThresholdsReSortByChildAndTypedPathsAreChecked) {
    ASSERT_TRUE(headless().ready);
    const std::string a = writeClip("a", 1.0f);
    const std::string b = writeClip("b", 2.0f);
    const std::string c = writeClip("c", 3.0f);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "thresholds");
    ASSERT_TRUE(doc);
    ASSERT_TRUE(window.addParameter("Speed", AnimatorParameterType::Float));
    ASSERT_TRUE(window.addState("Tree", ImVec2(300, 100), treeMotion("Speed")));
    anim::ControllerDef edited = doc->controller();
    anim::BlendTreeDef& tree = edited.layers[0].machine.states[0].motion.tree[0];
    tree.children.resize(3);
    tree.children[0].motion = clipMotion(window, a);
    tree.children[1].motion = clipMotion(window, b);
    tree.children[1].threshold = 1.0f;
    tree.children[2].motion = clipMotion(window, c);
    tree.children[2].threshold = 2.0f;
    std::string error;
    ASSERT_TRUE(doc->commit(edited, "Edit", "", &error)) << error;
    EXPECT_EQ(window.setBlendThreshold("Tree", 0, 1.5f), 1);
    const std::vector<anim::BlendChildDef>& children = machine(doc).states[0].motion.tree[0].children;
    ASSERT_EQ(children.size(), 3u);
    EXPECT_EQ(children[0].motion.clip.path, b);
    EXPECT_EQ(children[1].motion.clip.path, a) << "the moved child, past its neighbour";
    EXPECT_FLOAT_EQ(children[1].threshold, 1.5f);
    EXPECT_EQ(window.setBlendThreshold("Tree", 1, 9.0f), 2);
    EXPECT_EQ(machine(doc).states[0].motion.tree[0].children[2].motion.clip.path, a);
    EXPECT_EQ(window.setBlendThreshold("Tree", 7, 1.0f), -1);
    EXPECT_EQ(window.setBlendThreshold("Nope", 0, 1.0f), -1);

    anim::AnimatorAssetRef ref;
    ASSERT_TRUE(window.resolveTypedAsset("assets/" + a, true, ref)) << window.getStatus();
    EXPECT_EQ(ref.path, a) << "a project-relative path is stored asset-relative";
    EXPECT_TRUE(anim::isClipGuid(ref.guid));
    EXPECT_FALSE(window.resolveTypedAsset("animator/nope.anim", true, ref));
    EXPECT_NE(window.getStatus().find("does not exist"), std::string::npos) << window.getStatus();
    EXPECT_FALSE(window.resolveTypedAsset(a, false, ref)) << "a clip is not a mask";
    EXPECT_NE(window.getStatus().find("Avatar Mask"), std::string::npos) << window.getStatus();
    EXPECT_FALSE(window.resolveTypedAsset("../outside.anim", true, ref));
    EXPECT_NE(window.getStatus().find("outside the assets folder"), std::string::npos) << window.getStatus();
    anim::AvatarMaskDef maskData;
    writeAll(assets() / "animator/arm.mask", anim::writeMask(maskData));
    ASSERT_TRUE(window.resolveTypedAsset("animator/arm.mask", false, ref)) << window.getStatus();
    EXPECT_EQ(ref.path, "animator/arm.mask");
    EXPECT_TRUE(window.resolveTypedAsset("", true, ref));
    EXPECT_TRUE(ref.path.empty());
}

// Delete acts only when the Animator window has the focus (not when another window does)
TEST(AnimatorWindow, ShortcutsNeedTheWindowFocused) {
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "keys");
    ASSERT_TRUE(doc);
    ASSERT_TRUE(window.addState("A", ImVec2(200, 100), anim::MotionDef()));
    ASSERT_TRUE(window.addState("B", ImVec2(400, 100), anim::MotionDef()));
    Frames frames;
    bool focusOther = false;
    auto other = [&](){
        if (focusOther) ImGui::SetNextWindowFocus();
        focusOther = false;
        ImGui::Begin("Other");
        ImGui::End();
    };
    window.setOpen(true);
    frames.frame(window, other);
    focusOther = true;
    frames.frame(window, other);
    frames.frame(window, other);
    EXPECT_FALSE(window.isFocused());
    window.selectState("A");
    EXPECT_FALSE(window.canDelete()) << "another window has the focus";
    window.requestFocus();
    frames.frame(window, other);
    frames.frame(window, other);
    EXPECT_TRUE(window.isFocused());
    window.selectState("A");
    ASSERT_TRUE(window.canDelete());
    ASSERT_TRUE(window.removeSelection());
    EXPECT_EQ(machine(doc).states.size(), 1u);
    EXPECT_FALSE(window.canDelete()) << "nothing selected";
}

// a layer list change (remove, move, their undo and redo) keeps the current layer by name and
// drops a selection of the old indices: Delete never reaches another layer
TEST(AnimatorWindow, LayerListChangesDropStaleSelections) {
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "layerlist");
    ASSERT_TRUE(doc);
    ASSERT_TRUE(window.addState("", ImVec2(200, 100), anim::MotionDef()));     // Base Layer: New State
    ASSERT_TRUE(window.addLayer("New Layer"));
    ASSERT_TRUE(window.addState("", ImVec2(200, 100), anim::MotionDef()));     // New Layer: New State
    ASSERT_TRUE(window.addLayer("New Layer"));
    EXPECT_EQ(doc->controller().layers[2].name, "New Layer 1");
    ASSERT_TRUE(window.renameLayer(2, "Upper"));
    EXPECT_EQ(window.getLayer(), 2) << "a rename keeps the layer";
    ASSERT_TRUE(window.addState("", ImVec2(200, 100), anim::MotionDef()));     // Upper: New State
    window.selectState("New State");

    ASSERT_TRUE(window.removeLayer(1));
    EXPECT_EQ(window.getLayer(), 1) << "Upper, now index 1, stays current";
    EXPECT_EQ(window.describeSelection(), "") << "the old indices' selection goes";
    window.selectState("New State");
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(window.getLayer(), 2) << "after the undo Upper is index 2 again";
    EXPECT_EQ(window.describeSelection(), "");
    EXPECT_FALSE(window.removeSelection()) << "nothing selected: nothing deleted";
    for (const anim::LayerDef& layer : doc->controller().layers) EXPECT_EQ(layer.machine.states.size(), 1u) << layer.name;
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(window.getLayer(), 1);

    // a move keeps the current layer by name
    ASSERT_TRUE(window.moveLayer(1, 0));
    EXPECT_EQ(doc->controller().layers[0].name, "Upper");
    EXPECT_EQ(window.getLayer(), 0);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(window.getLayer(), 1);

    // an outside edit (MCP, a file) that drops the selected state: the selection goes
    window.selectState("New State");
    anim::ControllerDef edited = doc->controller();
    edited.layers[1].machine.states.clear();
    std::string error;
    ASSERT_TRUE(doc->commit(edited, "Edit", "", &error)) << error;
    EXPECT_EQ(window.describeSelection(), "");

    // removing the current layer falls back to the base
    ASSERT_TRUE(window.removeLayer(1));
    EXPECT_EQ(window.getLayer(), 0);
    EXPECT_EQ(doc->controller().layers.size(), 1u);
}

// the pinned Animator's controller field changes (the Inspector, MCP, an undo): the window follows it
TEST(AnimatorWindow, FollowsThePinnedAnimatorsControllerField) {
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> first = freshController(window, "fieldA");
    std::shared_ptr<AnimatorAssetDocument> second = freshController(window, "fieldB");
    ASSERT_TRUE(first && second);
    AnimatorScene s = animatorScene("animator/fieldA.controller");
    window.openController("animator/fieldA.controller", s.sceneProject->id, s.root);
    window.setOpen(true);
    Frames frames;
    frames.frame(window);
    EXPECT_EQ(window.getDocument(), first);
    s.animator().controller = AssetRef("animator/fieldB.controller");
    frames.frame(window);
    EXPECT_EQ(window.getDocument(), second);
    EXPECT_EQ(window.getEntity(), s.root);
    s.animator().controller = AssetRef();
    frames.frame(window);
    EXPECT_FALSE(window.getDocument()) << "an Animator without a controller: nothing to edit";
    s.animator().controller = AssetRef("animator/fieldA.controller");
    frames.frame(window);
    EXPECT_EQ(window.getDocument(), first);
}

// parameter names: S21 keeps any name (scripts pass it as a string), numbered when taken; a name
// that is not a Lua identifier is said (the rename's note and a validation warning), never dropped
TEST(AnimatorWindow, ParameterNamesAreKeptAndNonLuaNamesWarned) {
    ASSERT_TRUE(headless().ready);
    AnimatorWindow window(project(), nullptr);
    std::shared_ptr<AnimatorAssetDocument> doc = freshController(window, "names");
    ASSERT_TRUE(doc);
    ASSERT_TRUE(window.addParameter("NewFloat", AnimatorParameterType::Float));
    ASSERT_TRUE(window.addParameter("NewFloat", AnimatorParameterType::Float));
    EXPECT_EQ(doc->controller().parameters[0].name, "NewFloat");
    EXPECT_EQ(doc->controller().parameters[1].name, "NewFloat 1");
    ASSERT_TRUE(window.renameParameter("NewFloat", "Move Speed"));
    EXPECT_NE(window.getStatus().find("not a Lua identifier"), std::string::npos) << window.getStatus();
    ASSERT_EQ(doc->controller().parameters.size(), 2u) << "nothing dropped";
    EXPECT_EQ(doc->controller().parameters[0].name, "Move Speed");
    bool warned = false;
    for (const ClipWarning& warning : window.problems()){
        if (warning.path == "parameters[0].name" && warning.message.find("Lua") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned);
    ASSERT_TRUE(window.renameParameter("Move Speed", "move_speed"));
    EXPECT_TRUE(window.getStatus().empty()) << window.getStatus();
}
