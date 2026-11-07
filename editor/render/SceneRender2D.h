// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "SceneRender.h"

#include "UILayer.h"
#include "command/Command.h"
#include "io/watch/FileIdentity.h"

namespace jasperengine{
    struct CharacterController2DComponent;
    struct Joint2DComponent;
    struct Light2DComponent;
    struct OnScreenStickComponent;
    struct PolygonPoint;
    struct Rigidbody2DComponent;
    struct ShadowCaster2DComponent;
    struct TranslateTracksComponent;
}

namespace jasperengine::editor{

    class SceneRender2D: public SceneRender{
    private:
        LineRenderer* lines;
        LineRenderer* gridLines;
        // the component UI's navigation arrows (a Button's Navigation > Visualize)
        LineRenderer* navigationLines;
        std::map<Entity, LineRenderer*> containerLines;
        std::map<Entity, LineRenderer*> bodyLines;
        std::map<Entity, LineRenderer*> jointLines;
        std::map<Entity, LineRenderer*> controllerLines;
        std::map<Entity, LineRenderer*> light2DLines;
        std::map<Entity, LineRenderer*> shadowCaster2DLines;
        std::map<Entity, LineRenderer*> linePointLines;
        std::map<Entity, LineRenderer*> polygonPointLines;
        std::map<Entity, LineRenderer*> trackLines;
        std::map<Entity, LineRenderer*> stickLines;   // On-Screen Stick: handle range and dead zone
        std::map<Entity, CameraObjects> cameraObjects;
        std::map<Entity, SoundObjects> soundObjects;
        std::map<Entity, Light2DObjects> light2DObjects;
        LineRenderer* tileLines = nullptr;
        // the canvas rectangle, outlined while a UI Document is selected (its documents fill it)
        LineRenderer* documentLines = nullptr;
        unsigned int canvasFrameWidth = 0;
        unsigned int canvasFrameHeight = 0;
        bool isUI;
        int viewportWidth;
        int viewportHeight;

        void createLines(unsigned int width, unsigned int height);
        void applyZoomProjection();
        void updateGridLines();
        void updateNavigationLines(const std::vector<Entity>& entities);
        void addPointHandle(LineRenderer* linesObj, const Vector3& worldPoint, float halfSize, const Vector4& color);
        void createOrUpdateBodyLines(Entity entity, const Transform& transform, Rigidbody2DComponent& body, bool visible, bool highlighted);
        void createOrUpdateJointLines(Entity entity, const Joint2DComponent& joint, bool visible, bool highlighted);
        void createOrUpdateController2DLines(Entity entity, const Transform& transform, const CharacterController2DComponent& controller, bool visible, bool highlighted);
        void createOrUpdateLight2DLines(Entity entity, const Transform& transform, const Light2DComponent& light, bool visible, bool highlighted);
        void createOrUpdateStickLines(Entity entity, const OnScreenStickComponent& stick, bool visible);
        void createOrUpdateShadowCaster2DLines(Entity entity, const Transform& transform, const ShadowCaster2DComponent& occluder, bool visible, bool highlighted);
        void createOrUpdateLinePointLines(Entity entity, const Transform& transform, const LineRendererComponent& lines, bool visible);
        void createOrUpdatePolygonPointLines(Entity entity, const Transform& transform, const std::vector<PolygonPoint>& points, bool isMesh, bool visible);
        bool instanciateTrackLines(Entity entity);
        void createOrUpdateTrackLines(Entity entity, const TranslateTracksComponent& tracks, bool visible);

    protected:
        void hideAllGizmos() override;
        void hideEntityGizmos(Entity entity) override;
        void dropGoneGizmos() override;
        using SceneRender::dropGoneGizmos;

    public:
        SceneRender2D(Scene* scene, unsigned int width, unsigned int height, bool isUI);
        virtual ~SceneRender2D();

        void activate() override;
        void setCanvasFrameSize(unsigned int width, unsigned int height);
        void updateSize(int width, int height) override;
        void updateSelLines(std::vector<OBB> obbs) override;

        void update(std::vector<Entity> selEntities, std::vector<Entity> entities, Entity mainCamera, const SceneDisplaySettings& settings = SceneDisplaySettings{}) override;

        void zoomAtPosition(float width, float height, Vector2 pos, float zoomFactor);

        float getZoom() const;
        void setZoom(float zoom);
    };

}