// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "SceneRender.h"

#include "JasperEngine.h"
#include "gizmo/ViewportGizmo.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

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
        struct ShapeEdgeCache {
            bool valid = false;
            uint64_t signature = 0;
            std::vector<std::pair<Vector3, Vector3>> edges; // body-local space
        };
        std::vector<ShapeEdgeCache> shapeEdgeCaches;
    };

    class SceneRender3D: public SceneRender{
    private:

        static constexpr float kHullQuantizeScale = 4096.0f;
        // Above this the cloud is reduced to its extremes first. Matches
        // ConvexHullShape::cMaxPointsInHull, the cap Jolt applies to the same shape.
        static constexpr size_t kMaxHullInputPoints = 256;
        static constexpr size_t kMaxSourceParentHops = 64;

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
        // a selected Particle System's emitter shape, bounds and collision planes; a Force
        // Field's shape (S20 R28: the Particle Effect panel's gizmo toggles)
        std::map<Entity, LineRenderer*> particleLines;
        std::map<Entity, LineRenderer*> forceFieldLines;

        ViewportGizmo viewgizmo;

        float walkSpeedOffset = 0.0f;

        Vector2 linesOffset;
        float lastGridSpacing;
        float lastGridFarClip = 0.0f;

        void createLines();
        bool instanciateLightObject(Entity entity);
        bool instanciateCameraObject(Entity entity);
        bool instanciateSoundObject(Entity entity);
        bool instanciateBodyObject(Entity entity);
        bool instanciateBoneLines(Entity entity);
        static uint64_t quantizeHullKey(const Vector3& p);
        static void pushUniqueHullPoint(std::vector<Vector3>& points, std::unordered_set<uint64_t>& seen, const Vector3& p);
        static void reduceToExtremes(std::vector<Vector3>& points);
        static void buildConvexHullEdges(std::vector<Vector3> points, const std::function<void(const Vector3&, const Vector3&)>& emit);
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
        void createOrUpdateParticleLines(Entity entity, const Transform& transform, const ParticleSystemComponent& particles, bool visible);
        void createOrUpdateForceFieldLines(Entity entity, const Transform& transform, const ForceFieldComponent& field, bool visible);
        void createCameraFrustum(Entity entity, const Transform& transform, const CameraComponent& cameraComponent, bool fixedSizeFrustum, bool isMainCamera);
        void createDirectionalLightArrow(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected);
        void createPointLightSphere(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected);
        void createSpotLightCones(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected);

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
    };

}
