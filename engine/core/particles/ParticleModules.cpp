// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleModules.h"

using namespace jasperengine;

bool ParticleMainModule::operator==(const ParticleMainModule& o) const{
    return duration == o.duration && looping == o.looping && prewarm == o.prewarm && startDelay == o.startDelay &&
           startLifetime == o.startLifetime && startSpeed == o.startSpeed && startSize3D == o.startSize3D &&
           startSize == o.startSize && startSizeY == o.startSizeY && startSizeZ == o.startSizeZ &&
           startRotation3D == o.startRotation3D && startRotation == o.startRotation && startRotationX == o.startRotationX &&
           startRotationY == o.startRotationY && flipRotation == o.flipRotation && startColor == o.startColor &&
           gravityModifier == o.gravityModifier && gravitySource == o.gravitySource && simulationSpace == o.simulationSpace &&
           simulationSpeed == o.simulationSpeed && scalingMode == o.scalingMode && playOnAwake == o.playOnAwake &&
           maxParticles == o.maxParticles && emitterVelocityMode == o.emitterVelocityMode && stopAction == o.stopAction &&
           autoRandomSeed == o.autoRandomSeed && randomSeed == o.randomSeed;
}

bool ParticleEmissionBurst::operator==(const ParticleEmissionBurst& o) const{
    return time == o.time && count == o.count && cycles == o.cycles && interval == o.interval && probability == o.probability;
}

bool ParticleEmissionModule::operator==(const ParticleEmissionModule& o) const{
    return enabled == o.enabled && rateOverTime == o.rateOverTime && rateOverDistance == o.rateOverDistance && bursts == o.bursts;
}

bool ParticleShapeModule::operator==(const ParticleShapeModule& o) const{
    return enabled == o.enabled && shapeType == o.shapeType && radius == o.radius && radiusThickness == o.radiusThickness &&
           radiusMode == o.radiusMode && radiusSpread == o.radiusSpread && radiusSpeed == o.radiusSpeed && angle == o.angle &&
           length == o.length && arc == o.arc && arcMode == o.arcMode && arcSpread == o.arcSpread && arcSpeed == o.arcSpeed &&
           boxThickness == o.boxThickness && donutRadius == o.donutRadius && position == o.position && rotation == o.rotation &&
           scale == o.scale && alignToDirection == o.alignToDirection && randomDirectionAmount == o.randomDirectionAmount &&
           sphericalDirectionAmount == o.sphericalDirectionAmount && randomPositionAmount == o.randomPositionAmount;
}

bool ParticleVelocityOverLifetimeModule::operator==(const ParticleVelocityOverLifetimeModule& o) const{
    return enabled == o.enabled && x == o.x && y == o.y && z == o.z && orbitalX == o.orbitalX && orbitalY == o.orbitalY &&
           orbitalZ == o.orbitalZ && orbitalOffsetX == o.orbitalOffsetX && orbitalOffsetY == o.orbitalOffsetY &&
           orbitalOffsetZ == o.orbitalOffsetZ && radial == o.radial && speedModifier == o.speedModifier && space == o.space;
}

bool ParticleLimitVelocityOverLifetimeModule::operator==(const ParticleLimitVelocityOverLifetimeModule& o) const{
    return enabled == o.enabled && separateAxes == o.separateAxes && limit == o.limit && limitX == o.limitX &&
           limitY == o.limitY && limitZ == o.limitZ && space == o.space && dampen == o.dampen && drag == o.drag &&
           multiplyDragByParticleSize == o.multiplyDragByParticleSize &&
           multiplyDragByParticleVelocity == o.multiplyDragByParticleVelocity;
}

bool ParticleInheritVelocityModule::operator==(const ParticleInheritVelocityModule& o) const{
    return enabled == o.enabled && mode == o.mode && curve == o.curve;
}

bool ParticleForceOverLifetimeModule::operator==(const ParticleForceOverLifetimeModule& o) const{
    return enabled == o.enabled && x == o.x && y == o.y && z == o.z && space == o.space && randomized == o.randomized;
}

bool ParticleColorOverLifetimeModule::operator==(const ParticleColorOverLifetimeModule& o) const{
    return enabled == o.enabled && color == o.color;
}

bool ParticleColorBySpeedModule::operator==(const ParticleColorBySpeedModule& o) const{
    return enabled == o.enabled && color == o.color && range == o.range;
}

bool ParticleSizeOverLifetimeModule::operator==(const ParticleSizeOverLifetimeModule& o) const{
    return enabled == o.enabled && separateAxes == o.separateAxes && size == o.size && y == o.y && z == o.z;
}

bool ParticleSizeBySpeedModule::operator==(const ParticleSizeBySpeedModule& o) const{
    return enabled == o.enabled && separateAxes == o.separateAxes && size == o.size && y == o.y && z == o.z && range == o.range;
}

bool ParticleRotationOverLifetimeModule::operator==(const ParticleRotationOverLifetimeModule& o) const{
    return enabled == o.enabled && separateAxes == o.separateAxes && x == o.x && y == o.y && z == o.z;
}

bool ParticleRotationBySpeedModule::operator==(const ParticleRotationBySpeedModule& o) const{
    return enabled == o.enabled && separateAxes == o.separateAxes && x == o.x && y == o.y && z == o.z && range == o.range;
}

bool ParticleNoiseModule::operator==(const ParticleNoiseModule& o) const{
    return enabled == o.enabled && separateAxes == o.separateAxes && strength == o.strength && strengthY == o.strengthY &&
           strengthZ == o.strengthZ && frequency == o.frequency && scrollSpeed == o.scrollSpeed && damping == o.damping &&
           octaves == o.octaves && octaveMultiplier == o.octaveMultiplier && octaveScale == o.octaveScale &&
           positionAmount == o.positionAmount && rotationAmount == o.rotationAmount && sizeAmount == o.sizeAmount;
}

bool ParticleTextureSheetAnimationModule::operator==(const ParticleTextureSheetAnimationModule& o) const{
    return enabled == o.enabled && mode == o.mode && numTilesX == o.numTilesX && numTilesY == o.numTilesY &&
           animation == o.animation && rowMode == o.rowMode && rowIndex == o.rowIndex && timeMode == o.timeMode &&
           frameOverTime == o.frameOverTime && fps == o.fps && speedRange == o.speedRange && startFrame == o.startFrame &&
           cycles == o.cycles && flipU == o.flipU && flipV == o.flipV && sprites == o.sprites;
}

bool ParticleRendererModule::operator==(const ParticleRendererModule& o) const{
    return renderMode == o.renderMode && cameraVelocityScale == o.cameraVelocityScale && velocityScale == o.velocityScale &&
           lengthScale == o.lengthScale && alignment == o.alignment && sortMode == o.sortMode && sortingFudge == o.sortingFudge &&
           sortPriority == o.sortPriority && minParticleSize == o.minParticleSize && maxParticleSize == o.maxParticleSize &&
           pivot == o.pivot && flip == o.flip && mesh == o.mesh;
}

void ParticleSimState::clearParticles(){
    position.clear();
    velocity.clear();
    totalVelocity.clear();
    age.clear();
    lifetime.clear();
    startSize.clear();
    size.clear();
    rotation.clear();
    rotationSign.clear();
    startColor.clear();
    color.clear();
    seed.clear();
    frame.clear();
    alignRotation.clear();
}

void ParticleSimState::reserve(size_t capacity){
    position.reserve(capacity);
    velocity.reserve(capacity);
    totalVelocity.reserve(capacity);
    age.reserve(capacity);
    lifetime.reserve(capacity);
    startSize.reserve(capacity);
    size.reserve(capacity);
    rotation.reserve(capacity);
    rotationSign.reserve(capacity);
    startColor.reserve(capacity);
    color.reserve(capacity);
    seed.reserve(capacity);
    frame.reserve(capacity);
    alignRotation.reserve(capacity);
}
