// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Model sub-clips (spec 2.4, 9.R3/R11/R15): the names, the node paths that mirror
// MeshSystem::buildGLTFNodeHierarchy, the builder (LINEAR/STEP/CUBICSPLINE, strided and normalized
// accessors, sparse and out-of-bounds rejection), the clips bound 100% on the hierarchy loadGLTF
// builds (the animated hero, and the T-pose hero forced into that layout by an Animation Player),
// ModelComponent.animationEntities, and builds running on threads next to loads of the pooled model.
// Fixtures: tools/fixtures/pose-animation/assets/models/hero_animated.gltf (make_animated_hero.py)
// and hero_tpose.gltf; the edge cases use glTF models built in memory.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/BoneComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/ModelComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/Transform.h"
#include "component/TranslateTracksComponent.h"
#include "object/Model.h"
#include "pool/ModelPool.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"
#include "tiny_gltf.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

using namespace jasperengine;

namespace {

    const std::vector<std::string> heroClips = {"Walk", "Idle", "Animation 2", "Wave", "Wave 2"};

    std::string fixturePath(const std::string& name){
        return std::string(ANIM_POSE_FIXTURE_MODELS_DIR) + "/" + name;
    }

    std::string readFile(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // a fresh copy of a fixture model in the test assets: its asset-relative path
    std::string copyFixture(const std::string& name){
        static int copies = 0;
        std::string relative = "models/subclips" + std::to_string(copies++) + "/" + name;
        animtest::writeAsset(relative, readFile(fixturePath(name)));
        return relative;
    }

    // ---- the binding walk (spec 2.2) over Transform children in order, kept local so these tests
    // check the builder's paths against the entities and not against another implementation

    std::vector<Entity> childrenOf(Scene& scene, Entity parent){
        std::vector<Entity> children;
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            if (transforms->getComponentFromIndex(i).parent == parent) children.push_back(transforms->getEntity(i));
        }
        return children;
    }

    Entity resolve(Scene& scene, Entity root, const std::string& path){
        if (path.empty()) return root;
        Entity current = root;
        size_t start = 0;
        while (true){
            size_t slash = path.find('/', start);
            std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            std::string name = segment;
            int index = 0;
            size_t hash = segment.rfind('#');
            if (hash != std::string::npos && hash + 1 < segment.size() &&
                    segment.find_first_not_of("0123456789", hash + 1) == std::string::npos){
                name = segment.substr(0, hash);
                index = std::stoi(segment.substr(hash + 1));
            }
            Entity found = NULL_ENTITY;
            for (Entity child : childrenOf(scene, current)){
                if (scene.getEntityName(child) == name && index-- == 0){
                    found = child;
                    break;
                }
            }
            if (found == NULL_ENTITY || slash == std::string::npos) return found;
            current = found;
            start = slash + 1;
        }
    }

    ClipData buildClip(const std::string& reference, std::vector<ClipWarning>* warningsOut = nullptr){
        ClipData data;
        std::vector<ClipWarning> warnings;
        std::string error;
        EXPECT_TRUE(anim::buildModelSubClip(reference, data, warnings, &error)) << reference << ": " << error;
        if (warningsOut) *warningsOut = warnings;
        return data;
    }

    // ---- glTF models built in memory (no file): nodes, float/int accessors, animations

    struct GltfBuilder{
        tinygltf::Model model;

        GltfBuilder(){ model.buffers.resize(1); }

        int node(const std::string& name, const std::vector<int>& children = {}){
            tinygltf::Node node;
            node.name = name;
            node.children = children;
            model.nodes.push_back(node);
            return (int)model.nodes.size() - 1;
        }

        int view(const std::vector<unsigned char>& bytes, size_t stride = 0){
            std::vector<unsigned char>& data = model.buffers[0].data;
            while (data.size() % 4) data.push_back(0);
            tinygltf::BufferView view;
            view.buffer = 0;
            view.byteOffset = data.size();
            view.byteLength = bytes.size();
            view.byteStride = stride;
            data.insert(data.end(), bytes.begin(), bytes.end());
            model.bufferViews.push_back(view);
            return (int)model.bufferViews.size() - 1;
        }

        int accessor(int view, int componentType, int type, size_t count, size_t byteOffset = 0, bool normalized = false){
            tinygltf::Accessor accessor;
            accessor.bufferView = view;
            accessor.componentType = componentType;
            accessor.type = type;
            accessor.count = count;
            accessor.byteOffset = byteOffset;
            accessor.normalized = normalized;
            model.accessors.push_back(accessor);
            return (int)model.accessors.size() - 1;
        }

        static std::vector<unsigned char> bytes(const std::vector<float>& values){
            std::vector<unsigned char> out(values.size() * 4);
            std::memcpy(out.data(), values.data(), out.size());
            return out;
        }

        int floats(const std::vector<float>& values, int type){
            size_t width = type == TINYGLTF_TYPE_SCALAR ? 1 : (type == TINYGLTF_TYPE_VEC3 ? 3 : 4);
            return accessor(view(bytes(values)), TINYGLTF_COMPONENT_TYPE_FLOAT, type, values.size() / width);
        }

        int animation(const std::string& name){
            tinygltf::Animation animation;
            animation.name = name;
            model.animations.push_back(animation);
            return (int)model.animations.size() - 1;
        }

        void channel(int animation, int node, const std::string& path, int input, int output,
                     const std::string& interpolation = "LINEAR"){
            tinygltf::Animation& target = model.animations[animation];
            tinygltf::AnimationSampler sampler;
            sampler.input = input;
            sampler.output = output;
            sampler.interpolation = interpolation;
            target.samplers.push_back(sampler);
            tinygltf::AnimationChannel channel;
            channel.sampler = (int)target.samplers.size() - 1;
            channel.target_node = node;
            channel.target_path = path;
            target.channels.push_back(channel);
        }
    };

    bool hasWarning(const std::vector<ClipWarning>& warnings, const std::string& path, const std::string& text){
        for (const ClipWarning& warning : warnings){
            if (warning.path == path && warning.message.find(text) != std::string::npos) return true;
        }
        return false;
    }

    std::string describe(const std::vector<ClipWarning>& warnings){
        std::string out;
        for (const ClipWarning& warning : warnings) out += warning.path + ": " + warning.message + "\n";
        return out;
    }

}

TEST(ModelSubClips, NamesFollowTheLoaderAndNumberDuplicates){
    EXPECT_EQ(anim::subClipNames({"Walk", "", "Walk", "Walk", ""}),
              (std::vector<std::string>{"Walk", "Animation 1", "Walk 2", "Walk 3", "Animation 4"}));
    // a generated name another animation already has moves on to the next number
    EXPECT_EQ(anim::subClipNames({"Run", "Run 2", "Run", "Run"}),
              (std::vector<std::string>{"Run", "Run 2", "Run 3", "Run 4"}));
    EXPECT_EQ(anim::subClipNames({"Animation 1", ""}), (std::vector<std::string>{"Animation 1", "Animation 1 2"}));
    EXPECT_TRUE(anim::subClipNames({}).empty());
}

TEST(ModelSubClips, ListsTheFixtureAnimations){
    std::vector<anim::ModelClipInfo> clips;
    std::string error;
    ASSERT_TRUE(anim::listModelFileClips(fixturePath("hero_animated.gltf"), clips, &error)) << error;
    ASSERT_EQ(clips.size(), heroClips.size());
    const float durations[] = {1.0f, 2.0f, 1.5f, 0.9f, 0.8f};
    const size_t channels[] = {8, 3, 2, 1, 1};
    for (size_t i = 0; i < clips.size(); i++){
        EXPECT_EQ(clips[i].index, (int)i);
        EXPECT_EQ(clips[i].name, heroClips[i]);
        EXPECT_NEAR(clips[i].duration, durations[i], 1e-6f) << heroClips[i];
        EXPECT_EQ(clips[i].channels, channels[i]) << heroClips[i];
    }

    // a model without animations lists none; a missing or non-model file fails
    ASSERT_TRUE(anim::listModelFileClips(fixturePath("hero_tpose.gltf"), clips, &error)) << error;
    EXPECT_TRUE(clips.empty());
    EXPECT_FALSE(anim::listModelFileClips(fixturePath("nope.gltf"), clips, &error));
    EXPECT_NE(error.find("nope.gltf"), std::string::npos) << error;
    EXPECT_FALSE(anim::listModelFileClips("anim/walk.anim", clips, &error));
}

TEST(ModelSubClips, FindsClipsByNameThenByIndex){
    GltfBuilder gltf;
    gltf.node("A");
    for (const char* name : {"Walk", "", "1", "Walk"}) gltf.animation(name);
    EXPECT_EQ(anim::findModelClip(gltf.model, "Walk"), 0);
    EXPECT_EQ(anim::findModelClip(gltf.model, "Walk 2"), 3);
    EXPECT_EQ(anim::findModelClip(gltf.model, "Animation 1"), 1);
    // "1" is an animation's name, so it is not the index 1
    EXPECT_EQ(anim::findModelClip(gltf.model, "1"), 2);
    EXPECT_EQ(anim::findModelClip(gltf.model, "3"), 3);
    EXPECT_EQ(anim::findModelClip(gltf.model, "0"), 0);
    // one spelling per index, the one the pool's alias table knows
    EXPECT_EQ(anim::findModelClip(gltf.model, "03"), -1);
    EXPECT_EQ(anim::findModelClip(gltf.model, "00"), -1);
    EXPECT_EQ(anim::findModelClip(gltf.model, "4"), -1);
    EXPECT_EQ(anim::findModelClip(gltf.model, "-1"), -1);
    EXPECT_EQ(anim::findModelClip(gltf.model, ""), -1);
    EXPECT_EQ(anim::findModelClip(gltf.model, "Run"), -1);
    EXPECT_EQ(anim::findModelClip(gltf.model, "99999999999999999999"), -1);
}

TEST(ModelSubClips, NodePathsMirrorTheNodeHierarchy){
    GltfBuilder gltf;
    // 0 root [1, 2, 3, 6]; 1 "Arm"; 2 "" (Node 2); 3 "Arm" -> Arm#1; 4 child of 3 and of 6: the
    // last lister (6) wins; 5 parentless; 6 "Arm" -> Arm#2; 7 lists itself; 8 <-> 9 a loop
    gltf.node("Root", {1, 2, 3, 6});
    gltf.node("Arm");
    gltf.node("");
    gltf.node("Arm", {4});
    gltf.node("Hand");
    gltf.node("Loose");
    gltf.node("Arm", {4});
    gltf.node("Self", {7});
    gltf.node("A", {9});
    gltf.node("B", {8, 42});
    std::vector<std::string> paths = anim::modelNodePaths(gltf.model);
    ASSERT_EQ(paths.size(), 10u);
    EXPECT_EQ(paths[0], "Root");
    EXPECT_EQ(paths[1], "Root/Arm");
    EXPECT_EQ(paths[2], "Root/Node 2");
    EXPECT_EQ(paths[3], "Root/Arm#1");
    EXPECT_EQ(paths[4], "Root/Arm#2/Hand");
    EXPECT_EQ(paths[5], "Loose");
    EXPECT_EQ(paths[6], "Root/Arm#2");
    EXPECT_EQ(paths[7], "Self");
    // invalid files: the loop is cut where it comes back (best effort), nothing hangs
    EXPECT_FALSE(paths[8].empty());
    EXPECT_FALSE(paths[9].empty());
}

// Every track of every sub-clip binds on the hierarchy loadGLTF builds, to the entity of its
// glTF node, and carries the same keys as the loader's own animation entities
TEST(ModelSubClips, AnimatedHeroClipsBindEveryTrackAndMatchTheLoader){
    const std::string path = fixturePath("hero_animated.gltf");
    Scene scene;
    Model model(&scene);
    const Entity root = model.getEntity();
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, path, false, false, false));

    const ModelComponent& component = scene.getComponent<ModelComponent>(root);
    ASSERT_FALSE(component.nodesIdMapping.empty()) << "an animated model loads in the node-hierarchy layout";
    std::shared_ptr<tinygltf::Model> gltf = component.gltfModel;
    ASSERT_TRUE(gltf);

    std::vector<std::string> nodePaths = anim::modelNodePaths(*gltf);
    ASSERT_EQ(nodePaths.size(), gltf->nodes.size());
    for (const auto& node : component.nodesIdMapping){
        EXPECT_EQ(resolve(scene, root, nodePaths[node.first]), node.second) << nodePaths[node.first];
    }

    ASSERT_EQ(component.animations.size(), heroClips.size()) << "animationEntities defaults to true";
    size_t compared = 0;
    size_t tangentKeys = 0;
    for (size_t i = 0; i < heroClips.size(); i++){
        std::vector<ClipWarning> warnings;
        ClipData clip = buildClip(path + "#" + heroClips[i], &warnings);
        EXPECT_TRUE(warnings.empty()) << heroClips[i] << "\n" << describe(warnings);
        EXPECT_EQ(clip.frameRate, 30.0f);
        EXPECT_EQ(clip.wrap, ClipWrapMode::Once);
        EXPECT_FALSE(clip.hasExplicitLength());
        EXPECT_EQ(clip.source.kind, "model");
        EXPECT_EQ(clip.source.path, path);
        EXPECT_EQ(clip.source.name, heroClips[i]);
        EXPECT_EQ(clip.tracks.size(), gltf->animations[i].channels.size()) << heroClips[i];
        for (const ClipTrackData& track : clip.tracks){
            EXPECT_NE(resolve(scene, root, track.path), NULL_ENTITY) << heroClips[i] << ": " << track.path;
        }

        // the loader's entity tracks of the same animation: same targets, times and values
        const AnimationComponent& animation = scene.getComponent<AnimationComponent>(component.animations[i]);
        for (const ActionFrame& frame : animation.actions){
            const Entity target = scene.getComponent<ActionComponent>(frame.action).target;
            const KeyframeTracksComponent& keyframes = scene.getComponent<KeyframeTracksComponent>(frame.action);
            ClipChannel channel = ClipChannel::Position;
            if (scene.findComponent<RotateTracksComponent>(frame.action)) channel = ClipChannel::Rotation;
            if (scene.findComponent<ScaleTracksComponent>(frame.action)) channel = ClipChannel::Scale;
            const ClipTrackData* track = nullptr;
            for (const ClipTrackData& candidate : clip.tracks){
                if (candidate.channel == channel && resolve(scene, root, candidate.path) == target) track = &candidate;
            }
            ASSERT_TRUE(track) << heroClips[i] << ": no clip track for " << scene.getEntityName(target);
            ASSERT_EQ(track->keys.size(), keyframes.times.size()) << track->path;
            for (size_t k = 0; k < track->keys.size(); k++){
                const ClipKey& key = track->keys[k];
                EXPECT_NEAR(key.time, keyframes.times[k], 1e-6f);
                std::vector<float> expected;
                if (channel == ClipChannel::Rotation){
                    const Quaternion& q = scene.getComponent<RotateTracksComponent>(frame.action).values[k];
                    expected = {q.x, q.y, q.z, q.w};
                }else{
                    const Vector3& v = channel == ClipChannel::Scale ? scene.getComponent<ScaleTracksComponent>(frame.action).values[k]
                                                                     : scene.getComponent<TranslateTracksComponent>(frame.action).values[k];
                    expected = {v.x, v.y, v.z};
                }
                ASSERT_EQ(key.value.size(), expected.size());
                for (size_t c = 0; c < expected.size(); c++) EXPECT_NEAR(key.value[c], expected[c], 1e-6f) << track->path;
                // CUBICSPLINE tangents (the fixture's are non-zero): the loader's, x y z (w)
                std::vector<float> in, out;
                if (channel == ClipChannel::Rotation){
                    const RotateTracksComponent& rotate = scene.getComponent<RotateTracksComponent>(frame.action);
                    if (k < rotate.inTangents.size()){
                        const Quaternion& a = rotate.inTangents[k];
                        const Quaternion& b = rotate.outTangents[k];
                        in = {a.x, a.y, a.z, a.w};
                        out = {b.x, b.y, b.z, b.w};
                    }
                }else{
                    const std::vector<Vector3>& ins = channel == ClipChannel::Scale ? scene.getComponent<ScaleTracksComponent>(frame.action).inTangents
                                                                                    : scene.getComponent<TranslateTracksComponent>(frame.action).inTangents;
                    const std::vector<Vector3>& outs = channel == ClipChannel::Scale ? scene.getComponent<ScaleTracksComponent>(frame.action).outTangents
                                                                                     : scene.getComponent<TranslateTracksComponent>(frame.action).outTangents;
                    if (k < ins.size()){
                        in = {ins[k].x, ins[k].y, ins[k].z};
                        out = {outs[k].x, outs[k].y, outs[k].z};
                    }
                }
                EXPECT_EQ(key.inTangent.size(), in.size()) << track->path << " key " << k;
                for (size_t c = 0; c < in.size() && c < key.inTangent.size(); c++){
                    EXPECT_NEAR(key.inTangent[c], in[c], 1e-6f) << track->path << " key " << k;
                    EXPECT_NEAR(key.outTangent[c], out[c], 1e-6f) << track->path << " key " << k;
                }
                if (!in.empty()){
                    tangentKeys++;
                    bool nonZero = false;
                    for (float v : in) nonZero = nonZero || v != 0.0f;
                    for (float v : out) nonZero = nonZero || v != 0.0f;
                    EXPECT_TRUE(nonZero) << track->path << " key " << k << ": the fixture's tangents are not all zero";
                }
                // the entity tracks: STEP easings per segment, tangents for CUBICSPLINE
                bool step = k < keyframes.easings.size() && keyframes.easings[k] == EaseType::STEP;
                EXPECT_EQ(key.ease == EaseType::STEP, step && k + 1 < track->keys.size()) << track->path << " key " << k;
            }
            compared++;
        }
    }
    EXPECT_EQ(compared, 15u) << "every channel of the five animations";
    EXPECT_EQ(tangentKeys, 6u) << "Idle's two CUBICSPLINE channels, 3 keys each";
}

TEST(ModelSubClips, BuilderKeepsInterpolationsAndLayouts){
    const std::string path = fixturePath("hero_animated.gltf");
    ClipData walk = buildClip(path + "#Walk");
    int hips = walk.findTrack("Hero/mixamorig:Hips", ClipChannel::Position);
    ASSERT_GE(hips, 0);
    ASSERT_EQ(walk.tracks[hips].keys.size(), 5u);
    EXPECT_NEAR(walk.tracks[hips].keys[1].value[1], 0.92f, 1e-6f);
    EXPECT_EQ(walk.tracks[hips].keys[1].ease, EaseType::LINEAR);
    EXPECT_FALSE(walk.tracks[hips].keys[1].hasTangents());
    int leg = walk.findTrack("Hero/mixamorig:Hips/mixamorig:LeftUpLeg", ClipChannel::Rotation);
    ASSERT_GE(leg, 0);
    // x y z w: -25 degrees about X at the first key
    const std::vector<float>& q = walk.tracks[leg].keys[0].value;
    ASSERT_EQ(q.size(), 4u);
    EXPECT_NEAR(q[0], std::sin(-12.5f * 3.14159265f / 180.0f), 1e-5f);
    EXPECT_NEAR(q[3], std::cos(12.5f * 3.14159265f / 180.0f), 1e-5f);
    EXPECT_NEAR(walk.effectiveLength(), 1.0f, 1e-6f);

    ClipData idle = buildClip(path + "#Idle");
    int spine = idle.findTrack("Hero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1", ClipChannel::Rotation);
    ASSERT_GE(spine, 0);
    ASSERT_EQ(idle.tracks[spine].keys.size(), 3u);
    for (const ClipKey& key : idle.tracks[spine].keys){
        EXPECT_TRUE(key.hasTangents());
        EXPECT_EQ(key.inTangent.size(), 4u);
        EXPECT_EQ(key.ease, EaseType::LINEAR);
    }
    EXPECT_GE(idle.findTrack("Hero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:Neck/mixamorig:Head",
                             ClipChannel::Scale), 0);

    // the unnamed animation by its name and by its index; STEP on every segment
    ClipData step = buildClip(path + "#Animation 2");
    EXPECT_EQ(anim::writeClip(step), anim::writeClip(buildClip(path + "#2")));
    int hero = step.findTrack("Hero", ClipChannel::Position);
    ASSERT_GE(hero, 0);
    ASSERT_EQ(step.tracks[hero].keys.size(), 3u);
    EXPECT_EQ(step.tracks[hero].keys[0].ease, EaseType::STEP);
    EXPECT_EQ(step.tracks[hero].keys[1].ease, EaseType::STEP);

    // same-named animations are two clips
    ClipData wave = buildClip(path + "#Wave");
    ClipData wave2 = buildClip(path + "#Wave 2");
    EXPECT_GE(wave.findTrack("Hero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:RightShoulder/mixamorig:RightArm",
                             ClipChannel::Rotation), 0);
    EXPECT_GE(wave2.findTrack("Hero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:RightShoulder/mixamorig:RightArm/mixamorig:RightForeArm",
                              ClipChannel::Rotation), 0);
    EXPECT_EQ(anim::writeClip(wave2), anim::writeClip(buildClip(path + "#4")));

    // the built data is already normalized (the loader invariants): a write/parse round trip is clean
    anim::ClipParseResult parsed = anim::parseClip(anim::writeClip(idle));
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed.warnings.empty()) << describe(parsed.warnings);
    EXPECT_EQ(anim::writeClip(*parsed.data), anim::writeClip(idle));

    ClipData none;
    std::vector<ClipWarning> warnings;
    std::string error;
    EXPECT_FALSE(anim::buildModelSubClip(path + "#Run", none, warnings, &error));
    EXPECT_NE(error.find("Run"), std::string::npos) << error;
    EXPECT_FALSE(anim::buildModelSubClip(path + "#5", none, warnings, &error));
    EXPECT_FALSE(anim::buildModelSubClip(path, none, warnings, &error)) << "not a reference";
}

TEST(ModelSubClips, ReadsStridedNormalizedAndMorphAccessors){
    GltfBuilder gltf;
    gltf.node("Body", {1});
    gltf.node("Face");
    const int animation = gltf.animation("Mixed");
    const int times = gltf.floats({0.0f, 0.5f, 1.0f}, TINYGLTF_TYPE_SCALAR);

    // positions interleaved with padding: stride 16 bytes, accessor offset 4
    std::vector<float> interleaved = {9, 1, 2, 3,  9, 4, 5, 6,  9, 7, 8, 9};
    const int strided = gltf.accessor(gltf.view(GltfBuilder::bytes(interleaved), 16), TINYGLTF_COMPONENT_TYPE_FLOAT,
                                      TINYGLTF_TYPE_VEC3, 3, 4);
    gltf.channel(animation, 0, "translation", times, strided);

    // rotations as normalized shorts (x y z w)
    std::vector<int16_t> shorts = {0, 0, 0, 32767,  32767, 0, 0, 0,  0, 0, 0, -32768};
    std::vector<unsigned char> shortBytes(shorts.size() * 2);
    std::memcpy(shortBytes.data(), shorts.data(), shortBytes.size());
    const int rotations = gltf.accessor(gltf.view(shortBytes), TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_VEC4, 3, 0, true);
    gltf.channel(animation, 0, "rotation", times, rotations);

    // two morph weights per key as normalized unsigned bytes
    std::vector<unsigned char> weights = {0, 255, 51, 102, 255, 0};
    const int morph = gltf.accessor(gltf.view(weights), TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_SCALAR, 6, 0, true);
    gltf.channel(animation, 1, "weights", times, morph);

    // CUBICSPLINE weights: per key a1 a2, v1 v2, b1 b2
    const int cubicTimes = gltf.floats({0.0f, 1.0f}, TINYGLTF_TYPE_SCALAR);
    const int cubic = gltf.floats({1, 2, 0.1f, 0.2f, 3, 4,   5, 6, 0.7f, 0.8f, 7, 8}, TINYGLTF_TYPE_SCALAR);
    gltf.channel(animation, 0, "weights", cubicTimes, cubic, "CUBICSPLINE");

    ClipData clip;
    std::vector<ClipWarning> warnings;
    std::string error;
    ASSERT_TRUE(anim::buildSubClip(gltf.model, animation, clip, warnings, &error)) << error;
    EXPECT_TRUE(warnings.empty()) << describe(warnings);

    int position = clip.findTrack("Body", ClipChannel::Position);
    ASSERT_GE(position, 0);
    ASSERT_EQ(clip.tracks[position].keys.size(), 3u);
    EXPECT_EQ(clip.tracks[position].keys[0].value, (std::vector<float>{1, 2, 3}));
    EXPECT_EQ(clip.tracks[position].keys[2].value, (std::vector<float>{7, 8, 9}));

    int rotation = clip.findTrack("Body", ClipChannel::Rotation);
    ASSERT_GE(rotation, 0);
    ASSERT_EQ(clip.tracks[rotation].keys.size(), 3u);
    EXPECT_NEAR(clip.tracks[rotation].keys[0].value[3], 1.0f, 1e-6f);
    EXPECT_NEAR(std::fabs(clip.tracks[rotation].keys[1].value[0]), 1.0f, 1e-6f);
    // -32768 clamps to -1; the hemisphere pass may flip the key, it stays a unit w rotation
    EXPECT_NEAR(std::fabs(clip.tracks[rotation].keys[2].value[3]), 1.0f, 1e-6f);

    int face = clip.findTrack("Body/Face", ClipChannel::Morph);
    ASSERT_GE(face, 0);
    ASSERT_EQ(clip.tracks[face].keys.size(), 3u);
    EXPECT_NEAR(clip.tracks[face].keys[0].value[1], 1.0f, 1e-6f);
    EXPECT_NEAR(clip.tracks[face].keys[1].value[0], 0.2f, 1e-6f);
    EXPECT_NEAR(clip.tracks[face].keys[1].value[1], 0.4f, 1e-6f);

    int body = clip.findTrack("Body", ClipChannel::Morph);
    ASSERT_GE(body, 0);
    ASSERT_EQ(clip.tracks[body].keys.size(), 2u);
    const ClipKey& last = clip.tracks[body].keys[1];
    EXPECT_EQ(last.inTangent, (std::vector<float>{5, 6}));
    EXPECT_NEAR(last.value[0], 0.7f, 1e-6f);
    EXPECT_NEAR(last.value[1], 0.8f, 1e-6f);
    EXPECT_EQ(last.outTangent, (std::vector<float>{7, 8}));
}

TEST(ModelSubClips, RejectsBadChannelsWithWarnings){
    GltfBuilder gltf;
    gltf.node("Root", {1});
    gltf.node("a/b");
    const int animation = gltf.animation("Bad");
    const int times = gltf.floats({0.0f, 1.0f}, TINYGLTF_TYPE_SCALAR);
    const int good = gltf.floats({0, 0, 0, 1, 1, 1}, TINYGLTF_TYPE_VEC3);
    gltf.channel(animation, 0, "translation", times, good);                     // 0: kept

    const int sparse = gltf.floats({0, 0, 0, 1, 1, 1}, TINYGLTF_TYPE_VEC3);
    gltf.model.accessors[sparse].sparse.isSparse = true;
    gltf.channel(animation, 0, "scale", times, sparse);                         // 1: sparse
    // 2: the accessor runs past its view (3 elements in a 2-element view)
    const int overrun = gltf.accessor(gltf.model.accessors[good].bufferView, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, 3);
    gltf.channel(animation, 0, "scale", times, overrun);
    gltf.channel(animation, 0, "rotation", times, good);                        // 3: VEC3 rotation
    gltf.channel(animation, 0, "pointer", times, good);                         // 4: not animatable
    gltf.channel(animation, 7, "translation", times, good);                     // 5: no node
    gltf.channel(animation, 1, "translation", times, good);                     // 6: '/' in the name
    const int shorts = gltf.accessor(gltf.view(std::vector<unsigned char>(12, 0)), TINYGLTF_COMPONENT_TYPE_SHORT,
                                     TINYGLTF_TYPE_VEC3, 2);
    gltf.channel(animation, 0, "scale", times, shorts);                         // 7: integer scale
    gltf.channel(animation, 0, "translation", times, good, "CUBICSPLINE");      // 8: 2 outputs, needs 6
    gltf.model.animations[animation].channels.push_back(gltf.model.animations[animation].channels[0]);
    gltf.model.animations[animation].channels.back().sampler = 42;              // 9: no sampler
    gltf.channel(animation, 0, "translation", times, good, "QUADRATIC");        // 10: kept as LINEAR

    ClipData clip;
    std::vector<ClipWarning> warnings;
    std::string error;
    ASSERT_TRUE(anim::buildSubClip(gltf.model, animation, clip, warnings, &error)) << error;
    ASSERT_EQ(clip.tracks.size(), 1u) << "the two good translation channels merge into one track";
    EXPECT_EQ(clip.tracks[0].path, "Root");
    const std::string c = "animations[0].channels[";
    EXPECT_TRUE(hasWarning(warnings, c + "1]", "sparse")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "2]", "past the end")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "3]", "expected VEC4")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "4]", "not animatable")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "5]", "no node")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "6]", "can't address")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "7]", "float expected")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "8]", "expected 6")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "9]", "sampler 42")) << describe(warnings);
    EXPECT_TRUE(hasWarning(warnings, c + "10]", "QUADRATIC")) << describe(warnings);

    // nothing readable: an error, not an empty clip
    GltfBuilder empty;
    empty.node("Root");
    const int only = empty.animation("");
    empty.channel(only, 0, "translation", empty.floats({0.0f}, TINYGLTF_TYPE_SCALAR), 99);
    warnings.clear();
    EXPECT_FALSE(anim::buildSubClip(empty.model, only, clip, warnings, &error));
    EXPECT_NE(error.find("Animation 0"), std::string::npos) << error;
    EXPECT_TRUE(hasWarning(warnings, "animations[0].channels[0]", "does not exist")) << describe(warnings);
    EXPECT_FALSE(anim::buildSubClip(empty.model, 1, clip, warnings, &error));
    EXPECT_FALSE(anim::buildSubClip(empty.model, -1, clip, warnings, &error));
}

// One addressable-name rule (ClipBinding's isAddressableName): "Bone#" and "Bone#-1" can't be read
// back as paths, so their channels are dropped with a warning like "Bone#2"; "Take #b" is a name
TEST(ModelSubClips, UnaddressableNamesFollowClipBinding){
    GltfBuilder gltf;
    const std::vector<std::string> names = {"Bone#", "Bone#-1", "Bone#2", "Take #b"};
    for (const std::string& name : names) gltf.node(name);
    const int animation = gltf.animation("Names");
    const int times = gltf.floats({0.0f, 1.0f}, TINYGLTF_TYPE_SCALAR);
    const int values = gltf.floats({0, 0, 0, 1, 1, 1}, TINYGLTF_TYPE_VEC3);
    for (size_t i = 0; i < names.size(); i++) gltf.channel(animation, (int)i, "translation", times, values);

    ClipData clip;
    std::vector<ClipWarning> warnings;
    std::string error;
    ASSERT_TRUE(anim::buildSubClip(gltf.model, animation, clip, warnings, &error)) << error;
    ASSERT_EQ(clip.tracks.size(), 1u) << describe(warnings);
    EXPECT_EQ(clip.tracks[0].path, "Take #b");
    for (int i = 0; i < 3; i++){
        EXPECT_TRUE(hasWarning(warnings, "animations[0].channels[" + std::to_string(i) + "]", "can't address"))
            << names[i] << "\n" << describe(warnings);
    }
}

// The T-pose hero has no animation: alone it loads in the legacy layout (unchanged), on or under an
// Animation Player in the node-hierarchy layout, where the animated hero's clips bind 100%
TEST(ModelSubClips, PlayerForcesTheNodeHierarchyForCrossFileClips){
    const std::string tpose = fixturePath("hero_tpose.gltf");
    const std::string animated = fixturePath("hero_animated.gltf");

    {
        Scene scene;
        Model model(&scene);
        ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), tpose, false, false, false));
        const ModelComponent& component = scene.getComponent<ModelComponent>(model.getEntity());
        EXPECT_TRUE(component.nodesIdMapping.empty()) << "no player: the legacy skeleton layout";
        EXPECT_NE(component.skeleton, NULL_ENTITY);
        EXPECT_FALSE(scene.getSystem<MeshSystem>()->isNodeHierarchyForced(model.getEntity()));
        EXPECT_FALSE(scene.getSystem<MeshSystem>()->needsNodeHierarchyReload(model.getEntity()));

        // a player added afterwards: the model needs the reload that switches its layout
        scene.addComponent<AnimationPlayerComponent>(model.getEntity(), {});
        EXPECT_TRUE(scene.getSystem<MeshSystem>()->isNodeHierarchyForced(model.getEntity()));
        EXPECT_TRUE(scene.getSystem<MeshSystem>()->needsNodeHierarchyReload(model.getEntity()));
    }

    for (bool onParent : {false, true}){
        Scene scene;
        Model model(&scene);
        const Entity root = model.getEntity();
        if (onParent){
            Entity holder = scene.createEntity();
            scene.addComponent<Transform>(holder, {});
            scene.addComponent<AnimationPlayerComponent>(holder, {});
            scene.addEntityChild(holder, root, false);
        }else{
            scene.addComponent<AnimationPlayerComponent>(root, {});
        }
        ASSERT_TRUE(scene.getSystem<MeshSystem>()->isNodeHierarchyForced(root));
        ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, tpose, false, false, false));
        const ModelComponent& component = scene.getComponent<ModelComponent>(root);
        ASSERT_FALSE(component.nodesIdMapping.empty()) << (onParent ? "under" : "on") << " a player: node hierarchy";
        EXPECT_EQ(component.nodesIdMapping.size(), component.gltfModel->nodes.size());
        EXPECT_FALSE(scene.getSystem<MeshSystem>()->needsNodeHierarchyReload(root));

        size_t tracks = 0;
        for (const std::string& name : heroClips){
            ClipData clip = buildClip(animated + "#" + name);
            for (const ClipTrackData& track : clip.tracks){
                EXPECT_NE(resolve(scene, root, track.path), NULL_ENTITY) << name << ": " << track.path;
                tracks++;
            }
        }
        EXPECT_EQ(tracks, 15u);
    }
}

TEST(ModelSubClips, AnimationEntitiesFlagSkipsTheLoaderAnimations){
    const std::string path = fixturePath("hero_animated.gltf");
    Scene scene;
    Model model(&scene);
    const Entity root = model.getEntity();
    scene.getComponent<ModelComponent>(root).animationEntities = false;
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, path, false, false, false));
    const ModelComponent& component = scene.getComponent<ModelComponent>(root);
    EXPECT_TRUE(component.animations.empty());
    EXPECT_FALSE(component.nodesIdMapping.empty()) << "the layout does not depend on the flag";
    EXPECT_EQ(scene.getComponentArray<AnimationComponent>()->size(), 0u);

    // and the default makes them, with the loader's names (duplicates are not renumbered there)
    Model other(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(other.getEntity(), path, false, false, false));
    const ModelComponent& made = scene.getComponent<ModelComponent>(other.getEntity());
    ASSERT_EQ(made.animations.size(), 5u);
    EXPECT_EQ(scene.getEntityName(made.animations[2]), "Animation 2");
    EXPECT_EQ(scene.getEntityName(made.animations[4]), "Wave");
}

// Model:findAnimation / playAnimation without a player: the loader's entities of two animations
// named "Wave" are both "Wave", reachable as "Wave" and "Wave 2" (the sub-clip names the
// Inspector shows); unique names and the first duplicate find what they found before
TEST(ModelSubClips, ModelFindsDuplicateAnimationNamesByTheirSubClipNames){
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(fixturePath("hero_animated.gltf")));
    const ModelComponent& component = scene.getComponent<ModelComponent>(model.getEntity());
    ASSERT_EQ(component.animations.size(), 5u);
    ASSERT_EQ(scene.getEntityName(component.animations[3]), "Wave");
    ASSERT_EQ(scene.getEntityName(component.animations[4]), "Wave");
    const std::vector<Entity> animations = component.animations;

    EXPECT_EQ(model.findAnimation("Walk").getEntity(), animations[0]);
    EXPECT_EQ(model.findAnimation("Animation 2").getEntity(), animations[2]);
    EXPECT_EQ(model.findAnimation("Wave").getEntity(), animations[3]);
    EXPECT_EQ(model.findAnimation("Wave 2").getEntity(), animations[4]);
    EXPECT_THROW(model.findAnimation("Wave 3"), std::out_of_range);

    auto started = [&](Entity animation){
        const ActionComponent& action = scene.getComponent<ActionComponent>(animation);
        return action.startTrigger || action.state == ActionState::Running;
    };
    model.playAnimation("Wave 2", 0.0f);
    EXPECT_TRUE(started(animations[4]));
    EXPECT_FALSE(started(animations[3]));
    model.stopAnimations(0.0f);
    scene.getComponent<ActionComponent>(animations[4]).startTrigger = false;
    model.playAnimation("Wave");
    EXPECT_TRUE(started(animations[3]));
    EXPECT_FALSE(started(animations[4]));
}

// Blend shape weights and model (re)loads (MeshComponent::morphWeightsSet): a freshly loaded model
// gets its file's default weights; weights it holds after that (edited, all zero on purpose) or
// read from a saved scene survive a reload of the same file; another model file takes its own
TEST(ModelSubClips, MorphWeightsKeepTheSavedStateOverTheFileDefaults){
    nlohmann::json gltf = nlohmann::json::parse(readFile(std::string(ANIM_FIXTURES_DIR) + "/runtime-shaders/assets/skinned_morph.gltf"));
    ASSERT_EQ(gltf["meshes"].size(), 1u);
    gltf["meshes"][0]["weights"] = {0.75};
    const std::string withDefaults = animtest::writeAsset("models/morphs/defaults.gltf", gltf.dump());
    gltf["meshes"][0]["weights"] = {0.25};
    const std::string otherDefaults = animtest::writeAsset("models/morphs/other.gltf", gltf.dump());

    Scene scene;
    auto weightOf = [&](Entity root){
        // the mesh with the blend shapes (the model root, or its mesh node)
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const Entity entity = meshes->getEntity(i);
            if (entity == root || scene.isParentOf(root, entity)){
                const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                if (mesh.morphWeightsSet) return mesh.morphWeights[0];
            }
        }
        return -1.0f;
    };
    auto setWeight = [&](Entity root, float value){
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const Entity entity = meshes->getEntity(i);
            if ((entity == root || scene.isParentOf(root, entity)) && meshes->getComponentFromIndex(i).morphWeightsSet){
                meshes->getComponentFromIndex(i).morphWeights[0] = value;
            }
        }
    };

    // fresh: the file's defaults
    Model model(&scene);
    const Entity root = model.getEntity();
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, withDefaults, false, false, false));
    EXPECT_FLOAT_EQ(weightOf(root), 0.75f);
    // set to 0 on purpose (all weights 0), then the same file reloads: still 0
    setWeight(root, 0.0f);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, withDefaults, false, true, false));
    EXPECT_FLOAT_EQ(weightOf(root), 0.0f) << "a deliberate 0 survives the reload";
    setWeight(root, 0.4f);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, withDefaults, false, true, false));
    EXPECT_FLOAT_EQ(weightOf(root), 0.4f) << "an edited weight survives the reload";
    // another model file: its own defaults
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, otherDefaults, false, false, false));
    EXPECT_FLOAT_EQ(weightOf(root), 0.25f);

    // a mesh decoded from a saved scene (the editor's Stream sets the flag with the weights)
    // before its model loads keeps the saved zeros
    ASSERT_TRUE(scene.getComponent<MeshComponent>(root).morphWeightsSet) << "the single mesh loads on the model root";
    Model saved(&scene);
    scene.getComponent<MeshComponent>(saved.getEntity()).morphWeightsSet = true;
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(saved.getEntity(), withDefaults, false, false, false));
    EXPECT_FLOAT_EQ(scene.getComponent<MeshComponent>(saved.getEntity()).morphWeights[0], 0.0f) << "the saved zeros, not the file's 0.75";
    EXPECT_FLOAT_EQ(weightOf(saved.getEntity()), 0.0f);
}

// Blend shape names per mesh (MeshComponent::morphTargetNames): a model with two meshes whose
// targets are named differently ("Bend" on Strip, "Smile" and "Blink" on Face) keeps each mesh's
// own names; the model's name table still finds every name
TEST(ModelSubClips, MorphTargetNamesArePerMesh){
    nlohmann::json gltf = nlohmann::json::parse(readFile(std::string(ANIM_FIXTURES_DIR) + "/runtime-shaders/assets/skinned_morph.gltf"));
    nlohmann::json face = gltf["meshes"][0];
    face["name"] = "Face";
    face["primitives"][0]["targets"].push_back(face["primitives"][0]["targets"][0]);
    face["weights"] = {0.0, 0.0};
    face["extras"]["targetNames"] = {"Smile", "Blink"};
    gltf["meshes"].push_back(face);
    gltf["nodes"].push_back({{"name", "Face"}, {"mesh", 1}, {"skin", 0}});
    gltf["scenes"][0]["nodes"].push_back(gltf["nodes"].size() - 1);
    const std::string path = animtest::writeAsset("models/morphs/two_meshes.gltf", gltf.dump());

    Scene scene;
    Model model(&scene);
    const Entity root = model.getEntity();
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, path, false, false, false));
    std::map<std::string, std::vector<std::string>> byName;
    auto meshes = scene.getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++){
        const Entity entity = meshes->getEntity(i);
        if (scene.isParentOf(root, entity)) byName[scene.getEntityName(entity)] = meshes->getComponentFromIndex(i).morphTargetNames;
    }
    ASSERT_EQ(byName.size(), 2u) << "one mesh entity per mesh node";
    EXPECT_EQ(byName["Strip"], (std::vector<std::string>{"Bend"}));
    EXPECT_EQ(byName["Face"], (std::vector<std::string>{"Smile", "Blink"}));
    const ModelComponent& component = scene.getComponent<ModelComponent>(root);
    EXPECT_EQ(component.morphNameMapping.count("Bend"), 1u);
    EXPECT_EQ(component.morphNameMapping.count("Blink"), 1u);
}

// Builds on threads (the shader variant probe and play startup load off the main thread): from
// private parses, then from the pooled parse while the main thread pools it. Each round pools a
// fresh copy in the flat layout (skipEntities: loadGLTF's reload of saved entities), whose first
// load reserves room for synthetic buffers in the shared model, moving its buffers while the
// builders read them: they take turns on the pooled-glTF mutex.
TEST(ModelSubClips, ConcurrentBuildsAgree){
    const std::string path = copyFixture("hero_animated.gltf");
    std::vector<std::string> expected;
    for (const std::string& name : heroClips) expected.push_back(anim::writeClip(buildClip(path + "#" + name)));

    auto buildMatches = [&](const std::string& model, size_t clip){
        ClipData data;
        std::vector<ClipWarning> warnings;
        std::string error;
        std::vector<anim::ModelClipInfo> infos;
        if (!anim::buildModelSubClip(model + "#" + heroClips[clip], data, warnings, &error)) return false;
        if (data.source.path != model) return false;
        data.source.path = path;   // the copies differ only by their name
        return anim::writeClip(data) == expected[clip] && warnings.empty() &&
               anim::listModelFileClips(model, infos, &error) && infos.size() == heroClips.size();
    };

    {
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 6; t++){
            threads.emplace_back([&, t](){
                for (int i = 0; i < 6; i++){
                    if (!buildMatches(path, (size_t)(t + i) % heroClips.size())) failures++;
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
        EXPECT_EQ(failures.load(), 0) << "private parses";
    }

    const int rounds = 12;
    std::vector<std::string> copies(rounds);
    std::atomic<int> current{-1};
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::atomic<int> pooledBuilds{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; t++){
        threads.emplace_back([&, t](){
            size_t i = (size_t)t;
            while (!stop){
                int round = current.load();
                if (round < 0){
                    std::this_thread::yield();
                    continue;
                }
                const std::string& model = copies[round];
                const bool pooled = (bool)ModelPool::getGLTF(MeshSystem::getModelFilenameKey(model));
                if (!buildMatches(model, i++ % heroClips.size())) failures++;
                if (pooled) pooledBuilds++;
            }
        });
    }
    for (int round = 0; round < rounds; round++){
        copies[round] = copyFixture("hero_animated.gltf");
        current = round;
        // let the builders start on the private parse, then pool the copy under them
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Scene scene;
        Model model(&scene);
        if (!scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), copies[round], false, true, false)) failures++;
        // a writer that keeps loadGLTF's rule (the pooled model changes only under the mutex) and
        // whose change a reader without the mutex would see: the buffers are gone for a moment
        std::shared_ptr<tinygltf::Model> pooled = ModelPool::getGLTF(MeshSystem::getModelFilenameKey(copies[round]));
        if (!pooled) failures++;
        for (int cycle = 0; pooled && cycle < 5; cycle++){
            {
                std::lock_guard<std::recursive_mutex> lock(MeshSystem::getPooledGLTFMutex());
                std::vector<tinygltf::Buffer> buffers;
                buffers.swap(pooled->buffers);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                buffers.swap(pooled->buffers);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    stop = true;
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(pooledBuilds.load(), 0) << "some builds read the pooled parse";
}

// The node paths against the entities loadGLTF builds, on a hierarchy that is not trivially
// ordered: children listed out of index order, three same-named siblings (two of them mesh
// nodes), an unnamed child whose unnamed parent has a higher index, a node two parents list
// (the last one wins), a root named like the first one, a node only in a second glTF scene, and a
// non-joint node named like a bone next to it
TEST(ModelSubClips, NodePathsMatchTheLoaderOnAnUnorderedHierarchy){
    nlohmann::json g = nlohmann::json::parse(readFile(fixturePath("hero_animated.gltf")));
    nlohmann::json& nodes = g["nodes"];
    const int base = (int)nodes.size();
    const int hips = 1;
    ASSERT_EQ(nodes[hips]["name"], "mixamorig:Hips");
    // a static mesh of the hero's first primitive (no skin)
    nlohmann::json primitive = g["meshes"][0]["primitives"][0];
    nlohmann::json attributes = {{"POSITION", primitive["attributes"]["POSITION"]}};
    nlohmann::json plain = {{"attributes", attributes}, {"indices", primitive["indices"]}};
    g["meshes"].push_back({{"name", "Plain"}, {"primitives", nlohmann::json::array({plain})}});
    const int plainMesh = (int)g["meshes"].size() - 1;

    auto add = [&](const nlohmann::json& node){
        nodes.push_back(node);
        return (int)nodes.size() - 1;
    };
    const int dupA = add({{"name", "Dup"}, {"mesh", plainMesh}, {"translation", nlohmann::json::array({0.1, 0, 0})}});
    const int dupB = add({{"name", "Dup"}, {"mesh", plainMesh}});
    const int dupC = add({{"name", "Dup"}});
    const int shared = add({{"name", "Shared"}});
    const int lowChild = add(nlohmann::json::object());                      // unnamed
    const int highParent = add({{"children", nlohmann::json::array({lowChild})}});                  // unnamed root, higher index
    const int hero2 = add({{"name", "Hero"}, {"children", nlohmann::json::array({dupC, dupB, dupA, shared})}});
    const int second = add({{"name", "SecondScene"}});
    const int spine2 = add({{"name", "mixamorig:Spine"}});
    nodes[hips]["children"].push_back(shared);   // listed by Hips, then by hero2: hero2 wins
    nodes[hips]["children"].push_back(spine2);
    g["scenes"][0]["nodes"].push_back(hero2);
    g["scenes"][0]["nodes"].push_back(highParent);
    g["scenes"].push_back({{"name", "Other"}, {"nodes", nlohmann::json::array({second})}});

    // one animation moving each of them, on Walk's Hips translation sampler data
    const nlohmann::json walkSampler = g["animations"][0]["samplers"][0];
    nlohmann::json channels = nlohmann::json::array();
    nlohmann::json samplers = nlohmann::json::array();
    const std::vector<int> targets = {dupA, dupB, dupC, shared, lowChild, highParent, hero2, second, spine2};
    for (int target : targets){
        samplers.push_back(walkSampler);
        nlohmann::json channel;
        channel["sampler"] = samplers.size() - 1;
        channel["target"] = {{"node", target}, {"path", "translation"}};
        channels.push_back(channel);
    }
    nlohmann::json cases;
    cases["name"] = "Cases";
    cases["channels"] = channels;
    cases["samplers"] = samplers;
    g["animations"].push_back(cases);
    const std::string model = "models/cases" + std::to_string(base) + "/cases.gltf";
    animtest::writeAsset(model, g.dump(1));

    Scene scene;
    Model holder(&scene);
    const Entity root = holder.getEntity();
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, model, false, false, false));
    const ModelComponent& component = scene.getComponent<ModelComponent>(root);
    ASSERT_EQ(component.nodesIdMapping.size(), nodes.size());
    EXPECT_TRUE(component.meshNodesMapping.count(dupA) && component.meshNodesMapping.count(dupB));

    std::vector<std::string> paths = anim::modelNodePaths(*component.gltfModel);
    ASSERT_EQ(paths.size(), nodes.size());
    for (const auto& node : component.nodesIdMapping){
        EXPECT_EQ(resolve(scene, root, paths[node.first]), node.second) << node.first << ": " << paths[node.first];
    }
    EXPECT_EQ(paths[dupA], "Hero#1/Dup");
    EXPECT_EQ(paths[dupB], "Hero#1/Dup#1");
    EXPECT_EQ(paths[dupC], "Hero#1/Dup#2");
    EXPECT_EQ(paths[shared], "Hero#1/Shared");
    EXPECT_EQ(paths[highParent], "Node " + std::to_string(highParent));
    EXPECT_EQ(paths[lowChild], "Node " + std::to_string(highParent) + "/Node " + std::to_string(lowChild));
    EXPECT_EQ(paths[second], "SecondScene");
    EXPECT_EQ(paths[spine2], "Hero/mixamorig:Hips/mixamorig:Spine#1");

    std::vector<ClipWarning> warnings;
    ClipData clip = buildClip(model + "#Cases", &warnings);
    EXPECT_TRUE(warnings.empty()) << describe(warnings);
    ASSERT_EQ(clip.tracks.size(), targets.size());
    for (int target : targets){
        int track = clip.findTrack(paths[target], ClipChannel::Position);
        ASSERT_GE(track, 0) << paths[target];
        EXPECT_EQ(resolve(scene, root, clip.tracks[track].path), component.nodesIdMapping.at(target)) << paths[target];
    }
    // the hero's own bones keep their paths next to the look-alike
    ClipData walk = buildClip(model + "#Walk");
    for (const ClipTrackData& track : walk.tracks){
        Entity bound = resolve(scene, root, track.path);
        ASSERT_NE(bound, NULL_ENTITY) << track.path;
        EXPECT_TRUE(scene.findComponent<BoneComponent>(bound)) << track.path;
    }
}

// A quantized (non-unit) CUBICSPLINE rotation key is renormalized with its tangents
TEST(ModelSubClips, RenormalizedRotationKeysScaleTheirTangents){
    GltfBuilder gltf;
    gltf.node("Bone");
    const int animation = gltf.animation("Turn");
    const int times = gltf.floats({0.0f, 1.0f}, TINYGLTF_TYPE_SCALAR);
    // per key: in-tangent, value (length 2 for the first key), out-tangent
    const int values = gltf.floats({0.2f, 0, 0, 0,   0, 0, 0, 2,   0, 0.4f, 0, 0,
                                    0.1f, 0, 0, 0,   0, 0, 0, 1,   0, 0.1f, 0, 0}, TINYGLTF_TYPE_VEC4);
    gltf.channel(animation, 0, "rotation", times, values, "CUBICSPLINE");
    ClipData clip;
    std::vector<ClipWarning> warnings;
    std::string error;
    ASSERT_TRUE(anim::buildSubClip(gltf.model, animation, clip, warnings, &error)) << error;
    EXPECT_TRUE(warnings.empty()) << describe(warnings);
    ASSERT_EQ(clip.tracks.size(), 1u);
    const ClipKey& first = clip.tracks[0].keys[0];
    EXPECT_NEAR(first.value[3], 1.0f, 1e-6f);
    EXPECT_NEAR(first.inTangent[0], 0.1f, 1e-6f);
    EXPECT_NEAR(first.outTangent[1], 0.2f, 1e-6f);
    const ClipKey& last = clip.tracks[0].keys[1];
    EXPECT_NEAR(last.inTangent[0], 0.1f, 1e-6f) << "a unit key keeps its tangents";
}

// mergeStaticMeshes merges only models with more than one mesh node: a single-mesh model with the
// flag loads like any other, so a player still switches it; the scene root it stands in for
TEST(ModelSubClips, MergedFlagOnASingleMeshModelStillSwitches){
    Scene scene;
    Model model(&scene);
    const Entity root = model.getEntity();
    scene.getComponent<ModelComponent>(root).mergeStaticMeshes = true;
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(root, fixturePath("hero_tpose.gltf"), false, false, false));
    EXPECT_TRUE(scene.getComponent<ModelComponent>(root).nodesIdMapping.empty());
    EXPECT_FALSE(scene.getSystem<MeshSystem>()->needsNodeHierarchyReload(root));
    scene.addComponent<AnimationPlayerComponent>(root, {});
    EXPECT_TRUE(scene.getSystem<MeshSystem>()->needsNodeHierarchyReload(root));

    Matrix4 sceneRoot;
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->getLegacyRootTransform(root, sceneRoot)) << "a skinned lone mesh";
    EXPECT_EQ(sceneRoot, Matrix4()) << "the fixture's Hero node is identity";
}
