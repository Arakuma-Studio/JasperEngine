// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je-bench-late-transforms: the late transform pass and the on-demand world pose
// (docs/specs/S95-lua-runtime-gaps.md REQ-33, REQ-35, AC-9).
//
//   je-bench-late-transforms [--json <file>] [--count-allocs]
//
// A scene of 10 000 static Transforms (100 roots of 99 children) and one camera, no GPU.
// Reported (median of runs, microseconds): RenderSystem::update of that scene with nothing moved
// (the main pass a frame runs when the camera moves only after it), the late pass with nothing
// moved since (REQ-33's idle cost) and with the camera moved after the update (REQ-33's
// one-camera case; the MVPs it leaves are made by the draw's pass, not counted here), its share of
// the main pass, and one moved object's late pass (information); a follow camera's late pass with
// its target still and moved after the update. REQ-35: getWorldPosition's
// pose of a clean Transform 8 deep and of one under a moved parent (8 compositions), with
// --count-allocs their allocations and the late passes'. tools/perf/check_bench.py gates the JSON
// with tools/perf/bench-thresholds.json.

#include "BenchUtil.h"
#include "AllocationCounter.h"
#include "JobsTestSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/CameraFollowComponent.h"
#include "component/Transform.h"
#include "subsystem/CameraControlSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TransformPropagation.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int kRoots = 100;
    constexpr int kChildren = 99;
    constexpr int kRuns = 201;

    volatile float sink = 0.0f;

    Entity transformEntity(Scene& scene, Entity parent, const Vector3& position){
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.getComponent<Transform>(entity).position = position;
        if (parent != NULL_ENTITY)
            scene.addEntityChild(parent, entity, false);
        return entity;
    }

    // the median of kRuns calls of `body` (after 20 warm-ups), microseconds
    template<typename Body>
    double medianUs(Body&& body){
        for (int i = 0; i < 20; i++) body(i);
        std::vector<double> samples;
        samples.reserve(kRuns);
        for (int i = 0; i < kRuns; i++){
            const auto start = bench::Clock::now();
            body(i);
            samples.push_back(bench::secondsSince(start) * 1.0e6);
        }
        return bench::percentile(samples, 0.5);
    }

}

int main(int argc, char** argv){
    bool countAllocs = false;
    for (int i = 1; i < argc; i++){
        if (std::string(argv[i]) == "--count-allocs") countAllocs = true;
    }
    if (countAllocs && !testsupport::countsAllocations()){
        std::fprintf(stderr, "--count-allocs: this build cannot count allocations\n");
        return 2;
    }
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-late-transforms");

    Scene scene;
    for (int r = 0; r < kRoots; r++){
        const Entity root = transformEntity(scene, NULL_ENTITY, Vector3((float)r, 0, 0));
        for (int c = 0; c < kChildren; c++)
            transformEntity(scene, root, Vector3(0, (float)c, 1));
    }
    const Entity camera = transformEntity(scene, NULL_ENTITY, Vector3(0, 0, 10));
    CameraComponent cameraComponent;
    cameraComponent.type = CameraType::CAMERA_PERSPECTIVE;
    cameraComponent.useTarget = false;
    scene.addComponent<CameraComponent>(camera, cameraComponent);
    scene.setCamera(camera);
    const Entity mover = scene.getComponentArray<Transform>()->getEntity(kChildren);   // the first root's last child
    RenderSystem& render = *scene.getSystem<RenderSystem>();
    results.add("scene_transforms", (double)scene.getComponentArray<Transform>()->size(), "transforms");

    auto moveCamera = [&](int i){
        Transform& transform = scene.getComponent<Transform>(camera);
        transform.position = Vector3(0.001f * (float)(i % 1000), 0, 10);
        transform.needUpdate = true;
        scene.markChanged<Transform>(camera);
    };
    auto moveObject = [&](int i){
        Transform& transform = scene.getComponent<Transform>(mover);
        transform.position = Vector3(0, 0.001f * (float)(i % 1000), 1);
        transform.needUpdate = true;
        scene.markChanged<Transform>(mover);
    };

    for (int i = 0; i < 3; i++) render.update(1.0 / 60.0);

    // the frame's main pass, nothing moved before it
    const double mainUs = medianUs([&](int){ render.update(1.0 / 60.0); });
    results.add("main_pass_static_us", mainUs, "us");
    // the same pass when the camera moved before it (onUpdate): what a late move saves the next frame
    results.add("main_pass_camera_moved_us", medianUs([&](int i){ moveCamera(i); render.update(1.0 / 60.0); }), "us");

    // REQ-33: nothing moved after the update: one compare
    {
        render.update(1.0 / 60.0);
        const int calls = 100000;
        const auto start = bench::Clock::now();
        for (int i = 0; i < calls; i++) sink = sink + (render.updateLateTransforms().moved ? 1.0f : 0.0f);
        results.add("late_idle_us", bench::secondsSince(start) * 1.0e6 / calls, "us");
    }

    // REQ-33: the camera moved in onPostUpdate, after a main pass where nothing moved
    std::vector<double> late;
    late.reserve(kRuns);
    for (int i = 0; i < kRuns + 20; i++){
        render.update(1.0 / 60.0);
        moveCamera(i);
        const auto start = bench::Clock::now();
        render.updateLateTransforms();
        if (i >= 20) late.push_back(bench::secondsSince(start) * 1.0e6);
    }
    const double lateCameraUs = bench::percentile(late, 0.5);
    results.add("late_camera_moved_us", lateCameraUs, "us");
    results.add("late_camera_ratio", lateCameraUs / mainUs, "of the main pass");

    // one object moved after the update (information)
    late.clear();
    for (int i = 0; i < kRuns + 20; i++){
        render.update(1.0 / 60.0);
        moveObject(i);
        const auto start = bench::Clock::now();
        render.updateLateTransforms();
        if (i >= 20) late.push_back(bench::secondsSince(start) * 1.0e6);
    }
    results.add("late_one_object_moved_us", bench::percentile(late, 0.5), "us");

    if (countAllocs){
        // the late passes' own (the main pass between them is not counted)
        uint64_t allocations = 0;
        for (int i = 0; i < 600; i++){
            render.update(1.0 / 60.0);
            if (i % 2) moveCamera(i); else moveObject(i);
            testsupport::ThreadAllocationCount count;
            render.updateLateTransforms();
            render.updateLateTransforms();
            allocations += count.count();
        }
        results.add("late_steady_allocations", (double)allocations, "allocations");
    }

    // a follow camera (Camera Follow, rigid): the late pass with its target not moved after the
    // update (the idle cost a follow-camera game pays each frame), and with it moved (the follower
    // stepped again, then the camera's matrices)
    {
        Scene follow;
        const Entity target = transformEntity(follow, NULL_ENTITY, Vector3(0, 0, 0));
        const Entity eye = transformEntity(follow, NULL_ENTITY, Vector3(0, 0, 10));
        CameraComponent eyeCamera;
        eyeCamera.type = CameraType::CAMERA_PERSPECTIVE;
        eyeCamera.useTarget = false;
        follow.addComponent<CameraComponent>(eye, eyeCamera);
        follow.setCamera(eye);
        CameraFollowComponent followComponent;
        followComponent.target = target;
        followComponent.offset = Vector3(0, 0, 10);
        followComponent.damping = Vector3(0, 0, 0);
        followComponent.lookAt = false;
        follow.addComponent<CameraFollowComponent>(eye, followComponent);
        RenderSystem& followRender = *follow.getSystem<RenderSystem>();
        CameraControlSystem& controls = *follow.getSystem<CameraControlSystem>();
        auto frame = [&](){
            controls.step(1.0f / 60.0f, 1.0f / 60.0f);
            followRender.update(1.0 / 60.0);
        };
        auto moveTarget = [&](int i){
            Transform& transform = follow.getComponent<Transform>(target);
            transform.position = Vector3(0.001f * (float)(i % 1000), 0, 0);
            transform.needUpdate = true;
            follow.markChanged<Transform>(target);
        };
        for (int i = 0; i < 3; i++) frame();
        frame();
        const int calls = 100000;
        const auto start = bench::Clock::now();
        for (int i = 0; i < calls; i++) sink = sink + (followRender.updateLateTransforms().moved ? 1.0f : 0.0f);
        results.add("late_idle_follow_us", bench::secondsSince(start) * 1.0e6 / calls, "us");
        std::vector<double> moved;
        uint64_t allocations = 0;
        for (int i = 0; i < kRuns + 20; i++){
            frame();
            moveTarget(i);
            testsupport::ThreadAllocationCount count;
            const auto begin = bench::Clock::now();
            followRender.updateLateTransforms();
            const double us = bench::secondsSince(begin) * 1.0e6;
            const uint64_t allocated = count.count();
            if (i >= 20){
                moved.push_back(us);
                allocations += allocated;
            }
        }
        results.add("late_follow_target_moved_us", bench::percentile(moved, 0.5), "us");
        if (countAllocs)
            results.add("late_follow_allocations", (double)allocations, "allocations");
    }

    // REQ-35: a Transform 8 deep, clean, then under its moved root (8 compositions)
    {
        Scene chain;
        Entity parent = NULL_ENTITY;
        Entity root = NULL_ENTITY;
        for (int d = 0; d < 8; d++){
            parent = transformEntity(chain, parent, Vector3(1, 0.5f, 0));
            if (d == 0) root = parent;
            chain.getComponent<Transform>(parent).rotation = Quaternion(10.0f * (float)d, Vector3(0, 1, 0));
        }
        const Entity leaf = parent;
        updateWorldTransforms(chain);
        auto transforms = chain.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) transforms->getComponentFromIndex(i).needUpdate = false;

        results.add("world_pose_clean_depth8_us", medianUs([&](int){
            for (int k = 0; k < 1000; k++) sink = sink + computeWorldPose(chain, leaf).position.x;
        }) / 1000.0, "us");
        chain.getComponent<Transform>(root).needUpdate = true;
        results.add("world_pose_dirty_depth8_us", medianUs([&](int){
            for (int k = 0; k < 1000; k++) sink = sink + computeWorldPose(chain, leaf).position.x;
        }) / 1000.0, "us");
        if (countAllocs){
            testsupport::AllocationCountScope scope;
            for (int k = 0; k < 1000; k++) sink = sink + computeWorldPose(chain, leaf).position.x;
            chain.getComponent<Transform>(root).needUpdate = false;
            for (int k = 0; k < 1000; k++) sink = sink + computeWorldPose(chain, leaf).position.x;
            results.add("world_pose_allocations", (double)scope.count(), "allocations");
        }
    }

    const char* path = bench::jsonPath(argc, argv, "late-transforms.json");
    if (!results.write(path)) std::fprintf(stderr, "could not write %s\n", path);
    Engine::systemShutdown();
    return 0;
}
