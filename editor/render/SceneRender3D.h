// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "physics/PhysicsDebugLines.h"
#include "SceneRender.h"

#include "gizmo/ViewportGizmo.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>
#include "ecs/ComponentFwd.h"
#include "io/watch/FileIdentity.h"

namespace jasperengine{
    class Sprite;
    enum class LightType;
    struct BuoyancyComponent;
    struct CharacterControllerComponent;
    struct ForceFieldComponent;
    struct LightComponent;
    struct LocalVolumetricFogComponent;
    struct ParticleSystemComponent;
    struct PolygonPoint;
    struct ReflectionProbeComponent;
    struct TranslateTracksComponent;
    struct WaterVolumeComponent;
    struct WaterExcluderComponent;
    struct WaterWakeComponent;
    struct WheelColliderComponent;
}

namespace jasperengine::anim{
    class AnimatorOverlayPass;
}

namespace jasperengine::editor{

    struct LightObjects{
        Sprite* icon = nullptr;
        LineRenderer* lines = nullptr;

        LightType type;
        float innerConeCos = 0.0f;
        float outerConeCos = 0.0f;
        Vector3 direction = Vector3::ZERO;
        float range = 0.0f;
        bool spotMaskReady = false;
        float spotMaskAspect = 1.0f;
    };

    struct BodyObjects{
        LineRenderer* lines = nullptr;

        // Mesh and hull shapes walk their whole source buffer to build these, so they
        // are kept until the signature of what feeds them changes.
        std::vector<PhysicsDebugLines::EdgeCache> shapeEdgeCaches;
    };

    class SceneRender3D: public SceneRender{
    private:

        LineRenderer* lines;

        std::map<Entity, LightObjects> lightObjects;
        std::map<Entity, CameraObjects> cameraObjects;
        std::map<Entity, SoundObjects> soundObjects;
        std::map<Entity, BodyObjects> bodyObjects;
        std::map<Entity, LineRenderer*> jointLines;
        std::map<Entity, LineRenderer*> boneLines;
        std::map<Entity, LineRenderer*> linePointLines;
        std::map<Entity, LineRenderer*> polygonPointLines;
        std::map<Entity, LineRenderer*> trackLines;
        std::map<Entity, LineRenderer*> reflectionProbeLines;
        // a selected Local Volumetric Fog's outer shape and its inner (blend distance) one
        std::map<Entity, LineRenderer*> fogVolumeLines;
        std::map<Entity, LineRenderer*> controllerLines;
        std::map<Entity, LineRenderer*> waterVolumeLines;
        std::map<Entity, LineRenderer*> waterExcluderLines;
        std::map<Entity, LineRenderer*> waterWakeLines;
        std::map<Entity, LineRenderer*> buoyancyLines;
        // S96: the Wheel Colliders' circles, travels and (in Play) contacts
        std::map<Entity, LineRenderer*> wheelLines;
        // a selected Particle System's emitter shape, bounds and collision planes; a Force
        // Field's shape (S20 R28: the Particle Effect panel's gizmo toggles)
        std::map<Entity, LineRenderer*> particleLines;
        std::map<Entity, LineRenderer*> forceFieldLines;
        // Gizmos > Animation (S77 R49): one line set per Animator (tools layer), from the overlay pass
        std::map<Entity, LineRenderer*> animatorLines;
        std::unique_ptr<anim::AnimatorOverlayPass> animatorOverlay;
        // a highlighted Capsule Shadow Caster's capsules, with the selected one's handles
        std::map<Entity, LineRenderer*> capsuleShadowLines;

        ViewportGizmo viewgizmo;

        float walkSpeedOffset = 0.0f;

        Vector2 linesOffset;
        int gridPlane = 0;
        float lastGridSpacing;
        float lastGridFarClip = 0.0f;

        void createLines();
        bool instanciateLightObject(Entity entity);
        bool instanciateCameraObject(Entity entity);
        bool instanciateSoundObject(Entity entity);
        bool instanciateBodyObject(Entity entity);
        bool instanciateBoneLines(Entity entity);
        void createOrUpdateBoneLines(Entity entity, const ModelComponent& model, bool visible, bool highlighted);
        void createOrUpdateLightIcon(Entity entity, const Transform& transform, LightType lightType, bool newLight);
        void createOrUpdateCameraIcon(Entity entity, const Transform& transform, bool newCamera);
        void createOrUpdateSoundIcon(Entity entity, const Transform& transform, bool newSound);
        void createOrUpdateBodyLines(Entity entity, const Transform& transform, const RigidbodyComponent& body, bool visible, bool highlighted);
        void createOrUpdateJointLines(Entity entity, const Joint3DComponent& joint, bool visible, bool highlighted);
        void createOrUpdateLinePointLines(Entity entity, const Transform& transform, const LineRendererComponent& lines, bool visible);
        void createOrUpdatePolygonPointLines(Entity entity, const Transform& transform, const std::vector<PolygonPoint>& points, bool isMesh, bool visible);
        bool instanciateTrackLines(Entity entity);
        void createOrUpdateTrackLines(Entity entity, const TranslateTracksComponent& tracks, bool visible);
        void createOrUpdateReflectionProbeLines(Entity entity, const Transform& transform, const ReflectionProbeComponent& probe, bool visible);
        void createOrUpdateFogVolumeLines(Entity entity, const Transform& transform, const LocalVolumetricFogComponent& volume, bool visible);
        void createOrUpdateWaterVolumeLines(Entity entity, const Transform& transform, const WaterVolumeComponent& volume, bool visible, bool highlighted);
        void createOrUpdateWaterExcluderLines(Entity entity, const Transform& transform, const WaterExcluderComponent& excluder, bool visible, bool highlighted);
        void createOrUpdateWaterWakeLines(Entity entity, const Transform& transform, const WaterWakeComponent& wake, bool visible);
        void createOrUpdateBuoyancyLines(Entity entity, const Transform& transform, const BuoyancyComponent& buoyancy, bool visible, bool highlighted);
        void createOrUpdateWheelLines(Entity entity, const Transform& transform, const WheelColliderComponent& wheel, bool visible, bool highlighted);
        void createOrUpdateControllerLines(Entity entity, const Transform& transform, const CharacterControllerComponent& controller, bool visible, bool highlighted);
        void createOrUpdateCapsuleShadowLines(Entity entity, const Transform& transform, const CapsuleShadowCasterComponent& caster, bool visible, bool selected);
        void createOrUpdateParticleLines(Entity entity, const Transform& transform, const ParticleSystemComponent& particles, bool visible);
        void createOrUpdateForceFieldLines(Entity entity, const Transform& transform, const ForceFieldComponent& field, bool visible);
        void createCameraFrustum(Entity entity, const Transform& transform, const CameraComponent& cameraComponent, bool fullFrustum, bool isMainCamera);
        void createDirectionalLightArrow(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected);
        void createPointLightSphere(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected);
        void createSpotLightCones(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected);

    protected:
        void hideAllGizmos() override;
        void hideEntityGizmos(Entity entity) override;
        void dropGoneGizmos() override;
        using SceneRender::dropGoneGizmos;
        void drawOverlayTargets() override;

    public:
        SceneRender3D(Scene* scene);
        virtual ~SceneRender3D();

        void activate() override;
        void setOverlayScale(float scale) override;
        void updateSelLines(std::vector<OBB> obbs) override;

        void zoomCamera(float amount);

        float getWalkSpeedOffset() const;
        void setWalkSpeedOffset(float offset);

        void update(std::vector<Entity> selEntities, std::vector<Entity> entities, Entity mainCamera, const SceneDisplaySettings& settings = SceneDisplaySettings{}) override;

        ViewportGizmo* getViewportGizmo();

        // 0 the XZ plane (perspective and top/bottom views), 1 XY (front/back), 2 YZ (sides)
        void setGridPlane(int plane) override;
        int getGridPlane() const override { return gridPlane; }
    };

}
