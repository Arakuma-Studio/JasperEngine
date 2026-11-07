// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je-bench-anim: the Animator's budgets (docs/specs/S21-animation.md R19-R20, AC12).
//
//   je-bench-anim [--json <file>] [--workers <n>] [--count-allocs] [--frames <n>] [--clip <file.anim>]...
//                 [--fixtures <dir of hero_animated.gltf, hero_tpose.gltf>]
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
// a state-machine update (20 states, 60 transitions, no pose), an Animator's runtime state.
// Each is the median of N runs. tools/perf/check_bench.py gates the JSON with
// tools/perf/bench-thresholds.json.
// S74 R10, information unless the thresholds name them (docs/perf/animator.md): Web30 (30 of the
// same characters, one thread), an Animation Player crowd, a retargeted crowd, rig space and skin
// matrices alone, key groups with different times, sampleQuaternion against Quaternion::slerp per
// tier, and for the dense and reduced clip shapes (generated here) and each --clip file (real
// clips, kept out of the repository) the sample, sample + hierarchy, parse + build and
// AnimationClip::memoryBytes().
// On WebAssembly (tools/perf/bench-anim-wasm.sh) the same program, under another bench name.

#include "BenchUtil.h"
#include "AllocationCounter.h"
#include "JobsTestSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorInstance.h"
#include "animation/AnimatorPose.h"
#include "animation/AnimatorRig.h"
#include "animation/CompiledController.h"
#include "animation/KeyframeSampling.h"
#include "component/AnimatorComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "object/AnimationPlayer.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AnimatorSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
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

    std::unique_ptr<Crowd> buildCrowd(const Assets& assets, const std::vector<BoneSpec>& bones, int characters = kCharacters){
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
        for (int c = 0; c < characters; c++){
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
        for (int c = 0; c < characters; c++){
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


    // ---- S74 R10: clips, as bytes and as sampling cost

    // A rig made from a clip's own paths ("A/B/C" under a root), the clip bound to it
    struct ClipRig{
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        std::shared_ptr<const AnimationClip> clip;
        AnimatorRig rig;
        AnimatorRestMap rest;
        RigClipBinding binding;
        RigClipCursors cursors;
        Pose pose;
        std::vector<Affine> world;
        std::vector<Vector3> positions, scales;
        std::vector<Quaternion> rotations;

        explicit ClipRig(const std::shared_ptr<const AnimationClip>& loaded): clip(loaded){
            const Entity root = node(*scene, NULL_ENTITY, "Root", Vector3(0, 0, 0));
            std::vector<std::pair<std::string, Entity>> made;
            std::vector<std::string> paths;
            for (const AnimationClip::Track& track : clip->getTracks()){
                if (track.channel > ClipChannel::Scale || track.path.empty()) continue;
                paths.push_back(track.path);
                std::string path;
                Entity parent = root;
                std::stringstream parts(track.path);
                std::string part;
                while (std::getline(parts, part, '/')){
                    path += (path.empty() ? "" : "/") + part;
                    auto found = std::find_if(made.begin(), made.end(), [&](const auto& entry){ return entry.first == path; });
                    if (found == made.end()){
                        made.push_back({path, node(*scene, parent, part, Vector3(0, 0, 0))});
                        parent = made.back().second;
                    }else{
                        parent = found->second;
                    }
                }
            }
            rig.build(scene.get(), root, paths, {}, rest);
            binding.bind(clip, rig);
            cursors.resize(binding.groups.size());
            pose.resize(rig.bones.size(), rig.channels.size());
            pose.copyFrom(rig.rest);
            world.resize(rig.bones.size());
            positions.resize(rig.bones.size());
            scales.resize(rig.bones.size());
            rotations.resize(rig.bones.size());
        }
    };

    size_t keyCount(const AnimationClip& clip){
        size_t keys = 0;
        for (const AnimationClip::Track& track : clip.getTracks()) keys += track.keyCount;
        return keys;
    }

    // the rows of one clip, from its text: <name>_{tracks,keys,parse_build_ms,heap_bytes,sample_us,sample_hierarchy_us}
    void clipRows(bench::Results& results, const std::string& name, const std::string& text){
        std::vector<double> parseBuild;
        for (int r = 0; r < 5; r++){
            const auto start = bench::Clock::now();
            anim::ClipParseResult parsed = anim::parseClip(text);
            std::shared_ptr<const AnimationClip> built = parsed.ok() ? AnimationClip::build(parsed) : nullptr;
            parseBuild.push_back(bench::secondsSince(start) * 1000.0);
            if (!built){
                std::fprintf(stderr, "%s: %s\n", name.c_str(), parsed.error.toString().c_str());
                return;
            }
        }
        anim::ClipParseResult parsed = anim::parseClip(text);
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(parsed);
        results.add(name + "_tracks", (double)clip->getTracks().size(), "tracks");
        results.add(name + "_keys", (double)keyCount(*clip), "keys");
        results.add(name + "_parse_build_ms", bench::percentile(parseBuild, 0.5), "ms");
        results.add(name + "_text_bytes", (double)text.size(), "B");
        results.add(name + "_heap_bytes", (double)clip->memoryBytes(), "B");
        ClipRig bound(clip);
        const float length = std::max(clip->getLength(), 1e-3f);
        const int iterations = std::max(50, std::min(4000, 2000000 / std::max(1, (int)bound.binding.targets.size() * 8)));
        results.add(name + "_sample_us", microUs([&](int i){
            sampleClip(bound.binding, bound.rig, length * (float)(i % 997) * (1.0f / 997.0f), bound.cursors, bound.pose);
            sink = bound.pose.raw()[i & 63];
        }, iterations), "us");
        RigSpace space{bound.world.data(), bound.positions.data(), bound.rotations.data(), bound.scales.data()};
        results.add(name + "_sample_hierarchy_us", microUs([&](int i){
            sampleClip(bound.binding, bound.rig, length * (float)(i % 997) * (1.0f / 997.0f), bound.cursors, bound.pose);
            poseToRigSpace(bound.pose, bound.rig.parents.data(), space);
            sink = bound.positions[(size_t)i % bound.positions.size()].x;
        }, iterations), "us");
    }

    // A clip of R42's shapes (test shapes, not assets): `bones` bones in a tree, translation,
    // rotation and scale tracks, `totalKeys` keys over 2 s. `sharedTimes`: every track on the same
    // keys (a dense bake); otherwise each track's keys have their own times (key reduction's output)
    ClipData shapedClip(size_t bones, size_t totalKeys, bool sharedTimes){
        std::vector<std::string> paths(bones);
        for (size_t b = 0; b < bones; b++){
            const std::string name = "b" + std::to_string(b);
            paths[b] = b == 0 ? name : paths[(b - 1) / 2] + "/" + name;
        }
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        data.frameRate = 30.0f;
        const size_t tracks = bones * 3;
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (size_t t = 0; t < tracks; t++){
            const size_t bone = t / 3;
            const size_t keys = totalKeys / tracks + (t < totalKeys % tracks ? 1 : 0);
            ClipTrackData track;
            track.path = paths[bone];
            track.channel = t % 3 == 0 ? ClipChannel::Rotation : (t % 3 == 1 ? ClipChannel::Position : ClipChannel::Scale);
            std::vector<float> times;
            for (size_t k = 0; k < keys; k++){
                const float even = keys > 1 ? 2.0f * (float)k / (float)(keys - 1) : 0.0f;
                const float jitter = sharedTimes || k == 0 || k + 1 == keys ? 0.0f : (unit(rng) - 0.5f) * 1.5f / (float)(keys - 1);
                times.push_back(even + jitter);
            }
            for (size_t k = 0; k < keys; k++){
                ClipKey key;
                key.time = times[k];
                const float phase = 3.14159265f * times[k] + 0.37f * (float)bone;
                if (track.channel == ClipChannel::Rotation){
                    const float half = 0.5f * 0.4f * std::sin(phase);
                    key.value = {std::sin(half), 0.0f, 0.0f, std::cos(half)};
                }else if (track.channel == ClipChannel::Position){
                    key.value = {0.02f * std::sin(phase), 0.1f, 0.02f * std::cos(phase)};
                }else{
                    key.value = {1.0f, 1.0f + 0.01f * std::sin(phase), 1.0f};
                }
                track.keys.push_back(key);
            }
            data.tracks.push_back(track);
        }
        return data;
    }

    // motionClip with the bones' keys on `groups` different time grids: no key search is shared
    // between groups (RigClipBinding::groups)
    ClipData groupedClip(const std::vector<std::string>& paths, int groups){
        ClipData data = motionClip(paths, 18, 0.5f, 0.0f, false);
        for (size_t t = 0; t < data.tracks.size(); t++){
            const int group = (int)(t % (size_t)groups);
            std::vector<ClipKey>& keys = data.tracks[t].keys;
            for (size_t k = 1; k + 1 < keys.size(); k++) keys[k].time += 0.01f * (float)group / (float)groups;
        }
        return data;
    }

    Matrix4 affineMatrix(const Affine& a){
        Matrix4 m;
        m[0][0] = a.m[0]; m[0][1] = a.m[1]; m[0][2] = a.m[2]; m[0][3] = 0.0f;
        m[1][0] = a.m[3]; m[1][1] = a.m[4]; m[1][2] = a.m[5]; m[1][3] = 0.0f;
        m[2][0] = a.m[6]; m[2][1] = a.m[7]; m[2][2] = a.m[8]; m[2][3] = 0.0f;
        m[3][0] = a.m[9]; m[3][1] = a.m[10]; m[3][2] = a.m[11]; m[3][3] = 1.0f;
        return m;
    }

    // pairs of keys `degrees` apart (random axes): one tier of S74 R12 each
    std::vector<std::pair<Quaternion, Quaternion>> keyPairs(float degrees){
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::vector<std::pair<Quaternion, Quaternion>> pairs;
        for (int i = 0; i < 256; i++){
            Quaternion a(unit(rng), unit(rng), unit(rng), unit(rng));
            a.normalize();
            Vector3 axis(unit(rng), unit(rng), unit(rng));
            axis.normalize();
            const float half = 0.5f * degrees * 0.0174532925f;
            const Quaternion turn(std::cos(half), axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half));
            pairs.push_back({a, (a * turn).normalized()});
        }
        return pairs;
    }

    double sampleQuaternionNs(const std::vector<std::pair<Quaternion, Quaternion>>& pairs){
        return 1000.0 * microUs([&](int i){
            const std::pair<Quaternion, Quaternion>& pair = pairs[(size_t)i & 255];
            const Quaternion values[2] = {pair.first, pair.second};
            KeyLocation at;
            at.index = 1;
            at.interpolation = (float)(i & 63) * (1.0f / 64.0f) + 0.0078125f;
            sink = sampleQuaternion(at, values, nullptr, nullptr).w;
        }, 200000);
    }

    double slerpNs(const std::vector<std::pair<Quaternion, Quaternion>>& pairs){
        return 1000.0 * microUs([&](int i){
            const std::pair<Quaternion, Quaternion>& pair = pairs[(size_t)i & 255];
            sink = Quaternion::slerp((float)(i & 63) * (1.0f / 64.0f) + 0.0078125f, pair.first, pair.second).w;
        }, 200000);
    }

    // ---- S74 R10: the crowds no Animator row covers

    // characters with the humanoid hierarchy and an Animation Player (playing `clip`, or idle)
    struct PlayerCrowd{
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();

        double step(){
            const auto start = bench::Clock::now();
            scene->getSystem<ActionSystem>()->update(kDt);
            const double ms = bench::secondsSince(start) * 1000.0;
            auto transforms = scene->getComponentArray<Transform>();
            RenderSystem* render = scene->getSystem<RenderSystem>().get();
            for (size_t i = 0; i < transforms->size(); i++){
                render->updateTransform(transforms->getComponentFromIndex(i), transforms->getEntity(i));
            }
            return ms;
        }
    };

    std::unique_ptr<PlayerCrowd> buildPlayerCrowd(const std::vector<BoneSpec>& bones, const std::string& clip, int characters, bool playing){
        auto crowd = std::make_unique<PlayerCrowd>();
        Scene& scene = *crowd->scene;
        for (int c = 0; c < characters; c++){
            const Entity root = node(scene, NULL_ENTITY, "Hero" + std::to_string(c), Vector3((float)(c % 20), 0, (float)(c / 20)));
            std::vector<Entity> entities(bones.size());
            for (size_t b = 0; b < bones.size(); b++){
                entities[b] = node(scene, bones[b].parent < 0 ? root : entities[(size_t)bones[b].parent], bones[b].name, bones[b].position);
            }
            AnimationPlayer player(&scene, root);
            player.addClip(clip, "Move");
            player.setPlayAutomatically(playing);
        }
        return crowd;
    }

    void playerCrowdRows(bench::Results& results, const std::string& name, PlayerCrowd& crowd, int frames, bool countAllocs){
        for (int i = 0; i < 120; i++) crowd.step();
        std::vector<double> times;
        times.reserve((size_t)frames);
        for (int i = 0; i < frames; i++) times.push_back(crowd.step());
        results.add(name + "_p50_ms", bench::percentile(times, 0.5), "ms");
        results.add(name + "_p95_ms", bench::percentile(times, 0.95), "ms");
        if (countAllocs){
            uint64_t allocations = 0;
            for (int i = 0; i < frames; i++){
                testsupport::AllocationCountScope scope;
                crowd.scene->getSystem<ActionSystem>()->update(kDt);
                allocations += scope.count();
            }
            results.add(name + "_steady_state_allocations", (double)allocations, "allocations");
        }
    }

    // `characters` hero_tpose models playing hero_animated's Walk through the one retarget path
    void retargetCrowdRows(bench::Results& results, const std::string& fixtures, int characters, int frames){
        const std::string model = fixtures + "/hero_tpose.gltf";
        if (!fs::exists(model)){
            std::fprintf(stderr, "no retargeted crowd: %s is missing (--fixtures)\n", model.c_str());
            return;
        }
        ControllerDef def;
        LayerDef layer;
        layer.name = "Base Layer";
        StateDef walk;
        walk.name = "Walk";
        walk.motion.clip.path = fixtures + "/hero_animated.gltf#Walk";
        layer.machine.states = {walk};
        def.layers.push_back(layer);
        normalizeController(def);
        const std::string controller = writeFile("retarget.controller", writeController(def));

        Scene scene;
        std::vector<std::unique_ptr<Model>> models;
        for (int c = 0; c < characters; c++){
            models.push_back(std::make_unique<Model>(&scene));
            const Entity entity = models.back()->getEntity();
            if (!scene.getSystem<MeshSystem>()->loadGLTF(entity, model, false, false, false)){
                std::fprintf(stderr, "no retargeted crowd: cannot load %s\n", model.c_str());
                return;
            }
            scene.getComponent<ModelComponent>(entity).needUpdateModel = false;
            scene.getComponent<Transform>(entity).position = Vector3((float)(c % 10), 0, (float)(c / 10));
            AnimatorComponent animator;
            animator.controller = controller;
            animator.needsLoad = true;
            scene.addComponent<AnimatorComponent>(entity, animator);
        }
        AnimatorSystem* system = scene.getSystem<AnimatorSystem>().get();
        system->setParallel(false);
        system->update(0.0);
        int retargeted = 0;
        for (const std::unique_ptr<Model>& entry : models){
            const AnimatorInstance* instance = scene.getComponent<AnimatorComponent>(entry->getEntity()).runtime.get();
            retargeted += instance && instance->bound && !instance->clips.empty() && instance->clips[0].retargeted ? 1 : 0;
        }
        results.add("retarget_crowd_retargeted", (double)retargeted, "animators");
        for (int i = 0; i < 120; i++) system->update(kDt);
        std::vector<double> times;
        times.reserve((size_t)frames);
        for (int i = 0; i < frames; i++){
            const auto start = bench::Clock::now();
            system->update(kDt);
            times.push_back(bench::secondsSince(start) * 1000.0);
        }
        results.add("retarget_crowd_serial_p50_ms", bench::percentile(times, 0.5), "ms");
        results.add("retarget_crowd_serial_p95_ms", bench::percentile(times, 0.95), "ms");
    }

}

int main(int argc, char** argv){
    bool countAllocs = false;
    std::vector<std::string> clipFiles;
#ifdef JASPER_TEST_REPO_DIR
    std::string fixtures = JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models";
#else
    std::string fixtures;
#endif
    for (int i = 1; i < argc; i++){
        const std::string arg = argv[i];
        if (arg == "--workers" && i + 1 < argc) kWorkers = std::max(0, std::atoi(argv[++i]));
        else if (arg == "--frames" && i + 1 < argc) kFrames = std::max(60, std::atoi(argv[++i]));
        else if (arg == "--count-allocs") countAllocs = true;
        else if (arg == "--clip" && i + 1 < argc) clipFiles.push_back(argv[++i]);
        else if (arg == "--fixtures" && i + 1 < argc) fixtures = argv[++i];
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
#ifdef __EMSCRIPTEN__
    bench::Results results("je-bench-anim-wasm");
#else
    bench::Results results("je-bench-anim");
#endif

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
        // rig space and skin matrices alone, of a pose the crowd evaluated (S74 R10)
        {
            const AnimatorRig& rig = instance->rig;
            std::vector<Affine> world(rig.bones.size());
            std::vector<Vector3> positions(rig.bones.size()), scales(rig.bones.size());
            std::vector<Quaternion> rotations(rig.bones.size());
            RigSpace space{world.data(), positions.data(), rotations.data(), scales.data()};
            results.add("micro_rig_space65_us", microUs([&](int i){
                poseToRigSpace(instance->pose(), rig.parents.data(), space);
                sink = positions[(size_t)i % positions.size()].x;
            }, 4000), "us");
            const RigSkin& skin = rig.skins[0];
            const size_t skinned = std::min(skin.joints.size(), skin.inverseBindMatrices.size());
            std::vector<Matrix4> matrices(skinned);
            const Matrix4 inverseMesh;
            results.add("micro_skin_matrices65_us", microUs([&](int i){
                for (size_t j = 0; j < skinned; j++){
                    matrices[j] = inverseMesh * affineMatrix(world[(size_t)skin.joints[j]]) * skin.inverseBindMatrices[j];
                }
                sink = matrices[(size_t)i % skinned][3][1];
            }, 4000), "us");
        }
        // where the serial time goes (information): without the IK, then without the two upper
        // layers too, then without the crossfades too
        auto each = [&](auto&& change){
            for (Entity root : crowd->roots) change(crowd->scene->getComponent<AnimatorComponent>(root));
        };
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

    // ---- S74 R10: Web30, 30 of the crowd's characters on one thread (the owner's web budget, R44)
    {
        std::unique_ptr<Crowd> crowd = buildCrowd(assets, bones, 30);
        const CrowdTimes serial = runCrowd(*crowd, false, kFrames, countAllocs);
        results.add("web30_serial_p50_ms", serial.p50, "ms");
        results.add("web30_serial_p95_ms", serial.p95, "ms");
        results.add("web30_main_thread_p95_ms", serial.mainP95, "ms");
        if (countAllocs) results.add("web30_steady_state_allocations", (double)serial.allocations, "allocations");
    }
    // what the character's clips hold (R44's clip memory: a game's whole clip set stays under it)
    {
        size_t total = 0;
        for (const char* name : {"idle", "fwd", "back", "left", "right", "wave", "breath"}){
            std::ifstream file(assetDir / (std::string(name) + ".anim"), std::ios::binary);
            std::stringstream text;
            text << file.rdbuf();
            anim::ClipParseResult parsed = anim::parseClip(text.str());
            total += AnimationClip::build(parsed)->memoryBytes();
        }
        results.add("web30_clip_heap_bytes", (double)total, "B");
    }

    // ---- S74 R10: key groups with different times (one key search per group), then each tier of
    // the rotation sampler against the exact slerp it replaces
    {
        const std::vector<std::string> paths = bonePaths(bones);
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(groupedClip(paths, 8));
        ClipRig bound(clip);
        results.add("micro_sample65_key_groups", (double)bound.binding.groups.size(), "groups");
        results.add("micro_sample65_key_groups_us", microUs([&](int i){
            sampleClip(bound.binding, bound.rig, (float)(i % 997) * (1.0f / 997.0f), bound.cursors, bound.pose);
            sink = bound.pose.raw()[i & 63];
        }, 4000), "us");
        const std::pair<const char*, float> tiers[] = {{"nlerp_4deg", 4.0f}, {"corrected_20deg", 20.0f}, {"exact_60deg", 60.0f}};
        for (const std::pair<const char*, float>& tier : tiers){
            const std::vector<std::pair<Quaternion, Quaternion>> pairs = keyPairs(tier.second);
            results.add(std::string("micro_sample_quaternion_") + tier.first + "_ns", sampleQuaternionNs(pairs), "ns");
            results.add(std::string("micro_slerp_") + tier.first + "_ns", slerpNs(pairs), "ns");
        }
    }

    // ---- S74 R10: the dense and reduced shapes of R42, generated, and real clips from --clip
    clipRows(results, "clip_dense", anim::writeClip(shapedClip(67, 8241, true)));
    clipRows(results, "clip_reduced", anim::writeClip(shapedClip(44, 911, false)));
    for (const std::string& file : clipFiles){
        std::ifstream in(file, std::ios::binary);
        if (!in){
            std::fprintf(stderr, "--clip %s: cannot read\n", file.c_str());
            return 2;
        }
        std::stringstream text;
        text << in.rdbuf();
        std::string stem = fs::path(file).stem().generic_string();
        std::replace(stem.begin(), stem.end(), '.', '_');
        clipRows(results, "real_" + stem, text.str());
    }

    // ---- S74 R10: an Animation Player crowd, playing and idle, and a retargeted Animator crowd
    {
        const std::string fwd = (assetDir / "fwd.anim").generic_string();
        playerCrowdRows(results, "player_crowd200", *buildPlayerCrowd(bones, fwd, kCharacters, true), std::max(120, kFrames / 3), countAllocs);
        playerCrowdRows(results, "player_idle200", *buildPlayerCrowd(bones, fwd, kCharacters, false), std::max(120, kFrames / 3), countAllocs);
    }
    retargetCrowdRows(results, fixtures, 50, std::max(120, kFrames / 3));

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

    const char* path = bench::jsonPath(argc, argv, "anim.json");
    if (!results.write(path)) std::fprintf(stderr, "could not write %s\n", path);
    std::error_code ec;
    fs::remove_all(assetDir, ec);
    Engine::systemShutdown();
    return 0;
}
