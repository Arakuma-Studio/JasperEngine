// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Primitive.h"
#include "object/ui/Image.h"

#include "RenderUtil.h"
#include "gizmo/TranslateGizmo.h"
#include "gizmo/RotateGizmo.h"
#include "gizmo/ScaleGizmo.h"
#include "gizmo/Object2DGizmo.h"
#include "gizmo/AnchorGizmo.h"

namespace jasperengine::editor{

    class ToolsLayer{
    private:
        Scene* scene;
        Camera* camera;

        TranslateGizmo* tGizmo;
        RotateGizmo* rGizmo;
        ScaleGizmo* sGizmo;
        Object2DGizmo* oGizmo;
        AnchorGizmo* aGizmo;

        GizmoSelected gizmoSelected;
        GizmoSideSelected gizmoSideSelected;
        Gizmo2DSideSelected gizmo2DSideSelected;

        bool anchorGizmoEnabled;
        bool gizmoVisible;

    public:
        ToolsLayer(bool use2DGizmos);
        virtual ~ToolsLayer();

        void updateCamera(CameraComponent& extCamera, Transform& extCameraTransform);
        // Sees through a layered scene's camera as the view shows it (see LayerView)
        void updateCamera(const LayerView& layerView);
        void updateGizmo(Camera* sceneCam, Vector3& position, Quaternion& rotation, float scale, OBB obb, Ray& mouseRay, bool mouseClicked, Vector4& anchorData, Rect& anchorArea);

        void mouseDrag(Vector3 point);
        void mouseRelease();

        void enableTranslateGizmo();
        void enableRotateGizmo();
        void enableScaleGizmo();
        void enableObject2DGizmo();
        // Same tool as another tools layer: the one the toolbar sets
        void setGizmoSelected(GizmoSelected gizmoSelected);

        void setShowAnchorGizmo(bool enabled);
        void setShowObject2DRects(bool show);
        void setShowObject2DCross(bool show);

        void setGizmoVisible(bool visible);
        bool isGizmoVisible() const;
        // Move/Rotate/Scale in the Physics orange while Physics (G) is on
        void setPhysicsTint(bool tint);

        Scene* getScene();

        GameObject* getGizmoObject() const;
        Vector3 getGizmoPosition() const;
        Quaternion getGizmoRotation() const;

        GizmoSelected getGizmoSelected() const;
        GizmoSideSelected getGizmoSideSelected() const;
        Gizmo2DSideSelected getGizmo2DSideSelected() const;
    };

}
