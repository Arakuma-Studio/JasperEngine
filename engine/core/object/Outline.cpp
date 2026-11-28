// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "Outline.h"

#include "Log.h"
#include "util/Color.h"

#include <cmath>

using namespace jasperengine;

Outline::Outline(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<OutlineComponent>(entity)){
        scene->addComponent<OutlineComponent>(entity, {});
    }
}

Outline::~Outline(){
}

Outline::Outline(const Outline& rhs) = default;
Outline& Outline::operator=(const Outline& rhs) = default;

void Outline::setEnabled(bool enabled){
    getComponent<OutlineComponent>().enabled = enabled;
    markChanged<OutlineComponent>();
}

bool Outline::isEnabled() const{
    return getComponent<OutlineComponent>().enabled;
}

void Outline::setMode(OutlineMode mode){
    if ((int)mode < (int)OutlineMode::OUTLINE_ALL || (int)mode > (int)OutlineMode::SILHOUETTE_ONLY){
        Log::error("Outline mode %i is not an OutlineMode (OUTLINE_ALL .. SILHOUETTE_ONLY)", (int)mode);
        return;
    }
    getComponent<OutlineComponent>().mode = mode;
    markChanged<OutlineComponent>();
}

OutlineMode Outline::getMode() const{
    return getComponent<OutlineComponent>().mode;
}

void Outline::setColor(Vector4 color){
    getComponent<OutlineComponent>().color = Color::sRGBToLinear(color);
    markChanged<OutlineComponent>();
}

Vector4 Outline::getColor() const{
    return Color::linearTosRGB(getComponent<OutlineComponent>().color);
}

void Outline::setWidth(float width){
    if (!std::isfinite(width) || width < 0.0f){
        Log::error("Outline width must be a finite number of 0 or more, got %g", width);
        return;
    }
    getComponent<OutlineComponent>().width = width;
    markChanged<OutlineComponent>();
}

float Outline::getWidth() const{
    return getComponent<OutlineComponent>().width;
}

void Outline::setWidthUnits(OutlineWidthUnits units){
    if (units != OutlineWidthUnits::PIXELS && units != OutlineWidthUnits::WORLD){
        Log::error("Outline width units %i is not an OutlineWidthUnits (PIXELS, WORLD)", (int)units);
        return;
    }
    getComponent<OutlineComponent>().widthUnits = units;
    markChanged<OutlineComponent>();
}

OutlineWidthUnits Outline::getWidthUnits() const{
    return getComponent<OutlineComponent>().widthUnits;
}

void Outline::setIncludeChildren(bool includeChildren){
    getComponent<OutlineComponent>().includeChildren = includeChildren;
    markChanged<OutlineComponent>();
}

bool Outline::isIncludeChildren() const{
    return getComponent<OutlineComponent>().includeChildren;
}
