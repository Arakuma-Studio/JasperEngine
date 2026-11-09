// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SortingGroup.h"

#include "Log.h"
#include "component/SortingGroupComponent.h"
#include "registry/SortingLayers.h"

#include <algorithm>

using namespace jasperengine;

SortingGroup::SortingGroup(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<SortingGroupComponent>(entity)){
        scene->addComponent<SortingGroupComponent>(entity, {});
    }
}

SortingGroup::~SortingGroup(){
}

SortingGroup::SortingGroup(const SortingGroup& rhs) = default;
SortingGroup& SortingGroup::operator=(const SortingGroup& rhs) = default;

void SortingGroup::setEnabled(bool enabled){
    getComponent<SortingGroupComponent>().enabled = enabled;
    markChanged<SortingGroupComponent>();
}

bool SortingGroup::isEnabled() const{
    return getComponent<SortingGroupComponent>().enabled;
}

void SortingGroup::setSortingLayerId(int id){
    if (!SortingLayers::isValid(id)){
        Log::error("No Sorting Layer has id %i (Project Settings > Tags and Layers > Sorting Layers)", id);
        return;
    }
    getComponent<SortingGroupComponent>().sortingLayer = id;
    markChanged<SortingGroupComponent>();
}

int SortingGroup::getSortingLayerId() const{
    return getComponent<SortingGroupComponent>().sortingLayer;
}

void SortingGroup::setSortingLayerName(const std::string& name){
    const int32_t id = SortingLayers::nameToId(name);
    if (id < 0){
        Log::error("No Sorting Layer is named \"%s\" (Project Settings > Tags and Layers > Sorting Layers)", name.c_str());
        return;
    }
    setSortingLayerId(id);
}

std::string SortingGroup::getSortingLayerName() const{
    return SortingLayers::idToName(getSortingLayerId());
}

void SortingGroup::setSortingOrder(int order){
    getComponent<SortingGroupComponent>().sortingOrder = std::clamp(order, -32768, 32767);
    markChanged<SortingGroupComponent>();
}

int SortingGroup::getSortingOrder() const{
    return getComponent<SortingGroupComponent>().sortingOrder;
}
