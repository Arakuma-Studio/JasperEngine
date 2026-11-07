// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The component descriptions (editor/reflect/components/*.cpp) and what their tables share.
// Registry.cpp lists them; nothing else names them directly (reflect::find(ComponentType)).

#include "reflect/Field.h"
#include "reflect/Enums.h"

#include "registry/EntityRegistry.h"
#include "external/IconsFontAwesome6.h"

namespace jasperengine::editor::reflect {

    namespace detail {

        template<typename T>
        void* findComponent(EntityRegistry* registry, Entity entity) {
            return registry->findComponent<T>(entity);
        }

        template<typename T>
        void addComponent(EntityRegistry* registry, Entity entity, void* value) {
            registry->addComponent<T>(entity, std::move(*static_cast<T*>(value)));
        }

        template<typename T>
        void removeComponent(EntityRegistry* registry, Entity entity) {
            registry->removeComponent<T>(entity);
        }

        template<typename T>
        ComponentId componentIdOf(const EntityRegistry* registry) {
            return registry->getComponentId<T>();
        }

    }

    extern const ComponentDesc kTransformDesc;
    extern const ComponentDesc kScriptDesc;
    extern const ComponentDesc kMeshDesc;
    extern const ComponentDesc kLineRendererDesc;
    extern const ComponentDesc kLightDesc;
    extern const ComponentDesc kTerrainDesc;
    extern const ComponentDesc kPointsDesc;
    extern const ComponentDesc kSkyboxDesc;
    extern const ComponentDesc kModelDesc;
    extern const ComponentDesc kUIDesc;
    extern const ComponentDesc kRectTransformDesc;
    extern const ComponentDesc kActionDesc;
    extern const ComponentDesc kTweenDesc;
    extern const ComponentDesc kAlphaTweenDesc;
    extern const ComponentDesc kAnimationDesc;
    extern const ComponentDesc kAudioSourceDesc;
    extern const ComponentDesc kRigidbody2DDesc;
    extern const ComponentDesc kRigidbodyDesc;
    extern const ComponentDesc kBoneDesc;
    extern const ComponentDesc kPrefabDesc;
    extern const ComponentDesc kButtonDesc;
    extern const ComponentDesc kCameraDesc;
    extern const ComponentDesc kColorTweenDesc;
    extern const ComponentDesc kFogDesc;
    extern const ComponentDesc kImageDesc;
    extern const ComponentDesc kInstancedMeshDesc;
    extern const ComponentDesc kJoint2DDesc;
    extern const ComponentDesc kJoint3DDesc;
    extern const ComponentDesc kKeyframeTracksDesc;
    extern const ComponentDesc kMeshPolygonDesc;
    extern const ComponentDesc kMorphTracksDesc;
    extern const ComponentDesc kPanelDesc;
    extern const ComponentDesc kParticleSystemDesc;
    extern const ComponentDesc kPolygonDesc;
    extern const ComponentDesc kPositionTweenDesc;
    extern const ComponentDesc kProgressbarDesc;
    extern const ComponentDesc kRotateTracksDesc;
    extern const ComponentDesc kRotationTweenDesc;
    extern const ComponentDesc kScaleTweenDesc;
    extern const ComponentDesc kScaleTracksDesc;
    extern const ComponentDesc kScrollbarDesc;
    extern const ComponentDesc kSpriteAnimationDesc;
    extern const ComponentDesc kSpriteDesc;
    extern const ComponentDesc kTextDesc;
    extern const ComponentDesc kInputFieldDesc;
    extern const ComponentDesc kTilemapDesc;
    extern const ComponentDesc kTranslateTracksDesc;
    extern const ComponentDesc kLayoutGroupDesc;
    extern const ComponentDesc kMirrorDesc;
    extern const ComponentDesc kLight2DDesc;
    extern const ComponentDesc kShadowCaster2DDesc;
    extern const ComponentDesc kCapsuleShadowCasterDesc;
    extern const ComponentDesc kReflectionProbeDesc;
    extern const ComponentDesc kUIDocumentDesc;
    extern const ComponentDesc kLocalVolumetricFogDesc;
    extern const ComponentDesc kVolumetricCloudsDesc;
    extern const ComponentDesc kFluidSimulationDesc;
    extern const ComponentDesc kLatticeDesc;
    extern const ComponentDesc kLatticeModifierDesc;
    extern const ComponentDesc kLatticeFollowerDesc;
    extern const ComponentDesc kLatticeTracksDesc;
    extern const ComponentDesc kForceFieldDesc;
    extern const ComponentDesc kCharacterController2DDesc;
    extern const ComponentDesc kCharacterControllerDesc;
    extern const ComponentDesc kWaterVolumeDesc;
    extern const ComponentDesc kBuoyancyDesc;
    extern const ComponentDesc kPoseAnimatorDesc;
    extern const ComponentDesc kAnimationPlayerDesc;
    // navigation (S19, NavigationReflect.cpp)
    extern const ComponentDesc kNavMeshSurfaceDesc;
    extern const ComponentDesc kNavMeshAgentDesc;
    extern const ComponentDesc kNavMeshObstacleDesc;
    extern const ComponentDesc kNavMeshLinkDesc;
    extern const ComponentDesc kNavMeshModifierDesc;
    extern const ComponentDesc kNavGridDesc;
    extern const ComponentDesc kSplineDesc;
    extern const ComponentDesc kSplineExtrudeDesc;
    extern const ComponentDesc kSplineInstantiateDesc;
    extern const ComponentDesc kSplineAnimateDesc;
    extern const ComponentDesc kAnimatorDesc;
    extern const ComponentDesc kVertexAnimatorDesc;
    // upstream's components (UpstreamReflect.cpp)
    extern const ComponentDesc kTrailRendererDesc;
    extern const ComponentDesc kCanvasGroupDesc;
    extern const ComponentDesc kOnScreenStickDesc;
    extern const ComponentDesc kOnScreenButtonDesc;
    extern const ComponentDesc kTouchAreaDesc;
    extern const ComponentDesc kRagdollDesc;
    // UpstreamMediaReflect.cpp (camera shake, ui-scaling-text, video-mp4, safe-area)
    extern const ComponentDesc kCameraShakerDesc;
    extern const ComponentDesc kCameraFollowDesc;
    extern const ComponentDesc kTextEffectDesc;
    extern const ComponentDesc kVideoPlayerDesc;
    extern const ComponentDesc kSafeAreaDesc;
    extern const ComponentDesc kAudioSnapshotDesc;
    extern const ComponentDesc kWaterExcluderDesc;
    extern const ComponentDesc kWaterWakeDesc;
    // pixel art (PixelArtReflect.cpp)
    extern const ComponentDesc kPixelPerfectCameraDesc;

}

// find, add, remove and componentId of a component type (designated initializers of ComponentDesc)
#define JE_COMPONENT_ACCESS(T) \
    .find = &::jasperengine::editor::reflect::detail::findComponent<T>, \
    .add = &::jasperengine::editor::reflect::detail::addComponent<T>, \
    .remove = &::jasperengine::editor::reflect::detail::removeComponent<T>, \
    .componentId = &::jasperengine::editor::reflect::detail::componentIdOf<T>

// A component's own whole-component codec: the Stream encode/decode pair it always had
#define JE_STREAM_CODEC(T, encodeFn, decodeFn) \
    ::jasperengine::editor::reflect::ComponentCodec{ \
        [](const void* comp, const ::jasperengine::editor::reflect::EncodeContext&) { \
            return ::jasperengine::editor::Stream::encodeFn(*static_cast<const T*>(comp)); }, \
        [](const YAML::Node& node, void* comp, const ::jasperengine::editor::reflect::DecodeContext& ctx) { \
            *static_cast<T*>(comp) = ::jasperengine::editor::Stream::decodeFn(node, static_cast<const T*>(ctx.old)); } }

// The exported code of a component its own Factory::createX writes (with or without the project path)
#define JE_FACTORY_CODEGEN(fn) \
    [](::jasperengine::EntityRegistry* registry, ::jasperengine::Entity entity, const ::jasperengine::editor::reflect::CodegenContext& ctx) { \
        return ::jasperengine::editor::Factory::fn(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting, \
                                                   ctx.entityVarNames); }
#define JE_FACTORY_CODEGEN_PATH(fn) \
    [](::jasperengine::EntityRegistry* registry, ::jasperengine::Entity entity, const ::jasperengine::editor::reflect::CodegenContext& ctx) { \
        return ::jasperengine::editor::Factory::fn(ctx.indentSpaces, registry, entity, ctx.projectPath, ctx.sceneName, ctx.entityName, \
                                                   ctx.assignExisting, ctx.entityVarNames); }
