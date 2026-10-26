// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"
#include "LuaComponentSetters.h"

#include "ecs/Entity.h"
#include "ecs/Signature.h"
#include "ecs/EntityManager.h"
#include "ecs/ComponentArray.h"

#include "subsystem/ActionSystem.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"

#include "component/ActionComponent.h"
#include "component/TweenComponent.h"
#include "component/UIComponent.h"
#include "component/RectTransformComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/ButtonComponent.h"
#include "component/InputFieldComponent.h"
#include "component/ParticleSystemComponent.h"
#include "particles/LegacyParticleSystem.h"
#include "component/AudioSourceComponent.h"
#include "component/SpriteComponent.h"
#include "component/FogComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "component/TilemapComponent.h"

using namespace jasperengine;

void LuaBinding::registerECSClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("AudioSourceState")
        .addVariable("Playing", AudioSourceState::Playing)
        .addVariable("Paused", AudioSourceState::Paused)
        .addVariable("Stopped", AudioSourceState::Stopped)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("AudioSourceAttenuation")
        .addVariable("NO_ATTENUATION", AudioSourceAttenuation::NO_ATTENUATION)
        .addVariable("INVERSE_DISTANCE", AudioSourceAttenuation::INVERSE_DISTANCE)
        .addVariable("LINEAR_DISTANCE", AudioSourceAttenuation::LINEAR_DISTANCE)
        .addVariable("EXPONENTIAL_DISTANCE", AudioSourceAttenuation::EXPONENTIAL_DISTANCE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("AnchorPreset")
        .addVariable("NONE", AnchorPreset::NONE)
        .addVariable("TOP_LEFT", AnchorPreset::TOP_LEFT)
        .addVariable("TOP_RIGHT", AnchorPreset::TOP_RIGHT)
        .addVariable("BOTTOM_LEFT", AnchorPreset::BOTTOM_LEFT)
        .addVariable("BOTTOM_RIGHT", AnchorPreset::BOTTOM_RIGHT)
        .addVariable("CENTER_LEFT", AnchorPreset::CENTER_LEFT)
        .addVariable("CENTER_TOP", AnchorPreset::CENTER_TOP)
        .addVariable("CENTER_RIGHT", AnchorPreset::CENTER_RIGHT)
        .addVariable("CENTER_BOTTOM", AnchorPreset::CENTER_BOTTOM)
        .addVariable("CENTER", AnchorPreset::CENTER)
        .addVariable("LEFT_WIDE", AnchorPreset::LEFT_WIDE)
        .addVariable("TOP_WIDE", AnchorPreset::TOP_WIDE)
        .addVariable("RIGHT_WIDE", AnchorPreset::RIGHT_WIDE)
        .addVariable("BOTTOM_WIDE", AnchorPreset::BOTTOM_WIDE)
        .addVariable("VERTICAL_CENTER_WIDE", AnchorPreset::VERTICAL_CENTER_WIDE)
        .addVariable("HORIZONTAL_CENTER_WIDE", AnchorPreset::HORIZONTAL_CENTER_WIDE)
        .addVariable("FULL_LAYOUT", AnchorPreset::FULL_LAYOUT)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("PivotPreset")
        .addVariable("CENTER", PivotPreset::CENTER)
        .addVariable("TOP_CENTER", PivotPreset::TOP_CENTER)
        .addVariable("BOTTOM_CENTER", PivotPreset::BOTTOM_CENTER)
        .addVariable("LEFT_CENTER", PivotPreset::LEFT_CENTER)
        .addVariable("RIGHT_CENTER", PivotPreset::RIGHT_CENTER)
        .addVariable("TOP_LEFT", PivotPreset::TOP_LEFT)
        .addVariable("BOTTOM_LEFT", PivotPreset::BOTTOM_LEFT)
        .addVariable("TOP_RIGHT", PivotPreset::TOP_RIGHT)
        .addVariable("BOTTOM_RIGHT", PivotPreset::BOTTOM_RIGHT)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LayoutGroupType")
        .addVariable("VERTICAL", LayoutGroupType::VERTICAL)
        .addVariable("HORIZONTAL", LayoutGroupType::HORIZONTAL)
        .addVariable("VERTICAL_WRAP", LayoutGroupType::VERTICAL_WRAP)
        .addVariable("HORIZONTAL_WRAP", LayoutGroupType::HORIZONTAL_WRAP)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ScrollbarType")
        .addVariable("VERTICAL", ScrollbarType::VERTICAL)
        .addVariable("HORIZONTAL", ScrollbarType::HORIZONTAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ProgressbarType")
        .addVariable("VERTICAL", ProgressbarType::VERTICAL)
        .addVariable("HORIZONTAL", ProgressbarType::HORIZONTAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FogType")
        .addVariable("LINEAR", FogType::LINEAR)
        .addVariable("EXPONENTIAL", FogType::EXPONENTIAL)
        .addVariable("EXPONENTIALSQUARED", FogType::EXPONENTIALSQUARED)
        .addVariable("HEIGHT", FogType::HEIGHT)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FogColorMode")
        .addVariable("COLOR", FogColorMode::COLOR)
        .addVariable("SKY", FogColorMode::SKY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FogVolumeShape")
        .addVariable("BOX", FogVolumeShape::BOX)
        .addVariable("SPHERE", FogVolumeShape::SPHERE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FogVolumeFalloff")
        .addVariable("LINEAR", FogVolumeFalloff::LINEAR)
        .addVariable("EXPONENTIAL", FogVolumeFalloff::EXPONENTIAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CloudsPreset")
        .addVariable("CUSTOM", CloudsPreset::CUSTOM)
        .addVariable("SPARSE", CloudsPreset::SPARSE)
        .addVariable("CLOUDY", CloudsPreset::CLOUDY)
        .addVariable("OVERCAST", CloudsPreset::OVERCAST)
        .addVariable("STORMY", CloudsPreset::STORMY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CloudsRendering")
        .addVariable("AUTO", CloudsRendering::AUTO)
        .addVariable("SKY_MAP", CloudsRendering::SKY_MAP)
        .addVariable("SCREEN", CloudsRendering::SCREEN)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("AtmosphereQuality")
        .addVariable("OFF", AtmosphereQuality::OFF)
        .addVariable("LOW", AtmosphereQuality::LOW)
        .addVariable("MEDIUM", AtmosphereQuality::MEDIUM)
        .addVariable("HIGH", AtmosphereQuality::HIGH)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("AtmosphereQualityOverride")
        .addVariable("PROJECT", AtmosphereQualityOverride::PROJECT)
        .addVariable("LOW", AtmosphereQualityOverride::LOW)
        .addVariable("MEDIUM", AtmosphereQualityOverride::MEDIUM)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ParticleEmitterShape")
        .addVariable("Box", ParticleEmitterShape::Box)
        .addVariable("Sphere", ParticleEmitterShape::Sphere)
        .addVariable("Hemisphere", ParticleEmitterShape::Hemisphere)
        .addVariable("Circle", ParticleEmitterShape::Circle)
        .addVariable("Cone", ParticleEmitterShape::Cone)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<ActionSystem>("ActionSystem")
        .addFunction("actionStart", &ActionSystem::actionStart)
        .addFunction("actionStop", &ActionSystem::actionStop)
        .addFunction("actionPause", &ActionSystem::actionPause)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AudioSystem>("AudioSystem")
        .addStaticFunction("stopAll", &AudioSystem::stopAll)
        .addStaticFunction("pauseAll", &AudioSystem::pauseAll)
        .addStaticFunction("resumeAll", &AudioSystem::resumeAll)
        .addStaticFunction("checkActive", &AudioSystem::checkActive)
        .addStaticProperty("globalVolume", &AudioSystem::getGlobalVolume,  &AudioSystem::setGlobalVolume)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<MeshSystem>("MeshSystem")
        .addFunction("createPlane", &MeshSystem::createPlane)
        .addFunction("createBox", &MeshSystem::createBox)
        .addFunction("createSphere", &MeshSystem::createSphere)
        .addFunction("createCylinder", &MeshSystem::createCylinder)
        .addFunction("createCapsule", &MeshSystem::createCapsule)
        .addFunction("createTorus", &MeshSystem::createTorus)
        .addFunction("loadGLTF", &MeshSystem::loadGLTF)
        .addFunction("loadOBJ", &MeshSystem::loadOBJ)
        .addFunction("createInstancedMesh", &MeshSystem::createInstancedMesh)
        .addFunction("removeInstancedMesh", &MeshSystem::removeInstancedMesh)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<PhysicsSystem>("PhysicsSystem")
        .addProperty("gravity", (Vector3(PhysicsSystem::*)() const)&PhysicsSystem::getGravity, (void(PhysicsSystem::*)(Vector3))&PhysicsSystem::setGravity)
        .addFunction("setGravity",
            luabridge::overload<Vector3>(&PhysicsSystem::setGravity),
            luabridge::overload<float, float>(&PhysicsSystem::setGravity),
            luabridge::overload<float, float, float>(&PhysicsSystem::setGravity))
        .addProperty("gravity2D", (Vector2(PhysicsSystem::*)() const)&PhysicsSystem::getGravity2D, (void(PhysicsSystem::*)(Vector2))&PhysicsSystem::setGravity2D)
        .addFunction("setGravity2D",
            luabridge::overload<Vector2>(&PhysicsSystem::setGravity2D),
            luabridge::overload<float, float>(&PhysicsSystem::setGravity2D))
        .addProperty("gravity3D", (Vector3(PhysicsSystem::*)() const)&PhysicsSystem::getGravity3D, (void(PhysicsSystem::*)(Vector3))&PhysicsSystem::setGravity3D)
        .addFunction("setGravity3D",
            luabridge::overload<Vector3>(&PhysicsSystem::setGravity3D),
            luabridge::overload<float, float, float>(&PhysicsSystem::setGravity3D))
#ifdef JASPERENGINE_PHYSICS_2D
        .addProperty("pointsToMeterScale2D", &PhysicsSystem::getPointsToMeterScale2D,  &PhysicsSystem::setPointsToMeterScale2D)
        .addProperty("beginContact2D", [] (PhysicsSystem* self, lua_State* L) { return &self->beginContact2D; }, [] (PhysicsSystem* self, lua_State* L) { self->beginContact2D = L; })
        .addProperty("endContact2D", [] (PhysicsSystem* self, lua_State* L) { return &self->endContact2D; }, [] (PhysicsSystem* self, lua_State* L) { self->endContact2D = L; })
        .addProperty("beginSensorContact2D", [] (PhysicsSystem* self, lua_State* L) { return &self->beginSensorContact2D; }, [] (PhysicsSystem* self, lua_State* L) { self->beginSensorContact2D = L; })
        .addProperty("endSensorContact2D", [] (PhysicsSystem* self, lua_State* L) { return &self->endSensorContact2D; }, [] (PhysicsSystem* self, lua_State* L) { self->endSensorContact2D = L; })
        .addProperty("hitContact2D", [] (PhysicsSystem* self, lua_State* L) { return &self->hitContact2D; }, [] (PhysicsSystem* self, lua_State* L) { self->hitContact2D = L; })
        .addProperty("preSolve2D", [] (PhysicsSystem* self, lua_State* L) { return &self->preSolve2D; }, [] (PhysicsSystem* self, lua_State* L) { self->preSolve2D = L; })
        .addProperty("shouldCollide2D", [] (PhysicsSystem* self, lua_State* L) { return &self->shouldCollide2D; }, [] (PhysicsSystem* self, lua_State* L) { self->shouldCollide2D = L; })
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        .addProperty("lock3DBodies", &PhysicsSystem::isLock3DBodies, &PhysicsSystem::setLock3DBodies)
        // True inside a contact callback, where a body can be moved but not created
        .addProperty("steppingWorld3D", &PhysicsSystem::isSteppingWorld3D)
        .addProperty("onBodyActivated3D", [] (PhysicsSystem* self, lua_State* L) { return &self->onBodyActivated3D; }, [] (PhysicsSystem* self, lua_State* L) { self->onBodyActivated3D = L; })
        .addProperty("onBodyDeactivated3D", [] (PhysicsSystem* self, lua_State* L) { return &self->onBodyDeactivated3D; }, [] (PhysicsSystem* self, lua_State* L) { self->onBodyDeactivated3D = L; })
        .addProperty("onContactAdded3D", [] (PhysicsSystem* self, lua_State* L) { return &self->onContactAdded3D; }, [] (PhysicsSystem* self, lua_State* L) { self->onContactAdded3D = L; })
        .addProperty("onContactPersisted3D", [] (PhysicsSystem* self, lua_State* L) { return &self->onContactPersisted3D; }, [] (PhysicsSystem* self, lua_State* L) { self->onContactPersisted3D = L; })
        .addProperty("onContactRemoved3D", [] (PhysicsSystem* self, lua_State* L) { return &self->onContactRemoved3D; }, [] (PhysicsSystem* self, lua_State* L) { self->onContactRemoved3D = L; })
        .addProperty("shouldCollide3D", [] (PhysicsSystem* self, lua_State* L) { return &self->shouldCollide3D; }, [] (PhysicsSystem* self, lua_State* L) { self->shouldCollide3D = L; })
#endif
#ifdef JASPERENGINE_PHYSICS_2D
        .addFunction("createRigidbody2D", &PhysicsSystem::createRigidbody2D)
        .addFunction("removeRigidbody2D", &PhysicsSystem::removeRigidbody2D)
        .addFunction("loadRigidbody2D", &PhysicsSystem::loadRigidbody2D)
        .addFunction("destroyRigidbody2D", &PhysicsSystem::destroyRigidbody2D)
        .addFunction("destroyCollider2D", &PhysicsSystem::destroyCollider2D)
        .addFunction("loadDistanceJoint2D", &PhysicsSystem::loadDistanceJoint2D)
        .addFunction("loadRevoluteJoint2D", &PhysicsSystem::loadRevoluteJoint2D)
        .addFunction("loadPrismaticJoint2D", &PhysicsSystem::loadPrismaticJoint2D)
        .addFunction("loadMouseJoint2D", &PhysicsSystem::loadMouseJoint2D)
        .addFunction("loadWheelJoint2D", &PhysicsSystem::loadWheelJoint2D)
        .addFunction("loadWeldJoint2D", &PhysicsSystem::loadWeldJoint2D)
        .addFunction("loadMotorJoint2D", &PhysicsSystem::loadMotorJoint2D)
        .addFunction("destroyJoint2D", &PhysicsSystem::destroyJoint2D)
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        .addFunction("createRigidbody", &PhysicsSystem::createRigidbody)
        .addFunction("removeRigidbody", &PhysicsSystem::removeRigidbody)
        .addFunction("loadRigidbody", &PhysicsSystem::loadRigidbody)
        .addFunction("destroyRigidbody", &PhysicsSystem::destroyRigidbody)
        .addFunction("destroyCollider3D", &PhysicsSystem::destroyCollider3D)
        .addFunction("loadFixedJoint3D", &PhysicsSystem::loadFixedJoint3D)
        .addFunction("loadDistanceJoint3D", &PhysicsSystem::loadDistanceJoint3D)
        .addFunction("loadPointJoint3D", &PhysicsSystem::loadPointJoint3D)
        .addFunction("loadHingeJoint3D", &PhysicsSystem::loadHingeJoint3D)
        .addFunction("loadConeJoint3D", &PhysicsSystem::loadConeJoint3D)
        .addFunction("loadPrismaticJoint3D", &PhysicsSystem::loadPrismaticJoint3D)
        .addFunction("loadSwingTwistJoint3D", &PhysicsSystem::loadSwingTwistJoint3D)
        .addFunction("loadSixDOFJoint3D", &PhysicsSystem::loadSixDOFJoint3D)
        .addFunction("loadPathJoint3D", &PhysicsSystem::loadPathJoint3D)
        .addFunction("loadGearJoint3D", &PhysicsSystem::loadGearJoint3D)
        .addFunction("loadRackAndPinionJoint3D", &PhysicsSystem::loadRackAndPinionJoint3D)
        .addFunction("loadPulleyJoint3D", &PhysicsSystem::loadPulleyJoint3D)
        .addFunction("destroyJoint3D", &PhysicsSystem::destroyJoint3D)
        .addFunction("addBroadPhaseLayer3D",
            luabridge::overload<uint8_t, uint32_t>(&PhysicsSystem::addBroadPhaseLayer3D),
            luabridge::overload<uint8_t, uint32_t, uint32_t>(&PhysicsSystem::addBroadPhaseLayer3D))
#endif
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<RenderSystem>("RenderSystem")
        .addFunction("updateCameraSize", &RenderSystem::updateCameraSize)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UISystem>("UISystem")
        .addFunction("getAnchorReferenceRect", &UISystem::getAnchorReferenceRect)
        .addFunction("setAnchorReferenceSize", &UISystem::setAnchorReferenceSize)
        .addFunction("clearAnchorReferenceSize", &UISystem::clearAnchorReferenceSize)
        .addFunction("getAnchorReferenceWidth", &UISystem::getAnchorReferenceWidth)
        .addFunction("getAnchorReferenceHeight", &UISystem::getAnchorReferenceHeight)
        .addFunction("isInputFieldFocused", &UISystem::isInputFieldFocused)
        .addFunction("eventOnCharInput", &UISystem::eventOnCharInput)
        .addFunction("eventOnKeyDown", &UISystem::eventOnKeyDown)
        .addFunction("eventOnPointerDown", &UISystem::eventOnPointerDown)
        .addFunction("eventOnPointerUp", &UISystem::eventOnPointerUp)
        .addFunction("eventOnPointerMove", &UISystem::eventOnPointerMove)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<EntityManager>("EntityManager")
        .addConstructor <void (*) (void)> ()
        .addFunction("createUserEntity", &EntityManager::createUserEntity)
        .addFunction("createSystemEntity", &EntityManager::createSystemEntity)
        .addFunction("destroy", &EntityManager::destroy)
        .addFunction("setSignature", &EntityManager::setSignature)
        .addFunction("getSignature", &EntityManager::getSignature)
        .addFunction("setName", &EntityManager::setName)
        .addFunction("getName", &EntityManager::getName)
        .addFunction("isCreated", &EntityManager::isCreated)
        .addFunction("recreateEntity", &EntityManager::recreateEntity)
        .addFunction("getLastUserEntity", &EntityManager::getLastUserEntity)
        .endClass();

    luabridge::getGlobalNamespace(L)
        // Component fields are the raw data: writing one takes effect when its needUpdate
        // flag is set too, the way the object setters do it. State the systems own is read-only.
        .beginClass<ActionComponent>("ActionComponent")
        .addProperty("state", &ActionComponent::state)
        .addProperty("timecount", &ActionComponent::timecount)
        .addProperty("speed", &ActionComponent::speed, &setTrackedMember<&ActionComponent::speed>)
        .addProperty("startTrigger", &ActionComponent::startTrigger, &setTrackedMember<&ActionComponent::startTrigger>)
        .addProperty("stopTrigger", &ActionComponent::stopTrigger, &setTrackedMember<&ActionComponent::stopTrigger>)
        .addProperty("pauseTrigger", &ActionComponent::pauseTrigger, &setTrackedMember<&ActionComponent::pauseTrigger>)
        .addProperty("ownedTarget", &ActionComponent::ownedTarget, &setTrackedMember<&ActionComponent::ownedTarget>)
        .addProperty("target", &ActionComponent::target, &setTrackedMember<&ActionComponent::target>)
        .addProperty("onStart", [] (ActionComponent* self, lua_State* L) { return &self->onStart; }, [] (ActionComponent* self, lua_State* L) { self->onStart = L; })
        .addProperty("onPause", [] (ActionComponent* self, lua_State* L) { return &self->onPause; }, [] (ActionComponent* self, lua_State* L) { self->onPause = L; })
        .addProperty("onStop", [] (ActionComponent* self, lua_State* L) { return &self->onStop; }, [] (ActionComponent* self, lua_State* L) { self->onStop = L; })
        .addProperty("onStep", [] (ActionComponent* self, lua_State* L) { return &self->onStep; }, [] (ActionComponent* self, lua_State* L) { self->onStep = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ActionFrame>("ActionFrame")
        .addProperty("startTime", &ActionFrame::startTime, &ActionFrame::startTime)
        .addProperty("duration", &ActionFrame::duration, &ActionFrame::duration)
        .addProperty("action", &ActionFrame::action, &ActionFrame::action)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimationComponent>("AnimationComponent")
        .addProperty("actions", &AnimationComponent::actions, &setTrackedMember<&AnimationComponent::actions>)
        .addProperty("ownedActions", &AnimationComponent::ownedActions, &setTrackedMember<&AnimationComponent::ownedActions>)
        .addProperty("loop", &AnimationComponent::loop, &setTrackedMember<&AnimationComponent::loop>)
        .addProperty("duration", &AnimationComponent::duration, &setTrackedMember<&AnimationComponent::duration>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TweenComponent>("TweenComponent")
        .addProperty("time", &TweenComponent::time)
        .addProperty("value", &TweenComponent::value)
        .addProperty("duration", &TweenComponent::duration, &setTrackedMember<&TweenComponent::duration>)
        .addProperty("loop", &TweenComponent::loop, &setTrackedMember<&TweenComponent::loop>)
        .addProperty("function", [] (TweenComponent* self, lua_State* L) { return &self->function; }, [] (TweenComponent* self, lua_State* L) { self->function = L; })
        .endClass();
    
    luabridge::getGlobalNamespace(L)
        .beginClass<RectTransformComponent>("RectTransformComponent")
        .addProperty("width", &RectTransformComponent::width, &setTrackedMember<&RectTransformComponent::width>)
        .addProperty("height", &RectTransformComponent::height, &setTrackedMember<&RectTransformComponent::height>)
        .addProperty("anchorPointLeft", &RectTransformComponent::anchorPointLeft, &setTrackedMember<&RectTransformComponent::anchorPointLeft>)
        .addProperty("anchorPointTop", &RectTransformComponent::anchorPointTop, &setTrackedMember<&RectTransformComponent::anchorPointTop>)
        .addProperty("anchorPointRight", &RectTransformComponent::anchorPointRight, &setTrackedMember<&RectTransformComponent::anchorPointRight>)
        .addProperty("anchorPointBottom", &RectTransformComponent::anchorPointBottom, &setTrackedMember<&RectTransformComponent::anchorPointBottom>)
        .addProperty("anchorOffsetLeft", &RectTransformComponent::anchorOffsetLeft, &setTrackedMember<&RectTransformComponent::anchorOffsetLeft>)
        .addProperty("anchorOffsetTop", &RectTransformComponent::anchorOffsetTop, &setTrackedMember<&RectTransformComponent::anchorOffsetTop>)
        .addProperty("anchorOffsetRight", &RectTransformComponent::anchorOffsetRight, &setTrackedMember<&RectTransformComponent::anchorOffsetRight>)
        .addProperty("anchorOffsetBottom", &RectTransformComponent::anchorOffsetBottom, &setTrackedMember<&RectTransformComponent::anchorOffsetBottom>)
        .addProperty("positionOffset", &RectTransformComponent::positionOffset, &setTrackedMember<&RectTransformComponent::positionOffset>)
        .addProperty("pivot", &RectTransformComponent::pivot, &setTrackedMember<&RectTransformComponent::pivot>)
        .addProperty("anchorPreset", &RectTransformComponent::anchorPreset, &setTrackedMember<&RectTransformComponent::anchorPreset>)
        .addProperty("usingAnchors", &RectTransformComponent::usingAnchors, &setTrackedMember<&RectTransformComponent::usingAnchors>)
        .addProperty("panel", &RectTransformComponent::panel)
        .addProperty("containerBoxIndex", &RectTransformComponent::containerBoxIndex)
        .addProperty("scissor", &RectTransformComponent::scissor, &setTrackedMember<&RectTransformComponent::scissor>)
        .addProperty("ignoreScissor", &RectTransformComponent::ignoreScissor, &setTrackedMember<&RectTransformComponent::ignoreScissor>)
        .addProperty("ignoreEvents", &RectTransformComponent::ignoreEvents, &setTrackedMember<&RectTransformComponent::ignoreEvents>)
        .addProperty("needUpdateSizes", &RectTransformComponent::needUpdateSizes, &setTrackedMember<&RectTransformComponent::needUpdateSizes>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIComponent>("UIComponent")
        .addProperty("loaded", &UIComponent::loaded)
        .addProperty("minBufferCount", &UIComponent::minBufferCount)
        .addProperty("minIndicesCount", &UIComponent::minIndicesCount)
        .addProperty("primitiveType", &UIComponent::primitiveType, &setTrackedMember<&UIComponent::primitiveType>)
        .addProperty("vertexCount", &UIComponent::vertexCount)
        .addProperty("texture", &UIComponent::texture, &setTrackedMember<&UIComponent::texture>)
        // linear, unlike Image::setColor which converts from sRGB
        .addProperty("color", &UIComponent::color, &setTrackedMember<&UIComponent::color>)
        .addProperty("onGetFocus", [] (UIComponent* self, lua_State* L) { return &self->onGetFocus; }, [] (UIComponent* self, lua_State* L) { self->onGetFocus = L; })
        .addProperty("onLostFocus", [] (UIComponent* self, lua_State* L) { return &self->onLostFocus; }, [] (UIComponent* self, lua_State* L) { self->onLostFocus = L; })
        .addProperty("onPointerEnter", [] (UIComponent* self, lua_State* L) { return &self->onPointerEnter; }, [] (UIComponent* self, lua_State* L) { self->onPointerEnter = L; })
        .addProperty("onPointerLeave", [] (UIComponent* self, lua_State* L) { return &self->onPointerLeave; }, [] (UIComponent* self, lua_State* L) { self->onPointerLeave = L; })
        .addProperty("onPointerMove", [] (UIComponent* self, lua_State* L) { return &self->onPointerMove; }, [] (UIComponent* self, lua_State* L) { self->onPointerMove = L; })
        .addProperty("onPointerDown", [] (UIComponent* self, lua_State* L) { return &self->onPointerDown; }, [] (UIComponent* self, lua_State* L) { self->onPointerDown = L; })
        .addProperty("onPointerUp", [] (UIComponent* self, lua_State* L) { return &self->onPointerUp; }, [] (UIComponent* self, lua_State* L) { self->onPointerUp = L; })
        .addProperty("onClick", [] (UIComponent* self, lua_State* L) { return &self->onClick; }, [] (UIComponent* self, lua_State* L) { self->onClick = L; })
        .addProperty("onDoubleClick", [] (UIComponent* self, lua_State* L) { return &self->onDoubleClick; }, [] (UIComponent* self, lua_State* L) { self->onDoubleClick = L; })
        .addProperty("onDragStart", [] (UIComponent* self, lua_State* L) { return &self->onDragStart; }, [] (UIComponent* self, lua_State* L) { self->onDragStart = L; })
        .addProperty("onDrag", [] (UIComponent* self, lua_State* L) { return &self->onDrag; }, [] (UIComponent* self, lua_State* L) { self->onDrag = L; })
        .addProperty("onDragEnd", [] (UIComponent* self, lua_State* L) { return &self->onDragEnd; }, [] (UIComponent* self, lua_State* L) { self->onDragEnd = L; })
        .addProperty("automaticFlipY", &UIComponent::automaticFlipY, &setTrackedMember<&UIComponent::automaticFlipY>)
        .addProperty("flipY", &UIComponent::flipY, &setTrackedMember<&UIComponent::flipY>)
        .addProperty("pointerMoved", &UIComponent::pointerMoved)
        .addProperty("focused", &UIComponent::focused)
        .addProperty("needReload", &UIComponent::needReload, &setTrackedMember<&UIComponent::needReload>)
        .addProperty("needUpdateBuffer", &UIComponent::needUpdateBuffer, &setTrackedMember<&UIComponent::needUpdateBuffer>)
        .addProperty("needUpdateTexture", &UIComponent::needUpdateTexture, &setTrackedMember<&UIComponent::needUpdateTexture>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ButtonComponent>("ButtonComponent")
        .addProperty("label", &ButtonComponent::label)
        .addProperty("textureNormal", &ButtonComponent::textureNormal, &setTrackedMember<&ButtonComponent::textureNormal>)
        .addProperty("textureHovered", &ButtonComponent::textureHovered, &setTrackedMember<&ButtonComponent::textureHovered>)
        .addProperty("texturePressed", &ButtonComponent::texturePressed, &setTrackedMember<&ButtonComponent::texturePressed>)
        .addProperty("textureDisabled", &ButtonComponent::textureDisabled, &setTrackedMember<&ButtonComponent::textureDisabled>)
        // linear, unlike Button::setColorNormal and friends which convert from sRGB
        .addProperty("colorNormal", &ButtonComponent::colorNormal, &setTrackedMember<&ButtonComponent::colorNormal>)
        .addProperty("colorHovered", &ButtonComponent::colorHovered, &setTrackedMember<&ButtonComponent::colorHovered>)
        .addProperty("colorPressed", &ButtonComponent::colorPressed, &setTrackedMember<&ButtonComponent::colorPressed>)
        .addProperty("colorDisabled", &ButtonComponent::colorDisabled, &setTrackedMember<&ButtonComponent::colorDisabled>)
        .addProperty("onPress", [] (ButtonComponent* self, lua_State* L) { return &self->onPress; }, [] (ButtonComponent* self, lua_State* L) { self->onPress = L; })
        .addProperty("onRelease", [] (ButtonComponent* self, lua_State* L) { return &self->onRelease; }, [] (ButtonComponent* self, lua_State* L) { self->onRelease = L; })
        .addProperty("pressed", &ButtonComponent::pressed)
        .addProperty("hovered", &ButtonComponent::hovered)
        .addProperty("disabled", &ButtonComponent::disabled, &setTrackedMember<&ButtonComponent::disabled>)
        .addProperty("needUpdateButton", &ButtonComponent::needUpdateButton, &setTrackedMember<&ButtonComponent::needUpdateButton>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<PanelComponent>("PanelComponent")
        .addProperty("headerimage", &PanelComponent::headerimage)
        .addProperty("headercontainer", &PanelComponent::headercontainer)
        .addProperty("headertext", &PanelComponent::headertext)
        .addProperty("titleAnchorPreset", &PanelComponent::titleAnchorPreset, &setTrackedMember<&PanelComponent::titleAnchorPreset>)
        .addProperty("minWidth", &PanelComponent::minWidth, &setTrackedMember<&PanelComponent::minWidth>)
        .addProperty("minHeight", &PanelComponent::minHeight, &setTrackedMember<&PanelComponent::minHeight>)
        .addProperty("resizeMargin", &PanelComponent::resizeMargin, &setTrackedMember<&PanelComponent::resizeMargin>)
        .addProperty("canMove", &PanelComponent::canMove, &setTrackedMember<&PanelComponent::canMove>)
        .addProperty("canResize", &PanelComponent::canResize, &setTrackedMember<&PanelComponent::canResize>)
        .addProperty("canBringToFront", &PanelComponent::canBringToFront, &setTrackedMember<&PanelComponent::canBringToFront>)
        .addProperty("headerPointerDown", &PanelComponent::headerPointerDown)
        .addProperty("onMove", [] (PanelComponent* self, lua_State* L) { return &self->onMove; }, [] (PanelComponent* self, lua_State* L) { self->onMove = L; })
        .addProperty("onResize", [] (PanelComponent* self, lua_State* L) { return &self->onResize; }, [] (PanelComponent* self, lua_State* L) { self->onResize = L; })
        .addProperty("needUpdatePanel", &PanelComponent::needUpdatePanel, &setTrackedMember<&PanelComponent::needUpdatePanel>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ScrollbarComponent>("ScrollbarComponent")
        .addProperty("bar", &ScrollbarComponent::bar)
        .addProperty("type", &ScrollbarComponent::type, &setTrackedMember<&ScrollbarComponent::type>)
        .addProperty("onChange", [] (ScrollbarComponent* self, lua_State* L) { return &self->onChange; }, [] (ScrollbarComponent* self, lua_State* L) { self->onChange = L; })
        .addProperty("barSize", &ScrollbarComponent::barSize, &setTrackedMember<&ScrollbarComponent::barSize>)
        .addProperty("step", &ScrollbarComponent::step, &setTrackedMember<&ScrollbarComponent::step>)
        .addProperty("barMarginLeft", &ScrollbarComponent::barMarginLeft, &setTrackedMember<&ScrollbarComponent::barMarginLeft>)
        .addProperty("barMarginRight", &ScrollbarComponent::barMarginRight, &setTrackedMember<&ScrollbarComponent::barMarginRight>)
        .addProperty("barMarginTop", &ScrollbarComponent::barMarginTop, &setTrackedMember<&ScrollbarComponent::barMarginTop>)
        .addProperty("barMarginBottom", &ScrollbarComponent::barMarginBottom, &setTrackedMember<&ScrollbarComponent::barMarginBottom>)
        .addProperty("barPointerDown", &ScrollbarComponent::barPointerDown)
        .addProperty("barPointerPos", &ScrollbarComponent::barPointerPos)
        .addProperty("needUpdateScrollbar", &ScrollbarComponent::needUpdateScrollbar, &setTrackedMember<&ScrollbarComponent::needUpdateScrollbar>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ProgressbarComponent>("ProgressbarComponent")
        .addProperty("fill", &ProgressbarComponent::fill)
        .addProperty("type", &ProgressbarComponent::type, &setTrackedMember<&ProgressbarComponent::type>)
        .addProperty("value", &ProgressbarComponent::value, &setTrackedMember<&ProgressbarComponent::value>)
        .addProperty("fillMarginLeft", &ProgressbarComponent::fillMarginLeft, &setTrackedMember<&ProgressbarComponent::fillMarginLeft>)
        .addProperty("fillMarginRight", &ProgressbarComponent::fillMarginRight, &setTrackedMember<&ProgressbarComponent::fillMarginRight>)
        .addProperty("fillMarginTop", &ProgressbarComponent::fillMarginTop, &setTrackedMember<&ProgressbarComponent::fillMarginTop>)
        .addProperty("fillMarginBottom", &ProgressbarComponent::fillMarginBottom, &setTrackedMember<&ProgressbarComponent::fillMarginBottom>)
        .addProperty("needUpdateProgressbar", &ProgressbarComponent::needUpdateProgressbar, &setTrackedMember<&ProgressbarComponent::needUpdateProgressbar>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<InputFieldComponent>("InputFieldComponent")
        .addProperty("text", &InputFieldComponent::text)
        .addProperty("selection", &InputFieldComponent::selection)
        .addProperty("cursor", &InputFieldComponent::cursor)
        .addProperty("cursorBlink", &InputFieldComponent::cursorBlink, &setTrackedMember<&InputFieldComponent::cursorBlink>)
        .addProperty("cursorWidth", &InputFieldComponent::cursorWidth, &setTrackedMember<&InputFieldComponent::cursorWidth>)
        // linear, unlike InputField::setCursorColor and friends which convert from sRGB
        .addProperty("cursorColor", &InputFieldComponent::cursorColor, &setTrackedMember<&InputFieldComponent::cursorColor>)
        .addProperty("selectionColor", &InputFieldComponent::selectionColor, &setTrackedMember<&InputFieldComponent::selectionColor>)
        .addProperty("placeholderColor", &InputFieldComponent::placeholderColor, &setTrackedMember<&InputFieldComponent::placeholderColor>)
        .addProperty("placeholder", &InputFieldComponent::placeholder, &setTrackedMember<&InputFieldComponent::placeholder>)
        .addProperty("passwordChar", &InputFieldComponent::passwordChar, &setTrackedMember<&InputFieldComponent::passwordChar>)
        .addProperty("cursorIndex", &InputFieldComponent::cursorIndex, &setTrackedMember<&InputFieldComponent::cursorIndex>)
        .addProperty("selectionAnchor", &InputFieldComponent::selectionAnchor, &setTrackedMember<&InputFieldComponent::selectionAnchor>)
        .addProperty("scrollOffset", &InputFieldComponent::scrollOffset, &setTrackedMember<&InputFieldComponent::scrollOffset>)
        .addProperty("disabled", &InputFieldComponent::disabled, &setTrackedMember<&InputFieldComponent::disabled>)
        .addProperty("password", &InputFieldComponent::password, &setTrackedMember<&InputFieldComponent::password>)
        .addProperty("onChange", [] (InputFieldComponent* self, lua_State* L) { return &self->onChange; }, [] (InputFieldComponent* self, lua_State* L) { self->onChange = L; })
        .addProperty("onSubmit", [] (InputFieldComponent* self, lua_State* L) { return &self->onSubmit; }, [] (InputFieldComponent* self, lua_State* L) { self->onSubmit = L; })
        .addProperty("needUpdateInputField", &InputFieldComponent::needUpdateInputField, &setTrackedMember<&InputFieldComponent::needUpdateInputField>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ParticleBurst>("ParticleBurst")
        .addProperty("time", &ParticleBurst::time, &ParticleBurst::time)
        .addProperty("minCount", &ParticleBurst::minCount, &ParticleBurst::minCount)
        .addProperty("maxCount", &ParticleBurst::maxCount, &ParticleBurst::maxCount)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ParticleColorGradientStop>("ParticleColorGradientStop")
        .addProperty("time", &ParticleColorGradientStop::time, &ParticleColorGradientStop::time)
        .addProperty("color", &ParticleColorGradientStop::color, &ParticleColorGradientStop::color)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ParticleSystemComponent>("ParticleSystemComponent")
        .addProperty("enabled", &ParticleSystemComponent::enabled, &setTrackedMember<&ParticleSystemComponent::enabled>)
        .addProperty("playOnAwake", &ParticleSystemComponent::playOnAwake, &setTrackedMember<&ParticleSystemComponent::playOnAwake>)
        .addProperty("seed", &ParticleSystemComponent::seed, &setTrackedMember<&ParticleSystemComponent::seed>)
        .addProperty("playbackSpeed", &ParticleSystemComponent::playbackSpeed, &setTrackedMember<&ParticleSystemComponent::playbackSpeed>)
        .addProperty("effect", [] (const ParticleSystemComponent* self) { return self->effect.path; },
                     [] (ParticleSystemComponent* self, std::string path) { self->effect = path; self->needReload = true; })
        .addProperty("onCollision", [] (ParticleSystemComponent* self, lua_State* L) { return &self->onCollision; }, [] (ParticleSystemComponent* self, lua_State* L) { self->onCollision = L; })
        .addProperty("onParticleDeath", [] (ParticleSystemComponent* self, lua_State* L) { return &self->onParticleDeath; }, [] (ParticleSystemComponent* self, lua_State* L) { self->onParticleDeath = L; })
        .addProperty("onSubEmit", [] (ParticleSystemComponent* self, lua_State* L) { return &self->onSubEmit; }, [] (ParticleSystemComponent* self, lua_State* L) { self->onSubEmit = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AudioSourceComponent>("AudioSourceComponent")
        .addProperty("handle", &AudioSourceComponent::handle)
        .addProperty("state", &AudioSourceComponent::state)
        .addProperty("filename", &AudioSourceComponent::filename, &setTrackedMember<&AudioSourceComponent::filename>)
        .addProperty("loaded", &AudioSourceComponent::loaded)
        .addProperty("loadFailed", &AudioSourceComponent::loadFailed)
        .addProperty("enableClocked", &AudioSourceComponent::enableClocked, &setTrackedMember<&AudioSourceComponent::enableClocked>)
        .addProperty("lastPosition", &AudioSourceComponent::lastPosition)
        .addProperty("startTrigger", &AudioSourceComponent::startTrigger, &setTrackedMember<&AudioSourceComponent::startTrigger>)
        .addProperty("stopTrigger", &AudioSourceComponent::stopTrigger, &setTrackedMember<&AudioSourceComponent::stopTrigger>)
        .addProperty("pauseTrigger", &AudioSourceComponent::pauseTrigger, &setTrackedMember<&AudioSourceComponent::pauseTrigger>)
        .addProperty("onStart", [] (AudioSourceComponent* self, lua_State* L) { return &self->onStart; }, [] (AudioSourceComponent* self, lua_State* L) { self->onStart = L; })
        .addProperty("onPause", [] (AudioSourceComponent* self, lua_State* L) { return &self->onPause; }, [] (AudioSourceComponent* self, lua_State* L) { self->onPause = L; })
        .addProperty("onStop", [] (AudioSourceComponent* self, lua_State* L) { return &self->onStop; }, [] (AudioSourceComponent* self, lua_State* L) { self->onStop = L; })
        .addProperty("volume", &AudioSourceComponent::volume, &setTrackedMember<&AudioSourceComponent::volume>)
        .addProperty("pan", &AudioSourceComponent::pan, &setTrackedMember<&AudioSourceComponent::pan>)
        .addProperty("speed", &AudioSourceComponent::speed, &setTrackedMember<&AudioSourceComponent::speed>)
        .addProperty("looping", &AudioSourceComponent::looping, &setTrackedMember<&AudioSourceComponent::looping>)
        .addProperty("loopingPoint", &AudioSourceComponent::loopingPoint, &setTrackedMember<&AudioSourceComponent::loopingPoint>)
        .addProperty("protectVoice", &AudioSourceComponent::protectVoice, &setTrackedMember<&AudioSourceComponent::protectVoice>)
        .addProperty("inaudibleBehaviorMustTick", &AudioSourceComponent::inaudibleBehaviorMustTick, &setTrackedMember<&AudioSourceComponent::inaudibleBehaviorMustTick>)
        .addProperty("inaudibleBehaviorKill", &AudioSourceComponent::inaudibleBehaviorKill, &setTrackedMember<&AudioSourceComponent::inaudibleBehaviorKill>)
        .addProperty("minDistance", &AudioSourceComponent::minDistance, &setTrackedMember<&AudioSourceComponent::minDistance>)
        .addProperty("maxDistance", &AudioSourceComponent::maxDistance, &setTrackedMember<&AudioSourceComponent::maxDistance>)
        .addProperty("attenuationModel", &AudioSourceComponent::attenuationModel, &setTrackedMember<&AudioSourceComponent::attenuationModel>)
        .addProperty("attenuationRolloffFactor", &AudioSourceComponent::attenuationRolloffFactor, &setTrackedMember<&AudioSourceComponent::attenuationRolloffFactor>)
        .addProperty("dopplerFactor", &AudioSourceComponent::dopplerFactor, &setTrackedMember<&AudioSourceComponent::dopplerFactor>)
        .addProperty("length", &AudioSourceComponent::length)
        .addProperty("playingTime", &AudioSourceComponent::playingTime)
        .addProperty("needUpdate", &AudioSourceComponent::needUpdate, &setTrackedMember<&AudioSourceComponent::needUpdate>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TileRectData>("TileRectData")
        .addProperty("name", &TileRectData::name, &TileRectData::name)
        .addProperty("submeshId", &TileRectData::submeshId, &TileRectData::submeshId)
        .addProperty("rect", &TileRectData::rect, &TileRectData::rect)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TileData>("TileData")
        .addProperty("name", &TileData::name, &TileData::name)
        .addProperty("rectId", &TileData::rectId, &TileData::rectId)
        .addProperty("position", &TileData::position, &TileData::position)
        .addProperty("width", &TileData::width, &TileData::width)
        .addProperty("height", &TileData::height, &TileData::height)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<LineData>("LineData")
        .addProperty("pointA", &LineData::pointA, &LineData::pointA)
        .addProperty("pointB", &LineData::pointB, &LineData::pointB)
        .addProperty("colorA", &LineData::colorA, &LineData::colorA)
        .addProperty("colorB", &LineData::colorB, &LineData::colorB)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TilemapComponent>("TilemapComponent")
        .addProperty("width", &TilemapComponent::width, &setTrackedMember<&TilemapComponent::width>)
        .addProperty("height", &TilemapComponent::height, &setTrackedMember<&TilemapComponent::height>)
        .addProperty("automaticFlipY", &TilemapComponent::automaticFlipY, &setTrackedMember<&TilemapComponent::automaticFlipY>)
        .addProperty("flipY", &TilemapComponent::flipY, &setTrackedMember<&TilemapComponent::flipY>)
        .addProperty("textureScaleFactor", &TilemapComponent::textureScaleFactor, &setTrackedMember<&TilemapComponent::textureScaleFactor>)
        .addProperty("reserveTiles", &TilemapComponent::reserveTiles, &setTrackedMember<&TilemapComponent::reserveTiles>)
        .addProperty("numTiles", &TilemapComponent::numTiles, &setTrackedMember<&TilemapComponent::numTiles>)
        .addProperty("needUpdateTilemap", &TilemapComponent::needUpdateTilemap, &setTrackedMember<&TilemapComponent::needUpdateTilemap>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<PointData>("PointData")
        .addProperty("position", &PointData::position, &PointData::position)
        .addProperty("color", &PointData::color, &PointData::color)
        .addProperty("size", &PointData::size, &PointData::size)
        .addProperty("rotation", &PointData::rotation, &PointData::rotation)
        .addProperty("textureRect", &PointData::textureRect, &PointData::textureRect)
        .addProperty("visible", &PointData::visible, &PointData::visible)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<InstanceData>("InstanceData")
        .addProperty("position", &InstanceData::position, &InstanceData::position)
        .addProperty("rotation", &InstanceData::rotation, &InstanceData::rotation)
        .addProperty("scale", &InstanceData::scale, &InstanceData::scale)
        .addProperty("color", &InstanceData::color, &InstanceData::color)
        .addProperty("textureRect", &InstanceData::textureRect, &InstanceData::textureRect)
        .addProperty("visible", &InstanceData::visible, &InstanceData::visible)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
