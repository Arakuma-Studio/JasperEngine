// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "action/Action.h"
#include "action/Tween.h"
#include "action/AlphaTween.h"
#include "action/ColorTween.h"
#include "action/PositionTween.h"
#include "action/RotationTween.h"
#include "action/ScaleTween.h"
#include "action/Animation.h"
#include "action/ParticleSystem.h"
#include "action/SpriteAnimation.h"
#include "action/Ease.h"
#include "action/keyframe/KeyframeTracks.h"
#include "action/keyframe/MorphTracks.h"
#include "action/keyframe/LatticeTracks.h"
#include "action/keyframe/RotateTracks.h"
#include "action/keyframe/ScaleTracks.h"
#include "action/keyframe/TranslateTracks.h"

using namespace jasperengine;

void LuaBinding::registerActionClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("EaseType")
        .addVariable("LINEAR", EaseType::LINEAR)
        .addVariable("STEP", EaseType::STEP)
        .addVariable("QUAD_IN", EaseType::QUAD_IN)
        .addVariable("QUAD_OUT", EaseType::QUAD_OUT)
        .addVariable("QUAD_IN_OUT", EaseType::QUAD_IN_OUT)
        .addVariable("CUBIC_IN", EaseType::CUBIC_IN)
        .addVariable("CUBIC_OUT", EaseType::CUBIC_OUT)
        .addVariable("CUBIC_IN_OUT", EaseType::CUBIC_IN_OUT)
        .addVariable("QUART_IN", EaseType::QUART_IN)
        .addVariable("QUART_OUT", EaseType::QUART_OUT)
        .addVariable("QUART_IN_OUT", EaseType::QUART_IN_OUT)
        .addVariable("QUINT_IN", EaseType::QUINT_IN)
        .addVariable("QUINT_OUT", EaseType::QUINT_OUT)
        .addVariable("QUINT_IN_OUT", EaseType::QUINT_IN_OUT)
        .addVariable("SINE_IN", EaseType::SINE_IN)
        .addVariable("SINE_OUT", EaseType::SINE_OUT)
        .addVariable("SINE_IN_OUT", EaseType::SINE_IN_OUT)
        .addVariable("EXPO_IN", EaseType::EXPO_IN)
        .addVariable("EXPO_OUT", EaseType::EXPO_OUT)
        .addVariable("EXPO_IN_OUT", EaseType::EXPO_IN_OUT)
        .addVariable("CIRC_IN", EaseType::CIRC_IN)
        .addVariable("CIRC_OUT", EaseType::CIRC_OUT)
        .addVariable("CIRC_IN_OUT", EaseType::CIRC_IN_OUT)
        .addVariable("ELASTIC_IN", EaseType::ELASTIC_IN)
        .addVariable("ELASTIC_OUT", EaseType::ELASTIC_OUT)
        .addVariable("ELASTIC_IN_OUT", EaseType::ELASTIC_IN_OUT)
        .addVariable("BACK_IN", EaseType::BACK_IN)
        .addVariable("BACK_OUT", EaseType::BACK_OUT)
        .addVariable("BACK_IN_OUT", EaseType::BACK_IN_OUT)
        .addVariable("BOUNCE_IN", EaseType::BOUNCE_IN)
        .addVariable("BOUNCE_OUT", EaseType::BOUNCE_OUT)
        .addVariable("BOUNCE_IN_OUT", EaseType::BOUNCE_IN_OUT)
        .addVariable("CUSTOM", EaseType::CUSTOM)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ActionState")
        .addVariable("Running", ActionState::Running)
        .addVariable("Paused", ActionState::Paused)
        .addVariable("Stopped", ActionState::Stopped)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Action, EntityHandle>("Action")
        .addConstructor <void(Scene*), void(Scene*, Entity)> ()
        .addFunction("start", &Action::start)
        .addFunction("pause", &Action::pause)
        .addFunction("stop", &Action::stop)
        .addFunction("restart", &Action::restart)
        .addProperty("ownedTarget", &Action::getOwnedTarget, &Action::setOwnedTarget)
        .addProperty("target", (Entity(Action::*)()const)&Action::getTarget, (void(Action::*)(Entity))&Action::setTarget)
        .addFunction("setTarget", 
            luabridge::overload<GameObject*>(&Action::setTarget),
            luabridge::overload<Entity>(&Action::setTarget))
        .addProperty("speed", &Action::getSpeed, &Action::setSpeed)
        .addProperty("weight", &Action::getWeight, &Action::setWeight)
        .addFunction("isRunning", &Action::isRunning)
        .addFunction("isStopped", &Action::isStopped)
        .addFunction("isPaused", &Action::isPaused)
        .addFunction("getTimeCount", &Action::getTimeCount)
        .addFunction("getActionComponent", &Action::getComponent<ActionComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Tween, Action>("Tween")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setFunctionType", &Tween::setFunctionType)
        .addFunction("getTweenComponent", &Tween::getComponent<TweenComponent>)
        .addFunction("getTime", &Tween::getTime)
        .addFunction("getValue", &Tween::getValue)
        .addProperty("duration", &Tween::getDuration, &Tween::setDuration)
        .addProperty("loop", &Tween::isLoop, &Tween::setLoop)
        .addProperty("pingPong", &Tween::isPingPong, &Tween::setPingPong)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<AlphaTween, Tween>("AlphaTween")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setAction", &AlphaTween::setAction)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<ColorTween, Tween>("ColorTween")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setAction", 
            luabridge::overload<Vector3, Vector3, float, bool>(&ColorTween::setAction),
            luabridge::overload<Vector4, Vector4, float, bool>(&ColorTween::setAction))
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<PositionTween, Tween>("PositionTween")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setAction", &PositionTween::setAction)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<RotationTween, Tween>("RotationTween")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setAction", &RotationTween::setAction)
        .addFunction("setSpinAction", &RotationTween::setSpinAction)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<ScaleTween, Tween>("ScaleTween")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setAction", &ScaleTween::setAction)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<ParticleSystem, Action>("ParticleSystem")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &ParticleSystem::isEnabled, &ParticleSystem::setEnabled)
        .addFunction("reset", &ParticleSystem::reset)
        .addProperty("rate", &ParticleSystem::getRate, &ParticleSystem::setRate)
        .addProperty("maxPerUpdate", &ParticleSystem::getMaxPerUpdate, &ParticleSystem::setMaxPerUpdate)
        .addProperty("emitter", &ParticleSystem::isEmitter, &ParticleSystem::setEmitter)
        .addProperty("loop", &ParticleSystem::isLoop, &ParticleSystem::setLoop)
        .addProperty("localSpace", &ParticleSystem::isLocalSpace, &ParticleSystem::setLocalSpace)
        .addProperty("emitterShape", &ParticleSystem::getPositionInitializerShape, &ParticleSystem::setPositionInitializerShape)
        .addFunction("setLifeInitializer", 
            luabridge::overload<float>(&ParticleSystem::setLifeInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setLifeInitializer))
        .addFunction("setPositionInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setPositionInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setPositionInitializer))
        .addFunction("setSpherePositionInitializer",
            luabridge::overload<float>(&ParticleSystem::setSpherePositionInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setSpherePositionInitializer))
        .addFunction("setHemispherePositionInitializer",
            luabridge::overload<float>(&ParticleSystem::setHemispherePositionInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setHemispherePositionInitializer))
        .addFunction("setCirclePositionInitializer",
            luabridge::overload<float>(&ParticleSystem::setCirclePositionInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setCirclePositionInitializer))
        .addFunction("setConePositionInitializer", &ParticleSystem::setConePositionInitializer)
        .addFunction("addBurst",
            luabridge::overload<float, int>(&ParticleSystem::addBurst),
            luabridge::overload<float, int, int>(&ParticleSystem::addBurst))
        .addFunction("setBursts", &ParticleSystem::setBursts)
        .addFunction("clearBursts", &ParticleSystem::clearBursts)
        .addFunction("setPositionModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setPositionModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setPositionModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setPositionModifier))
        .addFunction("setVelocityInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setVelocityInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setVelocityInitializer))
        .addFunction("setVelocityModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setVelocityModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setVelocityModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setVelocityModifier))
        .addFunction("setAccelerationInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setAccelerationInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setAccelerationInitializer))
        .addFunction("setAccelerationModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setAccelerationModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setAccelerationModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setAccelerationModifier))
        .addFunction("setColorInitializer", 
            luabridge::overload<Vector3>(&ParticleSystem::setColorInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setColorInitializer))
        .addFunction("setColorModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setColorModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setColorModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setColorModifier))
        .addFunction("addColorGradientStop", &ParticleSystem::addColorGradientStop)
        .addFunction("setColorGradient", &ParticleSystem::setColorGradient)
        .addFunction("clearColorGradient", &ParticleSystem::clearColorGradient)
        .addFunction("setColorGradientUseSRGB", &ParticleSystem::setColorGradientUseSRGB)
        .addFunction("setAlphaInitializer", 
            luabridge::overload<float>(&ParticleSystem::setAlphaInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setAlphaInitializer))
        .addFunction("setAlphaModifier", 
            luabridge::overload<float, float, float, float>(&ParticleSystem::setAlphaModifier),
            luabridge::overload<float, float, float, float, EaseType>(&ParticleSystem::setAlphaModifier),
            luabridge::overload<float, float, float, float, Ease>(&ParticleSystem::setAlphaModifier))
        .addFunction("setSizeInitializer", 
            luabridge::overload<float>(&ParticleSystem::setSizeInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setSizeInitializer))
        .addFunction("setSizeModifier", 
            luabridge::overload<float, float, float, float>(&ParticleSystem::setSizeModifier),
            luabridge::overload<float, float, float, float, EaseType>(&ParticleSystem::setSizeModifier),
            luabridge::overload<float, float, float, float, Ease>(&ParticleSystem::setSizeModifier))
        .addFunction("setSpriteIntializer", 
            luabridge::overload<std::vector<int>>(&ParticleSystem::setSpriteIntializer),
            luabridge::overload<int, int>(&ParticleSystem::setSpriteIntializer))
        .addFunction("setSpriteModifier", 
            luabridge::overload<float, float, std::vector<int>>(&ParticleSystem::setSpriteModifier),
            luabridge::overload<float, float, std::vector<int>, EaseType>(&ParticleSystem::setSpriteModifier),
            luabridge::overload<float, float, std::vector<int>, Ease>(&ParticleSystem::setSpriteModifier))
        .addFunction("setRotationInitializer", 
            luabridge::overload<float>(&ParticleSystem::setRotationInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setRotationInitializer),
            luabridge::overload<Quaternion>(&ParticleSystem::setRotationInitializer),
            luabridge::overload<Quaternion, Quaternion>(&ParticleSystem::setRotationInitializer))
        .addFunction("setRotationModifier", 
            luabridge::overload<float, float, float, float>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, float, float, EaseType>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, float, float, Ease>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, Quaternion, Quaternion>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, Quaternion, Quaternion, EaseType>(&ParticleSystem::setRotationModifier),
            luabridge::overload<float, float, Quaternion, Quaternion, Ease>(&ParticleSystem::setRotationModifier))
        .addFunction("setScaleInitializer", 
            luabridge::overload<float>(&ParticleSystem::setScaleInitializer),
            luabridge::overload<Vector3>(&ParticleSystem::setScaleInitializer),
            luabridge::overload<float, float>(&ParticleSystem::setScaleInitializer),
            luabridge::overload<Vector3, Vector3>(&ParticleSystem::setScaleInitializer))
        .addFunction("setScaleModifier", 
            luabridge::overload<float, float, Vector3, Vector3>(&ParticleSystem::setScaleModifier),
            luabridge::overload<float, float, float, float>(&ParticleSystem::setScaleModifier),
            luabridge::overload<float, float, Vector3, Vector3, EaseType>(&ParticleSystem::setScaleModifier),
            luabridge::overload<float, float, Vector3, Vector3, Ease>(&ParticleSystem::setScaleModifier))
        .addFunction("getParticleSystemComponent", &ParticleSystem::getComponent<ParticleSystemComponent>)
        // deprecated: the old misspelt name, kept so existing scripts still run
        .addFunction("getParticlesomponent", &ParticleSystem::getComponent<ParticleSystemComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<SpriteAnimation, Action>("SpriteAnimation")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addFunction("setAnimation", 
            luabridge::overload<std::vector<int>, std::vector<int>, bool>(&SpriteAnimation::setAnimation),
            luabridge::overload<int, int, int, bool>(&SpriteAnimation::setAnimation))
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Animation, Action>("Animation")
        .addConstructor <void(Scene*), void(Scene*, Entity)> ()
        .addProperty("loop", &Animation::isLoop, &Animation::setLoop)
        .addProperty("ownedActions", &Animation::isOwnedActions, &Animation::setOwnedActions)
        .addProperty("blendWeight", &Animation::getBlendWeight, &Animation::setBlendWeight)
        .addProperty("defaultFadeTime", &Animation::getDefaultFadeTime, &Animation::setDefaultFadeTime)
        .addFunction("fadeIn", &Animation::fadeIn)
        .addFunction("fadeOut", &Animation::fadeOut)
        .addFunction("addActionFrame",
            luabridge::overload<float, float, Entity, Entity>(&Animation::addActionFrame),
            luabridge::overload<float, Entity, Entity>(&Animation::addActionFrame),
            luabridge::overload<float, float, Entity>(&Animation::addActionFrame),
            luabridge::overload<float, Entity>(&Animation::addActionFrame))
        .addFunction("getActionFrameSize", &Animation::getActionFrameSize)
        .addFunction("getActionFrame", &Animation::getActionFrame)
        .addFunction("setActionFrameStartTime", &Animation::setActionFrameStartTime)
        .addFunction("setActionFrameDuration", &Animation::setActionFrameDuration)
        .addFunction("setActionFrameEntity", &Animation::setActionFrameEntity)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Ease, FunctionSubscribe<float(float)>>("Ease")
        .addConstructor <void (*) (void), void (*) (EaseType)> ()
        .addFunction("__call", &Ease::call)
        .addFunction("call", &Ease::call)
        .addFunction("add", (bool (Ease::*)(const std::string&, lua_State*))&Ease::add)
        .addFunction("setFunction", (void (Ease::*)(lua_State*))&Ease::setFunction)
        .addProperty("type", &Ease::getType, &Ease::setType)
        .addFunction("getType", &Ease::getType)
        .addFunction("setType", &Ease::setType)
        .addFunction("getFunctionType", &Ease::getFunctionType)
        .addFunction("setFunctionType", &Ease::setFunctionType)
        .addStaticFunction("linear", &Ease::linear)
        .addStaticFunction("easeInQuad", &Ease::easeInQuad)
        .addStaticFunction("easeOutQuad", &Ease::easeOutQuad)
        .addStaticFunction("easeInOutQuad", &Ease::easeInOutQuad)
        .addStaticFunction("easeInCubic", &Ease::easeInCubic)
        .addStaticFunction("easeOutCubic", &Ease::easeOutCubic)
        .addStaticFunction("easeInOutCubic", &Ease::easeInOutCubic)
        .addStaticFunction("easeInQuart", &Ease::easeInQuart)
        .addStaticFunction("easeOutQuart", &Ease::easeOutQuart)
        .addStaticFunction("easeInQuint", &Ease::easeInQuint)
        .addStaticFunction("easeOutQuint", &Ease::easeOutQuint)
        .addStaticFunction("easeInOutQuint", &Ease::easeInOutQuint)
        .addStaticFunction("easeInSine", &Ease::easeInSine)
        .addStaticFunction("easeOutSine", &Ease::easeOutSine)
        .addStaticFunction("easeInOutSine", &Ease::easeInOutSine)
        .addStaticFunction("easeInExpo", &Ease::easeInExpo)
        .addStaticFunction("easeOutExpo", &Ease::easeOutExpo)
        .addStaticFunction("easeInOutExpo", &Ease::easeInOutExpo)
        .addStaticFunction("easeInCirc", &Ease::easeInCirc)
        .addStaticFunction("easeOutCirc", &Ease::easeOutCirc)
        .addStaticFunction("easeInOutCirc", &Ease::easeInOutCirc)
        .addStaticFunction("easeInElastic", &Ease::easeInElastic)
        .addStaticFunction("easeOutElastic", &Ease::easeOutElastic)
        .addStaticFunction("easeInOutElastic", &Ease::easeInOutElastic)
        .addStaticFunction("easeInBack", &Ease::easeInBack)
        .addStaticFunction("easeOutBack", &Ease::easeOutBack)
        .addStaticFunction("easeInOutBack", &Ease::easeInOutBack)
        .addStaticFunction("easeOutBounce", &Ease::easeOutBounce)
        .addStaticFunction("easeInBounce", &Ease::easeInBounce)
        .addStaticFunction("easeInOutBounce", &Ease::easeInOutBounce)

        .addStaticFunction("getFunction", &Ease::getFunction)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<KeyframeTracks, Action>("KeyframeTracks")
        .addConstructor <void(Scene*), void(Scene*, Entity)> ()
        .addFunction("setTimes", &KeyframeTracks::setTimes)
        .addFunction("setEasings", &KeyframeTracks::setEasings)
        .addFunction("setEasing", &KeyframeTracks::setEasing)
        .addProperty("loop", &KeyframeTracks::isLoop, &KeyframeTracks::setLoop)
        .addProperty("relative", &KeyframeTracks::isRelative, &KeyframeTracks::setRelative)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<MorphTracks, KeyframeTracks>("MorphTracks")
        .addConstructor <void(Scene*), void(Scene*, Entity), void(Scene*, std::vector<float>, std::vector<std::vector<float>>)> ()
        .addFunction("setValues", &MorphTracks::setValues)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<LatticeTracks, KeyframeTracks>("LatticeTracks")
        .addConstructor <void(Scene*), void(Scene*, Entity), void(Scene*, std::vector<float>, std::vector<int>, std::vector<std::vector<Vector3>>)> ()
        .addFunction("setPoints", &LatticeTracks::setPoints)
        .addFunction("setValues", &LatticeTracks::setValues)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<RotateTracks, KeyframeTracks>("RotateTracks")
        .addConstructor <void(Scene*), void(Scene*, Entity), void(Scene*, std::vector<float>, std::vector<Quaternion>)> ()
        .addFunction("setValues", &RotateTracks::setValues)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<ScaleTracks, KeyframeTracks>("ScaleTracks")
        .addConstructor <void(Scene*), void(Scene*, Entity), void(Scene*, std::vector<float>, std::vector<Vector3>)> ()
        .addFunction("setValues", &ScaleTracks::setValues)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<TranslateTracks, KeyframeTracks>("TranslateTracks")
        .addConstructor <void(Scene*), void(Scene*, Entity), void(Scene*, std::vector<float>, std::vector<Vector3>)> ()
        .addFunction("setValues", &TranslateTracks::setValues)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
