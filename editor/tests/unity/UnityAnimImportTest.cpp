// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// unity/UnityAnimImport and unity/UnityControllerImport (docs/specs/S94-rendering-anim-gaps.md
// F35, F36, AC33, AC34) on hand-written Unity YAML (fixtures/unity: no game or Asset Store file):
// a clip's position, Euler, quaternion and scale curves, Transform, material, GameObject and
// AudioSource float curves sampled at 7 times against Unity's Hermite curves (X mirrored), tangent
// modes kept only where the engine computes the same slopes, events, loop and the root transform
// settings; a muscle-only clip refused; a controller with sub-state machines, every transition
// kind, Solo, 1D / 2D / nested blend trees and a masked additive layer, its motions and mask
// imported beside it through the fixture project's GUIDs and reused on a second import; a mask's
// body parts and transform list.

#include <gtest/gtest.h>

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/CompiledController.h"
#include "animation/KeyframeSampling.h"
#include "io/AtomicFile.h"
#include "unity/UnityAnimImport.h"
#include "unity/UnityControllerImport.h"

#include <atomic>
#include <cmath>
#include <random>

using namespace jasperengine;
using namespace jasperengine::editor::unity;
namespace fs = std::filesystem;
using UnityAnimJson = nlohmann::json;

namespace {

    const fs::path FIXTURES = fs::path(JASPER_TEST_FIXTURES_DIR) / "unity";
    const fs::path UNITY_ANIMATIONS = FIXTURES / "Project" / "Assets" / "Animations";
    const float TIMES[] = {0.0f, 0.1f, 0.25f, 0.4f, 0.5f, 0.75f, 1.0f};

    // Unity's ease of a Hermite segment with zero slopes at both ends
    double hermiteEase(double s) { return 3 * s * s - 2 * s * s * s; }

    struct UnityImportTemp {
        fs::path root;
        UnityImportTemp() {
            static std::atomic<int> counter{0};
            std::random_device rd;
            root = fs::temp_directory_path() / ("jasper-unity-anim-" + std::to_string(rd()) + "-" + std::to_string(counter++)) / "assets";
            fs::create_directories(root);
        }
        ~UnityImportTemp() {
            std::error_code ec;
            fs::remove_all(root.parent_path(), ec);
        }
    };

    anim::TrackSample sampleTrackAt(const AnimationClip& clip, int track, float t) {
        anim::TrackSample sample;
        EXPECT_GE(track, 0);
        if (track >= 0) {
            EXPECT_TRUE(anim::sampleTrack(clip.getTracks()[(size_t)track], t, sample));
        }
        return sample;
    }

    double unitQuatDot(const Quaternion& a, double x, double y, double z, double w) {
        const double la = std::sqrt((double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z + (double)a.w * a.w);
        const double lb = std::sqrt(x * x + y * y + z * z + w * w);
        return std::fabs((a.x * x + a.y * y + a.z * z + a.w * w) / (la * lb));
    }

    const ClipTrackData* trackNamed(const ClipData& data, const std::string& path, ClipChannel channel) {
        const int index = data.findTrack(path, channel);
        return index < 0 ? nullptr : &data.tracks[(size_t)index];
    }

    const anim::StateDef* stateNamed(const anim::StateMachineDef& machine, const std::string& name) {
        for (const anim::StateDef& s : machine.states) {
            if (s.name == name) return &s;
        }
        return nullptr;
    }

    std::string importedText(const AssetImportResult& result, const std::string& name) {
        for (const auto& [path, text] : result.files) {
            if (path.filename() == name) return text;
        }
        return std::string();
    }

}

// AC33
TEST(UnityAnimImportTest, TransformAndFloatCurves) {
    const AnimImportResult result = importAnimFile(FIXTURES / "Idle.anim");
    ASSERT_TRUE(result.ok) << result.error;
    const ClipData& data = result.clip;
    EXPECT_EQ(data.wrap, ClipWrapMode::Loop) << "m_LoopTime";
    EXPECT_FLOAT_EQ(data.length, 1.0f);
    EXPECT_FLOAT_EQ(data.frameRate, 60.0f);

    // canonical text that reads back
    const std::string text = anim::writeClip(data);
    const anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    const std::shared_ptr<const AnimationClip> clip = AnimationClip::build(*parsed.data);
    ASSERT_TRUE(clip);

    const int hipsPosition = clip->findTrack("Hips", ClipChannel::Position);
    const int hipsScale = clip->findTrack("Hips", ClipChannel::Scale);
    const int foot = clip->findTrack("Hips/Leg/Foot", ClipChannel::Position);
    const int spine = clip->findTrack("Hips/Spine", ClipChannel::Rotation);
    const int head = clip->findTrack("Hips/Spine/Head", ClipChannel::Rotation);
    const int leg = clip->findTrack("Hips/Leg", ClipChannel::Position);
    const int alpha = parsed.data->findPropertyTrack("Hips/Spine/Head", "mesh", "submeshes[0].material.baseColorFactor.a");
    const int active = parsed.data->findPropertyTrack("Hips/Leg", "gameobject", "active");
    const int volume = parsed.data->findPropertyTrack("", "audiosource", "volume");
    ASSERT_GE(alpha, 0) << text;
    ASSERT_GE(active, 0) << text;
    ASSERT_GE(volume, 0) << text;

    for (float t : TIMES) {
        SCOPED_TRACE("t = " + std::to_string(t));
        const double h = hermiteEase(t);
        // m_PositionCurves (Clamped Auto, flat ends), X mirrored
        const anim::TrackSample p = sampleTrackAt(*clip, hipsPosition, t);
        EXPECT_NEAR(p.vector.x, -(1.0 + h), 1e-4);
        EXPECT_NEAR(p.vector.y, 2.0, 1e-4);
        EXPECT_NEAR(p.vector.z, 3.0 + 2.0 * h, 1e-4);
        // m_ScaleCurves: up to 2 at 0.5 and back
        const double hs = t <= 0.5f ? 1.0 + hermiteEase(t / 0.5) : 2.0 - hermiteEase((t - 0.5) / 0.5);
        EXPECT_NEAR(sampleTrackAt(*clip, hipsScale, t).vector.y, hs, 1e-4);
        // Auto keys whose stored slope (3 at the end) the engine would not compute: kept as stored
        EXPECT_NEAR(sampleTrackAt(*clip, foot, t).vector.y, (double)t * t * t, 1e-4);
        // m_EulerCurves: y = 90 t degrees, mirrored to -90 t
        const double half = -45.0 * t * 3.14159265358979 / 180.0;
        EXPECT_GE(unitQuatDot(sampleTrackAt(*clip, spine, t).rotation, 0, std::sin(half), 0, std::cos(half)), 1.0 - 1e-6);
        // m_RotationCurves: linear components, normalized, mirrored
        EXPECT_GE(unitQuatDot(sampleTrackAt(*clip, head, t).rotation, 0, -0.70710677 * t, 0, 1.0 - 0.29289323 * t), 1.0 - 1e-6);
        // Transform float curves merged at the union of their key times
        const anim::TrackSample l = sampleTrackAt(*clip, leg, t);
        EXPECT_NEAR(l.vector.x, -t, 1e-4);
        EXPECT_NEAR(l.vector.y, t <= 0.5f ? 2.0 * t : 2.0 - 2.0 * t, 1e-4);
        EXPECT_NEAR(l.vector.z, 0.5, 1e-4);
        // float curves: a material colour channel, a stepped m_IsActive, an AudioSource volume
        const anim::TrackSample a = sampleTrackAt(*clip, alpha, t);
        ASSERT_EQ(a.weights.size(), 1u);
        EXPECT_NEAR(a.weights[0], 1.0 - h, 1e-4);
        EXPECT_NEAR(sampleTrackAt(*clip, active, t).weights[0], t < 0.5f ? 1.0 : 0.0, 1e-6);
        EXPECT_NEAR(sampleTrackAt(*clip, volume, t).weights[0], 0.25 + 0.5 * h, 1e-4);
    }

    // tangent modes: kept where the engine computes Unity's slopes, else Free with the slopes
    const ClipTrackData* position = trackNamed(*parsed.data, "Hips", ClipChannel::Position);
    ASSERT_TRUE(position);
    EXPECT_EQ(position->keys.front().tangentMode, ClipTangentMode::ClampedAuto);
    EXPECT_EQ(trackNamed(*parsed.data, "Hips/Spine/Head", ClipChannel::Rotation)->keys.front().tangentMode, ClipTangentMode::Linear);
    EXPECT_EQ(trackNamed(*parsed.data, "Hips/Leg/Foot", ClipChannel::Position)->keys.back().tangentMode, ClipTangentMode::Free);
    const ClipTrackData* legData = trackNamed(*parsed.data, "Hips/Leg", ClipChannel::Position);
    ASSERT_TRUE(legData);
    ASSERT_EQ(legData->keys.size(), 3u) << "x and z keyed at 0 and 1, y also at 0.5";

    // events: Unity's one parameter
    ASSERT_EQ(parsed.data->events.size(), 2u);
    EXPECT_EQ(parsed.data->events[0].function, "Footstep");
    EXPECT_FLOAT_EQ(parsed.data->events[0].time, 0.25f);
    ASSERT_EQ(parsed.data->events[0].args.size(), 1u);
    EXPECT_EQ(std::get<std::string>(parsed.data->events[0].args[0]), "left");
    EXPECT_EQ(parsed.data->events[1].function, "OnHit");
    ASSERT_EQ(parsed.data->events[1].args.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(parsed.data->events[1].args[0]), 2.5);

    // the root transform: bake flags, Based Upon and the offsets in the clip (S94 F32), and reported
    const anim::ClipRootMotion rootMotion = anim::readClipRootMotion(parsed.data->unknownKeys);
    EXPECT_TRUE(rootMotion.present);
    EXPECT_TRUE(rootMotion.bakeRotation);
    EXPECT_TRUE(rootMotion.bakePositionY);
    EXPECT_FALSE(rootMotion.bakePositionXZ);
    EXPECT_EQ(rootMotion.rotationBasedUpon, anim::RootRotationBasis::BodyOrientation);
    EXPECT_NEAR(rootMotion.rotationOffset, 10.0f, 1e-6f);
    EXPECT_EQ(rootMotion.positionYBasedUpon, anim::RootPositionYBasis::Original);
    EXPECT_EQ(rootMotion.positionXZBasedUpon, anim::RootPositionXZBasis::CenterOfMass);
    const UnityAnimJson& report = result.report;
    EXPECT_EQ(report["root_motion"]["rotation_based_upon"], "body_orientation");
    EXPECT_NEAR(report["root_motion"]["rotation_offset"].get<double>(), 10.0, 1e-6);
    EXPECT_EQ(report["root_motion"]["position_y_based_upon"], "original");
    EXPECT_EQ(report["root_motion"]["position_xz_based_upon"], "center_of_mass");

    // what did not map is reported
    EXPECT_EQ(report["muscle_curves"]["count"], 1);
    ASSERT_EQ(report["unmapped"].size(), 1u) << report.dump(2);
    EXPECT_NE(report["unmapped"][0]["binding"].get<std::string>().find("MonoBehaviour.speed"), std::string::npos);
    EXPECT_EQ(report["tracks"], data.tracks.size());

    // mirror_x off: Unity's values as they are
    AnimImportOptions unmirrored;
    unmirrored.mirrorX = false;
    const AnimImportResult plain = importAnimFile(FIXTURES / "Idle.anim", unmirrored);
    ASSERT_TRUE(plain.ok);
    const std::shared_ptr<const AnimationClip> plainClip = AnimationClip::build(plain.clip);
    EXPECT_NEAR(sampleTrackAt(*plainClip, plainClip->findTrack("Hips", ClipChannel::Position), 0.5f).vector.x, 1.5, 1e-4);
}

// AC33: a clip of humanoid muscle curves only
TEST(UnityAnimImportTest, MuscleClipRefused) {
    const AnimImportResult result = importAnimFile(FIXTURES / "HumanoidWave.anim");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("humanoid (muscle-space) clip"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("6 muscle curves"), std::string::npos) << result.error;
}

// AC34
TEST(UnityControllerImportTest, StatesTransitionsBlendTrees) {
    UnityImportTemp assets;
    AssetImportOptions options;
    options.file = UNITY_ANIMATIONS / "Hero.controller";
    options.dest = assets.root / "anim" / "Hero.controller";
    options.engineAssets = assets.root;
    const AssetImportResult result = importUnityAnimationAsset(options);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.files.size(), 5u) << result.report.dump(2) << "\nthe controller, three clips once each, the mask";
    EXPECT_EQ(result.files.front().first, options.dest);

    const anim::ControllerParseResult parsed = anim::parseController(result.files.front().second);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    const anim::ControllerDef& def = parsed.data;
    ASSERT_EQ(def.parameters.size(), 6u);
    EXPECT_EQ(def.parameters[0].name, "Speed");
    EXPECT_EQ(def.parameters[0].type, AnimatorParameterType::Float);
    EXPECT_FLOAT_EQ(def.parameters[0].defaultValue, 0.5f);
    EXPECT_EQ(def.parameters[1].type, AnimatorParameterType::Bool);
    EXPECT_FLOAT_EQ(def.parameters[1].defaultValue, 1.0f);
    EXPECT_EQ(def.parameters[2].type, AnimatorParameterType::Trigger);
    EXPECT_EQ(def.parameters[3].type, AnimatorParameterType::Int);
    EXPECT_FLOAT_EQ(def.parameters[3].defaultValue, 2.0f);

    ASSERT_EQ(def.layers.size(), 2u);
    const anim::StateMachineDef& base = def.layers[0].machine;
    EXPECT_FLOAT_EQ(def.layers[0].weight, 1.0f) << "Unity stores 0 for the base layer";
    EXPECT_EQ(base.defaultState, "Idle");
    ASSERT_EQ(base.states.size(), 3u);
    const anim::StateDef* idle = stateNamed(base, "Idle");
    ASSERT_TRUE(idle);
    EXPECT_EQ(idle->motion.clip.path, "anim/Idle.anim");
    ASSERT_EQ(idle->transitions.size(), 2u);
    const anim::TransitionDef& toLocomotion = idle->transitions[0];
    EXPECT_EQ(toLocomotion.to, "Locomotion");
    ASSERT_EQ(toLocomotion.conditions.size(), 1u);
    EXPECT_EQ(toLocomotion.conditions[0].mode, AnimatorConditionMode::Greater);
    EXPECT_FLOAT_EQ(toLocomotion.conditions[0].threshold, 0.1f);
    EXPECT_FLOAT_EQ(toLocomotion.duration, 0.2f);
    EXPECT_FALSE(toLocomotion.hasExitTime);
    EXPECT_EQ(toLocomotion.interruptionSource, TransitionInterruptionSource::Current);
    EXPECT_FALSE(toLocomotion.mute) << "the Solo transition";
    EXPECT_EQ(idle->transitions[1].conditions[0].mode, AnimatorConditionMode::Equals);
    EXPECT_TRUE(idle->transitions[1].mute) << "Unity's Solo: the other transitions of the state are not taken";

    // blend trees: 1D, 2D with a nested 1D
    const anim::StateDef* locomotion = stateNamed(base, "Locomotion");
    ASSERT_TRUE(locomotion && locomotion->motion.isTree());
    EXPECT_EQ(locomotion->speedParameter, "Speed");
    const anim::BlendTreeDef& tree = locomotion->motion.tree[0];
    EXPECT_EQ(tree.type, BlendTreeType::Simple1D);
    EXPECT_EQ(tree.parameter, "Speed");
    ASSERT_EQ(tree.children.size(), 2u);
    EXPECT_EQ(tree.children[1].motion.clip.path, "anim/Run.anim");
    EXPECT_FLOAT_EQ(tree.children[1].threshold, 1.0f);
    EXPECT_FLOAT_EQ(tree.children[1].timeScale, 1.25f);
    const anim::StateDef* strafe = stateNamed(base, "Strafe");
    ASSERT_TRUE(strafe && strafe->motion.isTree());
    const anim::BlendTreeDef& tree2 = strafe->motion.tree[0];
    EXPECT_EQ(tree2.type, BlendTreeType::SimpleDirectional2D);
    EXPECT_EQ(tree2.parameterY, "MoveY");
    ASSERT_EQ(tree2.children.size(), 3u);
    EXPECT_FLOAT_EQ(tree2.children[1].position.x, 1.0f);
    ASSERT_TRUE(tree2.children[2].motion.isTree()) << "a nested blend tree";
    EXPECT_EQ(tree2.children[2].motion.tree[0].children.size(), 2u);

    // the sub-state machine, its exit, the entry and any-state transitions
    ASSERT_EQ(base.subMachines.size(), 1u);
    const anim::StateMachineDef& air = base.subMachines[0];
    EXPECT_EQ(air.name, "Air");
    const anim::StateDef* jump = stateNamed(air, "Jump");
    const anim::StateDef* fall = stateNamed(air, "Fall");
    ASSERT_TRUE(jump && fall);
    EXPECT_TRUE(jump->motion.empty()) << "an .fbx clip is reported, not guessed";
    ASSERT_EQ(jump->transitions.size(), 1u);
    EXPECT_EQ(jump->transitions[0].to, "Fall");
    EXPECT_TRUE(jump->transitions[0].hasExitTime);
    EXPECT_FLOAT_EQ(jump->transitions[0].exitTime, 0.9f);
    EXPECT_EQ(fall->transitions[0].to, "Exit");
    EXPECT_TRUE(fall->mirror);
    EXPECT_TRUE(fall->footIK);
    EXPECT_EQ(fall->tag, "air");
    EXPECT_FLOAT_EQ(fall->speed, 1.5f);
    ASSERT_EQ(base.anyState.size(), 1u);
    EXPECT_EQ(base.anyState[0].to, "/Air/Jump");
    EXPECT_EQ(base.anyState[0].conditions[0].mode, AnimatorConditionMode::If);
    ASSERT_EQ(base.entry.size(), 1u);
    EXPECT_EQ(base.entry[0].to, "Air");
    EXPECT_EQ(base.entry[0].conditions[0].mode, AnimatorConditionMode::IfNot);
    ASSERT_EQ(air.exitTransitions.size(), 1u);
    EXPECT_EQ(air.exitTransitions[0].to, "Idle");

    // the masked additive layer; its Walk is the base layer's file
    const anim::LayerDef& upper = def.layers[1];
    EXPECT_FLOAT_EQ(upper.weight, 0.6f);
    EXPECT_EQ(upper.blending, AnimatorLayerBlending::Additive);
    EXPECT_EQ(upper.mask.path, "anim/UpperBody.mask");
    EXPECT_EQ(upper.mask.guid, anim::readAnimatorAssetGuid(importedText(result, "UpperBody.mask")));
    EXPECT_EQ(upper.machine.states[0].motion.clip.guid, tree.children[0].motion.clip.guid);
    EXPECT_EQ(tree.children[0].motion.clip.guid, anim::readClipGuid(importedText(result, "Walk.anim")));

    // every target resolves
    const std::shared_ptr<const anim::CompiledController> compiled = anim::compileController(def);
    for (const ClipWarning& w : compiled->diagnostics) {
        EXPECT_EQ(w.message.find("no state or state machine named"), std::string::npos) << w.path << ": " << w.message;
    }
    // the model clip is reported with its name
    bool jumpReported = false;
    for (const UnityAnimJson& miss : result.report["unmapped"]) jumpReported = jumpReported || miss["reason"].get<std::string>().find("\"Jump\"") != std::string::npos;
    EXPECT_TRUE(jumpReported) << result.report.dump(2);

    // a second import beside the first reuses its clips and mask
    for (const auto& [path, text] : result.files) {
        ASSERT_TRUE(io::writeFileAtomic(path, text).written());
    }
    options.dest = assets.root / "anim" / "Hero 2.controller";
    const AssetImportResult again = importUnityAnimationAsset(options);
    ASSERT_TRUE(again.ok) << again.error;
    EXPECT_EQ(again.files.size(), 1u) << again.report.dump(2);
    for (const UnityAnimJson& c : again.report["clips"]) {
        EXPECT_TRUE(c["reused"].get<bool>()) << c.dump();
    }
    const anim::ControllerParseResult second = anim::parseController(again.files.front().second);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(stateNamed(second.data.layers[0].machine, "Idle")->motion.clip.guid, idle->motion.clip.guid);
}

// AC34
TEST(UnityMaskImportTest, BodyPartsAndPaths) {
    UnityImportTemp assets;
    AssetImportOptions options;
    options.file = UNITY_ANIMATIONS / "UpperBody.mask";
    options.dest = assets.root / "UpperBody.mask";
    options.engineAssets = assets.root;
    const AssetImportResult result = importUnityAnimationAsset(options);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.files.size(), 1u);
    const anim::MaskParseResult parsed = anim::parseMask(result.files[0].second);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    const anim::AvatarMaskDef& mask = parsed.data;
    using P = AvatarBodyPart;
    const std::pair<P, float> expected[] = {{P::Root, 0}, {P::Body, 1}, {P::Head, 1}, {P::LeftLeg, 0}, {P::RightLeg, 0}, {P::LeftArm, 1},
                                            {P::LeftHand, 1}, {P::RightArm, 1}, {P::RightHand, 1}, {P::LeftFingers, 1}, {P::RightFingers, 0}};
    for (const auto& [part, weight] : expected) {
        ASSERT_TRUE(mask.hasPart(part)) << anim::bodyPartName(part);
        EXPECT_FLOAT_EQ(mask.partWeight(part), weight) << anim::bodyPartName(part);
    }
    EXPECT_FLOAT_EQ(mask.defaultWeight, 0.0f) << "a transform list: unlisted transforms do not move";
    ASSERT_EQ(mask.paths.size(), 3u);
    EXPECT_GE(mask.findPath(""), 0);
    EXPECT_GE(mask.findPath("Armature"), 0);
    EXPECT_GE(mask.findPath("Armature/Hips/Spine"), 0);
    EXPECT_LT(mask.findPath("Armature/Hips"), 0);
    bool noted = false;
    for (const UnityAnimJson& note : result.report["notes"]) noted = noted || note["code"] == "transforms-and-body-parts";
    EXPECT_TRUE(noted) << result.report.dump(2);
}
