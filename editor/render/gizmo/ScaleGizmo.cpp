// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScaleGizmo.h"
#include <cfloat>

using namespace jasperengine;

const Vector3 editor::ScaleGizmo::centerColor = Vector3(0.8, 0.8, 0.8);
const Vector3 editor::ScaleGizmo::xaxisColor = Vector3(0.7, 0.2, 0.2);
const Vector3 editor::ScaleGizmo::yaxisColor = Vector3(0.2, 0.7, 0.2);
const Vector3 editor::ScaleGizmo::zaxisColor = Vector3(0.2, 0.2, 0.7);
const Vector3 editor::ScaleGizmo::centerColorHightlight = Vector3(0.9, 0.9, 0.9);
const Vector3 editor::ScaleGizmo::xaxisColorHightlight = Vector3(0.9, 0.7, 0.7);
const Vector3 editor::ScaleGizmo::yaxisColorHightlight = Vector3(0.7, 0.9, 0.7);
const Vector3 editor::ScaleGizmo::zaxisColorHightlight = Vector3(0.7, 0.7, 0.9);
const float editor::ScaleGizmo::rectAlpha = 0.6;

editor::ScaleGizmo::ScaleGizmo(Scene* scene, bool use2DGizmo): GameObject(scene){
    float cylinderRadius = 0.05;
    float cylinderHeight = 2;
    float centerSize = 0.3;
    float cubeSize = 0.2;

    this->use2DGizmo = use2DGizmo;

    centerbox = new Primitive(scene);
    xaxis = new Primitive(scene);
    xbox = new Primitive(scene);

    yaxis = new Primitive(scene);
    ybox = new Primitive(scene);

    if (!use2DGizmo){
        zaxis = new Primitive(scene);
        zbox = new Primitive(scene);
        xyrect = new Primitive(scene);
        xzrect = new Primitive(scene);
        yzrect = new Primitive(scene);
    }
    
    centerbox->createBox(centerSize, centerSize, centerSize);
    centerbox->setColor(paint(centerColor, 1.0));

    xaxis->createCylinder(cylinderRadius, cylinderHeight);
    xaxis->setColor(paint(xaxisColor, 1.0));
    xaxis->setPosition(cylinderHeight/2.0, 0, 0);
    xaxis->setRotation(0,0,90);

    yaxis->createCylinder(cylinderRadius, cylinderHeight);
    yaxis->setColor(paint(yaxisColor, 1.0));
    yaxis->setPosition(0, cylinderHeight/2.0, 0);

    xbox->createBox(cubeSize, cubeSize, cubeSize);
    xbox->setPosition(cylinderHeight, 0, 0);
    xbox->setColor(paint(xaxisColor, 1.0));

    ybox->createBox(cubeSize, cubeSize, cubeSize);
    ybox->setPosition(0, cylinderHeight, 0);
    ybox->setColor(paint(yaxisColor, 1.0));

    if (!use2DGizmo) {
        zaxis->createCylinder(cylinderRadius, cylinderHeight);
        zaxis->setColor(paint(zaxisColor, 1.0));
        zaxis->setPosition(0, 0, cylinderHeight/2.0);
        zaxis->setRotation(90,0,0);

        zbox->createBox(cubeSize, cubeSize, cubeSize);
        zbox->setPosition(0, 0, cylinderHeight);
        zbox->setColor(paint(zaxisColor, 1.0));

        xyrect->createBox(cylinderHeight/4.0, cylinderHeight/4.0, cylinderRadius);
        xzrect->createBox(cylinderHeight/4.0, cylinderRadius, cylinderHeight/4.0);
        yzrect->createBox(cylinderRadius, cylinderHeight/4.0, cylinderHeight/4.0);

        xyrect->setPosition(cylinderHeight/3.0, cylinderHeight/3.0, 0);
        xzrect->setPosition(cylinderHeight/3.0, 0, cylinderHeight/3.0);
        yzrect->setPosition(0, cylinderHeight/3.0, cylinderHeight/3.0);

        xyrect->setColor(paint(zaxisColor, rectAlpha));
        xzrect->setColor(paint(yaxisColor, rectAlpha));
        yzrect->setColor(paint(xaxisColor, rectAlpha));
    }

    this->addChild(centerbox);
    this->addChild(xaxis);
    this->addChild(xbox);
    this->addChild(yaxis);
    this->addChild(ybox);

    if (!use2DGizmo) {
        this->addChild(zaxis);
        this->addChild(zbox);
        this->addChild(xyrect);
        this->addChild(xzrect);
        this->addChild(yzrect);
    }
}

editor::ScaleGizmo::~ScaleGizmo(){
    delete centerbox;
    delete xaxis;
    delete xbox;
    delete yaxis;
    delete ybox;

    if (!use2DGizmo) {
        delete zaxis;
        delete zbox;
        delete xyrect;
        delete xzrect;
        delete yzrect;
    }
}

editor::GizmoSideSelected editor::ScaleGizmo::checkHover(const Ray& ray){

    editor::GizmoSideSelected gizmoSideSelected = GizmoSideSelected::NONE;

    AABB xaabb = xaxis->getModelMatrix() * Matrix4::scaleMatrix(Vector3(2, 1, 2)) * xaxis->getAABB().merge(xbox->getAABB());
    AABB yaabb = yaxis->getModelMatrix() * Matrix4::scaleMatrix(Vector3(2, 1, 2)) * yaxis->getAABB().merge(ybox->getAABB());
    AABB boxaabb = centerbox->getModelMatrix() * Matrix4::scaleMatrix(Vector3(1.5, 1.5, 1.5)) * centerbox->getAABB();
    AABB superboxaabb = centerbox->getModelMatrix() * Matrix4::scaleMatrix(Vector3(10, 10, 10)) * centerbox->getAABB();

    RayReturn rreturn[7] = {0};

    rreturn[0] = ray.intersects(xaabb);
    rreturn[1] = ray.intersects(yaabb);
    rreturn[6] = ray.intersects(boxaabb);

    if (!use2DGizmo) {
        AABB zaabb = zaxis->getModelMatrix() * Matrix4::scaleMatrix(Vector3(2, 1, 2)) * zaxis->getAABB().merge(zbox->getAABB());
        AABB xyaabb = xyrect->getWorldAABB();
        AABB xzaabb = xzrect->getWorldAABB();
        AABB yzaabb = yzrect->getWorldAABB();

        rreturn[2] = ray.intersects(zaabb);
        rreturn[3] = ray.intersects(xyaabb);
        rreturn[4] = ray.intersects(xzaabb);
        rreturn[5] = ray.intersects(yzaabb);
    }

    int axis = -1;
    float minDist = FLT_MAX;

    for (int i = 0; i < 7; i++){
        // Skip indices 2-5 for 2D gizmo
        if (use2DGizmo && (i >= 2 && i <= 5))
            continue;

        if (rreturn[i]){
            if (rreturn[i].distance <= minDist || (i >= 3 && axis <= 2)){
                minDist = rreturn[i].distance;
                axis = i;
            }
        }
    }

    if (axis == -1){
        if (RayReturn creturn = ray.intersects(superboxaabb)){
            axis = 6;
        }
    }

    if (axis == 0){
        xaxis->setColor(paint(xaxisColorHightlight, 1.0));
        xbox->setColor(paint(xaxisColorHightlight, 1.0));
        gizmoSideSelected = GizmoSideSelected::X;
    }else{
        xaxis->setColor(paint(xaxisColor, 1.0));
        xbox->setColor(paint(xaxisColor, 1.0));
    }

    if (axis == 1){
        yaxis->setColor(paint(yaxisColorHightlight, 1.0));
        ybox->setColor(paint(yaxisColorHightlight, 1.0));
        gizmoSideSelected = GizmoSideSelected::Y;
    }else{
        yaxis->setColor(paint(yaxisColor, 1.0));
        ybox->setColor(paint(yaxisColor, 1.0));
    }

    if (!use2DGizmo) {
        if (axis == 2){
            zaxis->setColor(paint(zaxisColorHightlight, 1.0));
            zbox->setColor(paint(zaxisColorHightlight, 1.0));
            gizmoSideSelected = GizmoSideSelected::Z;
        }else{
            zaxis->setColor(paint(zaxisColor, 1.0));
            zbox->setColor(paint(zaxisColor, 1.0));
        }

        if (axis == 3){
            xyrect->setColor(paint(zaxisColorHightlight, rectAlpha));
            gizmoSideSelected = GizmoSideSelected::XY;
        }else{
            xyrect->setColor(paint(zaxisColor, rectAlpha));
        }

        if (axis == 4){
            xzrect->setColor(paint(yaxisColorHightlight, rectAlpha));
            gizmoSideSelected = GizmoSideSelected::XZ;
        }else{
            xzrect->setColor(paint(yaxisColor, rectAlpha));
        }

        if (axis == 5){
            yzrect->setColor(paint(xaxisColorHightlight, rectAlpha));
            gizmoSideSelected = GizmoSideSelected::YZ;
        }else{
            yzrect->setColor(paint(xaxisColor, rectAlpha));
        }
    }

    if (axis == 6){
        centerbox->setColor(paint(centerColorHightlight, 1.0));
        gizmoSideSelected = use2DGizmo ? GizmoSideSelected::XY : GizmoSideSelected::XYZ;
    }else{
        centerbox->setColor(paint(centerColor, 1.0));
    }

    return gizmoSideSelected;
}
