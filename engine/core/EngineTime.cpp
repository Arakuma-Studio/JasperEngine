// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EngineTime.h"
#include "Engine.h"

using namespace jasperengine;

float Time::getDeltaTime(){
    // inside a Hit Stop Exempt GameObject's callbacks: the delta without the hit-stop (S48)
    return Engine::isInFixedStep() ? Engine::getUpdateTime() : Engine::getScriptDeltatime();
}

float Time::getUnscaledDeltaTime(){
    return Engine::isInFixedStep() ? getFixedUnscaledDeltaTime() : Engine::getUnscaledDeltatime();
}

float Time::getFixedDeltaTime(){
    return Engine::getUpdateTime();
}

void Time::setFixedDeltaTime(float seconds){
    Engine::setUpdateTime(seconds);
}

float Time::getFixedUnscaledDeltaTime(){
    const float scale = Engine::getTimeScale();
    return scale > 0.0f ? Engine::getUpdateTime() / scale : 0.0f;
}

double Time::getTime(){
    return Engine::isInFixedStep() ? Engine::getFixedTime() : Engine::getGameTime();
}

double Time::getFixedTime(){
    return Engine::getFixedTime();
}

double Time::getUnscaledTime(){
    return Engine::getUnscaledTime();
}

double Time::getRealtimeSinceStartup(){
    return Engine::getRealtimeSinceStartup();
}

double Time::getTimeSinceLevelLoad(){
    return Engine::getTimeSinceLevelLoad();
}

uint64_t Time::getFrameCount(){
    return Engine::getGameFrames();
}

float Time::getTimeScale(){
    return Engine::getTimeScale();
}

void Time::setTimeScale(float scale){
    Engine::setTimeScale(scale);
}

float Time::getMaximumDeltaTime(){
    return Engine::getMaxDeltatime();
}

void Time::setMaximumDeltaTime(float seconds){
    Engine::setMaxDeltatime(seconds);
}

double Time::getCaptureDeltaTime(){
    return Engine::getFixedDeltatime();
}

void Time::setCaptureDeltaTime(double seconds){
    Engine::setFixedDeltatime(seconds);
}

float Time::getSmoothDeltaTime(){
    return Engine::getSmoothDeltatime();
}

bool Time::isInFixedTimeStep(){
    return Engine::isInFixedStep();
}
