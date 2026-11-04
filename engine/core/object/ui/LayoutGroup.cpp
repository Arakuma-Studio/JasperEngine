// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LayoutGroup.h"

#include <algorithm>
#include <cmath>
#include "component/LayoutGroupComponent.h"

using namespace jasperengine;

LayoutGroup::LayoutGroup(Scene* scene): RectTransform(scene){
    addComponent<LayoutGroupComponent>();
}

LayoutGroup::LayoutGroup(Scene* scene, Entity entity): RectTransform(scene, entity){
}

LayoutGroup::~LayoutGroup(){
}

LayoutGroup::LayoutGroup(const LayoutGroup& rhs) = default;
LayoutGroup& LayoutGroup::operator=(const LayoutGroup& rhs) = default;
LayoutGroup::LayoutGroup(LayoutGroup&& rhs) noexcept = default;
LayoutGroup& LayoutGroup::operator=(LayoutGroup&& rhs) noexcept = default;

void LayoutGroup::setType(LayoutGroupType type){
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    container.type = type;
}

LayoutGroupType LayoutGroup::getType() const{
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    return container.type;
}

void LayoutGroup::setUseAllWrapSpace(bool useAllWrapSpace){
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    container.useAllWrapSpace = useAllWrapSpace;
}

bool LayoutGroup::isUseAllWrapSpace() const{
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    return container.useAllWrapSpace;
}

void LayoutGroup::setWrapCellWidth(unsigned int width){
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    container.wrapCellWidth = width;
}

unsigned int LayoutGroup::getWrapCellWidth() const{
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    return container.wrapCellWidth;
}

void LayoutGroup::setWrapCellHeight(unsigned int height){
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    container.wrapCellHeight = height;
}

unsigned int LayoutGroup::getWrapCellHeight() const{
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    return container.wrapCellHeight;
}

unsigned int LayoutGroup::getContentWidth() const{
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    float maxX = 0.0f;
    for (int b = 0; b < container.numBoxes; b++){
        if (container.boxes[b].layout != NULL_ENTITY){
            maxX = std::max(maxX, container.boxes[b].rect.getX() + container.boxes[b].rect.getWidth());
        }
    }

    return static_cast<unsigned int>(std::ceil(maxX));
}

unsigned int LayoutGroup::getContentHeight() const{
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

    float maxY = 0.0f;
    for (int b = 0; b < container.numBoxes; b++){
        if (container.boxes[b].layout != NULL_ENTITY){
            maxY = std::max(maxY, container.boxes[b].rect.getY() + container.boxes[b].rect.getHeight());
        }
    }

    return static_cast<unsigned int>(std::ceil(maxY));
}

void LayoutGroup::resize(){
    setSize(0, 0);
}

void LayoutGroup::setBoxExpand(bool expand){
    LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();
    for (int b = 0; b < MAX_CONTAINER_BOXES; b++){
        container.boxes[b].expand = expand;
    }
}

void LayoutGroup::setBoxExpand(size_t id, bool expand){
    if (id < MAX_CONTAINER_BOXES){
        LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

        container.boxes[id].expand = expand;
    }else{
        Log::error("Cannot find a box container with id %zu", id);
    }
}

bool LayoutGroup::isBoxExpand(size_t id) const{
    if (id < MAX_CONTAINER_BOXES){
        LayoutGroupComponent& container = getComponent<LayoutGroupComponent>();

        return container.boxes[id].expand;
    }else{
        Log::error("Cannot find a box container with id %zu", id);
        throw std::out_of_range("box container is out of range");
    }
}