// Copyright 2023 Sony Interactive Entertainment.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Modified by Eduardo Doria and contributors: ported from Bend Studio's bend_sss_cpu.h
// (BuildDispatchList, the light coordinate part), in NDC for a fragment shader.

#include "ContactShadows.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

Vector4 ContactShadows::lightCoordinate(const Vector4& lightProjection){
    // Floating point division in the shader has a practical limit for precision when the light
    // is very far off screen, so the XY coordinate takes an adjusted w (Bend's limit at its
    // tested wave size of 64)
    float xyLightW = lightProjection.w;
    const float fpLimit = 0.000002f * 64.0f;
    if (xyLightW >= 0.0f && xyLightW < fpLimit) xyLightW = fpLimit;
    else if (xyLightW < 0.0f && xyLightW > -fpLimit) xyLightW = -fpLimit;

    Vector4 coordinate;
    coordinate.x = lightProjection.x / xyLightW;
    coordinate.y = lightProjection.y / xyLightW;
    // the depth buffer holds [-1, 1] z as [0, 1] (Bend's expanded z range)
    coordinate.z = (lightProjection.w == 0.0f) ? 0.0f : (lightProjection.z / lightProjection.w);
    coordinate.z = coordinate.z * 0.5f + 0.5f;
    coordinate.w = lightProjection.w > 0.0f ? 1.0f : -1.0f;
    return coordinate;
}

void ContactShadows::sampleGroups(int samples, int& hard, int& fade){
    // Bend's 60 samples take 4 hard and 8 fading; fewer samples keep the same shares, with at
    // least four between them for the four averaged groups
    samples = std::clamp(samples, MIN_SAMPLES, MAX_SAMPLES);
    hard = std::max(2, samples / 4);
    fade = std::max(2, samples / 4);
    if (samples - hard - fade < 4)
        fade = std::max(0, samples - hard - 4);
}

float ContactShadows::thicknessShare(const Matrix4& projection, float viewDepth, float thickness){
    // the packed depth of a view depth (z negative in front): 0.5 * ndc z + 0.5
    auto packed = [&projection](float z){
        const float clipZ = projection.get(2, 2) * z + projection.get(3, 2);
        const float clipW = projection.get(2, 3) * z + projection.get(3, 3);
        return clipW != 0.0f ? 0.5f * clipZ / clipW + 0.5f : 1.0f;
    };
    const float z = -std::abs(viewDepth);
    const float depth = packed(z);
    const float behind = packed(z - std::abs(thickness));
    const float remaining = std::max(1.0f - depth, 1e-7f);
    return std::clamp((behind - depth) / remaining, 1e-5f, 1.0f);
}
