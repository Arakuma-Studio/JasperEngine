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

    struct BodyObjects{
        // Mesh and hull shapes walk their whole source buffer to build these, so they
        // are kept until the signature of what feeds them changes.
        std::vector<PhysicsDebugLines::EdgeCache> shapeEdgeCaches;
    };

    class SceneRender3D: public SceneRender{
    private:

        LineRenderer* lines;

        // one batch per gizmo kind for every GameObject (S98 REQ-30): in this scene (depth
        // tested) unless made for the tools layer (drawn over the meshes they run inside)
        GizmoIconBatch icons{true};
        GizmoLineBatch lightLines;
        GizmoLineBatch cameraLines;
        GizmoLineBatch bodyLines;
        std::map<Entity, BodyObjects> bodyObjects;
        GizmoLineBatch jointLines;
        GizmoLineBatch boneLines{true};
        GizmoLineBatch linePointLines;
        GizmoLineBatch polygonPointLines;
        GizmoLineBatch trackLines{true};
        GizmoLineBatch reflectionProbeLines;
        // a selected Local Volumetric Fog's outer shape and its inner (blend distance) one
        GizmoLineBatch fogVolumeLines;
        GizmoLineBatch controllerLines;
        GizmoLineBatch waterVolumeLines;
        GizmoLineBatch waterExcluderLines;
        GizmoLineBatch waterWakeLines;
        GizmoLineBatch buoyancyLines;
        // S96: the Wheel Colliders' circles, travels and (in Play) contacts
        GizmoLineBatch wheelLines;
        // a selected Particle System's emitter shape, bounds and collision planes; a Force
        // Field's shape (S20 R28: the Particle Effect panel's gizmo toggles)
        GizmoLineBatch particleLines;
        GizmoLineBatch forceFieldLines;
        // Gizmos > Animation (S77 R49): every Animator's lines (tools layer), from the overlay pass
        GizmoLineBatch animatorLines{true};
        std::unique_ptr<anim::AnimatorOverlayPass> animatorOverlay;
        // a highlighted Capsule Shadow Caster's capsules, with the selected one's handles
        GizmoLineBatch capsuleShadowLines;

        // every batch, for the passes that treat them alike (begin, flush, hide, sweep)
        std::vector<GizmoLineBatch*> lineBatches();

        ViewportGizmo viewgizmo;

        float walkSpeedOffset = 0.0f;

        Vector2 linesOffset;
        int gridPlane = 0;
        float lastGridSpacing;
        float lastGridFarClip = 0.0f;

        void createLines();
        void createOrUpdateBoneLines(Entity entity, const ModelComponent& model, bool visible, bool highlighted);
        void createOrUpdateBodyLines(Entity entity, const Transform& transform, const RigidbodyComponent& body, bool visible, bool highlighted);
        void createOrUpdateJointLines(Entity entity, const Joint3DComponent& joint, bool visible, bool highlighted);
        void createOrUpdateLinePointLines(Entity entity, const Transform& transform, const LineRendererComponent& lines, bool visible);
        void createOrUpdatePolygonPointLines(Entity entity, const Transform& transform, const std::vector<PolygonPoint>& points, bool isMesh, bool visible);
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
        void createDirectionalLightArrow(const Transform& transform, const LightComponent& light);
        void createPointLightSphere(const Transform& transform, const LightComponent& light);
        void createSpotLightCones(const Transform& transform, const LightComponent& light);

    protected:
        void hideAllGizmos() override;
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
