// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Decompose Into Convex Hulls (docs/specs/S96-physics-gaps.md REQ-20, D7) with several running at
// once: each job decomposes on a worker while the main thread starts the others, and each lands as
// its own undo step. The list of running decompositions is the main thread's; a job that read it
// (to find itself) raced the next start's push_back, which ThreadSanitizer reports
// (SANITIZE=thread, the nightly's TSan lane). Slow: three V-HACD runs at once (six took 6 s on a CI
// runner, 31 s under ASan).

#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/DecomposeColliderCmd.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // The App's side of enqueueMainThreadTask: tasks wait for the next editor frame
    struct DecomposeLoopHost : EditorHost {
        std::mutex mutex;
        std::vector<std::function<void()>> tasks;

        void enqueueMainThreadTask(std::function<void()> task) override {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
        }
        void processMainThreadTasks() override {
            std::vector<std::function<void()>> now;
            {
                std::lock_guard<std::mutex> lock(mutex);
                now.swap(tasks);
            }
            for (std::function<void()>& task : now) task();
        }
    };

    // a coarse torus (concave: V-HACD gives it several hulls), GameObject-local triangles
    void decomposeTorus(std::vector<Vector3>& vertices, std::vector<uint32_t>& indices) {
        constexpr int MAJOR = 12, MINOR = 6;
        constexpr float PI = 3.14159265358979323846f;
        for (int i = 0; i < MAJOR; i++) {
            const float u = 2.0f * PI * (float)i / (float)MAJOR;
            for (int j = 0; j < MINOR; j++) {
                const float v = 2.0f * PI * (float)j / (float)MINOR;
                const float ring = 1.0f + 0.3f * std::cos(v);
                vertices.emplace_back(ring * std::cos(u), 0.3f * std::sin(v), ring * std::sin(u));
            }
        }
        for (int i = 0; i < MAJOR; i++) {
            for (int j = 0; j < MINOR; j++) {
                const uint32_t a = (uint32_t)(i * MINOR + j);
                const uint32_t b = (uint32_t)(((i + 1) % MAJOR) * MINOR + j);
                const uint32_t c = (uint32_t)(((i + 1) % MAJOR) * MINOR + (j + 1) % MINOR);
                const uint32_t d = (uint32_t)(i * MINOR + (j + 1) % MINOR);
                indices.insert(indices.end(), {a, b, c, a, c, d});
            }
        }
    }

    class DecomposeColliderSlow : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"decompose-collider"};
        DecomposeLoopHost host;
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        std::vector<Entity> rings;
        // what each ring's decomposition gave (its `finished` call, on the main thread)
        std::vector<ColliderDecomposition::Outcome> outcomes;
        std::vector<bool> landed;

        // a decomposition is in the list from its start until it lands
        bool anyRunning() const {
            for (Entity ring : rings) {
                if (ColliderDecomposition::find(scene->id, ring, 0)) return true;
            }
            return false;
        }

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            setEditorHost(&host);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) {
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
        }

        void TearDown() override {
            // what still runs (a failed test) lands on a live project
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (scene && anyRunning() && std::chrono::steady_clock::now() < deadline) {
                host.processMainThreadTasks();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (project) {
                for (SceneProject& candidate : project->getScenes()) {
                    CommandHandle::remove(candidate.id);
                }
                project->releaseScenes();
            }
            project.reset();
            setEditorHost(nullptr);
        }

        Entity addRing(int n) {
            Scene* s = scene->scene;
            const Entity entity = s->createUserEntity();
            s->setEntityName(entity, "Ring " + std::to_string(n));
            Transform transform;
            transform.position = Vector3(3.0f * (float)n, 2.0f, 0.0f);
            s->addComponent<Transform>(entity, transform);
            RigidbodyComponent body;
            Collider3D collider;
            collider.type = Collider3DType::MESH;
            collider.source = Collider3DSource::RAW_MESH;
            collider.density = 500.0f;
            decomposeTorus(collider.vertices, collider.indices);
            body.colliders.push_back(collider);
            s->addComponent<RigidbodyComponent>(entity, body);
            scene->entities.push_back(entity);
            return entity;
        }
    };

}

TEST_F(DecomposeColliderSlow, DecompositionsStartedTogetherEachLandAsOneStep) {
    ASSERT_TRUE(ConvexDecompositionCache::available()) << "the editor test build has V-HACD";
    constexpr int RINGS = 3;
    for (int n = 0; n < RINGS; n++) rings.push_back(addRing(n));
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t stepsBefore = history->appliedCount();

    // all started before any lands: every start after the first pushes to the list while the
    // earlier jobs run
    outcomes.assign(RINGS, {});
    landed.assign(RINGS, false);
    std::vector<std::shared_ptr<ColliderDecomposition>> running;
    for (int n = 0; n < RINGS; n++) {
        ColliderDecomposition::Request request;
        request.sceneId = scene->id;
        request.entity = rings[n];
        request.quality = DecompositionQuality::LOW;
        request.maxHulls = 8;
        std::string error;
        running.push_back(ColliderDecomposition::start(project.get(), request, [this, n](const ColliderDecomposition::Outcome& outcome) {
            outcomes[n] = outcome;
            landed[n] = true;
        }, error));
        ASSERT_NE(running.back(), nullptr) << error;
        EXPECT_EQ(ColliderDecomposition::find(scene->id, rings[n], 0), running.back());
    }
    std::string error;
    ColliderDecomposition::Request again;
    again.sceneId = scene->id;
    again.entity = rings[0];
    EXPECT_EQ(ColliderDecomposition::start(project.get(), again, nullptr, error), nullptr);
    EXPECT_NE(error.find("being decomposed already"), std::string::npos) << error;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto allLanded = [this]() {
        for (bool done : landed) if (!done) return false;
        return true;
    };
    while (!allLanded() && std::chrono::steady_clock::now() < deadline) {
        host.processMainThreadTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(allLanded()) << "every decomposition lands within 120 s";

    for (int n = 0; n < RINGS; n++) {
        EXPECT_EQ(outcomes[n].error, "") << "ring " << n;
        EXPECT_GE(outcomes[n].hulls, 2u) << "ring " << n;
        EXPECT_EQ(outcomes[n].undoName, "Decompose Collider (Ring " + std::to_string(n) + ")");
        EXPECT_EQ(ColliderDecomposition::find(scene->id, rings[n], 0), nullptr) << "ring " << n << " left the list";
        const RigidbodyComponent& body = scene->scene->getComponent<RigidbodyComponent>(rings[n]);
        EXPECT_EQ(body.colliders.size(), outcomes[n].hulls);
        for (const Collider3D& collider : body.colliders) {
            EXPECT_EQ(collider.type, Collider3DType::CONVEX_HULL);
            EXPECT_FLOAT_EQ(collider.density, 500.0f);
        }
    }
    EXPECT_EQ(history->appliedCount(), stepsBefore + RINGS) << "one undo step each";
}
