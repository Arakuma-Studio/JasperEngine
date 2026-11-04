// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ToolsLayer.h"
#include "component/Transform.h"
#include "component/CameraComponent.h"

using namespace jasperengine;

editor::ToolsLayer::ToolsLayer(bool use2DGizmos){
    if (use2DGizmos){
        gizmoSelected = GizmoSelected::OBJECT2D;
    }else{
        gizmoSelected = GizmoSelected::TRANSLATE;
    }
    gizmoSideSelected = GizmoSideSelected::NONE;
    gizmo2DSideSelected = Gizmo2DSideSelected::NONE;

    scene = new Scene(EntityPool::System);
    camera = new Camera(scene);

    tGizmo = new TranslateGizmo(scene, use2DGizmos);
    rGizmo = new RotateGizmo(scene, use2DGizmos);
    sGizmo = new ScaleGizmo(scene, use2DGizmos);
    oGizmo = new Object2DGizmo(scene);
    aGizmo = new AnchorGizmo(scene);

    anchorGizmoEnabled = false;
    gizmoVisible = false;

    scene->setCamera(camera);
}

editor::ToolsLayer::~ToolsLayer(){
    delete camera;

    delete tGizmo;
    delete rGizmo;
    delete sGizmo;
    delete oGizmo;
    delete aGizmo;

    delete scene;
}

void editor::ToolsLayer::updateCamera(CameraComponent& extCamera, Transform& extCameraTransform){
    Entity entity = camera->getEntity();
    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(entity);

    camera->setPosition(extCameraTransform.position);
    camera->setTarget(extCamera.target);

    cameracomp.type = extCamera.type;
    cameracomp.leftClip = extCamera.leftClip;
    cameracomp.rightClip = extCamera.rightClip;
    cameracomp.bottomClip = extCamera.bottomClip;
    cameracomp.topClip = extCamera.topClip;
    cameracomp.nearClip = extCamera.nearClip;
    cameracomp.farClip = extCamera.farClip;
    cameracomp.yfov = extCamera.yfov;
    cameracomp.aspect = extCamera.aspect;
    cameracomp.useTarget = extCamera.useTarget;
    cameracomp.autoResize = extCamera.autoResize;
    if (extCamera.needUpdate){
        cameracomp.needUpdate = extCamera.needUpdate;
    }
}

void editor::ToolsLayer::updateCamera(const LayerView& layerView){
    Entity entity = camera->getEntity();
    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(entity);
    Transform& cameratransform = scene->getComponent<Transform>(entity);

    // the layer's own view matrix, so a camera turned by its rotation or a parent is seen
    // exactly; its projection covers the whole view (autoResize would refit it to the canvas)
    const bool changed = cameracomp.type != layerView.type
        || cameracomp.leftClip != layerView.leftClip || cameracomp.rightClip != layerView.rightClip
        || cameracomp.bottomClip != layerView.bottomClip || cameracomp.topClip != layerView.topClip
        || cameracomp.nearClip != layerView.nearClip || cameracomp.farClip != layerView.farClip
        || cameracomp.yfov != layerView.yfov || cameracomp.aspect != layerView.aspect
        || cameracomp.autoResize || !cameracomp.hasCustomViewMatrix
        || cameracomp.customViewMatrix != layerView.viewMatrix
        || cameratransform.position != layerView.worldPosition;

    if (!changed){
        return;
    }

    cameracomp.type = layerView.type;
    cameracomp.leftClip = layerView.leftClip;
    cameracomp.rightClip = layerView.rightClip;
    cameracomp.bottomClip = layerView.bottomClip;
    cameracomp.topClip = layerView.topClip;
    cameracomp.nearClip = layerView.nearClip;
    cameracomp.farClip = layerView.farClip;
    cameracomp.yfov = layerView.yfov;
    cameracomp.aspect = layerView.aspect;
    cameracomp.autoResize = false;
    cameracomp.useTarget = false;
    cameracomp.hasCustomViewMatrix = true;
    cameracomp.customViewMatrix = layerView.viewMatrix;
    cameracomp.needUpdate = true;

    camera->setPosition(layerView.worldPosition);
}

void editor::ToolsLayer::updateGizmo(Camera* sceneCam, Vector3& position, Quaternion& rotation, float scale, OBB obb, Ray& mouseRay, bool mouseClicked, Vector4& anchorData, Rect& anchorArea){
    if (gizmoSelected == GizmoSelected::TRANSLATE){
        tGizmo->setPosition(position);
        tGizmo->setRotation(rotation);
        tGizmo->setScale(scale);
        if (!mouseClicked){
            gizmoSideSelected = tGizmo->checkHover(mouseRay);
            gizmo2DSideSelected = Gizmo2DSideSelected::NONE;
        }
    }
    if (gizmoSelected == GizmoSelected::ROTATE){
        rGizmo->updateRotations(camera);
        rGizmo->setPosition(position);
        rGizmo->setRotation(rotation);
        rGizmo->setScale(scale);
        if (!mouseClicked){
            gizmoSideSelected = rGizmo->checkHover(mouseRay);
            gizmo2DSideSelected = Gizmo2DSideSelected::NONE;
        }
    }
    if (gizmoSelected == GizmoSelected::SCALE){
        sGizmo->setPosition(position);
        sGizmo->setRotation(rotation);
        sGizmo->setScale(scale);
        if (!mouseClicked){
            gizmoSideSelected = sGizmo->checkHover(mouseRay);
            gizmo2DSideSelected = Gizmo2DSideSelected::NONE;
        }
    }
    // only for single selections
    // do not use gizmo position and rotation
    if (gizmoSelected == GizmoSelected::OBJECT2D){
        oGizmo->setPosition(position);
        oGizmo->setRotation(rotation);
        oGizmo->setScale(scale);

        Vector3 center = obb.getCenter();
        Vector3 size = obb.getHalfExtents() * 2.0f;
        oGizmo->setCenter(rotation.getRotationMatrix().inverse() * (center - position) / scale);
        oGizmo->setSize(size.x / scale, size.y / scale);

        if (anchorGizmoEnabled){
            aGizmo->setScale(scale);

            aGizmo->setArea(Rect(anchorArea.getX() / scale, anchorArea.getY() / scale, anchorArea.getWidth() / scale, anchorArea.getHeight() / scale));
            aGizmo->setAnchors(anchorData.x, anchorData.y, anchorData.z, anchorData.w);
        }

        if (!mouseClicked){
            gizmoSideSelected = GizmoSideSelected::NONE;
            gizmo2DSideSelected = oGizmo->checkHover(mouseRay, obb);
        }
    }
}

void editor::ToolsLayer::mouseDrag(Vector3 point){
    if (gizmoSelected == GizmoSelected::ROTATE && gizmoSideSelected != GizmoSideSelected::NONE){
        rGizmo->drawLine(point);
    }
}

void editor::ToolsLayer::mouseRelease(){
    if (gizmoSelected == GizmoSelected::ROTATE){
        rGizmo->removeLine();
    }
}

void editor::ToolsLayer::enableTranslateGizmo(){
     gizmoSelected = GizmoSelected::TRANSLATE;
}

void editor::ToolsLayer::enableRotateGizmo(){
    gizmoSelected = GizmoSelected::ROTATE;
}

void editor::ToolsLayer::enableScaleGizmo(){
    gizmoSelected = GizmoSelected::SCALE;
}

void editor::ToolsLayer::enableObject2DGizmo(){
    gizmoSelected = GizmoSelected::OBJECT2D;
}

void editor::ToolsLayer::setGizmoSelected(GizmoSelected gizmoSelected){
    this->gizmoSelected = gizmoSelected;
}

void editor::ToolsLayer::setShowAnchorGizmo(bool enabled){
    anchorGizmoEnabled = enabled;
}

void editor::ToolsLayer::setShowObject2DRects(bool show){
    oGizmo->setShowRects(show);
}

void editor::ToolsLayer::setShowObject2DCross(bool show){
    oGizmo->setCrossVisible(show);
}

void editor::ToolsLayer::setGizmoVisible(bool visible){
    tGizmo->setVisible(false);
    rGizmo->setVisible(false);
    sGizmo->setVisible(false);
    oGizmo->setVisible(false);
    aGizmo->setVisible(false);

    if (gizmoSelected == GizmoSelected::TRANSLATE){
        tGizmo->setVisible(visible);
    }
    if (gizmoSelected == GizmoSelected::ROTATE){
        rGizmo->setVisible(visible);
    }
    if (gizmoSelected == GizmoSelected::SCALE){
        sGizmo->setVisible(visible);
    }
    if (gizmoSelected == GizmoSelected::OBJECT2D){
        oGizmo->setVisible(visible);
        if (anchorGizmoEnabled){
            aGizmo->setVisible(visible);
        }
    }

    gizmoVisible = visible;

    if (!visible){
        gizmoSideSelected = GizmoSideSelected::NONE;
        gizmo2DSideSelected = Gizmo2DSideSelected::NONE;
    }
}

bool editor::ToolsLayer::isGizmoVisible() const{
    return gizmoVisible;
}

void editor::ToolsLayer::setPhysicsTint(bool tint){
    tGizmo->setPhysicsTint(tint);
    rGizmo->setPhysicsTint(tint);
    sGizmo->setPhysicsTint(tint);
}

Scene* editor::ToolsLayer::getScene(){
    return scene;
}

GameObject* editor::ToolsLayer::getGizmoObject() const{
    if (gizmoSelected == GizmoSelected::TRANSLATE){
        return tGizmo;
    }
    if (gizmoSelected == GizmoSelected::ROTATE){
        return rGizmo;
    }
    if (gizmoSelected == GizmoSelected::SCALE){
        return sGizmo;
    }
    if (gizmoSelected == GizmoSelected::OBJECT2D){
        return oGizmo;
    }

    return nullptr;
}

Vector3 editor::ToolsLayer::getGizmoPosition() const{
    return getGizmoObject()->getPosition();
}

Quaternion editor::ToolsLayer::getGizmoRotation() const{
    return getGizmoObject()->getRotation();
}

editor::GizmoSelected editor::ToolsLayer::getGizmoSelected() const{
    return gizmoSelected;
}

editor::GizmoSideSelected editor::ToolsLayer::getGizmoSideSelected() const{
    return gizmoSideSelected;
}

editor::Gizmo2DSideSelected editor::ToolsLayer::getGizmo2DSideSelected() const{
    return gizmo2DSideSelected;
}
