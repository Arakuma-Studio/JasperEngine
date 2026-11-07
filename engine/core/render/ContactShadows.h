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
// Modified by Eduardo Doria and contributors: the light coordinate of Bend Studio's screen-space
// shadows (bend_sss_cpu.h, BuildDispatchList), for a fragment-shader port without wavefront
// dispatches (shaderlib/contact_shadows.frag).

#ifndef CONTACTSHADOWS_H
#define CONTACTSHADOWS_H

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

namespace jasperengine {

    // Contact shadows (docs/specs/S57-forge-lighting.md R3): a fullscreen pass after the depth
    // pre-pass marches from each pixel towards the light through the packed depth, as Bend
    // Studio's screen-space shadows do, and writes an R8 mask the opaque pass multiplies into
    // the light's shadow term (includes/lights.glsl, lightShadow).
    class JASPERENGINE_API ContactShadows {
    public:
        static constexpr int MIN_SAMPLES = 8;
        static constexpr int MAX_SAMPLES = 16;

        // The light in the pass's NDC (x, y, divided by w), its packed depth (0.5 * z / w + 0.5,
        // as includes/depth_util.glsl stores depth) and in w +1 when the light is in front of
        // the camera, -1 behind (Bend's LightCoordinate, in NDC instead of pixels: the shader maps
        // it to the depth texture's own orientation). lightProjection is the light through the
        // view-projection matrix, without the divide: (direction towards the light, 0) for a
        // directional light, (position, 1) for a spot. A light far off screen keeps w away from
        // zero by Bend's precision limit.
        static Vector4 lightCoordinate(const Vector4& lightProjection);

        // Bend's per-sample groups for a sample count: the first `hard` samples can shadow a
        // pixel alone, the last `fade` fade out, the ones between average in four groups
        static void sampleGroups(int samples, int& hard, int& fade);

    };

}

#endif // CONTACTSHADOWS_H
