// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavAreaOverride.h"

#include "Log.h"
#include "Scene.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    struct OverrideBox {
        Vector3 center, size;
        Quaternion rotation;
        uint8_t area = 0;
    };

    bool readBox(NavigationSystem* system, uint32_t handle, OverrideBox& box) {
        return system && system->getAreaOverride(handle, box.center, box.size, box.rotation, box.area);
    }

}

NavAreaOverride::NavAreaOverride(Scene* scene, uint32_t handle): scene(scene), handle(handle){
    if (scene)
        sceneLifetime = scene->getLifetime();
}

NavigationSystem* NavAreaOverride::system() const{
    if (!scene || !handle || sceneLifetime.expired())
        return nullptr;
    return scene->getSystem<NavigationSystem>().get();
}

bool NavAreaOverride::isValid() const{
    NavigationSystem* navigation = system();
    return navigation && navigation->hasAreaOverride(handle);
}

void NavAreaOverride::setArea(int area){
    OverrideBox box;
    if (!readBox(system(), handle, box)){
        Log::warn("NavAreaOverride: the volume was removed");
        return;
    }
    box.area = (uint8_t)std::clamp(area, 0, nav::AREA_COUNT - 1);
    system()->updateAreaOverride(handle, box.center, box.size, box.rotation, box.area);
}

int NavAreaOverride::getArea() const{
    OverrideBox box;
    return readBox(system(), handle, box) ? box.area : -1;
}

void NavAreaOverride::setCenter(Vector3 center){
    OverrideBox box;
    if (!readBox(system(), handle, box))
        return;
    box.center = center;
    system()->updateAreaOverride(handle, box.center, box.size, box.rotation, box.area);
}

Vector3 NavAreaOverride::getCenter() const{
    OverrideBox box;
    readBox(system(), handle, box);
    return box.center;
}

void NavAreaOverride::setSize(Vector3 size){
    OverrideBox box;
    if (!readBox(system(), handle, box))
        return;
    box.size = size;
    system()->updateAreaOverride(handle, box.center, box.size, box.rotation, box.area);
}

Vector3 NavAreaOverride::getSize() const{
    OverrideBox box;
    readBox(system(), handle, box);
    return box.size;
}

void NavAreaOverride::setRotation(Quaternion rotation){
    OverrideBox box;
    if (!readBox(system(), handle, box))
        return;
    box.rotation = rotation;
    system()->updateAreaOverride(handle, box.center, box.size, box.rotation, box.area);
}

Quaternion NavAreaOverride::getRotation() const{
    OverrideBox box;
    readBox(system(), handle, box);
    return box.rotation;
}

int NavAreaOverride::getAppliedPolygons() const{
    NavigationSystem* navigation = system();
    return navigation ? navigation->getAreaOverridePolygons(handle) : 0;
}

void NavAreaOverride::remove(){
    if (NavigationSystem* navigation = system())
        navigation->removeAreaOverride(handle);
}
