// SPDX-License-Identifier: MIT

// Physics Placement, editor side (physics/PlacementSettings, physics/PlacementSession): the feel
// presets and world settings, the movers of a selection, spec checks and fingerprints, the batch
// run, the interactive session's gestures, commit as one undo step (ObjectTransformCmd with
// explicit old values) and the place_with_physics / place-physics JSON. Runs a headless project
// with real 3D scenes, as the CLI does.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "component/Joint3DComponent.h"
#include "physics/PlacementSession.h"
#include "physics/PlacementSettings.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "util/EntityPath.h"
#include "Stream.h"

#include "json.hpp"

#include "support/PosixCompat.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

    // ---- one headless project for the whole binary (on the engine TestMain started)

    struct Headless {
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-placement-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            // the editor's own settings (last project, ...) stay out of the user's
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: placement\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already: S02's TestMain started it with its TestSystem
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            if (!out.project->loadProject(out.dir, false))
                throw std::runtime_error("failed to load the placement test project");
            return out;
        }();
        return h;
    }

    struct EngineShutdown : public ::testing::Environment {
        void TearDown() override;
    };

    bool headlessStarted = false;

    void EngineShutdown::TearDown() {
        if (!headlessStarted) return;
        placementProjectSwitching();
        // the project's scenes and their renders go before the engine does (~Project leaves
        // them: LeakSanitizer reported the last scene's SceneRender and what it holds)
        if (headless().project) headless().project->releaseScenes();
        ShaderPool::setShaderBuilder(nullptr);
        ShaderBuilder::requestShutdown();
        // (TestMain's environment shuts the engine down)
    }

    [[maybe_unused]] ::testing::Environment* const engineShutdown = ::testing::AddGlobalTestEnvironment(new EngineShutdown);

    Project* project() {
        headlessStarted = true;
        return headless().project.get();
    }

    // A new empty 3D scene, selected
    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Placement", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Scene* sceneOf(uint32_t sceneId) { return project()->getScene(sceneId)->scene; }

    Entity create(uint32_t sceneId, const std::string& name, EntityCreationType type, const Vector3& position,
                  Entity parent = NULL_ENTITY, const Vector3& scale = Vector3(1.0f, 1.0f, 1.0f),
                  const Quaternion& rotation = Quaternion()) {
        CreateEntityCmd* cmd = parent == NULL_ENTITY ? new CreateEntityCmd(project(), sceneId, name, type)
                                                     : new CreateEntityCmd(project(), sceneId, name, type, parent);
        cmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
        cmd->addProperty<Vector3>(ComponentType::Transform, "scale", scale);
        cmd->addProperty<Quaternion>(ComponentType::Transform, "rotation", rotation);
        cmd->setQuiet(true);
        CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

    Transform& transformOf(uint32_t sceneId, Entity entity) { return sceneOf(sceneId)->getComponent<Transform>(entity); }

    Quaternion yaw(float degrees) {
        const float r = degrees * 3.14159265358979f / 180.0f;
        return Quaternion(std::cos(r / 2.0f), 0.0f, std::sin(r / 2.0f), 0.0f);
    }

    void expectNear(const Vector3& a, const Vector3& b, float eps) {
        EXPECT_NEAR(a.x, b.x, eps);
        EXPECT_NEAR(a.y, b.y, eps);
        EXPECT_NEAR(a.z, b.z, eps);
    }

    void expectSameRotation(const Quaternion& a, const Quaternion& b, float eps) {
        EXPECT_NEAR(std::fabs(a.dot(b)), 1.0f, eps);
    }

    bool contains(const std::vector<std::string>& lines, const std::string& part) {
        for (const std::string& line : lines) {
            if (line.find(part) != std::string::npos) return true;
        }
        return false;
    }

#ifdef JASPERENGINE_PHYSICS_3D
#define REQUIRE_PHYSICS() do {} while (0)
#else
#define REQUIRE_PHYSICS() GTEST_SKIP() << "built without JASPERENGINE_PHYSICS_3D"
#endif

}

// ---- settings

TEST(PhysicsPlacement, FeelPresetsFollowTheSpecTable) {
    const PlacementTuning precise = placementFeelTuning(PlacementFeel::Precise);
    EXPECT_FLOAT_EQ(precise.maxSpeed, 4.0f);
    EXPECT_FLOAT_EQ(precise.contactSpeed, 0.3f);
    EXPECT_EQ(precise.substeps, 2);
    EXPECT_EQ(precise.velocityIterations, 10);
    EXPECT_EQ(precise.positionIterations, 4);
    const PlacementTuning balanced = placementFeelTuning(PlacementFeel::Balanced);
    EXPECT_FLOAT_EQ(balanced.maxSpeed, 10.0f);
    EXPECT_FLOAT_EQ(balanced.contactSpeed, 1.0f);
    EXPECT_EQ(balanced.substeps, 1);
    EXPECT_EQ(balanced.velocityIterations, 8);
    EXPECT_EQ(balanced.positionIterations, 2);
    const PlacementTuning fast = placementFeelTuning(PlacementFeel::Fast);
    EXPECT_FLOAT_EQ(fast.maxSpeed, 25.0f);
    EXPECT_FLOAT_EQ(fast.contactSpeed, 3.0f);
    EXPECT_EQ(fast.substeps, 1);
    EXPECT_EQ(fast.velocityIterations, 4);
    EXPECT_EQ(fast.positionIterations, 1);
    for (const PlacementTuning& t : {precise, balanced, fast}) EXPECT_FLOAT_EQ(t.maxAngularSpeed, 10.0f);
}

TEST(PhysicsPlacement, AdvancedOverridesApplyOnlyWhenNotNegative) {
    PhysicsPlacementSettings s;
    s.feel = PlacementFeel::Precise;
    PlacementTuning t = resolvePlacementTuning(s);
    EXPECT_FLOAT_EQ(t.maxSpeed, 4.0f);
    EXPECT_EQ(t.substeps, 2);
    s.maxSpeed = 7.5f;
    s.contactSpeed = 0.0f;   // 0 is an override, not "from the preset"
    s.maxAngularSpeed = 3.0f;
    s.substeps = 3;
    s.velocityIterations = 12;
    s.positionIterations = 5;
    s.teleportDistance = 9.0f;
    s.sleepLinear = 0.05f;
    s.sleepAngularDeg = 4.0f;
    s.sleepSeconds = 0.5f;
    t = resolvePlacementTuning(s);
    EXPECT_FLOAT_EQ(t.maxSpeed, 7.5f);
    EXPECT_FLOAT_EQ(t.contactSpeed, 0.0f);
    EXPECT_FLOAT_EQ(t.maxAngularSpeed, 3.0f);
    EXPECT_EQ(t.substeps, 3);
    EXPECT_EQ(t.velocityIterations, 12);
    EXPECT_EQ(t.positionIterations, 5);
    EXPECT_FLOAT_EQ(t.teleportDistance, 9.0f);
    EXPECT_FLOAT_EQ(t.sleepLinear, 0.05f);
    EXPECT_FLOAT_EQ(t.sleepAngularDeg, 4.0f);
    EXPECT_FLOAT_EQ(t.sleepSeconds, 0.5f);
}

// ---- movers of a selection, the cache

TEST(PhysicsPlacement, RootsDropSelectedDescendantsKeepOrderAndSkipTheDead) {
    const uint32_t sceneId = newScene();
    const Entity parent = create(sceneId, "Table", EntityCreationType::BOX, Vector3(0.0f, 0.0f, 0.0f));
    const Entity child = create(sceneId, "Leg", EntityCreationType::BOX, Vector3(0.0f, -1.0f, 0.0f), parent);
    const Entity grandchild = create(sceneId, "Foot", EntityCreationType::BOX, Vector3(0.0f, -1.0f, 0.0f), child);
    const Entity other = create(sceneId, "Chair", EntityCreationType::BOX, Vector3(3.0f, 0.0f, 0.0f));
    const std::vector<Entity> roots = placementRoots(project(), sceneId, {grandchild, other, child, parent, other, 999999u});
    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], other);
    EXPECT_EQ(roots[1], parent);
    // a selected child without its parent moves on its own
    const std::vector<Entity> alone = placementRoots(project(), sceneId, {child});
    ASSERT_EQ(alone.size(), 1u);
    EXPECT_EQ(alone[0], child);
    EXPECT_TRUE(placementRoots(project(), 4242424242u, {parent}).empty());
    EXPECT_TRUE(placementRoots(nullptr, sceneId, {parent}).empty());
}

// ---- spec checks, fingerprint

TEST(PhysicsPlacement, SpecChecksGiveFixItMessages) {
    const uint32_t sceneId = newScene();
    const Entity box = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f));
    PlacementSpec ok;
    ok.sceneId = sceneId;
    ok.entities = {box};
    EXPECT_TRUE(checkPlacementSpec(project(), ok).empty());

    auto problems = [&](const PlacementSpec& spec) { return checkPlacementSpec(project(), spec); };
    EXPECT_TRUE(contains(checkPlacementSpec(nullptr, ok), "no project"));
    PlacementSpec s = ok;
    s.sceneId = 4242424242u;
    EXPECT_TRUE(contains(problems(s), "unknown scene"));
    s = ok;
    s.entities.clear();
    EXPECT_TRUE(contains(problems(s), "no entities"));
    s = ok;
    s.entities = {999999u};
    EXPECT_TRUE(contains(problems(s), "is not a GameObject"));
    s = ok;
    s.mode = PlacementMode::Move;
    EXPECT_TRUE(contains(problems(s), "mode move needs a target"));
    s.hasTargetPosition = true;
    s.hasTargetDelta = true;
    EXPECT_TRUE(contains(problems(s), "give one of them"));
    s.hasTargetDelta = false;
    s.targetPosition = Vector3(NAN, 0.0f, 0.0f);
    EXPECT_TRUE(contains(problems(s), "finite"));
    s = ok;
    s.mode = PlacementMode::Move;
    s.hasTargetRotation = true;
    s.targetRotation = Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_TRUE(contains(problems(s), "non-zero quaternion"));
    s = ok;
    s.mode = PlacementMode::Snap;
    EXPECT_TRUE(contains(problems(s), "mode snap needs a ray"));
    s.hasSnapRay = true;
    s.snapRay = Ray(Vector3(0.0f, 5.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(contains(problems(s), "non-zero vector"));
    s = ok;
    s.mode = PlacementMode::Distribute;
    s.axis = 3;
    EXPECT_TRUE(contains(problems(s), "axis must be x, y or z"));
    s = ok;
    s.mode = PlacementMode::Gather;
    s.amount = 1.5f;
    EXPECT_TRUE(contains(problems(s), "amount must be between 0 and 1"));
    s = ok;
    s.maxSeconds = 0.0f;
    EXPECT_TRUE(contains(problems(s), "max_seconds"));
    s = ok;
    s.contextMargin = -1.0f;
    EXPECT_TRUE(contains(problems(s), "context_margin"));
    s = ok;
    s.jitter.rotationDeg = 200.0f;
    EXPECT_TRUE(contains(problems(s), "jitter.rotation_deg"));
    s = ok;
    s.context = PlacementSpec::Context::Entities;
    EXPECT_TRUE(contains(problems(s), "context lists no entities"));
    s.contextEntities = {999999u};
    EXPECT_TRUE(contains(problems(s), "context entity 999999"));
    s = ok;
    s.gravity = Vector3(0.0f, INFINITY, 0.0f);
    EXPECT_TRUE(contains(problems(s), "gravity"));
    s = ok;
    s.settings.maxMoverSize = 0.0f;
    EXPECT_TRUE(contains(problems(s), "size limit"));
    s = ok;
    s.settings.substeps = 0;
    EXPECT_TRUE(contains(problems(s), "substeps"));
    s = ok;
    s.settings.maxSpeed = 0.0f;
    EXPECT_TRUE(contains(problems(s), "max speed"));

    SceneProject* sp = project()->getScene(sceneId);
    sp->playState = ScenePlayState::PLAYING;
    EXPECT_TRUE(contains(problems(ok), "Play mode"));
    sp->playState = ScenePlayState::STOPPED;
}

TEST(PhysicsPlacement, FingerprintIsStableAndCoversWhatChangesTheResult) {
    PlacementSpec a;
    a.sceneId = 7;
    a.entities = {3, 1, 2};
    PlacementSpec b = a;
    b.entities = {2, 3, 1, 1};   // order and repeats do not change a run (movers sorted by path)
    EXPECT_EQ(a.fingerprint(), b.fingerprint());
    EXPECT_EQ(a.fingerprint().size(), 16u);
    EXPECT_EQ(a.fingerprint(), PlacementSpec(a).fingerprint());

    // the Scene-view gizmos and interactive-only settings do not either
    b.settings.showProxies = true;
    b.settings.showDistanceIndicators = true;
    b.settings.settleOnRelease = false;
    b.settings.maxSettleSeconds = 1.0f;
    EXPECT_EQ(a.fingerprint(), b.fingerprint());

    std::vector<PlacementSpec> changed(14, a);
    changed[0].seed = 2;
    changed[1].mode = PlacementMode::Separate;
    changed[2].gravity = Vector3(0.0f, -9.0f, 0.0f);
    changed[3].jitter.position = 0.1f;
    changed[4].settings.feel = PlacementFeel::Fast;
    changed[5].settings.colliders = PlacementColliderPolicy::GENERATED;
    changed[6].maxSeconds = 5.0f;
    changed[7].entities = {1, 2};
    changed[8].hasTargetDelta = true;
    changed[9].settings.maxSpeed = 3.0f;
    changed[10].context = PlacementSpec::Context::None;
    changed[11].settings.pushNeighbours = true;
    changed[12].settle = false;
    changed[13].sceneId = 8;
    for (size_t i = 0; i < changed.size(); i++) EXPECT_NE(changed[i].fingerprint(), a.fingerprint()) << i;
}

// ---- ObjectTransformCmd: many entities, explicit old values

TEST(PhysicsPlacement, TransformCommandTakesManyEntitiesAndExplicitOldValues) {
    const uint32_t sceneId = newScene();
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(0.0f, 0.0f, 0.0f));
    const Entity b = create(sceneId, "B", EntityCreationType::BOX, Vector3(5.0f, 0.0f, 0.0f));
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t position = history->position();

    // a preview already moved A: the explicit old value is what undo puts back
    transformOf(sceneId, a).position = Vector3(9.0f, 9.0f, 9.0f);
    ObjectTransformCmd* cmd = new ObjectTransformCmd(project(), sceneId);
    cmd->addEntity(a, Vector3(1.0f, 0.0f, 0.0f), yaw(90.0f), Vector3(1.0f, 1.0f, 1.0f), Vector3(0.0f, 0.0f, 0.0f), Quaternion(),
                   Vector3(1.0f, 1.0f, 1.0f));
    cmd->addEntity(b, Vector3(6.0f, 0.0f, 0.0f), Quaternion(), Vector3(2.0f, 2.0f, 2.0f));
    EXPECT_EQ(cmd->getEntityCount(), 2u);
    history->addCommandNoMerge(cmd);
    EXPECT_EQ(history->position(), position + 1);
    expectNear(transformOf(sceneId, a).position, Vector3(1.0f, 0.0f, 0.0f), 1e-6f);
    expectSameRotation(transformOf(sceneId, a).rotation, yaw(90.0f), 1e-6f);
    expectNear(transformOf(sceneId, b).scale, Vector3(2.0f, 2.0f, 2.0f), 1e-6f);
    EXPECT_TRUE(project()->getScene(sceneId)->isModified);

    history->undo();
    expectNear(transformOf(sceneId, a).position, Vector3(0.0f, 0.0f, 0.0f), 1e-6f);
    expectNear(transformOf(sceneId, b).position, Vector3(5.0f, 0.0f, 0.0f), 1e-6f);
    expectNear(transformOf(sceneId, b).scale, Vector3(1.0f, 1.0f, 1.0f), 1e-6f);
    history->redo();
    expectNear(transformOf(sceneId, a).position, Vector3(1.0f, 0.0f, 0.0f), 1e-6f);
    expectNear(transformOf(sceneId, b).position, Vector3(6.0f, 0.0f, 0.0f), 1e-6f);

    // the one-entity constructor still reads the old value at execute
    transformOf(sceneId, a).position = Vector3(3.0f, 3.0f, 3.0f);
    history->addCommandNoMerge(new ObjectTransformCmd(project(), sceneId, a, Vector3(4.0f, 0.0f, 0.0f), Quaternion(), Vector3(1.0f, 1.0f, 1.0f)));
    history->undo();
    expectNear(transformOf(sceneId, a).position, Vector3(3.0f, 3.0f, 3.0f), 1e-6f);
}

// ---- commit a result, JSON

namespace {

    PlacementResult handMadeResult(uint32_t sceneId, Entity root, Entity child) {
        PlacementResult result;
        result.ok = true;
        result.mode = PlacementMode::Drop;
        result.sceneId = sceneId;
        result.sceneStructureVersion = project()->getScene(sceneId)->structureVersion;
        result.seed = 3;
        result.stateHash = "00000000deadbeef";
        result.simSeconds = 2.3456789;
        result.steps = 282;
        result.allSettled = true;
        PlacementEntityResult a;
        a.entity = root;
        a.name = "Crate";
        a.path = "Crate";
        a.before.position = Vector3(0.0f, 3.0f, 0.0f);
        a.after.position = Vector3(5.0f, 1.0f, 0.0f);
        a.after.rotation = yaw(30.0f);
        a.afterLocalPosition = Vector3(5.0f, 1.0f, 0.0f);
        a.afterLocalRotation = yaw(30.0f);
        a.movedMeters = 5.3851648f;
        a.rotatedDegrees = 30.0000004f;
        a.settled = true;
        a.settleStep = 270;
        a.restingOn = {child};
        a.restingOnPaths = {"Shelf/Board"};
        a.maxPenetration = 0.00081234f;
        a.collider = "decomposed(6)";
        PlacementEntityResult b;
        b.entity = child;
        b.name = "Board";
        b.path = "Shelf/Board";
        b.mover = true;
        b.afterLocalPosition = Vector3(0.5f, -0.0000001f, 0.0f);
        b.afterLocalRotation = Quaternion();
        b.settled = true;
        b.settleStep = 12;
        b.collider = "box";
        b.warnings = {"teleported: its target was more than 5 m away"};
        result.entities = {a, b};
        result.contextBodies = 41;
        result.contextRegion = AABB(Vector3(-2.0f, -1.0f, -2.0f), Vector3(2.0f, 4.123456789f, 2.0f));
        result.warnings = {"Pebble#3 never came to rest within 10 s"};
        // what a run records at its start
        for (PlacementEntityResult& e : result.entities) {
            if (Transform* t = sceneOf(sceneId)->findComponent<Transform>(e.entity)) {
                e.beforeLocalPosition = t->position;
                e.beforeLocalRotation = t->rotation;
                e.beforeLocalScale = t->scale;
                e.beforeParent = t->parent;
            }
        }
        result.sceneEditStamp = placementSceneEditStamp(project(), sceneId);
        return result;
    }

}

TEST(PhysicsPlacement, CommitIsOneUndoStepInParentSpace) {
    const uint32_t sceneId = newScene();
    const Entity shelf = create(sceneId, "Shelf", EntityCreationType::BOX, Vector3(1.0f, 0.0f, 0.0f), NULL_ENTITY, Vector3(2.0f, 2.0f, 2.0f), yaw(90.0f));
    const Entity board = create(sceneId, "Board", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f), shelf);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    project()->getScene(sceneId)->isModified = false;
    const Vector3 boardBefore = transformOf(sceneId, board).position;
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t position = history->position();

    const PlacementResult result = handMadeResult(sceneId, crate, board);
    std::string error;
    ASSERT_TRUE(commitPlacement(project(), result, error)) << error;
    EXPECT_EQ(history->position(), position + 1);
    expectNear(transformOf(sceneId, crate).position, Vector3(5.0f, 1.0f, 0.0f), 1e-6f);
    expectSameRotation(transformOf(sceneId, crate).rotation, yaw(30.0f), 1e-6f);
    expectNear(transformOf(sceneId, board).position, Vector3(0.5f, -0.0000001f, 0.0f), 1e-6f);
    expectNear(transformOf(sceneId, board).scale, Vector3(1.0f, 1.0f, 1.0f), 1e-6f);   // scale kept
    EXPECT_TRUE(project()->getScene(sceneId)->isModified);

    history->undo();
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 1e-6f);
    expectNear(transformOf(sceneId, board).position, boardBefore, 1e-6f);
    EXPECT_FALSE(project()->getScene(sceneId)->isModified);
    history->redo();
    expectNear(transformOf(sceneId, crate).position, Vector3(5.0f, 1.0f, 0.0f), 1e-6f);

    // the same result again: the scene is not what it started from any more
    EXPECT_FALSE(commitPlacement(project(), result, error));
    EXPECT_EQ(error, "the scene changed during the simulation: run it again");
    // committing what is already there adds no step
    ASSERT_TRUE(commitPlacement(project(), handMadeResult(sceneId, crate, board), error)) << error;
    EXPECT_EQ(history->position(), position + 1);
}

TEST(PhysicsPlacement, CommitRefusesAChangedSceneAGoneEntityAndAFailedRun) {
    const uint32_t sceneId = newScene();
    const Entity board = create(sceneId, "Board", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f));
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();
    std::string error;

    PlacementResult failed = handMadeResult(sceneId, crate, board);
    failed.ok = false;
    failed.error = "cancelled";
    EXPECT_FALSE(commitPlacement(project(), failed, error));
    EXPECT_NE(error.find("did not finish"), std::string::npos);

    PlacementResult stale = handMadeResult(sceneId, crate, board);
    project()->getScene(sceneId)->structureVersion++;
    EXPECT_FALSE(commitPlacement(project(), stale, error));
    EXPECT_EQ(error, "the scene changed during the simulation: run it again");
    project()->getScene(sceneId)->structureVersion--;

    CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, board));
    const size_t afterDelete = CommandHandle::get(sceneId)->position();
    PlacementResult gone = handMadeResult(sceneId, crate, board);
    EXPECT_FALSE(commitPlacement(project(), gone, error));
    EXPECT_NE(error.find("Shelf/Board is gone"), std::string::npos);
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), afterDelete);
    EXPECT_EQ(afterDelete, position + 1);
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 1e-6f);   // nothing half-applied

    PlacementResult playing = handMadeResult(sceneId, crate, board);
    playing.entities.pop_back();
    project()->getScene(sceneId)->playState = ScenePlayState::PLAYING;
    EXPECT_FALSE(commitPlacement(project(), playing, error));
    EXPECT_NE(error.find("Play mode"), std::string::npos);
    project()->getScene(sceneId)->playState = ScenePlayState::STOPPED;

    PlacementResult noScene = playing;
    noScene.sceneId = 4242424242u;
    EXPECT_FALSE(commitPlacement(project(), noScene, error));
    EXPECT_FALSE(commitPlacement(nullptr, playing, error));
}

TEST(PhysicsPlacement, CommitRefusesAnyEditSinceTheRunStarted) {
    const uint32_t sceneId = newScene();
    const Entity board = create(sceneId, "Board", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f));
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    std::string error;
    // a Transform edited since the run started (no structure change): refused, nothing applied
    PlacementResult moved = handMadeResult(sceneId, crate, board);
    transformOf(sceneId, crate).position = Vector3(0.0f, 3.5f, 0.0f);
    EXPECT_FALSE(commitPlacement(project(), moved, error));
    EXPECT_EQ(error, "the scene changed during the simulation: run it again");
    transformOf(sceneId, crate).position = Vector3(0.0f, 3.0f, 0.0f);
    EXPECT_TRUE(commitPlacement(project(), moved, error)) << error;   // as it was: fine again
    CommandHandle::get(sceneId)->undo();

    // another GameObject moved (the floor the result rests on): the stamp catches it
    const Entity floor = create(sceneId, "Floor", EntityCreationType::BOX, Vector3(0.0f, -0.5f, 0.0f));
    PlacementResult floorMoved = handMadeResult(sceneId, crate, board);
    const uint64_t stamp = placementSceneEditStamp(project(), sceneId);
    EXPECT_NE(stamp, 0u);
    EXPECT_EQ(placementSceneEditStamp(project(), sceneId), stamp);
    CommandHandle::get(sceneId)->addCommandNoMerge(new ObjectTransformCmd(project(), sceneId, floor, Vector3(0.0f, -0.4f, 0.0f), Quaternion(), Vector3(1.0f, 1.0f, 1.0f)));
    EXPECT_NE(placementSceneEditStamp(project(), sceneId), stamp);
    EXPECT_FALSE(commitPlacement(project(), floorMoved, error));
    EXPECT_EQ(error, "the scene changed during the simulation: run it again");
    EXPECT_EQ(placementSceneEditStamp(project(), 4242424242u), 0u);

    // a parent changed: refused
    PlacementResult reparented = handMadeResult(sceneId, crate, board);
    reparented.entities[0].beforeParent = floor;
    EXPECT_FALSE(commitPlacement(project(), reparented, error));
    EXPECT_EQ(error, "the scene changed during the simulation: run it again");
}

TEST(PhysicsPlacement, ResultJsonHasTheSpecShapeRoundedTo1e5) {
    const uint32_t sceneId = newScene();
    const Entity shelf = create(sceneId, "Shelf", EntityCreationType::BOX, Vector3(0.0f, 0.0f, 0.0f));
    const Entity board = create(sceneId, "Board", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f), shelf);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const Entity room = create(sceneId, "Room", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 0.0f, 0.0f));
    PlacementResult result = handMadeResult(sceneId, crate, board);
    result.refused = {{room, "too large (> 25 m), treated as environment"}};

    const json j = placementResultJson(project(), result, false);
    EXPECT_EQ(j["applied"], false);
    EXPECT_EQ(j["mode"], "drop");
    EXPECT_EQ(j["seed"], 3);
    EXPECT_EQ(j["state_hash"], "00000000deadbeef");
    EXPECT_FLOAT_EQ(j["sim_seconds"].get<float>(), 2.34568f);
    EXPECT_EQ(j["steps"], 282);
    EXPECT_EQ(j["all_settled"], true);

    const json& e = j["entities"][0];
    EXPECT_EQ(e["id"], crate);
    EXPECT_EQ(e["name"], "Crate");
    EXPECT_EQ(e["path"], "Crate");
    EXPECT_EQ(e["before"]["position"], json::array({0.0, 3.0, 0.0}));
    EXPECT_EQ(e["before"]["rotation"], json::array({0.0, 0.0, 0.0, 1.0}));
    EXPECT_TRUE(e["after"].contains("local"));
    EXPECT_EQ(e["after"]["local"]["position"], json::array({5.0, 1.0, 0.0}));
    EXPECT_EQ(e["after"]["rotation"].size(), 4u);
    EXPECT_FLOAT_EQ(e["moved_m"].get<float>(), 5.38516f);
    EXPECT_FLOAT_EQ(e["rotated_deg"].get<float>(), 30.0f);
    EXPECT_EQ(e["settled"], true);
    EXPECT_EQ(e["settle_step"], 270);
    EXPECT_EQ(e["resting_on"], json::array({json{{"id", board}, {"path", "Shelf/Board"}}}));
    EXPECT_FLOAT_EQ(e["max_penetration_m"].get<float>(), 0.00081f);
    EXPECT_EQ(e["collider"], "decomposed(6)");
    EXPECT_TRUE(e["warnings"].is_array() && e["warnings"].empty());
    // -1e-7 rounds to 0, not -0
    const json& local = j["entities"][1]["after"]["local"]["position"];
    EXPECT_EQ(local[1].dump(), "0.0");
    EXPECT_EQ(j["entities"][1]["warnings"].size(), 1u);

    EXPECT_EQ(j["context"]["bodies"], 41);
    EXPECT_FLOAT_EQ(j["context"]["region"]["max"][1].get<float>(), 4.12346f);
    EXPECT_EQ(j["refused"], json::array({json{{"path", "Room"}, {"reason", "too large (> 25 m), treated as environment"}}}));
    EXPECT_EQ(j["warnings"], json::array({"Pebble#3 never came to rest within 10 s"}));

    const json applied = placementResultJson(project(), result, true);
    EXPECT_EQ(applied["applied"], true);

    PlacementResult empty;
    empty.ok = false;
    empty.error = "nothing can move";
    const json failed = placementResultJson(project(), empty, false);
    EXPECT_EQ(failed["error"], "nothing can move");
    EXPECT_TRUE(failed["context"]["region"].is_null());
}

// ---- batch run: what needs no simulation

TEST(PhysicsPlacement, RunRefusesABadSpecAndWhatCannotMove) {
    const uint32_t sceneId = newScene();
    const Entity empty = create(sceneId, "Marker", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 1.0f, 0.0f));

    PlacementRun notStarted;
    EXPECT_FALSE(notStarted.isDone());
    EXPECT_EQ(notStarted.progressText(), "not started");
    EXPECT_FLOAT_EQ(notStarted.progress(), 0.0f);

    PlacementSpec bad;
    bad.sceneId = sceneId;
    bad.mode = PlacementMode::Move;
    PlacementRun run;
    EXPECT_FALSE(run.start(project(), bad));
    EXPECT_TRUE(run.isDone());
    EXPECT_FALSE(run.result().ok);
    EXPECT_NE(run.result().error.find("no entities"), std::string::npos);
    EXPECT_NE(run.result().error.find("mode move needs a target"), std::string::npos);
    EXPECT_TRUE(run.advance(0.0));
    EXPECT_FLOAT_EQ(run.progress(), 1.0f);
    EXPECT_EQ(run.progressText(), "failed");

    PlacementSpec meshless;
    meshless.sceneId = sceneId;
    meshless.entities = {empty};
    PlacementRun refused;
    EXPECT_FALSE(refused.start(project(), meshless));
    EXPECT_NE(refused.result().error.find("nothing can move (Marker: "), std::string::npos) << refused.result().error;
    ASSERT_EQ(refused.result().refused.size(), 1u);
    EXPECT_EQ(refused.result().refused[0].entity, empty);
    EXPECT_FALSE(refused.result().refused[0].reason.empty());
    EXPECT_EQ(refused.result().specFingerprint, meshless.fingerprint());
    EXPECT_EQ(refused.result().sceneId, sceneId);
    std::string error;
    EXPECT_FALSE(commitPlacement(project(), refused.result(), error));

    PlacementRun cancelled;
    cancelled.cancel();
    EXPECT_TRUE(cancelled.isDone());
    EXPECT_TRUE(cancelled.result().cancelled);
}

// ---- interactive session: what needs no simulation

TEST(PhysicsPlacement, SessionIdleRefusalsCenterAndNoOps) {
    const uint32_t sceneId = newScene();
    const Entity parent = create(sceneId, "Shelf", EntityCreationType::GAMEOBJECT, Vector3(1.0f, 0.0f, 0.0f), NULL_ENTITY,
                                 Vector3(2.0f, 2.0f, 2.0f), yaw(90.0f));
    const Entity child = create(sceneId, "Marker", EntityCreationType::GAMEOBJECT, Vector3(1.0f, 0.0f, 0.0f), parent);
    const Entity other = create(sceneId, "Pin", EntityCreationType::GAMEOBJECT, Vector3(5.0f, 2.0f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    session.setSelection({child, other});
    const PlacementHud hud = session.getHud();
    ASSERT_EQ(hud.refused.size(), 2u);
    EXPECT_EQ(hud.refused[0].entity, child);
    EXPECT_EQ(hud.refused[1].entity, other);
    EXPECT_FALSE(hud.refused[0].reason.empty());
    EXPECT_FALSE(hud.refused[1].reason.empty());

    // The gizmo centre uses world positions, including the selected child's parent.
    const Vector3 childWorld = Vector3(1.0f, 0.0f, 0.0f) + yaw(90.0f) * Vector3(2.0f, 0.0f, 0.0f);
    expectNear(session.getMoversCenter(), (childWorld + Vector3(5.0f, 2.0f, 0.0f)) / 2.0f, 1e-5f);
    EXPECT_FALSE(session.begin(PlacementGesture::Move, {child, other}));
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position);
}

// ---- with the simulation (requires a build with 3D physics)

namespace {

    // A 20 x 20 m floor whose top is y = 0
    Entity makeFloor(uint32_t sceneId) {
        return create(sceneId, "Floor", EntityCreationType::BOX, Vector3(0.0f, -0.5f, 0.0f), NULL_ENTITY, Vector3(20.0f, 1.0f, 20.0f));
    }

    PlacementSpec specFor(uint32_t sceneId, const std::vector<Entity>& entities, PlacementMode mode = PlacementMode::Drop) {
        PlacementSpec spec;
        spec.sceneId = sceneId;
        spec.entities = entities;
        spec.mode = mode;
        return spec;
    }

    PlacementResult runToEnd(const PlacementSpec& spec) {
        PlacementRun run;
        EXPECT_TRUE(run.start(project(), spec)) << run.result().error;
        EXPECT_TRUE(run.advance(0.0));
        EXPECT_TRUE(run.isDone());
        return run.result();
    }

    const PlacementEntityResult* entityResult(const PlacementResult& result, Entity entity) {
        for (const PlacementEntityResult& e : result.entities) {
            if (e.entity == entity) return &e;
        }
        return nullptr;
    }

    void ticks(PlacementSession& session, int count) {
        for (int i = 0; i < count; i++) session.tick(1.0 / 60.0);
    }

    // ticks until the gesture is over (settled and committed); false if it never is
    bool tickUntilIdle(PlacementSession& session, int maxTicks = 1200) {
        for (int i = 0; i < maxTicks && session.isBusy(); i++) session.tick(1.0 / 60.0);
        return !session.isBusy();
    }

    PhysicsPlacementSettings noSettle() {
        PhysicsPlacementSettings settings;
        settings.settleOnRelease = false;
        return settings;
    }

}

TEST(PhysicsPlacementSim, DropRestsOnTheFloorPreviewsNothingAndCommitsOneStep) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity floor = makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();

    const PlacementResult result = runToEnd(specFor(sceneId, {crate}));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.allSettled);
    EXPECT_GT(result.steps, 0u);
    EXPECT_NEAR(result.simSeconds, result.steps / 120.0, 1e-6);
    EXPECT_EQ(result.stateHash.size(), 16u);
    EXPECT_GT(result.contextBodies, 0u);
    const PlacementEntityResult* e = entityResult(result, crate);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->path, "Crate");
    EXPECT_TRUE(e->mover);
    expectNear(e->before.position, Vector3(0.0f, 3.0f, 0.0f), 1e-5f);
    EXPECT_NEAR(e->after.position.y, 0.5f, 0.002f);
    EXPECT_NEAR(e->afterLocalPosition.y, 0.5f, 0.002f);
    EXPECT_NEAR(e->movedMeters, 2.5f, 0.01f);
    EXPECT_TRUE(e->settled);
    EXPECT_GT(e->settleStep, 0u);
    EXPECT_FALSE(e->fellOut);
    EXPECT_FALSE(e->collider.empty());
    ASSERT_EQ(e->restingOn.size(), 1u);
    EXPECT_EQ(e->restingOn[0], floor);
    EXPECT_EQ(e->restingOnPaths[0], "Floor");
    EXPECT_TRUE(result.warnings.empty()) << result.warnings[0];

    // a preview never touches the scene
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 0.0f);
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position);

    std::string error;
    ASSERT_TRUE(commitPlacement(project(), result, error)) << error;
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 0.5f, 0.002f);
    CommandHandle::get(sceneId)->undo();
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 0.0f);
}

TEST(PhysicsPlacementSim, SameArgumentsSameHashWhateverTheSlices) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    std::vector<Entity> crates;
    for (int i = 0; i < 5; i++) {
        crates.push_back(create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.3f * i, 1.5f + 1.2f * i, 0.1f * i), NULL_ENTITY,
                                Vector3(1.0f, 1.0f, 1.0f), yaw(17.0f * i)));
    }
    PlacementSpec spec = specFor(sceneId, crates);
    spec.jitter.position = 0.2f;
    spec.jitter.rotationDeg = 25.0f;
    spec.seed = 7;
    const PlacementResult straight = runToEnd(spec);

    PlacementRun sliced;
    ASSERT_TRUE(sliced.start(project(), spec));
    while (!sliced.advance(0.3)) {
        EXPECT_LT(sliced.progress(), 1.0f);
    }
    EXPECT_EQ(sliced.result().stateHash, straight.stateHash);
    ASSERT_EQ(sliced.result().entities.size(), straight.entities.size());
    for (size_t i = 0; i < straight.entities.size(); i++) {
        EXPECT_EQ(sliced.result().entities[i].entity, straight.entities[i].entity);
        expectNear(sliced.result().entities[i].after.position, straight.entities[i].after.position, 0.0f);
    }
    // the same crates given in another order: same movers (path order), same result
    PlacementSpec reversed = spec;
    std::reverse(reversed.entities.begin(), reversed.entities.end());
    EXPECT_EQ(runToEnd(reversed).stateHash, straight.stateHash);

    PlacementSpec otherSeed = spec;
    otherSeed.seed = 8;
    EXPECT_NE(runToEnd(otherSeed).stateHash, straight.stateHash);
}

TEST(PhysicsPlacementSim, SeparatePushesOverlapsApartWithoutGravity) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const Entity b = create(sceneId, "B", EntityCreationType::BOX, Vector3(0.5f, 3.0f, 0.0f));
    const PlacementResult result = runToEnd(specFor(sceneId, {a, b}, PlacementMode::Separate));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.allSettled);
    const PlacementEntityResult* ra = entityResult(result, a);
    const PlacementEntityResult* rb = entityResult(result, b);
    ASSERT_TRUE(ra && rb);
    EXPECT_GE(std::fabs(rb->after.position.x - ra->after.position.x), 1.0f - 0.0015f);
    EXPECT_NEAR(ra->after.position.y, 3.0f, 0.05f);

    // dropping them as they are says to separate first
    const PlacementResult drop = runToEnd(specFor(sceneId, {a, b}));
    EXPECT_TRUE(contains(drop.warnings, "use mode:separate first"));
}

TEST(PhysicsPlacementSim, MoveTargetsTheGroupAndStopsAtContacts) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 0.0f));
    create(sceneId, "Wall", EntityCreationType::BOX, Vector3(3.0f, 2.5f, 0.0f), NULL_ENTITY, Vector3(1.0f, 5.0f, 5.0f));

    PlacementSpec free = specFor(sceneId, {crate}, PlacementMode::Move);
    free.hasTargetDelta = true;
    free.targetDelta = Vector3(-2.0f, 0.0f, 0.0f);
    free.settle = false;
    const PlacementResult moved = runToEnd(free);
    ASSERT_TRUE(moved.ok) << moved.error;
    EXPECT_NEAR(entityResult(moved, crate)->after.position.x, -2.0f, 0.01f);

    PlacementSpec blocked = free;
    blocked.targetDelta = Vector3(5.0f, 0.0f, 0.0f);
    blocked.settle = true;
    const PlacementResult stopped = runToEnd(blocked);
    ASSERT_TRUE(stopped.ok) << stopped.error;
    EXPECT_LE(entityResult(stopped, crate)->after.position.x, 2.0f + 0.01f);   // the wall's face is at x = 2.5
    EXPECT_GE(entityResult(stopped, crate)->after.position.x, 1.5f);

    PlacementSpec rotated = free;
    rotated.hasTargetDelta = false;
    rotated.hasTargetRotation = true;
    rotated.targetRotation = yaw(90.0f);
    const PlacementResult turned = runToEnd(rotated);
    ASSERT_TRUE(turned.ok) << turned.error;
    EXPECT_NEAR(entityResult(turned, crate)->rotatedDegrees, 90.0f, 0.5f);
}

TEST(PhysicsPlacementSim, DistributeAndGatherAlongTheirAxes) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 0.0f));
    const Entity b = create(sceneId, "B", EntityCreationType::BOX, Vector3(1.2f, 0.5f, 0.0f));
    const Entity c = create(sceneId, "C", EntityCreationType::BOX, Vector3(6.0f, 0.5f, 0.0f));

    PlacementSpec distribute = specFor(sceneId, {c, a, b}, PlacementMode::Distribute);
    distribute.axis = 0;
    const PlacementResult spaced = runToEnd(distribute);
    ASSERT_TRUE(spaced.ok) << spaced.error;
    EXPECT_NEAR(entityResult(spaced, a)->after.position.x, 0.0f, 0.02f);
    EXPECT_NEAR(entityResult(spaced, b)->after.position.x, 3.0f, 0.02f);
    EXPECT_NEAR(entityResult(spaced, c)->after.position.x, 6.0f, 0.02f);

    // a free row behind them: half way to the centre
    const Entity d = create(sceneId, "D", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 4.0f));
    const Entity f = create(sceneId, "F", EntityCreationType::BOX, Vector3(6.0f, 0.5f, 4.0f));
    PlacementSpec gather = specFor(sceneId, {d, f}, PlacementMode::Gather);
    gather.amount = 0.5f;
    gather.settle = false;
    const PlacementResult gathered = runToEnd(gather);
    ASSERT_TRUE(gathered.ok) << gathered.error;
    EXPECT_NEAR(entityResult(gathered, d)->after.position.x, 1.5f, 0.02f);
    EXPECT_NEAR(entityResult(gathered, f)->after.position.x, 4.5f, 0.02f);
    EXPECT_NEAR(entityResult(gathered, d)->after.position.y, 0.5f, 0.01f);   // across gravity only
    EXPECT_TRUE(gathered.allSettled);

    // B stands between A and C: gathered all the way, A stops against it
    PlacementSpec blocked = specFor(sceneId, {a, c}, PlacementMode::Gather);
    blocked.settle = false;
    const PlacementResult stopped = runToEnd(blocked);
    ASSERT_TRUE(stopped.ok) << stopped.error;
    EXPECT_NEAR(entityResult(stopped, a)->after.position.x, 0.2f, 0.01f);
}

TEST(PhysicsPlacementSim, SnapRestsOnTheSurfaceTheRayHits) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 5.0f, 0.0f));
    PlacementSpec snap = specFor(sceneId, {crate}, PlacementMode::Snap);
    snap.hasSnapRay = true;
    snap.snapRay = Ray(Vector3(3.0f, 10.0f, 1.0f), Vector3(0.0f, -1.0f, 0.0f));
    const PlacementResult result = runToEnd(snap);
    ASSERT_TRUE(result.ok) << result.error;
    const PlacementEntityResult* e = entityResult(result, crate);
    EXPECT_NEAR(e->after.position.x, 3.0f, 0.05f);
    EXPECT_NEAR(e->after.position.z, 1.0f, 0.05f);
    EXPECT_NEAR(e->after.position.y, 0.5f, 0.01f);

    PlacementSpec miss = snap;
    miss.snapRay = Ray(Vector3(3.0f, 10.0f, 1.0f), Vector3(0.0f, 1.0f, 0.0f));
    PlacementRun run;
    EXPECT_FALSE(run.start(project(), miss));
    EXPECT_NE(run.result().error.find("hits nothing"), std::string::npos);
}

TEST(PhysicsPlacementSim, WarningsSayWhatToDo) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    PlacementSpec nothingBelow = specFor(sceneId, {crate});
    const PlacementResult fell = runToEnd(nothingBelow);
    ASSERT_TRUE(fell.ok) << fell.error;
    EXPECT_TRUE(contains(fell.warnings, "nothing to land on under Crate: it falls out of the world"));
    EXPECT_TRUE(contains(fell.warnings, "fell out of the world"));
    EXPECT_TRUE(entityResult(fell, crate)->fellOut);
    EXPECT_FALSE(fell.allSettled);

    makeFloor(sceneId);
    PlacementSpec hurried = specFor(sceneId, {crate});
    hurried.maxSeconds = 0.1f;
    const PlacementResult quick = runToEnd(hurried);
    EXPECT_TRUE(contains(quick.warnings, "Crate never came to rest within 0.1 s"));
    EXPECT_FALSE(quick.allSettled);
}

TEST(PhysicsPlacementSim, InteractiveDropPreviewsLiveAndCommitsOneStep) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();

    PlacementSession session(project(), sceneId);
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {crate}));
    EXPECT_TRUE(session.isGestureActive());
    EXPECT_EQ(session.getHud().movers, 1u);
    ticks(session, 20);
    EXPECT_LT(transformOf(sceneId, crate).position.y, 2.9f);   // previewed in the Transform
    EXPECT_EQ(session.getHud().status, "Simulating");
    PlacementOverlay overlay;
    session.collectOverlay(overlay);
    EXPECT_TRUE(overlay.active);
    ASSERT_EQ(overlay.ghosts.size(), 1u);
    expectNear(overlay.ghosts[0].position, Vector3(0.0f, 3.0f, 0.0f), 1e-5f);
    ticks(session, 240);
    EXPECT_EQ(session.getHud().status, "Resting");
    session.end();   // Drop commits on release
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 0.5f, 0.002f);
    CommandHandle::get(sceneId)->undo();
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 0.0f);
}

TEST(PhysicsPlacementSim, InteractiveCancelRestoresExactlyWithoutAStep) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.25f, 3.0f, -1.0f), NULL_ENTITY,
                                Vector3(1.0f, 1.0f, 1.0f), yaw(10.0f));
    const Transform before = transformOf(sceneId, crate);
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    session.setSettings(noSettle());
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(1.0f, 0.0f, 0.0f));
    ticks(session, 30);
    EXPECT_GT(transformOf(sceneId, crate).position.x, 0.5f);
    session.cancel();
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(transformOf(sceneId, crate).position, before.position);
    EXPECT_EQ(transformOf(sceneId, crate).rotation, before.rotation);
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position);

    // the destructor cancels a gesture still running
    {
        PlacementSession scoped(project(), sceneId);
        ASSERT_TRUE(scoped.begin(PlacementGesture::Move, {crate}));
        scoped.setMoveTarget(scoped.getMoversCenter() + Vector3(0.0f, 1.0f, 0.0f));
        ticks(scoped, 30);
        EXPECT_GT(transformOf(sceneId, crate).position.y, 3.5f);
    }
    EXPECT_EQ(transformOf(sceneId, crate).position, before.position);
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position);
}

TEST(PhysicsPlacementSim, InteractiveMoveSettlesOnReleaseThenCommits) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    // lifted and carried: it falls back down once released
    session.setMoveTarget(session.getMoversCenter() + Vector3(2.0f, 1.0f, 0.0f));
    ticks(session, 120);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 1.5f, 0.02f);
    session.end();
    EXPECT_EQ(session.getPhase(), PlacementPhase::Settling);
    EXPECT_EQ(session.getHud().status, "Settling\xE2\x80\xA6");
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position);   // the commit waits for the settle
    ASSERT_TRUE(tickUntilIdle(session));
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);
    EXPECT_NEAR(transformOf(sceneId, crate).position.x, 2.0f, 0.05f);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 0.5f, 0.005f);

    // a gesture begun while another settles ends that one first (committed)
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(0.0f, 1.0f, 0.0f));
    ticks(session, 60);
    session.end();
    ASSERT_TRUE(session.isBusy());
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {crate}));
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 2);
    session.cancel();
}

TEST(PhysicsPlacementSim, InteractiveRotateAndGatherSpreadFollowThePivot) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(-1.5f, 3.0f, 0.0f));
    const Entity b = create(sceneId, "B", EntityCreationType::BOX, Vector3(1.5f, 3.0f, 0.0f));
    PlacementSession session(project(), sceneId);
    session.setSettings(noSettle());

    ASSERT_TRUE(session.begin(PlacementGesture::Rotate, {a, b}));
    session.setRotateDelta(yaw(90.0f));
    ticks(session, 240);
    session.end();
    const Vector3 expectedA = yaw(90.0f) * Vector3(-1.5f, 0.0f, 0.0f) + Vector3(0.0f, 3.0f, 0.0f);
    expectNear(transformOf(sceneId, a).position, expectedA, 0.01f);
    expectNear(transformOf(sceneId, b).position, Vector3(0.0f, 6.0f, 0.0f) - expectedA, 0.01f);
    EXPECT_NEAR((transformOf(sceneId, a).position - transformOf(sceneId, b).position).length(), 3.0f, 0.01f);
    expectSameRotation(transformOf(sceneId, a).rotation, yaw(90.0f), 1e-3f);

    PhysicsPlacementSettings each = noSettle();
    each.rotatePivot = PlacementRotatePivot::EachObject;
    session.setSettings(each);
    const Vector3 aBefore = transformOf(sceneId, a).position;
    ASSERT_TRUE(session.begin(PlacementGesture::Rotate, {a, b}));
    session.setRotateDelta(yaw(-90.0f));
    ticks(session, 240);
    session.end();
    expectNear(transformOf(sceneId, a).position, aBefore, 0.01f);
    expectSameRotation(transformOf(sceneId, a).rotation, Quaternion(), 1e-3f);

    ASSERT_TRUE(session.begin(PlacementGesture::GatherSpread, {a, b}));
    session.setGatherScale(Vector3(1.0f, 1.0f, 3.0f));   // clamped to 2
    ticks(session, 240);
    session.end();
    EXPECT_NEAR((transformOf(sceneId, a).position - transformOf(sceneId, b).position).length(), 6.0f, 0.02f);
}

TEST(PhysicsPlacementSim, InteractiveDistributeButtonSettlesAndCommitsOneStep) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 0.0f));
    const Entity b = create(sceneId, "B", EntityCreationType::BOX, Vector3(1.2f, 0.5f, 0.0f));
    const Entity c = create(sceneId, "C", EntityCreationType::BOX, Vector3(6.0f, 0.5f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    ASSERT_TRUE(session.distribute({a, b, c}, 0));
    EXPECT_EQ(session.getPhase(), PlacementPhase::Settling);
    ASSERT_TRUE(tickUntilIdle(session));
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);
    EXPECT_NEAR(transformOf(sceneId, b).position.x, 3.0f, 0.05f);

    ASSERT_TRUE(session.gather({a, c}));
    ASSERT_TRUE(tickUntilIdle(session));
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 2);
    EXPECT_LT((transformOf(sceneId, a).position - transformOf(sceneId, c).position).length(), 6.0f);
}

TEST(PhysicsPlacementSim, InteractiveSurvivesADeletedMoverAndAClosedScene) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(-2.0f, 3.0f, 0.0f));
    const Entity b = create(sceneId, "B", EntityCreationType::BOX, Vector3(2.0f, 3.0f, 0.0f));
    PlacementSession session(project(), sceneId);
    session.setSettings(noSettle());
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {a, b}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(0.0f, 0.0f, 1.0f));
    ticks(session, 30);
    CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, b));
    session.tick(1.0 / 60.0);
    EXPECT_FALSE(session.isBusy());
    expectNear(transformOf(sceneId, a).position, Vector3(-2.0f, 3.0f, 0.0f), 0.0f);

    session.notifySceneChanged();
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {a}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(0.0f, 0.0f, 1.0f));
    ticks(session, 10);
    project()->getScene(sceneId)->isModified = false;
    project()->closeScene(sceneId, true);
    session.tick(1.0 / 60.0);
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(session.getHud().status, "Ready");
}

TEST(PhysicsPlacementSim, OverlayShowsTheTeleportRingAndDistances) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 2.0f, 0.0f));
    PlacementSession session(project(), sceneId);
    PhysicsPlacementSettings settings = noSettle();
    settings.showDistanceIndicators = true;
    settings.showProxies = true;
    session.setSettings(settings);
    session.setSelection({crate});
    PlacementOverlay overlay;
    session.collectOverlay(overlay);
    EXPECT_FALSE(overlay.active);
    bool down = false;
    for (const PlacementOverlay::Distance& d : overlay.distances) {
        if (d.axis == 1 && d.to.y < d.from.y) {
            down = true;
            EXPECT_NEAR(d.meters, 1.5f, 0.01f);
        }
    }
    EXPECT_TRUE(down);
    EXPECT_FALSE(overlay.proxySegments.empty());
    EXPECT_EQ(overlay.proxySegments.size() % 2, 0u);

    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    const Vector3 start = session.getMoversCenter();
    session.setMoveTarget(start + Vector3(3.0f, 0.0f, 0.0f));
    session.collectOverlay(overlay);
    EXPECT_TRUE(overlay.active);
    EXPECT_TRUE(overlay.showTeleportRing);
    EXPECT_FALSE(overlay.teleportExceeded);
    EXPECT_FLOAT_EQ(overlay.teleportRadius, 5.0f);
    expectNear(overlay.teleportCenter, start, 1e-5f);
    EXPECT_FALSE(overlay.region.isNull());
    EXPECT_TRUE(overlay.distances.empty());   // idle selection only
    // the handle past the ring: the ring says so, and the next step teleports the crate there
    session.setMoveTarget(start + Vector3(8.0f, 0.0f, 0.0f));
    session.collectOverlay(overlay);
    EXPECT_TRUE(overlay.teleportExceeded);
    session.tick(1.0 / 60.0);
    EXPECT_NEAR(session.getMoversCenter().x, start.x + 8.0f, 1e-3f);
    session.collectOverlay(overlay);
    EXPECT_FALSE(overlay.teleportExceeded);
    session.cancel();
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 2.0f, 0.0f), 0.0f);
}

TEST(PhysicsPlacementSim, ASecondGestureLandsOnTheFirstOnesResult) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const Entity b = create(sceneId, "B", EntityCreationType::BOX, Vector3(0.0f, 6.0f, 0.0f));
    PlacementSession session(project(), sceneId);
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {a}));
    ticks(session, 300);
    session.end();
    EXPECT_NEAR(transformOf(sceneId, a).position.y, 0.5f, 0.002f);
    // the world is kept between gestures: A is context again where it landed
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {b}));
    ticks(session, 360);
    session.end();
    EXPECT_NEAR(transformOf(sceneId, b).position.y, 1.5f, 0.004f);

    // an ordinary edit between gestures (A moved away by hand) is seen
    transformOf(sceneId, a).position = Vector3(5.0f, 0.5f, 0.0f);
    transformOf(sceneId, a).needUpdate = true;
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {b}));
    ticks(session, 360);
    session.end();
    EXPECT_NEAR(transformOf(sceneId, b).position.y, 0.5f, 0.004f);
}

TEST(PhysicsPlacementSim, ContextEntitiesOrNone) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity floor = makeFloor(sceneId);
    const Entity shelf = create(sceneId, "Shelf", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f), NULL_ENTITY, Vector3(2.0f, 0.2f, 2.0f));
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    PlacementSpec onlyFloor = specFor(sceneId, {crate});
    onlyFloor.context = PlacementSpec::Context::Entities;
    onlyFloor.contextEntities = {floor};
    const PlacementResult throughShelf = runToEnd(onlyFloor);
    EXPECT_NEAR(entityResult(throughShelf, crate)->after.position.y, 0.5f, 0.002f);
    PlacementSpec automatic = specFor(sceneId, {crate});
    const PlacementResult onShelf = runToEnd(automatic);
    EXPECT_NEAR(entityResult(onShelf, crate)->after.position.y, 1.6f, 0.003f);
    EXPECT_EQ(entityResult(onShelf, crate)->restingOn, std::vector<Entity>{shelf});
    PlacementSpec none = specFor(sceneId, {crate});
    none.context = PlacementSpec::Context::None;
    const PlacementResult fell = runToEnd(none);
    EXPECT_EQ(fell.contextBodies, 0u);
    EXPECT_TRUE(entityResult(fell, crate)->fellOut);
}

TEST(PhysicsPlacementSim, PushedNeighboursAreReportedAndCommitted) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 0.0f));
    const Entity pebble = create(sceneId, "Pebble", EntityCreationType::BOX, Vector3(1.5f, 0.25f, 0.0f), NULL_ENTITY, Vector3(0.5f, 0.5f, 0.5f));
    PlacementSpec push = specFor(sceneId, {crate}, PlacementMode::Move);
    push.hasTargetDelta = true;
    push.targetDelta = Vector3(3.0f, 0.0f, 0.0f);
    push.settings.pushNeighbours = true;
    const PlacementResult result = runToEnd(push);
    ASSERT_TRUE(result.ok) << result.error;
    const PlacementEntityResult* pushed = entityResult(result, pebble);
    ASSERT_NE(pushed, nullptr);
    EXPECT_FALSE(pushed->mover);
    EXPECT_GT(pushed->movedMeters, 0.5f);
    std::string error;
    const size_t position = CommandHandle::get(sceneId)->position();
    ASSERT_TRUE(commitPlacement(project(), result, error)) << error;
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);
    EXPECT_GT(transformOf(sceneId, pebble).position.x, 2.0f);
    CommandHandle::get(sceneId)->undo();
    expectNear(transformOf(sceneId, pebble).position, Vector3(1.5f, 0.25f, 0.0f), 0.0f);
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 0.5f, 0.0f), 0.0f);
}

TEST(PhysicsPlacementSim, InteractiveSnapRestsOnABoxTop) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    create(sceneId, "Table", EntityCreationType::BOX, Vector3(4.0f, 0.5f, 0.0f), NULL_ENTITY, Vector3(2.0f, 1.0f, 2.0f));
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f), NULL_ENTITY,
                                Vector3(0.5f, 0.5f, 0.5f));
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    ASSERT_TRUE(session.begin(PlacementGesture::Snap, {crate}));
    // the cursor ray, from a camera up and to the side, onto the table's top
    const Vector3 eye(4.0f, 6.0f, 4.0f);
    session.setSnapRay(Ray(eye, (Vector3(4.2f, 1.0f, 0.1f) - eye).normalized()));
    ticks(session, 180);
    EXPECT_NEAR(transformOf(sceneId, crate).position.x, 4.2f, 0.02f);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 1.25f, 0.01f);   // on the top, not in it
    session.end();
    ASSERT_TRUE(tickUntilIdle(session));
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 1.25f, 0.003f);
    EXPECT_NEAR(transformOf(sceneId, crate).position.z, 0.1f, 0.02f);
    CommandHandle::get(sceneId)->undo();
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 0.0f);
}

TEST(PhysicsPlacementSim, InteractiveStructureChangeMidGestureCancelsIt) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    EXPECT_FALSE(placementSceneBusy(project(), sceneId));
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {crate}));
    EXPECT_TRUE(placementSceneBusy(project(), sceneId));
    EXPECT_FALSE(placementSceneBusy(project(), 4242424242u));
    EXPECT_FALSE(placementSceneBusy(nullptr, sceneId));
    ticks(session, 10);
    EXPECT_LT(transformOf(sceneId, crate).position.y, 2.95f);
    // a GameObject added while the gesture runs (another tool, an agent): the gesture is
    // cancelled, the preview put back
    const Entity step = create(sceneId, "Step", EntityCreationType::BOX, Vector3(5.0f, 0.5f, 0.0f));
    project()->getScene(sceneId)->structureVersion++;
    session.tick(1.0 / 60.0);
    EXPECT_FALSE(session.isBusy());
    EXPECT_FALSE(placementSceneBusy(project(), sceneId));
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 0.0f);
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);   // the create only

    // re-parented mid-gesture: back where it started in the world, in its new parent's space
    const Entity holder = create(sceneId, "Holder", EntityCreationType::GAMEOBJECT, Vector3(5.0f, 1.0f, 0.0f), NULL_ENTITY,
                                 Vector3(1.0f, 1.0f, 1.0f), yaw(90.0f));
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(1.0f, 0.0f, 0.0f));
    ticks(session, 20);
    sceneOf(sceneId)->addEntityChild(holder, crate, true);
    project()->getScene(sceneId)->structureVersion++;
    session.tick(1.0 / 60.0);
    EXPECT_FALSE(session.isBusy());
    ASSERT_EQ(transformOf(sceneId, crate).parent, holder);
    const Transform& local = transformOf(sceneId, crate);
    expectNear(yaw(90.0f) * local.position + Vector3(5.0f, 1.0f, 0.0f), Vector3(0.0f, 3.0f, 0.0f), 1e-4f);
    expectSameRotation(yaw(90.0f) * local.rotation, Quaternion(), 1e-5f);

    // the next gesture sees the new box
    const Entity other = create(sceneId, "Other", EntityCreationType::BOX, Vector3(5.0f, 4.0f, 0.0f));
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {other}));
    ticks(session, 300);
    session.end();
    EXPECT_NEAR(transformOf(sceneId, other).position.y, 1.5f, 0.004f);
    (void)step;
}

TEST(PhysicsPlacementSim, BatchMoveNeverTeleportsThroughAWall) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 0.0f));
    create(sceneId, "Wall", EntityCreationType::BOX, Vector3(3.0f, 2.5f, 0.0f), NULL_ENTITY, Vector3(1.0f, 5.0f, 5.0f));
    PlacementSpec far = specFor(sceneId, {crate}, PlacementMode::Move);
    far.hasTargetDelta = true;
    far.targetDelta = Vector3(9.0f, 0.0f, 0.0f);   // past the 5 m teleport distance, behind the wall
    const PlacementResult result = runToEnd(far);
    ASSERT_TRUE(result.ok) << result.error;
    const PlacementEntityResult* e = entityResult(result, crate);
    EXPECT_FALSE(e->teleported);
    EXPECT_LE(e->after.position.x, 2.0f + 0.01f);
    EXPECT_GE(e->after.position.x, 1.5f);
    EXPECT_FALSE(contains(result.warnings, "teleported"));
}

TEST(PhysicsPlacementSim, CaveatsAreWarningsNotRefusals) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 2.0f, 0.0f));
    const Entity anchor = create(sceneId, "Anchor", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 5.0f, 0.0f));
    Joint3DComponent joint;
    joint.bodyA = crate;
    joint.bodyB = anchor;
    sceneOf(sceneId)->addComponent<Joint3DComponent>(anchor, joint);

    const PlacementResult result = runToEnd(specFor(sceneId, {crate}));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.refused.empty());
    EXPECT_TRUE(contains(entityResult(result, crate)->warnings, "simulated free"));
    EXPECT_TRUE(contains(result.warnings, "Crate: has joints: simulated free"));

    PlacementSession session(project(), sceneId);
    session.setSelection({crate});
    EXPECT_TRUE(session.getHud().refused.empty());
    ASSERT_EQ(session.getHud().caveats.size(), 1u);
    EXPECT_NE(session.getHud().caveats[0].reason.find("simulated free"), std::string::npos);
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {crate}));
    EXPECT_EQ(session.getHud().caveats.size(), 1u);
    session.cancel();
}

TEST(PhysicsPlacementSim, RunsAndSessionsOutliveTheirSceneAndCache) {
    REQUIRE_PHYSICS();
    // the scene closed under a run: advancing abandons it, destroying it touches nothing
    {
        const uint32_t sceneId = newScene();
        makeFloor(sceneId);
        const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
        std::unique_ptr<PlacementRun> run(new PlacementRun());
        ASSERT_TRUE(run->start(project(), specFor(sceneId, {crate})));
        EXPECT_FALSE(run->advance(0.01));
        project()->getScene(sceneId)->isModified = false;
        project()->closeScene(sceneId, true);
        ASSERT_EQ(project()->getScene(sceneId), nullptr);
        EXPECT_TRUE(run->advance(0.0));
        EXPECT_TRUE(run->result().cancelled);
        EXPECT_FALSE(run->result().ok);
        EXPECT_NE(run->result().error.find("went away"), std::string::npos);
        run.reset();
    }
    {
        const uint32_t sceneId = newScene();
        makeFloor(sceneId);
        const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
        std::unique_ptr<PlacementRun> run(new PlacementRun());
        ASSERT_TRUE(run->start(project(), specFor(sceneId, {crate})));
        project()->getScene(sceneId)->isModified = false;
        project()->closeScene(sceneId, true);
        run.reset();   // never advanced again: destroyed with its scene gone
    }
    // the decomposition cache dropped under a run (a project switch): the same
    {
        const uint32_t sceneId = newScene();
        makeFloor(sceneId);
        const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
        std::unique_ptr<PlacementRun> run(new PlacementRun());
        ASSERT_TRUE(run->start(project(), specFor(sceneId, {crate})));
        placementProjectSwitching();
        run->cancel();
        EXPECT_TRUE(run->result().cancelled);
        EXPECT_NE(run->result().error.find("went away"), std::string::npos);
        run.reset();

        std::unique_ptr<PlacementRun> unstarted(new PlacementRun());
        ASSERT_TRUE(unstarted->start(project(), specFor(sceneId, {crate})));
        placementProjectSwitching();
        unstarted.reset();
    }
    // a session: the cache dropped mid-gesture puts the poses back; the scene closed under it
    // leaves nothing to restore; destroying it then touches neither
    {
        const uint32_t sceneId = newScene();
        makeFloor(sceneId);
        const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
        std::unique_ptr<PlacementSession> session(new PlacementSession(project(), sceneId));
        ASSERT_TRUE(session->begin(PlacementGesture::Drop, {crate}));
        ticks(*session, 20);
        placementProjectSwitching();
        session->tick(1.0 / 60.0);
        EXPECT_FALSE(session->isBusy());
        expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 0.0f);
        EXPECT_EQ(session->getHud().status, "Ready");

        ASSERT_TRUE(session->begin(PlacementGesture::Drop, {crate}));   // a new world, a new cache
        ticks(*session, 20);
        placementProjectSwitching();
        session.reset();   // the destructor cancels without driving the stale world
        expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 3.0f, 0.0f), 0.0f);

        session.reset(new PlacementSession(project(), sceneId));
        ASSERT_TRUE(session->begin(PlacementGesture::Drop, {crate}));
        ticks(*session, 20);
        project()->getScene(sceneId)->isModified = false;
        project()->closeScene(sceneId, true);
        session.reset();
    }
}

TEST(PhysicsPlacementSim, BatchSnapPlacesOnTheSurfaceTheRayHits) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity table = create(sceneId, "Table", EntityCreationType::BOX, Vector3(2.0f, 0.5f, 0.0f), NULL_ENTITY, Vector3(2.0f, 1.0f, 2.0f));
    // on the floor beside the table: a path toward the top would jam against its side
    const Entity cube = create(sceneId, "Cube", EntityCreationType::BOX, Vector3(0.0f, 0.25f, 0.0f), NULL_ENTITY, Vector3(0.5f, 0.5f, 0.5f));
    PlacementSpec snap = specFor(sceneId, {cube}, PlacementMode::Snap);
    snap.hasSnapRay = true;
    snap.snapRay = Ray(Vector3(2.2f, 5.0f, 0.3f), Vector3(0.0f, -1.0f, 0.0f));
    const PlacementResult result = runToEnd(snap);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.allSettled);
    const PlacementEntityResult* e = entityResult(result, cube);
    EXPECT_NEAR(e->after.position.x, 2.2f, 0.01f);
    EXPECT_NEAR(e->after.position.z, 0.3f, 0.01f);
    EXPECT_NEAR(e->after.position.y, 1.25f, 0.003f);
    EXPECT_EQ(e->restingOn, std::vector<Entity>{table});
    EXPECT_FALSE(contains(result.warnings, "teleport"));
    EXPECT_TRUE(e->warnings.empty());

    // without settle it is put there and not stepped
    PlacementSpec placed = snap;
    placed.settle = false;
    const PlacementResult put = runToEnd(placed);
    ASSERT_TRUE(put.ok) << put.error;
    EXPECT_EQ(put.steps, 0u);
    EXPECT_TRUE(put.allSettled);
    EXPECT_NEAR(entityResult(put, cube)->after.position.y, 1.25f, 0.003f);

    // far away (past the teleport distance): on the floor where the ray hits
    PlacementSpec far = snap;
    far.snapRay = Ray(Vector3(9.0f, 5.0f, -3.0f), Vector3(0.0f, -1.0f, 0.0f));
    const PlacementResult landed = runToEnd(far);
    ASSERT_TRUE(landed.ok) << landed.error;
    EXPECT_NEAR(entityResult(landed, cube)->after.position.x, 9.0f, 0.01f);
    EXPECT_NEAR(entityResult(landed, cube)->after.position.z, -3.0f, 0.01f);
    EXPECT_NEAR(entityResult(landed, cube)->after.position.y, 0.25f, 0.003f);
    EXPECT_FALSE(contains(landed.warnings, "teleport"));
}

TEST(PhysicsPlacementSim, InteractiveSnapHopsOntoTheTableInsteadOfJamming) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    create(sceneId, "Table", EntityCreationType::BOX, Vector3(2.0f, 0.5f, 0.0f), NULL_ENTITY, Vector3(2.0f, 1.0f, 2.0f));
    const Entity cube = create(sceneId, "Cube", EntityCreationType::BOX, Vector3(0.0f, 0.25f, 0.0f), NULL_ENTITY, Vector3(0.5f, 0.5f, 0.5f));
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    ASSERT_TRUE(session.begin(PlacementGesture::Snap, {cube}));
    const Vector3 eye(-2.0f, 4.0f, 0.0f);
    // the cursor on the floor next to the cube: it slides there
    session.setSnapRay(Ray(eye, (Vector3(0.5f, 0.0f, 0.0f) - eye).normalized()));
    ticks(session, 30);
    EXPECT_NEAR(transformOf(sceneId, cube).position.x, 0.5f, 0.02f);
    EXPECT_NEAR(transformOf(sceneId, cube).position.y, 0.25f, 0.01f);
    // then onto the table top: blocked by the table's side, it hops on top
    session.setSnapRay(Ray(eye, (Vector3(2.0f, 1.0f, 0.2f) - eye).normalized()));
    ticks(session, 60);
    EXPECT_NEAR(transformOf(sceneId, cube).position.x, 2.0f, 0.02f);
    EXPECT_NEAR(transformOf(sceneId, cube).position.y, 1.25f, 0.01f);
    session.end();
    ASSERT_TRUE(tickUntilIdle(session));
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position + 1);
    EXPECT_NEAR(transformOf(sceneId, cube).position.y, 1.25f, 0.003f);
    EXPECT_NEAR(transformOf(sceneId, cube).position.z, 0.2f, 0.02f);

    // a far ray (past the 5 m teleport distance) lands on the surface it hits
    ASSERT_TRUE(session.begin(PlacementGesture::Snap, {cube}));
    session.setSnapRay(Ray(Vector3(8.0f, 6.0f, 3.0f), Vector3(0.0f, -1.0f, 0.0f)));
    ticks(session, 30);
    session.end();
    ASSERT_TRUE(tickUntilIdle(session));
    EXPECT_NEAR(transformOf(sceneId, cube).position.x, 8.0f, 0.02f);
    EXPECT_NEAR(transformOf(sceneId, cube).position.z, 3.0f, 0.02f);
    EXPECT_NEAR(transformOf(sceneId, cube).position.y, 0.25f, 0.003f);
}

namespace {

    Quaternion euler(float xDeg, float yDeg, float zDeg) {
        const float k = 3.14159265358979f / 180.0f;
        auto about = [](const Vector3& axis, float r) {
            return Quaternion(std::cos(r / 2.0f), axis.x * std::sin(r / 2.0f), axis.y * std::sin(r / 2.0f), axis.z * std::sin(r / 2.0f));
        };
        return about(Vector3(0, 1, 0), yDeg * k) * about(Vector3(1, 0, 0), xDeg * k) * about(Vector3(0, 0, 1), zDeg * k);
    }

    // The fixture's TableCrate scene (tools/fixtures/physics-placement): a table of five boxes
    // (top face at y = 1.0), an open-top crate of five boxes standing on it (inside 0.52 x 0.52,
    // floor at y = 1.04, rim at y = 1.42) and three small tilted boxes in the air above it
    struct TableCrate {
        uint32_t sceneId = 0;
        Entity table = NULL_ENTITY;
        Entity crate = NULL_ENTITY;
        std::vector<Entity> boxes;
    };

    TableCrate tableCrate() {
        TableCrate t;
        t.sceneId = newScene();
        const uint32_t id = t.sceneId;
        create(id, "Floor", EntityCreationType::BOX, Vector3(0.0f, -0.1f, 0.0f), NULL_ENTITY, Vector3(12.0f, 0.2f, 12.0f));
        t.table = create(id, "Table", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 0.0f, 0.0f));
        create(id, "Top", EntityCreationType::BOX, Vector3(0.0f, 0.95f, 0.0f), t.table, Vector3(1.6f, 0.1f, 1.0f));
        const float legs[4][2] = {{-0.7f, -0.4f}, {0.7f, -0.4f}, {-0.7f, 0.4f}, {0.7f, 0.4f}};
        for (int i = 0; i < 4; i++) {
            create(id, "Leg" + std::to_string(i + 1), EntityCreationType::BOX, Vector3(legs[i][0], 0.45f, legs[i][1]), t.table,
                   Vector3(0.08f, 0.9f, 0.08f));
        }
        const Entity props = create(id, "Props", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 0.0f, 0.0f));
        t.crate = create(id, "Crate", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 1.0f, 0.0f), props);
        create(id, "Bottom", EntityCreationType::BOX, Vector3(0.0f, 0.02f, 0.0f), t.crate, Vector3(0.6f, 0.04f, 0.6f));
        create(id, "WallN", EntityCreationType::BOX, Vector3(0.0f, 0.23f, 0.28f), t.crate, Vector3(0.6f, 0.38f, 0.04f));
        create(id, "WallS", EntityCreationType::BOX, Vector3(0.0f, 0.23f, -0.28f), t.crate, Vector3(0.6f, 0.38f, 0.04f));
        create(id, "WallE", EntityCreationType::BOX, Vector3(0.28f, 0.23f, 0.0f), t.crate, Vector3(0.04f, 0.38f, 0.52f));
        create(id, "WallW", EntityCreationType::BOX, Vector3(-0.28f, 0.23f, 0.0f), t.crate, Vector3(0.04f, 0.38f, 0.52f));
        t.boxes.push_back(create(id, "Box1", EntityCreationType::BOX, Vector3(-0.1f, 1.75f, -0.08f), props, Vector3(0.14f, 0.14f, 0.14f),
                                 euler(12.0f, 25.0f, 4.0f)));
        t.boxes.push_back(create(id, "Box2", EntityCreationType::BOX, Vector3(0.09f, 2.0f, 0.06f), props, Vector3(0.12f, 0.12f, 0.12f),
                                 euler(-8.0f, 40.0f, 15.0f)));
        t.boxes.push_back(create(id, "Box3", EntityCreationType::BOX, Vector3(0.0f, 2.3f, 0.1f), props, Vector3(0.16f, 0.1f, 0.12f),
                                 euler(30.0f, 5.0f, -10.0f)));
        return t;
    }

    // inside the crate (as it now stands): between its floor and its rim, within its walls
    void expectInsideCrate(const Vector3& box, const Vector3& crate) {
        EXPECT_GT(box.y, crate.y + 0.04f);
        EXPECT_LT(box.y, crate.y + 0.42f);
        EXPECT_LT(std::fabs(box.x - crate.x), 0.26f);
        EXPECT_LT(std::fabs(box.z - crate.z), 0.26f);
    }

}

TEST(PhysicsPlacementSim, TableCrateDropPushingNeighboursWithATightMargin) {
    REQUIRE_PHYSICS();
    const TableCrate t = tableCrate();
    const Vector3 tableBefore = transformOf(t.sceneId, t.table).position;
    PlacementSpec drop = specFor(t.sceneId, t.boxes);
    drop.settings.pushNeighbours = true;
    drop.contextMargin = 0.5f;
    const PlacementResult result = runToEnd(drop);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(contains(result.warnings, "fell out")) << (result.warnings.empty() ? "" : result.warnings[0]);
    EXPECT_FALSE(contains(result.warnings, "nothing to land on"));
    Vector3 crate(0.0f, 1.0f, 0.0f);
    for (const PlacementEntityResult& e : result.entities) {
        EXPECT_FALSE(e.fellOut) << e.path;
        if (e.entity == t.table) {
            EXPECT_LT(e.movedMeters, 0.02f);
        }
        if (e.entity == t.crate) {
            crate = e.after.position;
            EXPECT_FALSE(e.mover);   // nudged by the boxes landing in it: a pushed neighbour
        }
    }
    ASSERT_NE(entityResult(result, t.crate), nullptr);
    for (Entity box : t.boxes) {
        const PlacementEntityResult* e = entityResult(result, box);
        ASSERT_NE(e, nullptr);
        EXPECT_TRUE(e->settled) << e->path;
        EXPECT_FALSE(e->restingOn.empty()) << e->path;
        expectInsideCrate(e->after.position, crate);
    }
    std::string error;
    ASSERT_TRUE(commitPlacement(project(), result, error)) << error;
    EXPECT_LT((transformOf(t.sceneId, t.table).position - tableBefore).length(), 0.02f);
}

TEST(PhysicsPlacementSim, TableCrateInteractiveDropPushingNeighboursWithATightMargin) {
    REQUIRE_PHYSICS();
    const TableCrate t = tableCrate();
    const Vector3 tableBefore = transformOf(t.sceneId, t.table).position;
    const size_t position = CommandHandle::get(t.sceneId)->position();
    PlacementSession session(project(), t.sceneId);
    PhysicsPlacementSettings settings;
    settings.pushNeighbours = true;
    settings.regionMargin = 0.5f;
    session.setSettings(settings);
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, t.boxes));
    ticks(session, 360);
    EXPECT_EQ(session.getHud().status, "Resting");
    session.end();
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(CommandHandle::get(t.sceneId)->position(), position + 1);
    EXPECT_LT((transformOf(t.sceneId, t.table).position - tableBefore).length(), 0.02f);
    const Vector3 crate = transformOf(t.sceneId, t.crate).position;   // Props is at the origin, unturned
    EXPECT_GT(crate.y, 0.98f);   // still on the table
    for (Entity box : t.boxes) expectInsideCrate(transformOf(t.sceneId, box).position, crate);
    CommandHandle::get(t.sceneId)->undo();
    EXPECT_EQ(transformOf(t.sceneId, t.table).position, tableBefore);
}

TEST(PhysicsPlacementSim, EveryBoxOfADroppedStackReportsWhatItRestsOn) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity floor = makeFloor(sceneId);
    std::vector<Entity> boxes;
    for (int i = 0; i < 5; i++) {
        boxes.push_back(create(sceneId, "Box" + std::to_string(i + 1), EntityCreationType::BOX,
                               Vector3(i % 2 ? 0.01f : -0.01f, 0.2f + i * 0.402f + 0.002f, 0.0f), NULL_ENTITY, Vector3(0.4f, 0.4f, 0.4f)));
    }
    const PlacementResult result = runToEnd(specFor(sceneId, boxes));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.allSettled);
    for (size_t i = 0; i < boxes.size(); i++) {
        const PlacementEntityResult* e = entityResult(result, boxes[i]);
        ASSERT_NE(e, nullptr);
        EXPECT_TRUE(e->settled);
        EXPECT_GT(e->settleStep, 0u);
        EXPECT_EQ(e->restingOn, std::vector<Entity>{i == 0 ? floor : boxes[i - 1]}) << e->path;
        EXPECT_NEAR(e->after.position.y, 0.2f + 0.4f * i, 0.005f);
    }
    EXPECT_TRUE(result.warnings.empty()) << result.warnings[0];
}

TEST(PhysicsPlacementSim, ABatchRunFailsWhenTheSceneIsEditedBetweenSlices) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity floor = makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    PlacementRun run;
    ASSERT_TRUE(run.start(project(), specFor(sceneId, {crate})));
    EXPECT_EQ(run.result().sceneEditStamp, placementSceneEditStamp(project(), sceneId));
    EXPECT_FALSE(run.advance(0.01));
    // the floor moved by an agent while the run is between two slices
    CommandHandle::get(sceneId)->addCommandNoMerge(new ObjectTransformCmd(project(), sceneId, floor, Vector3(0.0f, -1.5f, 0.0f), Quaternion(),
                                                                           Vector3(20.0f, 1.0f, 20.0f)));
    EXPECT_TRUE(run.advance(0.0));
    EXPECT_FALSE(run.result().ok);
    EXPECT_EQ(run.result().error, "the scene changed during the simulation: run it again");

    // a finished run records where every entity started, and commits only onto that
    const PlacementResult result = runToEnd(specFor(sceneId, {crate}));
    ASSERT_TRUE(result.ok);
    const PlacementEntityResult* e = entityResult(result, crate);
    expectNear(e->beforeLocalPosition, Vector3(0.0f, 3.0f, 0.0f), 0.0f);
    EXPECT_EQ(e->beforeParent, NULL_ENTITY);
    std::string error;
    transformOf(sceneId, crate).position.x += 0.5f;
    EXPECT_FALSE(commitPlacement(project(), result, error));
    EXPECT_EQ(error, "the scene changed during the simulation: run it again");
}

TEST(PhysicsPlacementSim, UnderGravityOnlyATouchingBodyHasSettled) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 5.0f, 0.0f));
    PlacementSpec slow = specFor(sceneId, {crate});
    slow.gravity = Vector3(0.0f, -0.3f, 0.0f);
    slow.maxSeconds = 1.0f;
    const PlacementResult result = runToEnd(slow);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.allSettled);
    EXPECT_EQ(result.steps, 120u);
    EXPECT_FALSE(entityResult(result, crate)->settled);
    EXPECT_TRUE(contains(result.warnings, "Crate never came to rest within 1 s"));

    // the interactive settle does not commit it in mid-air either
    PlacementSession session(project(), sceneId);
    PhysicsPlacementSettings settings;
    settings.gravity = Vector3(0.0f, -0.3f, 0.0f);
    session.setSettings(settings);
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(0.5f, 0.0f, 0.0f));
    ticks(session, 30);
    session.end();
    ticks(session, 60);
    EXPECT_EQ(session.getPhase(), PlacementPhase::Settling);
    EXPECT_GT(transformOf(sceneId, crate).position.y, 1.0f);
    session.cancel();
}

TEST(PhysicsPlacementSim, LocalTargetsAreInTheFirstMoversParentInPathOrder) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity turned = create(sceneId, "P1", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 0.0f, 0.0f), NULL_ENTITY,
                                 Vector3(1.0f, 1.0f, 1.0f), yaw(90.0f));
    const Entity plain = create(sceneId, "P2", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 0.0f, 0.0f));
    const Entity a = create(sceneId, "A", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f), turned);
    const Entity z = create(sceneId, "Z", EntityCreationType::BOX, Vector3(3.0f, 3.0f, 0.0f), plain);
    PlacementSpec spec = specFor(sceneId, {z, a}, PlacementMode::Move);
    spec.hasTargetDelta = true;
    spec.targetDelta = Vector3(1.0f, 0.0f, 0.0f);
    spec.targetLocal = true;
    spec.settle = false;
    spec.context = PlacementSpec::Context::None;
    PlacementSpec reversed = spec;
    reversed.entities = {a, z};
    EXPECT_EQ(spec.fingerprint(), reversed.fingerprint());
    const PlacementResult first = runToEnd(spec);
    const PlacementResult second = runToEnd(reversed);
    ASSERT_TRUE(first.ok && second.ok);
    EXPECT_EQ(first.stateHash, second.stateHash);
    // P1/A comes first by path: its parent's +x is the world's -z
    const PlacementEntityResult* moved = entityResult(first, z);
    expectNear(moved->after.position - moved->before.position, Vector3(0.0f, 0.0f, -1.0f), 0.01f);
}

TEST(PhysicsPlacementSim, AnArrivalOnTheLastStepCounts) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    PlacementSpec spec = specFor(sceneId, {crate}, PlacementMode::Move);
    spec.hasTargetDelta = true;
    spec.targetDelta = Vector3(1.0f, 0.0f, 0.0f);
    spec.settle = false;
    spec.context = PlacementSpec::Context::None;
    const PlacementResult free = runToEnd(spec);
    ASSERT_TRUE(free.ok && free.allSettled);
    ASSERT_GT(free.steps, 1u);
    // exactly as long as the arrival takes
    spec.maxSeconds = static_cast<float>(free.steps) / 120.0f;
    const PlacementResult tight = runToEnd(spec);
    EXPECT_EQ(tight.steps, free.steps);
    EXPECT_TRUE(tight.allSettled);
    EXPECT_TRUE(tight.warnings.empty()) << tight.warnings[0];
    // one step short: not there
    spec.maxSeconds = static_cast<float>(free.steps - 1) / 120.0f;
    const PlacementResult shorter = runToEnd(spec);
    EXPECT_FALSE(shorter.allSettled);
    EXPECT_TRUE(contains(shorter.warnings, "did not reach the target"));
}

TEST(PhysicsPlacement, CommitRecordsOnlyRealOverridesOnAPrefabInstance) {
    const uint32_t sceneId = newScene();
    SceneProject* sp = project()->getScene(sceneId);
    const Entity root = create(sceneId, "Shelf", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 0.0f, 0.0f));
    const Entity board = create(sceneId, "Board", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f), root, Vector3(1.0f, 1.0f, 1.0f), yaw(30.0f));
    CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project(), sceneId, "Shelf.prefab",
                                                                       Stream::encodeEntity(root, sp->scene, project(), sp)));
    ASSERT_TRUE(project()->isPrefabInstanceRoot(sceneId, root));
    auto boardOverrides = [&]() {
        std::set<std::string> properties;
        for (const PrefabOverrideEntry& entry : project()->getPrefabOverrideEntries(sceneId, root)) {
            if (entry.kind == PrefabOverrideEntry::Kind::Property && entry.entity == board && entry.type == ComponentType::Transform) {
                properties.insert(entry.properties.begin(), entry.properties.end());
            }
        }
        return properties;
    };
    ASSERT_TRUE(boardOverrides().empty());

    // the board slid, its rotation read back a hair off (world to local and back)
    PlacementResult result;
    result.ok = true;
    result.sceneId = sceneId;
    result.sceneStructureVersion = sp->structureVersion;
    result.sceneEditStamp = placementSceneEditStamp(project(), sceneId);
    PlacementEntityResult e;
    e.entity = board;
    e.path = "Shelf/Board";
    const Transform& t = transformOf(sceneId, board);
    e.beforeLocalPosition = t.position;
    e.beforeLocalRotation = t.rotation;
    e.beforeLocalScale = t.scale;
    e.beforeParent = t.parent;
    e.afterLocalPosition = t.position + Vector3(0.25f, 0.0f, 0.0f);
    e.afterLocalRotation = Quaternion(t.rotation.w + 2e-8f, t.rotation.x, t.rotation.y - 3e-8f, t.rotation.z);
    result.entities = {e};
    std::string error;
    ASSERT_TRUE(commitPlacement(project(), result, error)) << error;
    EXPECT_EQ(boardOverrides(), std::set<std::string>{"position"});
    EXPECT_EQ(transformOf(sceneId, board).rotation, e.beforeLocalRotation);   // the old rotation kept exactly
    CommandHandle::get(sceneId)->undo();
    EXPECT_TRUE(boardOverrides().empty());
}

// The surface a ray hits is the nearest one in the scene, loaded or not: a floor already in the
// context under a table that is not yet must not take the hit
TEST(PhysicsPlacementSim, SnapHitsTheNearestSurfaceNotTheNearestLoadedOne) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    create(sceneId, "Floor", EntityCreationType::BOX, Vector3(0.0f, -0.5f, 0.0f), NULL_ENTITY, Vector3(60.0f, 1.0f, 60.0f));
    const Entity table = create(sceneId, "Table", EntityCreationType::BOX, Vector3(-10.0f, 0.5f, -10.0f), NULL_ENTITY, Vector3(2.0f, 1.0f, 2.0f));
    const Entity cube = create(sceneId, "Snapper", EntityCreationType::BOX, Vector3(-6.0f, 0.5f, -10.0f));
    const Ray ray(Vector3(-10.0f, 10.0f, -10.0f), Vector3(0.0f, -1.0f, 0.0f));

    PlacementSpec snap = specFor(sceneId, {cube}, PlacementMode::Snap);
    snap.hasSnapRay = true;
    snap.snapRay = ray;
    const PlacementResult result = runToEnd(snap);
    ASSERT_TRUE(result.ok) << result.error;
    const PlacementEntityResult* e = entityResult(result, cube);
    expectNear(e->after.position, Vector3(-10.0f, 1.5f, -10.0f), 0.005f);
    EXPECT_EQ(e->restingOn, std::vector<Entity>{table});

    PlacementSession session(project(), sceneId);
    ASSERT_TRUE(session.begin(PlacementGesture::Snap, {cube}));
    session.setSnapRay(ray);
    ticks(session, 60);
    expectNear(transformOf(sceneId, cube).position, Vector3(-10.0f, 1.5f, -10.0f), 0.01f);   // on top while held
    session.end();
    ASSERT_TRUE(tickUntilIdle(session));
    expectNear(transformOf(sceneId, cube).position, Vector3(-10.0f, 1.5f, -10.0f), 0.005f);
}

TEST(PhysicsPlacementSim, OverflowingContactsAreReported) {
    REQUIRE_PHYSICS();
    // 300 small boxes in one spot: more touching pairs than Jolt's contact buffers hold
    const uint32_t sceneId = newScene();
    std::vector<Entity> pile;
    for (int i = 0; i < 300; i++) {
        pile.push_back(create(sceneId, "Box", EntityCreationType::BOX, Vector3(0.0001f * i, 3.0f, 0.0f), NULL_ENTITY, Vector3(0.2f, 0.2f, 0.2f)));
    }
    PlacementSpec spec = specFor(sceneId, pile, PlacementMode::Separate);
    spec.maxSeconds = 1.0f / 120.0f;
    spec.context = PlacementSpec::Context::None;
    const PlacementResult result = runToEnd(spec);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(contains(result.warnings, "too many contacts in the placement world: results may be wrong; place fewer objects at once"));

    PlacementSession session(project(), sceneId);
    session.setSettings(noSettle());
    ASSERT_TRUE(session.begin(PlacementGesture::Move, pile));
    EXPECT_EQ(session.getHud().status.find("too many contacts"), std::string::npos);
    session.setMoveTarget(session.getMoversCenter());
    session.tick(1.0 / 60.0);
    EXPECT_NE(session.getHud().status.find("\xC2\xB7 too many contacts: place fewer objects at once"), std::string::npos)
        << session.getHud().status;
    session.cancel();
    // a new gesture starts clean
    const std::vector<Entity> one = {pile[0]};
    ASSERT_TRUE(session.begin(PlacementGesture::Move, one));
    EXPECT_EQ(session.getHud().status.find("too many contacts"), std::string::npos);
    session.cancel();

    // a run that never overflows says nothing
    PlacementSpec few = specFor(sceneId, one, PlacementMode::Separate);
    few.maxSeconds = 1.0f / 120.0f;
    EXPECT_FALSE(contains(runToEnd(few).warnings, "too many contacts"));
}

// The tool's finalise path (save, Play, a command hitting a busy scene: CommandHistory::
// beforeChange): the settle finishes and commits now, then the change runs
TEST(PhysicsPlacementSim, FinishNowCommitsTheSettleBeforeTheChangeThatAskedForIt) {
    REQUIRE_PHYSICS();
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 0.5f, 0.0f));
    const Entity marker = create(sceneId, "Marker", EntityCreationType::BOX, Vector3(6.0f, 0.5f, 0.0f));
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t position = history->position();
    PlacementSession session(project(), sceneId);
    EXPECT_FALSE(session.finishNow(1.0));   // idle: nothing to finish

    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(2.0f, 1.0f, 0.0f));
    ticks(session, 60);
    session.end();
    ASSERT_EQ(session.getPhase(), PlacementPhase::Settling);
    // a Delete pressed during the settle: the finalise first...
    EXPECT_TRUE(session.finishNow(2.0));
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(history->position(), position + 1);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 0.5f, 0.003f);   // it came to rest first
    EXPECT_NEAR(transformOf(sceneId, crate).position.x, 2.0f, 0.05f);
    // ...then the delete, a structure change: nothing of the committed gesture is cancelled
    history->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, marker));
    project()->getScene(sceneId)->structureVersion++;
    ticks(session, 10);
    EXPECT_EQ(history->position(), position + 2);
    EXPECT_NEAR(transformOf(sceneId, crate).position.x, 2.0f, 0.05f);
    history->undo();   // the delete
    history->undo();   // the placement
    expectNear(transformOf(sceneId, crate).position, Vector3(0.0f, 0.5f, 0.0f), 0.0f);
    history->redo();

    // the mover itself deleted during a settle: committed, then deleted, and undo brings both back
    const Vector3 rested = transformOf(sceneId, crate).position;
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(-1.0f, 1.0f, 0.0f));
    ticks(session, 60);
    session.end();
    EXPECT_TRUE(session.finishNow(2.0));
    const Vector3 committedPose = transformOf(sceneId, crate).position;
    EXPECT_NEAR(committedPose.x, rested.x - 1.0f, 0.05f);
    history->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, crate));
    project()->getScene(sceneId)->structureVersion++;
    ticks(session, 5);
    EXPECT_FALSE(session.isBusy());
    history->undo();
    expectNear(transformOf(sceneId, crate).position, committedPose, 1e-5f);
    history->undo();
    expectNear(transformOf(sceneId, crate).position, rested, 1e-5f);

    // a gesture still held when finishNow comes is released first; a Drop commits at once
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(0.0f, 1.0f, 0.0f));
    ticks(session, 30);
    const size_t before = history->position();
    EXPECT_TRUE(session.finishNow(2.0));
    EXPECT_EQ(history->position(), before + 1);
    EXPECT_NEAR(transformOf(sceneId, crate).position.y, 0.5f, 0.003f);
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {crate}));
    EXPECT_TRUE(session.finishNow(0.0));
    EXPECT_FALSE(session.isBusy());

    // out of wall time mid-settle: what it has is committed
    ASSERT_TRUE(session.begin(PlacementGesture::Move, {crate}));
    session.setMoveTarget(session.getMoversCenter() + Vector3(0.0f, 2.0f, 0.0f));
    ticks(session, 60);
    session.end();
    const size_t mid = history->position();
    EXPECT_TRUE(session.finishNow(0.0));
    EXPECT_EQ(history->position(), mid + 1);
    EXPECT_GT(transformOf(sceneId, crate).position.y, 2.0f);   // still up where it was released
}

TEST(PhysicsPlacementSim, FinishNowCancelsWhenNoStepCouldRun) {
    REQUIRE_PHYSICS();
    if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "no convex decomposition in this build";
    const uint32_t sceneId = newScene();
    makeFloor(sceneId);
    const Entity ring = create(sceneId, "Ring", EntityCreationType::TORUS, Vector3(0.0f, 2.0f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();
    PlacementSession session(project(), sceneId);
    PhysicsPlacementSettings settings;
    settings.decomposition = PlacementDecomposition::ALWAYS;
    settings.quality = DecompositionQuality::HIGH;
    session.setSettings(settings);
    ASSERT_TRUE(session.begin(PlacementGesture::Drop, {ring}));
    if (session.getHud().pendingDecompositions == 0) GTEST_SKIP() << "the torus' decomposition was already there";
    session.tick(1.0 / 60.0);   // waits for its colliders: no step
    EXPECT_FALSE(session.finishNow(0.0));
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position);
    expectNear(transformOf(sceneId, ring).position, Vector3(0.0f, 2.0f, 0.0f), 0.0f);
}
