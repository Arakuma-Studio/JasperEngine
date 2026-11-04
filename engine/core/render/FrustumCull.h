// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FRUSTUM_CULL_H
#define FRUSTUM_CULL_H

#include "Export.h"
#include "math/Plane.h"

#include <cstdint>

namespace jasperengine{

    // The frustum of a pass for many box tests: a serial that changes only when the planes do (a
    // verdict made against a serial still holds while it is current), and the planes that lie along
    // one axis, which 2D and UI cameras have all six of, tested with that axis alone. Same result
    // as Plane::getSide(centre, halfSize) for every plane (RenderSystem::isInsideCamera).
    class JASPERENGINE_API FrustumCull{
    public:
        // the pass's planes and far clip (0: no far plane); the serial moves when either changed
        void set(const Plane planes[6], float farClip);
        // false when the box is wholly outside one plane
        bool inside(const float lo[3], const float hi[3]) const;

        uint32_t getSerial() const { return serial; }
        // every plane lies along one axis
        bool isAxisAligned() const { return axisAligned; }

    private:
        Plane planes[6];
        float farClip = 0.0f;
        uint32_t serial = 0;
        bool axisAligned = false;
        int8_t axis[6] = {-1, -1, -1, -1, -1, -1}; // the plane's only non-zero normal component, -1 none
    };

}

#endif // FRUSTUM_CULL_H
