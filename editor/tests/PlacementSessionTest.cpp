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
