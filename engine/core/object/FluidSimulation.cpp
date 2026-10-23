// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FluidSimulation.h"
#include "fluid/FluidPresets.h"
#include "Log.h"

#include <cmath>
#include <memory>

using namespace jasperengine;

namespace {

    // Setters reject what the solver cannot use (logged, the value kept): NaN and infinity,
    // negatives, and zero where it divides or sizes a target. Other values are kept as given
    // (resolutions outside Pavel's sets included: the simulator sizes its targets from them).

    bool checkNonNegative(const char* field, float value){
        if (std::isfinite(value) && value >= 0.0f)
            return true;
        Log::error("FluidSimulation: %s must be a finite number >= 0 (got %g)", field, (double)value);
        return false;
    }

    bool checkPositive(const char* field, float value){
        if (std::isfinite(value) && value > 0.0f)
            return true;
        Log::error("FluidSimulation: %s must be a finite number > 0 (got %g)", field, (double)value);
        return false;
    }

    bool checkPositive(const char* field, unsigned int value){
        if (value > 0)
            return true;
        Log::error("FluidSimulation: %s must be > 0", field);
        return false;
    }

    bool checkFinite(const char* what, const Vector2& v){
        if (std::isfinite(v.x) && std::isfinite(v.y))
            return true;
        Log::error("FluidSimulation: %s must be finite", what);
        return false;
    }

    bool checkNonNegative(const char* what, const Vector3& v){
        if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f)
            return true;
        Log::error("FluidSimulation: %s must be finite and >= 0", what);
        return false;
    }

}

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

void FluidSimulation::setEnabled(bool value){
    getComponent().enabled = value;
}

bool FluidSimulation::isEnabled() const{
    return getComponent().enabled;
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
    if (!checkNonNegative("aspectRatio", value)) return;
    getComponent().aspectRatio = value;
}

float FluidSimulation::getAspectRatio() const{
    return getComponent().aspectRatio;
}

void FluidSimulation::setSimResolution(unsigned int value){
    if (!checkPositive("simResolution", value)) return;
    getComponent().simResolution = value;
}

unsigned int FluidSimulation::getSimResolution() const{
    return getComponent().simResolution;
}

void FluidSimulation::setDyeResolution(unsigned int value){
    if (!checkPositive("dyeResolution", value)) return;
    getComponent().dyeResolution = value;
}

unsigned int FluidSimulation::getDyeResolution() const{
    return getComponent().dyeResolution;
}

void FluidSimulation::setDensityDissipation(float value){
    if (!checkNonNegative("densityDissipation", value)) return;
    getComponent().densityDissipation = value;
}

float FluidSimulation::getDensityDissipation() const{
    return getComponent().densityDissipation;
}

void FluidSimulation::setVelocityDissipation(float value){
    if (!checkNonNegative("velocityDissipation", value)) return;
    getComponent().velocityDissipation = value;
}

float FluidSimulation::getVelocityDissipation() const{
    return getComponent().velocityDissipation;
}

void FluidSimulation::setPressure(float value){
    if (!checkNonNegative("pressure", value)) return;
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
    if (!checkNonNegative("curl", value)) return;
    getComponent().curl = value;
}

float FluidSimulation::getCurl() const{
    return getComponent().curl;
}

void FluidSimulation::setGradientScale(float value){
    if (!checkNonNegative("gradientScale", value)) return;
    getComponent().gradientScale = value;
}

float FluidSimulation::getGradientScale() const{
    return getComponent().gradientScale;
}

void FluidSimulation::setSimulationRate(float value){
    if (!checkPositive("simulationRate", value)) return;
    getComponent().simulationRate = value;
}

float FluidSimulation::getSimulationRate() const{
    return getComponent().simulationRate;
}

void FluidSimulation::setMaxSubsteps(unsigned int value){
    if (!checkPositive("maxSubsteps", value)) return;
    getComponent().maxSubsteps = value;
}

unsigned int FluidSimulation::getMaxSubsteps() const{
    return getComponent().maxSubsteps;
}

void FluidSimulation::setTimeScale(float value){
    if (!checkNonNegative("timeScale", value)) return;
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
    if (!checkPositive("splatRadius", value)) return;
    getComponent().splatRadius = value;
}

float FluidSimulation::getSplatRadius() const{
    return getComponent().splatRadius;
}

void FluidSimulation::setSplatForce(float value){
    if (!checkNonNegative("splatForce", value)) return;
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
    if (!checkNonNegative("colorUpdateSpeed", value)) return;
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
    if (!checkNonNegative("backColor", value)) return;
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
    if (!checkPositive("bloomResolution", value)) return;
    getComponent().bloomResolution = value;
}

unsigned int FluidSimulation::getBloomResolution() const{
    return getComponent().bloomResolution;
}

void FluidSimulation::setBloomIntensity(float value){
    if (!checkNonNegative("bloomIntensity", value)) return;
    getComponent().bloomIntensity = value;
}

float FluidSimulation::getBloomIntensity() const{
    return getComponent().bloomIntensity;
}

void FluidSimulation::setBloomThreshold(float value){
    if (!checkNonNegative("bloomThreshold", value)) return;
    getComponent().bloomThreshold = value;
}

float FluidSimulation::getBloomThreshold() const{
    return getComponent().bloomThreshold;
}

void FluidSimulation::setBloomSoftKnee(float value){
    if (!checkNonNegative("bloomSoftKnee", value)) return;
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
    if (!checkPositive("sunraysResolution", value)) return;
    getComponent().sunraysResolution = value;
}

unsigned int FluidSimulation::getSunraysResolution() const{
    return getComponent().sunraysResolution;
}

void FluidSimulation::setSunraysWeight(float value){
    if (!checkNonNegative("sunraysWeight", value)) return;
    getComponent().sunraysWeight = value;
}

float FluidSimulation::getSunraysWeight() const{
    return getComponent().sunraysWeight;
}

void FluidSimulation::setBackColor(const float r, const float g, const float b){
    setBackColor(Vector3(r, g, b));
}

void FluidSimulation::splat(Vector2 position, Vector2 velocity, Vector3 color, float radius){
    if (!checkFinite("splat position", position) || !checkFinite("splat velocity", velocity)) return;
    if (!checkNonNegative("splat color", color) || !checkNonNegative("splat radius", radius)) return;
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

std::vector<std::string> FluidSimulation::getPresetIds(){
    std::vector<std::string> ids;
    for (const FluidPreset& preset : getFluidPresets())
        ids.push_back(preset.id);
    return ids;
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
