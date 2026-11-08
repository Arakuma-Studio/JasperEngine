// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Every event a script can subscribe to is cleared at Stop (Engine::clearComponentSubscriptions,
// before the C++ script library is unloaded) and on tag removal (Scene::removeSubscriptionsByTag,
// a hot reload): runtime-core spec 6.5. The events are named one by one here, not through
// forEachEvent, so the test also holds for a clear that forgets one (tools/check_event_clears.py
// keeps the visitors complete).

#include <gtest/gtest.h>

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/InputFieldComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/SplineAnimateComponent.h"
#include "component/TextEffectComponent.h"
#include "component/TweenComponent.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/VideoPlayerComponent.h"
#include "device/Device.h"
#include "object/VideoPlayer.h"
#include "object/navigation/NavMeshAgent.h"
#include "object/navigation/NavMeshSurface.h"
#if JASPERENGINE_SOUND_LAYER
#include "object/sound/Audio.h"
#endif
#include "subsystem/PhysicsSystem.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    const char* const kTag = "ScriptX_onSomething";

    // one owner-less subscriber (an add(tag, lambda) of a C++ script, which no owner key removes)
    template<typename Event>
    void subscribe(Event& event) {
        event.add(kTag, [](auto&&...) {});
    }

    template<typename Ret, typename... Args>
    void subscribeRet(FunctionSubscribe<Ret(Args...)>& event) {
        event.add(kTag, [](Args...) { return Ret(); });
    }

    struct Named {
        std::string name;
        size_t (*size)(Scene& scene, Entity entity);
    };

    template<typename Component>
    Component& component(Scene& scene, Entity entity) {
        return scene.getComponent<Component>(entity);
    }

    // every event of every component with events, by name
    std::vector<Named> subscribeEverything(Scene& scene, Entity entity) {
        scene.addComponent<ActionComponent>(entity, {});
        scene.addComponent<AnimationPlayerComponent>(entity, {});
        scene.addComponent<AnimatorComponent>(entity, {});
        scene.addComponent<AudioSourceComponent>(entity, {});
        scene.addComponent<ButtonComponent>(entity, {});
        scene.addComponent<InputFieldComponent>(entity, {});
        scene.addComponent<NavMeshAgentComponent>(entity, {});
        scene.addComponent<NavMeshSurfaceComponent>(entity, {});
        scene.addComponent<OnScreenStickComponent>(entity, {});
        scene.addComponent<OnScreenButtonComponent>(entity, {});
        scene.addComponent<TouchAreaComponent>(entity, {});
        scene.addComponent<PanelComponent>(entity, {});
        scene.addComponent<ParticleSystemComponent>(entity, {});
        scene.addComponent<PoseAnimatorComponent>(entity, {});
        scene.addComponent<ScrollbarComponent>(entity, {});
        scene.addComponent<SplineAnimateComponent>(entity, {});
        scene.addComponent<TextEffectComponent>(entity, {});
        scene.addComponent<UIComponent>(entity, {});
        scene.addComponent<UIDocumentComponent>(entity, {});
        scene.addComponent<VertexAnimatorComponent>(entity, {});
        scene.addComponent<VideoPlayerComponent>(entity, {});

        std::vector<Named> named;
#define JE_EVENT(COMPONENT, EVENT) \
        subscribe(component<COMPONENT>(scene, entity).EVENT); \
        named.push_back({#COMPONENT "." #EVENT, [](Scene& s, Entity e) { return component<COMPONENT>(s, e).EVENT.size(); }});

        JE_EVENT(ActionComponent, onStart) JE_EVENT(ActionComponent, onPause) JE_EVENT(ActionComponent, onStop)
        JE_EVENT(ActionComponent, onStep)
        JE_EVENT(AnimationPlayerComponent, onEvent) JE_EVENT(AnimationPlayerComponent, onFinish)
        JE_EVENT(AnimationPlayerComponent, onStateEnter) JE_EVENT(AnimationPlayerComponent, onStateExit)
        JE_EVENT(AnimatorComponent, onEvent) JE_EVENT(AnimatorComponent, onStateEnter) JE_EVENT(AnimatorComponent, onStateExit)
        JE_EVENT(AnimatorComponent, onMontageEnded) JE_EVENT(AnimatorComponent, onAnimatorMove)
        JE_EVENT(AudioSourceComponent, onStart) JE_EVENT(AudioSourceComponent, onPause) JE_EVENT(AudioSourceComponent, onStop)
        JE_EVENT(ButtonComponent, onPress) JE_EVENT(ButtonComponent, onRelease)
        JE_EVENT(InputFieldComponent, onChange) JE_EVENT(InputFieldComponent, onSubmit)
        JE_EVENT(NavMeshAgentComponent, onDestinationReached) JE_EVENT(NavMeshAgentComponent, onPathFailed)
        JE_EVENT(NavMeshAgentComponent, onOffMeshLinkStart) JE_EVENT(NavMeshAgentComponent, onOffMeshLinkEnd)
        JE_EVENT(NavMeshSurfaceComponent, onBakeCompleted)
        JE_EVENT(OnScreenStickComponent, onValueChanged) JE_EVENT(OnScreenStickComponent, onPress)
        JE_EVENT(OnScreenStickComponent, onRelease)
        JE_EVENT(OnScreenButtonComponent, onPress) JE_EVENT(OnScreenButtonComponent, onRelease)
        JE_EVENT(TouchAreaComponent, onDrag)
        JE_EVENT(PanelComponent, onMove) JE_EVENT(PanelComponent, onResize)
        JE_EVENT(ParticleSystemComponent, onCollision) JE_EVENT(ParticleSystemComponent, onParticleDeath)
        JE_EVENT(ParticleSystemComponent, onSubEmit)
        JE_EVENT(PoseAnimatorComponent, onFinish)
        JE_EVENT(ScrollbarComponent, onChange)
        JE_EVENT(SplineAnimateComponent, onComplete)
        JE_EVENT(TextEffectComponent, onComplete) JE_EVENT(TextEffectComponent, onCharacterRevealed)
        JE_EVENT(UIComponent, onGetFocus) JE_EVENT(UIComponent, onLostFocus) JE_EVENT(UIComponent, onPointerEnter)
        JE_EVENT(UIComponent, onPointerLeave) JE_EVENT(UIComponent, onPointerMove) JE_EVENT(UIComponent, onPointerDown)
        JE_EVENT(UIComponent, onPointerUp) JE_EVENT(UIComponent, onClick) JE_EVENT(UIComponent, onDoubleClick)
        JE_EVENT(UIComponent, onDragStart) JE_EVENT(UIComponent, onDrag) JE_EVENT(UIComponent, onDragEnd)
        JE_EVENT(UIDocumentComponent, onLoad) JE_EVENT(UIDocumentComponent, onUnload)
        JE_EVENT(VertexAnimatorComponent, onEvent) JE_EVENT(VertexAnimatorComponent, onFinish)
        JE_EVENT(VertexAnimatorComponent, onInstanceEvent) JE_EVENT(VertexAnimatorComponent, onInstanceFinish)
        JE_EVENT(VideoPlayerComponent, onPrepareCompleted) JE_EVENT(VideoPlayerComponent, onStarted)
        JE_EVENT(VideoPlayerComponent, onLoopPointReached) JE_EVENT(VideoPlayerComponent, onErrorReceived)
        JE_EVENT(VideoPlayerComponent, onSeekCompleted) JE_EVENT(VideoPlayerComponent, onFrameReady)
#undef JE_EVENT
        return named;
    }

    std::vector<std::string> stillSubscribed(Scene& scene, Entity entity, const std::vector<Named>& named) {
        std::vector<std::string> left;
        for (const Named& event : named) {
            if (event.size(scene, entity) != 0) left.push_back(event.name);
        }
        return left;
    }

    std::string joined(const std::vector<std::string>& names) {
        std::string text;
        for (const std::string& name : names) text += (text.empty() ? "" : ", ") + name;
        return text;
    }

}

TEST(ComponentEventsTest, ClearReachesEveryEvent) {
    Scene scene;
    const Entity entity = scene.createEntity();
    const std::vector<Named> named = subscribeEverything(scene, entity);
    ASSERT_TRUE(stillSubscribed(scene, entity, named).size() == named.size());

    Engine::clearComponentSubscriptions(&scene);
    EXPECT_TRUE(stillSubscribed(scene, entity, named).empty())
        << "left after the clear at Stop: " << joined(stillSubscribed(scene, entity, named));
}

TEST(ComponentEventsTest, RemoveByTagReachesEveryEvent) {
    Scene scene;
    const Entity entity = scene.createEntity();
    const std::vector<Named> named = subscribeEverything(scene, entity);

    scene.removeSubscriptionsByTag("ScriptX_");
    EXPECT_TRUE(stillSubscribed(scene, entity, named).empty())
        << "left after the removal by tag: " << joined(stillSubscribed(scene, entity, named));
}

// a C++ script's tween ease (TweenComponent::function) is code of its library: the clear at Stop
// destroys it while the library is loaded (the scene is restored or deleted after the unload);
// a built-in ease keeps its curve
TEST(ComponentEventsTest, ClearDropsAScriptsTweenEase) {
    Scene scene;
    const Entity scripted = scene.createEntity();
    const Entity builtin = scene.createEntity();
    scene.addComponent<TweenComponent>(scripted, {});
    scene.addComponent<TweenComponent>(builtin, {});
    auto capture = std::make_shared<int>(0);
    const std::weak_ptr<int> alive = capture;
    scene.getComponent<TweenComponent>(scripted).function =
        std::function<float(float)>([capture](float t) { return t * t * t; });
    capture.reset();
    scene.getComponent<TweenComponent>(builtin).function = EaseType::QUAD_IN;
    ASSERT_FALSE(alive.expired());

    Engine::clearComponentSubscriptions(&scene);
    EXPECT_TRUE(alive.expired()) << "the script's function would be destroyed after its library is unloaded";
    Ease& ease = scene.getComponent<TweenComponent>(scripted).function;
    EXPECT_EQ(ease.getType(), EaseType::LINEAR);
    EXPECT_FLOAT_EQ(ease.call(0.5f), 0.5f);
    Ease& kept = scene.getComponent<TweenComponent>(builtin).function;
    EXPECT_EQ(kept.getType(), EaseType::QUAD_IN);
    EXPECT_FLOAT_EQ(kept.call(0.5f), 0.25f);
}

// the static events of classes other than Engine go with Engine's clears too: the editor's Stop
// calls clearAllSubscriptions before the C++ script library is unloaded, MusicPlayer::reset after
TEST(EngineEventsTest, ClearsReachOtherClassesStaticEvents) {
    for (bool byTag : {false, true}) {
        subscribe(Log::onMessageReceived);
        subscribe(Device::onThermalStateChanged);
        subscribe(Device::onLowMemory);
#if JASPERENGINE_SOUND_LAYER
        subscribe(Audio::onMusicBeat);
        subscribe(Audio::onMusicSection);
#endif
        if (byTag) Engine::removeSubscriptionsByTag("ScriptX_");
        else Engine::clearAllSubscriptions(false);
        const char* how = byTag ? "removal by tag" : "clear at Stop";
        EXPECT_EQ(Log::onMessageReceived.size(), 0u) << how;
        EXPECT_EQ(Device::onThermalStateChanged.size(), 0u) << how;
        EXPECT_EQ(Device::onLowMemory.size(), 0u) << how;
#if JASPERENGINE_SOUND_LAYER
        EXPECT_EQ(Audio::onMusicBeat.size(), 0u) << how << ": a C++ script's callback would outlive its library";
        EXPECT_EQ(Audio::onMusicSection.size(), 0u) << how << ": a C++ script's callback would outlive its library";
        Audio::onMusicBeat.clear();
        Audio::onMusicSection.clear();
#endif
    }
}

#ifdef JASPERENGINE_PHYSICS_3D
TEST(PhysicsEventsTest, ClearSubscriptionsIncludesControllerColliderHit) {
    Scene scene;
    PhysicsSystem& physics = *scene.getSystem<PhysicsSystem>();
    subscribe(physics.onControllerColliderHit);
    subscribe(physics.onContactAdded3D);
    subscribeRet(physics.shouldCollide3D);

    Engine::clearComponentSubscriptions(&scene);
    EXPECT_EQ(physics.onControllerColliderHit.size(), 0u) << "a C++ script's callback would outlive its library at Stop";
    EXPECT_EQ(physics.onContactAdded3D.size(), 0u);
    EXPECT_EQ(physics.shouldCollide3D.size(), 0u);
}

TEST(PhysicsEventsTest, RemoveByTagIncludesControllerColliderHit) {
    Scene scene;
    PhysicsSystem& physics = *scene.getSystem<PhysicsSystem>();
    subscribe(physics.onControllerColliderHit);

    scene.removeSubscriptionsByTag("ScriptX_");
    EXPECT_EQ(physics.onControllerColliderHit.size(), 0u);
}
#endif
