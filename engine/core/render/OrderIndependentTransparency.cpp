// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "OrderIndependentTransparency.h"

#include "render/SystemRender.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {
    // -1: the device's answer
    int supportOverride = -1;
}

bool OrderIndependentTransparency::isSupported(int samples){
    if (supportOverride >= 0)
        return supportOverride != 0;
    const RenderFeatures& features = SystemRender::getFeatures();
    const bool formats = features.renderRGBA16F && features.blendRGBA16F && features.renderR16F && features.blendR16F;
    if (samples > 1)
        return formats && features.msaaRGBA16F && features.msaaR16F;
    return formats;
}

void OrderIndependentTransparency::setSupportOverride(const bool* supported){
    supportOverride = supported ? (*supported ? 1 : 0) : -1;
}

float OrderIndependentTransparency::depthWeight(float viewDepth, float scale){
    const float z = std::abs(viewDepth) * scale;
    const float a = z / 5.0f;
    const float b = z / 200.0f;
    return std::clamp(10.0f / (1e-5f + a * a + b * b * b * b * b * b), 1e-2f, 3e3f);
}

const char* OrderIndependentTransparency::getModeName(TransparencyMode mode){
    return mode == TransparencyMode::ORDER_INDEPENDENT ? "order_independent" : "sorted";
}

bool OrderIndependentTransparency::modeFromName(const char* name, TransparencyMode& mode){
    if (!name)
        return false;
    if (std::strcmp(name, "sorted") == 0){
        mode = TransparencyMode::SORTED;
        return true;
    }
    if (std::strcmp(name, "order_independent") == 0){
        mode = TransparencyMode::ORDER_INDEPENDENT;
        return true;
    }
    return false;
}
