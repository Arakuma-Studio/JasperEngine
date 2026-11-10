// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HitStopExempt.h"

using namespace jasperengine;

HitStopExempt::HitStopExempt(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<HitStopExemptComponent>(entity)){
        scene->addComponent<HitStopExemptComponent>(entity, {});
    }
}

HitStopExempt::~HitStopExempt(){
}

HitStopExempt::HitStopExempt(const HitStopExempt& rhs) = default;
HitStopExempt& HitStopExempt::operator=(const HitStopExempt& rhs) = default;

void HitStopExempt::setEnabled(bool enabled){
    getComponent<HitStopExemptComponent>().enabled = enabled;
}

bool HitStopExempt::isEnabled() const{
    return getComponent<HitStopExemptComponent>().enabled;
}

void HitStopExempt::setIncludeChildren(bool includeChildren){
    getComponent<HitStopExemptComponent>().includeChildren = includeChildren;
}

bool HitStopExempt::isIncludeChildren() const{
    return getComponent<HitStopExemptComponent>().includeChildren;
}
