// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIClock.h"

#include "Log.h"

#include <cmath>

using namespace jasperengine;

namespace {

    double accumulatedTime = 0.0;
    std::optional<double> fixedTime;

}

double HtmlUIClock::now(){
    return fixedTime ? *fixedTime : accumulatedTime;
}

void HtmlUIClock::advance(double unscaledSeconds){
    if (fixedTime || !std::isfinite(unscaledSeconds) || unscaledSeconds <= 0.0)
        return;

    accumulatedTime += unscaledSeconds;
}

void HtmlUIClock::setFixedTime(std::optional<double> seconds){
    if (seconds && (!std::isfinite(*seconds) || *seconds < 0.0)){
        Log::warn("[HTML UI] HtmlUIClock::setFixedTime(%f) ignored: the time must be finite and not negative", *seconds);
        return;
    }

    // released: the running time goes on from where it was pinned
    if (fixedTime && !seconds){
        accumulatedTime = *fixedTime;
    }
    fixedTime = seconds;
}

std::optional<double> HtmlUIClock::getFixedTime(){
    return fixedTime;
}

void HtmlUIClock::reset(){
    accumulatedTime = 0.0;
    fixedTime.reset();
}
