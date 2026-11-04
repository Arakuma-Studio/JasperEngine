// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShadowCaster2D.h"
#include "component/ShadowCaster2DComponent.h"

using namespace jasperengine;

ShadowCaster2D::ShadowCaster2D(Scene* scene): GameObject(scene){
    addComponent<ShadowCaster2DComponent>();
}

ShadowCaster2D::ShadowCaster2D(Scene* scene, Entity entity): GameObject(scene, entity){
}

ShadowCaster2D::~ShadowCaster2D(){
}

ShadowCaster2D::ShadowCaster2D(const ShadowCaster2D& rhs) = default;
ShadowCaster2D& ShadowCaster2D::operator=(const ShadowCaster2D& rhs) = default;
ShadowCaster2D::ShadowCaster2D(ShadowCaster2D&& rhs) noexcept = default;
ShadowCaster2D& ShadowCaster2D::operator=(ShadowCaster2D&& rhs) noexcept = default;

void ShadowCaster2D::setShape(ShadowCaster2DShape shape){
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    occluder.shape = shape;
}

ShadowCaster2DShape ShadowCaster2D::getShape() const{
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    return occluder.shape;
}

void ShadowCaster2D::addVertex(Vector2 vertex){
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    occluder.points.push_back(vertex);
    occluder.shape = ShadowCaster2DShape::POLYGON;
}

void ShadowCaster2D::addVertex(float x, float y){
    addVertex(Vector2(x, y));
}

void ShadowCaster2D::clearVertices(){
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    occluder.points.clear();
}

unsigned int ShadowCaster2D::getVertexCount() const{
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    return (unsigned int)occluder.points.size();
}

void ShadowCaster2D::setClosed(bool closed){
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    occluder.closed = closed;
}

bool ShadowCaster2D::isClosed() const{
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    return occluder.closed;
}

void ShadowCaster2D::setEnabled(bool enabled){
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    occluder.enabled = enabled;
}

bool ShadowCaster2D::isEnabled() const{
    ShadowCaster2DComponent& occluder = getComponent<ShadowCaster2DComponent>();

    return occluder.enabled;
}
