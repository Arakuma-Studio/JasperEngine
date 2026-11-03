// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShakeInstance.h"

#include "math/Noise.h"

#include <algorithm>
#include <cmath>
#include <random>

using namespace jasperengine;
using namespace jasperengine::camera;

namespace{

    float clamp01(float value){
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    Quaternion axisAngleDegrees(float degrees, const Vector3& axis){
        const float half = degrees * 3.14159265358979323846f / 360.0f;
        const float s = std::sin(half);
        return Quaternion(std::cos(half), axis.x * s, axis.y * s, axis.z * s);
    }

}

float camera::shakeNoise(float x, float y){
    return (perlinNoise(x, y) - 0.5f) * 2.0f;
}

ShakeOffset camera::evaluateShake(const ShakePreset& preset, int baseSeed, float noiseTime, float strength){
    const float b = (float)baseSeed;
    const float s1 = b / 2.0f;
    const float s2 = b / 3.0f;
    const float s3 = b / 4.0f;
    const float t = noiseTime;

    ShakeOffset offset;
    offset.position = Vector3(shakeNoise(t + s1, b), shakeNoise(b, t), shakeNoise(s3 + t, b + t)) * strength;
    offset.position = Vector3(offset.position.x * preset.positionInfluence.x,
                              offset.position.y * preset.positionInfluence.y,
                              offset.position.z * preset.positionInfluence.z);
    offset.rotation = Vector3(shakeNoise(t - b, s3), shakeNoise(b, t + s2), shakeNoise(b + t, s1 + t)) * strength;
    offset.rotation = Vector3(offset.rotation.x * preset.rotationInfluence.x,
                              offset.rotation.y * preset.rotationInfluence.y,
                              offset.rotation.z * preset.rotationInfluence.z);
    return offset;
}

int camera::randomShakeSeed(){
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(-10000, 10000);
    return distribution(generator);
}

Quaternion camera::shakeEulerToQuaternion(const Vector3& degrees){
    const Quaternion qx = axisAngleDegrees(degrees.x, Vector3(1, 0, 0));
    const Quaternion qy = axisAngleDegrees(degrees.y, Vector3(0, 1, 0));
    const Quaternion qz = axisAngleDegrees(degrees.z, Vector3(0, 0, 1));
    return qy * (qx * qz);
}

ShakeInstance::ShakeInstance(const ShakePreset& preset, int seed): preset(preset){
    baseSeed = seed;
    noiseTime = (float)baseSeed;
    fadeInTime = preset.fadeIn;
    fadeOutTime = preset.fadeOut;
    state = ShakeState::FadingIn;
}

float ShakeInstance::getCurrentStrength() const{
    return preset.strength * fade * strengthScale;
}

float ShakeInstance::getCurrentRoughness() const{
    return preset.roughness * fade * roughnessScale;
}

void ShakeInstance::setFadeTimes(float fadeIn, float fadeOut){
    if (fadeIn >= 0.0f){
        preset.fadeIn = fadeIn;
        fadeInTime = fadeIn;
    }
    if (fadeOut >= 0.0f){
        preset.fadeOut = fadeOut;
        // a shake already fading out keeps the time its stop() gave
        if (state != ShakeState::FadingOut) fadeOutTime = fadeOut;
    }
}

ShakeOffset ShakeInstance::getOffset() const{
    if (state == ShakeState::Stopped) return ShakeOffset();
    return evaluateShake(preset, baseSeed, noiseTime, getCurrentStrength());
}

ShakeOffset ShakeInstance::update(float dt){
    ShakeOffset offset = getOffset();
    step(dt);
    return offset;
}

void ShakeInstance::step(float dt){
    if (!(dt > 0.0f) || state == ShakeState::Stopped) return;

    if (pauseFadeTime > 0.0f){
        pauseAmount += (paused ? dt : -dt) / pauseFadeTime;
    }
    pauseAmount = clamp01(pauseAmount);

    // the roughness of the fade before this step (MilkShake's order)
    noiseTime += (1.0f - pauseAmount) * dt * getCurrentRoughness();

    if (state == ShakeState::FadingIn){
        fade = fadeInTime > 0.0f ? fade + dt / fadeInTime : 1.0f;
    }else if (state == ShakeState::FadingOut){
        fade = fadeOutTime > 0.0f ? fade - dt / fadeOutTime : 0.0f;
    }
    fade = clamp01(fade);

    if (fade >= 1.0f && state == ShakeState::FadingIn){
        if (preset.type == ShakeType::Sustained){
            state = ShakeState::Sustained;
        }else{
            stop(preset.fadeOut);
        }
    }else if (fade <= 0.0f && state == ShakeState::FadingOut){
        state = ShakeState::Stopped;
    }
}

void ShakeInstance::stop(float fadeTime){
    if (state == ShakeState::Stopped) return;
    fadeOutTime = fadeTime >= 0.0f ? fadeTime : preset.fadeOut;
    state = ShakeState::FadingOut;
}

void ShakeInstance::pause(float fadeTime){
    paused = true;
    pauseFadeTime = std::max(0.0f, fadeTime);
    if (pauseFadeTime <= 0.0f) pauseAmount = 1.0f;
}

void ShakeInstance::resume(float fadeTime){
    paused = false;
    pauseFadeTime = std::max(0.0f, fadeTime);
    if (pauseFadeTime <= 0.0f) pauseAmount = 0.0f;
}

void TraumaState::add(float amount){
    if (!std::isfinite(amount)) return;
    trauma = clamp01(trauma + amount);
}

void TraumaState::step(float dt, float decay, float frequency){
    if (!(dt > 0.0f)) return;
    trauma = std::max(0.0f, trauma - std::max(0.0f, decay) * dt);
    clock += dt * frequency;
}

ShakeOffset TraumaState::getOffset(float power, const Vector3& maxOffset, const Vector3& maxAngle) const{
    ShakeOffset offset;
    if (trauma <= 0.0f) return offset;
    const float k = std::pow(trauma, power);
    const float tau = clock;
    const float sT = seed;
    offset.position = Vector3(maxOffset.x * shakeNoise(tau + 11.0f, sT),
                              maxOffset.y * shakeNoise(sT, tau + 23.0f),
                              maxOffset.z * shakeNoise(tau + 37.0f, sT + 5.0f)) * k;
    offset.rotation = Vector3(maxAngle.x * shakeNoise(tau + 41.0f, sT + 3.0f),
                              maxAngle.y * shakeNoise(sT + 7.0f, tau + 53.0f),
                              maxAngle.z * shakeNoise(tau + 61.0f, sT + 9.0f)) * k;
    return offset;
}

std::vector<ShakeOffset> camera::sampleShakePreset(const ShakePreset& preset, float duration, float hz, int seed){
    std::vector<ShakeOffset> samples;
    if (!(duration > 0.0f) || !(hz > 0.0f)) return samples;
    const float dt = 1.0f / hz;
    const int count = (int)std::ceil(duration * hz) + 1;
    samples.reserve((size_t)count);
    ShakeInstance instance(preset, seed);
    const float stopAt = preset.fadeIn + 1.0f;
    bool stopped = false;
    for (int i = 0; i < count; i++){
        if (!stopped && preset.type == ShakeType::Sustained && i * dt >= stopAt){
            instance.stop();
            stopped = true;
        }
        samples.push_back(instance.update(dt));
    }
    return samples;
}

ShakeOffset camera::shakeOffsetPeaks(const std::vector<ShakeOffset>& samples){
    ShakeOffset peaks;
    for (const ShakeOffset& sample : samples){
        peaks.position = Vector3(std::max(peaks.position.x, std::fabs(sample.position.x)),
                                 std::max(peaks.position.y, std::fabs(sample.position.y)),
                                 std::max(peaks.position.z, std::fabs(sample.position.z)));
        peaks.rotation = Vector3(std::max(peaks.rotation.x, std::fabs(sample.rotation.x)),
                                 std::max(peaks.rotation.y, std::fabs(sample.rotation.y)),
                                 std::max(peaks.rotation.z, std::fabs(sample.rotation.z)));
    }
    return peaks;
}
