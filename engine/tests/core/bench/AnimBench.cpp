// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je-bench-anim: the Animator's budgets (docs/specs/S21-animation.md R19-R20, AC12).
//
//   je-bench-anim [--json <file>] [--workers <n>] [--count-allocs] [--frames <n>]
//
// AnimBench.Crowd200: 200 skinned 65-bone humanoids (Mixamo names), all animating: per character
// a Freeform Cartesian blend tree of 5 clips (a quarter of them crossfading between two such
// states the whole time), an upper-body override layer with an Avatar Mask, an additive layer,
// Foot IK on both legs (raycasts against a static ground box, Jolt), a Look At over 3 bones, root
// motion, clip events, writeBones Exposed (4 bones) and the skin matrices. Reported: the
// AnimatorSystem's wall time per frame (p50 / p95) with the workers and serial, the main
// thread's share (prepare + finalize), and with --count-allocs the heap allocations of the
// steady-state frames.
// AnimBench.Micro*: a two-pose blend at 128 bones (Native and Scalar), one 65-bone clip sample,
// a state-machine update (20 states, 60 transitions, no pose), an Animator's runtime state, a
// Look At solve over 3 bones of a leaning chain (S74 R33, information).
// Each is the median of N runs. tools/perf/check_bench.py gates the JSON with
// tools/perf/bench-thresholds.json.

#include "BenchUtil.h"
#include "AllocationCounter.h"
#include "JobsTestSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorIK.h"
#include "animation/AnimatorInstance.h"
#include "animation/AnimatorPose.h"
#include "animation/AnimatorRig.h"
#include "animation/CompiledController.h"
#include "component/AnimatorComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "subsystem/AnimatorSystem.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;
namespace fs = std::filesystem;

namespace {

    int kWorkers = 3;
    int kFrames = 600;
    constexpr float kDt = 1.0f / 60.0f;
    constexpr int kCharacters = 200;

    // ---- the 65-bone humanoid (Mixamo names) ----

    struct BoneSpec{
        std::string name;
        int parent;
        Vector3 position;
    };

    std::vector<BoneSpec> humanoidBones(){
        std::vector<BoneSpec> b;
        auto add = [&](const std::string& name, int parent, Vector3 p){ b.push_back({name, parent, p}); return (int)b.size() - 1; };
        const int hips = add("mixamorig:Hips", -1, Vector3(0, 0.95f, 0));
        const int spine = add("mixamorig:Spine", hips, Vector3(0, 0.1f, 0));
        const int spine1 = add("mixamorig:Spine1", spine, Vector3(0, 0.12f, 0));
        const int spine2 = add("mixamorig:Spine2", spine1, Vector3(0, 0.13f, 0));
        const int neck = add("mixamorig:Neck", spine2, Vector3(0, 0.18f, 0));
        const int head = add("mixamorig:Head", neck, Vector3(0, 0.08f, 0));
        add("mixamorig:HeadTop_End", head, Vector3(0, 0.19f, 0));
        for (int side = 0; side < 2; side++){
            const char* s = side == 0 ? "Left" : "Right";
            const float x = side == 0 ? 1.0f : -1.0f;
            const int shoulder = add(std::string("mixamorig:") + s + "Shoulder", spine2, Vector3(0.06f * x, 0.12f, 0));
            const int arm = add(std::string("mixamorig:") + s + "Arm", shoulder, Vector3(0.12f * x, 0, 0));
            const int fore = add(std::string("mixamorig:") + s + "ForeArm", arm, Vector3(0.28f * x, 0, 0));
            const int hand = add(std::string("mixamorig:") + s + "Hand", fore, Vector3(0.26f * x, 0, 0));
            const char* fingers[5] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
            for (int f = 0; f < 5; f++){
                int parent = hand;
                for (int j = 1; j <= 4; j++){
                    const Vector3 p = j == 1 ? Vector3(0.08f * x, 0, 0.03f - 0.015f * (float)f) : Vector3(0.03f * x, 0, 0);
                    parent = add(std::string("mixamorig:") + s + "Hand" + fingers[f] + std::to_string(j), parent, p);
                }
            }
            const int up = add(std::string("mixamorig:") + s + "UpLeg", hips, Vector3(0.09f * x, -0.05f, 0));
            const int leg = add(std::string("mixamorig:") + s + "Leg", up, Vector3(0, -0.42f, 0));
            const int foot = add(std::string("mixamorig:") + s + "Foot", leg, Vector3(0, -0.4f, 0));
            const int toe = add(std::string("mixamorig:") + s + "ToeBase", foot, Vector3(0, -0.05f, 0.12f));
            add(std::string("mixamorig:") + s + "Toe_End", toe, Vector3(0, 0, 0.06f));
        }
        return b;
    }

    std::vector<std::string> bonePaths(const std::vector<BoneSpec>& bones){
        std::vector<std::string> paths(bones.size());
        for (size_t i = 0; i < bones.size(); i++){
            paths[i] = bones[i].parent < 0 ? bones[i].name : paths[(size_t)bones[i].parent] + "/" + bones[i].name;
        }
        return paths;
    }

    // ---- assets in a temporary folder ----

    fs::path assetDir;

    std::string writeFile(const std::string& name, const std::string& text){
        const fs::path path = assetDir / name;
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        return path.generic_string();
    }

    // a 1 s loop of every bone's rotation (30 keys a second, a swing of `amplitude` degrees with
    // its own phase) and the hips moving `forward` m/s: what a mocap clip costs to sample
    ClipData motionClip(const std::vector<std::string>& paths, float amplitude, float phase, float forward, bool hipsMotion,
                        const std::vector<size_t>& only = {}){
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        data.frameRate = 30.0f;
        for (size_t b = 0; b < paths.size(); b++){
            if (!only.empty() && std::find(only.begin(), only.end(), b) == only.end()) continue;
            ClipTrackData track;
            track.path = paths[b];
            track.channel = ClipChannel::Rotation;
            for (int k = 0; k <= 30; k++){
                const float t = (float)k / 30.0f;
                const float angle = amplitude * 0.0174532925f * std::sin(6.2831853f * t + phase + 0.37f * (float)b);
                const Vector3 axis = Vector3((float)(b % 3 == 0), (float)(b % 3 == 1), (float)(b % 3 == 2));
                ClipKey key;
                key.time = t;
                key.value = {axis.x * std::sin(0.5f * angle), axis.y * std::sin(0.5f * angle), axis.z * std::sin(0.5f * angle), std::cos(0.5f * angle)};
                track.keys.push_back(key);
            }
            data.tracks.push_back(track);
        }
        if (hipsMotion){
            ClipTrackData hips;
            hips.path = paths[0];
            hips.channel = ClipChannel::Position;
            for (int k = 0; k <= 30; k++){
                const float t = (float)k / 30.0f;
                ClipKey key;
                key.time = t;
                key.value = {0.0f, 0.95f + 0.02f * std::sin(12.566f * t), forward * t};
                hips.keys.push_back(key);
            }
            data.tracks.push_back(hips);
            ClipEventData step;
            step.time = 0.5f;
            step.function = "onStep";
            data.events.push_back(step);
        }
        return data;
    }

    struct Assets{
        std::string controller;
        std::vector<std::string> paths;
    };

    Assets writeAssets(const std::vector<BoneSpec>& bones){
        Assets out;
        out.paths = bonePaths(bones);
        const std::string idle = writeFile("idle.anim", writeClip(motionClip(out.paths, 4, 0.0f, 0.0f, true)));
        const std::string fwd = writeFile("fwd.anim", writeClip(motionClip(out.paths, 18, 0.5f, 1.4f, true)));
        const std::string back = writeFile("back.anim", writeClip(motionClip(out.paths, 16, 1.0f, -1.0f, true)));
        const std::string left = writeFile("left.anim", writeClip(motionClip(out.paths, 14, 1.5f, 0.3f, true)));
        const std::string right = writeFile("right.anim", writeClip(motionClip(out.paths, 14, 2.0f, 0.3f, true)));
        std::vector<size_t> upper, spineOnly;
        for (size_t b = 0; b < bones.size(); b++){
            if (out.paths[b].find("Shoulder") != std::string::npos || out.paths[b].find("Neck") != std::string::npos) upper.push_back(b);
            if (bones[b].name.find("Spine") != std::string::npos) spineOnly.push_back(b);
        }
        const std::string wave = writeFile("wave.anim", writeClip(motionClip(out.paths, 30, 0.2f, 0.0f, false, upper)));
        const std::string breath = writeFile("breath.anim", writeClip(motionClip(out.paths, 3, 0.0f, 0.0f, false, spineOnly)));

        AvatarMaskDef mask;
        mask.defaultWeight = 0.0f;
        for (std::optional<float>& part : mask.bodyParts) part = 0.0f;
        mask.bodyParts[(int)AvatarBodyPart::Head] = 1.0f;
        mask.bodyParts[(int)AvatarBodyPart::LeftArm] = mask.bodyParts[(int)AvatarBodyPart::RightArm] = 1.0f;
        mask.bodyParts[(int)AvatarBodyPart::LeftHand] = mask.bodyParts[(int)AvatarBodyPart::RightHand] = 1.0f;
        mask.bodyParts[(int)AvatarBodyPart::LeftFingers] = mask.bodyParts[(int)AvatarBodyPart::RightFingers] = 1.0f;
        const std::string maskPath = writeFile("upper.mask", writeMask(mask));

        ControllerDef def;
        auto parameter = [&](const char* name, AnimatorParameterType type){
            ParameterDef p;
            p.name = name;
            p.type = type;
            def.parameters.push_back(p);
        };
        parameter("X", AnimatorParameterType::Float);
        parameter("Y", AnimatorParameterType::Float);
        parameter("Alt", AnimatorParameterType::Bool);
        auto tree = [&](){
            MotionDef motion;
            BlendTreeDef t;
            t.type = BlendTreeType::FreeformCartesian2D;
            t.parameter = "X";
            t.parameterY = "Y";
            const std::pair<std::string, Vector2> children[] = {{idle, Vector2(0, 0)}, {fwd, Vector2(0, 1)}, {back, Vector2(0, -1)},
                                                                {left, Vector2(-1, 0)}, {right, Vector2(1, 0)}};
            for (const auto& entry : children){
                BlendChildDef child;
                child.motion.clip.path = entry.first;
                child.position = entry.second;
                t.children.push_back(child);
            }
            motion.tree.push_back(t);
            return motion;
        };
        auto conditionOn = [](bool on){
            ConditionDef c;
            c.parameter = "Alt";
            c.mode = on ? AnimatorConditionMode::If : AnimatorConditionMode::IfNot;
            return c;
        };
        LayerDef base;
        base.name = "Base Layer";
        StateDef move;
        move.name = "Move";
        move.motion = tree();
        StateDef move2 = move;
        move2.name = "Move2";
        move2.speed = 1.1f;
        TransitionDef toAlt;
        toAlt.to = "Move2";
        toAlt.conditions = {conditionOn(true)};
        // a crossfade that lasts the whole run: a quarter of the crowd blends two trees
        toAlt.duration = 1000.0f;
        toAlt.fixedDuration = true;
        move.transitions.push_back(toAlt);
        TransitionDef back2 = toAlt;
        back2.to = "Move";
        back2.conditions = {conditionOn(false)};
        move2.transitions.push_back(back2);
        base.machine.states = {move, move2};
        def.layers.push_back(base);

        LayerDef upperLayer;
        upperLayer.name = "Upper";
        upperLayer.weight = 0.7f;
        upperLayer.mask.path = maskPath;
        StateDef waving;
        waving.name = "Wave";
        waving.motion.clip.path = wave;
        upperLayer.machine.states = {waving};
        def.layers.push_back(upperLayer);

        LayerDef additive;
        additive.name = "Breath";
        additive.blending = AnimatorLayerBlending::Additive;
        StateDef breathing;
        breathing.name = "Breath";
        breathing.motion.clip.path = breath;
        additive.machine.states = {breathing};
        def.layers.push_back(additive);
        normalizeController(def);
        out.controller = writeFile("crowd.controller", writeController(def));
        return out;
    }

    // ---- the crowd ----

    struct Crowd{
        std::unique_ptr<Scene> scene;
        std::vector<Entity> roots;
        Entity lookTarget = NULL_ENTITY;

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            RenderSystem* render = scene->getSystem<RenderSystem>().get();
            for (size_t i = 0; i < transforms->size(); i++){
                render->updateTransform(transforms->getComponentFromIndex(i), transforms->getEntity(i));
            }
        }
    };

    Entity node(Scene& scene, Entity parent, const std::string& name, const Vector3& position){
        const Entity entity = scene.createEntity();
        scene.setEntityName(entity, name);
        Transform transform;
        transform.position = position;
        scene.addComponent<Transform>(entity, transform);
        if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
        return entity;
    }

    std::unique_ptr<Crowd> buildCrowd(const Assets& assets, const std::vector<BoneSpec>& bones){
        auto crowd = std::make_unique<Crowd>();
        crowd->scene = std::make_unique<Scene>();
        Scene& scene = *crowd->scene;
        // the ground: a static 400 m box whose top is y = 0
        {
            const Entity ground = node(scene, NULL_ENTITY, "Ground", Vector3(0, -0.5f, 0));
            RigidbodyComponent body;
            body.type = BodyType::STATIC;
            body.colliders.resize(1);
            body.colliders[0].type = Collider3DType::BOX;
            body.colliders[0].width = 400.0f;
            body.colliders[0].height = 1.0f;
            body.colliders[0].depth = 400.0f;
            scene.addComponent<RigidbodyComponent>(ground, body);
        }
        crowd->lookTarget = node(scene, NULL_ENTITY, "LookTarget", Vector3(0, 1.6f, 50.0f));
        for (int c = 0; c < kCharacters; c++){
            const Entity root = node(scene, NULL_ENTITY, "Hero" + std::to_string(c), Vector3((float)(c % 20) * 1.5f - 15.0f, 0, (float)(c / 20) * 1.5f));
            std::vector<Entity> entities(bones.size());
            for (size_t b = 0; b < bones.size(); b++){
                entities[b] = node(scene, bones[b].parent < 0 ? root : entities[(size_t)bones[b].parent], bones[b].name, bones[b].position);
            }
            // a skin over every bone (its mesh under the root)
            const Entity mesh = node(scene, root, "HeroMesh", Vector3(0, 0, 0));
            scene.addComponent<MeshComponent>(mesh, {});
            ModelComponent model;
            ModelSkinBinding skin;
            skin.mesh = mesh;
            skin.joints = entities;
            std::vector<Matrix4> world(bones.size());
            for (size_t b = 0; b < bones.size(); b++){
                const Matrix4 local = Matrix4::translateMatrix(bones[b].position);
                world[b] = bones[b].parent < 0 ? local : world[(size_t)bones[b].parent] * local;
                skin.inverseBindMatrices.push_back(world[b].inverse());
            }
            model.skinBindings.push_back(skin);
            scene.addComponent<ModelComponent>(root, model);
            scene.getComponent<ModelComponent>(root).needUpdateModel = false;

            AnimatorComponent animator;
            animator.controller = assets.controller;
            animator.applyRootMotion = true;
            animator.writeBones = AnimatorWriteBones::Exposed;
            animator.exposedBones = {"left_hand", "right_hand", "head", "hips"};
            animator.eventMode = AnimatorEventMode::LeaderOnly;
            for (int side = 0; side < 2; side++){
                AnimatorIKConstraint foot;
                foot.name = side == 0 ? "LeftFoot" : "RightFoot";
                foot.type = AnimatorIKType::FootIK;
                foot.bones = side == 0 ? std::vector<std::string>{"left_upper_leg", "left_lower_leg", "left_foot"}
                                       : std::vector<std::string>{"right_upper_leg", "right_lower_leg", "right_foot"};
                foot.pelvis = "hips";
                foot.footHeight = 0.05f;
                animator.ik.push_back(foot);
            }
            AnimatorIKConstraint look;
            look.name = "Look";
            look.type = AnimatorIKType::LookAt;
            look.bones = {"upper_chest", "neck", "head"};
            look.boneWeights = {0.2f, 0.3f, 0.5f};
            look.target = crowd->lookTarget;
            animator.ik.push_back(look);
            animator.needsLoad = true;
            scene.addComponent<AnimatorComponent>(root, animator);
            crowd->roots.push_back(root);
        }
        crowd->resolveTransforms();
        // the ground body exists before the first raycast
        scene.fixedUpdate(kDt);
        AnimatorSystem* system = scene.getSystem<AnimatorSystem>().get();
        system->update(0.0);
        // parameters: each its own place in the tree; a quarter crossfading for good
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        for (int c = 0; c < kCharacters; c++){
            AnimatorInstance* instance = scene.getComponent<AnimatorComponent>(crowd->roots[(size_t)c]).runtime.get();
            if (!instance || !instance->bound){
                std::fprintf(stderr, "character %d is not bound: %s %s\n", c, instance ? instance->bindError.c_str() : "no runtime",
                             instance ? instance->controllerError.c_str() : "");
                std::exit(2);
            }
            instance->setFloat("X", unit(rng));
            instance->setFloat("Y", unit(rng));
            instance->setBool("Alt", c % 4 == 0);
        }
        return crowd;
    }

    struct CrowdTimes{
        double p50 = 0.0, p95 = 0.0, mainP95 = 0.0;
        uint64_t allocations = 0;
        size_t sampled = 0;
    };

    CrowdTimes runCrowd(Crowd& crowd, bool parallel, int frames, bool countAllocs){
        AnimatorSystem* system = crowd.scene->getSystem<AnimatorSystem>().get();
        system->setParallel(parallel);
        // warm-up: every buffer at its size, the transitions running
        for (int i = 0; i < 120; i++){
            system->update(kDt);
            crowd.resolveTransforms();
        }
        std::vector<double> times, mains;
        times.reserve((size_t)frames);
        mains.reserve((size_t)frames);
        CrowdTimes out;
        std::unique_ptr<testsupport::AllocationCountScope> scope;
        uint64_t allocations = 0;
        for (int i = 0; i < frames; i++){
            crowd.resolveTransforms();
            if (countAllocs) scope = std::make_unique<testsupport::AllocationCountScope>();
            const auto start = bench::Clock::now();
            system->update(kDt);
            const double ms = bench::secondsSince(start) * 1000.0;
            if (scope){
                allocations += scope->count();
                scope.reset();
            }
            times.push_back(ms);
            mains.push_back(system->getStats().prepareMs + system->getStats().finalizeMs);
        }
        out.p50 = bench::percentile(times, 0.5);
        out.p95 = bench::percentile(times, 0.95);
        out.mainP95 = bench::percentile(mains, 0.95);
        out.allocations = allocations;
        out.sampled = system->getStats().sampled;
        return out;
    }

    // ---- micro benchmarks: the median of `runs` runs of `iterations` calls, per call in us ----

    template <class F>
    double microUs(F&& f, int iterations, int runs = 15){
        std::vector<double> samples;
        for (int r = 0; r < runs; r++){
            const auto start = bench::Clock::now();
            for (int i = 0; i < iterations; i++) f(i);
            samples.push_back(bench::secondsSince(start) * 1e6 / (double)iterations);
        }
        return bench::percentile(samples, 0.5);
    }

    volatile float sink = 0.0f;

    void randomPose(Pose& pose, std::mt19937& rng){
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        for (size_t b = 0; b < pose.boneCount(); b++){
            pose.setPosition(b, Vector3(unit(rng), unit(rng), unit(rng)));
            Quaternion q(unit(rng), unit(rng), unit(rng), unit(rng));
            q.normalize();
            pose.setRotation(b, q);
            pose.setScale(b, Vector3(1, 1, 1));
        }
    }

    // a 20-state controller with 60 transitions (3 per state, float and int conditions)
    ControllerDef stateMachine(){
        ControllerDef def;
        ParameterDef speed;
        speed.name = "Speed";
        def.parameters.push_back(speed);
        ParameterDef mode;
        mode.name = "Mode";
        mode.type = AnimatorParameterType::Int;
        def.parameters.push_back(mode);
        LayerDef layer;
        layer.name = "Base Layer";
        for (int s = 0; s < 20; s++){
            StateDef state;
            state.name = "S" + std::to_string(s);
            for (int t = 1; t <= 3; t++){
                TransitionDef transition;
                transition.to = "S" + std::to_string((s + t) % 20);
                ConditionDef a;
                a.parameter = "Speed";
                a.mode = AnimatorConditionMode::Greater;
                a.threshold = 10.0f + (float)t;     // never true: every transition is checked each update
                ConditionDef b;
                b.parameter = "Mode";
                b.mode = AnimatorConditionMode::Equals;
                b.threshold = (float)(s * 3 + t);
                transition.conditions = {a, b};
                state.transitions.push_back(transition);
            }
            layer.machine.states.push_back(state);
        }
        def.layers.push_back(layer);
        normalizeController(def);
        return def;
    }

}

int main(int argc, char** argv){
    bool countAllocs = false;
    for (int i = 1; i < argc; i++){
        const std::string arg = argv[i];
        if (arg == "--workers" && i + 1 < argc) kWorkers = std::max(0, std::atoi(argv[++i]));
        else if (arg == "--frames" && i + 1 < argc) kFrames = std::max(60, std::atoi(argv[++i]));
        else if (arg == "--count-allocs") countAllocs = true;
    }
    if (countAllocs && !testsupport::countsAllocations()){
        std::fprintf(stderr, "--count-allocs: this build cannot count allocations\n");
        return 2;
    }
    jobs::Config config;
    config.workers = kWorkers;
    config.reserveForFrame = 0;
    config.detectWaitCycles = false;
    jobs::initialize(config);
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    std::printf("je-bench-anim: %d workers + main, %u hardware threads, pose ops %s\n", jobs::system().getWorkerCount(),
                std::thread::hardware_concurrency(), poseNativeBackendName());
    bench::Results results("je-bench-anim");

    assetDir = fs::temp_directory_path() / ("je-bench-anim-" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(assetDir);
    const std::vector<BoneSpec> bones = humanoidBones();
    const Assets assets = writeAssets(bones);
    results.add("crowd_bones_per_character", (double)bones.size(), "bones");

    // ---- AnimBench.Crowd200
    {
        std::unique_ptr<Crowd> crowd = buildCrowd(assets, bones);
        const CrowdTimes parallel = runCrowd(*crowd, true, kFrames, false);
        results.add("crowd200_sampled", (double)parallel.sampled, "animators");
        results.add("crowd200_parallel_p50_ms", parallel.p50, "ms");
        results.add("crowd200_parallel_p95_ms", parallel.p95, "ms");
        results.add("crowd200_main_thread_p95_ms", parallel.mainP95, "ms");
        const CrowdTimes serial = runCrowd(*crowd, false, std::max(120, kFrames / 3), false);
        results.add("crowd200_serial_p50_ms", serial.p50, "ms");
        results.add("crowd200_serial_p95_ms", serial.p95, "ms");
        results.add("crowd200_parallel_speedup", serial.p50 / std::max(parallel.p50, 1e-9), "x");
        if (countAllocs){
            const CrowdTimes counted = runCrowd(*crowd, true, kFrames, true);
            results.add("crowd200_steady_state_allocations", (double)counted.allocations, "allocations");
        }
        const AnimatorInstance* instance = crowd->scene->getComponent<AnimatorComponent>(crowd->roots[0]).runtime.get();
        results.add("micro_instance_state_bytes", (double)instance->runtimeStateBytes(), "B");
        // where the serial time goes (information): without the IK, then without the two upper
        // layers too, then without the crossfades too
        auto each = [&](auto&& change){
            for (Entity root : crowd->roots) change(crowd->scene->getComponent<AnimatorComponent>(root));
        };
        each([](AnimatorComponent& c){ for (AnimatorIKConstraint& k : c.ik) k.enabled = k.type != AnimatorIKType::LookAt; });
        results.add("crowd200_serial_no_look_at_p50_ms", runCrowd(*crowd, false, 120, false).p50, "ms");
        each([](AnimatorComponent& c){ for (AnimatorIKConstraint& k : c.ik) k.enabled = false; });
        results.add("crowd200_serial_no_ik_p50_ms", runCrowd(*crowd, false, 120, false).p50, "ms");
        each([](AnimatorComponent& c){ c.runtime->setLayerWeight(1, 0.0f); c.runtime->setLayerWeight(2, 0.0f); });
        results.add("crowd200_serial_base_layer_p50_ms", runCrowd(*crowd, false, 120, false).p50, "ms");
        each([](AnimatorComponent& c){ c.runtime->setBool("Alt", false); c.runtime->play("Move", 0, 0.0f); });
        results.add("crowd200_serial_one_tree_p50_ms", runCrowd(*crowd, false, 120, false).p50, "ms");
        each([](AnimatorComponent& c){ c.writeBones = AnimatorWriteBones::Exposed; c.applyRootMotion = false; });
        results.add("crowd200_serial_no_root_motion_p50_ms", runCrowd(*crowd, false, 120, false).p50, "ms");
    }

    // ---- AnimBench.Micro: a two-pose blend at 128 bones, Native and Scalar
    {
        std::mt19937 rng(3);
        Pose a, b, out;
        a.resize(128, 0);
        b.resize(128, 0);
        out.resize(128, 0);
        randomPose(a, rng);
        randomPose(b, rng);
        auto blend = [&](int i){
            poseBlend(out, a, b, 0.3f + 0.0001f * (float)(i & 63), nullptr, nullptr);
            sink = out.raw()[i & 127];
        };
        setPoseOpsBackend(PoseBackend::Native);
        const double native = microUs(blend, 20000);
        setPoseOpsBackend(PoseBackend::Scalar);
        const double scalar = microUs(blend, 20000);
        setPoseOpsBackend(PoseBackend::Native);
        results.add("micro_blend128_us", native, "us");
        results.add("micro_blend128_scalar_us", scalar, "us");
        results.add("micro_blend128_simd_speedup", scalar / std::max(native, 1e-9), "x");
    }

    // ---- one 65-bone clip sample (the bench character's forward clip, bound to its rig)
    {
        Scene scene;
        const Entity root = node(scene, NULL_ENTITY, "Hero", Vector3(0, 0, 0));
        std::vector<Entity> entities(bones.size());
        for (size_t b = 0; b < bones.size(); b++){
            entities[b] = node(scene, bones[b].parent < 0 ? root : entities[(size_t)bones[b].parent], bones[b].name, bones[b].position);
        }
        std::string error;
        std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad((assetDir / "fwd.anim").generic_string(), &error);
        if (!clip){
            std::fprintf(stderr, "fwd.anim: %s\n", error.c_str());
            return 2;
        }
        std::vector<std::string> paths;
        for (const AnimationClip::Track& track : clip->getTracks()) paths.push_back(track.path);
        AnimatorRig rig;
        AnimatorRestMap rest;
        rig.build(&scene, root, paths, {}, rest);
        RigClipBinding binding;
        binding.bind(clip, rig);
        RigClipCursors cursors;
        cursors.resize(binding.groups.size());
        Pose pose;
        pose.resize(rig.bones.size(), rig.channels.size());
        pose.copyFrom(rig.rest);
        const double sample = microUs([&](int i){
            sampleClip(binding, rig, (float)(i % 997) * (1.0f / 997.0f), cursors, pose);
            sink = pose.raw()[i & 63];
        }, 4000);
        results.add("micro_sample65_us", sample, "us");
    }

    // ---- a state-machine update: 20 states, 60 transitions, no pose
    {
        std::shared_ptr<const CompiledController> compiled = compileController(stateMachine());
        AnimatorInstance instance;
        AnimatorComponent component;
        Scene scene;
        const Entity root = node(scene, NULL_ENTITY, "Machine", Vector3(0, 0, 0));
        instance.setController(compiled, 1, component.clipOverrides);
        instance.bind(&scene, root, component, {});
        AnimatorUpdateContext context;
        context.dt = kDt;
        context.sample = false;
        for (int i = 0; i < 10; i++) instance.update(component, context);
        const double update = microUs([&](int){ instance.update(component, context); }, 20000);
        results.add("micro_state_machine_update_us", update, "us");
    }

    // ---- a Look At solve: upper chest, neck and head of a chain leaning 30 degrees, the target
    // going round it; each call starts from the animated pose, as in an Animator
    {
        constexpr int kBones = 3;
        Pose animated, local;
        animated.resize(kBones, 0);
        const float half = 15.0f * 0.0174532925f;
        animated.setRotation(0, Quaternion(std::cos(half), std::sin(half), 0, 0));
        animated.setPosition(1, Vector3(0, 0.18f, 0));
        animated.setPosition(2, Vector3(0, 0.08f, 0));
        local.resize(kBones, 0);
        const int parents[kBones] = {-1, 0, 1};
        Affine world[kBones];
        Vector3 positions[kBones], scales[kBones];
        Quaternion rotations[kBones];
        IKContext context{&local, parents, RigSpace{world, positions, rotations, scales}};
        const int chain[kBones] = {0, 1, 2};
        const float weights[kBones] = {0.2f, 0.3f, 0.5f};
        const double lookAt = microUs([&](int i){
            local.copyFrom(animated);
            refreshRigSpace(context, 0);
            const float angle = (float)(i % 360) * 0.0174532925f;
            solveLookAt(context, chain, weights, kBones, Vector3(0, 0, 1), Vector3(std::sin(angle), 0.6f, std::cos(angle)), 60.0f, 1.0f);
            sink = rotations[2].w;
        }, 20000);
        results.add("micro_look_at3_us", lookAt, "us");
    }

    const char* path = bench::jsonPath(argc, argv, "anim.json");
    if (!results.write(path)) std::fprintf(stderr, "could not write %s\n", path);
    std::error_code ec;
    fs::remove_all(assetDir, ec);
    Engine::systemShutdown();
    return 0;
}
