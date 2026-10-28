// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplineExtrude.h"

#include "Scene.h"
#include "subsystem/SplineSystem.h"

using namespace jasperengine;

SplineExtrude::SplineExtrude(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

SplineExtrude::~SplineExtrude() = default;
SplineExtrude::SplineExtrude(const SplineExtrude& rhs) = default;
SplineExtrude& SplineExtrude::operator=(const SplineExtrude& rhs) = default;
SplineExtrude::SplineExtrude(SplineExtrude&& rhs) noexcept = default;
SplineExtrude& SplineExtrude::operator=(SplineExtrude&& rhs) noexcept = default;

SplineExtrudeComponent& SplineExtrude::component() const{
    return getComponent<SplineExtrudeComponent>();
}

bool SplineExtrude::isValid() const{
    return scene && scene->findComponent<SplineExtrudeComponent>(entity) != nullptr;
}

void SplineExtrude::setEnabled(bool enabled){ component().enabled = enabled; }
bool SplineExtrude::isEnabled() const{ return component().enabled; }

void SplineExtrude::setShape(SplineExtrudeShape value){ component().shape = value; }
SplineExtrudeShape SplineExtrude::getShape() const{ return component().shape; }

void SplineExtrude::setWidthSource(SplineWidthSource value){ component().widthSource = value; }
SplineWidthSource SplineExtrude::getWidthSource() const{ return component().widthSource; }

void SplineExtrude::setWidth(float value){ component().width = value; }
float SplineExtrude::getWidth() const{ return component().width; }

void SplineExtrude::setEdgeDrop(float value){ component().edgeDrop = value; }
float SplineExtrude::getEdgeDrop() const{ return component().edgeDrop; }

void SplineExtrude::setEdgeInset(float value){ component().edgeInset = value; }
float SplineExtrude::getEdgeInset() const{ return component().edgeInset; }

void SplineExtrude::setCrownRise(float value){ component().crownRise = value; }
float SplineExtrude::getCrownRise() const{ return component().crownRise; }

void SplineExtrude::setShoulderWidth(float value){ component().shoulderWidth = value; }
float SplineExtrude::getShoulderWidth() const{ return component().shoulderWidth; }

void SplineExtrude::setShoulderDrop(float value){ component().shoulderDrop = value; }
float SplineExtrude::getShoulderDrop() const{ return component().shoulderDrop; }

void SplineExtrude::setThickness(float value){ component().thickness = value; }
float SplineExtrude::getThickness() const{ return component().thickness; }

void SplineExtrude::setHeight(float value){ component().height = value; }
float SplineExtrude::getHeight() const{ return component().height; }

void SplineExtrude::setGrade(SplineGrade value){ component().grade = value; }
SplineGrade SplineExtrude::getGrade() const{ return component().grade; }

void SplineExtrude::setStepHeight(float value){ component().stepHeight = value; }
float SplineExtrude::getStepHeight() const{ return component().stepHeight; }

void SplineExtrude::setTubeSides(unsigned int value){ component().tubeSides = value; }
unsigned int SplineExtrude::getTubeSides() const{ return component().tubeSides; }

void SplineExtrude::setMaxHalfWidth(float value){ component().maxHalfWidth = value; }
float SplineExtrude::getMaxHalfWidth() const{ return component().maxHalfWidth; }

void SplineExtrude::setWaterEdgeDrop(float value){ component().waterEdgeDrop = value; }
float SplineExtrude::getWaterEdgeDrop() const{ return component().waterEdgeDrop; }

void SplineExtrude::setFlow(bool value){ component().flow = value; }
bool SplineExtrude::isFlow() const{ return component().flow; }

void SplineExtrude::setFlowSpeed(float value){ component().flowSpeed = value; }
float SplineExtrude::getFlowSpeed() const{ return component().flowSpeed; }

void SplineExtrude::setTilesPerMetreU(float value){ component().tilesPerMetreU = value; }
float SplineExtrude::getTilesPerMetreU() const{ return component().tilesPerMetreU; }

void SplineExtrude::setTilesPerMetreV(float value){ component().tilesPerMetreV = value; }
float SplineExtrude::getTilesPerMetreV() const{ return component().tilesPerMetreV; }

void SplineExtrude::setLevelCourseV(bool value){ component().levelCourseV = value; }
bool SplineExtrude::isLevelCourseV() const{ return component().levelCourseV; }

void SplineExtrude::setCorners(SplineCornerStyle value){ component().corners = value; }
SplineCornerStyle SplineExtrude::getCorners() const{ return component().corners; }

void SplineExtrude::setEndCaps(bool value){ component().endCaps = value; }
bool SplineExtrude::isEndCaps() const{ return component().endCaps; }

void SplineExtrude::setEndTaper(float value){ component().endTaper = value; }
float SplineExtrude::getEndTaper() const{ return component().endTaper; }

void SplineExtrude::setLateralOffset(float value){ component().lateralOffset = value; }
float SplineExtrude::getLateralOffset() const{ return component().lateralOffset; }

void SplineExtrude::setVerticalOffset(float value){ component().verticalOffset = value; }
float SplineExtrude::getVerticalOffset() const{ return component().verticalOffset; }

void SplineExtrude::setConform(SplineConform value){ component().conform = value; }
SplineConform SplineExtrude::getConform() const{ return component().conform; }

void SplineExtrude::setSlopeBlend(float value){ component().slopeBlend = value; }
float SplineExtrude::getSlopeBlend() const{ return component().slopeBlend; }

void SplineExtrude::setStationSpacing(float value){ component().stationSpacing = value; }
float SplineExtrude::getStationSpacing() const{ return component().stationSpacing; }

void SplineExtrude::setCollision(SplineCollision value){ component().collision = value; }
SplineCollision SplineExtrude::getCollision() const{ return component().collision; }

static SplineSystem* splineSystemOf(Scene* scene){
    return scene ? scene->findSystem<SplineSystem>().get() : nullptr;
}

unsigned int SplineExtrude::getVertexCount() const{
    SplineSystem* system = splineSystemOf(scene);
    return system ? system->getExtrudeStats(entity).vertices : 0;
}

unsigned int SplineExtrude::getChunkCount() const{
    SplineSystem* system = splineSystemOf(scene);
    return system ? system->getExtrudeStats(entity).chunks : 0;
}

bool SplineExtrude::isPending() const{
    SplineSystem* system = splineSystemOf(scene);
    return system && system->isRebuildPending(entity);
}

void SplineExtrude::rebuild(){
    if (SplineSystem* system = splineSystemOf(scene)){
        system->rebuildNow(entity);
    }
}
