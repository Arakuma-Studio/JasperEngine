// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's state machine (docs/specs/S21-animation.md AC3, R4, R5, D5): exit time every
// loop below 1 and once from 1, Any State first, triggers consumed once and kept until used,
// interruption sources and ordered interruption, sub-state machine entry and exit, no transition
// to self unless allowed, and an interrupted crossfade that blends on without a jump.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kDt = 1.0f / 60.0f;

    class StateMachine : public ::testing::Test, public AnimatorFixture {
    protected:
        std::string a, b, c, d;

        void SetUp() override{
            a = clip("a", hold("Hips", Vector3(0, 0, 0)));
            b = clip("b", hold("Hips", Vector3(10, 0, 0)));
            c = clip("c", hold("Hips", Vector3(-10, 0, 0)));
            d = clip("d", hold("Hips", Vector3(0, 0, 10)));
        }

        void start(const ControllerDef& def){
            animator(controllerFile("controller", def));
            step(0.0);
            ASSERT_TRUE(instance().bound);
        }

        std::string current(){ return instance().currentState(0).name; }
        std::string next(){ return instance().nextState(0).name; }
        Animator object(){ return Animator(&scene, root); }
    };

}

TEST_F(StateMachine, ExitTimeEachLoop){
    StateDef sa = state("A", a);
    TransitionDef t = transition("B", {condition("Go", AnimatorConditionMode::If)});
    t.hasExitTime = true;
    t.exitTime = 0.75f;
    sa.transitions.push_back(t);
    start(controller({parameter("Go", AnimatorParameterType::Bool)}, {sa, state("B", b)}));
    // the condition turns true after the first loop's 0.75: the transition waits for 1.75
    float time = 0.0f;
    while (time < 1.0f){
        step(kDt);
        time += kDt;
        ASSERT_EQ(current(), "A") << time;
    }
    object().setBool("Go", true);
    float switched = -1.0f;
    for (int i = 0; i < 120 && switched < 0.0f; i++){
        step(kDt);
        time += kDt;
        if (current() == "B") switched = time;
    }
    EXPECT_NEAR(switched, 1.75f, kDt + 1e-4f);
}

TEST_F(StateMachine, ExitTimeOnce){
    StateDef sa = state("A", a);
    TransitionDef t = transition("B");
    t.hasExitTime = true;
    t.exitTime = 1.5f;
    sa.transitions.push_back(t);
    start(controller({}, {sa, state("B", b)}));
    float time = 0.0f, switched = -1.0f;
    for (int i = 0; i < 200 && switched < 0.0f; i++){
        step(kDt);
        time += kDt;
        if (current() == "B") switched = time;
    }
    EXPECT_NEAR(switched, 1.5f, kDt + 1e-4f) << "an exit-time-only transition fires on exit time alone";

    // a condition that is false at 1.5 is not tried again later
    StateDef sa2 = state("A", a);
    TransitionDef late = transition("B", {condition("Go", AnimatorConditionMode::If)});
    late.hasExitTime = true;
    late.exitTime = 1.5f;
    sa2.transitions.push_back(late);
    start(controller({parameter("Go", AnimatorParameterType::Bool)}, {sa2, state("B", b)}));
    object().play("A", 0, 0.0f);
    step(kDt, 100);
    object().setBool("Go", true);
    step(kDt, 120);
    EXPECT_EQ(current(), "A");
}

TEST_F(StateMachine, AnyStateFirst){
    StateDef sa = state("A", a);
    sa.transitions.push_back(transition("B", {condition("Go", AnimatorConditionMode::If)}));
    ControllerDef def = controller({parameter("Go", AnimatorParameterType::Bool)}, {sa, state("B", b), state("C", c)});
    def.layers[0].machine.anyState.push_back(transition("C", {condition("Go", AnimatorConditionMode::If)}));
    start(def);
    object().setBool("Go", true);
    step(kDt);
    EXPECT_EQ(current(), "C");
}

TEST_F(StateMachine, TriggerConsumedOnce){
    StateDef sa = state("A", a);
    sa.transitions.push_back(transition("B", {condition("T", AnimatorConditionMode::If)}));
    sa.transitions.push_back(transition("C", {condition("T", AnimatorConditionMode::If)}));
    StateDef sb = state("B", b);
    sb.transitions.push_back(transition("C", {condition("T", AnimatorConditionMode::If)}));
    start(controller({parameter("T", AnimatorParameterType::Trigger)}, {sa, sb, state("C", c)}));
    object().setTrigger("T");
    step(kDt);
    EXPECT_EQ(current(), "B") << "the first eligible transition takes it";
    EXPECT_EQ(object().getBool("T"), false) << "consumed";
    step(kDt, 10);
    EXPECT_EQ(current(), "B") << "the trigger was used once";
}

TEST_F(StateMachine, TriggerPersistsUntilUsed){
    StateDef sa = state("A", a);
    sa.transitions.push_back(transition("B", {condition("T", AnimatorConditionMode::If), condition("Ready", AnimatorConditionMode::If)}));
    start(controller({parameter("T", AnimatorParameterType::Trigger), parameter("Ready", AnimatorParameterType::Bool)}, {sa, state("B", b)}));
    object().setTrigger("T");
    step(kDt, 30);
    EXPECT_EQ(current(), "A");
    EXPECT_TRUE(object().getBool("T")) << "kept until a transition uses it";
    object().setBool("Ready", true);
    step(kDt);
    EXPECT_EQ(current(), "B");
    EXPECT_FALSE(object().getBool("T"));
    // resetTrigger clears one that waits
    object().play("A");
    object().setBool("Ready", false);
    object().setTrigger("T");
    object().resetTrigger("T");
    object().setBool("Ready", true);
    step(kDt);
    EXPECT_EQ(current(), "A");
}

TEST_F(StateMachine, InterruptionNextThenCurrent){
    StateDef sa = state("A", a);
    TransitionDef toB = transition("B", {condition("GoB", AnimatorConditionMode::If)}, 0.5f);
    toB.interruptionSource = TransitionInterruptionSource::NextThenCurrent;
    sa.transitions.push_back(toB);
    sa.transitions.push_back(transition("C", {condition("GoC", AnimatorConditionMode::If)}, 0.5f));
    StateDef sb = state("B", b);
    sb.transitions.push_back(transition("D", {condition("GoD", AnimatorConditionMode::If)}, 0.5f));
    start(controller({parameter("GoB", AnimatorParameterType::Bool), parameter("GoC", AnimatorParameterType::Bool),
                      parameter("GoD", AnimatorParameterType::Bool)}, {sa, sb, state("C", c), state("D", d)}));
    object().setBool("GoB", true);
    step(kDt);
    ASSERT_TRUE(instance().isInTransition(0));
    EXPECT_EQ(next(), "B");
    object().setBool("GoB", false);
    object().setBool("GoC", true);
    object().setBool("GoD", true);
    step(kDt);
    EXPECT_EQ(next(), "D") << "the next state's transitions first";
    // None: nothing interrupts
    StateDef sa2 = state("A", a);
    sa2.transitions.push_back(transition("B", {condition("GoB", AnimatorConditionMode::If)}, 0.5f));
    sa2.transitions.push_back(transition("C", {condition("GoC", AnimatorConditionMode::If)}, 0.5f));
    start(controller({parameter("GoB", AnimatorParameterType::Bool), parameter("GoC", AnimatorParameterType::Bool)},
                     {sa2, state("B", b), state("C", c)}));
    object().play("A");
    object().setBool("GoB", true);
    object().setBool("GoC", false);
    step(kDt);
    object().setBool("GoC", true);
    step(kDt, 5);
    EXPECT_EQ(next(), "B");
}

TEST_F(StateMachine, OrderedInterruption){
    StateDef sa = state("A", a);
    sa.transitions.push_back(transition("C", {condition("GoC", AnimatorConditionMode::If)}, 0.5f));
    TransitionDef toB = transition("B", {condition("GoB", AnimatorConditionMode::If)}, 0.5f);
    toB.interruptionSource = TransitionInterruptionSource::Current;
    toB.orderedInterruption = true;
    sa.transitions.push_back(toB);
    sa.transitions.push_back(transition("D", {condition("GoD", AnimatorConditionMode::If)}, 0.5f));
    start(controller({parameter("GoB", AnimatorParameterType::Bool), parameter("GoC", AnimatorParameterType::Bool),
                      parameter("GoD", AnimatorParameterType::Bool)}, {sa, state("B", b), state("C", c), state("D", d)}));
    object().setBool("GoB", true);
    step(kDt);
    ASSERT_EQ(next(), "B");
    object().setBool("GoD", true);
    step(kDt);
    EXPECT_EQ(next(), "B") << "a later transition does not interrupt an ordered one";
    object().setBool("GoC", true);
    step(kDt);
    EXPECT_EQ(next(), "C") << "an earlier one does";
}

TEST_F(StateMachine, SubStateEntryExit){
    ControllerDef def;
    def.parameters = {parameter("GoSub", AnimatorParameterType::Bool), parameter("Flag", AnimatorParameterType::Bool),
                      parameter("GoExit", AnimatorParameterType::Bool), parameter("Flag2", AnimatorParameterType::Bool)};
    LayerDef layer;
    layer.name = "Base Layer";
    StateDef idle = state("Idle", a);
    idle.transitions.push_back(transition("Sub", {condition("GoSub", AnimatorConditionMode::If)}));
    StateMachineDef sub;
    sub.name = "Sub";
    sub.defaultState = "S1";
    sub.entry.push_back(transition("S2", {condition("Flag", AnimatorConditionMode::If)}));
    StateDef s1 = state("S1", b);
    s1.transitions.push_back(transition("Exit", {condition("GoExit", AnimatorConditionMode::If)}));
    sub.states = {s1, state("S2", c)};
    sub.exitTransitions.push_back(transition("Idle2", {condition("Flag2", AnimatorConditionMode::If)}));
    layer.machine.states = {idle, state("Idle2", d)};
    layer.machine.subMachines.push_back(sub);
    def.layers.push_back(layer);
    start(def);
    EXPECT_EQ(current(), "Idle");
    object().setBool("Flag", true);
    object().setBool("GoSub", true);
    step(kDt);
    EXPECT_EQ(instance().currentState(0).fullPath, "Base Layer.Sub.S2") << "the entry transition";

    start(def);
    object().play("Idle");
    object().setBool("Flag", false);
    object().setBool("GoSub", true);
    step(kDt);
    EXPECT_EQ(current(), "S1") << "the default state";
    object().setBool("GoSub", false);
    object().setBool("Flag2", true);
    object().setBool("GoExit", true);
    step(kDt);
    EXPECT_EQ(current(), "Idle2") << "Exit takes the parent's transitions out of the sub-machine";

    object().play("Base Layer.Sub.S1");
    object().setBool("Flag2", false);
    step(kDt);
    EXPECT_EQ(current(), "Idle") << "without one: the parent's entry (its default)";
}

TEST_F(StateMachine, NoTransitionToSelfUnlessAllowed){
    ControllerDef def = controller({parameter("Hold", AnimatorParameterType::Bool)}, {state("A", a)});
    TransitionDef any = transition("A", {condition("Hold", AnimatorConditionMode::If)});
    def.layers[0].machine.anyState.push_back(any);
    start(def);
    object().setBool("Hold", true);
    step(kDt, 30);
    EXPECT_GT(instance().currentState(0).normalizedTime, 0.4f) << "A keeps playing";

    def.layers[0].machine.anyState[0].canTransitionToSelf = true;
    start(def);
    object().setBool("Hold", true);
    step(kDt, 30);
    EXPECT_LT(instance().currentState(0).normalizedTime, 2.0f * kDt) << "restarted every update";
}

TEST_F(StateMachine, InterruptedBlendIsContinuous){
    StateDef sa = state("A", a);
    sa.transitions.push_back(transition("B", {condition("GoB", AnimatorConditionMode::If)}, 1.0f));
    ControllerDef def = controller({parameter("GoB", AnimatorParameterType::Bool), parameter("Hit", AnimatorParameterType::Trigger)},
                                   {sa, state("B", b), state("C", c)});
    TransitionDef hit = transition("C", {condition("Hit", AnimatorConditionMode::If)}, 1.0f);
    def.layers[0].machine.anyState.push_back(hit);
    start(def);
    object().setBool("GoB", true);
    float previous = position(hips).x;
    float maxStep = 0.0f;
    for (int i = 0; i < 30; i++){
        step(kDt);
        maxStep = std::max(maxStep, std::fabs(position(hips).x - previous));
        previous = position(hips).x;
    }
    object().setTrigger("Hit");
    float interruptStep = 0.0f;
    for (int i = 0; i < 90; i++){
        step(kDt);
        interruptStep = std::max(interruptStep, std::fabs(position(hips).x - previous));
        previous = position(hips).x;
    }
    EXPECT_TRUE(instance().currentState(0).name == "C");
    EXPECT_LE(interruptStep, maxStep * 1.5f + 1e-4f) << "the snapshot blends on from where the pose was";
    EXPECT_NEAR(position(hips).x, -10.0f, 1e-4f);
}

TEST_F(StateMachine, CrossFadeAndPlayFromScripts){
    start(controller({}, {state("A", a), state("B", b)}));
    Animator animator = object();
    animator.crossFadeInFixedTime("B", 0.5f);
    step(0.25);
    EXPECT_TRUE(animator.isInTransition(0));
    EXPECT_NEAR(position(hips).x, 5.0f, 1e-3f);
    step(0.3);
    EXPECT_EQ(current(), "B");
    animator.play("A", 0, 0.5f);
    step(0.0);
    EXPECT_EQ(current(), "A");
    EXPECT_NEAR(instance().currentState(0).normalizedTime, 0.5f, 1e-6f);
    // an unknown state warns once and does nothing
    animtest::takeWarnings();
    animator.play("Nope");
    animator.play("Nope");
    size_t warnings = 0;
    for (const std::string& w : animtest::takeWarnings()) warnings += w.find("unknown state \"Nope\"") != std::string::npos;
    EXPECT_EQ(warnings, 1u);
}
