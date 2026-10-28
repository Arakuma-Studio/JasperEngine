// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The icons of the Scene view (lights, cameras, audio sources, reflection probes) are drawn on
// top of the scene, so a click tests them first, in screen space: an icon within its radius
// (10 px times the overlay scale) of the pointer is hit (docs/specs/S26-scene-view.md R4).

#include "Scene.h"
#include "math/Matrix4.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine::editor {

    struct IconHit {
        Entity entity = NULL_ENTITY;
        float screenDistance = 0.0f;   // pixels from the pointer
        float depth = 0.0f;            // clip w (perspective) or z: nearer first among ties
    };

    struct IconPickFilter {
        bool lights = true;            // the Gizmos menu hides light icons in 3D
        bool audio = true;             // and audio source icons
        uint32_t layerMask = 0xFFFFFFFFu;
        uint8_t excludeEditorFlags = 0;
        // the GameObjects of the view (not the editor's own objects)
        std::function<bool(Entity)> accept;
    };

    // The icon base radius in logical pixels (times the view's overlay scale)
    inline constexpr float ICON_PICK_RADIUS = 10.0f;

    // Icons of `scene` within `radius` engine pixels of (x, y) in a view of width x height
    // drawn with viewProjection, nearest the pointer first
    void pickIcons(Scene* scene, const Matrix4& viewProjection, float width, float height, float x, float y, float radius,
                   const IconPickFilter& filter, std::vector<IconHit>& out);

    // world point -> view pixels (y down); false behind the camera
    bool projectToView(const Matrix4& viewProjection, const Vector3& world, float width, float height, float& x, float& y, float* depth = nullptr);

}
