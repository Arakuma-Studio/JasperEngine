// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 easings/spring/index.js (MIT), itself adapted from
// https://webkit.org/demos/spring/spring.js (c) 2016 WebKit - Apple Inc. The engine runs
// Anime's millisecond mode only (globals.timeScale = 1).

#include "AnimSpring.h"

#include "AnimCommon.h"

#include <cmath>

using namespace jasperengine;

namespace {
    const double maxSpringParamValue = AnimConst::K * 10;
    const double PI = 3.14159265358979323846;
}

AnimSpring::AnimSpring(const AnimSpringParams& params){
    const bool hasBounceOrDuration = params.bounce.has_value() || params.duration.has_value();
    maxRestSteps = restDuration / timeStep / AnimConst::K;
    maxIterations = maxDuration / timeStep / AnimConst::K;
    bn = AnimMath::clamp(params.bounce.value_or(.5), -1, 1);
    pd = AnimMath::clamp(params.duration.value_or(628), 10, maxSpringParamValue);
    m = AnimMath::clamp(params.mass.value_or(1), 1, maxSpringParamValue);
    s = AnimMath::clamp(params.stiffness.value_or(100), AnimConst::minValue, maxSpringParamValue);
    d = AnimMath::clamp(params.damping.value_or(10), AnimConst::minValue, maxSpringParamValue);
    v = AnimMath::clamp(params.velocity.value_or(0), -maxSpringParamValue, maxSpringParamValue);
    onComplete = params.onComplete;
    if (hasBounceOrDuration)
        calculateSDFromBD();
    compute();
}

double AnimSpring::ease(double t){
    const double currentTime = t * settlingDuration;
    const double perceivedTime = pd;
    if (currentTime >= perceivedTime && !completed){
        completed = true;
        if (onComplete)
            onComplete(parent);
    }
    if (currentTime < perceivedTime && completed){
        completed = false;
    }
    return t == 0 || t == 1 ? t : solve(t * solverDuration);
}

EaseFn AnimSpring::getEaseFunction(){
    std::shared_ptr<AnimSpring> self = shared_from_this();
    return [self](double t){ return self->ease(t); };
}

double AnimSpring::solve(double time) const{
    double t = time;
    if (zeta < 1){
        // underdamped
        t = std::exp(-t * zeta * w0) * (1 * std::cos(wd * t) + b * std::sin(wd * t));
    } else if (zeta == 1){
        // critically damped
        t = (1 + b * t) * std::exp(-t * w0);
    } else {
        // overdamped, exponentials instead of cosh/sinh to avoid Infinity
        t = ((1 + b) * std::exp((-zeta * w0 + wd) * t) + (1 - b) * std::exp((-zeta * w0 - wd) * t)) / 2;
    }
    return 1 - t;
}

void AnimSpring::calculateSDFromBD(){
    // SwiftUI's perceived duration (WWDC 2023), equations from Kevin Grajeda's article
    const double pds = pd / AnimConst::K;
    m = 1;
    v = 0;
    s = std::pow((2 * PI) / pds, 2);
    if (bn >= 0){
        d = ((1 - bn) * 4 * PI) / pds;
    } else {
        d = (4 * PI) / (pds * (1 + bn));
    }
    s = AnimMath::round(AnimMath::clamp(s, AnimConst::minValue, maxSpringParamValue), 3);
    d = AnimMath::round(AnimMath::clamp(d, AnimConst::minValue, 300), 3);
}

void AnimSpring::calculateBDFromSD(){
    const double pds = (2 * PI) / std::sqrt(s);
    pd = pds * AnimConst::K;
    const double z = d / (2 * std::sqrt(s));
    if (z <= 1){
        bn = 1 - (d * pds) / (4 * PI);
    } else {
        bn = (4 * PI) / (d * pds) - 1;
    }
    bn = AnimMath::round(AnimMath::clamp(bn, -1, 1), 3);
    pd = AnimMath::round(AnimMath::clamp(pd, 10, maxSpringParamValue), 3);
}

void AnimSpring::compute(){
    w0 = AnimMath::clamp(std::sqrt(s / m), AnimConst::minValue, AnimConst::K);
    const double bouncedZeta = zeta = d / (2 * std::sqrt(s * m));
    if (bouncedZeta < 1){
        wd = w0 * std::sqrt(1 - bouncedZeta * bouncedZeta);
        b = (bouncedZeta * w0 + -v) / wd;
    } else if (bouncedZeta == 1){
        wd = 0;
        b = -v + w0;
    } else {
        wd = w0 * std::sqrt(bouncedZeta * bouncedZeta - 1);
        b = (bouncedZeta * w0 + -v) / wd;
    }

    double solverTime = 0;
    double restSteps = 0;
    double iterations = 0;
    while (restSteps <= maxRestSteps && iterations <= maxIterations){
        if (std::fabs(1 - solve(solverTime)) < restThreshold){
            restSteps++;
        } else {
            restSteps = 0;
        }
        solverDuration = solverTime;
        solverTime += timeStep;
        iterations++;
    }
    settlingDuration = AnimMath::round(solverDuration * AnimConst::K, 0);
}

void AnimSpring::setBounce(double bounce){
    bn = AnimMath::clamp(bounce, -1, 1);
    calculateSDFromBD();
    compute();
}

void AnimSpring::setDuration(double duration){
    pd = AnimMath::clamp(duration, 10, maxSpringParamValue);
    calculateSDFromBD();
    compute();
}

void AnimSpring::setStiffness(double stiffness){
    s = AnimMath::clamp(stiffness, AnimConst::minValue, maxSpringParamValue);
    calculateBDFromSD();
    compute();
}

void AnimSpring::setDamping(double damping){
    d = AnimMath::clamp(damping, AnimConst::minValue, maxSpringParamValue);
    calculateBDFromSD();
    compute();
}

void AnimSpring::setMass(double mass){
    m = AnimMath::clamp(mass, 1, maxSpringParamValue);
    compute();
}

void AnimSpring::setVelocity(double velocity){
    v = AnimMath::clamp(velocity, -maxSpringParamValue, maxSpringParamValue);
    compute();
}
