// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT


#include "MeshPolygon.h"

#include "Engine.h"
#include "component/MeshPolygonComponent.h"
#include "subsystem/MeshSystem.h"
#include "component/MeshComponent.h"

using namespace jasperengine;

MeshPolygon::MeshPolygon(Scene* scene): Mesh(scene){
    addComponent<MeshPolygonComponent>();

    MeshComponent& mesh = getComponent<MeshComponent>();
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLE_STRIP;
    // its one submesh exists from the start, so a script can set its material right away
    mesh.numSubmeshes = 1;
}

MeshPolygon::MeshPolygon(Scene* scene, Entity entity): Mesh(scene, entity){
}

MeshPolygon::~MeshPolygon(){
}

MeshPolygon::MeshPolygon(const MeshPolygon& rhs) = default;
MeshPolygon& MeshPolygon::operator=(const MeshPolygon& rhs) = default;
MeshPolygon::MeshPolygon(MeshPolygon&& rhs) noexcept = default;
MeshPolygon& MeshPolygon::operator=(MeshPolygon&& rhs) noexcept = default;

bool MeshPolygon::createPolygon(){
    MeshPolygonComponent& pcomp = getComponent<MeshPolygonComponent>();
    MeshComponent& mesh = getComponent<MeshComponent>();

    return scene->getSystem<MeshSystem>()->createOrUpdateMeshPolygon(pcomp, mesh);
}

void MeshPolygon::addVertex(Vector3 vertex){
    MeshPolygonComponent& pcomp = getComponent<MeshPolygonComponent>();

    pcomp.points.push_back({vertex, Vector4(1.0f, 1.0f, 1.0f, 1.0f)});

    pcomp.needUpdatePolygon = true;
    markChanged<MeshPolygonComponent>();
}

void MeshPolygon::addVertex(float x, float y){
   addVertex(Vector3(x, y, 0));
}

void MeshPolygon::clearVertices(){
    MeshPolygonComponent& pcomp = getComponent<MeshPolygonComponent>();

    pcomp.points.clear();

    pcomp.needUpdatePolygon = true;
    markChanged<MeshPolygonComponent>();
}

unsigned int MeshPolygon::getWidth(){
    MeshPolygonComponent& pcomp = getComponent<MeshPolygonComponent>();

    return pcomp.width;
}

unsigned int MeshPolygon::getHeight(){
    MeshPolygonComponent& pcomp = getComponent<MeshPolygonComponent>();

    return pcomp.height;
}

void MeshPolygon::setFlipY(bool flipY){
    MeshPolygonComponent& pcomp = getComponent<MeshPolygonComponent>();

    pcomp.automaticFlipY = false;
    if (pcomp.flipY != flipY){
        pcomp.flipY = flipY;

        pcomp.needUpdatePolygon = true;
        markChanged<MeshPolygonComponent>();
    }
}

bool MeshPolygon::isFlipY() const{
    MeshPolygonComponent& pcomp = getComponent<MeshPolygonComponent>();

    return pcomp.flipY;
}