// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator on the headless device (docs/specs/S21-animation.md AC11): the animated hero at
// three controller poses side by side, rendered with the skin matrices the Animator writes (the
// RenderSystem skips its skins): Idle, the Walk at a quarter of its cycle, and a 1D blend tree
// half way between Idle and Walk with an upper-body Wave layer over it; a fourth figure is the
// A-pose robot playing the hero's Walk retargeted. Each Animator holds its state at a fixed
// normalized time (speed 0), so every frame draws the same poses. Golden
// engine/tests/core/golden/animator_poses.png, made on glcore (JASPER_TEST_UPDATE_GOLDEN=1)
// and matched on Vulkan within the cross-backend bound.

#include "AnimatorTestUtil.h"

#include "Engine.h"
#include "Scene.h"
#include "component/ModelComponent.h"
#include "object/Animator.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace animatortest;

namespace {

    CompareOptions goldenTolerance(){
        CompareOptions options;
        const bool goldenBackend = std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
        options.pixelThreshold = 16;
        options.maxBadFraction = goldenBackend ? 0.005 : 0.03;
        options.maxMeanDiff = goldenBackend ? 1.0 : 3.0;
        return options;
    }

    std::string modelPath(const std::string& name){
        return std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models/") + name;
    }

}

TEST(AnimatorGpu, ThreeControllerPosesAndARetargetedWalk){
    JASPER_REQUIRE_GPU();
    AnimationClipPool::clear();
    AnimatorAssetPool::clear();
    Scene scene;
    scene.setBackgroundColor(0.10f, 0.11f, 0.14f, 1.0f);
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 1.1f, 6.0f);
    camera.setTarget(0.0f, 0.9f, 0.0f);
    camera.activate();
    Light light(&scene);
    light.setType(LightType::DIRECTIONAL);
    light.setDirection(-0.4f, -1.0f, -0.6f);
    light.setIntensity(4.0f);
    light.setShadows(false);
    Engine::setScene(&scene);

    const std::string walk = modelPath("hero_animated.gltf") + "#Walk";
    const std::string idle = modelPath("hero_animated.gltf") + "#Idle";
    const std::string wave = modelPath("hero_animated.gltf") + "#Wave";

    // the controller: Idle, Walk, a Speed tree; an upper-body layer playing Wave
    anim::ControllerDef def;
    def.parameters = {parameter("Speed", AnimatorParameterType::Float, 0.5f), parameter("Upper", AnimatorParameterType::Float, 0.0f)};
    anim::LayerDef base;
    base.name = "Base Layer";
    anim::StateDef blend;
    blend.name = "Blend";
    anim::BlendTreeDef tree;
    tree.type = BlendTreeType::Simple1D;
    tree.parameter = "Speed";
    anim::BlendChildDef a;
    a.motion.clip.path = idle;
    anim::BlendChildDef b;
    b.motion.clip.path = walk;
    b.threshold = 1.0f;
    tree.children = {a, b};
    blend.motion.tree.push_back(tree);
    base.machine.states = {state("Idle", idle), state("Walk", walk), blend};
    def.layers.push_back(base);
    anim::AvatarMaskDef mask;
    for (bool& part : mask.bodyParts) part = false;
    mask.bodyParts[(int)AvatarBodyPart::LeftArm] = mask.bodyParts[(int)AvatarBodyPart::RightArm] = true;
    mask.bodyParts[(int)AvatarBodyPart::LeftHand] = mask.bodyParts[(int)AvatarBodyPart::RightHand] = true;
    const std::string maskPath = animtest::writeAsset("animator/gpu/arms.mask", anim::writeMask(mask));
    anim::LayerDef upper;
    upper.name = "Upper";
    upper.mask.path = maskPath;
    upper.machine.states = {state("Off", ""), state("Wave", wave)};
    def.layers.push_back(upper);
    const std::string controllerPath = animtest::writeAsset("animator/gpu/poses.controller", anim::writeController(def));

    struct Figure{
        const char* model;
        float x;
        const char* state;
        float time;
        bool waving;
    };
    const Figure figures[] = {
        {"hero_animated.gltf", -2.4f, "Idle", 0.3f, false},
        {"hero_animated.gltf", -0.8f, "Walk", 0.25f, false},
        {"hero_animated.gltf", 0.8f, "Blend", 0.6f, true},
        {"robot_apose.gltf", 2.4f, "Walk", 0.25f, false},
    };
    std::vector<std::unique_ptr<Model>> models;
    std::vector<std::unique_ptr<Animator>> animators;
    for (const Figure& figure : figures){
        models.push_back(std::make_unique<Model>(&scene));
        Model& model = *models.back();
        // as a scene's ModelComponent loads (the file's root nodes under the model's entity)
        ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), modelPath(figure.model), false, false, false));
        model.setPosition(figure.x, 0.0f, 0.0f);
        animators.push_back(std::make_unique<Animator>(&scene, model.getEntity()));
        Animator& animator = *animators.back();
        animator.setController(controllerPath);
        animator.setSpeed(0.0f);
    }
    Scene* current = &scene;
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
    // bound: each holds its state at its time (speed 0: no time passes)
    for (size_t i = 0; i < animators.size(); i++){
        Animator& animator = *animators[i];
        ASSERT_TRUE(animator.isLoaded()) << figures[i].model;
        animator.play(figures[i].state, 0, figures[i].time);
        animator.play(figures[i].waving ? "Wave" : "Off", 1, 0.4f);
        animator.setLayerWeight(1, figures[i].waving ? 1.0f : 0.0f);
        if (std::string(figures[i].model) == "robot_apose.gltf") EXPECT_TRUE(animator.isHumanoid());
    }
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
    for (size_t i = 0; i < animators.size(); i++){
        EXPECT_EQ(animators[i]->getCurrentState(0).name, figures[i].state);
        EXPECT_NEAR(animators[i]->getCurrentState(0).normalizedTime, figures[i].time, 1e-5f) << "held";
    }
    // the skins are the Animator's
    for (const auto& model : models){
        const ModelComponent& component = scene.getComponent<ModelComponent>(model->getEntity());
        ASSERT_FALSE(component.skinBindings.empty());
        EXPECT_TRUE(component.skinBindings[0].animatorDriven);
    }
    EXPECT_GOLDEN_OPTIONS("animator_poses", goldenTolerance());
    animators.clear();
    models.clear();
    Engine::removeScene(&scene);
}
