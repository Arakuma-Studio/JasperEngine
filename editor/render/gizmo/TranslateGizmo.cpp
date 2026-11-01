// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TranslateGizmo.h"
#include <cfloat>

using namespace jasperengine;

const Vector3 editor::TranslateGizmo::centerColor = Vector3(0.8, 0.8, 0.8);
const Vector3 editor::TranslateGizmo::xaxisColor = Vector3(0.7, 0.2, 0.2);
const Vector3 editor::TranslateGizmo::yaxisColor = Vector3(0.2, 0.7, 0.2);
const Vector3 editor::TranslateGizmo::zaxisColor = Vector3(0.2, 0.2, 0.7);
const Vector3 editor::TranslateGizmo::centerColorHightlight = Vector3(0.9, 0.9, 0.9);
const Vector3 editor::TranslateGizmo::xaxisColorHightlight = Vector3(0.9, 0.7, 0.7);
const Vector3 editor::TranslateGizmo::yaxisColorHightlight = Vector3(0.7, 0.9, 0.7);
const Vector3 editor::TranslateGizmo::zaxisColorHightlight = Vector3(0.7, 0.7, 0.9);
const float editor::TranslateGizmo::rectAlpha = 0.6;

editor::TranslateGizmo::TranslateGizmo(Scene* scene, bool use2DGizmo): GameObject(scene){
    float cylinderRadius = 0.05;
    float cylinderHeight = 2;
    float centerSize = 0.2;
    float arrowRadius = 0.1;
    float arrowHeight = 0.4;

    this->use2DGizmo = use2DGizmo;

    sphere = new Primitive(scene);

    xaxis = new Primitive(scene);
    xarrow = new Primitive(scene);

    yaxis = new Primitive(scene);
    yarrow = new Primitive(scene);

    if (!use2DGizmo){
        zaxis = new Primitive(scene);
        zarrow = new Primitive(scene);

        xyrect = new Primitive(scene);
        yzrect = new Primitive(scene);
        xzrect = new Primitive(scene);
    }
    
    sphere->createSphere(centerSize);
    sphere->setColor(paint(centerColor, 1.0));

    xaxis->createCylinder(cylinderRadius, cylinderHeight);
    xaxis->setColor(paint(xaxisColor, 1.0));
    xaxis->setPosition(cylinderHeight/2.0, 0, 0);
    xaxis->setRotation(0,0,90);

    yaxis->createCylinder(cylinderRadius, cylinderHeight);
    yaxis->setColor(paint(yaxisColor, 1.0));
    yaxis->setPosition(0, cylinderHeight/2.0, 0);

    xarrow->createCylinder(arrowRadius, 0.0, arrowHeight);
    xarrow->setPosition(cylinderHeight, 0, 0);
    xarrow->setColor(paint(xaxisColor, 1.0));
    xarrow->setRotation(0,0,-90);

    yarrow->createCylinder(arrowRadius, 0.0, arrowHeight);
    yarrow->setPosition(0, cylinderHeight, 0);
    yarrow->setColor(paint(yaxisColor, 1.0));

    if (!use2DGizmo){
        zaxis->createCylinder(cylinderRadius, cylinderHeight);
        zaxis->setColor(paint(zaxisColor, 1.0));
        zaxis->setPosition(0, 0, cylinderHeight/2.0);
        zaxis->setRotation(90,0,0);

        zarrow->createCylinder(arrowRadius, 0.0, arrowHeight);
        zarrow->setPosition(0, 0, cylinderHeight);
        zarrow->setColor(paint(zaxisColor, 1.0));
        zarrow->setRotation(90,0,0);

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

    this->addChild(sphere);
    this->addChild(xaxis);
    this->addChild(xarrow);
    this->addChild(yaxis);
    this->addChild(yarrow);

    if (!use2DGizmo){
        this->addChild(zaxis);
        this->addChild(zarrow);
        this->addChild(xyrect);
        this->addChild(xzrect);
        this->addChild(yzrect);
    }
}

editor::TranslateGizmo::~TranslateGizmo(){
    delete sphere;
    delete xaxis;
    delete xarrow;
    delete yaxis;
    delete yarrow;

    if (!use2DGizmo){
        delete zaxis;
        delete zarrow;
        delete xyrect;
        delete xzrect;
        delete yzrect;
    }
}

editor::GizmoSideSelected editor::TranslateGizmo::checkHover(const Ray& ray){

    editor::GizmoSideSelected gizmoSideSelected = GizmoSideSelected::NONE;

    AABB xaabb = xaxis->getModelMatrix() * Matrix4::scaleMatrix(Vector3(2, 1, 2)) * xaxis->getAABB().merge(xarrow->getAABB());
    AABB yaabb = yaxis->getModelMatrix() * Matrix4::scaleMatrix(Vector3(2, 1, 2)) * yaxis->getAABB().merge(yarrow->getAABB());
    AABB sphereaabb = sphere->getModelMatrix() * Matrix4::scaleMatrix(Vector3(1.5, 1.5, 1.5)) * sphere->getAABB();
    AABB supersphereaabb = sphere->getModelMatrix() * Matrix4::scaleMatrix(Vector3(5, 5, 5)) * sphere->getAABB();

    RayReturn rreturn[7] = {};

    rreturn[0] = ray.intersects(xaabb);
    rreturn[1] = ray.intersects(yaabb);

    if (!use2DGizmo) {
        AABB zaabb = zaxis->getModelMatrix() * Matrix4::scaleMatrix(Vector3(2, 1, 2)) * zaxis->getAABB().merge(zarrow->getAABB());
        AABB xyrect_aabb = xyrect->getWorldAABB();
        AABB xzrect_aabb = xzrect->getWorldAABB();
        AABB yzrect_aabb = yzrect->getWorldAABB();

        rreturn[2] = ray.intersects(zaabb);
        rreturn[3] = ray.intersects(xyrect_aabb);
        rreturn[4] = ray.intersects(xzrect_aabb);
        rreturn[5] = ray.intersects(yzrect_aabb);
    }

    rreturn[6] = ray.intersects(sphereaabb);

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
        if (RayReturn creturn = ray.intersects(supersphereaabb)){
            axis = 6;
        }
    }

    if (axis == 0){
        xaxis->setColor(paint(xaxisColorHightlight, 1.0));
        xarrow->setColor(paint(xaxisColorHightlight, 1.0));
        gizmoSideSelected = GizmoSideSelected::X;
    }else{
        xaxis->setColor(paint(xaxisColor, 1.0));
        xarrow->setColor(paint(xaxisColor, 1.0));
    }

    if (axis == 1){
        yaxis->setColor(paint(yaxisColorHightlight, 1.0));
        yarrow->setColor(paint(yaxisColorHightlight, 1.0));
        gizmoSideSelected = GizmoSideSelected::Y;
    }else{
        yaxis->setColor(paint(yaxisColor, 1.0));
        yarrow->setColor(paint(yaxisColor, 1.0));
    }

    if (!use2DGizmo) {
        if (axis == 2){
            zaxis->setColor(paint(zaxisColorHightlight, 1.0));
            zarrow->setColor(paint(zaxisColorHightlight, 1.0));
            gizmoSideSelected = GizmoSideSelected::Z;
        }else{
            zaxis->setColor(paint(zaxisColor, 1.0));
            zarrow->setColor(paint(zaxisColor, 1.0));
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
        sphere->setColor(paint(centerColorHightlight, 1.0));
        gizmoSideSelected = use2DGizmo ? GizmoSideSelected::XY : GizmoSideSelected::XYZ;
    }else{
        sphere->setColor(paint(centerColor, 1.0));
    }

    return gizmoSideSelected;
}
