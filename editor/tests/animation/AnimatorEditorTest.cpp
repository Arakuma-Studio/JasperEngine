// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's editor side (docs/specs/S21-animation.md R22-R27, R32, AC13): every Animator
// window operation is one named project undo step that undoes to the exact file and redoes to
// the same one; the asset documents (create, commit, merge keys, outside changes, refused kinds);
// renaming a clip rewrites the controllers and montages naming it; "from Animation Player"; the
// avatar mask and montage documents. A headless project, as AnimationClipAssetsTest runs.

#include <gtest/gtest.h>
#include "pose/PoseUiLogic.h"

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetIO.h"
#include "command/CommandHistory.h"
#include "command/type/RenameFileCmd.h"
#include "component/AnimationPlayerComponent.h"
#include "window/AnimatorWindow.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
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
            const fs::path root = fs::temp_directory_path() / ("jasper-animator-editor-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: animatoreditor\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
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

    std::string folder(const std::string& name) {
        fs::remove_all(root() / name);
        fs::create_directories(root() / name);
        return name;
    }

    std::string topName() {
        const UndoEntry* top = history()->top();
        return top ? top->name() : std::string();
    }

    std::shared_ptr<AnimatorAssetDocument> createController(const std::string& relative) {
        std::string error;
        std::shared_ptr<AnimatorAssetDocument> document =
            AnimatorAssetDocument::create(project(), relative, AnimatorAssetDocument::Kind::Controller, "Create Animator Controller", &error);
        EXPECT_TRUE(document) << error;
        return document;
    }

    class AnimatorEditor : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready) << "the headless project did not open";
        }
    };

    // runs `edit` and checks it made exactly one project step named `name` that undoes to the
    // file before it and redoes to the file after it
    template <class Edit>
    void oneStep(const fs::path& file, const std::string& name, Edit edit) {
        const std::string before = readAll(file);
        const size_t position = history()->position();
        ASSERT_TRUE(edit()) << name;
        const std::string after = readAll(file);
        EXPECT_NE(after, before) << name << ": the file changed";
        EXPECT_EQ(history()->position(), position + 1) << name << ": one undo step";
        EXPECT_EQ(topName(), name);
        ASSERT_TRUE(history()->undo());
        EXPECT_EQ(readAll(file), before) << name << ": undo gives the exact file back";
        ASSERT_TRUE(history()->redo());
        EXPECT_EQ(readAll(file), after) << name << ": redo writes the same file";
    }

}

// ---- the documents ---------------------------------------------------------------------------

TEST_F(AnimatorEditor, CreateWritesCanonicalFilesWithGuids) {
    const std::string dir = folder("create");
    const size_t position = history()->position();
    std::shared_ptr<AnimatorAssetDocument> controller = createController(dir + "/hero.controller");
    ASSERT_TRUE(controller);
    EXPECT_EQ(history()->position(), position + 1) << "one project step";
    const std::string text = readAll(root() / dir / "hero.controller");
    EXPECT_EQ(anim::animatorAssetKind(text), "controller");
    EXPECT_TRUE(anim::isClipGuid(anim::readAnimatorAssetGuid(text)));
    ASSERT_EQ(controller->controller().layers.size(), 1u);
    EXPECT_EQ(controller->controller().layers[0].name, "Base Layer");
    EXPECT_EQ(text, anim::writeController(controller->controller())) << "canonical";

    std::string error;
    std::shared_ptr<AnimatorAssetDocument> mask =
        AnimatorAssetDocument::create(project(), dir + "/upper.mask", AnimatorAssetDocument::Kind::Mask, "", &error);
    ASSERT_TRUE(mask) << error;
    EXPECT_EQ(topName().rfind("Create Avatar Mask", 0), 0u) << topName();
    std::shared_ptr<AnimatorAssetDocument> montage =
        AnimatorAssetDocument::create(project(), dir + "/wave.montage", AnimatorAssetDocument::Kind::Montage, "", &error);
    ASSERT_TRUE(montage) << error;
    ASSERT_EQ(montage->montage().sections.size(), 1u) << "a Default section";

    // refused: the wrong extension, an existing file, outside the project
    EXPECT_FALSE(AnimatorAssetDocument::create(project(), dir + "/x.mask", AnimatorAssetDocument::Kind::Controller, "", &error));
    EXPECT_FALSE(AnimatorAssetDocument::create(project(), dir + "/hero.controller", AnimatorAssetDocument::Kind::Controller, "", &error));
    EXPECT_NE(error.find("exists"), std::string::npos) << error;
    EXPECT_FALSE(AnimatorAssetDocument::create(project(), "/tmp/outside.controller", AnimatorAssetDocument::Kind::Controller, "", &error));

    // undo of a creation removes the file; the same document comes back on redo
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(fs::exists(root() / dir / "wave.montage"));
    ASSERT_TRUE(history()->redo());
    EXPECT_TRUE(fs::exists(root() / dir / "wave.montage"));
}

TEST_F(AnimatorEditor, CommitsMergeByKeyAndReloadOutsideChanges) {
    const std::string dir = folder("merge");
    std::shared_ptr<AnimatorAssetDocument> document = createController(dir + "/m.controller");
    ASSERT_TRUE(document);
    const fs::path file = root() / dir / "m.controller";
    const std::string created = readAll(file);
    std::string error;
    const size_t position = history()->position();
    for (int i = 1; i <= 3; i++){
        anim::ControllerDef def = document->controller();
        def.layers[0].weight = 1.0f;
        anim::StateDef state;
        state.name = "S";
        state.position = Vector2((float)i * 10.0f, 0.0f);
        def.layers[0].machine.states = {state};
        ASSERT_TRUE(document->commit(def, "Move States", "move", &error)) << error;
    }
    EXPECT_EQ(history()->position(), position + 1) << "a drag's commits merge into one step";
    EXPECT_FLOAT_EQ(document->controller().layers[0].machine.states[0].position.x, 30.0f);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(readAll(file), created);

    // a change on disk: the document reloads; a commit made from the stale data is refused
    anim::ControllerDef stale = document->controller();
    anim::ControllerDef outside = stale;
    anim::ParameterDef parameter;
    parameter.name = "Outside";
    outside.parameters.push_back(parameter);
    writeAll(file, anim::writeController(outside));
    anim::StateDef staleState;
    staleState.name = "Stale";
    stale.layers[0].machine.states.push_back(staleState);
    EXPECT_FALSE(document->commit(stale, "Edit", "", &error));
    EXPECT_NE(error.find("changed on disk"), std::string::npos) << error;
    ASSERT_EQ(document->controller().parameters.size(), 1u) << "reloaded";
    // a mask is no controller
    std::shared_ptr<AnimatorAssetDocument> mask =
        AnimatorAssetDocument::create(project(), dir + "/m.mask", AnimatorAssetDocument::Kind::Mask, "", &error);
    ASSERT_TRUE(mask);
    EXPECT_FALSE(mask->commit(document->controller(), "Edit", "", &error));
}

// ---- the Animator window's operations (AC13) ------------------------------------------------

TEST_F(AnimatorEditor, EveryWindowOperationIsOneNamedUndoStep) {
    const std::string dir = folder("window");
    ASSERT_TRUE(createController(dir + "/w.controller"));
    const fs::path file = root() / dir / "w.controller";
    AnimatorWindow window(project(), nullptr);
    window.openController(dir + "/w.controller", 0, NULL_ENTITY);
    ASSERT_TRUE(window.getDocument());

    anim::MotionDef walk;
    walk.clip.path = "models/hero.gltf#Walk";
    oneStep(file, "Create State Idle", [&](){ return window.addState("Idle", ImVec2(200, 100), anim::MotionDef()); });
    EXPECT_EQ(window.getDocument()->controller().layers[0].machine.defaultState, "Idle") << "the first state is the default";
    oneStep(file, "Create State Walk", [&](){ return window.addState("Walk", ImVec2(400, 100), walk); });
    // a taken name gets a number
    oneStep(file, "Create State Walk 1", [&](){ return window.addState("Walk", ImVec2(400, 200), walk); });
    oneStep(file, "Add Parameter Speed", [&](){ return window.addParameter("Speed", AnimatorParameterType::Float); });
    oneStep(file, "Make Transition Idle -> Walk", [&](){ return window.addTransition("Idle", "Walk", false, false); });
    const anim::TransitionDef& made = window.getDocument()->controller().layers[0].machine.states[0].transitions.at(0);
    EXPECT_TRUE(made.hasExitTime);
    EXPECT_FLOAT_EQ(made.exitTime, 0.75f);
    EXPECT_FLOAT_EQ(made.duration, 0.25f);
    oneStep(file, "Make Transition Any State -> Walk 1", [&](){ return window.addTransition("", "Walk 1", true, false); });
    oneStep(file, "Set Default State Walk", [&](){ return window.setDefaultState("Walk"); });
    oneStep(file, "Create Sub-State Machine Combat", [&](){ return window.addSubStateMachine("Combat", ImVec2(300, 300)); });
    oneStep(file, "Add Layer Upper", [&](){ return window.addLayer("Upper"); });
    EXPECT_EQ(window.getLayer(), 1) << "the new layer shows";
    // back on the base layer: delete a state (its transitions in and the default follow)
    window.openController(dir + "/w.controller", 0, NULL_ENTITY);
    window.selectState("Walk 1");
    oneStep(file, "Delete State Walk 1", [&](){ return window.removeSelection(); });

    // the saved controller compiles clean but for the unreachable / clip-less notes
    anim::ControllerParseResult parsed = anim::parseController(readAll(file));
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    EXPECT_EQ(parsed.data.layers.size(), 2u);
    EXPECT_EQ(parsed.data.layers[0].machine.defaultState, "Walk");
    EXPECT_EQ(parsed.data.layers[0].machine.subMachines.size(), 1u);
    EXPECT_TRUE(parsed.data.layers[0].machine.anyState.empty()) << "the transitions into a deleted state go with it";
}

// ---- assets following renames ---------------------------------------------------------------

TEST_F(AnimatorEditor, RenamingAClipRewritesTheControllersAndMontages) {
    const std::string dir = folder("rename");
    ClipData clip;
    ClipTrackData track;
    track.channel = ClipChannel::Position;
    ClipKey key;
    key.value = {0, 0, 0};
    track.keys = {key};
    clip.tracks.push_back(track);
    std::string error;
    ASSERT_TRUE(AnimationClipDocument::create(project(), dir + "/run.anim", clip, "Create", &error)) << error;
    std::shared_ptr<AnimatorAssetDocument> controller = createController(dir + "/c.controller");
    ASSERT_TRUE(controller);
    anim::ControllerDef def = controller->controller();
    anim::StateDef state;
    state.name = "Run";
    state.motion.clip.path = dir + "/run.anim";
    def.layers[0].machine.states.push_back(state);
    ASSERT_TRUE(controller->commit(def, "Add", "", &error)) << error;
    std::shared_ptr<AnimatorAssetDocument> montage =
        AnimatorAssetDocument::create(project(), dir + "/m.montage", AnimatorAssetDocument::Kind::Montage, "", &error);
    ASSERT_TRUE(montage);
    anim::MontageDef m = montage->montage();
    m.clip.path = dir + "/run.anim";
    ASSERT_TRUE(montage->commit(m, "Clip", "", &error)) << error;

    ASSERT_TRUE(history()->addCommand(new RenameFileCmd(project(), "run.anim", "sprint.anim", (root() / dir).string())));
    EXPECT_NE(readAll(root() / dir / "c.controller").find(dir + "/sprint.anim"), std::string::npos) << "the controller follows";
    EXPECT_NE(readAll(root() / dir / "m.montage").find(dir + "/sprint.anim"), std::string::npos) << "the montage follows";
    ASSERT_TRUE(history()->undo());
    EXPECT_NE(readAll(root() / dir / "c.controller").find(dir + "/run.anim"), std::string::npos) << "undo puts them back";
}

// ---- Create Animator Controller from Animation Player ---------------------------------------

TEST_F(AnimatorEditor, StatesFromAnAnimationPlayer) {
    AnimationPlayerComponent player;
    AnimationPlayerClip walk;
    walk.clip = "models/hero.gltf#Walk";
    walk.wrap = ClipWrapMode::Loop;
    AnimationPlayerClip jump;
    jump.clip = "anim/jump.anim";
    jump.guid = "0123456789abcdef0123456789abcdef";
    jump.wrap = ClipWrapMode::Once;
    player.clips = {walk, jump};
    player.defaultClip = "jump";
    anim::ControllerDef def;
    EXPECT_EQ(appendPlayerStates(def, player), 2u);
    ASSERT_EQ(def.layers.size(), 1u) << "a Base Layer is added";
    const anim::StateMachineDef& machine = def.layers[0].machine;
    ASSERT_EQ(machine.states.size(), 2u);
    EXPECT_EQ(machine.states[0].name, "Walk");
    // the entry's wrap replaced its clip's (a ping-pong clip loops forward): Repeat
    EXPECT_EQ(machine.states[0].loop, AnimatorStateLoop::Repeat);
    EXPECT_EQ(machine.states[1].name, "jump");
    EXPECT_EQ(machine.states[1].motion.clip.guid, jump.guid);
    EXPECT_EQ(machine.states[1].loop, AnimatorStateLoop::Once);
    EXPECT_EQ(machine.defaultState, "jump");
    EXPECT_NE(machine.states[0].position.x, machine.states[1].position.x) << "laid out apart";
}

// upstream's "Create Animator Controller from Clips" copied each entry's humanoid playback onto
// its state: S21's states carry Retarget, Mirror and Foot IK now (they were dropped)
TEST_F(AnimatorEditor, StatesFromAnAnimationPlayerKeepHumanoidPlayback) {
    AnimationPlayerComponent player;
    AnimationPlayerClip walk;
    walk.clip = "models/hero.gltf#Walk";
    walk.retarget = true;
    walk.mirror = true;
    AnimationPlayerClip idle;
    idle.clip = "anim/idle_h.anim";
    idle.footIK = true;
    player.clips = {walk, idle};
    anim::ControllerDef def;
    ASSERT_EQ(appendPlayerStates(def, player), 2u);
    const anim::StateMachineDef& machine = def.layers[0].machine;
    EXPECT_TRUE(machine.states[0].retarget);
    EXPECT_TRUE(machine.states[0].mirror);
    EXPECT_FALSE(machine.states[0].footIK);
    EXPECT_FALSE(machine.states[1].retarget);
    EXPECT_FALSE(machine.states[1].mirror);
    EXPECT_TRUE(machine.states[1].footIK);
    // written and read back with the flags
    anim::ControllerParseResult parsed = anim::parseController(anim::writeController(def));
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.data.layers[0].machine.states[0].retarget && parsed.data.layers[0].machine.states[0].mirror);
    EXPECT_TRUE(parsed.data.layers[0].machine.states[1].footIK);
}
