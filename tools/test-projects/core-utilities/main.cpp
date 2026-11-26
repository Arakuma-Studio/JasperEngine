// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S09's test project: the log of C++ and of Lua (lua/main.lua, with a Timer and a coroutine)
// reaches the platform's log with its level and category, in order. Checked in a browser
// (WEB_PROJECT=core-utilities tools/build.sh web, GATE=core-utilities tools/build.sh web-gate) and
// on an Android emulator (ANDROID_PROJECT=core-utilities tools/build.sh android, then
// tools/android-emulator-test.sh with tools/test-projects/core-utilities/android-expect.txt).
//
// culling-group-and-simulation-distance AC17 in the same build: a scene layer with a 10,000-sphere
// Culling Group and one with 1,000 Simulation Culling components. From 4 s of game time (after
// main.lua's Performance line, so the gate's ordered lines do not race) 200 frames of each scene's
// evaluation time give the p99, on a Release build (BUILD_TYPE=Release: R22's figures are release
// ones), printed with R22's web budget (0.5 ms the group, 0.25 ms the simulation). Over the budget is
// a warning, not a failure: R22's are device figures, and CI's browser renders in software (its p50
// alone is over them); wall-clock budgets are reported, never asserted (AGENTS.md). Then the
// scripted camera move: the group's camera turns away (every sphere becomes
// invisible, no band changes: 10,000 events) and the simulation camera moves 30 m back (the far
// 500 pass the distance: 500 culled once the 64-a-frame pacing has applied them).

#include "JasperEngine.h"

#include <algorithm>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int SPHERES = 10000;
    constexpr int COMPONENTS = 1000;
    constexpr double START_SECONDS = 4.0;
    constexpr int SAMPLES = 200;
    // frames after the move before the counts are read: 500 transitions at 64 a frame take 8
    constexpr int SETTLE_FRAMES = 12;
    constexpr double GROUP_BUDGET_MS = 0.5;
    constexpr double SIMULATION_BUDGET_MS = 0.25;

    Scene scene;
    Scene groupScene;
    Camera groupCamera(&groupScene);
    Scene simulationScene;
    Camera simulationCamera(&simulationScene);
    CullingGroup group;

    std::vector<double> groupTimes;
    std::vector<double> simulationTimes;
    int movedFrames = -1;
    int events = 0;
    int becameInvisible = 0;
    bool done = false;

    // sorts them: the p99, and the median the line also prints (load or a slower kernel)
    double p99(std::vector<double>& times){
        std::sort(times.begin(), times.end());
        return times[(size_t)(times.size() * 0.99)];
    }

    void gateBudget(const char* name, const std::vector<double>& sorted, double value, double budget){
        Log::print("[gate] %s p99 %.4f ms (p50 %.4f) over %d frames, web budget %g ms", name, value, sorted[sorted.size() / 2], SAMPLES,
                   budget);
        if (value <= budget){
            Log::print("[gate] %s p99 within %g ms", name, budget);
        }else{
            Log::warn("[gate] %s p99 %.4f ms over the %g ms web budget (a device figure: this browser may render in software)", name,
                      value, budget);
        }
    }

    void setUpCulling(){
        groupCamera.setType(CameraType::CAMERA_PERSPECTIVE);
        groupCamera.setPosition(0.0f, 0.0f, 0.0f);
        groupCamera.setTarget(Vector3(0.0f, 0.0f, -1.0f));
        groupScene.setCamera(&groupCamera);
        Engine::addSceneLayer(&groupScene);

        // 100 x 100 spheres 30 to 129 m ahead, inside the frustum, across bands 1 to 3
        group = CullingGroup::create(&groupScene);
        const float distances[] = {25.0f, 50.0f, 100.0f, 200.0f};
        group.setBoundingDistances(distances);
        group.setDistanceReferencePoint(Vector3(0.0f, 0.0f, 0.0f));
        std::vector<BoundingSphere> spheres(SPHERES);
        for (int i = 0; i < SPHERES; i++){
            spheres[(size_t)i] = BoundingSphere(Vector3((static_cast<float>(i % 100) - 49.5f) * 0.2f, 0.0f, -30.0f - static_cast<float>(i / 100)), 0.1f);
        }
        group.setBoundingSpheres(spheres);
        group.onStateChanged().add("coreUtilitiesCullingEvents", [](CullingGroupEvent event){
            if (movedFrames < 0){
                return;
            }
            events++;
            becameInvisible += event.hasBecomeInvisible() ? 1 : 0;
        });

        simulationCamera.setType(CameraType::CAMERA_PERSPECTIVE);
        simulationCamera.setPosition(0.0f, 0.0f, 0.0f);
        simulationCamera.setTarget(Vector3(0.0f, 0.0f, -1.0f));
        simulationScene.setCamera(&simulationCamera);
        Engine::addSceneLayer(&simulationScene);

        // 500 GameObjects 10 to 15 m from the camera and 500 at 30 to 35 m, 50 m simulation distance
        for (int i = 0; i < COMPONENTS; i++){
            const Entity entity = simulationScene.createEntity();
            Transform transform;
            transform.position = Vector3(0.0f, 0.0f, (i < COMPONENTS / 2 ? -10.0f : -30.0f) - static_cast<float>(i % (COMPONENTS / 2)) * 0.01f);
            simulationScene.addComponent<Transform>(entity, transform);
            SimulationCullingComponent component;
            component.simulationDistance = 50.0f;
            component.boundsRadius = 0.5f;
            component.simulateWhenVisible = false;
            component.freezeWhenGroundNotLoaded = false;
            simulationScene.addComponent<SimulationCullingComponent>(entity, component);
        }

        groupTimes.reserve(SAMPLES);
        simulationTimes.reserve(SAMPLES);
        Engine::onUpdate.add("coreUtilitiesCulling", [](){
            if (done || Engine::getGameTime() < START_SECONDS){
                return;
            }
            if ((int)groupTimes.size() < SAMPLES){
                // the evaluations of the frame before
                groupTimes.push_back(groupScene.getSystem<CullingSystem>()->getStats().evaluateMs);
                simulationTimes.push_back(simulationScene.getSystem<CullingSystem>()->getStats().evaluateMs);
                return;
            }
            if (movedFrames < 0){
                const double groupP99 = p99(groupTimes);
                gateBudget("culling group", groupTimes, groupP99, GROUP_BUDGET_MS);
                const double simulationP99 = p99(simulationTimes);
                gateBudget("simulation culling", simulationTimes, simulationP99, SIMULATION_BUDGET_MS);
                groupCamera.setTarget(Vector3(0.0f, 0.0f, 1.0f));
                simulationCamera.setPosition(0.0f, 0.0f, 30.0f);
                movedFrames = 0;
                return;
            }
            if (++movedFrames < SETTLE_FRAMES){
                return;
            }
            done = true;
            const CullingSystem::Stats simulation = simulationScene.getSystem<CullingSystem>()->getStats();
            Log::print("[gate] culling events %d (%d became invisible), simulation full %u culled %u after the camera move",
                       events, becameInvisible, simulation.full, simulation.culled);
            if (events == SPHERES && becameInvisible == SPHERES && simulation.culled == COMPONENTS / 2 && simulation.full == COMPONENTS / 2){
                Log::print("[gate] culling events as expected after the camera move");
            }else{
                Log::error("[gate] culling events: expected %d (all becoming invisible) and %d culled", SPHERES, COMPONENTS / 2);
            }
        });
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);

    Log::print("[gate] cpp info 2");
    Log::warn("[gate] cpp warning 3");
    Log::error("[gate] cpp error 4");
    JE_LOG_WARN(logCategoryRender(), "[gate] category warning 5");

    setUpCulling();
}
