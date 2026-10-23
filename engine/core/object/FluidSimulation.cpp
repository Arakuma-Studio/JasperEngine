// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FluidSimulation.h"
#include "fluid/FluidPresets.h"

#include <memory>

using namespace jasperengine;

FluidSimulation::FluidSimulation(Scene* scene): GameObject(scene){
    addComponent<FluidSimulationComponent>();
}

FluidSimulation::FluidSimulation(Scene* scene, Entity entity): GameObject(scene, entity){
}

FluidSimulation::~FluidSimulation(){
}

FluidSimulation::FluidSimulation(const FluidSimulation& rhs) = default;
FluidSimulation& FluidSimulation::operator=(const FluidSimulation& rhs) = default;
FluidSimulation::FluidSimulation(FluidSimulation&& rhs) noexcept = default;
FluidSimulation& FluidSimulation::operator=(FluidSimulation&& rhs) noexcept = default;

FluidSimulationComponent& FluidSimulation::getComponent() const{
    return GameObject::getComponent<FluidSimulationComponent>();
}

void FluidSimulation::setOutput(FluidOutput value){
    getComponent().output = value;
}

FluidOutput FluidSimulation::getOutput() const{
    return getComponent().output;
}

void FluidSimulation::setInteractive(bool value){
    getComponent().interactive = value;
}

bool FluidSimulation::isInteractive() const{
    return getComponent().interactive;
}

void FluidSimulation::setAspectRatio(float value){
    getComponent().aspectRatio = value;
}

float FluidSimulation::getAspectRatio() const{
    return getComponent().aspectRatio;
}

void FluidSimulation::setSimResolution(unsigned int value){
    getComponent().simResolution = value;
}

unsigned int FluidSimulation::getSimResolution() const{
    return getComponent().simResolution;
}

void FluidSimulation::setDyeResolution(unsigned int value){
    getComponent().dyeResolution = value;
}

unsigned int FluidSimulation::getDyeResolution() const{
    return getComponent().dyeResolution;
}

void FluidSimulation::setDensityDissipation(float value){
    getComponent().densityDissipation = value;
}

float FluidSimulation::getDensityDissipation() const{
    return getComponent().densityDissipation;
}

void FluidSimulation::setVelocityDissipation(float value){
    getComponent().velocityDissipation = value;
}

float FluidSimulation::getVelocityDissipation() const{
    return getComponent().velocityDissipation;
}

void FluidSimulation::setPressure(float value){
    getComponent().pressure = value;
}

float FluidSimulation::getPressure() const{
    return getComponent().pressure;
}

void FluidSimulation::setPressureIterations(unsigned int value){
    getComponent().pressureIterations = value;
}

unsigned int FluidSimulation::getPressureIterations() const{
    return getComponent().pressureIterations;
}

void FluidSimulation::setCurl(float value){
    getComponent().curl = value;
}

float FluidSimulation::getCurl() const{
    return getComponent().curl;
}

void FluidSimulation::setGradientScale(float value){
    getComponent().gradientScale = value;
}

float FluidSimulation::getGradientScale() const{
    return getComponent().gradientScale;
}

void FluidSimulation::setSimulationRate(float value){
    getComponent().simulationRate = value;
}

float FluidSimulation::getSimulationRate() const{
    return getComponent().simulationRate;
}

void FluidSimulation::setMaxSubsteps(unsigned int value){
    getComponent().maxSubsteps = value;
}

unsigned int FluidSimulation::getMaxSubsteps() const{
    return getComponent().maxSubsteps;
}

void FluidSimulation::setTimeScale(float value){
    getComponent().timeScale = value;
}

float FluidSimulation::getTimeScale() const{
    return getComponent().timeScale;
}

void FluidSimulation::setPaused(bool value){
    getComponent().paused = value;
}

bool FluidSimulation::isPaused() const{
    return getComponent().paused;
}

void FluidSimulation::setSplatRadius(float value){
    getComponent().splatRadius = value;
}

float FluidSimulation::getSplatRadius() const{
    return getComponent().splatRadius;
}

void FluidSimulation::setSplatForce(float value){
    getComponent().splatForce = value;
}

float FluidSimulation::getSplatForce() const{
    return getComponent().splatForce;
}

void FluidSimulation::setColorful(bool value){
    getComponent().colorful = value;
}

bool FluidSimulation::isColorful() const{
    return getComponent().colorful;
}

void FluidSimulation::setColorUpdateSpeed(float value){
    getComponent().colorUpdateSpeed = value;
}

float FluidSimulation::getColorUpdateSpeed() const{
    return getComponent().colorUpdateSpeed;
}

void FluidSimulation::setSplatsOnStart(bool value){
    getComponent().splatsOnStart = value;
}

bool FluidSimulation::isSplatsOnStart() const{
    return getComponent().splatsOnStart;
}

void FluidSimulation::setRandomSplatCount(unsigned int value){
    getComponent().randomSplatCount = value;
}

unsigned int FluidSimulation::getRandomSplatCount() const{
    return getComponent().randomSplatCount;
}

void FluidSimulation::setSeed(uint32_t value){
    getComponent().seed = value;
}

uint32_t FluidSimulation::getSeed() const{
    return getComponent().seed;
}

void FluidSimulation::setShading(bool value){
    getComponent().shading = value;
}

bool FluidSimulation::isShading() const{
    return getComponent().shading;
}

void FluidSimulation::setBackColor(Vector3 value){
    getComponent().backColor = value;
}

Vector3 FluidSimulation::getBackColor() const{
    return getComponent().backColor;
}

void FluidSimulation::setTransparent(bool value){
    getComponent().transparent = value;
}

bool FluidSimulation::isTransparent() const{
    return getComponent().transparent;
}

void FluidSimulation::setBloom(bool value){
    getComponent().bloom = value;
}

bool FluidSimulation::isBloom() const{
    return getComponent().bloom;
}

void FluidSimulation::setBloomIterations(unsigned int value){
    getComponent().bloomIterations = value;
}

unsigned int FluidSimulation::getBloomIterations() const{
    return getComponent().bloomIterations;
}

void FluidSimulation::setBloomResolution(unsigned int value){
    getComponent().bloomResolution = value;
}

unsigned int FluidSimulation::getBloomResolution() const{
    return getComponent().bloomResolution;
}

void FluidSimulation::setBloomIntensity(float value){
    getComponent().bloomIntensity = value;
}

float FluidSimulation::getBloomIntensity() const{
    return getComponent().bloomIntensity;
}

void FluidSimulation::setBloomThreshold(float value){
    getComponent().bloomThreshold = value;
}

float FluidSimulation::getBloomThreshold() const{
    return getComponent().bloomThreshold;
}

void FluidSimulation::setBloomSoftKnee(float value){
    getComponent().bloomSoftKnee = value;
}

float FluidSimulation::getBloomSoftKnee() const{
    return getComponent().bloomSoftKnee;
}

void FluidSimulation::setSunrays(bool value){
    getComponent().sunrays = value;
}

bool FluidSimulation::isSunrays() const{
    return getComponent().sunrays;
}

void FluidSimulation::setSunraysResolution(unsigned int value){
    getComponent().sunraysResolution = value;
}

unsigned int FluidSimulation::getSunraysResolution() const{
    return getComponent().sunraysResolution;
}

void FluidSimulation::setSunraysWeight(float value){
    getComponent().sunraysWeight = value;
}

float FluidSimulation::getSunraysWeight() const{
    return getComponent().sunraysWeight;
}

void FluidSimulation::setBackColor(const float r, const float g, const float b){
    setBackColor(Vector3(r, g, b));
}

void FluidSimulation::splat(Vector2 position, Vector2 velocity, Vector3 color, float radius){
    FluidSplat splat;
    splat.position = position;
    splat.velocity = velocity;
    splat.color = color;
    splat.radius = radius;
    getComponent().pendingSplats.push_back(splat);
}

void FluidSimulation::splat(const float x, const float y, const float dx, const float dy, const float r, const float g, const float b){
    splat(Vector2(x, y), Vector2(dx, dy), Vector3(r, g, b));
}

void FluidSimulation::randomSplats(unsigned int count){
    getComponent().pendingBursts.push_back(count);
}

void FluidSimulation::reset(){
    getComponent().needReset = true;
}

void FluidSimulation::step(int steps){
    if (steps > 0)
        getComponent().pendingSteps += (unsigned int)steps;
}

bool FluidSimulation::applyPreset(const std::string& id){
    return applyFluidPreset(getComponent(), id);
}

std::string FluidSimulation::getPreset() const{
    return matchFluidPreset(getComponent());
}

Texture& FluidSimulation::getTexture() const{
    FluidSimulationComponent& fluid = getComponent();
    if (!fluid.simulator)
        fluid.simulator = std::make_shared<FluidSimulator>();
    return fluid.simulator->getOutputTexture();
}

FluidStats FluidSimulation::getStats() const{
    FluidSimulationComponent& fluid = getComponent();
    if (!fluid.simulator)
        return FluidStats();
    return fluid.simulator->getStats();
}
