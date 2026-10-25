// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's curves view helpers and Record's sources (animation clips spec 4.4,
// 9.R5/R50): clipcurves (components, the runtime sampler's values, Euler continuity across keys,
// nice ticks); the record source helpers of ClipRecordHooks; LatticeEditorOps' drag recorded as
// one key edit per drag (MCP's discrete edits stay scene steps); the Pose window's listener on the
// chained beforeChannelWrite (a scene command on a posed bone edits its rest). Headless project as
// in AnimationClipControllerTest.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ClipRecordHooks.h"
#include "animation/KeyframeSampling.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/LatticeComponent.h"
#include "component/ModelComponent.h"
#include "lattice/LatticeEditorOps.h"
#include "object/Model.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseEditSession.h"
#include "subsystem/MeshSystem.h"
#include "window/AnimationWindowClipCurves.h"
#include "window/AnimationWindowClipUtil.h"

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
namespace fs = std::filesystem;

namespace {

    ClipKey key(float time, std::vector<float> value) {
        ClipKey out;
        out.time = time;
        out.value = std::move(value);
        return out;
    }

    std::vector<float> rotationX(float degrees) {
        return clipui::xyzwOf(Vector3(degrees, 0.0f, 0.0f));
    }

    // the same rotation (q or -q)
    bool sameRotation(const std::vector<float>& a, const std::vector<float>& b) {
        const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        return std::fabs(std::fabs(dot) - 1.0f) < 1e-4f;
    }

}

// ---- clipcurves (pure) ----------------------------------------------------------------------

TEST(ClipCurves, ComponentsAndLabels) {
    ClipTrackData position;
    position.channel = ClipChannel::Position;
    EXPECT_EQ(clipcurves::componentCount(position), 3u);
    EXPECT_EQ(clipcurves::componentLabel(position, 1, {}), "y");
    ClipTrackData morph;
    morph.channel = ClipChannel::Morph;
    morph.keys = {key(0.0f, {0.0f, 1.0f}), key(1.0f, {0.0f, 0.5f, 0.25f})};
    EXPECT_EQ(clipcurves::componentCount(morph), 3u) << "the widest key";
    EXPECT_EQ(clipcurves::componentLabel(morph, 0, {"Smile"}), "Smile");
    EXPECT_EQ(clipcurves::componentLabel(morph, 2, {"Smile"}), "Weight 2");
    ClipTrackData lattice;
    lattice.channel = ClipChannel::Lattice;
    lattice.points = {4, 13};
    EXPECT_EQ(clipcurves::componentCount(lattice), 6u);
    EXPECT_EQ(clipcurves::componentLabel(lattice, 4, {}), "Point 13 y");
    ClipTrackData property;
    property.channel = ClipChannel::Property;
    EXPECT_EQ(clipcurves::componentCount(property), 0u);
}

// The other Euler solution is the same rotation in the clip's convention, and nearestEuler picks
// the representation nearest to the reference (whole turns, or the mirrored middle angle)
TEST(ClipCurves, NearestEulerIsTheSameRotation) {
    const Vector3 angles[] = {Vector3(10, 20, 30), Vector3(-170, 80, 5), Vector3(90, -45, 179), Vector3(0, 89, 0)};
    for (const Vector3& e : angles) {
        const Vector3 other(e.x + 180.0f, 180.0f - e.y, e.z + 180.0f);
        EXPECT_TRUE(sameRotation(clipui::xyzwOf(e), clipui::xyzwOf(other))) << e.x << " " << e.y << " " << e.z;
        for (const Vector3& reference : {Vector3(400, 0, 0), Vector3(e.x + 170, 170 - e.y, e.z + 190), Vector3(-720, 360, 0)}) {
            const Vector3 near = clipcurves::nearestEuler(e, reference);
            EXPECT_TRUE(sameRotation(clipui::xyzwOf(e), clipui::xyzwOf(near)));
            // never farther than the plain whole-turn choice
            auto turn = [](float v, float r) { return v + 360.0f * std::round((r - v) / 360.0f); };
            const Vector3 plain(turn(e.x, reference.x), turn(e.y, reference.y), turn(e.z, reference.z));
            auto distance = [&reference](const Vector3& v) {
                return std::fabs(v.x - reference.x) + std::fabs(v.y - reference.y) + std::fabs(v.z - reference.z);
            };
            EXPECT_LE(distance(near), distance(plain) + 1e-3f);
        }
    }
    const Vector3 wrapped = clipcurves::nearestEuler(Vector3(-20, 0, 0), Vector3(170, 0, 0));
    EXPECT_NEAR(wrapped.x, 340.0f, 1e-3f);
    EXPECT_NEAR(wrapped.y, 0.0f, 1e-3f);
}

// A rotation turning 0 -> 170 -> 340 -> 510 degrees about x shows a continuous Euler curve: key
// points at those angles (not 0, 170, -20, 150) and samples between them without jumps
TEST(ClipCurves, RotationCurvesAreContinuousAcrossKeys) {
    ClipData data;
    ClipTrackData rotation;
    rotation.path = "Arm";
    rotation.channel = ClipChannel::Rotation;
    rotation.keys = {key(0.0f, rotationX(0)), key(1.0f, rotationX(170)), key(2.0f, rotationX(340)), key(3.0f, rotationX(510))};
    data.tracks = {rotation};
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    ASSERT_EQ(clip->getTracks().size(), 1u);
    const ClipData normalizedData = clip->toData();
    const ClipTrackData& normalized = normalizedData.tracks[0];

    const std::vector<Vector3> eulers = clipcurves::keyEulers(normalized);
    ASSERT_EQ(eulers.size(), 4u);
    const float expected[] = {0.0f, 170.0f, 340.0f, 510.0f};
    for (size_t k = 0; k < 4; k++) {
        EXPECT_NEAR(eulers[k].x, expected[k], 0.05f) << "key " << k;
        EXPECT_NEAR(eulers[k].y, 0.0f, 0.05f);
        EXPECT_EQ(clipcurves::keyValues(normalized, k, eulers)[0], eulers[k].x);
    }

    std::vector<float> times;
    for (int i = 0; i <= 300; i++) times.push_back(3.0f * (float)i / 300.0f);
    std::vector<std::vector<float>> curves;
    ASSERT_TRUE(clipcurves::sampleCurves(clip->getTracks()[0], normalized, times, curves));
    ASSERT_EQ(curves.size(), times.size());
    for (size_t i = 1; i < curves.size(); i++) {
        EXPECT_LT(std::fabs(curves[i][0] - curves[i - 1][0]), 10.0f) << "no jump at " << times[i];
    }
    // through the key points, and the runtime sampler's rotation at every time
    EXPECT_NEAR(curves[100][0], 170.0f, 0.05f);
    EXPECT_NEAR(curves[200][0], 340.0f, 0.05f);
    EXPECT_NEAR(curves[300][0], 510.0f, 0.05f);
    for (size_t i = 0; i < times.size(); i += 17) {
        anim::TrackSample sample;
        ASSERT_TRUE(anim::sampleTrack(clip->getTracks()[0], times[i], sample));
        std::vector<float> xyzw(4);
        quaternionToXYZW(sample.rotation, xyzw.data());
        EXPECT_TRUE(sameRotation(xyzw, clipui::xyzwOf(Vector3(curves[i][0], curves[i][1], curves[i][2])))) << times[i];
    }
}

// Position, morph and lattice curves are the runtime sampler's values (eases included)
TEST(ClipCurves, ValuesComeFromTheRuntimeSampler) {
    ClipData data;
    ClipTrackData position;
    position.path = "";
    position.channel = ClipChannel::Position;
    position.keys = {key(0.0f, {0, 0, 0}), key(1.0f, {1, 2, 3}), key(2.0f, {0, 4, 0})};
    position.keys[0].ease = EaseType::CUBIC_IN_OUT;
    ClipTrackData morph;
    morph.path = "Face";
    morph.channel = ClipChannel::Morph;
    morph.keys = {key(0.0f, {0.0f, 1.0f}), key(1.0f, {1.0f, 0.0f})};
    ClipTrackData lattice;
    lattice.path = "Cage";
    lattice.channel = ClipChannel::Lattice;
    lattice.points = {2};
    lattice.keys = {key(0.0f, {0, 0, 0}), key(2.0f, {0, 1, 0})};
    data.tracks = {position, morph, lattice};
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    const ClipData normalized = clip->toData();
    const std::vector<float> times = {0.0f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f};
    for (size_t t = 0; t < normalized.tracks.size(); t++) {
        const ClipTrackData& track = normalized.tracks[t];
        const int index = clip->findTrack(track.path, track.channel);
        ASSERT_GE(index, 0);
        std::vector<std::vector<float>> curves;
        ASSERT_TRUE(clipcurves::sampleCurves(clip->getTracks()[(size_t)index], track, times, curves));
        for (size_t i = 0; i < times.size(); i++) {
            anim::TrackSample sample;
            ASSERT_TRUE(anim::sampleTrack(clip->getTracks()[(size_t)index], times[i], sample));
            const std::vector<float> flat = clipui::flatten(sample);
            ASSERT_EQ(curves[i].size(), clipcurves::componentCount(track));
            for (size_t c = 0; c < curves[i].size(); c++) EXPECT_FLOAT_EQ(curves[i][c], flat[c]) << "track " << t << " t " << times[i];
        }
    }
    // the eased segment is not the straight line at its middle... and the sampled y at 0.5 says so
    std::vector<std::vector<float>> curves;
    const int index = clip->findTrack("", ClipChannel::Position);
    ASSERT_TRUE(clipcurves::sampleCurves(clip->getTracks()[(size_t)index], normalized.tracks[(size_t)normalized.findTrack("", ClipChannel::Position)], {0.25f}, curves));
    EXPECT_LT(curves[0][1], 0.5f) << "cubic in-out starts slower than linear";
}

TEST(ClipCurves, NiceTicks) {
    EXPECT_FLOAT_EQ(clipcurves::niceStep(10.0f, 100.0f, 26.0f), 5.0f);
    EXPECT_FLOAT_EQ(clipcurves::niceStep(1.0f, 400.0f, 26.0f), 0.1f);
    EXPECT_FLOAT_EQ(clipcurves::niceStep(360.0f, 200.0f, 26.0f), 50.0f);
    EXPECT_FLOAT_EQ(clipcurves::niceStep(0.003f, 300.0f, 30.0f), 0.0005f);
    const std::vector<float> ticks = clipcurves::ticks(-0.3f, 1.05f, 0.5f);
    ASSERT_EQ(ticks.size(), 3u);
    EXPECT_FLOAT_EQ(ticks[0], 0.0f);
    EXPECT_FLOAT_EQ(ticks[1], 0.5f);
    EXPECT_FLOAT_EQ(ticks[2], 1.0f);
    EXPECT_TRUE(clipcurves::ticks(0.0f, 1.0f, 0.0f).empty());
    EXPECT_TRUE(clipcurves::ticks(0.0f, 1e9f, 1.0f).empty()) << "capped";
}

// ---- record sources (headless project) ------------------------------------------------------

namespace {

    struct Host : public EditorHost {
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
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-clip-record-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: cliprecord\nassetsDir: assets\n";
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

    bool headlessStarted = false;

    Project* project() {
        headlessStarted = true;
        return headless().project.get();
    }
    fs::path assets() { return project()->getAssetsPath(); }

    struct Cleanup : public ::testing::Environment {
        void TearDown() override {
            if (!headlessStarted) return;
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

    struct RecordScene {
        uint32_t id = 0;
        SceneProject* sceneProject = nullptr;
        Scene* scene = nullptr;
        Entity hero = NULL_ENTITY;
        Entity cage = NULL_ENTITY;
        Entity prop = NULL_ENTITY;
    };

    Entity addObject(RecordScene& s, const std::string& name, Entity parent) {
        const Entity entity = s.scene->createUserEntity();
        s.scene->setEntityName(entity, name);
        s.scene->addComponent<Transform>(entity, {});
        if (parent != NULL_ENTITY) s.scene->addEntityChild(parent, entity, false);
        s.sceneProject->entities.push_back(entity);
        return entity;
    }

    // Hero (Animation Player: anim/rec.anim, a Position track) > Cage (Lattice), Prop beside it
    RecordScene recordScene() {
        RecordScene s;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        ClipData data;
        ClipTrackData position;
        position.path = "";
        position.channel = ClipChannel::Position;
        position.keys = {key(0.0f, {0, 0, 0})};
        data.tracks = {position};
        writeAll(assets() / "anim/rec.anim", AnimationClipDocument::fileText(data));
        AnimationClipDocument::checkDiskChanges(p);
        p->createNewScene("Record", SceneType::SCENE_3D);
        s.id = p->getSelectedSceneId();
        s.sceneProject = p->getScene(s.id);
        s.scene = s.sceneProject->scene;
        s.hero = addObject(s, "Hero", NULL_ENTITY);
        s.cage = addObject(s, "Cage", s.hero);
        s.scene->addComponent<LatticeComponent>(s.cage, {});
        s.prop = addObject(s, "Prop", NULL_ENTITY);
        s.scene->addComponent<LatticeComponent>(s.prop, {});
        AnimationPlayerComponent player;
        AnimationPlayerClip entry;
        entry.clip = "anim/rec.anim";
        player.clips = {entry};
        s.scene->addComponent<AnimationPlayerComponent>(s.hero, player);
        s.sceneProject->isModified = false;
        return s;
    }

    class ClipRecordSourcesTest : public ::testing::Test {
    protected:
        Host host;
        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            setEditorHost(&host);
        }
        void TearDown() override {
            ClipRecordHooks::endDrag();
            AnimationClipController::release(project());
            setEditorHost(nullptr);
        }
    };

}

TEST_F(ClipRecordSourcesTest, SourceHelpers) {
    RecordScene s = recordScene();
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    EXPECT_FALSE(ClipRecordHooks::claimsAny(s.id, {s.hero, s.prop}, ClipChannel::Position)) << "not recording";
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    EXPECT_TRUE(ClipRecordHooks::claimsAny(s.id, {s.prop, s.cage}, ClipChannel::Position)) << "the Cage is under the root";
    EXPECT_FALSE(ClipRecordHooks::claimsAny(s.id, {s.prop}, ClipChannel::Position));
    EXPECT_FALSE(ClipRecordHooks::claimsAny(s.id, {}, ClipChannel::Position));

    const ClipRecordValue rotation = ClipRecordHooks::transformValue(ClipChannel::Rotation, Vector3(1, 2, 3), Quaternion(0.7071068f, 0.7071068f, 0, 0), Vector3(4, 5, 6));
    ASSERT_EQ(rotation.value.size(), 4u);
    EXPECT_NEAR(rotation.value[0], 0.7071068f, 1e-6f) << "x y z w";
    EXPECT_NEAR(rotation.value[3], 0.7071068f, 1e-6f);
    EXPECT_EQ(ClipRecordHooks::transformValue(ClipChannel::Scale, Vector3(1, 2, 3), Quaternion(), Vector3(4, 5, 6)).value, (std::vector<float>{4, 5, 6}));
    EXPECT_EQ(ClipRecordHooks::transformValue(ClipChannel::Position, Vector3(1, 2, 3), Quaternion(), Vector3(4, 5, 6)).value, (std::vector<float>{1, 2, 3}));

    // a gesture opens a drag when none is open, else joins the open one
    bool opened = false;
    const std::string first = ClipRecordHooks::gestureMergeKey(&opened);
    EXPECT_TRUE(opened);
    EXPECT_EQ(first.rfind("record#", 0), 0u);
    const std::string joined = ClipRecordHooks::gestureMergeKey(&opened);
    EXPECT_FALSE(opened);
    EXPECT_EQ(joined, first);
    ClipRecordHooks::endDrag();
    EXPECT_NE(ClipRecordHooks::gestureMergeKey(&opened), first) << "a new gesture, a new merge key";
    ClipRecordHooks::endDrag();
    c.stopPreview();
}

// A Scene view drag of lattice points (LatticeEditorOps' continuous edit) while Record claims the
// lattice: live offsets, one key edit per drag at the playhead, no scene step; MCP's discrete
// setOffsets (no gesture) and an unbound lattice stay scene steps
TEST_F(ClipRecordSourcesTest, LatticeDragsRecordOneKeyEditPerDrag) {
    RecordScene s = recordScene();
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.setTime(0.5f);
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    size_t projectSteps = project()->getProjectCommandHistory()->position();
    size_t sceneSteps = CommandHandle::get(s.id)->position();

    // MCP edit_lattice (a discrete setOffsets, no record gesture) of the claimed cage: a scene
    // step, never a key
    ASSERT_TRUE(ClipRecordHooks::claims(s.id, s.cage, ClipChannel::Lattice));
    ASSERT_TRUE(LatticeEditorOps::setOffsets(project(), s.id, s.cage, {0}, {Vector3(0, 0, 0.5f)}, &error)) << error;
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), projectSteps);
    EXPECT_EQ(CommandHandle::get(s.id)->position(), sceneSteps + 1);
    EXPECT_LT(c.getDocument()->data().findTrack("Cage", ClipChannel::Lattice), 0);
    sceneSteps++;

    for (int step = 1; step <= 3; step++) {
        ASSERT_TRUE(LatticeEditorOps::setOffsetsContinuous(project(), s.id, s.cage, {4}, {Vector3(0, 0.1f * (float)step, 0)}, &error)) << error;
    }
    LatticeEditorOps::endContinuousEdit(s.id);
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), projectSteps + 1) << "one key edit for the drag";
    EXPECT_EQ(CommandHandle::get(s.id)->position(), sceneSteps) << "no scene step";
    EXPECT_NEAR(s.scene->getComponent<LatticeComponent>(s.cage).offsets[4].y, 0.3f, 1e-5f) << "the live point";
    const int track = c.getDocument()->data().findTrack("Cage", ClipChannel::Lattice);
    ASSERT_GE(track, 0);
    const ClipTrackData& recorded = c.getDocument()->data().tracks[(size_t)track];
    EXPECT_EQ(recorded.points, std::vector<int>{4});
    ASSERT_EQ(recorded.keys.size(), 1u);
    EXPECT_NEAR(recorded.keys[0].time, 0.5f, 1e-5f);
    EXPECT_NEAR(recorded.keys[0].value[1], 0.3f, 1e-5f);

    // a second drag: its own step
    ASSERT_TRUE(LatticeEditorOps::setOffsetsContinuous(project(), s.id, s.cage, {2}, {Vector3(0.2f, 0, 0)}, &error)) << error;
    LatticeEditorOps::endContinuousEdit(s.id);
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), projectSteps + 2);
    EXPECT_EQ(c.getDocument()->data().tracks[(size_t)c.getDocument()->data().findTrack("Cage", ClipChannel::Lattice)].points, (std::vector<int>{2, 4}));

    // an Inspector row (which opens a gesture first) is recorded
    bool opened = false;
    ClipRecordHooks::gestureMergeKey(&opened);
    ASSERT_TRUE(LatticeEditorOps::setOffsets(project(), s.id, s.cage, {1}, {Vector3(0, 0, 0.25f)}, &error)) << error;
    ClipRecordHooks::endDrag();
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), projectSteps + 3);
    EXPECT_EQ(CommandHandle::get(s.id)->position(), sceneSteps);

    // a lattice outside the bound hierarchy: a scene step
    ASSERT_TRUE(LatticeEditorOps::setOffsetsContinuous(project(), s.id, s.prop, {4}, {Vector3(0, 1, 0)}, &error)) << error;
    LatticeEditorOps::endContinuousEdit(s.id);
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), projectSteps + 3);
    EXPECT_EQ(CommandHandle::get(s.id)->position(), sceneSteps + 1);

    // leaving the preview (Record) puts the cage back
    c.stopPreview();
    EXPECT_FALSE(c.isRecording());
    EXPECT_NEAR(s.scene->getComponent<LatticeComponent>(s.cage).offsets[4].y, 0.0f, 1e-6f);
    EXPECT_NEAR(s.scene->getComponent<LatticeComponent>(s.cage).offsets[2].x, 0.0f, 1e-6f);
    EXPECT_NEAR(s.scene->getComponent<LatticeComponent>(s.cage).offsets[1].z, 0.0f, 1e-6f);
    EXPECT_NEAR(s.scene->getComponent<LatticeComponent>(s.cage).offsets[0].z, 0.5f, 1e-6f) << "the scene step stays";
}

// The Pose window listens on the chained beforeChannelWrite (it replaced
// poseInspectorHooks().beforeTransformWrite): a scene command writing a posed bone's Transform
// suspends the pose first, so the command records and edits the rest pose
TEST_F(ClipRecordSourcesTest, PoseWindowSuspendsThroughTheChannelWriteChain) {
    fs::create_directories(assets() / "models");
    fs::copy_file(fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "tools" / "fixtures" / "pose-animation" / "assets" /
                  "models" / "hero_tpose.gltf", assets() / "models/hero.gltf", fs::copy_options::overwrite_existing);
    RecordScene s = recordScene();
    Model heroModel(s.scene);
    const Entity model = heroModel.getEntity();
    s.scene->setEntityName(model, "Character");
    s.sceneProject->entities.push_back(model);
    ASSERT_TRUE(s.scene->getSystem<MeshSystem>()->loadGLTF(model, (assets() / "models/hero.gltf").string(), false, false, false));
    s.scene->getComponent<ModelComponent>(model).filename = "models/hero.gltf";
    Entity leftArm = NULL_ENTITY;
    for (const auto& entry : anim::descendantPaths(s.scene, model)) {
        const std::string name = s.scene->getEntityName(entry.second);
        if (name.size() >= 7 && name.compare(name.size() - 7, 7, "LeftArm") == 0) leftArm = entry.second;
    }
    ASSERT_NE(leftArm, NULL_ENTITY);
    pose::PoseAnimationData poseData;
    pose::PoseKey down;
    for (size_t i = 0; i < down.body.angles.size(); i++) down.body.angles[i] = (i % 3 == 0) ? 20.0f : 0.0f;
    poseData.poses = {down};
    std::string error;
    fs::remove(assets() / "pose/down.poseanim");
    ASSERT_TRUE(PoseAnimationDocument::create(project(), "assets/pose/down.poseanim", poseData, "Create", nullptr, &error)) << error;
    const Transform rest = s.scene->getComponent<Transform>(leftArm);

    PoseEditSession session(project());
    PoseEditSession::setActive(&session);
    ASSERT_TRUE(session.bind(s.scene, s.id, model, &error)) << error;
    ASSERT_TRUE(session.open("assets/pose/down.poseanim", &error)) << error;

    // what the Pose window registers
    const ClipRecordHooks::Id listener = ClipRecordHooks::addBeforeChannelWrite(&PoseEditSession::beforeChannelWrite);
    session.startPosing();
    ASSERT_TRUE(session.bonesApplied());
    ASSERT_NE(s.scene->getComponent<Transform>(leftArm).rotation, rest.rotation) << "posed";

    // a non-Transform channel of the bone leaves the pose alone
    ClipRecordHooks::beforeChannelWrite(s.id, leftArm, ClipChannel::Morph);
    EXPECT_TRUE(session.bonesApplied());

    // an Inspector / MCP position edit of the posed bone
    const Vector3 moved = rest.position + Vector3(0, 0.1f, 0);
    CommandHandle::get(s.id)->addCommandNoMerge(new PropertyCmd<Vector3>(project(), s.id, leftArm, ComponentType::Transform, "position", moved));
    EXPECT_FALSE(session.bonesApplied()) << "the pose was suspended before the command read the bone";
    const Transform& edited = s.scene->getComponent<Transform>(leftArm);
    EXPECT_EQ(edited.position, moved);
    EXPECT_EQ(edited.rotation, rest.rotation) << "the rest rotation, not the posed one";
    CommandHandle::get(s.id)->undo();
    EXPECT_EQ(s.scene->getComponent<Transform>(leftArm).position, rest.position) << "undo gives back the rest, not a posed value";

    ClipRecordHooks::removeChannelWriteListener(listener);
    session.goIdle();
    PoseEditSession::setActive(nullptr);
    session.unbind();
}
