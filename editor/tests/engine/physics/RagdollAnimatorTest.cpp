// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A ragdoll pauses the Animator of its model (docs/specs/S94-rendering-anim-gaps.md F37, AC35):
// simulated, the Animator stops writing and the skins go back to the render system, so they
// follow the bodies' bones; animated again, the Animator plays and drives the skins. Written red
// first: the Animator kept writing its clip over the falling bones and kept the skins.

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/ClipBinding.h"
#include "component/AnimatorComponent.h"
#include "component/ModelComponent.h"
#include "component/RagdollComponent.h"
#include "io/AtomicFile.h"
#include "object/Model.h"
#include "object/physics/Ragdoll.h"
#include "physics/RagdollBuilder.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "subsystem/AnimatorSystem.h"
#include "subsystem/MeshSystem.h"

#include <filesystem>
#include <string>

namespace RagdollAnimatorTest_cpp {

using namespace jasperengine;
using namespace jasperengine::physics3dtest;

namespace {

    std::filesystem::path scratchDir(){
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "jasper-ragdoll-animator";
        std::filesystem::create_directories(dir);
        return dir;
    }

    void writeText(const std::filesystem::path& path, const std::string& text){
        const io::WriteResult result = io::writeFileAtomic(path, text);
        ASSERT_TRUE(result.ok()) << io::describeWriteError(result);
    }

}

TEST(RagdollAnimatorTest, RagdollPausesAnimator){
    AnimationClipPool::clear();
    AnimatorAssetPool::clear();
    World3D world;
    world.floor();
    Model model(&world.scene);
    const Entity hero = model.getEntity();
    ASSERT_TRUE(world.scene.getSystem<MeshSystem>()->loadGLTF(
        hero, JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models/hero_tpose.gltf", false, false, false));
    world.scene.getComponent<ModelComponent>(hero).needUpdateModel = false;
    ASSERT_FALSE(world.scene.getComponent<ModelComponent>(hero).skinBindings.empty());

    // the ragdoll on the model's bones (what the wizard and create_ragdoll build)
    const RagdollBones bones = RagdollBuilder::guessBones(&world.scene, hero);
    RagdollBuildSettings settings;
    const RagdollBuildResult built = RagdollBuilder::build(&world.scene, hero, bones, settings);
    ASSERT_TRUE(built.ok) << built.error;
    ASSERT_TRUE(RagdollBuilder::apply(&world.scene, hero, built));

    // an Animator holding the hips where the T-pose has them
    Entity hips = NULL_ENTITY;
    for (const auto& entry : anim::descendantPaths(&world.scene, hero)){
        if (world.scene.getEntityNameRef(entry.second) == "mixamorig:Hips") hips = entry.second;
    }
    ASSERT_NE(hips, NULL_ENTITY);
    std::string hipsPath;
    ASSERT_TRUE(anim::relativePath(&world.scene, hero, hips, hipsPath));
    const Vector3 rest = world.transform(hips).position;
    ClipData clip;
    clip.wrap = ClipWrapMode::Loop;
    ClipTrackData track;
    track.path = hipsPath;
    track.channel = ClipChannel::Position;
    for (float t : {0.0f, 1.0f}){
        ClipKey key;
        key.time = t;
        key.value = {rest.x, rest.y, rest.z};
        track.keys.push_back(key);
    }
    clip.tracks.push_back(track);
    const std::filesystem::path dir = scratchDir();
    writeText(dir / "hold.anim", anim::writeClip(clip));
    anim::ControllerDef controller;
    anim::LayerDef layer;
    layer.name = "Base Layer";
    anim::StateDef state;
    state.name = "Hold";
    state.motion.clip.path = (dir / "hold.anim").string();
    layer.machine.states.push_back(state);
    controller.layers.push_back(layer);
    writeText(dir / "hold.controller", anim::writeController(controller));
    AnimatorComponent animator;
    animator.controller = (dir / "hold.controller").string();
    world.scene.addComponent<AnimatorComponent>(hero, animator);

    AnimatorSystem& animators = *world.scene.getSystem<AnimatorSystem>();
    auto frame = [&](int count){
        for (int i = 0; i < count; i++){
            world.step();
            animators.update(STEP);
            world.resolve();
        }
    };
    world.load();
    frame(3);
    const ModelSkinBinding& skin = world.scene.getComponent<ModelComponent>(hero).skinBindings[0];
    ASSERT_TRUE(world.scene.getComponent<AnimatorComponent>(hero).runtime &&
                world.scene.getComponent<AnimatorComponent>(hero).runtime->bound);
    EXPECT_TRUE(skin.animatorDriven) << "animated: the Animator writes the skin";
    const float standing = world.transform(hips).worldPosition.y;

    Ragdoll ragdoll(&world.scene, hero);
    ragdoll.setSimulated(true);
    EXPECT_FALSE(world.scene.getComponent<AnimatorComponent>(hero).enabled) << "the ragdoll paused the Animator";
    EXPECT_FALSE(world.scene.getComponent<ModelComponent>(hero).skinBindings[0].animatorDriven)
        << "the skin follows the bones (the render system computes it from them)";
    frame(120);
    EXPECT_GT(standing - world.transform(hips).worldPosition.y, 0.1f) << "the bodies moved the hips as it fell";
    EXPECT_FALSE(world.scene.getComponent<ModelComponent>(hero).skinBindings[0].animatorDriven);

    ragdoll.setSimulated(false);
    EXPECT_TRUE(world.scene.getComponent<AnimatorComponent>(hero).enabled) << "resumed on release";
    frame(1);
    EXPECT_TRUE(world.scene.getComponent<ModelComponent>(hero).skinBindings[0].animatorDriven) << "the Animator drives the skin again";
    EXPECT_NEAR(world.transform(hips).position.y, rest.y, 1e-4f) << "back on the clip";
    AnimationClipPool::clear();
    AnimatorAssetPool::clear();
}

}  // namespace RagdollAnimatorTest_cpp
