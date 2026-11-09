// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator through AnimatorSystem (docs/specs/S21-animation.md AC1, AC4, AC6, AC8, AC10-AC12):
// clips write what the Animation Player writes, layers with masks and additive, events (leader,
// all, seeks, montages, once per Animator), root motion across loop wraps, montage sections,
// bones written (All / Exposed), skin matrices equal the render system's, culling, overrides,
// hot reload, ownership, parallel equals serial, and no allocation in the steady state.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"
#include "component/Transform.h"
#include "support/AllocationCounter.h"

#include "Engine.h"
#include "component/AudioSourceComponent.h"
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "object/AnimationPlayer.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/RenderSystem.h"

#include <cmath>
#include <cstring>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kDt = 1.0f / 60.0f;

    std::string fixturePath(const std::string& name){
        return std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models") + "/" + name;
    }

    class AnimatorSystemTest : public ::testing::Test, public AnimatorFixture {
    protected:
        std::vector<AnimationEvent> events;

        void listen(Entity on = NULL_ENTITY){
            scene.getComponent<AnimatorComponent>(on == NULL_ENTITY ? root : on).onEvent.add("test", [this](const AnimationEvent& e){
                events.push_back(e);
            });
        }

        size_t count(const std::string& function) const{
            size_t n = 0;
            for (const AnimationEvent& e : events) n += e.functionName == function;
            return n;
        }

        // the world matrices of every Transform, parents first (what RenderSystem's pass does)
        void resolveTransforms(){
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                transform.needUpdate = false;
            }
        }
    };

    ClipTrackData propertyTrack(const std::string& path, const std::string& component, const std::string& name, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Property;
        t.component = component;
        t.property = name;
        t.keys = std::move(keys);
        return t;
    }

    ControllerDef oneState(const std::string& clip, AnimatorStateLoop loop = AnimatorStateLoop::Inherit){
        StateDef s = state("Play", clip);
        s.loop = loop;
        return controller({}, {s});
    }

}

// AC1: one state at weight 1 writes the same local TRS as the Animation Player, relative tracks
// and rotations included
TEST_F(AnimatorSystemTest, MatchesThePlayer){
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks = {
        track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(0.5f, {0.3f, 1.2f, 0}, EaseType::QUAD_IN_OUT), key(1, {0, 1, 0})}),
        track("Hips/Spine", ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(1, {0, 0.3826834f, 0, 0.9238795f})}),
        track("Hips/Leg", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 0, 0.2f})}, true),
        track("Hips/Leg/Foot", ClipChannel::Scale, {key(0, {1, 1, 1}), key(1, {1.5f, 1, 2})}),
    };
    const std::string path = clip("walk", data);
    // a twin hierarchy played by an Animation Player
    const Entity twin = node(NULL_ENTITY, "Twin", Vector3(0, 0, 0));
    const Entity twinHips = node(twin, "Hips", Vector3(0, 1, 0));
    const Entity twinSpine = node(twinHips, "Spine", Vector3(0, 0.3f, 0));
    node(twinSpine, "Head", Vector3(0, 0.4f, 0));
    const Entity twinLeg = node(twinHips, "Leg", Vector3(0.1f, -0.5f, 0));
    const Entity twinFoot = node(twinLeg, "Foot", Vector3(0, -0.45f, 0));
    AnimationPlayer player(&scene, twin);
    player.addClip(path, "walk");
    player.setPlayAutomatically(true);
    animator(controllerFile("c", oneState(path)));
    // binary-exact steps: both players reach the loop's end at exactly the same time
    const float dt = 1.0f / 64.0f;
    for (int i = 0; i < 90; i++){
        scene.getSystem<ActionSystem>()->update(dt);
        step(dt);
        const std::pair<Entity, Entity> pairs[] = {{hips, twinHips}, {spine, twinSpine}, {leg, twinLeg}, {foot, twinFoot}};
        for (const auto& p : pairs){
            const Transform& a = scene.getComponent<Transform>(p.first);
            const Transform& b = scene.getComponent<Transform>(p.second);
            EXPECT_LE((a.position - b.position).length(), 1e-6f) << "frame " << i;
            EXPECT_LE((a.scale - b.scale).length(), 1e-6f);
            EXPECT_GE(std::fabs(a.rotation.dot(b.rotation)), 1.0f - 1e-6f);
        }
    }
}

TEST_F(AnimatorSystemTest, LayersWithMaskAndAdditive){
    const std::string base = clip("base", hold("Hips", Vector3(1, 1, 0)));
    ClipData spineClip;
    spineClip.wrap = ClipWrapMode::Loop;
    spineClip.tracks = {track("Hips/Spine", ClipChannel::Position, {key(0, {0, 2, 0}), key(1, {0, 2, 0})}),
                        track("Hips", ClipChannel::Position, {key(0, {5, 5, 5}), key(1, {5, 5, 5})})};
    const std::string upper = clip("upper", spineClip);
    ClipData nod;
    nod.wrap = ClipWrapMode::Loop;
    nod.tracks = {track("Hips/Spine/Head", ClipChannel::Position, {key(0, {0, 0.4f, 0}), key(1, {0, 1.4f, 0})})};
    const std::string additive = clip("nod", nod);

    AvatarMaskDef mask;
    mask.paths = {{"Hips/Spine", 1.0f}, {"Hips", 0.0f}};
    const std::string maskPath = maskFile("upper", mask);

    ControllerDef def = controller({}, {state("Base", base)});
    LayerDef override;
    override.name = "Upper";
    override.weight = 1.0f;
    override.mask.path = maskPath;
    override.machine.states.push_back(state("Upper", upper));
    def.layers.push_back(override);
    LayerDef add;
    add.name = "Nod";
    add.weight = 0.5f;
    add.blending = AnimatorLayerBlending::Additive;
    add.machine.states.push_back(state("Nod", additive));
    def.layers.push_back(add);
    animator(controllerFile("c", def));
    step(0.0);
    step(0.5);
    EXPECT_NEAR(position(hips).x, 1.0f, 1e-6f) << "masked out: the base layer's";
    EXPECT_NEAR(position(spine).y, 2.0f, 1e-6f) << "masked in: the upper layer's";
    // additive: the head moves by half of (clip at 0.5 - clip at 0) = 0.25
    EXPECT_NEAR(position(head).y, 0.4f + 0.25f, 1e-5f);
    Animator object(&scene, root);
    object.setLayerWeight(2, 0.0f);
    step(kDt);
    EXPECT_EQ(position(head).y, 0.4f) << "an additive layer at weight 0 changes nothing";
    EXPECT_EQ(object.getLayerIndex("Upper"), 1);
}

// anim-properties on S21: a state's clip property tracks are written by the Animator (the
// Animation Player's bindings and key space), a discrete one stepped, a layer blending with what
// is under it (masks do not stop properties), and an entity keyed only by property tracks is not
// a bone of the rig
TEST_F(AnimatorSystemTest, PropertyTracksPlayOnTheAnimator){
    const Entity lamp = node(root, "Lamp", Vector3(0, 2, 0));
    LightComponent light;
    light.intensity = 1.0f;
    scene.addComponent<LightComponent>(lamp, light);
    const Entity box = node(root, "Box", Vector3(0, 0, 0));
    auto intensity = [&](){ return scene.getComponent<LightComponent>(lamp).intensity; };

    ClipData glow;
    glow.wrap = ClipWrapMode::ClampForever;
    glow.tracks = {propertyTrack("Lamp", "light", "intensity", {key(0, {1}), key(1, {3})}),
                   propertyTrack("Box", "gameobject", "active", {key(0, {1}), key(0.5f, {0})})};
    ClipData bright;
    bright.wrap = ClipWrapMode::Loop;
    bright.tracks = {propertyTrack("Lamp", "light", "intensity", {key(0, {5}), key(1, {5})})};

    AvatarMaskDef mask;
    mask.paths = {{"Hips", 1.0f}};
    ControllerDef def = oneState(clip("glow", glow));
    LayerDef top;
    top.name = "Bright";
    top.weight = 0.0f;
    top.mask.path = maskFile("hips", mask);
    top.machine.states.push_back(state("Bright", clip("bright", bright)));
    def.layers.push_back(top);
    animator(controllerFile("c", def));

    step(0.0);
    step(0.25);
    EXPECT_NEAR(intensity(), 1.5f, 1e-5f) << "the clip at 0.25";
    EXPECT_TRUE(scene.isEntityActive(box)) << "the key at 0 holds until 0.5";
    step(0.5);
    EXPECT_NEAR(intensity(), 2.5f, 1e-5f) << "the clip at 0.75";
    EXPECT_FALSE(scene.isEntityActive(box)) << "stepped: the key at 0.5 from then on";
    EXPECT_EQ(position(lamp), Vector3(0, 2, 0)) << "property tracks do not make their entity a bone";
    EXPECT_LT(instance().rig.findBone(lamp), 0);

    // the second layer at weight 0.5, its mask covering only Hips: properties pass masks
    Animator object(&scene, root);
    object.setLayerWeight(1, 0.5f);
    step(0.5);
    EXPECT_NEAR(intensity(), 0.5f * 3.0f + 0.5f * 5.0f, 1e-5f) << "the base clamped at 3, half of the layer's 5";

    // the layer at weight 0 changes nothing
    object.setLayerWeight(1, 0.0f);
    step(kDt);
    EXPECT_NEAR(intensity(), 3.0f, 1e-5f);
}

// S40 R4: audio cues of a blend tree's clips fire by the leader rule as clip events do; a source cue
// asks its AudioSource for a timeline start
TEST_F(AnimatorSystemTest, AudioCuesFireOnlyTheLeadersCues){
    AudioSourceComponent walkSource;
    walkSource.filename = "sounds/walk.wav";
    scene.addComponent<AudioSourceComponent>(spine, walkSource);
    AudioSourceComponent runSource;
    runSource.filename = "sounds/run.wav";
    scene.addComponent<AudioSourceComponent>(head, runSource);
    ClipAudioCue walkCue;
    walkCue.time = 0.5f;
    walkCue.kind = ClipCueKind::Source;
    walkCue.path = "Hips/Spine";
    ClipAudioCue runCue = walkCue;
    runCue.path = "Hips/Spine/Head";
    ClipData walkData = slide("Hips", 1.0f);
    walkData.audio = {walkCue};
    ClipData runData = slide("Hips", 2.0f);
    runData.audio = {runCue};
    ControllerDef def;
    def.parameters = {parameter("Speed", AnimatorParameterType::Float, 0.3f)};
    LayerDef layer;
    layer.name = "Base Layer";
    StateDef move;
    move.name = "Move";
    BlendTreeDef tree;
    tree.type = BlendTreeType::Simple1D;
    tree.parameter = "Speed";
    BlendChildDef a;
    a.motion.clip.path = clip("walkcue", walkData);
    a.threshold = 0;
    BlendChildDef b;
    b.motion.clip.path = clip("runcue", runData);
    b.threshold = 1;
    tree.children = {a, b};
    move.motion.tree.push_back(tree);
    layer.machine.states.push_back(move);
    def.layers.push_back(layer);
    animator(controllerFile("cues", def));
    step(0.0);
    int walkStarts = 0;
    int runStarts = 0;
    for (int i = 0; i < 30; i++){
        step(0.1);
        AudioSourceComponent& walk = scene.getComponent<AudioSourceComponent>(spine);
        AudioSourceComponent& run = scene.getComponent<AudioSourceComponent>(head);
        if (walk.startTrigger){
            walkStarts++;
            EXPECT_TRUE(walk.timelineStart);
            walk.startTrigger = false;
        }
        if (run.startTrigger){
            runStarts++;
            run.startTrigger = false;
        }
    }
    EXPECT_EQ(walkStarts, 3) << "the leader (walk at 0.7) fires its cue each crossing of 0.5";
    EXPECT_EQ(runStarts, 0) << "the other clip's cue does not";
}

TEST_F(AnimatorSystemTest, EventsLeaderOnlyAllAndSeek){
    ClipData walkData = slide("Hips", 1.0f);
    walkData.events = {event(0.5f, "onStep")};
    ClipData runData = slide("Hips", 2.0f);
    runData.events = {event(0.5f, "onRunStep")};
    const std::string walk = clip("walk", walkData);
    const std::string run = clip("run", runData);
    ControllerDef def;
    def.parameters = {parameter("Speed", AnimatorParameterType::Float, 0.3f)};
    LayerDef layer;
    layer.name = "Base Layer";
    StateDef move;
    move.name = "Move";
    BlendTreeDef tree;
    tree.type = BlendTreeType::Simple1D;
    tree.parameter = "Speed";
    BlendChildDef a;
    a.motion.clip.path = walk;
    a.threshold = 0;
    BlendChildDef b;
    b.motion.clip.path = run;
    b.threshold = 1;
    tree.children = {a, b};
    move.motion.tree.push_back(tree);
    layer.machine.states.push_back(move);
    def.layers.push_back(layer);
    AnimatorComponent& component = animator(controllerFile("c", def));
    listen();
    step(0.0);
    step(0.1, 30);      // 3 s: three crossings of 0.5
    EXPECT_EQ(count("onStep"), 3u) << "the leader (walk at 0.7) fires";
    EXPECT_EQ(count("onRunStep"), 0u);
    EXPECT_EQ(events.front().state, "Base Layer.Move");
    events.clear();
    component.eventMode = AnimatorEventMode::All;
    step(0.1, 10);
    EXPECT_EQ(count("onStep"), 1u);
    EXPECT_EQ(count("onRunStep"), 1u);
    events.clear();
    Animator(&scene, root).play("Move", 0, 0.9f);
    step(0.0);
    EXPECT_TRUE(events.empty()) << "a seek fires nothing";
}

// AC8: one Animator driving three skinned meshes of two models fires an event once per pass
TEST_F(AnimatorSystemTest, EventsOncePerAnimator){
    Model first(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(first.getEntity(), fixturePath("hero_tpose.gltf"), false, false, false));
    scene.getComponent<ModelComponent>(first.getEntity()).needUpdateModel = false;
    Model second(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(second.getEntity(), fixturePath("robot_apose.gltf"), false, false, false));
    scene.getComponent<ModelComponent>(second.getEntity()).needUpdateModel = false;
    Model third(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(third.getEntity(), fixturePath("hero_tpose.gltf"), false, false, false));
    scene.getComponent<ModelComponent>(third.getEntity()).needUpdateModel = false;
    scene.addEntityChild(root, first.getEntity(), false);
    scene.addEntityChild(root, second.getEntity(), false);
    scene.addEntityChild(root, third.getEntity(), false);
    ClipData data = slide("Hips", 1.0f);
    data.events = {event(0.25f, "hit")};
    animator(controllerFile("c", oneState(clip("hit", data))));
    listen();
    step(0.0);
    ASSERT_TRUE(instance().bound);
    EXPECT_EQ(instance().rig.skins.size(), 3u);
    for (int i = 0; i < 300; i++) step(kDt);
    EXPECT_EQ(count("hit"), 5u) << "300 frames = 5 loops";
}

TEST_F(AnimatorSystemTest, RootMotionAcrossLoopWraps){
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(0.5f, {0, 1.2f, 1}), key(1, {0, 1, 2})})};
    AnimatorComponent& component = animator(controllerFile("c", oneState(clip("walk", data))));
    component.applyRootMotion = true;
    step(0.0);
    float previous = position(root).z;
    for (int i = 0; i < 600; i++){
        step(kDt);
        const float z = position(root).z;
        EXPECT_GE(z, previous - 1e-5f) << "no backward step at a wrap (frame " << i << ")";
        previous = z;
        // the pose keeps the hips in place (their motion moved the GameObject)
        EXPECT_NEAR(position(hips).z, 0.0f, 1e-5f);
    }
    EXPECT_NEAR(position(root).z, 20.0f, 20.0f * 0.001f) << "10 laps = 10 x the clip's displacement";
    EXPECT_NEAR(position(root).y, 0.0f, 1e-5f) << "bake_position_y (the default) keeps the height in the pose";
}

TEST_F(AnimatorSystemTest, RootMotionConfigurableBoneAndBakeFlags){
    const Entity mover = node(root, "Mover", Vector3(0, 0, 0));
    ClipData data;
    data.wrap = ClipWrapMode::Once;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0, 1, 5})}),
                   track("Mover", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {3, 2, 0})})};
    data.unknownKeys.push_back({"root_motion", "{\"bone\": \"Mover\", \"bake_position_y\": false}"});
    AnimatorComponent& component = animator(controllerFile("c", oneState(clip("move", data))));
    component.applyRootMotion = true;
    step(0.0);
    step(0.25, 4);
    EXPECT_NEAR(position(root).x, 3.0f, 1e-4f) << "the clip's root_motion.bone, not the shallowest moved bone";
    EXPECT_NEAR(position(root).y, 2.0f, 1e-4f) << "bake_position_y false: the height moves the GameObject";
    EXPECT_NEAR(position(root).z, 0.0f, 1e-4f);
    EXPECT_NEAR(position(hips).z, 5.0f, 1e-4f) << "other bones keep their motion";
    EXPECT_NEAR(position(mover).x, 0.0f, 1e-4f);
}

TEST_F(AnimatorSystemTest, OnAnimatorMoveReceivesTheDelta){
    AnimatorComponent& component = animator(controllerFile("c", oneState(clip("walk", slide("Hips", 1.0f)))));
    component.applyRootMotion = true;
    Vector3 total(0, 0, 0);
    component.onAnimatorMove.add("test", [&](Vector3 delta, Quaternion){ total = total + delta; });
    step(0.0);
    step(0.1, 5);
    EXPECT_NEAR(total.x, 0.5f, 1e-4f);
    EXPECT_NEAR(position(root).x, 0.0f, 1e-6f) << "a handler takes over: not applied";
}

TEST_F(AnimatorSystemTest, MontageSectionsChainJumpAndInterrupt){
    ClipData data = slide("Hips", 3.0f, ClipWrapMode::Once, 3.0f);
    data.events = {event(1.5f, "onImpact")};
    const std::string attack = clip("attack", data);
    MontageDef montage;
    montage.clip.path = attack;
    montage.slot = "Default";
    montage.blendIn.time = 0.0f;
    montage.blendOut.time = 0.0f;
    montage.sections = {{"A", 0.0f, "B"}, {"B", 1.0f, ""}, {"C", 2.0f, ""}};
    MontageEventDef own;
    own.time = 0.5f;
    own.function = "onWindup";
    montage.events = {own};
    const std::string path = montageFile("attack", montage);
    animator(controllerFile("c", oneState(clip("idle", hold("Hips", Vector3(0, 1, 0))))));
    listen();
    std::vector<std::pair<std::string, bool>> ended;
    scene.getComponent<AnimatorComponent>(root).onMontageEnded.add("test", [&](const std::string& m, bool interrupted){
        ended.push_back({m, interrupted});
    });
    step(0.0);
    Animator object(&scene, root);
    ASSERT_TRUE(object.playMontage(path));
    step(0.0);
    EXPECT_TRUE(object.isMontagePlaying());
    step(0.6);
    EXPECT_EQ(object.getMontageSection(), "A");
    EXPECT_EQ(count("onWindup"), 1u) << "the montage's own event";
    step(0.6);
    EXPECT_EQ(object.getMontageSection(), "B") << "A chains to B";
    step(0.5);
    EXPECT_EQ(count("onImpact"), 1u) << "the clip's event";
    step(0.6);
    ASSERT_EQ(ended.size(), 1u) << "B is last: the montage ends at C's start";
    EXPECT_FALSE(ended[0].second);
    EXPECT_FALSE(object.isMontagePlaying());
    // jump and set-next
    ASSERT_TRUE(object.playMontage(path));
    step(0.0);
    EXPECT_TRUE(object.montageJumpToSection("C"));
    step(0.1);
    EXPECT_NEAR(position(hips).x, 2.1f, 1e-3f);
    EXPECT_TRUE(object.montageJumpToSection("A"));
    EXPECT_TRUE(object.montageSetNextSection("A", "C"));
    step(1.1);
    EXPECT_EQ(object.getMontageSection(), "C");
    // a second montage on the slot blends the first out, interrupted
    ended.clear();
    ASSERT_TRUE(object.playMontage(path));
    step(kDt, 3);
    ASSERT_GE(ended.size(), 1u);
    EXPECT_TRUE(ended[0].second);
}

TEST_F(AnimatorSystemTest, ExposedWritesOnlyListedBonesAndAttachments){
    const Entity sword = node(head, "Sword", Vector3(0, 0, 0));
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {1, 1, 0})}),
                   track("Hips/Spine", ClipChannel::Position, {key(0, {0, 0.3f, 0}), key(1, {1, 0.3f, 0})}),
                   track("Hips/Spine/Head", ClipChannel::Position, {key(0, {0, 0.4f, 0}), key(1, {1, 0.4f, 0})}),
                   track("Hips/Leg", ClipChannel::Position, {key(0, {0.1f, -0.5f, 0}), key(1, {1, -0.5f, 0})})};
    AnimatorComponent& component = animator(controllerFile("c", oneState(clip("all", data))));
    component.writeBones = AnimatorWriteBones::Exposed;
    component.exposedBones = {"Hips/Leg"};
    step(0.0);
    step(0.5);
    EXPECT_EQ(position(hips).x, 0.0f) << "not exposed";
    EXPECT_EQ(position(spine).x, 0.0f);
    EXPECT_NEAR(position(head).x, 0.5f, 1e-6f) << "a bone with an attachment";
    EXPECT_NEAR(position(leg).x, 0.55f, 1e-6f) << "listed";
    (void)sword;
    // the evaluated pose is still there for scripts
    EXPECT_GT(Animator(&scene, root).getBonePosition("Hips/Spine").x, 0.0f);
}

// AC11: the Animator's skin matrices are what the render system computes from the bone transforms
TEST_F(AnimatorSystemTest, SkinMatricesMatchTheRenderSystem){
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), fixturePath("hero_animated.gltf"), false, false, false));
    scene.getComponent<ModelComponent>(model.getEntity()).needUpdateModel = false;
    const Entity hero = model.getEntity();
    scene.getComponent<Transform>(hero).position = Vector3(2, 0, -1);
    scene.getComponent<Transform>(hero).rotation = aboutY(30);
    const std::string walk = fixturePath("hero_animated.gltf") + "#Walk";
    animator(controllerFile("c", oneState(walk)), hero);
    step(0.0);
    ASSERT_TRUE(instance(hero).bound);
    ASSERT_FALSE(instance(hero).rig.skins.empty());
    step(0.37);
    resolveTransforms();
    const ModelComponent& component = scene.getComponent<ModelComponent>(hero);
    ASSERT_FALSE(component.skinBindings.empty());
    EXPECT_TRUE(component.skinBindings[0].animatorDriven);
    const ModelSkinBinding& binding = component.skinBindings[0];
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(binding.mesh);
    const Matrix4 inverseMesh = scene.getComponent<Transform>(binding.mesh).modelMatrix.inverse();
    float worst = 0.0f;
    for (size_t j = 0; j < binding.joints.size(); j++){
        const Matrix4 expected = inverseMesh * scene.getComponent<Transform>(binding.joints[j]).modelMatrix * binding.inverseBindMatrices[j];
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++) worst = std::max(worst, std::fabs(mesh.bonesMatrix[j][c][r] - expected[c][r]));
        }
    }
    EXPECT_LE(worst, 1e-5f);
    // removing the Animator gives the skin back to the render system
    scene.removeComponent<AnimatorComponent>(hero);
    EXPECT_FALSE(scene.getComponent<ModelComponent>(hero).skinBindings[0].animatorDriven);
}

TEST_F(AnimatorSystemTest, CullUpdateTransformsAdvancesWithoutWriting){
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), fixturePath("hero_animated.gltf"), false, false, false));
    scene.getComponent<ModelComponent>(model.getEntity()).needUpdateModel = false;
    const Entity hero = model.getEntity();
    const Entity camera = scene.createEntity();
    scene.addComponent<Transform>(camera, {});
    CameraComponent cameraComponent;
    scene.addComponent<CameraComponent>(camera, cameraComponent);
    // a camera whose frustum excludes everything (its planes all face away)
    CameraComponent& c = scene.getComponent<CameraComponent>(camera);
    for (int p = 0; p < 6; p++) c.frustumPlanes[p] = Plane(Vector3(0, 1, 0), -1000.0f);
    c.farClip = 1.0f;
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks = {track("Hero/mixamorig:Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {1, 1, 0})})};
    data.events = {event(0.5f, "tick")};
    AnimatorComponent& component = animator(controllerFile("c", oneState(clip("cull", data))), hero);
    component.cullingMode = AnimatorCullingMode::CullUpdateTransforms;
    listen(hero);
    step(0.0);
    ASSERT_TRUE(instance(hero).bound);
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(scene.getComponent<ModelComponent>(hero).skinBindings[0].mesh);
    const Matrix4 before = mesh.bonesMatrix[0];
    const Vector3 hipsBefore = scene.getComponent<Transform>(instance(hero).rig.bones[(size_t)instance(hero).rig.findBone("Hero/mixamorig:Hips")].entity).position;
    step(0.1, 12);
    EXPECT_EQ(count("tick"), 1u) << "events still fire";
    EXPECT_NEAR(instance(hero).currentState(0).normalizedTime, 1.2f, 1e-4f) << "time advances";
    EXPECT_EQ(std::memcmp(&mesh.bonesMatrix[0], &before, sizeof(Matrix4)), 0) << "no skin written";
    const Vector3 hipsAfter = scene.getComponent<Transform>(instance(hero).rig.bones[(size_t)instance(hero).rig.findBone("Hero/mixamorig:Hips")].entity).position;
    EXPECT_EQ(hipsAfter, hipsBefore);
    EXPECT_GT(system().getStats().culled, 0u);
    component.cullingMode = AnimatorCullingMode::CullCompletely;
    const float time = instance(hero).currentState(0).normalizedTime;
    step(0.1, 5);
    EXPECT_EQ(instance(hero).currentState(0).normalizedTime, time) << "nothing advances";
}

TEST_F(AnimatorSystemTest, ClipOverridesHotReloadAndOwnership){
    const std::string a = clip("a", hold("Hips", Vector3(1, 0, 0)));
    const std::string b = clip("b", hold("Hips", Vector3(2, 0, 0)));
    ControllerDef def = controller({}, {state("Play", a), state("Other", b)});
    const std::string path = controllerFile("c", def);
    AnimatorComponent& component = animator(path);
    component.clipOverrides = {{a, b}};
    step(0.0);
    step(kDt);
    EXPECT_NEAR(position(hips).x, 2.0f, 1e-6f) << "the override plays";
    component.clipOverrides.clear();
    component.needsLoad = true;
    step(kDt);
    EXPECT_NEAR(position(hips).x, 1.0f, 1e-6f);
    step(0.3);
    const double time = instance().layers[0].current.time;
    // the controller file changes (a state added): rebound, the state's time kept
    def.layers[0].machine.states.push_back(state("New", a));
    animtest::writeAsset(path, writeController(def));
    AnimatorAssetPool::invalidate(path);
    step(0.0);
    EXPECT_EQ(instance().controller->states.size(), 3u);
    EXPECT_NEAR(instance().layers[0].current.time, time, 1e-6f);
    // a second Animator on the hierarchy below drives nothing
    animtest::takeWarnings();
    // (its root bone is the hips the first one animates)
    const std::string c2 = controllerFile("c2", oneState(clip("spine", hold("", Vector3(0, 9, 0)))));
    animator(c2, hips);
    step(kDt);
    EXPECT_FALSE(instance(hips).bound);
    EXPECT_TRUE(instance(hips).bindFailed);
}

// AC12: the wave's result does not depend on the workers
TEST(AnimatorSystemDeterminism, ParallelEqualsSerial){
    std::unique_ptr<AnimatorFixture> runs[2];
    std::vector<std::vector<float>> results(2);
    for (int r = 0; r < 2; r++){
        runs[r] = std::make_unique<AnimatorFixture>();
        AnimatorFixture& f = *runs[r];
        f.system().setParallel(r == 0);
        ClipData a = slide("Hips", 1.0f);
        a.tracks.push_back(track("Hips/Spine", ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(1, {0, 0.7071068f, 0, 0.7071068f})}));
        ClipData b = slide("Hips/Leg", 2.0f, ClipWrapMode::PingPong, 0.7f);
        ControllerDef def;
        def.parameters = {parameter("Speed", AnimatorParameterType::Float, 0.4f)};
        LayerDef layer;
        layer.name = "Base Layer";
        StateDef move;
        move.name = "Move";
        BlendTreeDef tree;
        tree.type = BlendTreeType::Simple1D;
        tree.parameter = "Speed";
        BlendChildDef ca;
        ca.motion.clip.path = f.clip("a", a);
        BlendChildDef cb;
        cb.motion.clip.path = f.clip("b", b);
        cb.threshold = 1.0f;
        tree.children = {ca, cb};
        move.motion.tree.push_back(tree);
        layer.machine.states.push_back(move);
        def.layers.push_back(layer);
        const std::string controllerPath = f.controllerFile("c", def);
        std::vector<Entity> roots;
        for (int i = 0; i < 24; i++){
            const Entity character = f.node(NULL_ENTITY, "Character", Vector3((float)i, 0, 0));
            const Entity hips = f.node(character, "Hips", Vector3(0, 1, 0));
            f.node(hips, "Spine", Vector3(0, 0.3f, 0));
            f.node(f.node(hips, "Leg", Vector3(0.1f, -0.5f, 0)), "Foot", Vector3(0, -0.45f, 0));
            f.animator(controllerPath, character);
            roots.push_back(character);
        }
        f.step(0.0);
        for (int frame = 0; frame < 600; frame++){
            for (size_t i = 0; i < roots.size(); i++){
                Animator(&f.scene, roots[i]).setFloat("Speed", 0.5f + 0.5f * std::sin(0.01f * (float)(frame + (int)i)));
            }
            f.step(kDt);
        }
        auto transforms = f.scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            const Transform& t = transforms->getComponentFromIndex(i);
            results[(size_t)r].insert(results[(size_t)r].end(), {t.position.x, t.position.y, t.position.z, t.rotation.w, t.rotation.x,
                                                                  t.rotation.y, t.rotation.z});
        }
    }
    ASSERT_EQ(results[0].size(), results[1].size());
    EXPECT_EQ(std::memcmp(results[0].data(), results[1].data(), results[0].size() * sizeof(float)), 0) << "bit-identical";
}

TEST(AnimatorSystemAllocations, ZeroAllocationsInTheSteadyState){
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this build cannot count allocations";
    AnimatorFixture f;
    ClipData walk = slide("Hips", 1.0f);
    walk.events = {event(0.5f, "s")};
    ClipData nod;
    nod.wrap = ClipWrapMode::Loop;
    nod.tracks = {track("Hips/Spine/Head", ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(1, {0.3826834f, 0, 0, 0.9238795f})})};
    ControllerDef def = controller({parameter("X", AnimatorParameterType::Float, 0.5f)}, {state("Walk", f.clip("walk", walk))});
    LayerDef add;
    add.name = "Nod";
    add.blending = AnimatorLayerBlending::Additive;
    add.machine.states.push_back(state("Nod", f.clip("nod", nod)));
    def.layers.push_back(add);
    AnimatorComponent& component = f.animator(f.controllerFile("c", def));
    component.applyRootMotion = true;
    AnimatorIKConstraint look;
    look.name = "Look";
    look.type = AnimatorIKType::LookAt;
    look.bones = {"Hips/Spine", "Hips/Spine/Head"};
    look.targetPosition = Vector3(1, 2, 3);
    component.ik = {look};
    size_t fired = 0;
    component.onEvent.add("test", [&](const AnimationEvent&){ fired++; });
    f.step(0.0);
    for (int i = 0; i < 120; i++) f.step(kDt);
    testsupport::AllocationCountScope scope;
    for (int i = 0; i < 600; i++) f.step(kDt);
    EXPECT_EQ(scope.count(), 0u);
    EXPECT_GE(fired, 10u) << "events fired during the measurement";
}

// AC10: without an Animator nothing changes: the Animation Player plays exactly as before
TEST_F(AnimatorSystemTest, PlayerScenesUnchanged){
    const std::string path = clip("walk", slide("Hips", 3.0f));
    AnimationPlayer player(&scene, root);
    player.addClip(path, "walk");
    player.setPlayAutomatically(true);
    std::vector<Vector3> withSystem;
    for (int i = 0; i < 240; i++){
        scene.getSystem<ActionSystem>()->update(kDt);
        step(kDt);
        withSystem.push_back(position(hips));
    }
    Scene other;
    const Entity otherRoot = other.createEntity();
    other.setEntityName(otherRoot, "Hero");
    other.addComponent<Transform>(otherRoot, {});
    const Entity otherHips = other.createEntity();
    other.setEntityName(otherHips, "Hips");
    Transform t;
    t.position = Vector3(0, 1, 0);
    other.addComponent<Transform>(otherHips, t);
    other.addEntityChild(otherRoot, otherHips, false);
    AnimationPlayer otherPlayer(&other, otherRoot);
    otherPlayer.addClip(path, "walk");
    otherPlayer.setPlayAutomatically(true);
    for (int i = 0; i < 240; i++){
        other.getSystem<ActionSystem>()->update(kDt);
        const Vector3 p = other.getComponent<Transform>(otherHips).position;
        EXPECT_EQ(std::memcmp(&p, &withSystem[(size_t)i], sizeof(Vector3)), 0) << "frame " << i;
    }
}

// an edit-mode preview of an Animator whose controller file changed (its pool entry dropped, as a
// document commit or the hot reload does) evaluates the new controller, the runtime kept
TEST_F(AnimatorSystemTest, PreviewPicksUpAnEditedController){
    const std::string clipPath = clip("slide", slide("Hips", 1.0f));
    ControllerDef def = oneState(clipPath);
    const std::string path = controllerFile("edited", def);
    animator(path);
    std::string error;
    ASSERT_TRUE(system().preview(root, 0.1f, &error)) << error;
    ASSERT_EQ(instance().controller->layers.size(), 1u);
    LayerDef upper;
    upper.name = "Upper";
    upper.machine.states = {state("Still", "")};
    def.layers.push_back(upper);
    animtest::writeAsset(path, anim::writeController(def));
    AnimatorAssetPool::invalidate(path);
    ASSERT_TRUE(system().preview(root, 0.1f, &error)) << error;
    EXPECT_EQ(instance().controller->layers.size(), 2u) << "the edited controller";
    EXPECT_EQ(instance().layers.size(), 2u);
}
