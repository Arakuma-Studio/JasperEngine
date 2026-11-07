// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "OrderIndependentTransparency.h"

#include "render/SystemRender.h"

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
