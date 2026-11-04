// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's curves view helpers and Record's sources (animation clips spec 4.4,
// 9.R5/R50): clipcurves (components, the runtime sampler's values, Euler continuity across keys,
// nice ticks); the record source helpers of ClipRecordHooks; LatticeEditorOps' drag recorded as
// one key edit per drag (MCP's discrete edits stay scene steps); the Pose window's listener on the
// chained beforeChannelWrite (a scene command on a posed bone edits its rest). Headless project as
// in AnimationClipControllerTest.

#include <gtest/gtest.h>
#include "component/Transform.h"

#include "support/EditorTestSupport.h"

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
#include "command/type/MorphWeightsCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "component/MeshComponent.h"
#include "window/AnimationWindowClip.h"
#include "imgui.h"
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
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
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
            ClipRecordHooks::endAllGestures();
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

    // a source's gesture opens when it has none, else joins its own; another source's is never
    // joined; Record stopping ends every gesture
    bool opened = false;
    const std::string first = ClipRecordHooks::gestureMergeKey(RecordSource::Gizmo, &opened);
    EXPECT_TRUE(opened);
    EXPECT_EQ(first.rfind("record#", 0), 0u);
    const std::string joined = ClipRecordHooks::gestureMergeKey(RecordSource::Gizmo, &opened);
    EXPECT_FALSE(opened);
    EXPECT_EQ(joined, first);
    const std::string other = ClipRecordHooks::gestureMergeKey(RecordSource::Inspector, &opened);
    EXPECT_TRUE(opened);
    EXPECT_NE(other, first) << "another source: its own gesture";
    ClipRecordHooks::endGesture(RecordSource::Inspector);
    EXPECT_TRUE(ClipRecordHooks::hasGesture(RecordSource::Gizmo)) << "ending one source's gesture leaves the others";
    ClipRecordHooks::endGesture(RecordSource::Gizmo);
    EXPECT_NE(ClipRecordHooks::gestureMergeKey(RecordSource::Gizmo, &opened), first) << "a new gesture, a new merge key";
    c.setRecording(false);
    EXPECT_FALSE(ClipRecordHooks::hasGesture(RecordSource::Gizmo)) << "no gesture outlives Record";
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

    // the Inspector's Offset row (asks to record) is recorded, its own one-step gesture
    ASSERT_TRUE(LatticeEditorOps::setOffsets(project(), s.id, s.cage, {1}, {Vector3(0, 0, 0.25f)}, &error, true)) << error;
    EXPECT_FALSE(ClipRecordHooks::hasGesture(RecordSource::Inspector));
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
    fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" / "assets" /
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

// ---- review round 1 -------------------------------------------------------------------------

namespace {

    // the Hero player's clip replaced by `data` (before binding)
    void writeRecClip(const ClipData& data) {
        writeAll(assets() / "anim/rec.anim", AnimationClipDocument::fileText(data));
        AnimationClipDocument::checkDiskChanges(project());
    }

    ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<ClipKey> keys, std::vector<int> points = {}) {
        ClipTrackData out;
        out.path = path;
        out.channel = channel;
        out.keys = std::move(keys);
        out.points = std::move(points);
        return out;
    }

    Vector3 offsetOf(const RecordScene& s, int point) {
        return s.scene->getComponent<LatticeComponent>(s.cage).offsets[(size_t)point];
    }

}

// C1 (P1, P2 + redo after stop): a scene step on a previewed lattice changes only the points it
// edits; the previewed points are never baked into the scene, by the step, its undo or its redo
TEST_F(ClipRecordSourcesTest, LatticeSceneStepsDuringThePreviewKeepThePreviewedPointsRest) {
    RecordScene s = recordScene();
    ClipData data;
    data.tracks = {track("Cage", ClipChannel::Lattice, {key(0.0f, {0, 0.5f, 0}), key(1.0f, {0, 0.5f, 0})}, {4})};
    writeRecClip(data);
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    c.update(0.0);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_NEAR(offsetOf(s, 4).y, 0.5f, 1e-5f) << "previewed";

    // P1: an edit of point 0 (computed from the shown offsets, as every LatticeEditorOps edit)
    ASSERT_TRUE(LatticeEditorOps::setOffsets(project(), s.id, s.cage, {0}, {Vector3(0, 0, 0.25f)}, &error)) << error;
    c.update(0.0);
    EXPECT_NEAR(offsetOf(s, 4).y, 0.5f, 1e-5f) << "still previewed";
    c.stopPreview();
    EXPECT_NEAR(offsetOf(s, 0).z, 0.25f, 1e-6f) << "the edited point";
    EXPECT_NEAR(offsetOf(s, 4).y, 0.0f, 1e-6f) << "the previewed point is not baked";
    // redo after the preview: the command's own values are clean
    CommandHandle::get(s.id)->undo();
    EXPECT_NEAR(offsetOf(s, 0).z, 0.0f, 1e-6f);
    CommandHandle::get(s.id)->redo();
    EXPECT_NEAR(offsetOf(s, 0).z, 0.25f, 1e-6f);
    EXPECT_NEAR(offsetOf(s, 4).y, 0.0f, 1e-6f) << "redo bakes nothing";
    CommandHandle::get(s.id)->undo();

    // P2: the same edit undone during the preview
    c.seek(0.5f);
    c.update(0.0);
    ASSERT_NEAR(offsetOf(s, 4).y, 0.5f, 1e-5f);
    ASSERT_TRUE(LatticeEditorOps::setOffsets(project(), s.id, s.cage, {0}, {Vector3(0, 0, 0.25f)}, &error)) << error;
    CommandHandle::get(s.id)->undo();
    c.update(0.0);
    c.stopPreview();
    EXPECT_NEAR(offsetOf(s, 0).z, 0.0f, 1e-6f) << "undone";
    EXPECT_NEAR(offsetOf(s, 4).y, 0.0f, 1e-6f) << "the undo bakes nothing";
}

// C1 (P3): the Blend Shapes rows' MorphWeightsCmd during the preview changes only the edited weight
TEST_F(ClipRecordSourcesTest, BlendShapeEditsDuringThePreviewKeepThePreviewedWeightsRest) {
    RecordScene s = recordScene();
    const Entity face = addObject(s, "Face", s.hero);
    s.scene->addComponent<MeshComponent>(face, {});
    ClipData data;
    data.tracks = {track("Face", ClipChannel::Morph, {key(0.0f, {0.8f}), key(1.0f, {0.8f})})};
    writeRecClip(data);
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    c.update(0.0);
    ASSERT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[0], 0.8f, 1e-5f) << "previewed";

    // what the slider builds: the shown weights with weight 1 changed (two drag steps, merged)
    for (float value : {0.25f, 0.5f}) {
        MorphWeightsCmd::Target target;
        target.entity = face;
        const MeshComponent& mesh = s.scene->getComponent<MeshComponent>(face);
        std::copy(mesh.morphWeights, mesh.morphWeights + MAX_MORPHTARGETS, target.after.begin());
        target.after[1] = value;
        ASSERT_TRUE(CommandHandle::get(s.id)->addCommand(new MorphWeightsCmd(project(), s.id, {target})));
        c.update(0.0);
    }
    CommandHandle::get(s.id)->mark();
    EXPECT_TRUE(s.sceneProject->isModified);
    c.stopPreview();
    const MeshComponent& mesh = s.scene->getComponent<MeshComponent>(face);
    EXPECT_NEAR(mesh.morphWeights[0], 0.0f, 1e-6f) << "the previewed weight is not baked";
    EXPECT_NEAR(mesh.morphWeights[1], 0.5f, 1e-6f) << "the edited weight";
    // one undo step for the drag; the modified flag back
    CommandHandle::get(s.id)->undo();
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[1], 0.0f, 1e-6f);
    EXPECT_FALSE(s.sceneProject->isModified);
    CommandHandle::get(s.id)->redo();
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[1], 0.5f, 1e-6f);
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[0], 0.0f, 1e-6f) << "redo bakes nothing";
    // the weights are a Catalog property (prefab instance overrides: morphWeights[i])
    float* weight = Catalog::getPropertyRef<float>(s.scene, face, ComponentType::MeshComponent, "morphWeights[1]");
    ASSERT_NE(weight, nullptr);
    EXPECT_FLOAT_EQ(*weight, 0.5f);
    EXPECT_EQ(Catalog::getPropertyRef<float>(s.scene, face, ComponentType::MeshComponent, "morphWeights[8]"), nullptr);
}

// C1 (P4): an Inspector Position row edit of x during the preview keeps the rest of y and z
TEST_F(ClipRecordSourcesTest, TransformRowEditsDuringThePreviewKeepTheOtherComponentsRest) {
    RecordScene s = recordScene();
    ClipData data;
    data.tracks = {track("", ClipChannel::Position, {key(0.0f, {1, 2, 3}), key(1.0f, {1, 2, 3})})};
    writeRecClip(data);
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    c.update(0.0);
    const Vector3 shown = s.scene->getComponent<Transform>(s.hero).position;
    ASSERT_EQ(shown, Vector3(1, 2, 3));
    // what the row's x field builds: (new x, shown y, shown z), y and z carried as shown
    auto* row = new PropertyCmd<Vector3>(project(), s.id, s.hero, ComponentType::Transform, "position", Vector3(5, shown.y, shown.z));
    row->setShownComponents(0b110);
    CommandHandle::get(s.id)->addCommandNoMerge(row);
    c.update(0.0);
    c.stopPreview();
    EXPECT_EQ(s.scene->getComponent<Transform>(s.hero).position, Vector3(5, 0, 0)) << "only x is the edit";
    CommandHandle::get(s.id)->undo();
    CommandHandle::get(s.id)->redo();
    EXPECT_EQ(s.scene->getComponent<Transform>(s.hero).position, Vector3(5, 0, 0)) << "redo bakes nothing";
}

// FX item 16: a named write equal to the previewed value is the new rest (set_properties of a
// position, an Inspector row's edited field, the Blend Shapes row's weight, a lattice point set by
// index); a gizmo-style whole write left as shown keeps the rest, and an undo restores exactly
TEST_F(ClipRecordSourcesTest, NamedWritesEqualToThePreviewAreTheNewRest) {
    RecordScene s = recordScene();
    const Entity face = addObject(s, "Face", s.hero);
    s.scene->addComponent<MeshComponent>(face, {});
    ClipData data;
    data.tracks = {track("", ClipChannel::Position, {key(0.0f, {1, 2, 3}), key(1.0f, {1, 2, 3})}),
                   track("Face", ClipChannel::Morph, {key(0.0f, {0.8f, 0.4f}), key(1.0f, {0.8f, 0.4f})}),
                   track("Cage", ClipChannel::Lattice, {key(0.0f, {0, 0.5f, 0, 0, 0.25f, 0}), key(1.0f, {0, 0.5f, 0, 0, 0.25f, 0})}, {4, 5})};
    writeRecClip(data);
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    auto preview = [&](){
        c.seek(0.5f);
        c.update(0.0);
        ASSERT_TRUE(c.isPreviewing());
        ASSERT_EQ(s.scene->getComponent<Transform>(s.hero).position, Vector3(1, 2, 3)) << "previewed";
    };
    auto position = [&](){ return s.scene->getComponent<Transform>(s.hero).position; };

    // set_properties {transform: {position: [1, 2, 3]}} (a PropertyCmd of the whole value)
    preview();
    CommandHandle::get(s.id)->addCommandNoMerge(new PropertyCmd<Vector3>(project(), s.id, s.hero, ComponentType::Transform, "position", Vector3(1, 2, 3)));
    c.update(0.0);
    c.stopPreview();
    EXPECT_EQ(position(), Vector3(1, 2, 3)) << "the value set on purpose is the scene's";
    CommandHandle::get(s.id)->undo();
    EXPECT_EQ(position(), Vector3(0, 0, 0)) << "undone";

    // an Inspector row: x typed as shown is the edit, y and z (carried as shown) keep the rest
    preview();
    auto* row = new PropertyCmd<Vector3>(project(), s.id, s.hero, ComponentType::Transform, "position", Vector3(1, 2, 3));
    row->setShownComponents(0b110);
    CommandHandle::get(s.id)->addCommandNoMerge(row);
    c.update(0.0);
    c.stopPreview();
    EXPECT_EQ(position(), Vector3(1, 0, 0));
    CommandHandle::get(s.id)->undo();

    // an undo during the preview gives the value it replaced back as the rest
    preview();
    CommandHandle::get(s.id)->addCommandNoMerge(new PropertyCmd<Vector3>(project(), s.id, s.hero, ComponentType::Transform, "position", Vector3(7, 7, 7)));
    c.update(0.0);
    CommandHandle::get(s.id)->undo();
    c.update(0.0);
    c.stopPreview();
    EXPECT_EQ(position(), Vector3(0, 0, 0)) << "the undo's value";

    // a whole-Transform write left as shown (the gizmo's ObjectTransformCmd) keeps the rest
    preview();
    const Transform shown = s.scene->getComponent<Transform>(s.hero);
    CommandHandle::get(s.id)->addCommandNoMerge(new ObjectTransformCmd(project(), s.id, s.hero, shown.position, shown.rotation, shown.scale));
    c.update(0.0);
    c.stopPreview();
    EXPECT_EQ(position(), Vector3(0, 0, 0)) << "a gizmo write of the shown pose is no edit";
    CommandHandle::get(s.id)->undo();

    // the Blend Shapes row: its weight as shown is the edit, the other weight keeps its rest
    preview();
    MorphWeightsCmd::Target target;
    target.entity = face;
    const MeshComponent& previewed = s.scene->getComponent<MeshComponent>(face);
    std::copy(previewed.morphWeights, previewed.morphWeights + MAX_MORPHTARGETS, target.after.begin());
    ASSERT_NEAR(target.after[1], 0.4f, 1e-6f);
    auto* weights = new MorphWeightsCmd(project(), s.id, {target});
    weights->setEditedWeights({1});
    ASSERT_TRUE(CommandHandle::get(s.id)->addCommand(weights));
    CommandHandle::get(s.id)->mark();
    c.update(0.0);
    c.stopPreview();
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[1], 0.4f, 1e-6f) << "the row's weight";
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[0], 0.0f, 1e-6f) << "the other weight's rest";
    CommandHandle::get(s.id)->undo();
    // set_properties {mesh: {"morphWeights[0]": 0.8}}: that weight, as shown, is the edit
    preview();
    CommandHandle::get(s.id)->addCommandNoMerge(new PropertyCmd<float>(project(), s.id, face, ComponentType::MeshComponent, "morphWeights[0]", 0.8f));
    c.update(0.0);
    c.stopPreview();
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[0], 0.8f, 1e-6f);
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[1], 0.0f, 1e-6f);
    CommandHandle::get(s.id)->undo();

    // a lattice point set by index (edit_lattice, the Inspector Offset row) to what the preview
    // shows: that point is the edit (also when no shown offset changes), the other keeps its rest
    preview();
    ASSERT_NEAR(offsetOf(s, 4).y, 0.5f, 1e-5f);
    ASSERT_TRUE(LatticeEditorOps::setOffsets(project(), s.id, s.cage, {4}, {Vector3(0, 0.5f, 0)}, &error)) << error;
    c.update(0.0);
    c.stopPreview();
    EXPECT_NEAR(offsetOf(s, 4).y, 0.5f, 1e-6f) << "the named point";
    EXPECT_NEAR(offsetOf(s, 5).y, 0.0f, 1e-6f) << "the other previewed point keeps its rest";
    CommandHandle::get(s.id)->undo();
    EXPECT_NEAR(offsetOf(s, 4).y, 0.0f, 1e-6f);
}

// I4 (P5): a discrete lattice edit (MCP edit_lattice) while another source holds a record gesture
// is a scene step, never recorded into that gesture
TEST_F(ClipRecordSourcesTest, AnMcpLatticeEditDuringAUserDragIsNeverRecorded) {
    RecordScene s = recordScene();
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    const size_t projectSteps = project()->getProjectCommandHistory()->position();
    const size_t sceneSteps = CommandHandle::get(s.id)->position();
    // a gizmo drag in progress (its gesture open) and an Inspector row being dragged
    ClipRecordHooks::gestureMergeKey(RecordSource::Gizmo, nullptr);
    ClipRecordHooks::gestureMergeKey(RecordSource::Inspector, nullptr);
    ASSERT_TRUE(LatticeEditorOps::setOffsets(project(), s.id, s.cage, {0}, {Vector3(0, 0, 0.5f)}, &error)) << error;
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), projectSteps) << "not recorded";
    EXPECT_EQ(CommandHandle::get(s.id)->position(), sceneSteps + 1) << "a scene step";
    EXPECT_LT(c.getDocument()->data().findTrack("Cage", ClipChannel::Lattice), 0);
    c.stopPreview();
    EXPECT_FALSE(ClipRecordHooks::hasGesture(RecordSource::Gizmo)) << "stopping the preview ends the gestures";
}

// I5 (P6): Record keys on the Samples grid even when the playhead is between frames, and a
// playing preview pauses there
TEST_F(ClipRecordSourcesTest, RecordKeysOnTheFrameGrid) {
    RecordScene s = recordScene();
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.517f);
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    ASSERT_TRUE(ClipRecordHooks::claims(s.id, s.hero, ClipChannel::Position)) << "recording";
    c.play();
    ASSERT_TRUE(c.isPlaying());
    ASSERT_TRUE(ClipRecordHooks::claims(s.id, s.hero, ClipChannel::Position)) << "playing";
    ClipRecordValue value;
    value.channel = ClipChannel::Position;
    value.value = {0, 1, 0};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, s.hero, value, ""));
    EXPECT_FALSE(c.isPlaying()) << "the first record pauses the playing preview";
    AnimationClipController::KeyRef ref;
    // 0.517 s is frame 15.51 at 30 fps: the nearest frame is 16
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "", ClipChannel::Position, 16.0f / 30.0f, ref)) << "frame 16";
    EXPECT_NEAR(c.getDocument()->data().tracks[ref.track].keys[ref.key].time, 16.0f / 30.0f, 1e-6f);
    EXPECT_NEAR(c.getTime(), 16.0f / 30.0f, 1e-6f) << "the playhead on the key";
    c.stopPreview();
}

// I2: one absolute value per dragged key: a Position key's x and a Rotation key's Euler y move by
// the same delta on their own curves, one merge key
TEST_F(ClipRecordSourcesTest, KeyDragsTakeOneAbsoluteValuePerKey) {
    RecordScene s = recordScene();
    ClipData data;
    data.tracks = {track("", ClipChannel::Position, {key(0.0f, {0, 0, 0}), key(1.0f, {0, 0, 0})}),
                   track("Cage", ClipChannel::Rotation, {key(0.0f, clipui::xyzwOf(Vector3(30, 0, 0))), key(1.0f, clipui::xyzwOf(Vector3(30, 0, 0)))})};
    writeRecClip(data);
    AnimationClipController& c = AnimationClipController::shared(project());
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    const ClipData& clip = c.getDocument()->data();
    AnimationClipController::KeyRef position, rotation;
    ASSERT_TRUE(AnimationClipController::findKey(clip, "", ClipChannel::Position, 1.0f, position));
    ASSERT_TRUE(AnimationClipController::findKey(clip, "Cage", ClipChannel::Rotation, 1.0f, rotation));
    const size_t steps = project()->getProjectCommandHistory()->position();
    ASSERT_TRUE(c.beginKeyDrag({position, rotation}, &error)) << error;
    for (float delta : {5.0f, 10.0f}) {
        const ClipData& base = c.dragBaseData();
        std::vector<AnimationClipController::KeyDragValue> values(2);
        values[0].dragIndex = 0;
        values[0].value = base.tracks[position.track].keys[position.key].value;
        values[0].value[0] += delta;
        values[1].dragIndex = 1;
        const Vector3 euler = clipcurves::keyEulers(base.tracks[rotation.track])[rotation.key];
        values[1].value = clipui::xyzwOf(Vector3(euler.x, euler.y + delta, euler.z));
        ASSERT_TRUE(c.updateKeyDrag(0.0f, values, nullptr, &error)) << error;
    }
    c.endKeyDrag();
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), steps + 1) << "one merge key";
    const ClipData& after = c.getDocument()->data();
    ASSERT_TRUE(AnimationClipController::findKey(after, "", ClipChannel::Position, 1.0f, position));
    ASSERT_TRUE(AnimationClipController::findKey(after, "Cage", ClipChannel::Rotation, 1.0f, rotation));
    EXPECT_EQ(after.tracks[position.track].keys[position.key].value, (std::vector<float>{10, 0, 0}));
    const Vector3 euler = clipcurves::keyEulers(after.tracks[rotation.track])[rotation.key];
    EXPECT_NEAR(euler.x, 30.0f, 0.01f);
    EXPECT_NEAR(euler.y, 10.0f, 0.01f);
    EXPECT_NEAR(euler.z, 0.0f, 0.01f);
    // a value of the wrong width is refused
    ASSERT_TRUE(c.beginKeyDrag({position}, &error));
    std::vector<AnimationClipController::KeyDragValue> bad(1);
    bad[0].value = {1, 2};
    EXPECT_FALSE(c.updateKeyDrag(0.0f, bad, nullptr, &error));
    c.endKeyDrag();
}

// I6: the window's settings handler keeps its section in an ini written by a session that never
// shows the window
TEST(ClipWindowSettings, AnIniSavedWithoutTheWindowKeepsItsSettings) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGui::GetIO().IniFilename = nullptr;
    AnimationWindowClip::registerSettingsHandler();
    AnimationWindowClip::registerSettingsHandler();     // once per context
    const char* ini = "[JasperAnimationWindow][Clip]\nLabelWidth=412.0\nView=1\n\n";
    ImGui::LoadIniSettingsFromMemory(ini);
    size_t size = 0;
    const std::string saved = ImGui::SaveIniSettingsToMemory(&size);
    EXPECT_NE(saved.find("[JasperAnimationWindow][Clip]"), std::string::npos) << saved;
    EXPECT_NE(saved.find("LabelWidth=412.0"), std::string::npos) << saved;
    EXPECT_NE(saved.find("View=1"), std::string::npos) << saved;
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}
