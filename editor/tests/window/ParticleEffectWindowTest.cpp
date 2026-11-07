// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Particle Effect window (docs/specs/S91-effect-authoring.md R17-R21, R25, AC14), driven
// through the functions its buttons, rows and fields call: a file and its sub-emitters as the
// Outline with their timing rows; the eye, which changes the stage's copies and never the file or
// the undo history; an edit while paused that re-simulates to the same time with the same seed
// (the stage's state then equal to a fresh one seeked there); an edit as one undo step that undo
// takes back; Reroll and the per-loop seed; a GameObject subtree's systems; the exposed
// parameters and triggers; and every panel drawn in a frame.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "window/ParticleEffectWindow.h"

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
using namespace jasperengine::particles;
namespace fs = std::filesystem;
using Background = ParticleEffectWindow::Background;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-particle-effect-window-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: particleeffectproject\nassetsDir: assets\n";
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

    bool started = false;
    Project* project() {
        started = true;
        return headless().project.get();
    }
    CommandHistory* history() { return project()->getProjectCommandHistory(); }

    struct Cleanup : public ::testing::Environment {
        void TearDown() override {
            if (!started) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const cleanup = ::testing::AddGlobalTestEnvironment(new Cleanup);

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // an effect file (assets-relative), read again by the library
    std::string writeEffect(const std::string& path, const ParticleEffectDocument& document) {
        const fs::path file = project()->getAssetsPath() / path;
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << ParticleEffectCodec::write(document);
        ParticleEffectLibrary::reload(path);
        return path;
    }

    // a small looping fountain; with a child, a Death sub-emitter on `child`
    ParticleEffectDocument fountain(const std::string& child = std::string()) {
        ParticleEffectDocument d;
        d.duration = 2.0f;
        d.maxParticles = 200;
        d.main.startLifetime = MinMaxCurve::twoConstants(0.4f, 0.8f);
        d.main.startSpeed = MinMaxCurve(3.0f);
        d.emission.rateOverTime = MinMaxCurve(40.0f);
        if (!child.empty()) {
            d.subEmittersModule.present = true;
            SubEmitterEntry entry;
            entry.trigger = SubEmitterTrigger::Death;
            entry.effect = child;
            entry.emitCount = 2;
            d.subEmitters.push_back(entry);
        }
        return d;
    }

    ParticleEffectDocument flash() {
        ParticleEffectDocument d;
        d.looping = false;
        d.duration = 0.5f;
        d.maxParticles = 400;
        d.main.startLifetime = MinMaxCurve(0.3f);
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        return d;
    }

    // frames of the window's clock until its stage is at its time
    void settle(ParticleEffectWindow& window) {
        for (int i = 0; i < 2000 && (i == 0 || window.isResimulating()); i++) window.update(1.0 / 60.0);
        ASSERT_FALSE(window.isResimulating());
    }

    void play(ParticleEffectWindow& window, int frames) {
        window.play();
        for (int i = 0; i < frames; i++) window.update(1.0 / 30.0);
    }

    // an ImGui frame loop of its own (the window's show())
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
        void frame(ParticleEffectWindow& window) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            window.show();
            ImGui::EndFrame();
        }
    };

}

TEST(ParticleEffectWindow, AFileAndItsSubEmittersAreTheOutline) {
    ASSERT_TRUE(headless().ready);
    const std::string child = writeEffect("fx/outline_flash.particles", flash());
    const std::string parent = writeEffect("fx/outline_rocket.particles", fountain(child));
    ParticleEffectWindow window(project());
    window.openEffect(parent);
    EXPECT_TRUE(window.isOpen());
    window.update(1.0 / 60.0);
    ASSERT_EQ(window.getNodes().size(), 2u);
    EXPECT_EQ(window.getNodes()[0].name, "outline_rocket");
    EXPECT_EQ(window.getNodes()[0].effect, parent);
    EXPECT_EQ(window.getNodes()[1].name, "outline_flash");
    EXPECT_EQ(window.getNodes()[1].parent, 0);
    EXPECT_EQ(window.getNodes()[1].depth, 1);
    // the timing rows are the nodes', analytic
    ASSERT_EQ(window.timing().size(), 2u);
    EXPECT_EQ(window.timing()[1].trigger, "death");
    EXPECT_FLOAT_EQ(window.timing()[1].start, 0.4f);
    // one system on the stage: its child plays inside it
    EXPECT_EQ(window.getStage().systemCount(), 1u);
}

TEST(ParticleEffectWindow, TheEyeChangesTheStageAndNeverTheFile) {
    ASSERT_TRUE(headless().ready);
    const std::string child = writeEffect("fx/eye_flash.particles", flash());
    const std::string parent = writeEffect("fx/eye_rocket.particles", fountain(child));
    const std::string parentBytes = readAll(project()->getAssetsPath() / parent);
    const std::string childBytes = readAll(project()->getAssetsPath() / child);
    ParticleEffectWindow window(project());
    window.openEffect(parent);
    settle(window);
    const size_t steps = history()->position();

    window.setHidden(0, true);
    window.setHidden(1, true);
    settle(window);
    EXPECT_TRUE(window.isHidden(0));
    // the stage's copies draw nothing; the files, the library's documents and the history are as they were
    const std::string key = std::string(ParticleEffectWindow::STAGE_KEY_PREFIX) + "0.particles";
    ASSERT_TRUE(ParticleEffectLibrary::get(key).document);
    EXPECT_EQ(ParticleEffectLibrary::get(key).document->renderer.renderMode, RenderMode::None);
    EXPECT_EQ(ParticleEffectLibrary::get(parent).document->renderer.renderMode, RenderMode::Billboard);
    EXPECT_EQ(readAll(project()->getAssetsPath() / parent), parentBytes);
    EXPECT_EQ(readAll(project()->getAssetsPath() / child), childBytes);
    EXPECT_EQ(history()->position(), steps);
    // the hidden copy still points at its child's copy: the children play on
    EXPECT_EQ(ParticleEffectLibrary::get(key).document->subEmitters[0].effect, std::string(ParticleEffectWindow::STAGE_KEY_PREFIX) + "1.particles");

    window.setHidden(0, false);
    settle(window);
    EXPECT_EQ(ParticleEffectLibrary::get(key).document->renderer.renderMode, RenderMode::Billboard);
    EXPECT_EQ(readAll(project()->getAssetsPath() / parent), parentBytes);
}

TEST(ParticleEffectWindow, APausedEditHoldsTheTimeAndTheSeed) {
    ASSERT_TRUE(headless().ready);
    const std::string path = writeEffect("fx/locked.particles", fountain());
    ParticleEffectWindow window(project());
    window.openEffect(path);
    window.setSeed(77);
    settle(window);
    play(window, 25);
    window.pause();
    settle(window);
    const double time = window.getTime();
    ASSERT_GT(time, 0.5);
    ASSERT_GT(window.stageStats().live, 0u);

    ASSERT_TRUE(window.editSelected([](ParticleEffectDocument& d) {
        d.main.startSpeed = MinMaxCurve(9.0f);
        return true;
    }));
    // the edit reached the file and the library; the stage re-simulates over frames
    ParticleEffectDocument edited;
    ASSERT_TRUE(window.readSelected(edited));
    EXPECT_EQ(edited.main.startSpeed, MinMaxCurve(9.0f));
    window.update(1.0 / 60.0);
    EXPECT_TRUE(window.isResimulating());
    settle(window);
    EXPECT_FALSE(window.isPlaying());
    EXPECT_DOUBLE_EQ(window.getTime(), time);
    EXPECT_EQ(window.getSeed(), 77u);
    const uint64_t afterEdit = window.stageStats().stateHash;

    // the same frame as a stage made from the edited file and taken to that time with that seed
    ParticleEffectWindow fresh(project());
    fresh.openEffect(path);
    fresh.setSeed(77);
    fresh.seek(time);
    settle(fresh);
    EXPECT_EQ(afterEdit, fresh.stageStats().stateHash);
    EXPECT_GT(fresh.stageStats().live, 0u);
}

TEST(ParticleEffectWindow, AnEditIsOneUndoStepAndUndoPutsTheFileBack) {
    ASSERT_TRUE(headless().ready);
    const std::string path = writeEffect("fx/undo.particles", fountain());
    const std::string before = readAll(project()->getAssetsPath() / path);
    ParticleEffectWindow window(project());
    window.openEffect(path);
    settle(window);
    const size_t steps = history()->position();
    // a drag: three values, one step
    for (float speed : {6.0f, 7.0f, 8.0f}) {
        window.editSelected([speed](ParticleEffectDocument& d) {
            d.main.startSpeed = MinMaxCurve(speed);
            return true;
        }, speed == 8.0f);
    }
    EXPECT_EQ(history()->position(), steps + 1);
    // a no-op makes none
    EXPECT_FALSE(window.editSelected([](ParticleEffectDocument&) { return true; }));
    EXPECT_EQ(history()->position(), steps + 1);
    EXPECT_NE(readAll(project()->getAssetsPath() / path), before);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(readAll(project()->getAssetsPath() / path), before);
}

TEST(ParticleEffectWindow, RerollAndThePerLoopSeed) {
    ASSERT_TRUE(headless().ready);
    const std::string path = writeEffect("fx/seeds.particles", fountain());
    ParticleEffectWindow window(project());
    window.openEffect(path);
    settle(window);
    play(window, 12);
    window.pause();
    settle(window);
    const double time = window.getTime();
    const uint32_t seed = window.getSeed();
    const uint64_t hash = window.stageStats().stateHash;
    window.reroll();
    settle(window);
    EXPECT_NE(window.getSeed(), seed);
    EXPECT_DOUBLE_EQ(window.getTime(), time);
    EXPECT_NE(window.stageStats().stateHash, hash);

    // the clock loops over the range; each loop a new seed only when asked
    window.setLoopRange(true, 0.0f, 0.3f);
    const uint32_t kept = window.getSeed();
    play(window, 20);
    EXPECT_LE(window.getTime(), 0.3 + 1e-6);
    EXPECT_EQ(window.getSeed(), kept);
    window.setPerLoopSeed(true);
    play(window, 20);
    EXPECT_NE(window.getSeed(), kept);
}

TEST(ParticleEffectWindow, ASubtreeIsItsSystemsUnderTheRoot) {
    ASSERT_TRUE(headless().ready);
    const std::string path = writeEffect("fx/subtree.particles", fountain());
    Project* p = project();
    p->createNewScene("Particles", SceneType::SCENE_3D);
    SceneProject* sceneProject = p->getScene(p->getSelectedSceneId());
    Scene* scene = sceneProject->scene;
    auto make = [&](const char* name, Entity parent) {
        const Entity e = scene->createUserEntity();
        scene->setEntityName(e, name);
        Transform transform;
        transform.position = Vector3(1.0f, 0.0f, 0.0f);
        scene->addComponent<Transform>(e, transform);
        if (parent != NULL_ENTITY) scene->addEntityChild(parent, e, false);
        sceneProject->entities.push_back(e);
        return e;
    };
    const Entity root = make("Explosion", NULL_ENTITY);
    const Entity sparks = make("Sparks", root);
    const Entity smoke = make("Smoke", root);
    const Entity outside = make("Elsewhere", NULL_ENTITY);
    ParticleSystemComponent asset;
    asset.effect = AssetRef(path);
    scene->addComponent<ParticleSystemComponent>(sparks, asset);
    ParticleSystemComponent inlineSystem;
    inlineSystem.inlineEffect = fountain();
    inlineSystem.seed = 5;
    scene->addComponent<ParticleSystemComponent>(smoke, inlineSystem);
    scene->addComponent<ParticleSystemComponent>(outside, ParticleSystemComponent());

    ParticleEffectWindow window(p);
    window.openSubtree(sceneProject->id, root);
    settle(window);
    ASSERT_EQ(window.getNodes().size(), 2u);
    EXPECT_EQ(window.getNodes()[0].name, "Sparks");
    EXPECT_EQ(window.getNodes()[0].effect, path);
    EXPECT_EQ(window.getNodes()[1].name, "Smoke");
    EXPECT_TRUE(window.getNodes()[1].effect.empty());
    EXPECT_EQ(window.getStage().systemCount(), 2u);
    EXPECT_EQ(window.uiState().subtreeRoot, "Explosion");

    // an inline system's edit is a step of the scene's history, on its GameObject
    window.select(1);
    CommandHistory* sceneHistory = CommandHandle::get(sceneProject->id);
    const size_t sceneSteps = sceneHistory->position();
    ASSERT_TRUE(window.editSelected([](ParticleEffectDocument& d) {
        d.duration = 3.0f;
        return true;
    }));
    EXPECT_EQ(sceneHistory->position(), sceneSteps + 1);
    EXPECT_FLOAT_EQ(scene->getComponent<ParticleSystemComponent>(smoke).inlineEffect.duration, 3.0f);
    window.clearSceneState(sceneProject->id);
    EXPECT_TRUE(window.getNodes().empty());
}

TEST(ParticleEffectWindow, ExposedParametersAndTriggersDriveTheStageOnly) {
    ASSERT_TRUE(headless().ready);
    const std::string child = writeEffect("fx/trigger_flash.particles", flash());
    ParticleEffectDocument d = fountain();
    ExposedParameter rate;
    rate.name = "Rate";
    rate.path = "emission.rateOverTime";
    d.exposed.push_back(rate);
    d.subEmittersModule.present = true;
    SubEmitterEntry entry;
    entry.trigger = SubEmitterTrigger::Trigger;
    entry.triggerName = "Boom";
    entry.effect = child;
    d.subEmitters.push_back(entry);
    const std::string path = writeEffect("fx/exposed.particles", d);
    const std::string bytes = readAll(project()->getAssetsPath() / path);
    ParticleEffectWindow window(project());
    window.openEffect(path);
    settle(window);
    const std::vector<ParticleEffectWindow::ExposedRow> rows = window.exposedRows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].name, "Rate");
    EXPECT_FLOAT_EQ(rows[0].value.x, 40.0f);
    EXPECT_EQ(window.triggerNames(), std::vector<std::string>{"Boom"});

    window.setExposedValue("Rate", Vector4(5.0f, 0.0f, 0.0f, 0.0f));
    window.fireTrigger("Boom");
    window.update(1.0 / 60.0);
    EXPECT_FLOAT_EQ(window.exposedRows()[0].value.x, 5.0f);
    const ParticleSystemComponent& system = window.getStage().getScene()->getComponent<ParticleSystemComponent>(window.getStage().systemEntity(0));
    ASSERT_EQ(system.exposedValues.size(), 1u);
    EXPECT_FLOAT_EQ(system.exposedValues[0].value.x, 5.0f);
    EXPECT_EQ(readAll(project()->getAssetsPath() / path), bytes);
}

TEST(ParticleEffectWindow, EveryPanelDrawsInAFrame) {
    ASSERT_TRUE(headless().ready);
    const std::string child = writeEffect("fx/frame_flash.particles", flash());
    const std::string path = writeEffect("fx/frame_rocket.particles", fountain(child));
    ParticleEffectWindow window(project());
    bool checksDrawn = false;
    ParticleEffectWindow::setChecksPanel([&checksDrawn](ParticleEffectWindow&) { checksDrawn = true; });
    Frames frames;
    frames.frame(window);           // closed: nothing
    window.openEffect(path);
    for (Background background : {Background::Dark, Background::Light, Background::Checker, Background::SceneSky}) {
        window.setBackground(background);
        frames.frame(window);
    }
    window.setOverdraw(true);
    window.setMovingEmitter(true);
    window.setHidden(1, true);
    frames.frame(window);
    frames.frame(window);
    EXPECT_TRUE(checksDrawn);
    EXPECT_TRUE(window.isOverdraw());
    ParticleEffectWindow::setChecksPanel(nullptr);
    window.close();
    frames.frame(window);
}
