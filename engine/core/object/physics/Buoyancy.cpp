// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Buoyancy.h"

using namespace jasperengine;

Buoyancy::Buoyancy(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

Buoyancy::~Buoyancy() = default;
Buoyancy::Buoyancy(const Buoyancy& rhs) = default;
Buoyancy& Buoyancy::operator=(const Buoyancy& rhs) = default;
Buoyancy::Buoyancy(Buoyancy&& rhs) noexcept = default;
Buoyancy& Buoyancy::operator=(Buoyancy&& rhs) noexcept = default;

BuoyancyComponent& Buoyancy::component() const{
    return getComponent<BuoyancyComponent>();
}

bool Buoyancy::isValid() const{
    return scene && scene->findComponent<BuoyancyComponent>(entity) != nullptr;
}

void Buoyancy::setEnabled(bool enabled){ component().enabled = enabled; }
bool Buoyancy::isEnabled() const{ return component().enabled; }

void Buoyancy::setColumns(BuoyancyColumns columns){ component().columns = columns; }
BuoyancyColumns Buoyancy::getColumns() const{ return component().columns; }

void Buoyancy::setVolumeScale(float volumeScale){ component().volumeScale = volumeScale; }
float Buoyancy::getVolumeScale() const{ return component().volumeScale; }

void Buoyancy::setDragScale(float dragScale){ component().dragScale = dragScale; }
float Buoyancy::getDragScale() const{ return component().dragScale; }

void Buoyancy::setCustomPoints(const std::vector<Vector3>& points){ component().customPoints = points; }
std::vector<Vector3> Buoyancy::getCustomPoints() const{ return component().customPoints; }

