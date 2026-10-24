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
#include "Platform.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "physics/PlacementSession.h"
#include "physics/PlacementSettings.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "util/EntityPath.h"

#include "json.hpp"

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
using nlohmann::json;

namespace {

    // ---- one headless project for the whole binary (Engine::systemInit runs once)

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
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

    struct EngineShutdown : public ::testing::Environment {
        void TearDown() override;
    };

    bool headlessStarted = false;

    void EngineShutdown::TearDown() {
        if (!headlessStarted) return;
        placementProjectSwitching();
        ShaderPool::setShaderBuilder(nullptr);
        ShaderBuilder::requestShutdown();
        Engine::systemShutdown();
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

    // The engine's PlacementWorld can build movers (false while it is the contract stub)
    bool physicsAvailable(std::string* why = nullptr) {
        const uint32_t sceneId = newScene();
        const Entity box = create(sceneId, "Probe", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f));
        PlacementWorld world(sceneOf(sceneId), PlacementWorldSettings());
        std::string reason;
        const bool ok = world.addMover(box, &reason);
        if (why) *why = reason;
        return ok;
    }

#define REQUIRE_PHYSICS()                                                                  \
    do {                                                                                   \
        std::string reason;                                                                \
        if (!physicsAvailable(&reason)) GTEST_SKIP() << "PlacementWorld: " << reason;    \
    } while (0)

}

TEST(PhysicsPlacement, HeadlessProjectOpens) {
    ASSERT_TRUE(headless().ready);
    const uint32_t sceneId = newScene();
    ASSERT_NE(project()->getScene(sceneId), nullptr);
    const Entity box = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(1.0f, 2.0f, 3.0f));
    ASSERT_NE(box, NULL_ENTITY);
    expectNear(transformOf(sceneId, box).position, Vector3(1.0f, 2.0f, 3.0f), 1e-6f);
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

TEST(PhysicsPlacement, WorldSettingsMapEveryField) {
    PhysicsPlacementSettings s;
    s.feel = PlacementFeel::Fast;
    s.colliders = PlacementColliderPolicy::BOTH;
    s.decomposition = PlacementDecomposition::ALWAYS;
    s.quality = DecompositionQuality::HIGH;
    s.ignoreLayerMask = 0x14u;
    s.maxMoverSize = 12.0f;
    s.pushNeighbours = true;
    ConvexDecompositionCache cache("");
    const PlacementWorldSettings w = makePlacementWorldSettings(s, &cache);
    EXPECT_FLOAT_EQ(w.tuning.maxSpeed, 25.0f);
    EXPECT_EQ(w.colliders, PlacementColliderPolicy::BOTH);
    EXPECT_EQ(w.decomposition, PlacementDecomposition::ALWAYS);
    EXPECT_EQ(w.decompositionCache, &cache);
    const DecompositionParams params = DecompositionParams::fromQuality(DecompositionQuality::HIGH);
    EXPECT_EQ(w.decompositionMaxHulls, params.maxHulls);
    EXPECT_EQ(w.decompositionResolution, params.resolution);
    EXPECT_EQ(w.decompositionMaxVerticesPerHull, params.maxVerticesPerHull);
    EXPECT_EQ(w.ignoreLayerMask, 0x14u);
    EXPECT_FLOAT_EQ(w.maxMoverSize, 12.0f);
    EXPECT_TRUE(w.pushNeighbours);
    EXPECT_TRUE(std::isinf(w.killY));
    EXPECT_FLOAT_EQ(w.fixedStep, 1.0f / 120.0f);
}

TEST(PhysicsPlacement, NamesRoundTrip) {
    for (PlacementFeel f : {PlacementFeel::Precise, PlacementFeel::Balanced, PlacementFeel::Fast}) {
        PlacementFeel out = PlacementFeel::Balanced;
        ASSERT_TRUE(parsePlacementFeel(placementFeelName(f), out));
        EXPECT_EQ(out, f);
    }
    for (PlacementColliderPolicy p : {PlacementColliderPolicy::AUTO, PlacementColliderPolicy::AUTHORED, PlacementColliderPolicy::GENERATED,
                                      PlacementColliderPolicy::BOTH}) {
        PlacementColliderPolicy out = PlacementColliderPolicy::AUTO;
        ASSERT_TRUE(parsePlacementColliderPolicy(placementColliderPolicyName(p), out));
        EXPECT_EQ(out, p);
    }
    for (PlacementDecomposition d : {PlacementDecomposition::NEVER, PlacementDecomposition::CACHED, PlacementDecomposition::ALWAYS}) {
        PlacementDecomposition out = PlacementDecomposition::CACHED;
        ASSERT_TRUE(parsePlacementDecomposition(placementDecompositionName(d), out));
        EXPECT_EQ(out, d);
    }
    for (PlacementMode m : {PlacementMode::Drop, PlacementMode::Separate, PlacementMode::Move, PlacementMode::Snap, PlacementMode::Gather,
                            PlacementMode::Distribute}) {
        PlacementMode out = PlacementMode::Drop;
        ASSERT_TRUE(parsePlacementMode(placementModeName(m), out));
        EXPECT_EQ(out, m);
    }
    PlacementFeel feel;
    PlacementColliderPolicy policy;
    PlacementDecomposition decomposition;
    PlacementMode mode;
    EXPECT_FALSE(parsePlacementFeel("Balanced", feel));
    EXPECT_FALSE(parsePlacementFeel(nullptr, feel));
    EXPECT_FALSE(parsePlacementColliderPolicy("hull", policy));
    EXPECT_FALSE(parsePlacementDecomposition("", decomposition));
    EXPECT_FALSE(parsePlacementMode("settle", mode));
    EXPECT_STREQ(placementModeName(PlacementMode::Distribute), "distribute");
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

TEST(PhysicsPlacement, DecompositionCacheLivesInTheProjectAndIsDroppedOnSwitch) {
    EXPECT_EQ(placementDecompositionCache(nullptr), nullptr);
    ConvexDecompositionCache* cache = placementDecompositionCache(project());
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(fs::path(cache->getDirectory()), project()->getProjectInternalPath() / "cache" / "colliders");
    EXPECT_EQ(placementDecompositionCache(project()), cache);
    placementProjectSwitching();
    ConvexDecompositionCache* again = placementDecompositionCache(project());
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(fs::path(again->getDirectory()), project()->getProjectInternalPath() / "cache" / "colliders");
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

    // committing what is already there adds no step
    ASSERT_TRUE(commitPlacement(project(), result, error)) << error;
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
    EXPECT_EQ(error, "scene changed during simulation: run it again");

    PlacementResult gone = handMadeResult(sceneId, crate, board);
    CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, board));
    const size_t afterDelete = CommandHandle::get(sceneId)->position();
    gone.sceneStructureVersion = project()->getScene(sceneId)->structureVersion;
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

TEST(PhysicsPlacement, ResultJsonHasTheSpecShapeRoundedTo1e5) {
    const uint32_t sceneId = newScene();
    const Entity shelf = create(sceneId, "Shelf", EntityCreationType::BOX, Vector3(0.0f, 0.0f, 0.0f));
    const Entity board = create(sceneId, "Board", EntityCreationType::BOX, Vector3(0.0f, 1.0f, 0.0f), shelf);
    const Entity crate = create(sceneId, "Crate", EntityCreationType::BOX, Vector3(0.0f, 3.0f, 0.0f));
    const Entity room = create(sceneId, "Room", EntityCreationType::GAMEOBJECT, Vector3(0.0f, 0.0f, 0.0f));
    PlacementResult result = handMadeResult(sceneId, crate, board);
    result.refused = {{room, "too large (> 25 m), treated as environment"}};

    const json j = placementResultJson(project(), result, false);
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
    const std::vector<std::string> expected = {"all_settled", "applied", "context", "entities", "hint", "mode", "refused",
                                               "seed", "sim_seconds", "state_hash", "steps", "warnings"};
    EXPECT_EQ(keys, expected);
    EXPECT_EQ(j["applied"], false);
    EXPECT_EQ(j["mode"], "drop");
    EXPECT_EQ(j["seed"], 3);
    EXPECT_EQ(j["state_hash"], "00000000deadbeef");
    EXPECT_FLOAT_EQ(j["sim_seconds"].get<float>(), 2.34568f);
    EXPECT_EQ(j["steps"], 282);
    EXPECT_EQ(j["all_settled"], true);

    const json& e = j["entities"][0];
    std::vector<std::string> entityKeys;
    for (auto it = e.begin(); it != e.end(); ++it) entityKeys.push_back(it.key());
    const std::vector<std::string> expectedEntity = {"after", "before", "collider", "id", "max_penetration_m", "moved_m", "name",
                                                     "path", "resting_on", "rotated_deg", "settle_step", "settled", "warnings"};
    EXPECT_EQ(entityKeys, expectedEntity);
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
    EXPECT_NE(j["hint"].get<std::string>().find("apply:true"), std::string::npos);

    const json applied = placementResultJson(project(), result, true);
    EXPECT_EQ(applied["applied"], true);
    EXPECT_NE(applied["hint"].get<std::string>().find("one undo step"), std::string::npos);
    EXPECT_EQ(applied["hint"].get<std::string>().find("apply:true"), std::string::npos);

    PlacementResult empty;
    empty.ok = false;
    empty.error = "nothing can move";
    const json failed = placementResultJson(project(), empty, false);
    EXPECT_EQ(failed["error"], "nothing can move");
    EXPECT_TRUE(failed["context"]["region"].is_null());
    EXPECT_EQ(failed["hint"], "");
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
    const Entity parent = create(sceneId, "Shelf", EntityCreationType::GAMEOBJECT, Vector3(1.0f, 0.0f, 0.0f), NULL_ENTITY, Vector3(2.0f, 2.0f, 2.0f), yaw(90.0f));
    const Entity child = create(sceneId, "Marker", EntityCreationType::GAMEOBJECT, Vector3(1.0f, 0.0f, 0.0f), parent);
    const Entity other = create(sceneId, "Pin", EntityCreationType::GAMEOBJECT, Vector3(5.0f, 2.0f, 0.0f));
    const size_t position = CommandHandle::get(sceneId)->position();

    PlacementSession session(project(), sceneId);
    EXPECT_EQ(session.getSceneId(), sceneId);
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(session.getHud().status, "Ready");

    // meshless GameObjects are refused, with a reason, for the HUD and the labels
    session.setSelection({child, other});
    PlacementHud hud = session.getHud();
    EXPECT_EQ(hud.status, "Ready");
    ASSERT_EQ(hud.refused.size(), 2u);
    EXPECT_EQ(hud.refused[0].entity, child);
    EXPECT_FALSE(hud.refused[0].reason.empty());
    PlacementOverlay overlay;
    session.collectOverlay(overlay);
    EXPECT_FALSE(overlay.active);
    EXPECT_EQ(overlay.refused.size(), 2u);
    EXPECT_TRUE(overlay.ghosts.empty());

    // the gizmo's centre: the roots' world positions (the child through its rotated, scaled parent)
    const Vector3 childWorld = Vector3(1.0f, 0.0f, 0.0f) + yaw(90.0f) * Vector3(2.0f, 0.0f, 0.0f);
    expectNear(session.getMoversCenter(), (childWorld + Vector3(5.0f, 2.0f, 0.0f)) / 2.0f, 1e-5f);

    EXPECT_FALSE(session.begin(PlacementGesture::Move, {child, other}));
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(session.getHud().refused.size(), 2u);
    EXPECT_FALSE(session.begin(PlacementGesture::None, {child}));
    EXPECT_FALSE(session.distribute({child, other}, 5));
    EXPECT_FALSE(session.gather({child}));

    // nothing running: these do nothing, and a tick without an editor App is safe
    session.setMoveTarget(Vector3(1.0f, 0.0f, 0.0f));
    session.setRotateDelta(yaw(10.0f));
    session.setGatherScale(Vector3(0.5f, 0.5f, 0.5f));
    session.setSnapRay(Ray(Vector3(), Vector3(0.0f, -1.0f, 0.0f)));
    session.setPointerRay(Ray(Vector3(), Vector3(0.0f, -1.0f, 0.0f)));
    session.tick(1.0 / 60.0);
    session.end();
    session.cancel();
    session.notifySceneChanged();
    EXPECT_FALSE(session.isGestureActive());
    EXPECT_EQ(session.getPhase(), PlacementPhase::Idle);
    EXPECT_EQ(session.getGesture(), PlacementGesture::None);
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), position);

    PhysicsPlacementSettings settings;
    settings.feel = PlacementFeel::Precise;
    settings.showDistanceIndicators = true;
    session.setSettings(settings);
    EXPECT_EQ(session.getSettings().feel, PlacementFeel::Precise);
    session.collectOverlay(overlay);
    EXPECT_TRUE(overlay.distances.empty());   // nothing movable selected

    // a scene that is not 3D: no gesture
    project()->getScene(sceneId)->playState = ScenePlayState::PLAYING;
    EXPECT_FALSE(session.begin(PlacementGesture::Drop, {child}));
    project()->getScene(sceneId)->playState = ScenePlayState::STOPPED;

    PlacementSession nowhere(project(), 4242424242u);
    nowhere.setSelection({child});
    EXPECT_FALSE(nowhere.begin(PlacementGesture::Drop, {child}));
    nowhere.tick(0.1);
    EXPECT_EQ(nowhere.getHud().status, "Ready");
    EXPECT_TRUE(nowhere.getHud().refused.empty());
}

// ---- with the simulation (skipped while the engine's PlacementWorld is the contract stub)

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
    EXPECT_NE(result.hint.find("Settled on Floor"), std::string::npos) << result.hint;

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
    int slices = 0;
    while (!sliced.advance(0.3)) {
        slices++;
        EXPECT_NE(sliced.progressText().find(" asleep"), std::string::npos);
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
    EXPECT_NE(result.hint.find("Separated 2 objects"), std::string::npos) << result.hint;

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

    PlacementSpec gather = specFor(sceneId, {a, c}, PlacementMode::Gather);
    gather.amount = 0.5f;
    gather.settle = false;
    const PlacementResult gathered = runToEnd(gather);
    ASSERT_TRUE(gathered.ok) << gathered.error;
    EXPECT_NEAR(entityResult(gathered, a)->after.position.x, 1.5f, 0.02f);
    EXPECT_NEAR(entityResult(gathered, c)->after.position.x, 4.5f, 0.02f);
    EXPECT_NEAR(entityResult(gathered, a)->after.position.y, 0.5f, 0.01f);   // across gravity only
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
    EXPECT_TRUE(contains(fell.warnings, "nothing to land on within"));
    EXPECT_TRUE(contains(fell.warnings, "fell out of the world"));
    EXPECT_TRUE(entityResult(fell, crate)->fellOut);
    EXPECT_FALSE(fell.allSettled);

    makeFloor(sceneId);
    PlacementSpec hurried = specFor(sceneId, {crate});
    hurried.maxSeconds = 0.1f;
    const PlacementResult quick = runToEnd(hurried);
    EXPECT_TRUE(contains(quick.warnings, "Crate never came to rest within 0.1 s"));
    EXPECT_FALSE(quick.allSettled);
    EXPECT_NE(quick.hint.find("0 of 1 settled"), std::string::npos) << quick.hint;
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
    session.setMoveTarget(session.getMoversCenter() + Vector3(8.0f, 0.0f, 0.0f));
    session.tick(1.0 / 60.0);
    session.collectOverlay(overlay);
    EXPECT_TRUE(overlay.active);
    EXPECT_TRUE(overlay.showTeleportRing);
    EXPECT_TRUE(overlay.teleportExceeded);
    EXPECT_FLOAT_EQ(overlay.teleportRadius, 5.0f);
    EXPECT_FALSE(overlay.region.isNull());
    EXPECT_TRUE(overlay.distances.empty());   // idle selection only
    session.cancel();
}
