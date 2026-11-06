// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Mirror.h"
#include "component/MirrorComponent.h"

using namespace jasperengine;

Mirror::Mirror(Scene* scene): Primitive(scene){
    addComponent<MirrorComponent>();
}

Mirror::Mirror(Scene* scene, Entity entity): Primitive(scene, entity){
}

Mirror::~Mirror(){
}

Mirror::Mirror(const Mirror& rhs) = default;
Mirror& Mirror::operator=(const Mirror& rhs) = default;
Mirror::Mirror(Mirror&& rhs) noexcept = default;
Mirror& Mirror::operator=(Mirror&& rhs) noexcept = default;

void Mirror::setNormal(Vector3 normal){
    MirrorComponent& mirror = getComponent<MirrorComponent>();

    mirror.normal = normal;
}

void Mirror::setNormal(const float x, const float y, const float z){
    setNormal(Vector3(x, y, z));
}

Vector3 Mirror::getNormal() const{
    MirrorComponent& mirror = getComponent<MirrorComponent>();

    return mirror.normal;
}
