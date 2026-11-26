// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "Ambient2D.h"

#include "Scene.h"

#include <algorithm>

namespace jasperengine::editor {

    float ambientLight2DBrightness(const Scene& scene) {
        const Vector3 color = scene.getAmbientLight2DColorLinear();
        return scene.getAmbientLight2DIntensity() * std::max({color.x, color.y, color.z});
    }

    bool isAmbientLight2DBright(const Scene& scene) {
        return ambientLight2DBrightness(scene) >= kBrightAmbient2D;
    }

}
